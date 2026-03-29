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

#include <thread>

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
#include "thumbimageupdater.h"
#include "thumbnail.h"
#include "windows/rtwindow.h"

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif // _WIN32

FilePanel::FilePanel () : parent(nullptr), error(0)
{
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
    clearPreloadCache();
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
    if (!parent) {
        return false;
    }

    // Check if it's already open BEFORE loading the file
    if (App::get().options().tabbedUI && parent->selectEditorPanel(thm->getFileName())) {
        thm->decreaseRef();
        return true;
    }

    // Check if the image is already being opened and set the image loading status if it is not
    bool loading = thm->imageLoad( true );
    if( !loading ) {
        return false;
    }

    pendingLoadMutex.lock();
    pendingLoad *pl = new pendingLoad();
    pl->complete = false;
    pl->pc = nullptr;
    pl->thm = thm;
    pl->epanel = nullptr;
    pendingLoads.push_back(pl);
    pendingLoadMutex.unlock();

    // Pause preview loading to free IO for the full image load.
    // Resumes in imageLoaded() after the editor opens.
    // Thumbnail upgrades continue in the background at normal priority.
    previewLoader->pause();

    // Don't signal stop here — aborting the processing thread mid-OpenMP
    // causes access violations. Let it finish naturally; close() in open()
    // will handle cleanup on a background thread.
    const auto& opts = App::get().options();

    // Switch to editor view immediately so the user sees the transition
    // while the image loads in the background.
    if (opts.tabbedUI) {
#ifdef _WIN32
        int winGdiHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        if (winGdiHandles == 0 || winGdiHandles > 6500)
#endif
        {
            EditorPanel* ep = Gtk::manage(new EditorPanel());
            parent->addEditorPanel(ep, thm->getFileName());
            ep->setAspect();
            pl->epanel = ep;
        }
    } else {
        parent->SetEditorCurrent();
    }

    // Show the clicked image's thumbnail INSTANTLY as a preview.
    // Try cached Pixbuf first (free), fall back to processThumbImage (~50-200ms).
    if (!opts.tabbedUI && parent->epanel) {
        Glib::RefPtr<Gdk::Pixbuf> quickPb;
        double displayScale = 1.0;

        // Fast path: cached Pixbuf from a previous filmstrip render
        double cachedScale = 1.0;
        quickPb = thm->getCachedPixbuf(cachedScale);

        if (quickPb) {
            int fullW = 0, fullH = 0;
            thm->getOriginalSize(fullW, fullH);
            if (fullW > 0) {
                displayScale = static_cast<double>(fullW) / quickPb->get_width();
            }
        } else {
            // Slow path: generate thumbnail now (~50-200ms for QUICK, ~200-400ms for FULL)
            double thumbScale = 1.0;
            rtengine::IImage8* thumbImg = thm->processThumbImage(thm->getProcParams(), 400, thumbScale);
            if (thumbImg) {
                int tw = thumbImg->getWidth(), th = thumbImg->getHeight();
                if (tw > 0 && th > 0 && thumbImg->getData()) {
                    auto pb = Gdk::Pixbuf::create_from_data(
                        thumbImg->getData(), Gdk::COLORSPACE_RGB, false, 8, tw, th, tw * 3);
                    quickPb = pb->copy();
                    int fullW = 0, fullH = 0;
                    thm->getOriginalSize(fullW, fullH);
                    if (fullW > 0) {
                        displayScale = static_cast<double>(fullW) / tw;
                    }
                }
                delete thumbImg;
            }
        }

        if (quickPb) {
            parent->epanel->setQuickPreview(quickPb, displayScale);
        }
    }

    // Check preload cache before starting a new background load
    rtengine::InitialImage* cachedImg = nullptr;
    {
        std::lock_guard<std::mutex> lock(preloadMutex_);
        auto it = preloadCache_.find(std::string(thm->getFileName()));
        if (it != preloadCache_.end()) {
            cachedImg = it->second;
            preloadCache_.erase(it);
            // Ownership transferred to caller — no decreaseRef
        }
    }

    if (cachedImg) {
        // Cache hit — open immediately without expensive I/O
        bool opened = false;
        const auto& opts2 = App::get().options();
        if (opts2.tabbedUI) {
            if (pl->epanel) {
                pl->epanel->open(pl->thm, cachedImg);
                if (!(opts2.multiDisplayMode > 0)) {
                    parent->set_title_decorated(pl->thm->getFileName());
                }
                opened = true;
            } else {
                // GDI limit — release the cached image
                cachedImg->decreaseRef();
                thm->decreaseRef();
            }
        } else {
            parent->epanel->open(pl->thm, cachedImg);
            parent->set_title_decorated(pl->thm->getFileName());
            opened = true;
        }

        parent->setProgress(0.);
        parent->setProgressStr("");

        // Clean up pendingLoad entry
        pendingLoadMutex.lock();
        for (auto it2 = pendingLoads.begin(); it2 != pendingLoads.end(); ++it2) {
            if (*it2 == pl) {
                pendingLoads.erase(it2);
                break;
            }
        }
        pendingLoadMutex.unlock();
        delete pl;

        previewLoader->resume();
        thm->imageLoad(false);

        if (opened) {
            preloadAdjacent(thm->getFileName());
        }

        return true;
    }

    ProgressConnector<rtengine::InitialImage*> *ld = new ProgressConnector<rtengine::InitialImage*>();
    ld->startFunc (sigc::bind(sigc::ptr_fun(&rtengine::InitialImage::load), thm->getFileName (), thm->getType() == FT_Raw, &error, parent->getProgressListener()),
                   sigc::bind(sigc::mem_fun(*this, &FilePanel::imageLoaded), thm, ld) );
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
    pendingLoadMutex.lock();

    // find our place in the array and mark the entry as complete
    for (unsigned int i = 0; i < pendingLoads.size(); i++) {
        if (pendingLoads[i]->thm == thm) {
            pendingLoads[i]->pc = pc;
            pendingLoads[i]->complete = true;
            break;
        }
    }

    const auto& options = App::get().options();
    bool decThumbRef = false;

    // The purpose of the pendingLoads vector is to open tabs in the same order as the loads where initiated. It has no effect on single editor mode.
    while (pendingLoads.size() > 0 && pendingLoads.front()->complete) {
        pendingLoad *pl = pendingLoads.front();

        if (pl->pc->returnValue()) {
            if (options.tabbedUI) {
                // Editor panel was pre-created in fileSelected() for
                // immediate view switch; just open the image in it now.
                if (pl->epanel) {
                    pl->epanel->open(pl->thm, pl->pc->returnValue());

                    if (!(options.multiDisplayMode > 0)) {
                        parent->set_title_decorated(pl->thm->getFileName());
                    }
                } else {
                    // GDI handle limit was hit — panel wasn't created
                    Glib::ustring msg_ = Glib::ustring("<b>") + M("MAIN_MSG_CANNOTLOAD") + " \"" + escapeHtmlChars(thm->getFileName()) + "\" .\n" + M("MAIN_MSG_TOOMANYOPENEDITORS") + "</b>";
                    Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
                    msgd.run ();
                    decThumbRef = true;
                }
            } else {
                // View was already switched in fileSelected(); just open.
                parent->epanel->open(pl->thm, pl->pc->returnValue() );
                parent->set_title_decorated(pl->thm->getFileName());
            }
        } else {
            Glib::ustring msg_ = Glib::ustring("<b>") + M("MAIN_MSG_CANNOTLOAD") + " \"" + escapeHtmlChars(thm->getFileName()) + "\" .\n</b>";
            Gtk::MessageDialog msgd (*parent, msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.run ();
            decThumbRef = true;
        }
        delete pl->pc;

        {
            GThreadLock lock; // Acquiring the GUI... not sure that it's necessary, but it shouldn't harm
            parent->setProgress(0.);
            parent->setProgressStr("");
        }

        pendingLoads.erase(pendingLoads.begin());
        delete pl;
    }

    pendingLoadMutex.unlock();

    // Resume preview loading now that the editor image is loaded
    previewLoader->resume();

    thm->imageLoad( false );
    if (decThumbRef) {
        thm->decreaseRef();
    }

    // Preload adjacent images for faster filmstrip navigation
    preloadAdjacent(thm->getFileName());

    return false; // MUST return false from idle function
}

void FilePanel::clearPreloadCache()
{
    std::lock_guard<std::mutex> lock(preloadMutex_);
    for (auto& kv : preloadCache_) {
        kv.second->decreaseRef();
    }
    preloadCache_.clear();
}

void FilePanel::preloadAdjacent(const Glib::ustring& fname)
{
    if (!fileCatalog || !fileCatalog->fileBrowser) return;

    // Full RAW preload for N±3 (instant editor opening)
    auto entries = fileCatalog->fileBrowser->getAdjacentEntries(fname, 3);

    // Build set of filenames that should be in the full-image cache
    std::set<std::string> keep;
    for (const auto& e : entries) {
        keep.insert(std::string(e.fname));
    }

    // Evict stale entries and find what needs loading
    std::vector<FileBrowser::AdjacentEntry> toLoad;
    {
        std::lock_guard<std::mutex> lock(preloadMutex_);

        for (auto it = preloadCache_.begin(); it != preloadCache_.end(); ) {
            if (keep.find(it->first) == keep.end()) {
                it->second->decreaseRef();
                it = preloadCache_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& e : entries) {
            if (preloadCache_.find(std::string(e.fname)) == preloadCache_.end()) {
                toLoad.push_back(e);
            }
        }
    }

    for (const auto& entry : toLoad) {
        Glib::ustring loadFname = entry.fname;
        bool isRaw = entry.isRaw;

        std::thread([this, loadFname, isRaw]() {
            int err = 0;
            rtengine::InitialImage* img = rtengine::InitialImage::load(loadFname, isRaw, &err, nullptr);
            if (img && !err) {
                std::lock_guard<std::mutex> lock(preloadMutex_);
                auto it = preloadCache_.find(std::string(loadFname));
                if (it != preloadCache_.end()) {
                    img->decreaseRef();
                } else {
                    preloadCache_[std::string(loadFname)] = img;
                }
            }
        }).detach();
    }

    // Thumbnail preload for N±5 (ensures filmstrip previews are processed)
    fileCatalog->fileBrowser->refreshAdjacentThumbnails(fname, 5);
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
