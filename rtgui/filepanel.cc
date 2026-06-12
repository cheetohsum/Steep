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
#include "filepanel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "albumbrowser.h"
#include "guiutils.h"
#include "options.h"
#include "rtimage.h"
#include "filebrowser.h"
#include "filecatalog.h"
#include "batchtoolpanelcoord.h"
#include "dirbrowser.h"
#include "editorpanel.h"
#include "inspector.h"
#include "placesbrowser.h"
#include "previewloader.h"
#include "rawloadactivity.h"
#include "thumbimageupdater.h"
#include "thumbnail.h"
#include "windows/rtwindow.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif // _WIN32

static void lowerBackgroundPreloadThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

static void setBackgroundPreloadThreadPriority(bool hotCandidate)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), hotCandidate ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

static void raiseForegroundLoadThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

class BackgroundPreloadOpenMPGuard
{
public:
    BackgroundPreloadOpenMPGuard()
    {
#ifdef _OPENMP
        previousThreads_ = std::max(1, omp_get_max_threads());
        omp_set_num_threads(std::max(1, std::min(previousThreads_, 3)));
#endif
    }

    ~BackgroundPreloadOpenMPGuard()
    {
#ifdef _OPENMP
        omp_set_num_threads(previousThreads_);
#endif
    }

private:
#ifdef _OPENMP
    int previousThreads_ = 1;
#endif
};

static double cachedPreviewScaleForEditor(
    Thumbnail* thumbnail,
    const Glib::RefPtr<Gdk::Pixbuf>& pixbuf,
    double fallbackScale)
{
    if (!thumbnail || !pixbuf || pixbuf->get_width() <= 0 || pixbuf->get_height() <= 0) {
        return fallbackScale > 0.0 ? fallbackScale : 1.0;
    }

    const CacheImageData* const cfs = thumbnail->getCacheImageData();
    if (!cfs || cfs->width <= 0 || cfs->height <= 0) {
        return fallbackScale > 0.0 ? fallbackScale : 1.0;
    }

    int fullW = cfs->width;
    int fullH = cfs->height;
    const auto& params = thumbnail->getProcParamsU();
    if (params.coarse.rotate == 90 || params.coarse.rotate == 270) {
        std::swap(fullW, fullH);
    }

    const double scaleW = static_cast<double>(fullW) / static_cast<double>(pixbuf->get_width());
    const double scaleH = static_cast<double>(fullH) / static_cast<double>(pixbuf->get_height());
    const double scale = std::max(scaleW, scaleH);
    return scale > 0.0 ? scale : (fallbackScale > 0.0 ? fallbackScale : 1.0);
}

// Serializes RAW InitialImage::load calls (foreground + preload). RawImageSource's
// load path uses rtengine globals / OMP pools that are not safe to run concurrently
// on two images at once: running two RAW decodes in parallel was observed to
// corrupt shared state and later crash rgbProc's OMP workers reading freed buffers.
// Foreground opens get priority: background preloads only start when no click-
// initiated load is active or queued.
struct RawLoadGate {
    enum class PreloadAcquireResult {
        Acquired,
        Busy,
        TooSoon
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool active = false;
    int foregroundWaiting = 0;
    std::unordered_set<std::string> editorActiveFiles;
    std::chrono::steady_clock::time_point lastForegroundActivity;
    std::string lastForegroundFile;
    static constexpr int kForegroundCancelPollMs = 25;

    void noteForegroundIntent(const std::string& fname = std::string())
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastForegroundActivity = std::chrono::steady_clock::now();
        lastForegroundFile = fname;
    }

    void setEditorActivity(const std::string& fname, bool active)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!fname.empty()) {
                if (active) {
                    editorActiveFiles.insert(fname);
                } else {
                    editorActiveFiles.erase(fname);
                }
            } else if (!active) {
                editorActiveFiles.clear();
            }

            lastForegroundActivity = std::chrono::steady_clock::now();
            lastForegroundFile = fname;
        }
        cv.notify_all();
    }

    struct ForegroundPressure {
        bool any = false;
        bool sameFile = false;
    };

    template<typename ShouldCancel>
    bool acquireForeground(ShouldCancel shouldCancel)
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++foregroundWaiting;
        lastForegroundActivity = std::chrono::steady_clock::now();

        auto stopWaiting = [this]() {
            --foregroundWaiting;
            cv.notify_all();
        };

        while (active) {
            if (shouldCancel()) {
                stopWaiting();
                return false;
            }

            cv.wait_for(
                lock,
                std::chrono::milliseconds(kForegroundCancelPollMs),
                [this] { return !active; });
        }

        if (shouldCancel()) {
            stopWaiting();
            return false;
        }

        --foregroundWaiting;
        active = true;
        return true;
    }

    void acquireForeground()
    {
        acquireForeground([] { return false; });
    }

    PreloadAcquireResult preloadReadinessLocked(
        std::chrono::milliseconds foregroundQuietFor,
        std::chrono::milliseconds& retryAfter,
        bool includeEditorActivity) const
    {
        retryAfter = std::chrono::milliseconds(0);

        if (active || foregroundWaiting > 0 || (includeEditorActivity && !editorActiveFiles.empty())) {
            return PreloadAcquireResult::Busy;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto quietFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastForegroundActivity);
        if (quietFor < foregroundQuietFor) {
            retryAfter = foregroundQuietFor - quietFor;
            return PreloadAcquireResult::TooSoon;
        }

        return PreloadAcquireResult::Acquired;
    }

    PreloadAcquireResult checkPreloadReadiness(
        std::chrono::milliseconds foregroundQuietFor,
        std::chrono::milliseconds& retryAfter,
        bool includeEditorActivity = true)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return preloadReadinessLocked(foregroundQuietFor, retryAfter, includeEditorActivity);
    }

    PreloadAcquireResult tryAcquirePreload(
        std::chrono::milliseconds foregroundQuietFor,
        std::chrono::milliseconds& retryAfter,
        bool includeEditorActivity = true)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = preloadReadinessLocked(foregroundQuietFor, retryAfter, includeEditorActivity);
        if (result != PreloadAcquireResult::Acquired) {
            return result;
        }

        active = true;
        return PreloadAcquireResult::Acquired;
    }

    ForegroundPressure foregroundPressureSince(
        const std::chrono::steady_clock::time_point& since,
        const std::string& fname,
        bool includeEditorActivity = true)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const bool any = foregroundWaiting > 0
            || (includeEditorActivity && !editorActiveFiles.empty())
            || lastForegroundActivity > since;
        return {
            any,
            any && !fname.empty() && lastForegroundFile == fname
        };
    }

    void release(bool foreground)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            active = false;
            if (foreground) {
                lastForegroundActivity = std::chrono::steady_clock::now();
            }
        }
        cv.notify_all();
    }
};

static RawLoadGate g_rawLoadGate;

void noteRawLoadForegroundActivity(const std::string& fname)
{
    g_rawLoadGate.noteForegroundIntent(fname);
}

bool isRawLoadForegroundQuietForMs(int quietMs)
{
    std::lock_guard<std::mutex> lock(g_rawLoadGate.mutex);
    if (g_rawLoadGate.active || g_rawLoadGate.foregroundWaiting > 0) {
        return false;
    }
    if (g_rawLoadGate.lastForegroundActivity == std::chrono::steady_clock::time_point{}) {
        return true;
    }

    const auto quietFor = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_rawLoadGate.lastForegroundActivity);
    return quietFor >= std::chrono::milliseconds(std::max(0, quietMs));
}

int rawLoadForegroundQuietRetryMs(int quietMs, int minRetryMs)
{
    const int minRetry = std::max(0, minRetryMs);
    const int quietTarget = std::max(0, quietMs);
    std::lock_guard<std::mutex> lock(g_rawLoadGate.mutex);

    if (g_rawLoadGate.active || g_rawLoadGate.foregroundWaiting > 0) {
        return minRetry;
    }
    if (g_rawLoadGate.lastForegroundActivity == std::chrono::steady_clock::time_point{}) {
        return 0;
    }

    const auto quietFor = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_rawLoadGate.lastForegroundActivity);
    const auto remaining = std::chrono::milliseconds(quietTarget) - quietFor;

    if (remaining <= std::chrono::milliseconds(0)) {
        return 0;
    }

    return std::max<int>(minRetry, static_cast<int>(remaining.count()));
}

void setRawLoadEditorActivity(const std::string& fname, bool active)
{
    g_rawLoadGate.setEditorActivity(fname, active);
}

struct RawLoadLease {
    bool acquired;
    bool foreground;
    RawLoadGate::PreloadAcquireResult preloadResult;
    std::chrono::milliseconds retryAfter;

    explicit RawLoadLease(
        bool foreground,
        std::chrono::milliseconds preloadQuietFor = std::chrono::milliseconds(0),
        bool includeEditorActivity = true) :
        acquired(false),
        foreground(foreground),
        preloadResult(RawLoadGate::PreloadAcquireResult::Busy),
        retryAfter(0)
    {
        if (foreground) {
            g_rawLoadGate.acquireForeground();
            acquired = true;
            preloadResult = RawLoadGate::PreloadAcquireResult::Acquired;
        } else {
            preloadResult = g_rawLoadGate.tryAcquirePreload(preloadQuietFor, retryAfter, includeEditorActivity);
            acquired = preloadResult == RawLoadGate::PreloadAcquireResult::Acquired;
        }
    }

    template<typename ShouldCancel>
    explicit RawLoadLease(ShouldCancel shouldCancel) :
        acquired(g_rawLoadGate.acquireForeground(shouldCancel)),
        foreground(true),
        preloadResult(acquired ? RawLoadGate::PreloadAcquireResult::Acquired : RawLoadGate::PreloadAcquireResult::Busy),
        retryAfter(0)
    {
    }

    ~RawLoadLease()
    {
        if (acquired) {
            g_rawLoadGate.release(foreground);
        }
    }

    void releaseNow()
    {
        if (acquired) {
            g_rawLoadGate.release(foreground);
            acquired = false;
        }
    }
};

static void g_fileSelLog(const char* fmt, ...)
{
    static std::mutex logMu;
    std::lock_guard<std::mutex> lk(logMu);
    static FILE* f = nullptr;
    if (!f) {
        const char* home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        std::string path = home ? std::string(home) + "\\steep-fileSel.log" : "steep-fileSel.log";
        // Open in append mode: other translation units (imagearea.cc,
        // thumbbrowserbase.cc) also log to this file; using "w" here
        // would truncate their earlier writes.
        f = std::fopen(path.c_str(), "a");
    }
    if (!f) return;
    using clk = std::chrono::steady_clock;
    static auto base = clk::now();
    long long tms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - base).count();
    std::fprintf(f, "[t=%lldms] ", tms);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fflush(f);
}

static bool g_fileSelLogEnabled()
{
    static const bool enabled = std::getenv("STEEP_FILESEL_LOG") != nullptr;
    return enabled;
}

#define FILESEL_LOG(...) \
    do { \
        if (g_fileSelLogEnabled()) { \
            g_fileSelLog(__VA_ARGS__); \
        } \
    } while (false)

static long long fileSelDurationMs(
    const std::chrono::steady_clock::time_point& from,
    const std::chrono::steady_clock::time_point& to)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

static std::atomic<unsigned long long> foregroundLoadPriorityIds(1);

static unsigned long long nextForegroundLoadPriorityId()
{
    return foregroundLoadPriorityIds.fetch_add(1, std::memory_order_relaxed);
}

#ifdef _WIN32
static std::mutex foregroundLoadPriorityMutex;
static std::unordered_map<unsigned long long, HANDLE> foregroundLoadPriorityHandles;
#endif

class ForegroundLoadPriorityHandle {
public:
    explicit ForegroundLoadPriorityHandle(unsigned long long priorityId) :
        priorityId_(priorityId)
    {
#ifdef _WIN32
        if (priorityId_ == 0) {
            return;
        }

        HANDLE duplicated = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(),
                GetCurrentThread(),
                GetCurrentProcess(),
                &duplicated,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            handle_ = duplicated;
            std::lock_guard<std::mutex> lock(foregroundLoadPriorityMutex);
            foregroundLoadPriorityHandles[priorityId_] = handle_;
        }
#endif
    }

    ~ForegroundLoadPriorityHandle()
    {
#ifdef _WIN32
        HANDLE closeHandle = nullptr;
        if (priorityId_ != 0 && handle_) {
            std::lock_guard<std::mutex> lock(foregroundLoadPriorityMutex);
            const auto it = foregroundLoadPriorityHandles.find(priorityId_);
            if (it != foregroundLoadPriorityHandles.end() && it->second == handle_) {
                closeHandle = handle_;
                foregroundLoadPriorityHandles.erase(it);
            }
        }

        if (closeHandle) {
            CloseHandle(closeHandle);
        }
#endif
    }

private:
    unsigned long long priorityId_;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
};

