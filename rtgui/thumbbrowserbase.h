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

#include <cstddef>
#include <set>
#include <vector>

#include <gtkmm.h>

#include "guiutils.h"
#include "options.h"
#include "thumbimageupdater.h"

/*
 * Class handling the list of ThumbBrowserEntry objects and their position in it's allocated space
 */

class Inspector;
class ThumbBrowserEntryBase;

class ThumbBrowserBase :
    public Gtk::Grid
{

    class Internal :
        public Gtk::DrawingArea
    {
        //Cairo::RefPtr<Cairo::Context> cc;
        int ofsX, ofsY;
        ThumbBrowserBase* parent;
        bool dirty;

        // caching some very often used values
        Glib::RefPtr<Gtk::StyleContext> style;
        Gdk::RGBA textn;
        Gdk::RGBA texts;
        Gdk::RGBA bgn;
        Gdk::RGBA bgs;

    public:
        Internal ();
        void setParent (ThumbBrowserBase* p);
        void on_realize() override;
        void on_style_updated() override;
        bool on_configure_event(GdkEventConfigure *configure_event) override;
        bool on_draw(const ::Cairo::RefPtr< Cairo::Context> &cr) override;

        Gtk::SizeRequestMode get_request_mode_vfunc () const override;
        void get_preferred_height_vfunc (int &minimum_height, int &natural_height) const final;
        void get_preferred_width_vfunc (int &minimum_width, int &natural_width) const final;
        void get_preferred_height_for_width_vfunc (int width, int &minimum_height, int &natural_height) const final;
        void get_preferred_width_for_height_vfunc (int height, int &minimum_width, int &natural_width) const final;

        bool on_button_press_event (GdkEventButton* event) override;
        bool on_button_release_event (GdkEventButton* event) override;
        bool on_motion_notify_event (GdkEventMotion* event) override;
        bool on_scroll_event (GdkEventScroll* event) override;
        bool on_key_press_event (GdkEventKey* event) override;
        bool on_query_tooltip (int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip);
        void setPosition (int x, int y);

        Glib::RefPtr<Gtk::StyleContext> getStyle() {
            return style;
        }
        Gdk::RGBA getNormalTextColor() {
            return textn;
        }
        Gdk::RGBA getSelectedTextColor() {
            return texts;
        }
        Gdk::RGBA getNormalBgColor() {
            return bgn;
        }
        Gdk::RGBA getSelectedBgColor() {
            return bgs;
        }

        void setDirty ()
        {
            dirty = true;
        }
        bool isDirty  ()
        {
            return dirty;
        }
    };

public:

    enum eLocation {
        THLOC_BATCHQUEUE,
        THLOC_FILEBROWSER,
        THLOC_EDITOR
    } location;

protected:
    virtual int getMaxThumbnailHeight() const
    {
        return App::get().options().maxThumbnailHeight;    // Differs between batch and file
    }
    virtual void saveThumbnailHeight (int height) = 0;
    virtual int  getThumbnailHeight () = 0;

    Internal internal;
    Gtk::Scrollbar hscroll;
    Gtk::Scrollbar vscroll;
    bool hscrollForceHidden;
    int lastDeviceScale;

    int inW, inH;

    Inspector *inspector;
    bool isInspectorActive;

    void resizeThumbnailArea (int w, int h);
    void internalAreaResized (Gtk::Allocation& req);
    void buttonPressed (int x, int y, int button, GdkEventType type, int state, int clx, int cly, int clw, int clh);

    void onInternalAreaDraw();

public:

    void setInspector(Inspector* inspector)
    {
        this->inspector = inspector;
    }
    Inspector* getInspector()
    {
        return inspector;
    }
    void disableInspector();
    void enableInspector();
    enum Arrangement {TB_Horizontal, TB_Vertical};
    void configScrollBars ();
    void scrollChanged ();
    void scroll (int direction, double deltaX=0.0, double deltaY=0.0);
    void scrollPage (int direction);

private:
    void selectSingle (ThumbBrowserEntryBase* clicked);
    void selectRange (ThumbBrowserEntryBase* clicked, bool additional);
    void selectSet (ThumbBrowserEntryBase* clicked);

public:
    void selectPrev (int distance, bool enlarge);
    void selectNext (int distance, bool enlarge);
    void selectFirst (bool enlarge);
    void selectLast (bool enlarge);

    virtual bool isInTabMode()
    {
        return false;
    }

    eLocation getLocation()
    {
        return location;
    }

protected:

    int eventTime;

    MyRWMutex entryRW;  // Locks access to following 'fd' AND 'selected'
    std::vector<ThumbBrowserEntryBase*> fd;
    std::vector<ThumbBrowserEntryBase*> selected;
    ThumbBrowserEntryBase* lastClicked;
    ThumbBrowserEntryBase* anchor;

    int previewHeight;
    int numOfCols;
    int lastRowHeight;

    Arrangement arrangement;

    std::set<Glib::ustring> editedFiles;

    void arrangeFiles (ThumbBrowserEntryBase* entry = nullptr, bool filterStateCurrent = false);
    void zoomChanged (bool zoomIn);
    virtual void entriesOrderChanged_() {}
    virtual void entriesInserted_(const std::vector<ThumbBrowserEntryBase*>& entries) {}

public:

    ThumbBrowserBase ();
    ~ThumbBrowserBase () override;

    void zoomIn ()
    {
        zoomChanged (true);
    }
    void zoomOut ()
    {
        zoomChanged (false);
    }
    void setThumbnailHeight (int newHeight);
    int getEffectiveHeight ();

    const std::vector<ThumbBrowserEntryBase*>& getEntries ()
    {
        return fd;
    }
    void on_style_updated () override;
    void resort ();

protected:
    // Reposition pinned entries (pinAfter set) directly after their anchor.
    // Caller must hold the entryRW write lock.
    void applyPinnedOrder_ ();

public: // re-apply sort method
    void redraw (ThumbBrowserEntryBase* entry = nullptr, bool filterStateCurrent = false);   // arrange files and draw area
    void refreshThumbImages (); // refresh thumbnail sizes, re-generate thumbnail images, arrange and draw
    void refreshQuickThumbImages (); // refresh thumbnail sizes, re-generate thumbnail images, arrange and draw
    void refreshEditedState (const std::set<Glib::ustring>& efiles);

    void insertEntry (ThumbBrowserEntryBase* entry);
    void insertEntries (const std::vector<ThumbBrowserEntryBase*>& entries);

    // Pause/resume layout during animations to prevent arrangeFiles() from
    // competing with animation frames on the main thread.
    void pauseLayout ();
    void resumeLayout ();

    // Request a reflow, coalescing bursts into a single pass.
    void scheduleRelayout ();

protected:
    bool redrawPending_ = false;
    // Refcounted: the sidebar slide and the filmstrip slide can overlap, and
    // whichever finishes first must not unpause the other.
    int layoutPauseDepth_ = 0;
    bool layoutPaused_ () const { return layoutPauseDepth_ > 0; }
    // A size-allocate arrived while layout was paused (panel slide animations
    // re-allocate every frame); one relayout on resume covers them all.
    bool layoutDeferredResize_ = false;
    std::vector<ThumbBrowserEntryBase*> pendingInserts_;
    std::vector<ThumbBrowserEntryBase*> visibleEntries_;
    std::vector<ThumbBrowserEntryBase*> previousVisibleEntries_;
    std::vector<ThumbBrowserEntryBase*> entriesToDraw_;
    std::vector<ThumbImageUpdater::Request> visibleThumbnailRequests_;
    std::vector<ThumbBrowserEntryBase*> drawableEntries_;
    std::size_t visibleGenerationCounter_ = 0;
    MyMutex pendingMutex_;
    sigc::connection redrawTimeout_;
    // Coalesces "an entry changed shape, reflow the grid" requests. During a
    // cold folder load essentially every delivered RAW thumbnail raises one,
    // and each relayout is O(N) over the folder.
    sigc::connection relayoutConn_;
    void clearVisibleEntries_();
    void clearDrawableEntries_();
    void schedulePendingInsertRedraw_();
    void flushPendingInserts_ ();
private:
    enum ViewportRelation {
        VREL_BEFORE,
        VREL_INSIDE,
        VREL_OUTSIDE,
        VREL_AFTER
    };

    ViewportRelation viewportRelation_(const ThumbBrowserEntryBase* entry, int x, int y, int w, int h) const;
    std::size_t firstViewportCandidate_(int x, int y) const;
    void rebuildDrawableEntries_();
    void syncEntryOffset_(ThumbBrowserEntryBase* entry);
    bool onRedrawIdle_ ();
    bool applyTabModeEntryGeometry_ (bool enable, unsigned int generation);
    unsigned int tabModeGeneration_ = 0;
public:

    void getScrollPosition (double& h, double& v);
    void setScrollPosition (double h, double v);
    int getScrollOffsetX () const;
    int getScrollOffsetY () const;

    void setArrangement (Arrangement a);
    void enableTabMode(bool enable);  // set both thumb sizes and arrangements

    virtual bool checkFilter (ThumbBrowserEntryBase* entry) const
    {
        return true;
    }
    virtual void rightClicked () = 0;
    virtual void doubleClicked (ThumbBrowserEntryBase* entry) {}
    virtual bool keyPressed (GdkEventKey* event)
    {
        return true;
    }
    virtual void selectionChanged () {}

    virtual void redrawNeeded (ThumbBrowserEntryBase* entry);
    virtual void thumbRearrangementNeeded () {}
    // The set of on-screen entries changed (scroll, resize, reflow).
    virtual void visibleRangeChanged () {}

    Gtk::Widget* getDrawingArea ()
    {
        return &internal;
    }

    Glib::RefPtr<Gtk::StyleContext> getStyle() {
        return internal.getStyle();
    }
    Gdk::RGBA getNormalTextColor() {
        return internal.getNormalTextColor();
    }
    Gdk::RGBA getSelectedTextColor() {
        return internal.getSelectedTextColor();
    }
    Gdk::RGBA getNormalBgColor() {
        return internal.getNormalBgColor();
    }
    Gdk::RGBA getSelectedBgColor() {
        return internal.getSelectedBgColor();
    }

    void setHScrollVisible (bool visible) {
        hscrollForceHidden = !visible;
        if (!visible) {
            hscroll.hide ();
        }
    }

};
