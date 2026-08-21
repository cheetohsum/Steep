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
#include <string>
#include <vector>

#include <gtkmm.h>
#include <giomm.h>

#include "guiutils.h"

class DirTreeView; // Forward declaration for subclass with hover interception

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

        DirTreeColumns()
        {
            add(icon_name);
            add(filename);
            add(dirname);
            add(monitor);
            add(photoCount);
            add(childrenLoaded);
        }
    };

    DirTreeColumns dtColumns;
    Gtk::TreeViewColumn tvc;
    Gtk::CellRendererText crt;


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
    void countPhotosInChildren (const Gtk::TreeModel::iterator& parent);

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
    void toggleCurrentDirFavorite ();

    // Chevron rotation animation
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> chevronFrames_;
    std::map<std::string, int> chevronAnimFrame_;
    std::map<std::string, bool> chevronAnimExpanding_;
    sigc::connection chevronAnimConn_;
    void startChevronAnim();

    // Child row reveal (fade-in on expand)
    std::string revealParentPath_;
    double revealAlpha_ = 1.0;

    // Expand/collapse animation
    sigc::connection expandAnimConn_;
    double expandAnimFraction_ = 0.0;
    int expandAnimStartH_ = 0;
    int expandAnimTargetH_ = 0;
    bool expandAnimExpanding_ = true;

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
};

inline DirBrowser::DirSelectionSignal DirBrowser::dirSelected () const
{
    return dirSelectionSignal;
}
