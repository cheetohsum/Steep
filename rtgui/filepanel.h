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
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include <gtkmm.h>

#include "albumbrowser.h"
#include "exportpanel.h"
#include "filecatalog.h"
#include "fileselectionlistener.h"
#include "filterpanel.h"
#include "history.h"
#include "placesbrowser.h"
#include "pparamschangelistener.h"
#include "progressconnector.h"
#include "recentbrowser.h"

#include "rtengine/noncopyable.h"

class BatchToolPanelCoordinator;
class EditorPanel;
class RTWindow;
class DirBrowser;
struct PreloadManager;
struct RecentInitialImageCache;

class FilePanel final :
    public Gtk::Paned,
    public FileSelectionListener,
    public rtengine::NonCopyable
{
public:
    FilePanel ();
    ~FilePanel () override;

    Gtk::Paned* placespaned;
    Gtk::Paned* dirpaned;

    Gtk::Box* rightBox;

    DirBrowser* dirBrowser;
    FilterPanel* filterPanel;
    ExportPanel* exportPanel;
    FileCatalog* fileCatalog;
    Gtk::Paned *ribbonPane;

    void setParent (RTWindow* p)
    {
        parent = p;
    }
    void init (); // don't call it directly, the constructor calls it as idle source
    void on_realize () override;
    void setAspect();
    void open (const Glib::ustring& d); // open a file or a directory
    void refreshEditedState (const std::set<Glib::ustring>& efiles)
    {
        fileCatalog->refreshEditedState (efiles);
    }
    void loadingThumbs(Glib::ustring str, double rate);

    // call this before closing RT: it saves file browser's related things into options
    void saveOptions ();

    // interface fileselectionlistener
    bool fileSelected(Thumbnail* thm) override;
    bool fileSelected(Thumbnail* thm, eRTNav preloadDirectionHint);
    bool addBatchQueueJobs(const std::vector<BatchQueueEntry*>& entries) override;

    void optionsChanged         ();
    bool imageLoaded( Thumbnail* thm, ProgressConnector<rtengine::InitialImage*> * );
    static rtengine::InitialImage* loadAuxiliaryInitialImage(
        const Glib::ustring& fname,
        bool isRaw,
        int* errorCode,
        const std::shared_ptr<std::atomic<bool>>& cancel);
    std::function<void(rtengine::InitialImage*)> makeRecentInitialImageCacheFunc(
        const Glib::ustring& fname,
        bool isRaw);

    bool handleShortcutKey (GdkEventKey* event);
    bool handleShortcutKeyRelease(GdkEventKey *event);
    void updateTPVScrollbar (bool hide);
    void updateToolPanelToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);

    // Fade browser content without affecting the left sidebar
    void setContentOpacity (double opacity);

    // Fast-export settings widget (former Fast Export tab content), to be
    // embedded into the Export/queue tab by RTWindow
    Gtk::Widget* takeFastExportSettingsWidget () { return fastExportSettingsWidget_; }

    // Returns sidebar insets for queue overlay positioning
    void getQueueOverlayInsets (int& left, int& top, int& right) const;
    void closeAlbumView ();  // close album view + deselect sidebar
    void openSelectedInEditor ();  // open the selected browser thumbnail in editor
    bool transientEditPreviewRequested(
        const Glib::ustring& filename,
        const rtengine::procparams::ProcParams* params,
        bool restore);

    // Left panel visibility for sync with editor sidebar
    bool isLeftPanelVisible() const { return browserHideLp_ && browserHideLp_->get_active(); }
    void setLeftPanelVisible(bool visible) { if (browserHideLp_ && browserHideLp_->get_active() != visible) browserHideLp_->set_active(visible); }