static bool demoteForegroundLoadPriority(unsigned long long priorityId)
{
#ifdef _WIN32
    if (priorityId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(foregroundLoadPriorityMutex);
    const auto it = foregroundLoadPriorityHandles.find(priorityId);
    return it != foregroundLoadPriorityHandles.end()
        && SetThreadPriority(it->second, THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)priorityId;
    return false;
#endif
}

enum class InitialImageReleaseLogKind {
    Discarded,
    PreloadCache
};

struct InitialImageReleaseTask {
    std::vector<rtengine::InitialImage*> images;
    std::string reason;
    std::string fname;
    InitialImageReleaseLogKind logKind;
};

class InitialImageReleaseQueue {
public:
    void enqueue(
        std::vector<rtengine::InitialImage*> images,
        const char* reason,
        const Glib::ustring& fname,
        InitialImageReleaseLogKind logKind)
    {
        if (images.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(InitialImageReleaseTask{
                std::move(images),
                reason ? reason : "",
                std::string(fname),
                logKind
            });

            if (!workerStarted_) {
                workerStarted_ = true;
                std::thread([this]() { run(); }).detach();
            }
        }

        cv_.notify_one();
    }

private:
    void run()
    {
        lowerBackgroundPreloadThreadPriority();

        while (true) {
            InitialImageReleaseTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return !queue_.empty(); });
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            const size_t count = task.images.size();
            const auto releaseStart = g_fileSelLogEnabled()
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};

            for (auto* img : task.images) {
                if (img) {
                    img->decreaseRef();
                }
            }

            if (!g_fileSelLogEnabled()) {
                continue;
            }

            if (task.logKind == InitialImageReleaseLogKind::Discarded) {
                FILESEL_LOG("[imageLoaded] discarded image release duration=%lldms reason=%s file=%s\n",
                    fileSelDurationMs(releaseStart, std::chrono::steady_clock::now()),
                    task.reason.c_str(),
                    task.fname.c_str());
            } else {
                FILESEL_LOG("[preload] cache release duration=%lldms count=%zu reason=%s file=%s\n",
                    fileSelDurationMs(releaseStart, std::chrono::steady_clock::now()),
                    count,
                    task.reason.c_str(),
                    task.fname.c_str());
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InitialImageReleaseTask> queue_;
    bool workerStarted_ = false;
};

static InitialImageReleaseQueue& initialImageReleaseQueue()
{
    static InitialImageReleaseQueue* queue = new InitialImageReleaseQueue();
    return *queue;
}

static void releaseInitialImageInBackground(
    rtengine::InitialImage* img,
    const char* reason,
    const Glib::ustring& fname)
{
    if (!img) {
        return;
    }

    std::vector<rtengine::InitialImage*> images;
    images.push_back(img);
    initialImageReleaseQueue().enqueue(
        std::move(images),
        reason,
        fname,
        InitialImageReleaseLogKind::Discarded);
}

static void releasePreloadImagesInBackground(
    std::vector<rtengine::InitialImage*> images,
    const char* reason,
    const Glib::ustring& fname)
{
    if (images.empty()) {
        return;
    }

    initialImageReleaseQueue().enqueue(
        std::move(images),
        reason,
        fname,
        InitialImageReleaseLogKind::PreloadCache);
}

// Bounded, thread-safe cache for pre-loaded adjacent InitialImages.
// Owned via shared_ptr so in-flight load threads can safely outlive FilePanel:
// they each hold a copy of the shared_ptr and check `stopped` before inserting.
struct PreloadManager {
    // Tunables. Keep full-image preloads narrow but useful: directional
    // navigation warms likely forward RAWs first, while the byte cap prevents
    // runaway RAW memory use.
    static constexpr size_t kMaxBytes    = 1024ULL * 1024 * 1024;
    static constexpr size_t kMaxEntries  = 3;
    static constexpr int    kRadius      = 4;
    static constexpr size_t kDirectionalBacktrackEntries = 0;
    static constexpr size_t kDirectionalLeadEntries = 1;
    static constexpr int    kThumbnailRefreshRadius = 2;
    static constexpr int    kQuickPreviewWarmRadius = 2;
    static constexpr int    kStartDelayMs = 1800;
    static constexpr int    kDirectionalStartDelayMs = 125;
    static constexpr int    kInterLoadDelayMs = 900;
    static constexpr int    kDirectionalInterLoadDelayMs = 350;
    static constexpr int    kForegroundQuietMs = 900;
    static constexpr int    kDirectionalForegroundQuietMs = 600;
    static constexpr int    kDirectionalThroughEditorRawQuietMs = 300;
    static constexpr int    kRapidDirectionalCadenceMs = 850;
    static constexpr int    kRapidDirectionalForegroundQuietMs = 600;
    static constexpr int    kRapidImmediateRawForegroundQuietMs = 150;
    static constexpr int    kRapidDecodeDebounceCadenceMs = 200;
    static constexpr int    kRapidDecodeDebounceMinMs = 110;
    static constexpr int    kRapidDecodeDebounceMaxMs = 180;
    static constexpr int    kRapidDecodeDebounceExtraMs = 10;
    static constexpr int    kRapidNonRawDecodeDebounceMinMs = 115;
    static constexpr int    kRapidNonRawDecodeDebounceMaxMs = 150;
    static constexpr int    kRapidNonRawDecodeDebounceExtraMs = 15;
    static constexpr unsigned kRapidNonRawDecodeDebounceRunLength = 2;
    static constexpr int    kDirectionalScrubDecodeDebounceCadenceMs = 350;
    static constexpr int    kDirectionalScrubDecodeDebounceMinMs = 260;
    static constexpr int    kDirectionalScrubDecodeDebounceMaxMs = 360;
    static constexpr int    kDirectionalScrubDecodeDebounceExtraMs = 10;
    static constexpr unsigned kDirectionalScrubDecodeDebounceRunLength = 3;
    static constexpr int    kNonRawForegroundQuietMs = 125;
    static constexpr int    kDirectionalHintKeepAliveMs = 1500;
    static constexpr int    kPreloadRetryMs = 75;
    static constexpr int    kPreloadBusyRetryMs = 250;
    static constexpr int    kDelayedRawPreloadReadyPollMs = 10;
    static constexpr int    kCachedOpenPreloadSettleMs = 0;
    static constexpr size_t kFallbackEntryBytes = 256ULL * 1024 * 1024;

    enum class ForegroundPriorityResult {
        NotQueued,
        Loading
    };

    struct Entry {
        rtengine::InitialImage* img;
        size_t bytes;
    };

    static size_t estimatedEntryBytes(const FileBrowser::AdjacentEntry& entry)
    {
        if (entry.estimatedBytes > 0) {
            return entry.estimatedBytes;
        }

        const size_t frames = entry.isRaw ? std::max(1u, entry.frameCount) : 1u;
        return kFallbackEntryBytes * frames;
    }

    static size_t nonRawBytesPerPixel(rtengine::IIOSampleFormat sampleFormat)
    {
        if (sampleFormat & rtengine::IIOSF_UNSIGNED_CHAR) {
            return 4;
        }
        if (sampleFormat & rtengine::IIOSF_UNSIGNED_SHORT) {
            return 8;
        }

        return 16;
    }

    static bool isLikely8BitJpegFile(const Glib::ustring& fname)
    {
        const Glib::ustring basename = Glib::path_get_basename(fname.lowercase());
        const Glib::ustring::size_type pos = basename.find_last_of('.');
        if (pos >= basename.length() - 1) {
            return false;
        }

        const Glib::ustring extension = basename.substr(pos + 1);
        return extension == "jpg" || extension == "jpeg" || extension == "jpe";
    }

    static size_t nonRawLoadedBytesPerPixel(rtengine::InitialImage* img, rtengine::IIOSampleFormat fallbackSampleFormat)
    {
        rtengine::IIOSampleFormat sampleFormat = fallbackSampleFormat;
        if (img) {
            if (const auto* metadata = img->getMetaData()) {
                const auto loadedSampleFormat = metadata->getSampleFormat();
                if (loadedSampleFormat != rtengine::IIOSF_UNKNOWN) {
                    sampleFormat = loadedSampleFormat;
                }
            }
            if (sampleFormat == rtengine::IIOSF_UNKNOWN && isLikely8BitJpegFile(img->getFileName())) {
                sampleFormat = rtengine::IIOSF_UNSIGNED_CHAR;
            }
        }

        return nonRawBytesPerPixel(sampleFormat);
    }

    static size_t estimatedLoadedEntryBytes(const FileBrowser::AdjacentEntry& entry, rtengine::InitialImage* img)
    {
        size_t bytes = estimatedEntryBytes(entry);
        if (!img) {
            return bytes;
        }

        auto* src = img->getImageSource();
        if (!src) {
            return bytes;
        }

        int w = 0;
        int h = 0;
        src->getFullSize(w, h);
        if (w <= 0 || h <= 0) {
            return bytes;
        }

        const size_t bytesPerPixel = entry.isRaw
            ? ((src->getSensorType() == rtengine::ST_BAYER || src->getSensorType() == rtengine::ST_FUJI_XTRANS) ? 6 : 12)
            : nonRawLoadedBytesPerPixel(img, entry.sampleFormat);

        const int frameCount = entry.isRaw ? src->getFrameCount() : 1;
        const size_t frames = static_cast<size_t>(std::max(1, frameCount));
        return static_cast<size_t>(w) * static_cast<size_t>(h) * bytesPerPixel * frames;
    }

    static size_t estimatedLoadedInitialImageBytes(rtengine::InitialImage* img, bool isRaw)
    {
        if (!img) {
            return kFallbackEntryBytes;
        }

        auto* src = img->getImageSource();
        if (!src) {
            return kFallbackEntryBytes;
        }

        int w = 0;
        int h = 0;
        src->getFullSize(w, h);
        if (w <= 0 || h <= 0) {
            return kFallbackEntryBytes;
        }

        const size_t bytesPerPixel = isRaw
            ? ((src->getSensorType() == rtengine::ST_BAYER || src->getSensorType() == rtengine::ST_FUJI_XTRANS) ? 6 : 12)
            : nonRawLoadedBytesPerPixel(img, rtengine::IIOSF_UNKNOWN);

        const int frameCount = isRaw ? src->getFrameCount() : 1;
        const size_t frames = static_cast<size_t>(std::max(1, frameCount));
        return static_cast<size_t>(w) * static_cast<size_t>(h) * bytesPerPixel * frames;
    }

    std::mutex              mutex;
    std::condition_variable cv;
    std::atomic<bool>       stopped{false};
    bool                    workerRunning = false;
    std::string             loading;      // actively decoding; eligible for foreground handoff
    std::unordered_map<std::string, Entry> cache;
    std::vector<std::string> wanted;      // ordered by priority, nearest first
    std::unordered_set<std::string> wantedSet;
    std::vector<std::string> hotWanted;   // ordered by decode priority
    std::unordered_set<std::string> hotWantedSet;
    std::unordered_set<std::string> foregroundHandoffSet;
    std::unordered_set<std::string> foregroundRecycleWantedSet;
    std::vector<FileBrowser::AdjacentEntry> wantedEntries;
    std::vector<FileBrowser::AdjacentEntry> hotWantedEntries;
    size_t                  totalBytes = 0;
    int                     startDelayMs = kStartDelayMs;
    int                     interLoadDelayMs = kInterLoadDelayMs;
    int                     foregroundQuietMs = kForegroundQuietMs;
    int                     immediateRawQuietMs = kForegroundQuietMs;
    bool                    rawStrideMode = false;
    bool                    rawStrideCanPreloadThroughEditor = false;
    unsigned                scheduleGeneration = 0;
#ifdef _WIN32
    HANDLE                  workerThreadHandle = nullptr;
#endif

    size_t priorityIndexLocked(const std::string& fname) const
    {
        auto it = std::find(wanted.begin(), wanted.end(), fname);
        if (it == wanted.end()) {
            return std::numeric_limits<size_t>::max();
        }
        return static_cast<size_t>(std::distance(wanted.begin(), it));
    }

    bool isLowerPriorityCachedLocked(
        const std::unordered_map<std::string, Entry>::const_iterator& it,
        size_t candidatePriority) const
    {
        const size_t cachedPriority = priorityIndexLocked(it->first);
        return cachedPriority > candidatePriority;
    }

    bool hasRoomForLocked(const std::string& fname, size_t bytesNeeded) const
    {
        const size_t candidatePriority = priorityIndexLocked(fname);
        if (candidatePriority == std::numeric_limits<size_t>::max()) {
            return false;
        }

        size_t evictableBytes = 0;
        size_t evictableCount = 0;
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (isLowerPriorityCachedLocked(it, candidatePriority)) {
                evictableBytes += it->second.bytes;
                ++evictableCount;
            }
        }

        const size_t cacheSizeAfterEviction = cache.size() > evictableCount ? cache.size() - evictableCount : 0;
        const size_t bytesAfterEviction = totalBytes > evictableBytes ? totalBytes - evictableBytes : 0;

        return cacheSizeAfterEviction < kMaxEntries
            && bytesNeeded <= kMaxBytes
            && bytesAfterEviction <= kMaxBytes - bytesNeeded;
    }

    bool makeRoomForLocked(
        const std::string& fname,
        size_t bytesNeeded,
        std::vector<rtengine::InitialImage*>* evictedImages = nullptr)
    {
        if (bytesNeeded > kMaxBytes) {
            return false;
        }

        const size_t candidatePriority = priorityIndexLocked(fname);
        if (candidatePriority == std::numeric_limits<size_t>::max()) {
            return false;
        }

        auto fits = [&]() {
            return cache.size() < kMaxEntries
                && totalBytes <= kMaxBytes - bytesNeeded;
        };

        while (!fits()) {
            auto victim = cache.end();
            size_t victimPriority = candidatePriority;

            for (auto it = cache.begin(); it != cache.end(); ++it) {
                const size_t cachedPriority = priorityIndexLocked(it->first);
                if (cachedPriority > candidatePriority
                    && (victim == cache.end() || cachedPriority > victimPriority)) {
                    victim = it;
                    victimPriority = cachedPriority;
                }
            }

            if (victim == cache.end()) {
                return false;
            }

            totalBytes = (totalBytes > victim->second.bytes)
                ? totalBytes - victim->second.bytes
                : 0;
            if (evictedImages) {
                evictedImages->push_back(victim->second.img);
            } else {
                victim->second.img->decreaseRef();
            }
            cache.erase(victim);
        }

        return true;
    }

    bool makeRoomForForegroundLocked(
        size_t bytesNeeded,
        std::vector<rtengine::InitialImage*>* evictedImages = nullptr)
    {
        if (bytesNeeded > kMaxBytes) {
            return false;
        }

        auto fits = [&]() {
            return cache.size() < kMaxEntries
                && totalBytes <= kMaxBytes - bytesNeeded;
        };

        while (!fits()) {
            if (cache.empty()) {
                return false;
            }

            auto victim = cache.end();
            size_t victimPriority = 0;
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                const size_t cachedPriority = priorityIndexLocked(it->first);
                if (victim == cache.end() || cachedPriority > victimPriority) {
                    victim = it;
                    victimPriority = cachedPriority;
                }
            }

            totalBytes = (totalBytes > victim->second.bytes)
                ? totalBytes - victim->second.bytes
                : 0;
            if (evictedImages) {
                evictedImages->push_back(victim->second.img);
            } else {
                victim->second.img->decreaseRef();
            }
            cache.erase(victim);
        }

        return true;
    }

    ~PreloadManager() {
        // Last shared_ptr ref released: no thread still touches us.
        for (auto& kv : cache) {
            kv.second.img->decreaseRef();
        }
    }

    void forgetWantedLocked(const std::string& fname) {
        auto wi = std::find(wanted.begin(), wanted.end(), fname);
        if (wi != wanted.end()) wanted.erase(wi);
        wantedSet.erase(fname);
        auto hwi = std::find(hotWanted.begin(), hotWanted.end(), fname);
        if (hwi != hotWanted.end()) hotWanted.erase(hwi);
        hotWantedSet.erase(fname);
        wantedEntries.erase(
            std::remove_if(
                wantedEntries.begin(),
                wantedEntries.end(),
                [&fname](const FileBrowser::AdjacentEntry& e) {
                    return e.fnameRaw == fname;
                }),
            wantedEntries.end());
        hotWantedEntries.erase(
            std::remove_if(
                hotWantedEntries.begin(),
                hotWantedEntries.end(),
                [&fname](const FileBrowser::AdjacentEntry& e) {
                    return e.fnameRaw == fname;
                }),
            hotWantedEntries.end());
    }

    void clearLoadingLocked(const std::string& fname = std::string())
    {
        if (loading.empty()) {
            return;
        }

        if (fname.empty() || loading == fname) {
            loading.clear();
            cv.notify_all();
        }
    }

    void keepOnlyForegroundWantedLocked(const std::string& fname)
    {
        wanted.assign(1, fname);
        wantedSet.clear();
        wantedSet.insert(fname);
        hotWanted.assign(1, fname);
        hotWantedSet.clear();
        hotWantedSet.insert(fname);
        wantedEntries.erase(
            std::remove_if(
                wantedEntries.begin(),
                wantedEntries.end(),
                [&fname](const FileBrowser::AdjacentEntry& e) {
                    return e.fnameRaw != fname;
                }),
            wantedEntries.end());
        hotWantedEntries.erase(
            std::remove_if(
                hotWantedEntries.begin(),
                hotWantedEntries.end(),
                [&fname](const FileBrowser::AdjacentEntry& e) {
                    return e.fnameRaw != fname;
                }),
            hotWantedEntries.end());
    }

    // Pop an entry if cached; transfers ownership to caller (no decreaseRef).
    rtengine::InitialImage* take(const std::string& fname) {
        std::lock_guard<std::mutex> lk(mutex);
        auto it = cache.find(fname);
        if (it == cache.end()) return nullptr;
        auto* img = it->second.img;
        totalBytes = (totalBytes > it->second.bytes) ? totalBytes - it->second.bytes : 0;
        cache.erase(it);
        forgetWantedLocked(fname);
        foregroundHandoffSet.erase(fname);
        foregroundRecycleWantedSet.erase(fname);
        cv.notify_all();
        return img;
    }

    template<typename ShouldCancel>
    rtengine::InitialImage* takeOrWaitForLoading(const std::string& fname, ShouldCancel shouldCancel)
    {
        std::unique_lock<std::mutex> lk(mutex);

        while (true) {
            auto it = cache.find(fname);
            if (it != cache.end()) {
                auto* img = it->second.img;
                totalBytes = (totalBytes > it->second.bytes) ? totalBytes - it->second.bytes : 0;
                cache.erase(it);
                forgetWantedLocked(fname);
                foregroundHandoffSet.erase(fname);
                foregroundRecycleWantedSet.erase(fname);
                cv.notify_all();
                return img;
            }

            const bool waitingForLoading = loading == fname;
            const bool waitingForRecycle = foregroundRecycleWantedSet.count(fname) != 0;
            if (stopped || (!waitingForLoading && !waitingForRecycle)) {
                foregroundHandoffSet.erase(fname);
                return nullptr;
            }

            if (waitingForLoading) {
                foregroundHandoffSet.insert(fname);
            }

            if (shouldCancel()) {
                foregroundHandoffSet.erase(fname);
                return nullptr;
            }

            cv.wait_for(lk, std::chrono::milliseconds(25));
        }
    }

    bool isWanted(const std::string& fname) {
        // Caller must hold mutex.
        return wantedSet.count(fname) != 0;
    }

    bool isHotWanted(const std::string& fname) {
        // Caller must hold mutex.
        return hotWantedSet.count(fname) != 0;
    }

    bool isLoading(const std::string& fname) {
        std::lock_guard<std::mutex> lk(mutex);
        return loading == fname;
    }

    bool hasCachedOrLoading(const std::string& fname) {
        std::lock_guard<std::mutex> lk(mutex);
        return loading == fname || cache.count(fname) != 0;
    }

    long long waitForDifferentLoadToSettle(const std::string& fname, std::chrono::milliseconds timeout)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + timeout;
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait_until(lk, deadline, [&]() {
            return stopped || loading.empty() || loading == fname;
        });

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    }

    void markForegroundRecycleTarget(const std::string& fname)
    {
        std::lock_guard<std::mutex> lk(mutex);
        foregroundRecycleWantedSet.insert(fname);
        cv.notify_all();
    }

    void clearForegroundRecycleTarget(const std::string& fname)
    {
        std::lock_guard<std::mutex> lk(mutex);
        foregroundRecycleWantedSet.erase(fname);
        cv.notify_all();
    }

    bool cacheDecodedIfHotWanted(
        const std::string& fname,
        rtengine::InitialImage* img,
        size_t bytes,
        std::vector<rtengine::InitialImage*>* evictedImages = nullptr)
    {
        if (!img) {
            return false;
        }

        std::lock_guard<std::mutex> lk(mutex);
        const bool wantedForRecycle = foregroundRecycleWantedSet.count(fname) != 0;
        if (stopped
            || (!isHotWanted(fname) && !wantedForRecycle)
            || cache.count(fname)
            || bytes > kMaxBytes) {
            return false;
        }

        if (!wantedForRecycle && !hasRoomForLocked(fname, bytes)) {
            return false;
        }

        auto fits = [&]() {
            return cache.size() < kMaxEntries
                && totalBytes <= kMaxBytes - bytes;
        };

        const size_t candidatePriority = wantedForRecycle ? 0 : priorityIndexLocked(fname);
        while (!fits()) {
            auto victim = cache.end();
            size_t victimPriority = 0;

            for (auto it = cache.begin(); it != cache.end(); ++it) {
                const size_t cachedPriority = priorityIndexLocked(it->first);
                const bool canEvict = wantedForRecycle || cachedPriority > candidatePriority;
                if (canEvict
                    && (victim == cache.end() || cachedPriority > victimPriority)) {
                    victim = it;
                    victimPriority = cachedPriority;
                }
            }

            if (victim == cache.end()) {
                return false;
            }

            totalBytes = (totalBytes > victim->second.bytes)
                ? totalBytes - victim->second.bytes
                : 0;
            if (evictedImages) {
                evictedImages->push_back(victim->second.img);
            } else {
                victim->second.img->decreaseRef();
            }
            cache.erase(victim);
        }

        cache.emplace(fname, Entry{img, bytes});
        totalBytes += bytes;
        foregroundRecycleWantedSet.erase(fname);
        cv.notify_all();
        return true;
    }

    ForegroundPriorityResult prioritizeForeground(const std::string& fname) {
        std::lock_guard<std::mutex> lk(mutex);
        const bool loadingForeground = loading == fname;
        const bool loadingOther = !loading.empty() && !loadingForeground;

        if (loadingForeground) {
            foregroundHandoffSet.insert(fname);
            // Foreground can take over an already-started preload via
            // takeOrWaitForLoading(); keep only that image wanted so the
            // decoded result is handed off instead of discarded.
            keepOnlyForegroundWantedLocked(fname);
        } else {
            // Drop this image from adjacent preloading so the foreground load
            // has unambiguous ownership of the currently selected file.
            forgetWantedLocked(fname);
        }

#ifdef _WIN32
        if (workerThreadHandle) {
            if (loadingForeground) {
                SetThreadPriority(workerThreadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
            } else if (loadingOther) {
                // A different in-flight preload is holding the RAW gate. Finish
                // it at normal priority so the foreground request waits less;
                // the worker lowers itself again before the next candidate.
                SetThreadPriority(workerThreadHandle, THREAD_PRIORITY_NORMAL);
            }
        }
#endif
        cv.notify_all();
        if (loadingForeground) {
            return ForegroundPriorityResult::Loading;
        }

        return ForegroundPriorityResult::NotQueued;
    }
};

