/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "filebrowserentry.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cropguilistener.h"
#include "cursormanager.h"
#include "guiutils.h"
#include "inspector.h"
#include "previewloader.h"
#include "procparamchangers.h"
#include "rtsurface.h"
#include "threadutils.h"
#include "thumbbrowserbase.h"
#include "thumbnail.h"
#include "toolbar.h"

#include "rtengine/image8.h"
#include "rtengine/procparams.h"

#define CROPRESIZEBORDER 4

namespace
{

// Whether the cheap embedded camera preview should be replaced by a render
// through the processing parameters. Edited files always are; untouched RAWs
// are too when processed previews are on, so the browser shows the same
// picture the editor opens instead of the camera's own JPEG interpretation.
bool wantsProcessedPreview(const Thumbnail* thumbnail)
{
    const auto& options = App::get().options();

    if (!options.internalThumbIfUntouched || thumbnail->isPParamsValid()) {
        return true;
    }

    return options.processedRawPreviews && thumbnail->getType() == FT_Raw;
}

}

//extern Glib::Threads::Thread* mainThread;

std::shared_ptr<RTSurface> FileBrowserEntry::editedIcon(std::shared_ptr<RTSurface>(nullptr));
std::shared_ptr<RTSurface> FileBrowserEntry::recentlySavedIcon(std::shared_ptr<RTSurface>(nullptr));
std::shared_ptr<RTSurface> FileBrowserEntry::enqueuedIcon(std::shared_ptr<RTSurface>(nullptr));
std::shared_ptr<RTSurface> FileBrowserEntry::hdr(std::shared_ptr<RTSurface>(nullptr));
std::shared_ptr<RTSurface> FileBrowserEntry::ps(std::shared_ptr<RTSurface>(nullptr));

