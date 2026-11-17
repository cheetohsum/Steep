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
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtkmm.h>
#include <giomm.h>

#include "guiutils.h"
#include "motion.h"

class DirTreeView; // Forward declaration for subclass with hover interception
class FolderNameRenderer; // folder name with its optional strip of photos

class DirBrowser : public Gtk::Box
{
public:
    typedef sigc::signal<void, const Glib::ustring&, const Glib::ustring&> DirSelectionSignal;

private:

    Glib::RefPtr<Gtk::TreeStore> dirTreeModel;

    struct DirTreeColumns : public Gtk::TreeModelColumnRecord {
    public:
        Gtk::TreeModelColumn<Glib::ustring> filename;
        Gtk::TreeModelColumn<Glib::ustring> icon_name;
        Gtk::TreeModelColumn<Glib::ustring> dirname;
        Gtk::TreeModelColumn<Glib::RefPtr<Gio::FileMonitor> > monitor;
        Gtk::TreeModelColumn<Glib::ustring> photoCount;
        Gtk::TreeModelColumn<bool> childrenLoaded;
        // Folder previews: how many tiles the row reserves (0 = none) and
        // the strip once it is loaded.
        Gtk::TreeModelColumn<int> previewSlots;
        Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf> > previewStrip;
        // Encoded rating / flag / reject / label counts (see FolderStats)
        Gtk::TreeModelColumn<Glib::ustring> flagStats;

        DirTreeColumns()
        {
            add(icon_name);
            add(filename);
            add(dirname);
            add(monitor);
            add(photoCount);
            add(childrenLoaded);
            add(previewSlots);
            add(previewStrip);
            add(flagStats);
        }
    };

    DirTreeColumns dtColumns;
    Gtk::TreeViewColumn tvc;
    FolderNameRenderer* crt;


    DirTreeView *dirtree;
    Gtk::ScrolledWindow *scrolledwindow4;
    DirSelectionSignal dirSelectionSignal;

    void fillRoot ();

    Glib::ustring openfolder;
    Glib::ustring closedfolder;
    Glib::ustring icdrom;
    Glib::ustring ifloppy;
    Glib::ustring ihdd;
    Glib::ustring inetwork;
    Glib::ustring iremovable;

    bool expandSuccess;
    bool reuseLoadedDirs_;

#ifdef _WIN32
    unsigned int volumes;
public:
    void updateVolumes ();
    void updateDirTree  (const Gtk::TreeModel::iterator& iter);
    void updateDirTreeRoot  ();
private:
    void addRoot (char letter);
#endif
    void addDir (const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirname);
    Gtk::TreePath expandToDir (const Glib::ustring& dirName);
    Glib::ustring highlightDirInternal (const Glib::ustring& dirName, bool collapseTree);
    void updateDir (const Gtk::TreeModel::iterator& iter);
    void scheduleUpdateDir (const Gtk::TreeModel::iterator& iter);
    void flushPendingDirUpdates ();
    std::map<std::string, Gtk::TreeRowReference> pendingDirUpdates_;
    sigc::connection dirUpdateConn_;
    void countPhotosInChildren (const Gtk::TreeModel::iterator& parent);

    // Per-folder rating / flag / label summary chips
    struct ScanEntry {
        Glib::ustring dirname;
        Glib::ustring pathStr;
    };
    void scanFolderStats (std::vector<ScanEntry> entries);
    Gtk::CellRenderer* statsCR_ = nullptr;
    bool statsChipAt (int bx, int by, Gtk::TreeModel::Path* pathOut, int* chipOut, Gdk::Rectangle* rectOut);
    bool onQueryTooltip (int x, int y, bool keyboard, const Glib::RefPtr<Gtk::Tooltip>& tooltip);
    std::set<std::string> pendingStatDirs_;
    sigc::connection statRefreshConn_;
    sigc::connection flagsChangedConn_;
    void queueFolderStatsRefresh (const Glib::ustring& dirname);

    // Bumped whenever previews already queued must be dropped (setting
    // changed, tree rebuilt); workers compare it before every folder.
    std::atomic<int> previewGeneration_{0};
    void refreshFolderPreviews ();

    // The directory currently open in the catalog. The tree uses hover
    // selection, so the GTK selection follows the pointer and is dropped when
    // it leaves the tree; the open folder has to be tracked separately and
    // painted by the cell renderers.
    Glib::ustring currentDir_;
    Gtk::TreeRowReference currentRowRef_;

