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
#include <mutex>

#include <gtkmm.h>

#include <giomm.h>

class PlacesTreeView; // Forward declaration

class PlacesBrowser :
    public Gtk::Box
{
public:
    typedef sigc::slot<void, const Glib::ustring&> DirSelectionSlot;

private:

    class PlacesColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        Gtk::TreeModelColumn<Glib::RefPtr<Gio::Icon> >   icon;
        Gtk::TreeModelColumn<Glib::ustring>              label;
        Gtk::TreeModelColumn<Glib::ustring>              root;
        Gtk::TreeModelColumn<int>                        type;
        Gtk::TreeModelColumn<bool>                       rowSeparator;
        Gtk::TreeModelColumn<Glib::ustring>              photoCount;
        Gtk::TreeModelColumn<Glib::ustring>              hiddenId;
        PlacesColumns()
        {
            add(icon);
            add(label);
            add(root);
            add(type);
            add(rowSeparator);
            add(photoCount);
            add(hiddenId);
        }
    };
    PlacesColumns            placesColumns;
    Gtk::ScrolledWindow*    scrollw;
    PlacesTreeView*         treeView;

    // Hover highlighting via PlacesTreeView subclass + cell_data_func
    Gtk::TreeModel::Path hoveredPath_;
    Glib::RefPtr<Gtk::ListStore> placesModel;
    Glib::RefPtr<Gio::VolumeMonitor> vm;
    DirSelectionSlot             selectDir;
    Glib::ustring                lastSelectedDir;
    Gtk::Menu*                   rightClickMenu;
    Gtk::MenuItem*               addMenuItem;
    Gtk::MenuItem*               removeMenuItem;
    Gtk::MenuItem*               hideDriveMenuItem;
    Gtk::SeparatorMenuItem*      hiddenDrivesSeparator;
    Gtk::MenuItem*               hiddenDrivesMenuItem;
    Gtk::Menu*                   hiddenDrivesMenu;
    std::map<Glib::ustring, Glib::ustring> hiddenDriveLabels_;
    // Row captured when the context menu was opened. The tree uses hover
    // selection, so the live selection can move to whatever row lies under
    // the pointer while the menu closes — menu actions must not re-read it.
    Glib::ustring                contextMenuHiddenId_;
    Glib::ustring                contextMenuRoot_;
    bool                         contextMenuIsFavorite_ = false;
    std::map<Glib::ustring, int> photoCountCache_;
    std::mutex photoCountMutex_;
    std::atomic<bool> countingActive_{false};

public:

    PlacesBrowser ();

    void setDirSelector (const DirSelectionSlot& selectDir);
    void dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile);

    void refreshPlacesList ();
    void mountChanged (const Glib::RefPtr<Gio::Mount>& m);
    void volumeChanged (const Glib::RefPtr<Gio::Volume>& v);
    void driveChanged (const Glib::RefPtr<Gio::Drive>& d);
    bool rowSeparatorFunc (const Glib::RefPtr<Gtk::TreeModel>& model, const Gtk::TreeModel::iterator& iter);
    void selectionChanged ();
    void addPressed ();
    void delPressed ();
    void hideSelectedDrive();
    void restoreHiddenDrive(Glib::ustring hiddenId);
    void rebuildHiddenDrivesMenu();
    bool onButtonPress (GdkEventButton* event);
    void startPhotoCount ();
    static int countPhotosInDir (const Glib::ustring& dirPath);

public:

    static Glib::ustring userHomeDir ();
    static Glib::ustring userPicturesDir ();

};

inline void PlacesBrowser::setDirSelector (const PlacesBrowser::DirSelectionSlot& selectDir)
{
    this->selectDir = selectDir;
}