private:
    void on_NB_switch_page(Gtk::Widget* page, guint page_num);
    void pauseBackgroundWorkForForeground();
    void resumeBackgroundWorkAfterForeground();
    void resumeBackgroundWorkIfCurrent(unsigned generation);
    void resumeThumbnailWorkIfCurrent(unsigned generation);
    void resumeThumbnailWorkNow();
    void resumeBackgroundWorkNow();
    void cancelScheduledBackgroundResume();
    void scheduleAdjacentPreload(
        const Glib::ustring& fname,
        eRTNav preferredDirection,
        bool refreshThumbnails,
        int minStartDelayMs = 0);

    PlacesBrowser* placesBrowser;
    RecentBrowser* recentBrowser;
    AlbumBrowser* albumBrowser_;
    Gtk::Widget* fastExportSettingsWidget_ = nullptr;

    void onAlbumSelected (const std::set<std::string>& whitelist);
    void onAlbumViewRequested (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files);

    Inspector* inspectorPanel;
    Gtk::Paned* tpcPaned;
    BatchToolPanelCoordinator* tpc;
    History* history;
    RTWindow* parent;
    Gtk::Notebook* rightNotebook;
    sigc::connection rightNotebookSwitchConn;

    struct pendingLoad {
        bool complete;
        bool superseded;     // user clicked a newer image; discard result when decode finishes
        std::shared_ptr<std::atomic<bool>> staleSkipped;
        std::shared_ptr<std::atomic<bool>> quickPreviewAllowed;
        ProgressConnector<rtengine::InitialImage*> *pc;
        Thumbnail *thm;
        EditorPanel *epanel; // pre-created panel (tabbed mode only)
        eRTNav preloadDirectionHint;
        std::chrono::steady_clock::time_point startedAt;
        sigc::connection delayedStartConn;
        bool loadStarted;
        unsigned long long foregroundPriorityId;
    };
    MyMutex pendingLoadMutex;
    std::deque<struct pendingLoad*> pendingLoads;

    int error;

    // Preload cache for adjacent images (speeds up filmstrip navigation).
    // Held in a shared_ptr so detached load threads can safely outlive FilePanel.
    std::shared_ptr<PreloadManager> preload_;
    std::shared_ptr<RecentInitialImageCache> recentInitialImageCache_;
    std::shared_ptr<std::atomic<unsigned>> foregroundLoadGeneration_;
    guint backgroundResumeTimeoutId_;
    unsigned backgroundResumeGeneration_;
    unsigned adjacentPreloadGeneration_;
    bool backgroundWorkPausedForForeground_;
    bool thumbnailWorkPausedForForeground_;
    bool adjacentPreloadIdlePending_;
    Glib::ustring adjacentPreloadFname_;
    eRTNav adjacentPreloadDirection_;
    bool adjacentPreloadRefreshThumbnails_;
    int adjacentPreloadMinStartDelayMs_;
    Glib::ustring recentDirectionalPreloadFname_;
    eRTNav recentDirectionalPreloadDirection_;
    std::chrono::steady_clock::time_point recentDirectionalPreloadUntil_;
    std::chrono::steady_clock::time_point lastDirectionalSelectionAt_;
    eRTNav recentDirectionalSelectionDirection_;
    int recentDirectionalSelectionGapMs_;
    unsigned recentDirectionalSelectionRunLength_;
    bool recentDirectionalSelectionWasRaw_;
    unsigned recentDirectionalRawSelectionRunLength_;

public:
    void preloadAdjacent(
        const Glib::ustring& fname,
        eRTNav preferredDirection = NAV_NONE,
        bool refreshThumbnails = true,
        int minStartDelayMs = 0);

private:

    void cancelUnstartedPendingLoads(const char* reason, const Glib::ustring& currentFile = Glib::ustring());

    IdleRegister idle_register;

    // Browser footer bar (matches editor's bottom toolbar layout)
    Gtk::ToggleButton* browserHideLp_;
    Gtk::Image* iBrowserLpShow_ = nullptr;  // C++-owned swap icons (never Gtk::manage)
    Gtk::Image* iBrowserLpHide_ = nullptr;
};