    static bool isSamePath (const Glib::ustring& a, const Glib::ustring& b);
    void setCurrentDir (const Glib::ustring& absDirPath, const Gtk::TreeModel::Path& path);
    bool isCurrentDirRow (const Gtk::TreeModel::iterator& iter) const;
    void paintCurrentDirBackground (Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& iter) const;

    IdleRegister idle_register;

    // Chevron expand/collapse indicators
    Glib::RefPtr<Gdk::Pixbuf> chevronRightPixbuf_;
    Glib::RefPtr<Gdk::Pixbuf> chevronDownPixbuf_;

    // Inline favorite star shown on the current (open) folder row
    Gtk::CellRendererPixbuf* starCR_ = nullptr;
    Glib::RefPtr<Gdk::Pixbuf> starFilledPixbuf_;
    Glib::RefPtr<Gdk::Pixbuf> starHollowPixbuf_;
    bool isCurrentDirFavorite () const;
    bool isDirFavorite (const Glib::ustring& dir) const;
    void toggleCurrentDirFavorite ();
    void toggleDirFavorite (const Glib::ustring& dir);

    // Row under the pointer: it alone shows the favorite star.
    Gtk::TreeModel::Path hoverRow_;

    // Right-click popover: folder previews on/off, their size and count
    // (sliders), and opening the clicked folder in the file manager.
    std::unique_ptr<Gtk::Popover> previewPopover_;
    Gtk::Switch* previewSwitch_ = nullptr;
    Gtk::Switch* previewRatedSwitch_ = nullptr;
    Gtk::Scale* previewSizeScale_ = nullptr;
    Gtk::Scale* previewCountScale_ = nullptr;
    bool previewPopoverSyncing_ = false;
    sigc::connection previewApplyConn_;
    Glib::ustring previewMenuDir_;
    void showPreviewMenu (GdkEventButton* event);
    void buildPreviewPopover ();
    void schedulePreviewApply ();
    static void setPreviewOptions (bool show, int count, int size, bool preferRated);
    static void openInFileManager (const Glib::ustring& dir);

    // Chevron rotation animation
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> chevronFrames_;
    std::map<std::string, int> chevronAnimFrame_;
    std::map<std::string, steep::motion::Handle> chevronAnims_;
    void animateChevron(const std::string& pathStr, bool expanding);

    // Child row reveal (clip-in on expand)
    std::string revealParentPath_;
    steep::motion::Handle revealAnim_;
    void animateReveal(const std::string& parentPathStr);
    void clearReveal();

    // Expand/collapse height animation
    steep::motion::Handle expandAnim_;

    // Hover thumbnail popup
    Gtk::Window* hoverPopup_;
    Gtk::Box* hoverBox_;
    Gtk::Image* hoverImages_[5];
    Gtk::TreeModel::Path hoveredPath_;
    sigc::connection hoverTimer_;
    bool popupVisible_;
    int hoverSession_;

    bool onMotionNotify(GdkEventMotion* event);
    bool onLeaveNotify(GdkEventCrossing* event);
    void showHoverPopup(const Gtk::TreeModel::Path& path);
    void hideHoverPopup();
    void loadHoverThumbnails(const Glib::ustring& dirname, int session);

public:
    DirBrowser ();
    ~DirBrowser() override;

    void fillDirTree ();
    void on_sort_column_changed() const;
    void browseForFolder ();
    void row_expanded   (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path);
    void row_collapsed  (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path);
    void row_activated  (const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    void file_changed   (const Glib::RefPtr<Gio::File>& file, const Glib::RefPtr<Gio::File>& other_file, Gio::FileMonitorEvent event_type, const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirName);
    void open           (const Glib::ustring& dirName, const Glib::ustring& fileName = "", bool collapseTree = true); // goes to dir "dirName" and selects file "fileName"
    void selectDir      (Glib::ustring dir);
    void highlightDir   (const Glib::ustring& dirName, bool collapseTree = false);
    Glib::ustring getHighlightedDir () const;

    DirSelectionSignal dirSelected () const;

    // Emitted (by any DirBrowser instance) when the favorites list changes
    // via the inline star, so every PlacesBrowser can refresh.
    static sigc::signal<void>& favoritesChanged ();

    // Emitted when the folder preview options change (Preferences).
    static sigc::signal<void>& previewSettingsChanged ();

    // Emitted after ratings / flags / colour labels / trash change for
    // files in a folder, so the tree refreshes that folder's chips.
    static sigc::signal<void, const Glib::ustring&>& folderFlagsChanged ();
};

inline DirBrowser::DirSelectionSignal DirBrowser::dirSelected () const
{
    return dirSelectionSignal;
}