#ifdef _WIN32
struct PreloadWorkerPriorityHandle {
    explicit PreloadWorkerPriorityHandle(const std::shared_ptr<PreloadManager>& state) :
        state(state),
        handle(nullptr)
    {
        HANDLE duplicated = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(),
                GetCurrentThread(),
                GetCurrentProcess(),
                &duplicated,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            handle = duplicated;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->workerThreadHandle = handle;
        }
    }

    ~PreloadWorkerPriorityHandle()
    {
        HANDLE closeHandle = nullptr;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->workerThreadHandle == handle) {
                state->workerThreadHandle = nullptr;
                closeHandle = handle;
            }
        }

        if (closeHandle) {
            CloseHandle(closeHandle);
        }
    }

    std::shared_ptr<PreloadManager> state;
    HANDLE handle;
};
#endif

struct ForegroundLoadRequest {
    std::shared_ptr<std::atomic<unsigned>> generation;
    std::shared_ptr<std::atomic<bool>> staleSkipped;
    unsigned requestGeneration = 0;
    unsigned long long priorityId = 0;
    bool skipIfStale = false;
    int decodeStartDelayMs = 0;

    bool isStale() const
    {
        return skipIfStale
            && generation
            && generation->load(std::memory_order_acquire) != requestGeneration;
    }

    void markStaleSkipped() const
    {
        if (staleSkipped) {
            staleSkipped->store(true, std::memory_order_release);
        }
    }
};

static rtengine::InitialImage* loadInitialImageSerialized(
    Glib::ustring fname,
    std::string fnameRaw,
    bool isRaw,
    int* errorCode,
    rtengine::ProgressListener* pl,
    std::shared_ptr<PreloadManager> preload,
    std::shared_ptr<ForegroundLoadRequest> request)
{
    using clk = std::chrono::steady_clock;
    const bool logSelection = g_fileSelLogEnabled();
    const auto loadStart = logSelection ? clk::now() : clk::time_point{};

    raiseForegroundLoadThreadPriority();
    ForegroundLoadPriorityHandle foregroundPriorityHandle(request ? request->priorityId : 0);

    auto elapsedFromStart = [&]() -> long long {
        return fileSelDurationMs(loadStart, clk::now());
    };

    auto finishWithoutLoad = [&](const char* reason) -> rtengine::InitialImage* {
        if (errorCode) {
            *errorCode = 0;
        }
        if (preload && request && request->isStale()) {
            preload->clearForegroundRecycleTarget(fnameRaw);
        }
        if (logSelection) {
            FILESEL_LOG("[loadInitial] +%lldms canceled (%s) raw=%d file=%s\n",
                elapsedFromStart(), reason, static_cast<int>(isRaw), fnameRaw.c_str());
        }
        return nullptr;
    };

    const auto stale = [&]() {
        const bool result = request && request->isStale();
        if (result) {
            request->markStaleSkipped();
        }
        return result;
    };

    auto tryPreloadHandoff = [&](const char* phase) -> rtengine::InitialImage* {
        if (!preload) {
            return nullptr;
        }

        const auto waitStart = logSelection ? clk::now() : clk::time_point{};
        auto* cached = preload->takeOrWaitForLoading(fnameRaw, stale);
        if (logSelection) {
            const auto waitedMs = fileSelDurationMs(waitStart, clk::now());
            if (cached) {
                FILESEL_LOG("[loadInitial] +%lldms preload handoff HIT (%s) wait=%lldms file=%s\n",
                    elapsedFromStart(), phase, waitedMs, fnameRaw.c_str());
            } else if (waitedMs >= 10) {
                FILESEL_LOG("[loadInitial] +%lldms preload handoff miss (%s) wait=%lldms file=%s\n",
                    elapsedFromStart(), phase, waitedMs, fnameRaw.c_str());
            }
        }
        return cached;
    };

    if (logSelection) {
        FILESEL_LOG("[loadInitial] enter raw=%d file=%s\n",
            static_cast<int>(isRaw), fnameRaw.c_str());
    }

    if (stale()) {
        return finishWithoutLoad("stale before preload handoff");
    }

    if (auto* cached = tryPreloadHandoff("before gate")) {
        if (errorCode) {
            *errorCode = 0;
        }
        return cached;
    }

    if (stale()) {
        return finishWithoutLoad("stale after preload handoff");
    }

    if (request && request->decodeStartDelayMs > 0) {
        const auto delayStart = logSelection ? clk::now() : clk::time_point{};
        auto remaining = std::chrono::milliseconds(request->decodeStartDelayMs);
        while (remaining.count() > 0) {
            if (stale()) {
                return finishWithoutLoad("stale during rapid decode debounce");
            }

            const auto slice = std::min(remaining, std::chrono::milliseconds(15));
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
        if (logSelection) {
            FILESEL_LOG("[loadInitial] +%lldms rapid decode debounce waited=%lldms file=%s\n",
                elapsedFromStart(),
                fileSelDurationMs(delayStart, clk::now()),
                fnameRaw.c_str());
        }

        if (stale()) {
            return finishWithoutLoad("stale after rapid decode debounce");
        }

        if (auto* cached = tryPreloadHandoff("after rapid debounce")) {
            if (errorCode) {
                *errorCode = 0;
            }
            return cached;
        }

        if (stale()) {
            return finishWithoutLoad("stale after rapid debounce handoff");
        }
    }

    std::unique_ptr<RawLoadLease> loadLease;
    if (isRaw) {
        const auto gateStart = logSelection ? clk::now() : clk::time_point{};
        loadLease.reset(new RawLoadLease([&]() {
            return stale();
        }));
        if (!loadLease->acquired) {
            if (logSelection) {
                FILESEL_LOG("[loadInitial] +%lldms raw gate wait canceled wait=%lldms file=%s\n",
                    elapsedFromStart(),
                    fileSelDurationMs(gateStart, clk::now()),
                    fnameRaw.c_str());
            }
            return finishWithoutLoad("raw gate wait canceled");
        }
        if (logSelection) {
            FILESEL_LOG("[loadInitial] +%lldms raw gate acquired wait=%lldms file=%s\n",
                elapsedFromStart(),
                fileSelDurationMs(gateStart, clk::now()),
                fnameRaw.c_str());
        }
    }

    if (stale()) {
        return finishWithoutLoad("stale after gate");
    }

    if (auto* cached = tryPreloadHandoff("after gate")) {
        if (errorCode) {
            *errorCode = 0;
        }
        return cached;
    }

    if (stale()) {
        return finishWithoutLoad("stale before decode");
    }

    const auto decodeStart = logSelection ? clk::now() : clk::time_point{};
    if (logSelection) {
        FILESEL_LOG("[loadInitial] +%lldms decode start raw=%d file=%s\n",
            elapsedFromStart(), static_cast<int>(isRaw), fnameRaw.c_str());
    }

    auto* img = rtengine::InitialImage::load(fname, isRaw, errorCode, pl);

    if (logSelection) {
        FILESEL_LOG("[loadInitial] +%lldms decode done duration=%lldms raw=%d err=%d result=%d file=%s\n",
            elapsedFromStart(),
            fileSelDurationMs(decodeStart, clk::now()),
            static_cast<int>(isRaw),
            errorCode ? *errorCode : 0,
            static_cast<int>(img != nullptr),
            fnameRaw.c_str());
    }

    if (img && request && request->isStale() && preload) {
        const size_t cachedBytes = PreloadManager::estimatedLoadedInitialImageBytes(img, isRaw);
        std::vector<rtengine::InitialImage*> evictedImages;
        if (preload->cacheDecodedIfHotWanted(fnameRaw, img, cachedBytes, &evictedImages)) {
            releasePreloadImagesInBackground(std::move(evictedImages), "foreground-recycle", fname);
            request->markStaleSkipped();
            if (errorCode) {
                *errorCode = 0;
            }
            if (logSelection) {
                FILESEL_LOG("[preload] recycled superseded foreground bytes=%zu file=%s\n",
                    cachedBytes,
                    fnameRaw.c_str());
            }
            return nullptr;
        }
        preload->clearForegroundRecycleTarget(fnameRaw);
    }

    return img;
}

rtengine::InitialImage* FilePanel::loadAuxiliaryInitialImage(
    const Glib::ustring& fname,
    bool isRaw,
    int* errorCode,
    const std::shared_ptr<std::atomic<bool>>& cancel)
{
    using clk = std::chrono::steady_clock;
    const bool logSelection = g_fileSelLogEnabled();
    const auto loadStart = logSelection ? clk::now() : clk::time_point{};
    const std::string fnameRaw(fname);

    lowerBackgroundPreloadThreadPriority();

    auto elapsedFromStart = [&]() -> long long {
        return fileSelDurationMs(loadStart, clk::now());
    };
    auto canceled = [&]() {
        return cancel && cancel->load(std::memory_order_acquire);
    };
    auto sleepCancellable = [&](std::chrono::milliseconds delay) {
        auto remaining = delay;
        while (remaining.count() > 0) {
            if (canceled()) {
                return true;
            }

            const auto slice = std::min(remaining, std::chrono::milliseconds(25));
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }

        return canceled();
    };
    auto finishCanceled = [&](const char* phase) -> rtengine::InitialImage* {
        if (errorCode) {
            *errorCode = 0;
        }
        if (logSelection) {
            FILESEL_LOG("[auxLoad] +%lldms canceled (%s) raw=%d file=%s\n",
                elapsedFromStart(), phase, static_cast<int>(isRaw), fnameRaw.c_str());
        }
        return nullptr;
    };

    if (logSelection) {
        FILESEL_LOG("[auxLoad] enter raw=%d file=%s\n", static_cast<int>(isRaw), fnameRaw.c_str());
    }

    if (canceled()) {
        return finishCanceled("before gate");
    }

    std::unique_ptr<RawLoadLease> loadLease;
    if (isRaw) {
        while (true) {
            loadLease.reset(new RawLoadLease(
                false,
                std::chrono::milliseconds(PreloadManager::kForegroundQuietMs)));
            if (loadLease->acquired) {
                break;
            }

            auto retryDelay = std::chrono::milliseconds(PreloadManager::kPreloadRetryMs);
            if (loadLease->preloadResult == RawLoadGate::PreloadAcquireResult::TooSoon) {
                retryDelay = std::max(retryDelay, loadLease->retryAfter);
            }
            if (logSelection) {
                FILESEL_LOG("[auxLoad] +%lldms raw gate deferred retry=%lldms file=%s\n",
                    elapsedFromStart(),
                    static_cast<long long>(retryDelay.count()),
                    fnameRaw.c_str());
            }

            loadLease.reset();
            if (sleepCancellable(retryDelay)) {
                return finishCanceled("waiting for gate");
            }
        }

        if (logSelection) {
            FILESEL_LOG("[auxLoad] +%lldms raw gate acquired file=%s\n",
                elapsedFromStart(), fnameRaw.c_str());
        }
    }

    if (canceled()) {
        return finishCanceled("after gate");
    }

    const auto decodeStart = logSelection ? clk::now() : clk::time_point{};
    auto* img = rtengine::InitialImage::load(fname, isRaw, errorCode, nullptr);
    if (logSelection) {
        FILESEL_LOG("[auxLoad] +%lldms decode done duration=%lldms raw=%d err=%d result=%d file=%s\n",
            elapsedFromStart(),
            fileSelDurationMs(decodeStart, clk::now()),
            static_cast<int>(isRaw),
            errorCode ? *errorCode : 0,
            static_cast<int>(img != nullptr),
            fnameRaw.c_str());
    }

    return img;
}

void FilePanel::cancelScheduledBackgroundResume()
{
    if (backgroundResumeTimeoutId_) {
        g_source_remove(backgroundResumeTimeoutId_);
        backgroundResumeTimeoutId_ = 0;
    }
}

void FilePanel::cancelUnstartedPendingLoads(const char* reason, const Glib::ustring& currentFile)
{
    std::vector<pendingLoad*> canceledLoads;
    pendingLoadMutex.lock();
    for (auto it = pendingLoads.begin(); it != pendingLoads.end(); ) {
        pendingLoad* const pending = *it;
        if (pending
            && !pending->loadStarted
            && pending->delayedStartConn.connected()) {
            pending->superseded = true;
            if (pending->quickPreviewAllowed) {
                pending->quickPreviewAllowed->store(false, std::memory_order_release);
            }
            pending->delayedStartConn.disconnect();
            canceledLoads.push_back(pending);
            it = pendingLoads.erase(it);
        } else {
            ++it;
        }
    }
    pendingLoadMutex.unlock();

    if (canceledLoads.empty()) {
        return;
    }

    for (pendingLoad* const pending : canceledLoads) {
        if (pending->thm) {
            pending->thm->imageLoad(false);
            pending->thm->decreaseRef();
        }
        delete pending->pc;
        delete pending;
    }

    FILESEL_LOG("[fileSel] canceled delayed pending loads=%zu reason=%s file=%s\n",
        canceledLoads.size(),
        reason ? reason : "",
        currentFile.c_str());
}

void FilePanel::pauseBackgroundWorkForForeground()
{
    ++backgroundResumeGeneration_;
    cancelScheduledBackgroundResume();

    if (!backgroundWorkPausedForForeground_) {
        fileCatalog->pausePreviewBatchProcessing();
        previewLoader->pause();
        backgroundWorkPausedForForeground_ = true;
    }

    if (!thumbnailWorkPausedForForeground_) {
        thumbImageUpdater->pause();
        FileBrowserEntry::pauseQueuedImageUpdates();
        thumbnailWorkPausedForForeground_ = true;
    }
}

void FilePanel::resumeThumbnailWorkNow()
{
    if (!thumbnailWorkPausedForForeground_) {
        return;
    }

    thumbImageUpdater->resume();
    FileBrowserEntry::resumeQueuedImageUpdates();
    thumbnailWorkPausedForForeground_ = false;
}

void FilePanel::resumeBackgroundWorkNow()
{
    if (!backgroundWorkPausedForForeground_ && !thumbnailWorkPausedForForeground_) {
        return;
    }

    resumeThumbnailWorkNow();

    if (!backgroundWorkPausedForForeground_) {
        return;
    }

    previewLoader->resume();
    fileCatalog->resumePreviewBatchProcessing();
    backgroundWorkPausedForForeground_ = false;
}

void FilePanel::resumeBackgroundWorkIfCurrent(unsigned generation)
{
    if (generation != backgroundResumeGeneration_) {
        return;
    }

    backgroundResumeTimeoutId_ = 0;
    resumeBackgroundWorkNow();
}

void FilePanel::resumeThumbnailWorkIfCurrent(unsigned generation)
{
    if (generation != backgroundResumeGeneration_) {
        return;
    }

    resumeThumbnailWorkNow();
}

void FilePanel::resumeBackgroundWorkAfterForeground()
{
    if (!backgroundWorkPausedForForeground_ && !thumbnailWorkPausedForForeground_) {
        return;
    }

    ++backgroundResumeGeneration_;
    cancelScheduledBackgroundResume();

    struct ResumeData {
        FilePanel* panel;
        unsigned generation;
    };

    static constexpr guint kBackgroundResumeDelayMs =
        PreloadManager::kForegroundQuietMs + PreloadManager::kPreloadRetryMs;
    const unsigned generation = backgroundResumeGeneration_;

    auto* data = new ResumeData{this, generation};
    backgroundResumeTimeoutId_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        kBackgroundResumeDelayMs,
        [](gpointer userData) -> gboolean {
            auto* resumeData = static_cast<ResumeData*>(userData);
            resumeData->panel->resumeBackgroundWorkIfCurrent(resumeData->generation);
            return G_SOURCE_REMOVE;
        },
        data,
        [](gpointer userData) {
            delete static_cast<ResumeData*>(userData);
        });
}