namespace
{

constexpr unsigned THUMBNAIL_MAX_RETRIES = 4;

struct QueuedImageUpdate {
    FileBrowserEntryIdleHelper* helper;
    rtengine::IImage8* img;
    hidpi::LogicalSize size;
    int deviceScale;
    double imageScale;
    rtengine::procparams::CropParams crop;
    std::uint64_t generation;
    // Rendered for a cell the user can currently see. Those keep flowing while
    // the foreground pause is held, so opening the editor doesn't leave the
    // filmstrip full of blank cells for the duration.
    bool visible;
};

struct ThumbnailRetry {
    FileBrowserEntryIdleHelper* helper;
    std::uint64_t generation;
};

std::mutex queuedImageUpdatesMutex;
using QueuedImageUpdateList = std::list<QueuedImageUpdate>;
QueuedImageUpdateList queuedImageUpdates;
std::unordered_map<FileBrowserEntryIdleHelper*, QueuedImageUpdateList::iterator> queuedImageUpdateIndex;
bool queuedImageUpdateIdlePending = false;
std::atomic<unsigned> queuedImageUpdatePauseDepth{0};

void discardQueuedImageUpdate(FileBrowserEntryIdleHelper* helper, rtengine::IImage8* img)
{
    delete img;

    if (helper->destroyed) {
        if (helper->pending == 1) {
            delete helper;
        } else {
            --helper->pending;
        }
    } else {
        --helper->pending;
        helper->fbentry->discardQueuedImageUpdate();
    }
}

void applyQueuedImageUpdate(QueuedImageUpdate& update)
{
    FileBrowserEntryIdleHelper* helper = update.helper;

    if (helper->destroyed) {
        if (helper->pending == 1) {
            delete helper;
        } else {
            --helper->pending;
        }

        delete update.img;
        return;
    }

    if (update.generation != helper->imageUpdateGeneration.load()) {
        discardQueuedImageUpdate(helper, update.img);
        return;
    }

    helper->fbentry->_updateImage(
        update.img,
        update.size,
        update.deviceScale,
        update.imageScale,
        update.crop);
    --helper->pending;
}

gboolean drainQueuedImageUpdates(gpointer)
{
    using clock = std::chrono::steady_clock;

    constexpr std::size_t MAX_UPDATES_PER_IDLE = 64;
    constexpr std::size_t PAUSED_MAX_UPDATES_PER_IDLE = 16;
    constexpr std::size_t MIN_UPDATES_PER_IDLE = 8;
    constexpr std::size_t IMAGE_UPDATE_DRAIN_CHUNK = 8;
    constexpr auto IMAGE_UPDATE_IDLE_BUDGET = std::chrono::milliseconds(8);

    const auto start = clock::now();
    std::size_t processed = 0;
    std::vector<QueuedImageUpdate> updates;
    updates.reserve(IMAGE_UPDATE_DRAIN_CHUNK);

    // While paused, deliver only results for cells that are on screen, and in
    // smaller slices so the foreground keeps the GUI thread.
    const bool visibleOnly =
        queuedImageUpdatePauseDepth.load(std::memory_order_acquire) > 0;
    const std::size_t maxUpdates =
        visibleOnly ? PAUSED_MAX_UPDATES_PER_IDLE : MAX_UPDATES_PER_IDLE;

    while (processed < maxUpdates) {
        if (processed >= MIN_UPDATES_PER_IDLE
            && clock::now() - start >= IMAGE_UPDATE_IDLE_BUDGET) {
            break;
        }

        updates.clear();
        bool pendingRemains = false;
        {
            std::lock_guard<std::mutex> lock(queuedImageUpdatesMutex);

            if (queuedImageUpdates.empty()) {
                queuedImageUpdateIdlePending = false;
                return FALSE;
            }

            const std::size_t count = std::min(
                IMAGE_UPDATE_DRAIN_CHUNK,
                std::min(queuedImageUpdates.size(), maxUpdates - processed)
            );

            for (auto it = queuedImageUpdates.begin();
                 it != queuedImageUpdates.end() && updates.size() < count;) {
                if (visibleOnly && !it->visible) {
                    pendingRemains = true;
                    ++it;
                    continue;
                }

                updates.push_back(std::move(*it));
                queuedImageUpdateIndex.erase(updates.back().helper);
                it = queuedImageUpdates.erase(it);
            }

            if (updates.empty()) {
                // Nothing deliverable right now; the resume path reschedules.
                queuedImageUpdateIdlePending = false;
                return FALSE;
            }

            pendingRemains = pendingRemains || !queuedImageUpdates.empty();
        }

        for (auto& update : updates) {
            applyQueuedImageUpdate(update);
            ++processed;
        }

        if (!pendingRemains) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(queuedImageUpdatesMutex);
    if (queuedImageUpdates.empty()) {
        queuedImageUpdateIdlePending = false;
        return FALSE;
    }

    return TRUE;
}

void queueImageUpdate(QueuedImageUpdate&& update)
{
    bool schedule = false;
    const bool wasVisible = update.visible;
    {
        std::lock_guard<std::mutex> lock(queuedImageUpdatesMutex);

        const auto existing = queuedImageUpdateIndex.find(update.helper);
        if (existing != queuedImageUpdateIndex.end()) {
            discardQueuedImageUpdate(existing->second->helper, existing->second->img);
            *existing->second = std::move(update);
        } else {
            queuedImageUpdates.push_back(std::move(update));
            auto inserted = queuedImageUpdates.end();
            --inserted;
            queuedImageUpdateIndex[inserted->helper] = inserted;
        }

        if (!queuedImageUpdateIdlePending) {
            // Visible results are delivered even while paused, so they must be
            // able to (re)arm the drain; anything else waits for resume.
            schedule = wasVisible
                || queuedImageUpdatePauseDepth.load(std::memory_order_acquire) == 0;
            queuedImageUpdateIdlePending = schedule;
        }
    }

    if (schedule) {
        gdk_threads_add_idle_full(G_PRIORITY_LOW, drainQueuedImageUpdates, nullptr, nullptr);
    }
}

gboolean thumbnailRetryTimeout(gpointer data)
{
    auto* retry = static_cast<ThumbnailRetry*>(data);
    FileBrowserEntryIdleHelper* helper = retry->helper;

    if (helper->destroyed) {
        if (helper->pending == 1) {
            delete helper;
        } else {
            --helper->pending;
        }
        return G_SOURCE_REMOVE;
    }

    helper->fbentry->runThumbnailRetry(retry->generation);
    --helper->pending;
    return G_SOURCE_REMOVE;
}

} // namespace

void FileBrowserEntry::pauseQueuedImageUpdates()
{
    queuedImageUpdatePauseDepth.fetch_add(1, std::memory_order_acq_rel);
}

void FileBrowserEntry::resumeQueuedImageUpdates()
{
    unsigned depth = queuedImageUpdatePauseDepth.load(std::memory_order_acquire);
    while (depth > 0) {
        if (queuedImageUpdatePauseDepth.compare_exchange_weak(
                depth,
                depth - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (depth > 1) {
                return;
            }
            break;
        }
    }

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(queuedImageUpdatesMutex);
        if (!queuedImageUpdates.empty() && !queuedImageUpdateIdlePending) {
            queuedImageUpdateIdlePending = true;
            schedule = true;
        }
    }

    if (schedule) {
        gdk_threads_add_idle_full(G_PRIORITY_LOW, drainQueuedImageUpdates, nullptr, nullptr);
    }
}

FileBrowserEntry::FileBrowserEntry (Thumbnail* thm, const Glib::ustring& fname)
    : ThumbBrowserEntryBase (fname, thm), wasInside(false), iatlistener(nullptr), press_x(0), press_y(0), action_x(0), action_y(0), rot_deg(0.0), landscape(true), deliveredPreviewAspectRatio_(0.0), cropParams(new rtengine::procparams::CropParams), cropgl(nullptr), suppressThumbnailRefresh(false), lazyThumbnailRequestPending(false), thumbnailPreviewUsable_(false), liveEditorPreview_(false), thumbnailRetryCount_(0), thumbnailRetryGeneration_(0), state(SNormal), crop_custom_ratio(0.f)
{
    browserFileNameUpper_ = Glib::path_get_basename(fname);
    std::transform(browserFileNameUpper_.begin(), browserFileNameUpper_.end(), browserFileNameUpper_.begin(), ::toupper);

    feih = new FileBrowserEntryIdleHelper;
    feih->fbentry = this;
    feih->destroyed = false;
    feih->pending = 0;
    feih->imageUpdateGeneration = 0;

    italicstyle = thumbnail->getType() != FT_Raw;

    scale = 1;

    thumbnail->addThumbnailListener (this);
}

FileBrowserEntry::~FileBrowserEntry ()
{
    thumbImageUpdater->removeJobs (this);

    // so jobs arriving now do nothing
    if (feih->pending) {
        feih->destroyed = true;
    } else {
        delete feih;
        feih = nullptr;
    }

    if (thumbnail) {
        thumbnail->removeThumbnailListener (this);
        thumbnail->decreaseRef ();
    }
}

void FileBrowserEntry::init ()
{
    editedIcon = std::shared_ptr<RTSurface>(new RTSurface("tick-small", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    recentlySavedIcon = std::shared_ptr<RTSurface>(new RTSurface("save-small", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    enqueuedIcon = std::shared_ptr<RTSurface>(new RTSurface("gears-small", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    hdr = std::shared_ptr<RTSurface>(new RTSurface("filetype-hdr", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    ps = std::shared_ptr<RTSurface>(new RTSurface("filetype-ps", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    FileThumbnailButtonSet::ensureIconsLoaded();
}

void FileBrowserEntry::refreshThumbnailImage(bool upgradeHint)
{

    if (!thumbnail) {
        return;
    }

    if (suppressThumbnailRefresh) {
        return;
    }

    liveEditorPreview_.store(false, std::memory_order_release);
    thumbImageUpdater->add (this, &updatepriority, upgradeHint, upgradeHint, this);
}

void FileBrowserEntry::refreshThumbnailImage ()
{
    refreshThumbnailImage(false);
}

void FileBrowserEntry::refreshQuickThumbnailImage ()
{

    if (!thumbnail) {
        return;
    }

    if (suppressThumbnailRefresh) {
        return;
    }

    const bool upgrade_to_processed = wantsProcessedPreview(thumbnail);
    const bool request_upgrade = upgrade_to_processed && thumbnail->isQuick();
    bool usablePreview = thumbnailPreviewUsable_.load(std::memory_order_acquire);
    if (!usablePreview) {
        usablePreview = hasUsableThumbnailPreview();
    }
    // Paint the cheap quick RAW thumbnail before queueing a processed upgrade.
    // The redraw after that first paint will ask for the upgrade again.
    const bool quickFirst = request_upgrade && !usablePreview;
    const bool queue_upgrade =
        request_upgrade
        && !quickFirst
        && !previewLoader->hasPendingWork();

    if (!queue_upgrade && usablePreview) {
        return;
    }

    thumbImageUpdater->add(this, &updatepriority, queue_upgrade, false, this);
}

void FileBrowserEntry::appendQuickThumbnailJob(std::vector<ThumbImageUpdater::Request>& requests, bool cachePixbuf)
{
    if (!thumbnail) {
        return;
    }

    if (suppressThumbnailRefresh) {
        return;
    }

    double cachedPixbufScale = 1.0;
    const bool needsCachedPixbuf = cachePixbuf && !thumbnail->tryGetCachedPixbuf(cachedPixbufScale);

    if (!needsCachedPixbuf
            && thumbnailRetryCount_.load(std::memory_order_acquire) > THUMBNAIL_MAX_RETRIES) {
        return;
    }

    if (!needsCachedPixbuf && lazyThumbnailRequestPending.load(std::memory_order_acquire)) {
        return;
    }

    const bool upgrade_to_processed = wantsProcessedPreview(thumbnail);
    const bool request_upgrade = upgrade_to_processed && thumbnail->isQuick();
    bool usablePreview = thumbnailPreviewUsable_.load(std::memory_order_acquire);
    if (!usablePreview) {
        usablePreview = hasUsableThumbnailPreview();
    }
    // Paint the cheap quick RAW thumbnail before queueing a processed upgrade.
    // The redraw after that first paint will ask for the upgrade again.
    const bool quickFirst = request_upgrade && !usablePreview;
    const bool queue_upgrade =
        request_upgrade
        && !quickFirst
        && !previewLoader->hasPendingWork();

    if (!needsCachedPixbuf && !queue_upgrade && usablePreview) {
        return;
    }

    if (lazyThumbnailRequestPending.exchange(true, std::memory_order_acq_rel) && !needsCachedPixbuf) {
        return;
    }

    requests.push_back({this, &updatepriority, queue_upgrade, false, cachePixbuf || shouldCacheRenderedThumbnailPixbuf(), this});
}

void FileBrowserEntry::retryThumbnailNow()
{
    if (!thumbnail || hasUsableThumbnailPreview()) {
        return;
    }

    thumbnailRetryCount_.store(0, std::memory_order_release);
    thumbnailRetryGeneration_.fetch_add(1, std::memory_order_acq_rel);
    lazyThumbnailRequestPending.store(true, std::memory_order_release);
    refreshQuickThumbnailImage();
}

void FileBrowserEntry::runThumbnailRetry(std::uint64_t generation)
{
    if (generation != thumbnailRetryGeneration_.load(std::memory_order_acquire)
            || hasUsableThumbnailPreview()) {
        return;
    }

    lazyThumbnailRequestPending.store(true, std::memory_order_release);
    refreshQuickThumbnailImage();

    if (parent) {
        parent->redrawNeeded(this);
    }
}

bool FileBrowserEntry::cacheCurrentPreviewForQuickOpen()
{
    if (!thumbnail) {
        return false;
    }

    double cachedScale = 1.0;
    bool cachedPixbufBusy = false;
    if (thumbnail->tryGetCachedPixbuf(cachedScale, &cachedPixbufBusy)) {
        return true;
    }
    if (cachedPixbufBusy) {
        return false;
    }

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    double previewScale = 1.0;
    {
        MyTryReaderLock lock(lockRW);
        if (!lock.owns_lock()) {
            return false;
        }

        if (preview.empty()
            || previewDataLayout.width <= 0
            || previewDataLayout.height <= 0
            || preview.size() != static_cast<size_t>(previewDataLayout.width * previewDataLayout.height * 3)) {
            return false;
        }

        pixbuf = Gdk::Pixbuf::create(
            Gdk::COLORSPACE_RGB,
            false,
            8,
            previewDataLayout.width,
            previewDataLayout.height);
        if (!pixbuf || !pixbuf->get_pixels()) {
            return false;
        }

        const int srcStride = previewDataLayout.width * 3;
        const int dstStride = pixbuf->get_rowstride();
        guint8* const dstPixels = pixbuf->get_pixels();
        for (int y = 0; y < previewDataLayout.height; ++y) {
            std::copy(
                preview.begin() + static_cast<size_t>(y * srcStride),
                preview.begin() + static_cast<size_t>((y + 1) * srcStride),
                dstPixels + y * dstStride);
        }
        previewScale = scale;
    }

    return thumbnail->trySetCachedPixbuf(pixbuf, previewScale, false);
}

hidpi::ScaledDeviceSize FileBrowserEntry::getLivePreviewDeviceSize () const
{
    return previewSize.scaleToDevice(pendingDeviceScale);
}

bool FileBrowserEntry::setLiveEditorPreview (
    const Glib::RefPtr<Gdk::Pixbuf>& pixbuf,
    double imageScale,
    const rtengine::procparams::CropParams& crop)
{
    if (!pixbuf || !feih || !thumbnail || imageScale <= 0.0) {
        return false;
    }

    const hidpi::LogicalSize logicalSize = previewSize;
    const int deviceScale = pendingDeviceScale;
    const hidpi::ScaledDeviceSize deviceSize = logicalSize.scaleToDevice(deviceScale);
    if (pixbuf->get_width() != deviceSize.width || pixbuf->get_height() != deviceSize.height) {
        return false;
    }

    auto* image = new rtengine::Image8(deviceSize.width, deviceSize.height);
    image->setSampleFormat(rtengine::IIOSF_UNSIGNED_CHAR);
    image->setSampleArrangement(rtengine::IIOSA_CHUNKY);
    const guint8* source = pixbuf->get_pixels();
    if (!source) {
        delete image;
        return false;
    }

    const int sourceStride = pixbuf->get_rowstride();
    for (int y = 0; y < deviceSize.height; ++y) {
        image->setScanline(y, source + y * sourceStride, 8, 3);
    }

    // Supersede queued thumbnail renders. Any worker result that arrives after
    // this point is discarded until a non-editor refresh explicitly wins.
    liveEditorPreview_.store(true, std::memory_order_release);
    {
        // The editor frame supersedes anything retained for other cell sizes.
        MYWRITERLOCK(l, lockRW);
        invalidatePreviewSlots();
    }
    ++feih->imageUpdateGeneration;
    thumbnail->trySetCachedPixbuf(pixbuf, 1.0 / imageScale, false);
    _updateImage(image, logicalSize, deviceScale, imageScale, crop, false);
    return true;
}

void FileBrowserEntry::invalidateTransientPreview(bool refresh)
{
    if (feih) {
        ++feih->imageUpdateGeneration;
    }

    {
        MYWRITERLOCK(l, lockRW);
        invalidatePreviewSlots();
    }

    if (refresh) {
        refreshThumbnailImage();
    }
}

bool FileBrowserEntry::hasUsableThumbnailPreview ()
{
    MYREADERLOCK(l, lockRW);

    bool usable = false;
    if (preview.empty() || activeDeviceScale != pendingDeviceScale) {
        thumbnailPreviewUsable_.store(false, std::memory_order_release);
        return false;
    }

    const hidpi::ScaledDeviceSize device = previewSize.scaleToDevice(activeDeviceScale);
    const std::size_t expectedSize = static_cast<std::size_t>(device.width) * device.height * 3;

    usable = previewDataLayout.width == device.width
            && previewDataLayout.height == device.height
            && preview.size() == expectedSize;
    thumbnailPreviewUsable_.store(usable, std::memory_order_release);

    return usable;
}

bool FileBrowserEntry::imageUpdateMatchesCurrentPreview (hidpi::LogicalSize size, int deviceScale)
{
    MYREADERLOCK(l, lockRW);
    return size == previewSize && deviceScale == pendingDeviceScale;
}

void FileBrowserEntry::calcThumbnailSize ()
{
    if (thumbnail) {
        int ow = previewSize.width, oh = previewSize.height;
        thumbnail->getThumbnailSize(previewSize.width, previewSize.height);
        if (deliveredPreviewAspectRatio_ > 0.0) {
            previewSize.width = std::max(
                1,
                static_cast<int>(std::lround(previewSize.height * deliveredPreviewAspectRatio_)));
        }

        hidpi::ScaledDeviceSize device = previewSize.scaleToDevice(activeDeviceScale);
        size_t expected_size = [&]() {
            int dataSize = device.width * device.height * 3;
            return static_cast<size_t>(dataSize);
        }();

        if (ow != previewSize.width || oh != previewSize.height || preview.size() != expected_size) {
            liveEditorPreview_.store(false, std::memory_order_release);
            thumbnailRetryCount_.store(0, std::memory_order_release);
            thumbnailRetryGeneration_.fetch_add(1, std::memory_order_acq_rel);
            lazyThumbnailRequestPending.store(false, std::memory_order_release);

            // A preview rendered earlier at exactly this size (typically the
            // other view's thumbnail height) can be put straight back on
            // screen — no worker job, no disk read, no pipeline run.
            if (restorePreviewForSize(previewSize, pendingDeviceScale)) {
                thumbnailPreviewUsable_.store(true, std::memory_order_release);
                return;
            }

            thumbnailPreviewUsable_.store(false, std::memory_order_release);

            // Nothing retained for the new size: keep the stale pixels so the
            // cell keeps showing the photo (updateBackBuffer scales them to
            // fit) instead of flashing an empty tile. The re-render is left to
            // the viewport scan in ThumbBrowserBase::Internal::on_draw, which
            // only queues work for entries the user can actually see.
            if (preview.empty() && !filtered) {
                refreshThumbnailImage();
            }
        }
    }
}

void FileBrowserEntry::savePreviewSlotExtras (PreviewSlot& slot) const
{
    slot.aspect = deliveredPreviewAspectRatio_;
    slot.imgScale = scale;
    slot.landscape = landscape;
}

void FileBrowserEntry::loadPreviewSlotExtras (const PreviewSlot& slot)
{
    deliveredPreviewAspectRatio_ = slot.aspect;
    scale = slot.imgScale;
    landscape = slot.landscape;
}

void FileBrowserEntry::onDeviceScaleChanged (int newDeviceScale)
{
    if (newDeviceScale != activeDeviceScale) {
        liveEditorPreview_.store(false, std::memory_order_release);
        thumbnailPreviewUsable_.store(false, std::memory_order_release);
        thumbnailRetryCount_.store(0, std::memory_order_release);
        thumbnailRetryGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }

    ThumbBrowserEntryBase::onDeviceScaleChanged(newDeviceScale);
}

std::size_t FileBrowserEntry::getImageAreaIconState ()
{
    if (!thumbnail) {
        return 0;
    }

    std::size_t state = 0;

    if (thumbnail->isHDR() && hdr) {
        state |= 1;
    }

    if (thumbnail->isPixelShift() && ps) {
        state |= 2;
    }

    // The export-queue badge is baked into the cached back buffer, so a
    // change in queue membership has to invalidate it like the others.
    if (thumbnail->isEnqueued()) {
        state |= 4;
    }

    return state;
}

bool FileBrowserEntry::imageAreaIconsChanged ()
{
    return getImageAreaIconState() != bbImageAreaIconState;
}

std::vector<std::shared_ptr<RTSurface>> FileBrowserEntry::getIconsOnImageArea ()
{
    // Icons (checkmarks etc.) removed from thumbnail overlay for cleaner look
    return {};
}

std::vector<std::shared_ptr<RTSurface>> FileBrowserEntry::getSpecificityIconsOnImageArea ()
{
    if (!thumbnail) {
        return {};
    }

    std::vector<std::shared_ptr<RTSurface>> ret;

    if (thumbnail->isHDR() && hdr) {
        ret.push_back (hdr);
    }

    if (thumbnail->isPixelShift() && ps) {
        ret.push_back (ps);
    }

    return ret;
}

void FileBrowserEntry::customBackBufferUpdate (Cairo::RefPtr<Cairo::Context> c)
{
    // somewhere in pipeline customBackBufferUpdate is called when scale == 1.0, which is nonsense for a thumb
    if (scale == 1.0 || !cropParams->enabled) return;

    bool inFilmstrip = parent && parent->getLocation() == ThumbBrowserBase::THLOC_EDITOR;

    auto drawScaled = [&](const rtengine::procparams::CropParams& crop) {
        double zoom = scale / activeDeviceScale;
        if (inFilmstrip) {
            // In filmstrip: hide non-crop areas with filmstrip background color
            c->save();
            c->set_line_width(0.0);
            c->rectangle(prevPos.x, prevPos.y, previewSize.width, previewSize.height);
            c->clip();

            double c1x = crop.x * zoom;
            double c1y = crop.y * zoom;
            double c2x = (crop.x + crop.w) * zoom;
            double c2y = (crop.y + crop.h) * zoom;

            Gdk::RGBA bg = parent->getNormalBgColor();
            c->set_source_rgb(bg.get_red(), bg.get_green(), bg.get_blue());
            c->rectangle(prevPos.x, prevPos.y, previewSize.width, round(c1y) + 0.5);
            c->rectangle(prevPos.x, prevPos.y + round(c2y) + 0.5, previewSize.width, previewSize.height - round(c2y));
            c->rectangle(prevPos.x, prevPos.y + round(c1y) + 0.5, round(c1x) + 0.5, round(c2y - c1y + 1) + 0.5);
            c->rectangle(prevPos.x + round(c2x) + 0.5, prevPos.y + round(c1y) + 0.5, previewSize.width - round(c2x), round(c2y - c1y + 1) + 0.5);
            c->fill();
            c->restore();
        } else {
            drawCrop(c, prevPos.x, prevPos.y, previewSize.width, previewSize.height,
                     previewSize.width, previewSize.height,
                     0, 0, zoom, crop, true, false);
        }
    };

    if (state == SCropSelecting || state == SResizeH1 || state == SResizeH2 || state == SResizeW1 || state == SResizeW2 || state == SResizeTL || state == SResizeTR || state == SResizeBL || state == SResizeBR || state == SCropMove) {
        drawScaled(*cropParams);
    } else {
        rtengine::procparams::CropParams cparams = thumbnail->getProcParams().crop;
        switch (App::get().options().cropGuides) {
        case Options::CROP_GUIDE_NONE:
            cparams.guide = rtengine::procparams::CropParams::Guide::NONE;
            break;
        case Options::CROP_GUIDE_FRAME:
            cparams.guide = rtengine::procparams::CropParams::Guide::FRAME;
            break;
        default:
            break;
        }

        if (cparams.enabled && !thumbnail->isQuick()) { // Quick thumb have arbitrary sizes, so don't apply the crop
            drawScaled(cparams);
        }
    }
}

void FileBrowserEntry::getIconSize (int& w, int& h) const
{

    w = editedIcon->getWidth ();
    h = editedIcon->getHeight ();
}

FileThumbnailButtonSet* FileBrowserEntry::getThumbButtonSet ()
{

    return (static_cast<FileThumbnailButtonSet*>(buttonSet));
}

FileThumbnailButtonSet* FileBrowserEntry::ensureThumbButtonSet (LWButtonListener* listener)
{
    auto* thumbButtonSet = getThumbButtonSet();

    if (!thumbButtonSet) {
        thumbButtonSet = new FileThumbnailButtonSet(this);
        addButtonSet(thumbButtonSet);
    }

    thumbButtonSet->setButtonListener(listener);
    return thumbButtonSet;
}

void FileBrowserEntry::resizeWithoutThumbnailJob (int h)
{
    suppressThumbnailRefresh = true;
    struct SuppressRefreshGuard {
        FileBrowserEntry* entry;
        ~SuppressRefreshGuard()
        {
            entry->suppressThumbnailRefresh = false;
        }
    } guard{this};

    resize(h);
}

void FileBrowserEntry::procParamsChanged (Thumbnail* thm, int whoChangedIt, bool upgradeHint)
{
    if (whoChangedIt == EDITOR && liveEditorPreview_.load(std::memory_order_acquire)) {
        if (parent) {
            parent->redrawNeeded(this);
        }
        return;
    }

    liveEditorPreview_.store(false, std::memory_order_release);

    // The rendered pixels no longer match the processing parameters, so
    // previews retained for the other view's size are stale too.
    {
        MYWRITERLOCK(l, lockRW);
        invalidatePreviewSlots();
    }

    if ( thumbnail->isQuick() ) {
        refreshQuickThumbnailImage ();
    } else {
        refreshThumbnailImage(upgradeHint);
    }
}

void FileBrowserEntry::updateImage(const ThumbImageUpdateListener::ImageUpdate& update)
{
    if (!feih) {
        lazyThumbnailRequestPending.store(false, std::memory_order_release);
        delete update.img;
        return;
    }

    if (liveEditorPreview_.load(std::memory_order_acquire)) {
        lazyThumbnailRequestPending.store(false, std::memory_order_release);
        delete update.img;
        return;
    }

    if (!imageUpdateMatchesCurrentPreview(update.size, update.device_scale)) {
        thumbnailPreviewUsable_.store(false, std::memory_order_release);
        lazyThumbnailRequestPending.store(false, std::memory_order_release);
        delete update.img;
        return;
    }

    redrawRequests++;
    feih->pending++;
    const std::uint64_t generation = ++feih->imageUpdateGeneration;

    queueImageUpdate({
        feih,
        update.img,
        update.size,
        update.device_scale,
        update.scale,
        update.crop,
        generation,
        updatepriority
    });
}

void FileBrowserEntry::updateImageFailed(hidpi::LogicalSize size, int deviceScale, bool upgrade)
{
    lazyThumbnailRequestPending.store(false, std::memory_order_release);

    if (!feih || liveEditorPreview_.load(std::memory_order_acquire)
            || !imageUpdateMatchesCurrentPreview(size, deviceScale)) {
        return;
    }

    // An upgrade can become unnecessary while queued. Keep an existing quick
    // preview instead of retrying processed work the user cannot distinguish.
    if (upgrade && hasUsableThumbnailPreview()) {
        thumbnailRetryCount_.store(0, std::memory_order_release);
        return;
    }

    constexpr unsigned RETRY_DELAYS_MS[THUMBNAIL_MAX_RETRIES] = {50, 150, 400, 1000};
    const unsigned attempt = thumbnailRetryCount_.fetch_add(1, std::memory_order_acq_rel);
    if (attempt >= THUMBNAIL_MAX_RETRIES) {
        return;
    }

    ++feih->pending;
    auto* retry = new ThumbnailRetry{
        feih,
        thumbnailRetryGeneration_.load(std::memory_order_acquire)
    };
    g_timeout_add_full(
        G_PRIORITY_LOW,
        RETRY_DELAYS_MS[attempt],
        thumbnailRetryTimeout,
        retry,
        [](gpointer data) {
            delete static_cast<ThumbnailRetry*>(data);
        });
}

void FileBrowserEntry::discardQueuedImageUpdate()
{
    --redrawRequests;
}

void FileBrowserEntry::_updateImage(
    rtengine::IImage8* img,
    hidpi::LogicalSize size,
    int deviceScale,
    double imageScale,
    const rtengine::procparams::CropParams& crop,
    bool queuedRequest)
{
    MYWRITERLOCK(l, lockRW);

    if (queuedRequest) {
        redrawRequests--;
    }
    if (!(size == previewSize) || deviceScale != pendingDeviceScale) {
        thumbnailPreviewUsable_.store(false, std::memory_order_release);
        lazyThumbnailRequestPending.store(false, std::memory_order_release);
        delete img;
        return;
    }

    int imw = img->getWidth();
    int imh = img->getHeight();

    bool newLandscape = imw > imh;
    bool geometryChanged = false;

    const hidpi::LogicalSize deliveredSize(
        std::max(1, (imw + deviceScale - 1) / deviceScale),
        std::max(1, (imh + deviceScale - 1) / deviceScale));

    // Cached metadata can report the embedded preview's orientation instead
    // of the rendered pixels. Keep browser and filmstrip cells tied to the
    // aspect ratio users actually see.
    if (std::abs(deliveredSize.height - previewSize.height) <= 1) {
        deliveredPreviewAspectRatio_ = static_cast<double>(imw) / imh;
        if (deliveredSize.width != previewSize.width) {
            previewSize.width = deliveredSize.width;
            width = previewSize.width + 2 * sideMargin + 2 * borderWidth;
            geometryChanged = true;
        }
    }

    scale = imageScale;
    *this->cropParams = crop;
    activeDeviceScale = pendingDeviceScale;

    // Check if image has been rotated since last time
    const bool rotated = geometryChanged || (!preview.empty() && newLandscape != landscape);

    previewDataLayout.width = imw;
    previewDataLayout.height = imh;
    int dataSize = imw * imh * 3;
    preview.resize(dataSize);
    if (img->getData()) {
        std::copy(img->getData(), img->getData() + preview.size(), preview.begin());
    } else {
        std::fill(preview.begin(), preview.end(), 0);
    }

    const bool updateBackBufferNow =
        parent
        && drawable
        && insideWindow(0, 0, parent->getDrawingArea()->get_width(), parent->getDrawingArea()->get_height());

    if (updateBackBufferNow) {
        GThreadLock lock;
        updateBackBuffer();
    } else if (backBuffer) {
        backBuffer->setDirty(true);
    }

    landscape = newLandscape;

    delete img;
    thumbnailPreviewUsable_.store(true, std::memory_order_release);
    lazyThumbnailRequestPending.store(false, std::memory_order_release);
    thumbnailRetryCount_.store(0, std::memory_order_release);
    thumbnailRetryGeneration_.fetch_add(1, std::memory_order_acq_rel);

    if (parent) {
        if (rotated) {
            parent->thumbRearrangementNeeded();
        } else if (updateBackBufferNow && redrawRequests == 0) {
            parent->redrawNeeded(this);
        }
    }
}

bool FileBrowserEntry::motionNotify (int x, int y)
{

    const bool b = ThumbBrowserEntryBase::motionNotify(x, y);

    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    const int ix = x - startx - offsetX;
    const int iy = y - starty - offsetY;

    Inspector* inspector = parent->getInspector();

    if (inspector && inspector->isActive() && (!parent->isInTabMode() || App::get().options().inspectorWindow)) {
        const rtengine::Coord2D coord(getPosInImgSpace(x, y));

        if (coord.x != -1.) {
            if (!wasInside) {
                inspector->switchImage(filename);
                wasInside = true;
            }
            inspector->mouseMove(coord, 0);
        } else {
            wasInside = false;
        }
    }

    if (inside(x, y)) {
        updateCursor(ix, iy);
    }

    if (state == SRotateSelecting) {
        action_x = x;
        action_y = y;
        parent->redrawNeeded (this);
    } else if (state == SResizeH1 && cropgl) {
        int oy = cropParams->y;
        cropParams->y = action_y + (y - press_y) / scale * activeDeviceScale;
        cropParams->h += oy - cropParams->y;
        cropgl->cropHeight1Resized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeH2 && cropgl) {
        cropParams->h = action_y + (y - press_y) / scale * activeDeviceScale;
        cropgl->cropHeight2Resized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeW1 && cropgl) {
        int ox = cropParams->x;
        cropParams->x = action_x + (x - press_x) / scale * activeDeviceScale;
        cropParams->w += ox - cropParams->x;
        cropgl->cropWidth1Resized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeW2 && cropgl) {
        cropParams->w = action_x + (x - press_x) / scale * activeDeviceScale;
        cropgl->cropWidth2Resized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeTL && cropgl) {
        int ox = cropParams->x;
        cropParams->x = action_x + (x - press_x) / scale * activeDeviceScale;
        cropParams->w += ox - cropParams->x;
        int oy = cropParams->y;
        cropParams->y = action_y + (y - press_y) / scale * activeDeviceScale;
        cropParams->h += oy - cropParams->y;
        cropgl->cropTopLeftResized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeTR && cropgl) {
        cropParams->w = action_x + (x - press_x) / scale * activeDeviceScale;
        int oy = cropParams->y;
        cropParams->y = action_y + (y - press_y) / scale * activeDeviceScale;
        cropParams->h += oy - cropParams->y;
        cropgl->cropTopRightResized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeBL && cropgl) {
        int ox = cropParams->x;
        cropParams->x = action_x + (x - press_x) / scale * activeDeviceScale;
        cropParams->w += ox - cropParams->x;
        cropParams->h = action_y + (y - press_y) / scale * activeDeviceScale;
        cropgl->cropBottomLeftResized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SResizeBR && cropgl) {
        cropParams->w = action_x + (x - press_x) / scale * activeDeviceScale;
        cropParams->h = action_y + (y - press_y) / scale * activeDeviceScale;
        cropgl->cropBottomRightResized (cropParams->x, cropParams->y, cropParams->w, cropParams->h, crop_custom_ratio);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SCropMove && cropgl) {
        cropParams->x = action_x + (x - press_x) / scale * activeDeviceScale;
        cropParams->y = action_y + (y - press_y) / scale * activeDeviceScale;
        cropgl->cropMoved (cropParams->x, cropParams->y, cropParams->w, cropParams->h);
        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    } else if (state == SCropSelecting && cropgl) {
        int cx1 = press_x;
        int cy1 = press_y;
        int cx2 = (ix - prevPos.x) / scale * activeDeviceScale;
        int cy2 = (iy - prevPos.y) / scale * activeDeviceScale;
        cropgl->cropResized (cx1, cy1, cx2, cy2);

        if (cx2 > cx1) {
            cropParams->x = cx1;
            cropParams->w = cx2 - cx1 + 1;
        } else {
            cropParams->x = cx2;
            cropParams->w = cx1 - cx2 + 1;
        }

        if (cy2 > cy1) {
            cropParams->y = cy1;
            cropParams->h = cy2 - cy1 + 1;
        } else {
            cropParams->y = cy2;
            cropParams->h = cy1 - cy2 + 1;
        }

        {
        MYREADERLOCK(l, lockRW);
        updateBackBuffer ();
        }
        parent->redrawNeeded (this);
    }

    return b;
}

bool FileBrowserEntry::pressNotify   (int button, int type, int bstate, int x, int y)
{

    bool b = ThumbBrowserEntryBase::pressNotify (button, type, bstate, x, y);

    if (!iatlistener || !iatlistener->getToolBar()) {
        return true;
    }

    ToolMode tm = iatlistener->getToolBar()->getTool ();
    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    int ix = x - startx - offsetX;
    int iy = y - starty - offsetY;

    if (tm == TMNone) {
        return b;
    }

    crop_custom_ratio = 0.f;

    if (!b && selected && inside (x, y)) {
        if (button == 1 && type == GDK_BUTTON_PRESS && state == SNormal) {
            if ((bstate & GDK_SHIFT_MASK) && cropParams->w > 0 && cropParams->h > 0) {
                crop_custom_ratio = float(cropParams->w) / float(cropParams->h);
            }
            if (onArea (CropTopLeft, ix, iy)) {
                state = SResizeTL;
                press_x = x;
                action_x = cropParams->x;
                press_y = y;
                action_y = cropParams->y;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropTopRight, ix, iy)) {
                state = SResizeTR;
                press_x = x;
                action_x = cropParams->w;
                press_y = y;
                action_y = cropParams->y;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropBottomLeft, ix, iy)) {
                state = SResizeBL;
                press_x = x;
                action_x = cropParams->x;
                press_y = y;
                action_y = cropParams->h;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropBottomRight, ix, iy)) {
                state = SResizeBR;
                press_x = x;
                action_x = cropParams->w;
                press_y = y;
                action_y = cropParams->h;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropTop, ix, iy)) {
                state = SResizeH1;
                press_y = y;
                action_y = cropParams->y;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropBottom, ix, iy)) {
                state = SResizeH2;
                press_y = y;
                action_y = cropParams->h;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropLeft, ix, iy)) {
                state = SResizeW1;
                press_x = x;
                action_x = cropParams->x;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropRight, ix, iy)) {
                state = SResizeW2;
                press_x = x;
                action_x = cropParams->w;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if ((bstate & GDK_SHIFT_MASK) && onArea (CropInside, ix, iy)) {
                state = SCropMove;
                press_x = x;
                press_y = y;
                action_x = cropParams->x;
                action_y = cropParams->y;
                cropgl = iatlistener->startCropEditing (thumbnail);
                b = true;
            } else if (onArea (CropImage, ix, iy)) {
                if (tm == TMStraighten) {
                    state = SRotateSelecting;
                    press_x = x;
                    press_y = y;
                    action_x = x;
                    action_y = y;
                    rot_deg = 0;
                    b = true;
                } else if (tm == TMSpotWB) {
                    iatlistener->spotWBselected (
                        (ix - prevPos.x) / scale * activeDeviceScale,
                        (iy - prevPos.y) / scale * activeDeviceScale,
                        thumbnail);
                    b = true;
                } else if (tm == TMCropSelect) {
                    cropgl = iatlistener->startCropEditing (thumbnail);

                    if (cropgl) {
                        state = SCropSelecting;
                        press_x = cropParams->x = (ix - prevPos.x) / scale * activeDeviceScale;
                        press_y = cropParams->y = (iy - prevPos.y) / scale * activeDeviceScale;
                        cropParams->w = cropParams->h = 1;
                        cropgl->cropInit (cropParams->x, cropParams->y, cropParams->w, cropParams->h);
                        b = true;
                    }
                }
            }
        }

        updateCursor (ix, iy);
    }

    return b;
}

bool FileBrowserEntry::releaseNotify (int button, int type, int bstate, int x, int y)
{

    bool b = ThumbBrowserEntryBase::releaseNotify (button, type, bstate, x, y);

    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    int ix = x - startx - offsetX;
    int iy = y - starty - offsetY;

    if (!b) {
        if (state == SRotateSelecting) {
            iatlistener->rotateSelectionReady (rot_deg, thumbnail);

            if (iatlistener->getToolBar()) {
                iatlistener->getToolBar()->setTool (TMHand);
            }
        } else if (cropgl && (state == SCropSelecting || state == SResizeH1 || state == SResizeH2 || state == SResizeW1 || state == SResizeW2 || state == SResizeTL || state == SResizeTR || state == SResizeBL || state == SResizeBR || state == SCropMove)) {
            cropgl->cropManipReady ();
            cropgl = nullptr;
            iatlistener->cropSelectionReady ();

            if (iatlistener->getToolBar()) {
                iatlistener->getToolBar()->setTool (TMHand);
            }
        }

        state = SNormal;

        if (parent) {
            parent->redrawNeeded (this);
        }

        updateCursor (ix, iy);
    }

    return b;
}

bool FileBrowserEntry::onArea (CursorArea a, int x, int y)
{

    MYREADERLOCK(l, lockRW);
    if (!drawable || preview.empty()) {
        return false;
    }

    double adjustedScale = scale / activeDeviceScale;

    int x1 = (x - prevPos.x) / adjustedScale;
    int y1 = (y - prevPos.y) / adjustedScale;
    int cropResizeBorder = CROPRESIZEBORDER / adjustedScale;

    switch (a) {
    case CropImage:
    {
        hidpi::LogicalCoord pos(x, y);
        return pos.x >= prevPos.x && pos.x < prevPos.x + previewSize.width
                && pos.y >= prevPos.y && pos.y < prevPos.y + previewSize.height;
    }
    case CropTopLeft:
        return cropParams->enabled &&
               y1 >= cropParams->y - cropResizeBorder &&
               y1 <= cropParams->y + cropResizeBorder &&
               x1 >= cropParams->x - cropResizeBorder &&
               x1 <= cropParams->x + cropResizeBorder;

    case CropTopRight:
        return cropParams->enabled &&
               y1 >= cropParams->y - cropResizeBorder &&
               y1 <= cropParams->y + cropResizeBorder &&
               x1 >= cropParams->x + cropParams->w - 1 - cropResizeBorder &&
               x1 <= cropParams->x + cropParams->w - 1 + cropResizeBorder;

    case CropBottomLeft:
        return cropParams->enabled &&
               y1 >= cropParams->y + cropParams->h - 1 - cropResizeBorder &&
               y1 <= cropParams->y + cropParams->h - 1 + cropResizeBorder &&
               x1 >= cropParams->x - cropResizeBorder &&
               x1 <= cropParams->x + cropResizeBorder;

    case CropBottomRight:
        return cropParams->enabled &&
               y1 >= cropParams->y + cropParams->h - 1 - cropResizeBorder &&
               y1 <= cropParams->y + cropParams->h - 1 + cropResizeBorder &&
               x1 >= cropParams->x + cropParams->w - 1 - cropResizeBorder &&
               x1 <= cropParams->x + cropParams->w - 1 + cropResizeBorder;

    case CropTop:
        return cropParams->enabled &&
               x1 > cropParams->x + cropResizeBorder &&
               x1 < cropParams->x + cropParams->w - 1 - cropResizeBorder &&
               y1 > cropParams->y - cropResizeBorder &&
               y1 < cropParams->y + cropResizeBorder;

    case CropBottom:
        return cropParams->enabled &&
               x1 > cropParams->x + cropResizeBorder &&
               x1 < cropParams->x + cropParams->w - 1 - cropResizeBorder &&
               y1 > cropParams->y + cropParams->h - 1 - cropResizeBorder &&
               y1 < cropParams->y + cropParams->h - 1 + cropResizeBorder;

    case CropLeft:
        return cropParams->enabled &&
               y1 > cropParams->y + cropResizeBorder &&
               y1 < cropParams->y + cropParams->h - 1 - cropResizeBorder &&
               x1 > cropParams->x - cropResizeBorder &&
               x1 < cropParams->x + cropResizeBorder;

    case CropRight:
        return cropParams->enabled &&
               y1 > cropParams->y + cropResizeBorder &&
               y1 < cropParams->y + cropParams->h - 1 - cropResizeBorder &&
               x1 > cropParams->x + cropParams->w - 1 - cropResizeBorder &&
               x1 < cropParams->x + cropParams->w - 1 + cropResizeBorder;

    case CropInside:
        return cropParams->enabled &&
               y1 > cropParams->y &&
               y1 < cropParams->y + cropParams->h - 1 &&
               x1 > cropParams->x &&
               x1 < cropParams->x + cropParams->w - 1;
    default: /* do nothing */ ;
    }

    return false;
}


void FileBrowserEntry::updateCursor (int x, int y)
{

    if (!iatlistener || !iatlistener->getToolBar()) {
        return;
    }

    CursorShape newCursor = CSUndefined;

    ToolMode tm = iatlistener->getToolBar()->getTool ();
    Glib::RefPtr<Gdk::Window> w = parent->getDrawingArea ()->get_window();

    if (!selected) {
        newCursor = CSArrow;
    } else if (state == SNormal) {
        if (tm == TMHand && (onArea (CropTop, x, y) || onArea (CropBottom, x, y))) {
            newCursor = CSResizeHeight;
        } else if (tm == TMHand && (onArea (CropLeft, x, y) || onArea (CropRight, x, y))) {
            newCursor = CSResizeWidth;
        } else if (tm == TMHand && (onArea (CropTopLeft, x, y))) {
            newCursor = CSResizeTopLeft;
        } else if (tm == TMHand && (onArea (CropTopRight, x, y))) {
            newCursor = CSResizeTopRight;
        } else if (tm == TMHand && (onArea (CropBottomLeft, x, y))) {
            newCursor = CSResizeBottomLeft;
        } else if (tm == TMHand && (onArea (CropBottomRight, x, y))) {
            newCursor = CSResizeBottomRight;
        } else if (onArea (CropImage, x, y)) {
            if (tm == TMHand) {
                newCursor = CSArrow;
            } else if (tm == TMSpotWB) {
                newCursor = CSSpotWB;
            } else if (tm == TMCropSelect) {
                newCursor = CSCropSelect;
            } else if (tm == TMStraighten) {
                newCursor = CSStraighten;
            }
        } else {
            newCursor = CSArrow;
        }
    } else if (state == SCropSelecting) {
        newCursor = CSCropSelect;
    } else if (state == SRotateSelecting) {
        newCursor = CSStraighten;
    } else if (state == SCropMove) {
        newCursor = CSMove;
    } else if (state == SResizeW1 || state == SResizeW2) {
        newCursor = CSResizeWidth;
    } else if (state == SResizeH1 || state == SResizeH2) {
        newCursor = CSResizeHeight;
    } else if (state == SResizeTL) {
        newCursor = CSResizeTopLeft;
    } else if (state == SResizeTR) {
        newCursor = CSResizeTopRight;
    } else if (state == SResizeBL) {
        newCursor = CSResizeBottomLeft;
    } else if (state == SResizeBR) {
        newCursor = CSResizeBottomRight;
    }

    if (newCursor != cursor_type) {
        cursor_type = newCursor;
        CursorManager::setCursorOfMainWindow (w, cursor_type);
    }
}

void FileBrowserEntry::draw (Cairo::RefPtr<Cairo::Context> cc)
{

    ThumbBrowserEntryBase::draw (cc);

    if (state == SRotateSelecting) {
        drawStraightenGuide (cc);
    }
}

void FileBrowserEntry::drawStraightenGuide (Cairo::RefPtr<Cairo::Context> cr)
{

    if (action_x != press_x || action_y != press_y) {
        double arg = (press_x - action_x) / sqrt(double((press_x - action_x) * (press_x - action_x) + (press_y - action_y) * (press_y - action_y)));
        double sol1, sol2;
        double pi = rtengine::RT_PI;

        if (press_y > action_y) {
            sol1 = acos(arg) * 180 / pi;
            sol2 = -acos(-arg) * 180 / pi;
        } else {
            sol1 = acos(-arg) * 180 / pi;
            sol2 = -acos(arg) * 180 / pi;
        }

        if (fabs(sol1) < fabs(sol2)) {
            rot_deg = sol1;
        } else {
            rot_deg = sol2;
        }

        if (rot_deg < -45) {
            rot_deg = 90.0 + rot_deg;
        } else if (rot_deg > 45) {
            rot_deg = - 90.0 + rot_deg;
        }
    } else {
        rot_deg = 0;
    }

    Glib::RefPtr<Pango::Context> context = parent->getDrawingArea()->get_pango_context () ;
    Pango::FontDescription fontd = parent->getDrawingArea()->get_style_context()->get_font();
    fontd.set_weight (Pango::WEIGHT_BOLD);
    const int fontSize = 8; // pt
    // Non-absolute size is defined in "Pango units" and shall be multiplied by
    // Pango::SCALE from "pt":
    fontd.set_size (fontSize * Pango::SCALE);
    context->set_font_description (fontd);
    Glib::RefPtr<Pango::Layout> deglayout = parent->getDrawingArea()->create_pango_layout(Glib::ustring::compose ("%1 deg", Glib::ustring::format(std::setprecision(2), rot_deg)));

    int x1 = press_x;
    int y1 = press_y;
    int y2 = action_y;
    int x2 = action_x;

    {
    MYREADERLOCK(l, lockRW);
    const int offsetX = getOffsetX();
    const int offsetY = getOffsetY();
    if (x2 < prevPos.x + offsetX + startx) {
        y2 = y1 - (double)(y1 - y2) * (x1 - (prevPos.x + offsetX + startx)) / (x1 - x2);
        x2 = prevPos.x + offsetX + startx;
    } else if (x2 >= previewSize.width + prevPos.x + offsetX + startx) {
        y2 = y1 - (double)(y1 - y2) * (x1 - (previewSize.width + prevPos.x + offsetX + startx - 1)) / (x1 - x2);
        x2 = previewSize.width + prevPos.x + offsetX + startx - 1;
    }

    if (y2 < prevPos.y + offsetY + starty) {
        x2 = x1 - (double)(x1 - x2) * (y1 - (prevPos.y + offsetY + starty)) / (y1 - y2);
        y2 = prevPos.y + offsetY + starty;
    } else if (y2 >= previewSize.height + prevPos.y + offsetY + starty) {
        x2 = x1 - (double)(x1 - x2) * (y1 - (previewSize.height + prevPos.y + offsetY + starty - 1)) / (y1 - y2);
        y2 = previewSize.height + prevPos.y + offsetY + starty - 1;
    }
    }

    cr->set_line_width (1.5);
    cr->set_source_rgb (1.0, 1.0, 1.0);
    cr->move_to (x1, y1);
    cr->line_to (x2, y2);
    cr->stroke ();
    cr->set_source_rgb (0.0, 0.0, 0.0);
    std::valarray<double> ds (1);
    ds[0] = 4;
    cr->set_dash (ds, 0);
    cr->move_to (x1, y1);
    cr->line_to (x2, y2);
    cr->stroke ();

    if (press_x != action_x && press_y != action_y) {
        cr->set_source_rgb (0.0, 0.0, 0.0);
        cr->move_to ((x1 + x2) / 2 + 1, (y1 + y2) / 2 + 1);
        deglayout->add_to_cairo_context (cr);
        cr->move_to ((x1 + x2) / 2 + 1, (y1 + y2) / 2 - 1);
        deglayout->add_to_cairo_context (cr);
        cr->move_to ((x1 + x2) / 2 - 1, (y1 + y2) / 2 + 1);
        deglayout->add_to_cairo_context (cr);
        cr->move_to ((x1 + x2) / 2 + 1, (y1 + y2) / 2 + 1);
        deglayout->add_to_cairo_context (cr);
        cr->fill ();
        cr->set_source_rgb (1.0, 1.0, 1.0);
        cr->move_to ((x1 + x2) / 2, (y1 + y2) / 2);
        deglayout->add_to_cairo_context (cr);
        cr->fill ();
    }
}
