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

#include <map>
#include <mutex>
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
    bool addBatchQueueJobs(const std::vector<BatchQueueEntry*>& entries) override;

    void optionsChanged         ();
    bool imageLoaded( Thumbnail* thm, ProgressConnector<rtengine::InitialImage*> * );

    bool handleShortcutKey (GdkEventKey* event);
    bool handleShortcutKeyRelease(GdkEventKey *event);
    void updateTPVScrollbar (bool hide);
    void updateToolPanelToolLocations(
        const std::vector<Glib::ustring> &favorites, bool cloneFavoriteTools);

    // Fade browser content without affecting the left sidebar
    void setContentOpacity (double opacity);

    // Returns sidebar insets for queue overlay positioning
    void getQueueOverlayInsets (int& left, int& top, int& right) const;
    void closeAlbumView ();  // close album view + deselect sidebar
    void openSelectedInEditor ();  // open the selected browser thumbnail in editor

    // Left panel visibility for sync with editor sidebar
    bool isLeftPanelVisible() const { return browserHideLp_ && browserHideLp_->get_active(); }
    void setLeftPanelVisible(bool visible) { if (browserHideLp_ && browserHideLp_->get_active() != visible) browserHideLp_->set_active(visible); }

private:
    void on_NB_switch_page(Gtk::Widget* page, guint page_num);

    PlacesBrowser* placesBrowser;
    RecentBrowser* recentBrowser;
    AlbumBrowser* albumBrowser_;

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
        ProgressConnector<rtengine::InitialImage*> *pc;
        Thumbnail *thm;
        EditorPanel *epanel; // pre-created panel (tabbed mode only)
    };
    MyMutex pendingLoadMutex;
    std::vector<struct pendingLoad*> pendingLoads;

    // Adjacent-image preload cache (N±3 around the current selection).
    // Hit on open → skip expensive RAW I/O; miss → start a background load.
    std::mutex preloadMutex_;
    std::map<std::string, rtengine::InitialImage*> preloadCache_;
    void clearPreloadCache();
    void preloadAdjacent(const Glib::ustring& fname);

    int error;

    IdleRegister idle_register;

    // Browser footer bar (matches editor's bottom toolbar layout)
    Gtk::ToggleButton* browserHideLp_;
    Gtk::Image* iBrowserLpShow_;
    Gtk::Image* iBrowserLpHide_;
};