void FilePanel::scheduleAdjacentPreload(const Glib::ustring& fname, eRTNav preferredDirection, bool refreshThumbnails)
{
    const auto now = std::chrono::steady_clock::now();
    eRTNav effectiveDirection = preferredDirection;

    if (preferredDirection == NAV_NEXT || preferredDirection == NAV_PREVIOUS) {
        recentDirectionalPreloadFname_ = fname;
        recentDirectionalPreloadDirection_ = preferredDirection;
        recentDirectionalPreloadUntil_ = now + std::chrono::milliseconds(PreloadManager::kDirectionalHintKeepAliveMs);
    } else if (!recentDirectionalPreloadFname_.empty()
        && now < recentDirectionalPreloadUntil_
        && (recentDirectionalPreloadFname_ == fname
            || (recentDirectionalSelectionGapMs_ > 0
                && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDirectionalCadenceMs))) {
        effectiveDirection = recentDirectionalPreloadDirection_;
        FILESEL_LOG("[preload] preserving directional hint dir=%d anchor=%s\n",
            static_cast<int>(effectiveDirection),
            fname.c_str());
    } else if (!recentDirectionalPreloadFname_.empty()
        && (recentDirectionalPreloadFname_ != fname || now >= recentDirectionalPreloadUntil_)) {
        recentDirectionalPreloadFname_.clear();
        recentDirectionalPreloadDirection_ = NAV_NONE;
        recentDirectionalPreloadUntil_ = {};
    }

    if (adjacentPreloadIdlePending_
        && adjacentPreloadFname_ == fname
        && adjacentPreloadDirection_ == effectiveDirection) {
        adjacentPreloadRefreshThumbnails_ = adjacentPreloadRefreshThumbnails_ || refreshThumbnails;
        FILESEL_LOG("[preload] coalesced pending schedule dir=%d refresh=%d anchor=%s\n",
            static_cast<int>(effectiveDirection),
            static_cast<int>(adjacentPreloadRefreshThumbnails_),
            fname.c_str());
        return;
    }

    const unsigned generation = ++adjacentPreloadGeneration_;
    adjacentPreloadIdlePending_ = true;
    adjacentPreloadFname_ = fname;
    adjacentPreloadDirection_ = effectiveDirection;
    adjacentPreloadRefreshThumbnails_ = refreshThumbnails;

    idle_register.add(
        [this, generation]() -> bool {
            if (generation != adjacentPreloadGeneration_) {
                return false;
            }

            const Glib::ustring scheduledFname = adjacentPreloadFname_;
            const eRTNav scheduledDirection = adjacentPreloadDirection_;
            const bool scheduledRefresh = adjacentPreloadRefreshThumbnails_;
            adjacentPreloadIdlePending_ = false;
            preloadAdjacent(scheduledFname, scheduledDirection, scheduledRefresh);
            return false;
        },
        G_PRIORITY_DEFAULT_IDLE);
}

