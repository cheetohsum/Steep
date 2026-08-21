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
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <gtkmm.h>

#include "editenums.h"
#include "filethumbnailbuttonset.h"
#include "imageareatoollistener.h"
#include "thumbbrowserentrybase.h"
#include "thumbimageupdater.h"
#include "thumbnaillistener.h"

#include "rtengine/noncopyable.h"
#include "rtengine/rtengine.h"

class FileBrowserEntry;
class Thumbnail;
class RTSurface;

struct FileBrowserEntryIdleHelper {
    FileBrowserEntry* fbentry;
    bool destroyed;
    std::atomic<int> pending;
    std::atomic<std::uint64_t> imageUpdateGeneration;
};

class FileThumbnailButtonSet;
class FileBrowserEntry final : public ThumbBrowserEntryBase,
    public ThumbnailListener,
    public ThumbImageUpdateListener,
    public rtengine::NonCopyable
{

    double scale;
    bool wasInside;
    ImageAreaToolListener* iatlistener;
    int press_x, press_y, action_x, action_y;
    double rot_deg;
    bool landscape;
    double deliveredPreviewAspectRatio_;
    const std::unique_ptr<rtengine::procparams::CropParams> cropParams;
    CropGUIListener* cropgl;
    FileBrowserEntryIdleHelper* feih;
    std::string browserPathKey_;
    std::string browserFileNameUpper_;
    std::string browserOriginalFamilyKey_;
    bool suppressThumbnailRefresh;
    std::atomic<bool> lazyThumbnailRequestPending;
    std::atomic<bool> thumbnailPreviewUsable_;
    std::atomic<bool> liveEditorPreview_;
    std::atomic<unsigned> thumbnailRetryCount_;
    std::atomic<std::uint64_t> thumbnailRetryGeneration_;

    ImgEditState state;
    float crop_custom_ratio;

    bool hasUsableThumbnailPreview ();
    bool imageUpdateMatchesCurrentPreview (hidpi::LogicalSize size, int deviceScale);
    bool onArea (CursorArea a, int x, int y);
    void updateCursor (int x, int y);
    void drawStraightenGuide (Cairo::RefPtr<Cairo::Context> c);
    void customBackBufferUpdate (Cairo::RefPtr<Cairo::Context> c) override;
    void savePreviewSlotExtras (PreviewSlot& slot) const override;
    void loadPreviewSlotExtras (const PreviewSlot& slot) override;
    void refreshThumbnailImage(bool upgradeHint);

public:

    static std::shared_ptr<RTSurface> editedIcon;
    static std::shared_ptr<RTSurface> recentlySavedIcon;
    static std::shared_ptr<RTSurface> enqueuedIcon;
    static std::shared_ptr<RTSurface> hdr;
    static std::shared_ptr<RTSurface> ps;

    FileBrowserEntry (Thumbnail* thm, const Glib::ustring& fname);
    ~FileBrowserEntry () override;
    static void init ();
    static void pauseQueuedImageUpdates ();
    static void resumeQueuedImageUpdates ();
    void draw (Cairo::RefPtr<Cairo::Context> cc) override;
    const std::string& getBrowserPathKey () const { return browserPathKey_; }
    void setBrowserPathKey (const std::string& key) { browserPathKey_ = key; }
    void setBrowserPathKey (std::string&& key) { browserPathKey_ = std::move(key); }
    const std::string& getBrowserFileNameUpper () const { return browserFileNameUpper_; }
    const std::string& getBrowserOriginalFamilyKey () const { return browserOriginalFamilyKey_; }
    void setBrowserOriginalFamilyKey (std::string&& key) { browserOriginalFamilyKey_ = std::move(key); }

    void setImageAreaToolListener (ImageAreaToolListener* l)
    {
        iatlistener = l;
    }

    FileThumbnailButtonSet* getThumbButtonSet ();
    FileThumbnailButtonSet* ensureThumbButtonSet (LWButtonListener* listener);
    void resizeWithoutThumbnailJob (int h);

    void refreshThumbnailImage () override;
    void refreshQuickThumbnailImage () override;
    void appendQuickThumbnailJob (std::vector<ThumbImageUpdater::Request>& requests, bool cachePixbuf = false) override;
    void retryThumbnailNow ();
    void runThumbnailRetry (std::uint64_t generation);
    bool cacheCurrentPreviewForQuickOpen ();
    hidpi::ScaledDeviceSize getLivePreviewDeviceSize () const;
    bool setLiveEditorPreview (
        const Glib::RefPtr<Gdk::Pixbuf>& pixbuf,
        double imageScale,
        const rtengine::procparams::CropParams& crop);
    void invalidateTransientPreview(bool refresh);
    void calcThumbnailSize () override;
    void onDeviceScaleChanged (int newDeviceScale) override;
    std::size_t getImageAreaIconState () override;
    bool imageAreaIconsChanged () override;

    std::vector<std::shared_ptr<RTSurface>> getIconsOnImageArea () override;
    std::vector<std::shared_ptr<RTSurface>> getSpecificityIconsOnImageArea () override;
    void getIconSize (int& w, int& h) const override;

    // thumbnaillistener interface
    void procParamsChanged (Thumbnail* thm, int whoChangedIt, bool upgradeHint) override;
    // thumbimageupdatelistener interface
    void updateImage(const ThumbImageUpdateListener::ImageUpdate& update) override;
    void updateImageFailed(hidpi::LogicalSize size, int deviceScale, bool upgrade) override;
    void discardQueuedImageUpdate();
    void _updateImage(
        rtengine::IImage8* img,
        hidpi::LogicalSize size,
        int deviceScale,
        double imageScale,
        const rtengine::procparams::CropParams& crop,
        bool queuedRequest = true); // inside gtk thread

    bool    motionNotify  (int x, int y) override;
    bool    pressNotify   (int button, int type, int bstate, int x, int y) override;
    bool    releaseNotify (int button, int type, int bstate, int x, int y) override;
};
