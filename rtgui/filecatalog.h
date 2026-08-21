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
#include <memory>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <giomm.h>

#include "exiffiltersettings.h"
#include "exportpanel.h"
#include "filebrowser.h"
#include "fileselectionchangelistener.h"
#include "fileselectionlistener.h"
#include "filterpanel.h"
#include "previewloader.h"
#include "threadutils.h"

#include "rtengine/noncopyable.h"

class FilePanel;
class CoarsePanel;
class MyHScale;
class RTSurface;
class ToolBar;

/*
 * Class:
 *   - handling the list of file (add/remove them)
 *   - handling the thumbnail toolbar,
 *   - monitoring the directory (for any change)
 */
class FileCatalog final : public Gtk::Box,
    public PreviewLoaderListener,
    public FilterPanelListener,
    public FileBrowserListener,
    public ExportPanelListener,
    public rtengine::NonCopyable
{
public:
    typedef sigc::slot<void, const Glib::ustring&> DirSelectionSlot;

private:
    struct FileMonitorInfo {
        FileMonitorInfo(const Glib::RefPtr<Gio::FileMonitor> &file_monitor, const Glib::ustring &file_path) :
            fileMonitor(file_monitor), filePath(file_path) {}
        Glib::RefPtr<Gio::FileMonitor> fileMonitor;
        Glib::ustring filePath;
    };

    /**
     * @brief Data that is used to reset the file catalog contents if user presses Y,X or SHIFT+F3/F4
     */
    struct DirectoryResetInfo {
        DirectoryResetInfo() :
            recursive(false) {}
        bool          recursive;
        Glib::ustring directory;
    };

    FilePanel* filepanel;
    Gtk::Box* hBox;
    Glib::ustring selectedDirectory;
    DirectoryResetInfo resetData;
    std::atomic<int> selectedDirectoryId;
    int readyQuickPreviewWarmDirectoryId_;
    bool enabled;
    bool inTabMode;  // Tab mode has e.g. different progress bar handling
    Glib::ustring imageToSelect_fname;
    std::string imageToSelect_key;
    Glib::ustring refImageForOpen_fname; // Next/previous for Editor's perspective
    eRTNav actionNextPrevious;

    FileSelectionListener* listener;
    FileSelectionChangeListener* fslistener;
    ImageAreaToolListener* iatlistener;
    DirSelectionSlot selectDir;

    Gtk::Box* buttonBar;
    MyScrolledToolbar* stb_;
    Gtk::Box* hbToolBar1;
    Gtk::Box* filterBar_ = nullptr;   // contents of the filter revealer

    Gtk::Box* fltrRankbox;
    Gtk::Box* fltrLabelbox;
    Gtk::Box* fltrVbox1;

    // Color label hover-expand animation
    Gtk::Box* colorLabelContainer_;
    Gtk::DrawingArea* colorLabelSummary_;
    Gtk::Revealer* colorLabelRevealer_;
    bool colorLabelExpanded_;
    double colorSummaryOpacity_;
    sigc::connection colorFadeConn_;
    sigc::connection colorCollapseDelay_;

    Gtk::Box* fltrEditedBox;
    Gtk::Box* fltrRecentlySavedBox;
    Gtk::Box* fltrVbox2;

    Gtk::Revealer* filterRevealer_;
    Gtk::ToggleButton* bFilterToggle_;

    Gtk::Separator* vSepiLeftPanel;

    Gtk::ToggleButton* tbLeftPanel_1;
    Gtk::ToggleButton* tbRightPanel_1;
    Gtk::ToggleButton* bFilterClear;
    Gtk::ToggleButton* bUnRanked;
    Gtk::ToggleButton* bRank[5];
    Gtk::ToggleButton* bUnCLabeled;
    Gtk::ToggleButton* bCLabel[5];//color label
    Gtk::ToggleButton* bEdited[2];
    Gtk::ToggleButton* bRecentlySaved[2];
    Gtk::ToggleButton* bPicked;
    Gtk::ToggleButton* bRejected;
    Gtk::ToggleButton* bUnflagged;

    // Rejects view: shows only rejected photos, with mass delete
    Gtk::ToggleButton* bRejectsView = nullptr;
    Gtk::Box* rejectsButtonBox = nullptr;
    bool rejectsViewActive_ = false;

    // Right-click menu on the rejected-filter button
    Gtk::Popover* rejectsPopover_ = nullptr;
    Gtk::CheckButton* hideRejectsCheck_ = nullptr;
    Gtk::Scale* cullFocusScale_ = nullptr;
    Gtk::Scale* cullExposureScale_ = nullptr;
    Gtk::Button* cullUndoButton_ = nullptr;
    void showRejectsPopover ();
    void rejectsViewToggled ();
    void deleteAllRejects ();
    Gtk::ToggleButton* bTrash;
    Gtk::ToggleButton* bNotTrash;
    Gtk::ToggleButton* bOriginal;
    Gtk::ToggleButton* bRecursive;
    Gtk::ToggleButton* categoryButtons[23];
    Gtk::ToggleButton* exifInfo;
    sigc::connection bCateg[23];
    Gtk::Image* iFilterClear, *igFilterClear;
    Gtk::Image* iranked[5], *igranked[5], *iUnRanked, *igUnRanked;
    Gtk::Image* iCLabeled[5], *igCLabeled[5], *iUnCLabeled, *igUnCLabeled;
    Gtk::Image* iEdited[2], *igEdited[2];
    Gtk::Image* iRecentlySaved[2], *igRecentlySaved[2];
    Gtk::Image *iPicked, *igPicked, *iRejected, *igRejected, *iUnflagged, *igUnflagged;
    Gtk::Image *iTrashShowEmpty, *iTrashShowFull;
    Gtk::Image *iNotTrash, *iOriginal;
    Gtk::Image *iRefreshWhite, *iRefreshRed;
    Gtk::Image *iLeftPanel_1_Show, *iLeftPanel_1_Hide, *iRightPanel_1_Show, *iRightPanel_1_Hide;
    Gtk::Image *iQueryClear;

    Gtk::Entry* BrowsePath;
    Gtk::Button* buttonBrowsePath;

    // Name of whatever the browser is currently showing, centred in the toolbar
    Gtk::Label* dirTitleLabel_ = nullptr;
    Gtk::EventBox* dirTitleEvtBox_ = nullptr;
    Gtk::Menu* titleMenu_ = nullptr;              // right-click menu on the title
    Gtk::CheckMenuItem* titlePinItem_ = nullptr;
    Gtk::CheckMenuItem* titleFullPathItem_ = nullptr;
    bool titleMenuBlock_ = false;
    Glib::ustring titleName_;      // short display name (folder name / "Album: X")
    Glib::ustring titleFullPath_;  // full path when showing a real directory, else empty
    static Glib::ustring directoryTitle (const Glib::ustring& directory);
    void setBrowserTitle (const Glib::ustring& text, const Glib::ustring& tooltip);
    void applyBrowserTitle ();

    Gtk::Entry* Query;
    Gtk::Button* buttonQueryClear;

    double hScrollPos[18];
    double vScrollPos[18];
    int lastScrollPos;

    Gtk::Box* trashButtonBox;

    MyHScale* zoomSlider_;
    // Magnifier toggle revealing the path/search/reload cluster
    Gtk::ToggleButton* searchToggle_ = nullptr;
    Gtk::Revealer* searchRevealer_ = nullptr;

    // Metadata (EXIF) filters popover in the browser filter bar
    Gtk::MenuButton* metadataFilterButton_ = nullptr;
    Gtk::Popover* metadataFilterPopover_ = nullptr;
public:
    void embedMetadataFilterPanel (Gtk::Widget* panel);
private:

    // Debounced thumbnail-zoom application
    sigc::connection zoomSliderApplyConn_;
    int lastAppliedZoomHeight_ = -1;

    RTImage* progressImage;
    Gtk::Label* progressLabel;

    MyMutex dirEFSMutex;
    ExifFilterSettings dirEFS;
    ExifFilterSettings currentEFS;
    bool hasValidCurrentEFS;

    FilterPanel* filterPanel;
    ExportPanel* exportPanel;

    int previewsToLoad;
    int previewsLoaded;


    std::vector<Glib::ustring> fileNameList;
    std::unordered_set<std::string> queuedPreviewKeys_;
    std::set<Glib::ustring> editedFiles;
    guint modifierKey; // any modifiers held when rank button was pressed

    std::vector<FileMonitorInfo> dirMonitors;

    IdleRegister idle_register;

    // Preview batching: collect thumbnails from background PreviewLoader
    // threads and process them in batches to avoid thousands of individual
    // idle callbacks that saturate the GTK main loop.
    std::mutex previewBatchMutex_;
    std::deque<std::pair<int, FileBrowserEntry*>> pendingPreviews_;
    std::vector<std::pair<int, FileBrowserEntry*>> previewChunkScratch_;
    std::vector<FileBrowserEntry*> entriesToAddScratch_;
    bool previewBatchPending_ = false;
    std::atomic<bool> previewBatchFirstDrainPending_{false};
    std::atomic<unsigned> previewBatchPauseDepth_{0};
    bool directoryScanComplete_ = true;
    bool previewsFinishedPending_ = false;
    bool previewsFinishRetryQueued_ = false;
    bool folderLoadTimingActive_ = false;
    bool folderLoadFirstPreviewLogged_ = false;
    int folderLoadNextPreviewMilestone_ = 0;
    std::chrono::steady_clock::time_point folderLoadStart_;
    guint navigationBenchmarkTimeoutId_ = 0;
    sigc::connection filmstripCenterConnection_;
    bool navigationBenchmarkStarted_ = false;
    int navigationBenchmarkRemaining_ = 0;
    int navigationBenchmarkIntervalMs_ = 0;
    std::size_t navigationBenchmarkIndex_ = 0;
    eRTNav navigationBenchmarkDirection_ = NAV_NEXT;
    bool navigationBenchmarkRawOnly_ = false;
    bool processPendingPreviews_();
    void schedulePreviewsFinishedRetry_(int dir_id, unsigned int delayMs);
    void startFolderLoadTiming_();
    void stopFolderLoadTiming_();
    void logFolderLoadTiming_(const char* stage) const;
    void scheduleNavigationBenchmark_();
    void scheduleNavigationBenchmarkStep_(int dirId);
    bool runNavigationBenchmarkStep_(int dirId);
    sigc::connection reparseDirectoryConn_;
    bool reparseDirectoryQueued_ = false;
    void scheduleReparseDirectory_();

    bool earlySelectDone_ = false;
    std::unordered_set<std::string> albumWhitelist_;
    bool inAlbumMode_;
    // key -> (path, anchor path) of session-pinned partner files
    std::map<std::string, std::pair<Glib::ustring, Glib::ustring>> pinnedPartners_;
    Glib::ustring savedDirectory_;

    // Global scope: show matches for the active filter from every folder
    // steep knows about (favorites, recents, current), streamed in via the
    // album-mode display rails.
    Gtk::ToggleButton* bGlobalScope_ = nullptr;
    Gtk::DrawingArea* globalScopeGlobe_ = nullptr;
    std::shared_ptr<RTSurface> globalScopeGlobeSurface_;
    sigc::connection globalToggleConn_;
    sigc::connection globalRescanConn_;
    sigc::connection globalGlobeAnimConn_;
    bool globalScopeActive_ = false;
    bool inGlobalStart_ = false;
    bool globalGlobeScanning_ = false;
    bool globalGlobeSettling_ = false;
    double globalGlobeAngle_ = 0.0;
    double globalGlobeAngularVelocity_ = 0.0;
    double globalGlobeSettleTarget_ = 0.0;
    std::chrono::steady_clock::time_point globalGlobeLastTick_;
    int globalScanGen_ = 0;
    std::string globalLastScanKey_;
    std::shared_ptr<std::atomic<bool>> globalAliveToken_;
    std::shared_ptr<std::atomic<bool>> globalScanCancel_;
    bool drawGlobalScopeGlobe(const Cairo::RefPtr<Cairo::Context>& cr);
    bool tickGlobalScopeGlobe();
    void startGlobalScopeGlobe();
    void settleGlobalScopeGlobe();
    void globalScopeToggled();
    void startGlobalScan();
    void scheduleGlobalRescan();
    // Coalesces the filter + viewport refresh that follows each batch of
    // global-scan matches into one pass per burst.
    void scheduleGlobalScanRefresh();
    sigc::connection globalScanRefreshConn_;
    // Deferred, coalesced filter application (cancelled when the directory is
    // closed, so filtering a folder that is being discarded costs nothing).
    void scheduleFilterApply();
    sigc::connection filterApplyConn_;
    void resetGlobalScopeQuiet();

    // Filetype filter dropdown
    Gtk::MenuButton* filetypeButton_;
    Gtk::Popover* filetypePopover_;
    Gtk::Box* filetypeBox_;
    Gtk::CheckButton* filetypeAllCheck_;
    Gtk::CheckButton* filetypeDefaultCheck_;
    std::map<std::string, Gtk::CheckButton*> filetypeChecks_;
    std::set<std::string> knownFiletypes_;       // all types seen (uppercase)
    std::set<std::string> selectedFiletypes_;    // active types (uppercase); empty = all
    bool filetypeBlockSignals_ = false;
    bool filetypeUpdateQueued_ = false;
    void updateFiletypeFilter();
    void scheduleFiletypeFilterUpdate_();
    void flushFiletypeFilterUpdate_();
    void onFiletypeCheckToggled(const std::string& filetype);
    void onFiletypeAllToggled();
    void onFiletypeDefaultToggled();
    void updateFiletypeDefaultCheck_();
    void resetFiletypeFilter();

    // Color label hover-expand handlers
    bool onColorLabelChildEnter(GdkEventCrossing* event);
    bool onColorLabelChildLeave(GdkEventCrossing* event);
    bool onColorLabelFadeTick();

    void addAndOpenFile (const Glib::ustring& fname);
    void addFile (const Glib::ustring& fName);
    void addFiles (const std::vector<Glib::ustring>& fNames);
    void addFiles (std::vector<Glib::ustring>&& fNames);
    void addFiles (std::vector<Glib::ustring>&& fNames, std::vector<std::string>&& fNameKeys);
    std::vector<Glib::ustring> getFileList(std::vector<Glib::RefPtr<Gio::File>> *dirs_explored = nullptr);
    void refreshDirectoryMonitors(const std::vector<Glib::RefPtr<Gio::File>> &dirs_to_monitor);
    void trashChanged ();

public:
    // Snapshot of the active filter state (used by the double-exposure
    // picker to inherit the browser's filtering).
    BrowserFilter getFilter ();

    // thumbnail browsers
    FileBrowser* fileBrowser;

    CoarsePanel* coarsePanel;
    ToolBar* toolBar;

    FileCatalog (CoarsePanel* cp, ToolBar* tb, FilePanel* filepanel);
    ~FileCatalog() override;
    void dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile);
    void closeDir    ();
    void refreshEditedState (const std::set<Glib::ustring>& efiles);

    // previewloaderlistener interface
    void previewReady (int dir_id, FileBrowserEntry* fdn) override;
    void previewReadyBatch (PreviewLoaderListener::PreviewReadyBatch&& entries) override;
    void previewsFinished (int dir_id) override;
    // called asynchronously from the main event loop
    void previewsFinishedUI(int dir_id);
    void pausePreviewBatchProcessing();
    void resumePreviewBatchProcessing();

    void _refreshProgressBar();

    void setInspector(Inspector* inspector)
    {
        if (fileBrowser) {
            fileBrowser->setInspector(inspector);
        }
    }
    void disableInspector()
    {
        if (fileBrowser) {
            fileBrowser->disableInspector();
        }
    }
    void enableInspector()
    {
        if (fileBrowser) {
            fileBrowser->enableInspector();
        }
    }

    // filterpanel interface
    void exifFilterChanged () override;

    // exportpanel interface
    void exportRequested() override;

    Glib::ustring lastSelectedDir ()
    {
        return selectedDirectory;
    }
    void setEnabled (bool e);   // if not enabled, it does not open image
    void enableTabMode(bool enable);  // sets progress bar

    // accessors for FileBrowser
    void redrawAll ();
    void refreshThumbImages ();
    // Re-applies the browser-toolbar filter (used when returning from the
    // editor so the filmstrip's filter state doesn't linger) and resumes
    // any paused preview loading.
    void reapplyBrowserFilter ();
    void refreshHeight ();

    void filterApplied() override;
    void openRequested(const std::vector<Thumbnail*>& tbe, eRTNav preloadDirectionHint = NAV_NONE) override;
    void deleteRequested(const std::vector<FileBrowserEntry*>& tbe, bool inclBatchProcessed, bool onlySelected) override;
    void copyMoveRequested(const std::vector<FileBrowserEntry*>& tbe, bool moveRequested) override;
    void developRequested(const std::vector<FileBrowserEntry*>& tbe, bool fastmode) override;
    void renameRequested(const std::vector<FileBrowserEntry*>& tbe) override;
    void selectionChanged(const std::vector<Thumbnail*>& tbe) override;
    void clearFromCacheRequested(const std::vector<FileBrowserEntry*>& tbe, bool leavenotrace) override;
    void quickActionProgress(const Glib::ustring& text, double progress) override;
    bool transientEditPreviewRequested(
        const Glib::ustring& filename,
        const rtengine::procparams::ProcParams* params,
        bool restore) override;
    bool isInTabMode() const override;

    void emptyTrash ();
    bool trashIsEmpty ();

    void setFileSelectionListener (FileSelectionListener* l)
    {
        listener = l;
    }
    void setFileSelectionChangeListener (FileSelectionChangeListener* l)
    {
        fslistener = l;
    }
    void setImageAreaToolListener (ImageAreaToolListener* l)
    {
        iatlistener = l;
    }
    void setDirSelector (const DirSelectionSlot& selectDir);

    void setFilterPanel (FilterPanel* fpanel);
    void setExportPanel (ExportPanel* expanel);
    void exifInfoButtonToggled();
    void filterToggled();
    void categoryButtonToggled (Gtk::ToggleButton* b, bool isMouseClick);
    void showRecursiveToggled();
    bool capture_event(GdkEventButton* event);
    void filterChanged ();
    void setAlbumWhitelist (const std::set<std::string>& whitelist);
    void showAlbumFiles (const Glib::ustring& albumName, const std::vector<Glib::ustring>& files);
    void exitAlbumMode ();
    bool isInAlbumMode () const { return inAlbumMode_; }

    // Surface a double-exposure partner in the strip directly after the
    // image it composites into, and open it in the editor. The pin lasts
    // for the session or until the user browses to a different folder.
    void openPartnerForEditing (const Glib::ustring& path, const Glib::ustring& anchorPath);
    bool isInRealAlbumMode () const { return inAlbumMode_ && !globalScopeActive_; }

    void saveResetState ();
    bool restoreResetState ();

    void on_realize() override;
    void reparseDirectory ();
    void _openImage (const std::vector<Thumbnail*>& tmb, eRTNav preloadDirectionHint = NAV_NONE);

    void zoomIn ();
    void zoomOut ();
    void zoomSliderChanged ();

    void buttonBrowsePathPressed ();
    bool BrowsePath_key_pressed (GdkEventKey *event);
    void buttonQueryClearPressed ();
    void executeQuery ();
    bool Query_key_pressed(GdkEventKey *event);
    void updateFBQueryTB (bool singleRow);
    void updateFBToolBarVisibility (bool showFilmStripToolBar);

    void tbLeftPanel_1_toggled ();
    void tbLeftPanel_1_visible (bool visible);
    // Top of the thumbnail area, so callers can align an overlay below the
    // browser's own toolbar instead of on top of it
    Gtk::Widget* getThumbnailArea()
    {
        return hBox;
    }
    void tbRightPanel_1_toggled ();
    void tbRightPanel_1_visible (bool visible);

    void openNextImage ()
    {
        fileBrowser->openNextImage();
    }
    void openPrevImage ()
    {
        fileBrowser->openPrevImage();
    }
    void selectImage (Glib::ustring fname, bool clearFilters);
    void openNextPreviousEditorImage (Glib::ustring fname, eRTNav nextPrevious);

    bool handleShortcutKey (GdkEventKey* event);
    bool handleShortcutKeyRelease(GdkEventKey *event);

    bool CheckSidePanelsVisibility();
    void toggleSidePanels();
    void toggleLeftPanel();
    void toggleRightPanel();

    void showToolBar();
    void hideToolBar();

    // Browser title (folder/album name centred in the toolbar). The static
    // signal carries (display text, tooltip) and fires whenever the title or
    // its display options change, so pinned copies elsewhere can follow.
    Glib::ustring getBrowserTitleText() const;
    Glib::ustring getBrowserTitleTooltip() const;
    static sigc::signal<void, const Glib::ustring&, const Glib::ustring&>& browserTitleChanged();

    // Filetype filter state — shared between browser and editor filter bars
    const std::set<std::string>& getKnownFiletypes() const { return knownFiletypes_; }
    const std::set<std::string>& getSelectedFiletypes() const { return selectedFiletypes_; }
    void setSelectedFiletypes(const std::set<std::string>& sel);
    bool isCurrentFiletypeFilterDefault() const;
    void setCurrentFiletypeFilterAsDefault(bool active);
    void updateFiletypeButtonLabel();

    void on_dir_changed (const Glib::RefPtr<Gio::File>& file, const Glib::RefPtr<Gio::File>& other_file, Gio::FileMonitorEvent event_type, bool internal);

};

inline void FileCatalog::setDirSelector (const FileCatalog::DirSelectionSlot& selectDir)
{
    this->selectDir = selectDir;
}