FilePanel::FilePanel () :
    parent(nullptr),
    error(0),
    foregroundLoadGeneration_(std::make_shared<std::atomic<unsigned>>(0)),
    backgroundResumeTimeoutId_(0),
    backgroundResumeGeneration_(0),
    adjacentPreloadGeneration_(0),
    backgroundWorkPausedForForeground_(false),
    thumbnailWorkPausedForForeground_(false),
    adjacentPreloadIdlePending_(false),
    adjacentPreloadDirection_(NAV_NONE),
    adjacentPreloadRefreshThumbnails_(false),
    recentDirectionalPreloadDirection_(NAV_NONE),
    recentDirectionalSelectionDirection_(NAV_NONE),
    recentDirectionalSelectionGapMs_(0),
    recentDirectionalSelectionRunLength_(0),
    recentDirectionalSelectionWasRaw_(false),
    recentDirectionalRawSelectionRunLength_(0)
{
    preload_ = std::make_shared<PreloadManager>();


    const auto& options = App::get().options();

    // Contains everything except for the batch Tool Panel and tabs (Fast Export, Inspect, etc)
    dirpaned = Gtk::manage ( new Gtk::Paned () );
    dirpaned->set_position (options.dirBrowserWidth);

    // The directory tree
    dirBrowser = Gtk::manage ( new DirBrowser () );
    // Places
    placesBrowser = Gtk::manage ( new PlacesBrowser () );
    // Recent Folders
    recentBrowser = Gtk::manage ( new RecentBrowser () );
    // Albums
    albumBrowser_ = Gtk::manage ( new AlbumBrowser () );

    // The whole left panel. Contains Places, Recent Folders, Folders and Albums.
    placespaned = Gtk::manage ( new Gtk::Paned (Gtk::ORIENTATION_VERTICAL) );
    placespaned->set_name ("PlacesPaned");
    placespaned->set_size_request(250, -1);

    Gtk::Box* obox = Gtk::manage (new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    obox->get_style_context()->add_class ("plainback");
    obox->pack_start (*dirBrowser, Gtk::PACK_EXPAND_WIDGET, 0);
    dirBrowser->set_size_request(-1, 200);
    obox->pack_start (*recentBrowser, Gtk::PACK_SHRINK, 4);
    obox->pack_start (*albumBrowser_, Gtk::PACK_SHRINK, 0);

    placespaned->pack1 (*placesBrowser, false, false); // no resize, no shrink
    placespaned->pack2 (*obox, true, false);            // resize, no shrink
    int placesPos = std::max(options.dirBrowserHeight, 300);
    placespaned->set_position(placesPos);
    // Guard: prevent GTK from collapsing the places panel
    placespaned->property_position().signal_changed().connect([this]() {
        if (placespaned->get_position() < 200) {
            placespaned->set_position(std::max(App::get().options().dirBrowserHeight, 300));
        }
    });

    // Wire album selection to filter and album view
    albumBrowser_->albumSelected().connect(sigc::mem_fun(*this, &FilePanel::onAlbumSelected));
    albumBrowser_->albumViewRequested().connect(sigc::mem_fun(*this, &FilePanel::onAlbumViewRequested));

    dirpaned->pack1 (*placespaned, false, false);

    tpc = new BatchToolPanelCoordinator (this);
    // Location bar
    fileCatalog = Gtk::manage ( new FileCatalog (tpc->coarse, tpc->getToolBar(), this) );
    fileCatalog->tbLeftPanel_1_visible(false); // left toggle now in FilePanel footer
    // Holds the location bar and thumbnails
    ribbonPane = Gtk::manage ( new Gtk::Paned() );
    ribbonPane->add(*fileCatalog);
    ribbonPane->set_size_request(50, 150);
    dirpaned->pack2 (*ribbonPane, true, true);

    DirBrowser::DirSelectionSignal dirSelected = dirBrowser->dirSelected ();
    dirSelected.connect (sigc::mem_fun (fileCatalog, &FileCatalog::dirSelected));
    dirSelected.connect (sigc::mem_fun (recentBrowser, &RecentBrowser::dirSelected));
    dirSelected.connect (sigc::mem_fun (placesBrowser, &PlacesBrowser::dirSelected));
    dirSelected.connect (sigc::mem_fun (tpc, &BatchToolPanelCoordinator::dirSelected));
    dirSelected.connect ([this](const Glib::ustring& dir, const Glib::ustring&) {
        albumBrowser_->setCurrentDirectory(dir);
    });
    fileCatalog->setDirSelector (sigc::mem_fun (dirBrowser, &DirBrowser::selectDir));
    placesBrowser->setDirSelector (sigc::mem_fun (dirBrowser, &DirBrowser::selectDir));
    recentBrowser->setDirSelector (sigc::mem_fun (dirBrowser, &DirBrowser::selectDir));
    fileCatalog->setFileSelectionListener (this);

    rightBox = Gtk::manage ( new Gtk::Box () );
    rightBox->set_size_request(350, 100);
    rightNotebook = Gtk::manage ( new Gtk::Notebook () );
    rightNotebookSwitchConn = rightNotebook->signal_switch_page().connect_notify( sigc::mem_fun(*this, &FilePanel::on_NB_switch_page) );
    //Gtk::Box* taggingBox = Gtk::manage ( new Gtk::Box(Gtk::ORIENTATION_VERTICAL) );

    history = Gtk::manage ( new History (false) );

    tpc->addPParamsChangeListener (history);
    history->setProfileChangeListener (tpc);
    history->set_size_request(-1, 50);

    Gtk::ScrolledWindow* sFilterPanel = Gtk::manage ( new Gtk::ScrolledWindow() );
    filterPanel = Gtk::manage ( new FilterPanel () );
    sFilterPanel->add (*filterPanel);

    inspectorPanel = new Inspector();
    fileCatalog->setInspector(inspectorPanel);

    Gtk::ScrolledWindow* sExportPanel = Gtk::manage ( new Gtk::ScrolledWindow() );
    exportPanel = Gtk::manage ( new ExportPanel () );
    sExportPanel->add (*exportPanel);
    sExportPanel->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    fileCatalog->setFilterPanel (filterPanel);
    fileCatalog->setExportPanel (exportPanel);
    fileCatalog->setImageAreaToolListener (tpc);
    fileCatalog->fileBrowser->setBatchPParamsChangeListener (tpc);

    // Wire "Set as album cover" from file browser to album browser
    fileCatalog->fileBrowser->setAlbumCoverSetter([this](const Glib::ustring& filePath) {
        int nodeId = albumBrowser_->getSelectedNodeId();
        if (nodeId >= 0) {
            albumBrowser_->setCoverForAlbum(nodeId, filePath);
        }
    });
    fileCatalog->fileBrowser->setAlbumModeChecker([this]() {
        return fileCatalog->isInAlbumMode();
    });

    //------------------

    rightNotebook->set_tab_pos (Gtk::POS_LEFT);

    Gtk::Label* devLab = Gtk::manage ( new Gtk::Label (M("MAIN_TAB_DEVELOP")) );
    devLab->set_name ("LabelRightNotebook");
    devLab->set_angle (90);
    Gtk::Label* inspectLab = nullptr;
    if (!options.inspectorWindow) {
        inspectLab = Gtk::manage ( new Gtk::Label (M("MAIN_TAB_INSPECT")) );
        inspectLab->set_name ("LabelRightNotebook");
        inspectLab->set_angle (90);
    }
    Gtk::Label* filtLab = Gtk::manage ( new Gtk::Label (M("MAIN_TAB_FILTER")) );
    filtLab->set_name ("LabelRightNotebook");
    filtLab->set_angle (90);
    //Gtk::Label* tagLab = Gtk::manage ( new Gtk::Label (M("MAIN_TAB_TAGGING")) );
    //tagLab->set_angle (90);
    Gtk::Label* exportLab = Gtk::manage ( new Gtk::Label (M("MAIN_TAB_EXPORT")) );
    exportLab->set_name ("LabelRightNotebook");
    exportLab->set_angle (90);

    tpcPaned = Gtk::manage ( new Gtk::Paned (Gtk::ORIENTATION_VERTICAL) );
    auto* tpcBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    tpcBox->pack_start(*tpc->modeButtonBar, Gtk::PACK_SHRINK, 0);
    tpcBox->pack_start(*tpc->modeStack);
    tpcPaned->pack1 (*tpcBox, false, true);
    tpcPaned->pack2 (*history, true, false);

    rightNotebook->append_page (*sFilterPanel, *filtLab);
    if (!options.inspectorWindow)
        rightNotebook->append_page (*inspectorPanel, *inspectLab);
    rightNotebook->append_page (*tpcPaned, *devLab);
    //rightNotebook->append_page (*taggingBox, *tagLab); commented out: currently the tab is empty ...
    rightNotebook->append_page (*sExportPanel, *exportLab);
    rightNotebook->set_name ("RightNotebook");

    rightBox->pack_start (*rightNotebook);

    // Wrap dirpaned + footer in a vertical box so the footer spans full width
    auto* dirpanedBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    dirpanedBox->pack_start(*dirpaned, Gtk::PACK_EXPAND_WIDGET);

    // Bottom footer bar matching the editor's EditorToolbarBottom layout
    auto* footerBar = Gtk::manage(new Gtk::Grid());
    footerBar->set_name("BrowserFooterBar");

    // Left sidebar toggle (matches editor's hidehp position)
    browserHideLp_ = Gtk::manage(new Gtk::ToggleButton());
    browserHideLp_->set_relief(Gtk::RELIEF_NONE);
    browserHideLp_->set_active(options.showHistory);
    iBrowserLpShow_ = Gtk::manage(new RTImage("panel-to-right", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    iBrowserLpHide_ = Gtk::manage(new RTImage("panel-to-left", Gtk::ICON_SIZE_LARGE_TOOLBAR));
    browserHideLp_->set_image(options.showHistory ? *iBrowserLpHide_ : *iBrowserLpShow_);
    browserHideLp_->set_tooltip_markup(M("MAIN_TOOLTIP_SHOWHIDELP1"));
    browserHideLp_->signal_toggled().connect([this]() {
        if (browserHideLp_->get_active()) {
            placespaned->show();
            browserHideLp_->set_image(*iBrowserLpHide_);
        } else {
            placespaned->hide();
            browserHideLp_->set_image(*iBrowserLpShow_);
        }
    });
    footerBar->attach(*browserHideLp_, 0, 0, 1, 1);

    // Spacer
    auto* footerSpacer = Gtk::manage(new Gtk::Label(""));
    footerSpacer->set_hexpand(true);
    footerBar->attach(*footerSpacer, 1, 0, 1, 1);

    auto* stbFooter = Gtk::manage(new MyScrolledToolbar());
    stbFooter->set_name("EditorToolbarBottom");
    stbFooter->set_vexpand(false);
    stbFooter->add(*footerBar);
    dirpanedBox->pack_start(*stbFooter, Gtk::PACK_SHRINK);

    pack1(*dirpanedBox, true, true);
    pack2(*rightBox, false, false);

    fileCatalog->setFileSelectionChangeListener (tpc);

    fileCatalog->setFileSelectionListener (this);

    idle_register.add(
        [this]() -> bool
        {
            init();
            return false;
        }
    );

    show_all ();
}

FilePanel::~FilePanel ()
{
    if (foregroundLoadGeneration_) {
        foregroundLoadGeneration_->fetch_add(1, std::memory_order_acq_rel);
    }

    cancelScheduledBackgroundResume();
    cancelUnstartedPendingLoads("filepanel-destroy");
    resumeBackgroundWorkNow();

    // Signal any in-flight preload threads to discard their results, then
    // drop our ref. The threads hold their own shared_ptr; the manager is
    // destroyed (and remaining cached images released) when the last one
    // completes.
    if (preload_) {
        preload_->stopped = true;
        preload_.reset();
    }
    idle_register.destroy();

    rightNotebookSwitchConn.disconnect();

    if (inspectorPanel) {
        delete inspectorPanel;
    }

    delete tpc;
}

void FilePanel::on_realize ()
{
    Gtk::Paned::on_realize ();
    tpc->closeAllTools();
}


void FilePanel::setContentOpacity (double opacity)
{
    // Fade only the content area (thumbnails + right panel), keep left sidebar static
    if (ribbonPane) ribbonPane->set_opacity(opacity);
    if (rightBox) rightBox->set_opacity(opacity);
}

void FilePanel::setAspect ()
{
    int winW, winH;
    parent->get_size(winW, winH);
    const auto& options = App::get().options();
    placespaned->set_position(std::max(options.dirBrowserHeight, 300));
    dirpaned->set_position(options.dirBrowserWidth);
    tpcPaned->set_position(options.browserToolPanelHeight);
    set_position(winW - options.browserToolPanelWidth);

    if (!options.browserDirPanelOpened) {
        fileCatalog->toggleLeftPanel();
    }

    if (!options.browserToolPanelOpened) {
        fileCatalog->toggleRightPanel();
    }
}

void FilePanel::init ()
{

    dirBrowser->fillDirTree ();
    placesBrowser->refreshPlacesList ();

    if (!App::get().argv1().empty() && Glib::file_test (App::get().argv1(), Glib::FILE_TEST_EXISTS)) {
        Glib::ustring d(App::get().argv1());
        if (!Glib::file_test(d, Glib::FILE_TEST_IS_DIR)) {
            d = Glib::path_get_dirname(d);
        }
        dirBrowser->open(d);
    } else {
        const auto& options = App::get().options();
        if (options.startupDir == STARTUPDIR_HOME) {
            dirBrowser->open (PlacesBrowser::userPicturesDir ());
        } else if (options.startupDir == STARTUPDIR_CURRENT) {
            dirBrowser->open (App::get().argv0());
        } else if (options.startupDir == STARTUPDIR_CUSTOM || options.startupDir == STARTUPDIR_LAST) {
            if (options.startupPath.length() && Glib::file_test(options.startupPath, Glib::FILE_TEST_EXISTS) && Glib::file_test(options.startupPath, Glib::FILE_TEST_IS_DIR)) {
                dirBrowser->open (options.startupPath);
            } else {
                // Fallback option if the path is empty or the folder doesn't exist
                dirBrowser->open (PlacesBrowser::userPicturesDir ());
            }
        }
    }
}

void FilePanel::on_NB_switch_page(Gtk::Widget* page, guint page_num)
{
    if (page_num == 1) {
        // switching the inspector "on"
        fileCatalog->enableInspector();
    } else {
        // switching the inspector "off"
        fileCatalog->disableInspector();
    }
}

bool FilePanel::fileSelected (Thumbnail* thm)
{
    return fileSelected(thm, NAV_NONE);
}

bool FilePanel::fileSelected (Thumbnail* thm, eRTNav preloadDirectionHint)
{
    using clk = std::chrono::steady_clock;
    const bool logSelection = g_fileSelLogEnabled();
    const auto selectionAt = clk::now();
    auto t0 = logSelection ? selectionAt : clk::time_point{};
    auto ms = [&](clk::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t - t0).count();
    };

    static auto lastReturn = clk::now();
    if (logSelection) {
        auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - lastReturn).count();
        FILESEL_LOG("---- fileSel ENTER (gap since last return: %lldms) ----\n", (long long)gapMs);
    }

    // Re-entrance guard. On Windows, some GTK calls internally pump the
    // message pump, which can dispatch a queued WM_LBUTTONDOWN and re-enter
    // fileSelected() while a prior call is still mid-teardown. Without this
    // guard, concurrent close()+open() cycles crash in OMP workers.
    static thread_local int s_reentryDepth = 0;
    struct ReentryGuard {
        int& d;
        ReentryGuard(int& x) : d(x) { ++d; }
        ~ReentryGuard() { --d; }
    } guard(s_reentryDepth);
    if (s_reentryDepth > 1) {
        FILESEL_LOG("[fileSel] RE-ENTRY DETECTED (depth=%d) - dropping\n",
                     s_reentryDepth);
        // Caller (_openImage) decreaseRefs on false return.
        return false;
    }

    if (!parent) {
        return false;
    }

    const auto& opts = App::get().options();
    const Glib::ustring selectedFileName = thm->getFileName();
    const std::string selectedFileNameRaw(selectedFileName);
    const bool selectedIsRaw = thm->getType() == FT_Raw;
    const bool directionalSelection =
        preloadDirectionHint == NAV_NEXT || preloadDirectionHint == NAV_PREVIOUS;

    if (directionalSelection) {
        recentDirectionalSelectionGapMs_ = 0;
        if (lastDirectionalSelectionAt_ != clk::time_point{}) {
            const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                selectionAt - lastDirectionalSelectionAt_).count();
            if (gapMs > 0 && gapMs <= 5000) {
                recentDirectionalSelectionGapMs_ = static_cast<int>(gapMs);
            }
        }
        const bool continuingDirectionalRun =
            recentDirectionalSelectionDirection_ == preloadDirectionHint
            && recentDirectionalSelectionGapMs_ > 0
            && recentDirectionalSelectionGapMs_ <= 5000;

        if (continuingDirectionalRun
            && recentDirectionalSelectionGapMs_ > 0
            && recentDirectionalSelectionGapMs_ <= PreloadManager::kDirectionalScrubDecodeDebounceCadenceMs) {
            ++recentDirectionalSelectionRunLength_;
        } else {
            recentDirectionalSelectionRunLength_ = 1;
        }
        if (selectedIsRaw) {
            recentDirectionalRawSelectionRunLength_ =
                continuingDirectionalRun && recentDirectionalSelectionWasRaw_
                    ? recentDirectionalRawSelectionRunLength_ + 1
                    : 1;
        } else {
            recentDirectionalRawSelectionRunLength_ = 0;
        }
        recentDirectionalSelectionWasRaw_ = selectedIsRaw;
        recentDirectionalSelectionDirection_ = preloadDirectionHint;
        lastDirectionalSelectionAt_ = selectionAt;
    } else {
        recentDirectionalSelectionGapMs_ = 0;
        recentDirectionalSelectionRunLength_ = 0;
        recentDirectionalSelectionWasRaw_ = false;
        recentDirectionalRawSelectionRunLength_ = 0;
    }
    // Mark foreground pressure before any same-file/tab checks. Those checks can
    // still do small amounts of GTK/bookkeeping work, and a preloader should not
    // grab the serialized RAW gate while a user-visible open is entering.
    g_rawLoadGate.noteForegroundIntent(selectedFileNameRaw);

    // Check if it's already open BEFORE loading the file
    if (opts.tabbedUI && parent->selectEditorPanel(selectedFileName)) {
        thm->decreaseRef();
        return true;
    }

    if (!opts.tabbedUI
        && parent->epanel
        && parent->epanel->getFileName() == selectedFileName) {
        FILESEL_LOG("[fileSel] already open in single editor, switching without reload file=%s\n",
            selectedFileNameRaw.c_str());
        parent->SetEditorCurrent();
        parent->set_title_decorated(selectedFileName);
        thm->decreaseRef();
        return true;
    }

    bool recycleSupersededPendingLoad = false;
    if (!opts.tabbedUI && selectedIsRaw) {
        pendingLoadMutex.lock();
        for (const auto* p : pendingLoads) {
            if (p->thm == thm && p->superseded && !p->complete) {
                recycleSupersededPendingLoad = true;
                break;
            }
        }
        pendingLoadMutex.unlock();
    }
    if (recycleSupersededPendingLoad && preload_) {
        preload_->markForegroundRecycleTarget(selectedFileNameRaw);
        FILESEL_LOG("[fileSel] +%lldms marked recycle target file=%s\n",
            (long long)ms(clk::now()),
            selectedFileNameRaw.c_str());
    }

    // Check if the image is already being opened and set the image loading status if it is not
    bool loading = thm->imageLoad( true );
    if( !loading ) {
        bool pendingSameLoad = false;

        if (!opts.tabbedUI) {
            pendingLoadMutex.lock();
            for (const auto* p : pendingLoads) {
                if (p->thm == thm && !p->superseded) {
                    pendingSameLoad = true;
                    break;
                }
            }
            pendingLoadMutex.unlock();
        }

        if (pendingSameLoad) {
            FILESEL_LOG("[fileSel] already loading in single editor, switching without duplicate load file=%s\n",
                selectedFileNameRaw.c_str());
            parent->SetEditorCurrent();
            parent->set_title_decorated(selectedFileName);
            thm->decreaseRef();
            return true;
        }

        return false;
    }

    const bool skipStaleForegroundLoad = !opts.tabbedUI;
    const unsigned loadGeneration = skipStaleForegroundLoad
        ? foregroundLoadGeneration_->fetch_add(1, std::memory_order_acq_rel) + 1
        : foregroundLoadGeneration_->load(std::memory_order_acquire);

    rtengine::InitialImage* cachedImg = nullptr;
    if (preload_) {
        preload_->prioritizeForeground(selectedFileNameRaw);
        cachedImg = preload_->take(selectedFileNameRaw);
    }

    auto supersedePendingSingleEditorLoads = [&]() {
        if (!skipStaleForegroundLoad) {
            return;
        }

        std::vector<Thumbnail*> supersededThumbnails;
        std::vector<pendingLoad*> canceledBeforeStart;
        size_t demotedForegroundLoads = 0;
        pendingLoadMutex.lock();
        for (auto it = pendingLoads.begin(); it != pendingLoads.end(); ) {
            auto* p = *it;
            if (!p->superseded) {
                p->superseded = true;
                if (p->quickPreviewAllowed) {
                    p->quickPreviewAllowed->store(false, std::memory_order_release);
                }
                if (!p->loadStarted && p->delayedStartConn.connected()) {
                    p->delayedStartConn.disconnect();
                    canceledBeforeStart.push_back(p);
                    it = pendingLoads.erase(it);
                    continue;
                }
                if (p->thm) {
                    supersededThumbnails.push_back(p->thm);
                }
                if (p->loadStarted && demoteForegroundLoadPriority(p->foregroundPriorityId)) {
                    ++demotedForegroundLoads;
                }
            }
            ++it;
        }
        pendingLoadMutex.unlock();

        for (auto* pendingThm : supersededThumbnails) {
            pendingThm->imageLoad(false);
        }

        for (auto* canceled : canceledBeforeStart) {
            if (canceled->thm) {
                canceled->thm->imageLoad(false);
                canceled->thm->decreaseRef();
            }
            delete canceled->pc;
            delete canceled;
        }

        if (!supersededThumbnails.empty() || !canceledBeforeStart.empty()) {
            FILESEL_LOG("[fileSel] +%lldms superseded pending loads=%zu clearedLoading=%zu demoted=%zu file=%s\n",
                (long long)ms(clk::now()),
                supersededThumbnails.size() + canceledBeforeStart.size(),
                supersededThumbnails.size() + canceledBeforeStart.size(),
                demotedForegroundLoads,
                selectedFileNameRaw.c_str());
        }
    };

    if (cachedImg) {
        supersedePendingSingleEditorLoads();

        // Pause queued background UI work around the editor handoff so preview
        // and thumbnail jobs do not compete while a decoded adjacent image is attached.
        pauseBackgroundWorkForForeground();

        EditorPanel* epanel = nullptr;
        if (opts.tabbedUI) {
#ifdef _WIN32
            int winGdiHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            if (winGdiHandles == 0 || winGdiHandles > 6500)
#endif
            {
                epanel = Gtk::manage(new EditorPanel());
                parent->addEditorPanel(epanel, selectedFileName);
                epanel->setAspect();
            }
        } else {
            parent->SetEditorCurrent();
        }

        FILESEL_LOG("[fileSel] +%lldms SetEditorCurrent cached\n", (long long)ms(clk::now()));

        if (preload_ && PreloadManager::kCachedOpenPreloadSettleMs > 0) {
            const long long preloadSettleWaitMs = preload_->waitForDifferentLoadToSettle(
                selectedFileNameRaw,
                std::chrono::milliseconds(PreloadManager::kCachedOpenPreloadSettleMs));
            if (preloadSettleWaitMs > 0) {
                FILESEL_LOG("[fileSel] +%lldms waited active preload before cached open wait=%lldms file=%s\n",
                    (long long)ms(clk::now()),
                    preloadSettleWaitMs,
                    selectedFileNameRaw.c_str());
            }
        }

        bool opened = false;
        bool releaseThumbRef = false;
        const auto cachedOpenStart = clk::now();
        if (opts.tabbedUI) {
            if (epanel) {
                epanel->open(thm, cachedImg);
                if (!(opts.multiDisplayMode > 0)) {
                    parent->set_title_decorated(selectedFileName);
                }
                opened = true;
            } else {
                releaseInitialImageInBackground(
                    cachedImg,
                    "cached-no-editor-panel",
                    selectedFileName);
                releaseThumbRef = true;
            }
        } else {
            parent->epanel->open(thm, cachedImg);
            parent->set_title_decorated(selectedFileName);
            opened = true;
        }

        parent->setProgress(0.);
        parent->setProgressStr("");
        if (opened) {
            FILESEL_LOG("[fileSel] cached opened after %lldms openDuration=%lldms file=%s\n",
                fileSelDurationMs(t0, clk::now()),
                fileSelDurationMs(cachedOpenStart, clk::now()),
                selectedFileNameRaw.c_str());
            scheduleAdjacentPreload(selectedFileName, preloadDirectionHint, true);
        }

        bool pendingLoadsRemain = false;
        pendingLoadMutex.lock();
        pendingLoadsRemain = !pendingLoads.empty();
        pendingLoadMutex.unlock();
        if (!pendingLoadsRemain) {
            resumeBackgroundWorkAfterForeground();
        }
        thm->imageLoad(false);
        if (releaseThumbRef) {
            thm->decreaseRef();
        }

        return true;
    }

    auto foregroundRequest = std::make_shared<ForegroundLoadRequest>();
    foregroundRequest->generation = foregroundLoadGeneration_;
    foregroundRequest->requestGeneration = loadGeneration;
    foregroundRequest->priorityId = nextForegroundLoadPriorityId();
    foregroundRequest->skipIfStale = skipStaleForegroundLoad;
    foregroundRequest->staleSkipped = std::make_shared<std::atomic<bool>>(false);
    const bool rapidDirectionalSelection = directionalSelection
        && recentDirectionalSelectionGapMs_ > 0
        && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDecodeDebounceCadenceMs;
    const bool mediumDirectionalScrub = directionalSelection
        && recentDirectionalSelectionRunLength_ >= PreloadManager::kDirectionalScrubDecodeDebounceRunLength
        && recentDirectionalSelectionGapMs_ > PreloadManager::kRapidDecodeDebounceCadenceMs
        && recentDirectionalSelectionGapMs_ <= PreloadManager::kDirectionalScrubDecodeDebounceCadenceMs;
    if (selectedIsRaw && (rapidDirectionalSelection || mediumDirectionalScrub)) {
        foregroundRequest->decodeStartDelayMs = mediumDirectionalScrub
            ? std::max(
                PreloadManager::kDirectionalScrubDecodeDebounceMinMs,
                std::min(
                    PreloadManager::kDirectionalScrubDecodeDebounceMaxMs,
                    recentDirectionalSelectionGapMs_ + PreloadManager::kDirectionalScrubDecodeDebounceExtraMs))
            : std::max(
                PreloadManager::kRapidDecodeDebounceMinMs,
                std::min(
                    PreloadManager::kRapidDecodeDebounceMaxMs,
                    recentDirectionalSelectionGapMs_ + PreloadManager::kRapidDecodeDebounceExtraMs));
        FILESEL_LOG("[fileSel] +%lldms rapid decode debounce mode=%s delay=%dms cadence=%dms run=%u file=%s\n",
            (long long)ms(clk::now()),
            mediumDirectionalScrub ? "scrub" : "rapid",
            foregroundRequest->decodeStartDelayMs,
            recentDirectionalSelectionGapMs_,
            recentDirectionalSelectionRunLength_,
            selectedFileNameRaw.c_str());
    } else if (!selectedIsRaw
        && rapidDirectionalSelection
        && recentDirectionalSelectionRunLength_ >= PreloadManager::kRapidNonRawDecodeDebounceRunLength) {
        foregroundRequest->decodeStartDelayMs = std::max(
            PreloadManager::kRapidNonRawDecodeDebounceMinMs,
            std::min(
                PreloadManager::kRapidNonRawDecodeDebounceMaxMs,
                recentDirectionalSelectionGapMs_ + PreloadManager::kRapidNonRawDecodeDebounceExtraMs));
        FILESEL_LOG("[fileSel] +%lldms rapid decode debounce mode=rapid-nonraw delay=%dms cadence=%dms run=%u file=%s\n",
            (long long)ms(clk::now()),
            foregroundRequest->decodeStartDelayMs,
            recentDirectionalSelectionGapMs_,
            recentDirectionalSelectionRunLength_,
            selectedFileNameRaw.c_str());
    }

    supersedePendingSingleEditorLoads();

    pendingLoadMutex.lock();
    // In single-editor mode only one image is visible at a time: if the user
    // clicks a new image while earlier decodes are still running, mark those
    // older loads as superseded so imageLoaded() discards their results
    // instead of rapid-firing open()+close() calls on the editor panel
    // (which otherwise races with OMP workers inside rgbProc).
    pendingLoad *pl = new pendingLoad();
    pl->complete = false;
    pl->superseded = false;
    pl->staleSkipped = foregroundRequest->staleSkipped;
    pl->quickPreviewAllowed = std::make_shared<std::atomic<bool>>(true);
    pl->pc = nullptr;
    pl->thm = thm;
    pl->epanel = nullptr;
    pl->preloadDirectionHint = preloadDirectionHint;
    pl->startedAt = t0;
    pl->loadStarted = false;
    pl->foregroundPriorityId = foregroundRequest->priorityId;
    pendingLoads.push_back(pl);
    FILESEL_LOG("[fileSel] +%lldms enqueued (pending=%zu)\n",
           (long long)ms(clk::now()), pendingLoads.size());
    pendingLoadMutex.unlock();

    // Pause preview and thumbnail loading to free IO/CPU for the full image load.
    // Resumes in imageLoaded() after the editor opens.
    pauseBackgroundWorkForForeground();

    // Don't signalStop the old processor here; doing so caused OMP workers
    // in Color::RGB2Lab to race with the new image's processor spinning up,
    // crashing in LUT::operator[] on freed data. close() in open() handles
    // the old processor safely on a background cleanup thread after the
    // RAW decode completes.
    // Switch to editor view immediately so the user sees the transition
    // while the image loads in the background.
    if (opts.tabbedUI) {
#ifdef _WIN32
        int winGdiHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        if (winGdiHandles == 0 || winGdiHandles > 6500)
#endif
        {
            EditorPanel* ep = Gtk::manage(new EditorPanel());
            parent->addEditorPanel(ep, selectedFileName);
            ep->setAspect();
            pl->epanel = ep;
        }
    } else {
        parent->SetEditorCurrent();
    }
    FILESEL_LOG("[fileSel] +%lldms SetEditorCurrent\n", (long long)ms(clk::now()));

    // Instant thumbnail preview: use the clicked image's cached filmstrip
    // Pixbuf before kicking the full RAW decode. Avoid synchronous thumbnail
    // rendering here; first paint matters more than manufacturing a fallback
    // preview on the GTK thread.
    EditorPanel* quickPreviewTarget = opts.tabbedUI ? pl->epanel : parent->epanel;
    if (quickPreviewTarget) {
        Glib::RefPtr<Gdk::Pixbuf> quickPb;
        double displayScale = 1.0;

        double cachedScale = 1.0;
        bool cachedPixbufBusy = false;
        quickPb = thm->tryGetCachedPixbuf(cachedScale, &cachedPixbufBusy);
        FILESEL_LOG("[fileSel] +%lldms getCachedPixbuf %s\n",
               (long long)ms(clk::now()), quickPb ? "HIT" : (cachedPixbufBusy ? "BUSY" : "MISS"));

        if (quickPb) {
            displayScale = cachedPreviewScaleForEditor(thm, quickPb, cachedScale > 0.0 ? cachedScale : displayScale);
            quickPreviewTarget->setQuickPreview(quickPb, displayScale, selectedFileName);
            FILESEL_LOG("[fileSel] +%lldms setQuickPreview done\n",
                   (long long)ms(clk::now()));
            // NOTE: do NOT call gdk_window_process_all_updates() here. On
            // Windows it pumps the message pump, which dispatches queued
            // WM_LBUTTONDOWN events and re-enters fileSelected() recursively.
            // That racing caused crashes. queue_draw() above is sufficient:
            // GTK paints the override on the next main-loop iteration (a few
            // ms later), still feels instant to the user.
        } else if (!cachedPixbufBusy && !opts.tabbedUI) {
            Thumbnail* previewThm = thm;
            previewThm->increaseRef();
            auto previewAllowed = pl->quickPreviewAllowed;
            auto previewRequest = foregroundRequest;
            RTWindow* previewParent = parent;
            const Glib::ustring previewFileName = selectedFileName;
            std::thread([previewThm, previewAllowed, previewRequest, previewParent, previewFileName]() {
                lowerBackgroundPreloadThreadPriority();
                const auto previewStart = std::chrono::steady_clock::now();
                double previewScale = 1.0;
                auto pixbuf = previewThm->tryLoadCachedPreviewPixbuf(
                    std::max(1, App::get().options().maxThumbnailHeight),
                    previewScale);
                previewScale = cachedPreviewScaleForEditor(previewThm, pixbuf, previewScale);
                previewThm->decreaseRef();

                const bool allowed = previewAllowed
                    && previewAllowed->load(std::memory_order_acquire)
                    && (!previewRequest || !previewRequest->isStale());

                if (!pixbuf || !allowed) {
                    FILESEL_LOG("[fileSel] async cached preview %s duration=%lldms file=%s\n",
                        pixbuf ? "canceled" : "miss",
                        fileSelDurationMs(previewStart, std::chrono::steady_clock::now()),
                        previewFileName.c_str());
                    return;
                }

                Glib::signal_idle().connect_once(
                    [pixbuf, previewScale, previewAllowed, previewRequest, previewParent, previewFileName, previewStart]() {
                        if (!previewAllowed
                            || !previewAllowed->load(std::memory_order_acquire)
                            || (previewRequest && previewRequest->isStale())
                            || !previewParent
                            || !previewParent->epanel) {
                            FILESEL_LOG("[fileSel] async cached preview canceled duration=%lldms file=%s\n",
                                fileSelDurationMs(previewStart, std::chrono::steady_clock::now()),
                                previewFileName.c_str());
                            return;
                        }

                        previewParent->epanel->setQuickPreview(pixbuf, previewScale, previewFileName);
                        FILESEL_LOG("[fileSel] async cached preview done duration=%lldms file=%s\n",
                            fileSelDurationMs(previewStart, std::chrono::steady_clock::now()),
                            previewFileName.c_str());
                    },
                    Glib::PRIORITY_HIGH_IDLE);
            }).detach();
        }
    }

    ProgressConnector<rtengine::InitialImage*> *ld = new ProgressConnector<rtengine::InitialImage*>();
    pl->pc = ld;

    auto startForegroundLoad = [this, selectedFileName, selectedFileNameRaw, selectedIsRaw, foregroundRequest, thm, ld, t0](const char* mode) {
        ld->startFunc(
            sigc::bind(
                sigc::ptr_fun(&loadInitialImageSerialized),
                selectedFileName,
                selectedFileNameRaw,
                selectedIsRaw,
                &error,
                parent->getProgressListener(),
                preload_,
                foregroundRequest),
            sigc::bind(sigc::mem_fun(*this, &FilePanel::imageLoaded), thm, ld));
        FILESEL_LOG("[fileSel] +%lldms startFunc kicked mode=%s\n",
            fileSelDurationMs(t0, std::chrono::steady_clock::now()),
            mode ? mode : "");
    };

    const bool deferForegroundWorkerStart = skipStaleForegroundLoad
        && foregroundRequest->decodeStartDelayMs > 0;
    if (deferForegroundWorkerStart) {
        const int delayedStartMs = foregroundRequest->decodeStartDelayMs;
        const bool wakeWhenPreloadReady = selectedIsRaw && preload_ != nullptr;
        foregroundRequest->decodeStartDelayMs = 0;
        pl->delayedStartConn = Glib::signal_timeout().connect(
            [this, pl, startForegroundLoad, t0, delayedStartMs, selectedFileNameRaw, wakeWhenPreloadReady]() -> bool {
                const bool preloadReady = wakeWhenPreloadReady
                    && preload_
                    && preload_->hasCachedOrLoading(selectedFileNameRaw);
                const bool delayElapsed =
                    fileSelDurationMs(t0, std::chrono::steady_clock::now()) >= delayedStartMs;

                if (!preloadReady && !delayElapsed) {
                    return true;
                }

                bool shouldStart = false;
                pendingLoadMutex.lock();
                if (pl && !pl->superseded) {
                    pl->loadStarted = true;
                    shouldStart = true;
                }
                pendingLoadMutex.unlock();

                if (shouldStart) {
                    FILESEL_LOG("[fileSel] +%lldms delayed startFunc firing delay=%dms reason=%s file=%s\n",
                        fileSelDurationMs(t0, std::chrono::steady_clock::now()),
                        delayedStartMs,
                        preloadReady ? "preload-ready" : "timer",
                        selectedFileNameRaw.c_str());
                    startForegroundLoad(preloadReady ? "preload-ready" : "delayed");
                }
                return false;
            },
            wakeWhenPreloadReady ? PreloadManager::kDelayedRawPreloadReadyPollMs : delayedStartMs,
            Glib::PRIORITY_HIGH_IDLE);
        FILESEL_LOG("[fileSel] +%lldms startFunc delayed delay=%dms\n",
            (long long)ms(clk::now()),
            delayedStartMs);
    } else {
        pendingLoadMutex.lock();
        pl->loadStarted = true;
        pendingLoadMutex.unlock();
        startForegroundLoad("immediate");
    }

    if (preload_) {
        if (!preload_->isLoading(selectedFileNameRaw)) {
            scheduleAdjacentPreload(selectedFileName, preloadDirectionHint, false);
        }
    }
    FILESEL_LOG("[fileSel] +%lldms RETURN\n", (long long)ms(clk::now()));
    if (logSelection) {
        lastReturn = clk::now();
    }
    return true;
}

