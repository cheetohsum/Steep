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

#include <chrono>
#include <atomic>
#include <cstddef>
#include <tuple>
#include <string>
#include <vector>
#include <gtkmm.h>

#include "cursormanager.h"
#include "guiutils.h"
#include "hidpi.h"
#include "threadutils.h"
#include "options.h"
#include "thumbnail.h"
#include "thumbimageupdater.h"
#include "widgets/basic/lwbuttonset.h"

#include "rtengine/coord2d.h"

class Thumbnail;
class ThumbBrowserBase;
class RTSurface;
class ThumbBrowserEntryBase
{

public:
    enum eWithFilename {
        WFNAME_NONE,
        WFNAME_REDUCED,
        WFNAME_FULL
    };

protected:
    int fnlabw, fnlabh; // dimensions of the filename label
    int dtlabw, dtlabh; // dimensions of the date/time label
    int exlabw, exlabh; // dimensions of the exif label
    bool textMetricsValid_;
    int textMetricsInfoW_;
    int textMetricsInfoH_;
    int textMetricsScaleFactor_;
    eWithFilename textMetricsWithFilename_;
    bool textMetricsShowDateTime_;
    bool textMetricsShowBasicExif_;
    bool textMetricsItalicStyle_;
    Glib::ustring textMetricsFont_;
    hidpi::LogicalSize previewSize;
    hidpi::LogicalCoord prevPos;

    int activeDeviceScale;
    int pendingDeviceScale;

    int upperMargin;
    int borderWidth;
    int textGap;
    int sideMargin;
    int lowerMargin;


    MyRWMutex lockRW;  // Locks access to all image thumb changing actions

    std::vector<guint8> preview;  // holds the preview image. used in updateBackBuffer.
    struct PreviewDataLayout {
        int width = 0;
        int height = 0;
    };
    PreviewDataLayout previewDataLayout;

    // Rendered previews retained per logical size. The browser grid and the
    // editor filmstrip ask for different heights, so without this every view
    // switch would drop each entry's pixels and re-run the whole thumbnail
    // pipeline for the entire folder. Two slots cover both view sizes.
    struct PreviewSlot {
        hidpi::LogicalSize logicalSize;
        int deviceScale = 0;
        PreviewDataLayout layout;
        std::vector<guint8> data;
        double aspect = 0.0;
        double imgScale = 1.0;
        bool landscape = false;
        bool valid = false;
        unsigned lastUse = 0;
    };
    static constexpr std::size_t PREVIEW_SLOT_COUNT = 2;
    PreviewSlot previewSlots_[PREVIEW_SLOT_COUNT];
    unsigned previewSlotClock_;

    // Extra per-size state owned by derived entries (image scale, orientation).
    virtual void savePreviewSlotExtras (PreviewSlot& slot) const { (void)slot; }
    virtual void loadPreviewSlotExtras (const PreviewSlot& slot) { (void)slot; }

    // Copy the live preview into the slot matching `size` (must be called with
    // the write lock held). Returns false when there is nothing worth keeping.
    bool stashPreviewForSize (hidpi::LogicalSize size, int deviceScale);
    // Restore a previously rendered preview for `size`, if one is retained.
    bool restorePreviewForSize (hidpi::LogicalSize size, int deviceScale);
    void invalidatePreviewSlots ();

    Glib::ustring dispname;

    LWButtonSet* buttonSet;

    int width;      // minimal width
    int height;     // minimal height
    // Arranged size (back buffer dimensions)
    // set by arrangeFiles() of thumbbrowser
    hidpi::LogicalSize expected;
    int startx;     // x coord. in the widget
    int starty;     // y coord. in the widget

    int ofsX, ofsY; // offset due to the scrolling of the parent

    std::atomic<int> redrawRequests;

    ThumbBrowserBase* parent;
    ThumbBrowserEntryBase* original;

    Glib::RefPtr<BackBuffer> backBuffer;
    bool bbSelected, bbFramed;
    guint8* bbPreview;
    std::size_t bbImageAreaIconState;
    std::vector<std::shared_ptr<RTSurface>> bbIcons;
    std::vector<std::shared_ptr<RTSurface>> bbSpecificityIcons;
    CursorShape cursor_type;

    void drawFrame (Cairo::RefPtr<Cairo::Context> cr, const Gdk::RGBA& bg, const Gdk::RGBA& fg);
    void getTextSizes (int& w, int& h);
    bool buttonSetVisible () const;
    void ensureInfoLines () const;
    const std::string& getExifCollateKey () const;
    virtual std::size_t getImageAreaIconState ();
    virtual bool imageAreaIconsChanged ();

    // called during updateBackBuffer for custom overlays
    virtual void customBackBufferUpdate (Cairo::RefPtr<Cairo::Context> c) {}

private:
    const std::string collate_name;
    mutable std::string collate_exif;
    mutable bool collate_exif_valid = false;
    const std::string collate_ext;

public:

    Thumbnail* thumbnail;

// thumbnail preview properties:
    Glib::ustring filename;
    // Session-pinned entry (e.g. a double-exposure partner surfaced next to
    // the image it composites into): sorts directly after the entry whose
    // filename this holds, and bypasses browser filters. Empty = normal.
    Glib::ustring pinAfter;
    mutable Glib::ustring exifline;
    mutable Glib::ustring datetimeline;
    mutable bool infoLinesValid = false;

// misc attributes
    bool selected;
    bool drawable;
    bool filtered;
    bool framed;
    bool processing;
    bool italicstyle;
    bool edited;
    bool recentlysaved;
    bool updatepriority;
    std::size_t visibleGeneration;
    eWithFilename withFilename;

    explicit ThumbBrowserEntryBase (const Glib::ustring& fname, Thumbnail *thm);
    virtual ~ThumbBrowserEntryBase ();

    void setParent (ThumbBrowserBase* l)
    {
        parent = l;
    }

    void updateBackBuffer ();
    // Drop the cell surface and any retained per-size previews for an entry
    // that is far outside the viewport. The decoded preview is kept, so
    // redrawing needs no thumbnail work.
    void releaseOffscreenBuffers ();
    void resize (int h);
    virtual void draw (Cairo::RefPtr<Cairo::Context> cc);

    void addButtonSet (LWButtonSet* bs);
    void setMargins (int upper, int lower)
    {
        upperMargin = upper;
        lowerMargin = lower;
    }
    void setDirty ()
    {
        if (backBuffer) backBuffer->setDirty(true);
    }
    int getMinimalHeight () const
    {
        return height;
    }
    int getMinimalWidth () const
    {
        return width;
    }

    int getEffectiveWidth () const
    {
        return expected.width;
    }
    int getEffectiveHeight () const
    {
        return expected.height;
    }

    std::pair<hidpi::LogicalSize, int> getDesiredPreviewSize() const;

    int getStartX () const
    {
        return startx;
    }
    int getStartY () const
    {
        return starty;
    }
    int getOffsetX () const;
    int getOffsetY () const;
    int getX () const;
    int getY () const;
    bool shouldCacheRenderedThumbnailPixbuf () const;

    bool inside (int x, int y) const;
    rtengine::Coord2D getPosInImgSpace (int x, int y) const;
    bool insideWindow (int x, int y, int w, int h) const;
    void setPosition (int x, int y, int w, int h);
    void setOffset (int x, int y);

    bool compare (const ThumbBrowserEntryBase& other, Options::SortMethod method) const
    {
        int cmp = 0;
        switch (method){
        case Options::SORT_BY_NAME:
            return collate_name < other.collate_name;
        case Options::SORT_BY_DATE:
            cmp = thumbnail->getDateTime().compare(other.thumbnail->getDateTime());
            break;
        case Options::SORT_BY_EXIF:
            cmp = getExifCollateKey().compare(other.getExifCollateKey());
            break;
        case Options::SORT_BY_RANK:
            cmp = thumbnail->getRank() - other.thumbnail->getRank();
            break;
        case Options::SORT_BY_LABEL:
            cmp = thumbnail->getColorLabel() - other.thumbnail->getColorLabel();
            break;
        case Options::SORT_BY_FILETYPE:
            cmp = collate_ext.compare(other.collate_ext);
            break;
        case Options::SORT_METHOD_COUNT: abort();
        }

        // Always fall back to sorting by name
        if (!cmp)
            cmp = collate_name.compare(other.collate_name);

        return cmp < 0;
    }

    virtual void onDeviceScaleChanged(int newDeviceScale);

    virtual void refreshThumbnailImage () = 0;
    virtual void refreshQuickThumbnailImage () {}
    virtual void appendQuickThumbnailJob (std::vector<ThumbImageUpdater::Request>& requests, bool cachePixbuf = false);
    virtual void calcThumbnailSize () = 0;

    virtual void drawProgressBar (Glib::RefPtr<Gdk::Window> win, const Gdk::RGBA& foregr, const Gdk::RGBA& backgr, int x, int w, int y, int h) {}

    virtual std::vector<std::shared_ptr<RTSurface>> getIconsOnImageArea ();
    virtual std::vector<std::shared_ptr<RTSurface>> getSpecificityIconsOnImageArea ();
    virtual void getIconSize (int& w, int& h) const = 0;

    virtual bool motionNotify (int x, int y);
    virtual bool pressNotify (int button, int type, int bstate, int x, int y);
    virtual bool releaseNotify (int button, int type, int bstate, int x, int y);
    virtual std::tuple<Glib::ustring, bool> getToolTip (int x, int y) const;

    inline ThumbBrowserEntryBase* getOriginal() const
    {
        return original;
    }

    inline void setOriginal(ThumbBrowserEntryBase* original)
    {
        this->original = original;
    }

    // Animation for rating/label changes (filmstrip overlay)
    void startRatingAnimation ();
    void startColorLabelAnimation ();
    void startPickAnimation ();

protected:
    // Animation state
    double animRatingAlpha_;       // 0.0 = no anim, >0 = fading out
    double animColorAlpha_;
    double animPickAlpha_;
    sigc::connection animTimerConn_;
    std::chrono::steady_clock::time_point animStartTime_;
    bool animRatingActive_;
    bool animColorActive_;
    bool animPickActive_;
    static constexpr double ANIM_DURATION_MS = 600.0;

    void drawFilmstripOverlays (Cairo::RefPtr<Cairo::Context> cc, int x, int y);
    bool animTick ();
};