bool FilePanel::addBatchQueueJobs(const std::vector<BatchQueueEntry*>& entries)
{
    if (parent) {
        parent->addBatchQueueJobs (entries);
    }

    return true;
}

bool FilePanel::imageLoaded( Thumbnail* thm, ProgressConnector<rtengine::InitialImage*> *pc )
{
    const auto callbackAt = std::chrono::steady_clock::now();
    const auto& options = App::get().options();
    const bool preserveOpenOrder = options.tabbedUI;
    FILESEL_LOG("[imageLoaded] RAW decode finished\n");
    std::vector<pendingLoad*> readyLoads;
    pendingLoadMutex.lock();
    FILESEL_LOG("[imageLoaded] locked, pending=%zu\n", pendingLoads.size());
    readyLoads.reserve(pendingLoads.size());

    // find our place in the array and mark the entry as complete
    for (unsigned int i = 0; i < pendingLoads.size(); i++) {
        if (pendingLoads[i]->thm == thm) {
            pendingLoads[i]->pc = pc;
            pendingLoads[i]->complete = true;
            if (pendingLoads[i]->startedAt.time_since_epoch().count() != 0) {
                FILESEL_LOG("[imageLoaded] callback after %lldms file=%s\n",
                    fileSelDurationMs(pendingLoads[i]->startedAt, callbackAt),
                    thm ? thm->getFileName().c_str() : "");
            }
            break;
        }
    }

    if (preserveOpenOrder) {
        // Preserve tab-open ordering by draining only contiguous completed
        // loads from the front.
        while (pendingLoads.size() > 0 && pendingLoads.front()->complete) {
            readyLoads.push_back(pendingLoads.front());
            pendingLoads.pop_front();
        }
    } else {
        // In single-editor navigation, older entries are superseded by newer
        // clicks. Do not make the newest completed image wait behind a stale
        // in-flight request just because that request is earlier in the queue.
        for (auto it = pendingLoads.begin(); it != pendingLoads.end(); ) {
            if ((*it)->complete) {
                readyLoads.push_back(*it);
                it = pendingLoads.erase(it);
            } else {
                ++it;
            }
        }
    }
    pendingLoadMutex.unlock();

    bool preloadOpenedImage = false;
    eRTNav preloadDirectionHint = NAV_NONE;
    Glib::ustring preloadFname;
    bool resetProgress = false;

    for (pendingLoad* pl : readyLoads) {
        Thumbnail* const loadThm = pl->thm;
        rtengine::InitialImage* const loadedImage = pl->pc ? pl->pc->returnValue() : nullptr;
        const bool staleSkipped = pl->staleSkipped
            && pl->staleSkipped->load(std::memory_order_acquire);
        bool releaseThumbRef = false;

        if (pl->quickPreviewAllowed) {
            pl->quickPreviewAllowed->store(false, std::memory_order_release);
        }

        FILESEL_LOG("[imageLoaded] processing front: superseded=%d, staleSkipped=%d, hasReturn=%d\n",
               (int)pl->superseded, (int)staleSkipped, (int)(loadedImage != nullptr));

        if (pl->superseded || staleSkipped) {
            // User clicked a newer image before this decode completed:
            // discard the loaded InitialImage to avoid rapid-fire opens
            // that race with OMP workers in the previous image's ipc.
            FILESEL_LOG("[imageLoaded] SUPERSEDED - discarding result\n");
            if (loadedImage) {
                const Glib::ustring loadedFname = loadThm ? loadThm->getFileName() : Glib::ustring();
                const std::string loadedFnameRaw(loadedFname);
                const bool loadedIsRaw = loadThm && loadThm->getType() == FT_Raw;
                const size_t cachedBytes = PreloadManager::estimatedLoadedInitialImageBytes(loadedImage, loadedIsRaw);
                std::vector<rtengine::InitialImage*> evictedImages;
                const bool cachedForBacktrack = preload_
                    && preload_->cacheDecodedIfHotWanted(loadedFnameRaw, loadedImage, cachedBytes, &evictedImages);

                if (cachedForBacktrack) {
                    releasePreloadImagesInBackground(std::move(evictedImages), "foreground-recycle", loadedFname);
                    FILESEL_LOG("[preload] recycled superseded foreground bytes=%zu file=%s\n",
                        cachedBytes,
                        loadedFnameRaw.c_str());
                } else {
                    releaseInitialImageInBackground(
                        loadedImage,
                        pl->superseded ? "superseded" : "stale",
                        loadedFname);
                }
            }
            delete pl->pc;
            delete pl;
            if (loadThm) {
                loadThm->imageLoad(false);
            }
            continue;
        }

        if (loadedImage) {
            bool opened = false;
            const auto openStart = std::chrono::steady_clock::now();

            if (options.tabbedUI) {
                // Editor panel was pre-created in fileSelected() for
                // immediate view switch; just open the image in it now.
                if (pl->epanel) {
                    pl->epanel->open(loadThm, loadedImage);

                    if (!(options.multiDisplayMode > 0)) {
                        parent->set_title_decorated(loadThm->getFileName());
                    }
                    opened = true;
                } else {
                    // GDI handle limit was hit: panel wasn't created
                    Glib::ustring msg_ = Glib::ustring("<b>") + M("MAIN_MSG_CANNOTLOAD") + " \"" + escapeHtmlChars(loadThm->getFileName()) + "\" .\n" + M("MAIN_MSG_TOOMANYOPENEDITORS") + "</b>";
                    Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
                    msgd.run ();
                    releaseInitialImageInBackground(
                        loadedImage,
                        "no-editor-panel",
                        loadThm ? loadThm->getFileName() : Glib::ustring());
                    releaseThumbRef = true;
                }
            } else {
                // View was already switched in fileSelected(); just open.
                FILESEL_LOG("[imageLoaded] calling epanel->open\n");
                parent->epanel->open(loadThm, loadedImage);
                FILESEL_LOG("[imageLoaded] epanel->open returned\n");
                parent->set_title_decorated(loadThm->getFileName());
                opened = true;
            }

            if (opened) {
                if (preload_ && loadThm) {
                    preload_->clearForegroundRecycleTarget(std::string(loadThm->getFileName()));
                }
                if (pl->startedAt.time_since_epoch().count() != 0) {
                    FILESEL_LOG("[imageLoaded] opened after %lldms openDuration=%lldms file=%s\n",
                        fileSelDurationMs(pl->startedAt, std::chrono::steady_clock::now()),
                        fileSelDurationMs(openStart, std::chrono::steady_clock::now()),
                        loadThm ? loadThm->getFileName().c_str() : "");
                }
                preloadOpenedImage = true;
                preloadDirectionHint = pl->preloadDirectionHint;
                preloadFname = loadThm->getFileName();
            }
        } else {
            Glib::ustring msg_ = Glib::ustring("<b>") + M("MAIN_MSG_CANNOTLOAD") + " \"" + escapeHtmlChars(loadThm->getFileName()) + "\" .\n</b>";
            Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.run ();
            releaseThumbRef = true;
        }
        delete pl->pc;
        resetProgress = true;

        delete pl;
        if (loadThm) {
            loadThm->imageLoad(false);
            if (releaseThumbRef) {
                loadThm->decreaseRef();
            }
        }
    }

    if (resetProgress) {
        parent->setProgress(0.);
        parent->setProgressStr("");
    }

    pendingLoadMutex.lock();
    const bool pendingLoadsRemain = !pendingLoads.empty();
    pendingLoadMutex.unlock();

    // Preload adjacent images for faster filmstrip navigation. Queue this
    // after the editor handoff so the newly opened image gets first paint
    // before adjacent-cache bookkeeping runs on the GTK thread.
    if (preloadOpenedImage && !preloadFname.empty()) {
        scheduleAdjacentPreload(preloadFname, preloadDirectionHint, true);
    }

    // Resume background preview and thumbnail loading after the adjacent
    // preload idle has first chance at the post-open quiet window.
    if (!pendingLoadsRemain) {
        resumeBackgroundWorkAfterForeground();
    }

    return false; // MUST return false from idle function
}

void FilePanel::preloadAdjacent(const Glib::ustring& fname, eRTNav preferredDirection, bool refreshThumbnails)
{
    if (!fileCatalog || !fileCatalog->fileBrowser) return;
    if (!preload_ || preload_->stopped) return;

    const auto now = std::chrono::steady_clock::now();
    if (preferredDirection == NAV_NEXT || preferredDirection == NAV_PREVIOUS) {
        recentDirectionalPreloadFname_ = fname;
        recentDirectionalPreloadDirection_ = preferredDirection;
        recentDirectionalPreloadUntil_ = now + std::chrono::milliseconds(PreloadManager::kDirectionalHintKeepAliveMs);
    } else if (!recentDirectionalPreloadFname_.empty()
        && now < recentDirectionalPreloadUntil_
        && (recentDirectionalPreloadFname_ == fname
            || (recentDirectionalSelectionGapMs_ > 0
                && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDirectionalCadenceMs))) {
        preferredDirection = recentDirectionalPreloadDirection_;
        FILESEL_LOG("[preload] preserving directional hint at run dir=%d anchor=%s\n",
            static_cast<int>(preferredDirection),
            fname.c_str());
    }

    ++adjacentPreloadGeneration_;

    const bool directionalPreload = preferredDirection == NAV_NEXT || preferredDirection == NAV_PREVIOUS;
    const bool rapidDirectionalPreload = directionalPreload
        && recentDirectionalSelectionGapMs_ > 0
        && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDirectionalCadenceMs;
    const bool rawStridePreload = directionalPreload
        && recentDirectionalRawSelectionRunLength_ >= 2;
    const bool rawStrideCanPreloadThroughEditor = rawStridePreload
        && recentDirectionalSelectionGapMs_ > PreloadManager::kRapidDirectionalCadenceMs;
    const int quickPreviewWarmRadius = (!refreshThumbnails && rawStridePreload)
        ? 0
        : PreloadManager::kQuickPreviewWarmRadius;

    // Entries come ordered by likely navigation direction; preserve that as
    // the full RAW preload priority list. When thumbnail refresh is requested,
    // fold it into the same adjacent scan to avoid a second GTK-thread pass.
    auto entries = fileCatalog->fileBrowser->getAdjacentEntriesAndRefresh(
        fname,
        PreloadManager::kRadius,
        refreshThumbnails ? PreloadManager::kThumbnailRefreshRadius : 0,
        quickPreviewWarmRadius,
        preferredDirection);

    std::vector<std::string> newWanted;
    newWanted.reserve(entries.size());
    std::unordered_set<std::string> newWantedSet;
    newWantedSet.reserve(entries.size());
    std::vector<std::string> newHotWanted;
    newHotWanted.reserve(std::min(entries.size(), PreloadManager::kMaxEntries));
    std::unordered_set<std::string> newHotWantedSet;
    newHotWantedSet.reserve(std::min(entries.size(), PreloadManager::kMaxEntries));
    std::vector<FileBrowser::AdjacentEntry> newHotWantedEntries;
    newHotWantedEntries.reserve(std::min(entries.size(), PreloadManager::kMaxEntries));
    for (const auto& e : entries) {
        newWantedSet.insert(e.fnameRaw);
        newWanted.emplace_back(e.fnameRaw);
    }

    auto addHotEntry = [&](const FileBrowser::AdjacentEntry& e) {
        if (!newHotWantedSet.insert(e.fnameRaw).second) {
            return false;
        }

        newHotWanted.emplace_back(e.fnameRaw);
        newHotWantedEntries.push_back(e);
        return true;
    };

    auto addHotWanted = [&](bool rawOnly, bool nonRawOnly, int sideFilter, size_t targetCount, size_t maxNewEntries = PreloadManager::kMaxEntries, size_t skipMatches = 0) {
        size_t matched = 0;
        size_t added = 0;
        for (const auto& e : entries) {
            if (newHotWantedSet.size() >= targetCount || added >= maxNewEntries) {
                return;
            }
            if (rawOnly && !e.isRaw) {
                continue;
            }
            if (nonRawOnly && e.isRaw) {
                continue;
            }
            if (sideFilter > 0 && !e.preferredSide) {
                continue;
            }
            if (sideFilter < 0 && e.preferredSide) {
                continue;
            }
            if (matched++ < skipMatches) {
                continue;
            }
            if (addHotEntry(e)) {
                ++added;
            }
        }
    };

    const bool preserveCachedWantedOnRetarget = rapidDirectionalPreload;
    if (directionalPreload) {
        const size_t backtrackReserve = rapidDirectionalPreload
            ? 0
            : std::min(
                PreloadManager::kDirectionalBacktrackEntries,
                PreloadManager::kMaxEntries);
        const size_t preferredTarget = PreloadManager::kMaxEntries - backtrackReserve;
        const size_t immediateEntries = std::min<size_t>(1, preferredTarget);

        if (rawStridePreload) {
            // Give RAW neighbors first decode priority only after we have seen
            // an actual RAW-to-RAW directional stride. Mixed JPG/RAW browsing
            // keeps the visible next/previous item as the first hot candidate.
            const size_t primaryRawSlots = std::min<size_t>(1, preferredTarget);
            addHotWanted(true, false, 1, primaryRawSlots, primaryRawSlots);
            addHotWanted(
                false,
                false,
                1,
                std::min(PreloadManager::kMaxEntries, primaryRawSlots + immediateEntries),
                immediateEntries);
        } else {
            addHotWanted(false, false, 1, immediateEntries, immediateEntries);
        }
        addHotWanted(true, false, 1, preferredTarget);
        addHotWanted(true, false, -1, preferredTarget + backtrackReserve);
        addHotWanted(false, false, 1, preferredTarget);
        addHotWanted(false, false, -1, preferredTarget + backtrackReserve);
        addHotWanted(true, false, -1, PreloadManager::kMaxEntries);
        addHotWanted(false, false, -1, PreloadManager::kMaxEntries);
        addHotWanted(true, false, 0, PreloadManager::kMaxEntries);
        addHotWanted(false, false, 0, PreloadManager::kMaxEntries);
    } else {
        // Mouse/filmstrip switching often has no explicit direction hint. Do
        // not restrict that path to non-RAWs; nearest RAW neighbors are exactly
        // the expensive cases where decoded handoff makes switching feel fast.
        addHotWanted(true, false, 0, PreloadManager::kMaxEntries);
        addHotWanted(false, false, 0, PreloadManager::kMaxEntries);
    }

    const int scheduledStartDelayMs = directionalPreload
        ? PreloadManager::kDirectionalStartDelayMs
        : PreloadManager::kStartDelayMs;
    const int scheduledInterLoadDelayMs = directionalPreload
        ? PreloadManager::kDirectionalInterLoadDelayMs
        : PreloadManager::kInterLoadDelayMs;
    int scheduledForegroundQuietMs = PreloadManager::kForegroundQuietMs;
    if (directionalPreload) {
        scheduledForegroundQuietMs = PreloadManager::kDirectionalForegroundQuietMs;
        if (recentDirectionalSelectionGapMs_ > 0
            && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDirectionalCadenceMs) {
            scheduledForegroundQuietMs = PreloadManager::kRapidDirectionalForegroundQuietMs;
        }
    }
    int scheduledImmediateRawQuietMs = scheduledForegroundQuietMs;
    if (directionalPreload
        && recentDirectionalSelectionGapMs_ > 0
        && recentDirectionalSelectionGapMs_ <= PreloadManager::kRapidDirectionalCadenceMs) {
        scheduledImmediateRawQuietMs = std::min(
            scheduledForegroundQuietMs,
            PreloadManager::kRapidImmediateRawForegroundQuietMs);
    }
    if (rawStrideCanPreloadThroughEditor) {
        scheduledForegroundQuietMs = std::min(
            scheduledForegroundQuietMs,
            PreloadManager::kDirectionalThroughEditorRawQuietMs);
        scheduledImmediateRawQuietMs = std::min(
            scheduledImmediateRawQuietMs,
            PreloadManager::kDirectionalThroughEditorRawQuietMs);
    }
    const size_t scheduledWantedCount = newWantedSet.size();
    const size_t scheduledHotCount = newHotWantedSet.size();
    bool startWorker = false;
    bool rawQueueUnchanged = false;
    std::vector<rtengine::InitialImage*> evictedPreloadImages;
    {
        std::lock_guard<std::mutex> lock(preload_->mutex);
        rawQueueUnchanged =
            preload_->wanted == newWanted
            && preload_->hotWanted == newHotWanted
            && preload_->startDelayMs == scheduledStartDelayMs
            && preload_->interLoadDelayMs == scheduledInterLoadDelayMs
            && preload_->foregroundQuietMs == scheduledForegroundQuietMs
            && preload_->immediateRawQuietMs == scheduledImmediateRawQuietMs
            && preload_->rawStrideMode == rawStridePreload
            && preload_->rawStrideCanPreloadThroughEditor == rawStrideCanPreloadThroughEditor;

        if (rawQueueUnchanged) {
            FILESEL_LOG("[preload] unchanged wanted=%zu hot=%zu dir=%d rawStride=%d throughEditor=%d quiet=%dms immediateRawQuiet=%dms cadence=%dms anchor=%s\n",
                scheduledWantedCount,
                scheduledHotCount,
                static_cast<int>(preferredDirection),
                static_cast<int>(rawStridePreload),
                static_cast<int>(rawStrideCanPreloadThroughEditor),
                scheduledForegroundQuietMs,
                scheduledImmediateRawQuietMs,
                recentDirectionalSelectionGapMs_,
                fname.c_str());
        } else {

            // Evict cached entries that are no longer hot. The wanted list can be
            // wider than the decoded cache, so keeping lower-priority neighbors
            // would block newly-nearer images from being preloaded.
            for (auto it = preload_->cache.begin(); it != preload_->cache.end(); ) {
                const bool keepWarmNeighbor = preserveCachedWantedOnRetarget
                    && newWantedSet.count(it->first) != 0;
                if (!newHotWantedSet.count(it->first) && !keepWarmNeighbor) {
                    preload_->totalBytes = (preload_->totalBytes > it->second.bytes)
                        ? preload_->totalBytes - it->second.bytes : 0;
                    evictedPreloadImages.push_back(it->second.img);
                    it = preload_->cache.erase(it);
                } else {
                    ++it;
                }
            }
            preload_->wanted = std::move(newWanted);
            preload_->wantedSet = std::move(newWantedSet);
            preload_->hotWanted = std::move(newHotWanted);
            preload_->hotWantedSet = std::move(newHotWantedSet);
            preload_->wantedEntries = std::move(entries);
            preload_->hotWantedEntries = std::move(newHotWantedEntries);
            preload_->startDelayMs = scheduledStartDelayMs;
            preload_->interLoadDelayMs = scheduledInterLoadDelayMs;
            preload_->foregroundQuietMs = scheduledForegroundQuietMs;
            preload_->immediateRawQuietMs = scheduledImmediateRawQuietMs;
            preload_->rawStrideMode = rawStridePreload;
            preload_->rawStrideCanPreloadThroughEditor = rawStrideCanPreloadThroughEditor;
            ++preload_->scheduleGeneration;
            preload_->cv.notify_all();
            FILESEL_LOG("[preload] scheduled wanted=%zu hot=%zu dir=%d rawStride=%d throughEditor=%d quiet=%dms immediateRawQuiet=%dms cadence=%dms anchor=%s\n",
                scheduledWantedCount,
                scheduledHotCount,
                static_cast<int>(preferredDirection),
                static_cast<int>(rawStridePreload),
                static_cast<int>(rawStrideCanPreloadThroughEditor),
                scheduledForegroundQuietMs,
                scheduledImmediateRawQuietMs,
                recentDirectionalSelectionGapMs_,
                fname.c_str());
        }

        if (!preload_->workerRunning) {
            for (const auto& e : preload_->hotWantedEntries) {
                if (preload_->isHotWanted(e.fnameRaw)
                    && !preload_->cache.count(e.fnameRaw)
                    && preload_->hasRoomForLocked(e.fnameRaw, PreloadManager::estimatedEntryBytes(e))) {
                    preload_->workerRunning = true;
                    startWorker = true;
                    break;
                }
            }
        }
    }
    releasePreloadImagesInBackground(std::move(evictedPreloadImages), "retarget", fname);
    FILESEL_LOG("[preload] scheduled wanted=%zu hot=%zu dir=%d quickWarm=%d startDelay=%dms interDelay=%dms foregroundQuiet=%dms startWorker=%d anchor=%s\n",
        scheduledWantedCount,
        scheduledHotCount,
        static_cast<int>(preferredDirection),
        quickPreviewWarmRadius,
        scheduledStartDelayMs,
        scheduledInterLoadDelayMs,
        scheduledForegroundQuietMs,
        static_cast<int>(startWorker),
        fname.c_str());

    if (startWorker) {
        auto state = preload_;  // capture shared_ptr so thread can outlive `this`
        std::thread([state]() {
            const bool logPreload = g_fileSelLogEnabled();
#ifdef _WIN32
            PreloadWorkerPriorityHandle priorityHandle(state);
#endif
            lowerBackgroundPreloadThreadPriority();

            auto finishWorker = [state]() {
                std::lock_guard<std::mutex> lk(state->mutex);
                state->clearLoadingLocked();
                state->workerRunning = false;
            };

            auto waitForPreloadWake = [state](std::chrono::milliseconds delay) {
                if (delay.count() <= 0) {
                    return false;
                }

                std::unique_lock<std::mutex> lk(state->mutex);
                state->cv.wait_for(lk, delay);
                if (state->stopped) {
                    state->clearLoadingLocked();
                    state->workerRunning = false;
                    return true;
                }

                return false;
            };

            // Give rapid clicks first shot at the RAW gate. Directional
            // next/previous navigation gets a shorter delay because the next
            // likely image is predictable and valuable to have decoded.
            int startDelayMs = PreloadManager::kStartDelayMs;
            const auto workerStart = std::chrono::steady_clock::now();
            while (true) {
                std::unique_lock<std::mutex> lk(state->mutex);
                startDelayMs = state->startDelayMs;
                const unsigned observedScheduleGeneration = state->scheduleGeneration;
                const auto startDeadline = workerStart + std::chrono::milliseconds(startDelayMs);
                const bool wokeForSchedule = state->cv.wait_until(lk, startDeadline, [state, observedScheduleGeneration]() {
                    return state->stopped.load(std::memory_order_relaxed)
                        || state->scheduleGeneration != observedScheduleGeneration;
                });
                if (state->stopped) {
                    state->clearLoadingLocked();
                    state->workerRunning = false;
                    return;
                }
                if (!wokeForSchedule || std::chrono::steady_clock::now() >= startDeadline) {
                    break;
                }
            }
            if (logPreload) {
                FILESEL_LOG("[preload] worker start initialDelay=%dms\n", startDelayMs);
            }

            std::unordered_map<std::string, std::chrono::steady_clock::time_point> rawGateDeferredUntil;

            while (true) {
                FileBrowser::AdjacentEntry entry;
                bool hotPriorityCandidate = false;
                bool waitForDeferredRaw = false;
                std::chrono::milliseconds deferredRawDelay(0);
                {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    const auto candidateNow = std::chrono::steady_clock::now();

                    if (!state->loading.empty()
                        && (!state->isHotWanted(state->loading) || state->cache.count(state->loading))) {
                        state->clearLoadingLocked();
                    }

                    if (state->stopped) {
                        state->clearLoadingLocked();
                        state->workerRunning = false;
                        return;
                    }

                    auto candidateReady = [&](const FileBrowser::AdjacentEntry& e, bool rawOnly) {
                        if (rawOnly != e.isRaw) {
                            return false;
                        }
                        if (e.isRaw) {
                            const auto deferred = rawGateDeferredUntil.find(e.fnameRaw);
                            if (deferred != rawGateDeferredUntil.end()
                                && deferred->second > candidateNow) {
                                return false;
                            }
                        }
                        return state->isHotWanted(e.fnameRaw)
                            && !state->cache.count(e.fnameRaw);
                    };

                    auto findCandidate = [&](bool rawOnly) {
                        return std::find_if(
                            state->hotWantedEntries.begin(),
                            state->hotWantedEntries.end(),
                            [candidateReady, rawOnly](const FileBrowser::AdjacentEntry& e) {
                                return candidateReady(e, rawOnly);
                            });
                    };

                    auto waitForNearestDeferredRaw = [&]() {
                        auto nearestDeferredRaw = std::chrono::steady_clock::time_point::max();
                        for (const auto& candidate : state->hotWantedEntries) {
                            if (!candidate.isRaw
                                || !state->isHotWanted(candidate.fnameRaw)
                                || state->cache.count(candidate.fnameRaw)
                                || !state->hasRoomForLocked(candidate.fnameRaw, PreloadManager::estimatedEntryBytes(candidate))) {
                                continue;
                            }

                            const auto deferred = rawGateDeferredUntil.find(candidate.fnameRaw);
                            if (deferred != rawGateDeferredUntil.end()
                                && deferred->second > candidateNow
                                && deferred->second < nearestDeferredRaw) {
                                nearestDeferredRaw = deferred->second;
                            }
                        }

                        if (nearestDeferredRaw == std::chrono::steady_clock::time_point::max()) {
                            return false;
                        }

                        waitForDeferredRaw = true;
                        deferredRawDelay = std::chrono::duration_cast<std::chrono::milliseconds>(
                            nearestDeferredRaw - candidateNow);
                        return true;
                    };

                    auto it = state->hotWantedEntries.end();
                    if (!state->hotWantedEntries.empty()) {
                        const auto immediateIt = state->hotWantedEntries.begin();
                        const auto immediateDeferred = rawGateDeferredUntil.find(immediateIt->fnameRaw);
                        if (immediateIt->isRaw
                            && state->isHotWanted(immediateIt->fnameRaw)
                            && !state->cache.count(immediateIt->fnameRaw)
                            && state->hasRoomForLocked(immediateIt->fnameRaw, PreloadManager::estimatedEntryBytes(*immediateIt))
                            && immediateDeferred != rawGateDeferredUntil.end()
                            && immediateDeferred->second > candidateNow) {
                            waitForDeferredRaw = true;
                            deferredRawDelay = std::chrono::duration_cast<std::chrono::milliseconds>(
                                immediateDeferred->second - candidateNow);
                        } else if (candidateReady(*immediateIt, immediateIt->isRaw)) {
                            it = immediateIt;
                        }
                    }
                    if (!waitForDeferredRaw && it == state->hotWantedEntries.end()) {
                        it = findCandidate(true);
                    }
                    if (!waitForDeferredRaw
                        && it == state->hotWantedEntries.end()
                        && state->rawStrideMode) {
                        waitForNearestDeferredRaw();
                    }
                    if (!waitForDeferredRaw && it == state->hotWantedEntries.end()) {
                        it = findCandidate(false);
                    }

                    if (!waitForDeferredRaw && it == state->hotWantedEntries.end()) {
                        if (!waitForNearestDeferredRaw()) {
                            state->clearLoadingLocked();
                            state->workerRunning = false;
                            return;
                        }
                    }

                    if (!waitForDeferredRaw) {
                        if (!state->hasRoomForLocked(it->fnameRaw, PreloadManager::estimatedEntryBytes(*it))) {
                            state->forgetWantedLocked(it->fnameRaw);
                            continue;
                        }

                        entry = *it;
                        hotPriorityCandidate =
                            !state->hotWantedEntries.empty()
                            && it == state->hotWantedEntries.begin();
                        setBackgroundPreloadThreadPriority(hotPriorityCandidate);
                    }
                }
                if (waitForDeferredRaw) {
                    if (waitForPreloadWake(std::max(
                            deferredRawDelay,
                            std::chrono::milliseconds(PreloadManager::kPreloadRetryMs)))) {
                        return;
                    }
                    continue;
                }

                const std::string& loadFname = entry.fnameRaw;
                if (logPreload) {
                    FILESEL_LOG("[preload] candidate raw=%d file=%s\n",
                        static_cast<int>(entry.isRaw), loadFname.c_str());
                }

                // Serialize with foreground loads. Preloads only enter the
                // decode gate when no foreground open is active or waiting.
                int err = 0;
                rtengine::InitialImage* img = nullptr;
                {
                    std::unique_ptr<RawLoadLease> loadLease;
                    const auto gateCandidateStart = std::chrono::steady_clock::now();
                    bool includeEditorActivity = entry.isRaw;
                    if (entry.isRaw) {
                        int foregroundQuietMs = PreloadManager::kForegroundQuietMs;
                        {
                            std::lock_guard<std::mutex> lk(state->mutex);
                            foregroundQuietMs = state->foregroundQuietMs;
                            if (!state->hotWantedEntries.empty()
                                && state->hotWantedEntries.front().fnameRaw == loadFname) {
                                foregroundQuietMs = state->immediateRawQuietMs;
                            }
                            includeEditorActivity = !state->rawStrideCanPreloadThroughEditor;
                        }
                        loadLease.reset(new RawLoadLease(
                            false,
                            std::chrono::milliseconds(foregroundQuietMs),
                            includeEditorActivity));
                        if (!loadLease->acquired) {
                            if (loadLease->preloadResult == RawLoadGate::PreloadAcquireResult::TooSoon) {
                                const auto retryDelay = std::max(
                                    loadLease->retryAfter,
                                    std::chrono::milliseconds(PreloadManager::kPreloadRetryMs));
                                if (logPreload) {
                                    FILESEL_LOG("[preload] raw gate too soon retry=%lldms file=%s\n",
                                        static_cast<long long>(retryDelay.count()),
                                        loadFname.c_str());
                                }
                                rawGateDeferredUntil[loadFname] = std::chrono::steady_clock::now() + retryDelay;
                                continue;
                            }
                            if (logPreload) {
                                FILESEL_LOG("[preload] raw gate busy retry=%dms file=%s\n",
                                    PreloadManager::kPreloadBusyRetryMs,
                                    loadFname.c_str());
                            }
                            rawGateDeferredUntil[loadFname] =
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(PreloadManager::kPreloadBusyRetryMs);
                            continue;
                        }
                    } else {
                        std::chrono::milliseconds retryAfter(0);
                        const auto preloadReadiness = g_rawLoadGate.checkPreloadReadiness(
                            std::chrono::milliseconds(PreloadManager::kNonRawForegroundQuietMs),
                            retryAfter,
                            false);
                        if (preloadReadiness != RawLoadGate::PreloadAcquireResult::Acquired) {
                            if (preloadReadiness == RawLoadGate::PreloadAcquireResult::TooSoon) {
                                const auto retryDelay = std::max(
                                    retryAfter,
                                    std::chrono::milliseconds(PreloadManager::kPreloadRetryMs));
                                if (logPreload) {
                                    FILESEL_LOG("[preload] foreground quiet retry=%lldms file=%s\n",
                                        static_cast<long long>(retryDelay.count()),
                                        loadFname.c_str());
                                }
                                if (waitForPreloadWake(retryDelay)) {
                                    return;
                                }
                                continue;
                            }
                            if (logPreload) {
                                FILESEL_LOG("[preload] foreground busy retry=%dms file=%s\n",
                                    PreloadManager::kPreloadBusyRetryMs,
                                    loadFname.c_str());
                            }
                            if (waitForPreloadWake(std::chrono::milliseconds(PreloadManager::kPreloadBusyRetryMs))) {
                                return;
                            }
                            continue;
                        }
                    }
                    const auto pressureBeforeLoading =
                        g_rawLoadGate.foregroundPressureSince(gateCandidateStart, loadFname, includeEditorActivity);
                    if (pressureBeforeLoading.any && !pressureBeforeLoading.sameFile) {
                        if (logPreload) {
                            FILESEL_LOG("[preload] yield to foreground before decode retry=%dms file=%s\n",
                                PreloadManager::kPreloadRetryMs,
                                loadFname.c_str());
                        }
                        if (loadLease) {
                            loadLease->releaseNow();
                        }
                        if (waitForPreloadWake(std::chrono::milliseconds(PreloadManager::kPreloadRetryMs))) {
                            return;
                        }
                        continue;
                    }
                    if (state->stopped) {
                        finishWorker();
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(state->mutex);
                        if (!state->isHotWanted(loadFname) || state->cache.count(loadFname)) {
                            continue;
                        }
                        // Advertise handoff only once this worker is about to
                        // decode. While it is merely waiting for foreground
                        // quiet time, a click should steal the load instead.
                        state->loading = loadFname;
                    }
                    const auto pressureAfterLoading =
                        g_rawLoadGate.foregroundPressureSince(gateCandidateStart, loadFname, includeEditorActivity);
                    if (pressureAfterLoading.any && !pressureAfterLoading.sameFile) {
                        bool stillWanted = false;
                        {
                            std::lock_guard<std::mutex> lk(state->mutex);
                            stillWanted = state->isHotWanted(loadFname) && !state->cache.count(loadFname);
                            state->clearLoadingLocked(loadFname);
                        }

                        if (logPreload) {
                            FILESEL_LOG("[preload] yield to foreground before decode retry=%dms wanted=%d file=%s\n",
                                PreloadManager::kPreloadRetryMs,
                                static_cast<int>(stillWanted),
                                loadFname.c_str());
                        }
                        if (loadLease) {
                            loadLease->releaseNow();
                        }
                        if (waitForPreloadWake(std::chrono::milliseconds(PreloadManager::kPreloadRetryMs))) {
                            return;
                        }
                        continue;
                    }
                    const auto decodeStart = logPreload ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    if (logPreload) {
                        FILESEL_LOG("[preload] decode start raw=%d file=%s\n",
                            static_cast<int>(entry.isRaw), loadFname.c_str());
                    }
                    {
                        BackgroundPreloadOpenMPGuard ompGuard;
                        img = rtengine::InitialImage::load(loadFname, entry.isRaw, &err, nullptr);
                    }
                    if (logPreload) {
                        FILESEL_LOG("[preload] decode done duration=%lldms err=%d result=%d file=%s\n",
                            fileSelDurationMs(decodeStart, std::chrono::steady_clock::now()),
                            err,
                            static_cast<int>(img != nullptr),
                            loadFname.c_str());
                    }
                }

                if (!img || err) {
                    if (img) {
                        std::vector<rtengine::InitialImage*> releaseImages;
                        releaseImages.push_back(img);
                        releasePreloadImagesInBackground(std::move(releaseImages), "decode-error", Glib::ustring(loadFname));
                    }
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->forgetWantedLocked(loadFname);
                    state->clearLoadingLocked(loadFname);
                    continue;
                }

                const size_t bytes = PreloadManager::estimatedLoadedEntryBytes(entry, img);

                int interLoadDelayMs = PreloadManager::kInterLoadDelayMs;
                std::vector<rtengine::InitialImage*> releaseImages;
                const char* releaseReason = nullptr;
                bool cachedDecodedImage = false;
                {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    const bool foregroundHandoff = state->foregroundHandoffSet.count(loadFname) != 0;
                    if (state->stopped
                        || (!foregroundHandoff && !state->isHotWanted(loadFname))
                        || state->cache.count(loadFname)) {
                        if (logPreload) {
                            FILESEL_LOG("[preload] drop decoded stopped=%d wanted=%d handoff=%d alreadyCached=%d file=%s\n",
                                static_cast<int>(state->stopped),
                                static_cast<int>(state->isHotWanted(loadFname)),
                                static_cast<int>(foregroundHandoff),
                                static_cast<int>(state->cache.count(loadFname) != 0),
                                loadFname.c_str());
                        }
                        releaseImages.push_back(img);
                        releaseReason = "drop-decoded";
                        state->foregroundHandoffSet.erase(loadFname);
                        state->clearLoadingLocked(loadFname);
                    } else if (!(foregroundHandoff
                        ? state->makeRoomForForegroundLocked(bytes, &releaseImages)
                        : state->makeRoomForLocked(loadFname, bytes, &releaseImages))) {
                        if (logPreload) {
                            FILESEL_LOG("[preload] drop decoded no-room handoff=%d bytes=%zu total=%zu entries=%zu file=%s\n",
                                static_cast<int>(foregroundHandoff),
                                bytes,
                                state->totalBytes,
                                state->cache.size(),
                                loadFname.c_str());
                        }
                        releaseImages.push_back(img);
                        releaseReason = "no-room";
                        state->forgetWantedLocked(loadFname);
                        state->foregroundHandoffSet.erase(loadFname);
                        state->clearLoadingLocked(loadFname);
                    } else {
                        state->cache.emplace(loadFname, PreloadManager::Entry{img, bytes});
                        state->totalBytes += bytes;
                        cachedDecodedImage = true;
                        if (logPreload) {
                            FILESEL_LOG("[preload] cached bytes=%zu total=%zu entries=%zu handoff=%d file=%s\n",
                                bytes,
                                state->totalBytes,
                                state->cache.size(),
                                static_cast<int>(foregroundHandoff),
                                loadFname.c_str());
                        }
                        state->clearLoadingLocked(loadFname);
                        interLoadDelayMs = state->interLoadDelayMs;
                    }
                }
                if (!releaseImages.empty()) {
                    releasePreloadImagesInBackground(
                        std::move(releaseImages),
                        releaseReason ? releaseReason : "evict",
                        Glib::ustring(loadFname));
                }
                if (!cachedDecodedImage) {
                    continue;
                }
                if (waitForPreloadWake(std::chrono::milliseconds(interLoadDelayMs))) {
                    return;
                }
            }
        }).detach();
    }

    // Thumbnail refresh, when requested, was folded into the adjacent scan
    // above; the click path still uses refreshThumbnails=false.
}
void FilePanel::saveOptions ()
{
    auto& options = App::get().mut_options();

    int winW, winH;
    parent->get_size(winW, winH);
    options.dirBrowserWidth = dirpaned->get_position ();
    options.dirBrowserHeight = placespaned->get_position ();
    options.browserToolPanelWidth = winW - get_position();
    options.browserToolPanelHeight = tpcPaned->get_position ();

    if (options.startupDir == STARTUPDIR_LAST && !fileCatalog->lastSelectedDir().empty()) {
        options.startupPath = fileCatalog->lastSelectedDir ();
    }

    fileCatalog->closeDir ();
}

void FilePanel::open (const Glib::ustring& d)
{

    if (Glib::file_test (d, Glib::FILE_TEST_IS_DIR)) {
        dirBrowser->open (d.c_str());
    } else if (Glib::file_test (d, Glib::FILE_TEST_EXISTS)) {
        dirBrowser->open (Glib::path_get_dirname(d), Glib::path_get_basename(d));
    }
}

void FilePanel::optionsChanged ()
{

    tpc->optionsChanged ();
    fileCatalog->refreshThumbImages ();
}

bool FilePanel::handleShortcutKey (GdkEventKey* event)
{

    if(tpc->getToolBar() && tpc->getToolBar()->handleShortcutKey(event)) {
        return true;
    }

    if(tpc->handleShortcutKey(event)) {
        return true;
    }

    if(fileCatalog->handleShortcutKey(event)) {
        return true;
    }

    return false;
}

bool FilePanel::handleShortcutKeyRelease(GdkEventKey *event)
{
    if(fileCatalog->handleShortcutKeyRelease(event)) {
        return true;
    }

    return false;
}

void FilePanel::onAlbumSelected (const std::set<std::string>& whitelist)
{
    if (fileCatalog) {
        fileCatalog->setAlbumWhitelist(whitelist);
    }
}

void FilePanel::onAlbumViewRequested (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files)
{
    if (fileCatalog) {
        if (albumName.empty()) {
            fileCatalog->exitAlbumMode();
        } else {
            fileCatalog->showAlbumFiles(albumName, files);
        }
    }
}

void FilePanel::closeAlbumView ()
{
    if (albumBrowser_) {
        albumBrowser_->deselectAlbum();
    }
    if (fileCatalog) {
        fileCatalog->exitAlbumMode();
    }
}

void FilePanel::openSelectedInEditor ()
{
    if (!parent || !fileCatalog || !fileCatalog->fileBrowser) return;

    Thumbnail* thm = fileCatalog->fileBrowser->getSelectedThumbnail();
    if (!thm) return;

    // Don't re-open the same image that's already loaded in the editor
    if (parent->epanel && parent->epanel->getFileName() == thm->getFileName()) {
        return;
    }

    thm->increaseRef();
    fileCatalog->openRequested({thm});
}

void FilePanel::loadingThumbs(Glib::ustring str, double rate)
{
    GThreadLock lock; // All GUI access from idle_add callbacks or separate thread HAVE to be protected

    if( !str.empty()) {
        parent->setProgressStr(str);
    }

    parent->setProgress( rate );
}

void FilePanel::updateTPVScrollbar (bool hide)
{
    tpc->updateTPVScrollbar (hide);
}

void FilePanel::updateToolPanelToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools)
{
    if (tpc) {
        tpc->updateToolLocations(favorites, cloneFavoriteTools);
    }
}

void FilePanel::getQueueOverlayInsets (int& left, int& top, int& right) const
{
    // Left: directory browser pane width (the paned split position)
    left = dirpaned ? dirpaned->get_position () : 0;
    // Right: right notebook panel width
    right = (rightBox && rightBox->get_visible()) ? rightBox->get_allocated_width () : 0;
    // No filmstrip in file browser
    top = 0;
}
