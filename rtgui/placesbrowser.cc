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
#include "placesbrowser.h"

#include <algorithm>
#include <iostream>
#include <thread>

#include "guiutils.h"
#include "multilangmgr.h"
#include "options.h"
#include "rtimage.h"
#include "toolpanel.h"

class PlacesTreeView : public Gtk::TreeView {
public:
    PlacesTreeView() : Glib::ObjectBase("PlacesTreeView") {
        add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    }

    std::function<void(const Gtk::TreeModel::Path&)> onHoverChanged;
protected:
    bool on_motion_notify_event(GdkEventMotion* event) override {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* col;
        int cx, cy;
        if (get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path, col, cx, cy)) {
            if (hoveredPath_.empty() || path != hoveredPath_) {
                hoveredPath_ = path;
                if (onHoverChanged) onHoverChanged(path);
                queue_draw();
            }
        } else if (!hoveredPath_.empty()) {
            hoveredPath_ = Gtk::TreeModel::Path();
            if (onHoverChanged) onHoverChanged(hoveredPath_);
            queue_draw();
        }
        return Gtk::TreeView::on_motion_notify_event(event);
    }
    bool on_leave_notify_event(GdkEventCrossing* event) override {
        if (!hoveredPath_.empty()) {
            hoveredPath_ = Gtk::TreeModel::Path();
            if (onHoverChanged) onHoverChanged(hoveredPath_);
            queue_draw();
        }
        return Gtk::TreeView::on_leave_notify_event(event);
    }
private:
    Gtk::TreeModel::Path hoveredPath_;
};

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <shlwapi.h>
#include <shlobj.h>
#endif

PlacesBrowser::PlacesBrowser ()
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);
    set_name("PlacesBrowserWidget");
    set_size_request(-1, 300);

    // The compact section header; favoriting lives in the context menu.
    Gtk::Box* headerBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    headerBar->set_name("PlacesHeader");
    auto* headerIcon = Gtk::manage(new RTImage("star-small", Gtk::ICON_SIZE_SMALL_TOOLBAR));
    headerIcon->set_tooltip_text(M("MAIN_FRAME_PLACES"));
    headerIcon->set_margin_start(6);
    headerIcon->set_margin_end(4);
    headerBar->pack_start(*headerIcon, Gtk::PACK_SHRINK);
    pack_start(*headerBar, Gtk::PACK_SHRINK, 0);

    scrollw = Gtk::manage (new Gtk::ScrolledWindow ());
    scrollw->set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrollw->set_overlay_scrolling(false);
    pack_start (*scrollw, Gtk::PACK_EXPAND_WIDGET);

    treeView = Gtk::manage (new PlacesTreeView ());
    treeView->set_name("PlacesBrowserTree");

    // Hover highlighting: set_hover_selection(true) is required for motion
    // events to reach the bin_window (and thus the virtual override).
    treeView->set_hover_selection(true);
    treeView->onHoverChanged = [this](const Gtk::TreeModel::Path& path) {
        hoveredPath_ = path;
    };

    scrollw->add (*treeView);

    // Right-click context menu for places
    rightClickMenu = Gtk::manage(new Gtk::Menu());
    addMenuItem = Gtk::manage(new Gtk::MenuItem(M("MAIN_FRAME_PLACES_ADD")));
    addMenuItem->signal_activate().connect(sigc::mem_fun(*this, &PlacesBrowser::addPressed));
    rightClickMenu->append(*addMenuItem);
    removeMenuItem = Gtk::manage(new Gtk::MenuItem(M("MAIN_FRAME_PLACES_DEL")));
    removeMenuItem->signal_activate().connect(sigc::mem_fun(*this, &PlacesBrowser::delPressed));
    rightClickMenu->append(*removeMenuItem);
    hideDriveMenuItem = Gtk::manage(new Gtk::MenuItem(M("MAIN_FRAME_PLACES_HIDE_DRIVE")));
    hideDriveMenuItem->signal_activate().connect(sigc::mem_fun(*this, &PlacesBrowser::hideSelectedDrive));
    rightClickMenu->append(*hideDriveMenuItem);
    hiddenDrivesSeparator = Gtk::manage(new Gtk::SeparatorMenuItem());
    rightClickMenu->append(*hiddenDrivesSeparator);
    hiddenDrivesMenuItem = Gtk::manage(new Gtk::MenuItem(M("MAIN_FRAME_PLACES_HIDDEN_DRIVES")));
    hiddenDrivesMenu = Gtk::manage(new Gtk::Menu());
    hiddenDrivesMenuItem->set_submenu(*hiddenDrivesMenu);
    rightClickMenu->append(*hiddenDrivesMenuItem);
    rightClickMenu->show_all();
    treeView->signal_button_press_event().connect(sigc::mem_fun(*this, &PlacesBrowser::onButtonPress), false);

    placesModel = Gtk::ListStore::create (placesColumns);
    treeView->set_model (placesModel);
    treeView->set_headers_visible (false);

    Gtk::TreeView::Column *iviewcol = Gtk::manage (new Gtk::TreeView::Column (M("MAIN_FRAME_PLACES")));
    Gtk::CellRendererText *labelCR  = Gtk::manage (new Gtk::CellRendererText());
    labelCR->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
    Gtk::CellRendererText *countCR = Gtk::manage (new Gtk::CellRendererText());
    countCR->property_foreground() = "#888888";
    countCR->property_xalign() = 1.0;

    iviewcol->pack_start (*labelCR, true);
    iviewcol->pack_end (*countCR, false);

    // cell_data_funcs for data binding + hover highlighting
    iviewcol->set_cell_data_func(*labelCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        textCR->property_text() = (*iter)[placesColumns.label];
        auto rowPath = placesModel->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        textCR->property_cell_background_set() = hovered;
        if (hovered) textCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });
    iviewcol->set_cell_data_func(*countCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        textCR->property_text() = (*iter)[placesColumns.photoCount];
        auto rowPath = placesModel->get_path(iter);
        bool hovered = !hoveredPath_.empty() && rowPath == hoveredPath_;
        textCR->property_cell_background_set() = hovered;
        if (hovered) textCR->property_cell_background() = Glib::ustring("#3a3f4b");
    });

    treeView->append_column (*iviewcol);

    treeView->set_row_separator_func (sigc::mem_fun(*this, &PlacesBrowser::rowSeparatorFunc));

    vm = Gio::VolumeMonitor::get();

    vm->signal_mount_changed().connect (sigc::mem_fun(*this, &PlacesBrowser::mountChanged));
    vm->signal_mount_added().connect (sigc::mem_fun(*this, &PlacesBrowser::mountChanged));
    vm->signal_mount_removed().connect (sigc::mem_fun(*this, &PlacesBrowser::mountChanged));
    vm->signal_volume_changed().connect (sigc::mem_fun(*this, &PlacesBrowser::volumeChanged));
    vm->signal_volume_added().connect (sigc::mem_fun(*this, &PlacesBrowser::volumeChanged));
    vm->signal_volume_removed().connect (sigc::mem_fun(*this, &PlacesBrowser::volumeChanged));
    vm->signal_drive_connected().connect (sigc::mem_fun(*this, &PlacesBrowser::driveChanged));
    vm->signal_drive_disconnected().connect (sigc::mem_fun(*this, &PlacesBrowser::driveChanged));
    vm->signal_drive_changed().connect (sigc::mem_fun(*this, &PlacesBrowser::driveChanged));

    // NOTE: Do NOT connect selection signal_changed — hover_selection triggers
    // it on every mouse move. Instead, selectionChanged is called explicitly
    // from onButtonPress for real clicks only.

    show_all ();
}

// For drive letter comparison
bool compareMountByRoot (Glib::RefPtr<Gio::Mount> a, Glib::RefPtr<Gio::Mount> b)
{
    return a->get_root()->get_parse_name() < b->get_root()->get_parse_name();
}

namespace
{

// Persist options right away so hide/favorite changes survive an unclean
// exit (options are otherwise only written on shutdown).
void persistOptionsQuiet()
{
    try {
        Options::save();
    } catch (Options::Error& error) {
        std::cerr << "Failed to save options: " << error.get_msg() << std::endl;
    }
}

Glib::ustring hiddenPlaceId(const char* kind, Glib::ustring value)
{
#ifdef _WIN32
    value = value.lowercase();
#endif
    return Glib::ustring(kind) + ":" + value;
}

Glib::ustring volumeHiddenId(const Glib::RefPtr<Gio::Volume>& volume)
{
    Glib::ustring value = volume->get_uuid();
    if (value.empty()) {
        value = volume->get_name();
    }
    return hiddenPlaceId("volume", value);
}

}

void PlacesBrowser::refreshPlacesList ()
{
    placesModel->clear ();
    hiddenDriveLabels_.clear();

    const auto& options = App::get().options();
    // append favorites
    for (size_t i = 0; i < options.favoriteDirs.size(); i++) {
        Glib::RefPtr<Gio::File> fav = Gio::File::create_for_path (options.favoriteDirs[i]);

        if (fav && fav->query_exists()) {
            try {
                if (auto info = fav->query_info ()) {
                    // Show "Parent/Folder" so user can tell nesting context
                    Glib::ustring displayLabel = info->get_display_name();
                    auto parent = fav->get_parent();
                    if (parent) {
                        try {
                            auto parentInfo = parent->query_info();
                            if (parentInfo) {
                                displayLabel = parentInfo->get_display_name()
                                    + "/" + displayLabel;
                            }
                        } catch (...) {}
                    }
                    Gtk::TreeModel::Row newrow = *(placesModel->append());
                    newrow[placesColumns.label] = displayLabel;
                    newrow[placesColumns.icon]  = info->get_icon ();
                    newrow[placesColumns.root]  = fav->get_parse_name ();
                    newrow[placesColumns.type]  = 5;
                    newrow[placesColumns.rowSeparator] = false;
                }
            } catch(Gio::Error&) {}
        }
    }

    // append home ("My documents" on Windows) and pictures directories.
    // These carry a hiddenId so they can be hidden like drives.
    bool specialSeparatorAdded = false;
    auto appendSpecialDir = [this, &options, &specialSeparatorAdded](
            const Glib::ustring& dirPath, const Glib::ustring& hiddenId) {
        Glib::RefPtr<Gio::File> hfile = Gio::File::create_for_path (dirPath);

        if (!hfile || !hfile->query_exists()) {
            return;
        }

        try {
            if (auto info = hfile->query_info ()) {
                hiddenDriveLabels_[hiddenId] = info->get_display_name ();

                if (std::find(options.hiddenDriveRoots.begin(), options.hiddenDriveRoots.end(), hiddenId)
                        != options.hiddenDriveRoots.end()) {
                    return;
                }

                if (!specialSeparatorAdded && !placesModel->children().empty()) {
                    Gtk::TreeModel::Row separator = *(placesModel->append());
                    separator[placesColumns.rowSeparator] = true;
                    specialSeparatorAdded = true;
                }

                Gtk::TreeModel::Row newrow = *(placesModel->append());
                newrow[placesColumns.label] = info->get_display_name ();
                newrow[placesColumns.icon]  = info->get_icon ();
                newrow[placesColumns.root]  = hfile->get_parse_name ();
                newrow[placesColumns.type]  = 4;
                newrow[placesColumns.rowSeparator] = false;
                newrow[placesColumns.hiddenId] = hiddenId;
            }
        } catch (Gio::Error&) {}
    };

    appendSpecialDir(userHomeDir(), "special:home");
    appendSpecialDir(userPicturesDir(), "special:pictures");

    // scan all drives
    bool driveSeparatorAdded = false;
    auto appendDrive = [this, &options, &driveSeparatorAdded](
            const Glib::ustring& label,
            const Glib::RefPtr<Gio::Icon>& icon,
            const Glib::ustring& root,
            int type,
            const Glib::ustring& hiddenId) {
        hiddenDriveLabels_[hiddenId] = label;

        if (std::find(options.hiddenDriveRoots.begin(), options.hiddenDriveRoots.end(), hiddenId)
                != options.hiddenDriveRoots.end()) {
            return;
        }

        if (!driveSeparatorAdded && !placesModel->children().empty()) {
            Gtk::TreeModel::Row separator = *(placesModel->append());
            separator[placesColumns.rowSeparator] = true;
            driveSeparatorAdded = true;
        }

        Gtk::TreeModel::Row newrow = *(placesModel->append());
        newrow[placesColumns.label] = label;
        newrow[placesColumns.icon] = icon;
        newrow[placesColumns.root] = root;
        newrow[placesColumns.type] = type;
        newrow[placesColumns.rowSeparator] = false;
        newrow[placesColumns.hiddenId] = hiddenId;
    };

    std::vector<Glib::RefPtr<Gio::Drive> > drives = vm->get_connected_drives ();

    for (size_t j = 0; j < drives.size (); j++) {
        std::vector<Glib::RefPtr<Gio::Volume> > volumes = drives[j]->get_volumes ();

        if (volumes.empty()) {
            appendDrive(
                drives[j]->get_name(),
                drives[j]->get_icon(),
                "",
                3,
                hiddenPlaceId("drive", drives[j]->get_name()));
        }

        for (size_t i = 0; i < volumes.size (); i++) {
            Glib::RefPtr<Gio::Mount> mount = volumes[i]->get_mount ();

            if (mount) { // placesed volumes
                appendDrive(
                    mount->get_name(),
                    mount->get_icon(),
                    mount->get_root()->get_parse_name(),
                    1,
                    volumeHiddenId(volumes[i]));
            } else { // unplacesed volumes
                appendDrive(
                    volumes[i]->get_name(),
                    volumes[i]->get_icon(),
                    "",
                    2,
                    volumeHiddenId(volumes[i]));
            }
        }
    }

    // volumes not belonging to drives
    std::vector<Glib::RefPtr<Gio::Volume> > volumes = vm->get_volumes ();

    for (size_t i = 0; i < volumes.size (); i++) {
        if (!volumes[i]->get_drive ()) {
            Glib::RefPtr<Gio::Mount> mount = volumes[i]->get_mount ();

            if (mount) { // placesed volumes
                appendDrive(
                    mount->get_name(),
                    mount->get_icon(),
                    mount->get_root()->get_parse_name(),
                    1,
                    volumeHiddenId(volumes[i]));
            } else { // unplacesed volumes
                appendDrive(
                    volumes[i]->get_name(),
                    volumes[i]->get_icon(),
                    "",
                    2,
                    volumeHiddenId(volumes[i]));
            }
        }
    }

    // places not belonging to volumes
    // (Drives in Windows)
    std::vector<Glib::RefPtr<Gio::Mount> > mounts = vm->get_mounts ();

#ifdef _WIN32
    // on Windows, it's usual to sort by drive letter, not by name
    std::sort (mounts.begin(), mounts.end(), compareMountByRoot);
#endif

    for (size_t i = 0; i < mounts.size (); i++) {
        if (!mounts[i]->get_volume ()) {
            const auto root = mounts[i]->get_root()->get_parse_name();
            appendDrive(
                mounts[i]->get_name(),
                mounts[i]->get_icon(),
                root,
                1,
                hiddenPlaceId("mount", root));
        }
    }

    rebuildHiddenDrivesMenu();
    startPhotoCount();
}

bool PlacesBrowser::rowSeparatorFunc (const Glib::RefPtr<Gtk::TreeModel>& model, const Gtk::TreeModel::iterator& iter)
{

    return iter->get_value (placesColumns.rowSeparator);
}

void PlacesBrowser::mountChanged (const Glib::RefPtr<Gio::Mount>& m)
{
    GThreadLock lock;
    refreshPlacesList ();
}

void PlacesBrowser::volumeChanged (const Glib::RefPtr<Gio::Volume>& m)
{
    GThreadLock lock;
    refreshPlacesList ();
}

void PlacesBrowser::driveChanged (const Glib::RefPtr<Gio::Drive>& m)
{
    GThreadLock lock;
    refreshPlacesList ();
}

void PlacesBrowser::selectionChanged ()
{

    Glib::RefPtr<Gtk::TreeSelection> selection = treeView->get_selection();
    Gtk::TreeModel::iterator iter = selection->get_selected();

    if (iter) {
        if (iter->get_value (placesColumns.type) == 2) {
            std::vector<Glib::RefPtr<Gio::Volume> > volumes = vm->get_volumes ();

            for (size_t i = 0; i < volumes.size(); i++)
                if (volumes[i]->get_name () == iter->get_value (placesColumns.label)) {
                    volumes[i]->mount ();
                    break;
                }
        } else if (iter->get_value (placesColumns.type) == 3) {
            std::vector<Glib::RefPtr<Gio::Drive> > drives = vm->get_connected_drives ();

            for (size_t i = 0; i < drives.size(); i++)
                if (drives[i]->get_name () == iter->get_value (placesColumns.label)) {
                    drives[i]->poll_for_media ();
                    break;
                }
        } else if (selectDir) {
            selectDir (iter->get_value (placesColumns.root));
        }
    }
}

void PlacesBrowser::dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile)
{
    lastSelectedDir = dirname;

    // Invalidate cache for this directory (contents may have changed)
    {
        std::lock_guard<std::mutex> lock(photoCountMutex_);
        photoCountCache_.erase(dirname);
    }
}

void PlacesBrowser::addPressed ()
{

    if (lastSelectedDir.empty()) {
        return;
    }

    auto& options = App::get().mut_options();
    // check if the dirname is already in the list. If yes, return.
    for (size_t i = 0; i < options.favoriteDirs.size(); i++)
        if (options.favoriteDirs[i] == lastSelectedDir) {
            return;
        }

    // append
    Glib::RefPtr<Gio::File> hfile = Gio::File::create_for_path (lastSelectedDir);

    if (hfile && hfile->query_exists()) {
        try {
            if (auto info = hfile->query_info ()) {
                options.favoriteDirs.push_back (hfile->get_parse_name ());
                persistOptionsQuiet();
                refreshPlacesList ();
            }
        } catch(Gio::Error&) {}
    }
}

void PlacesBrowser::delPressed ()
{
    // Use the row captured at right-click time — the hover-selection tree's
    // live selection is unreliable once the menu has closed.
    if (!contextMenuIsFavorite_ || contextMenuRoot_.empty()) {
        return;
    }

    auto& options = App::get().mut_options();
    std::vector<Glib::ustring>::iterator i = std::find (options.favoriteDirs.begin(), options.favoriteDirs.end(), contextMenuRoot_);

    if (i != options.favoriteDirs.end()) {
        options.favoriteDirs.erase (i);
        persistOptionsQuiet();
    }

    refreshPlacesList ();
}

void PlacesBrowser::hideSelectedDrive()
{
    // Use the row captured at right-click time (see delPressed).
    if (contextMenuHiddenId_.empty()) {
        return;
    }

    auto& hiddenDrives = App::get().mut_options().hiddenDriveRoots;
    if (std::find(hiddenDrives.begin(), hiddenDrives.end(), contextMenuHiddenId_) == hiddenDrives.end()) {
        hiddenDrives.push_back(contextMenuHiddenId_);
        persistOptionsQuiet();
    }

    refreshPlacesList();
}

void PlacesBrowser::restoreHiddenDrive(Glib::ustring hiddenId)
{
    auto& hiddenDrives = App::get().mut_options().hiddenDriveRoots;
    hiddenDrives.erase(
        std::remove(hiddenDrives.begin(), hiddenDrives.end(), hiddenId),
        hiddenDrives.end());
    persistOptionsQuiet();
    refreshPlacesList();
}

void PlacesBrowser::rebuildHiddenDrivesMenu()
{
    for (auto* child : hiddenDrivesMenu->get_children()) {
        hiddenDrivesMenu->remove(*child);
    }

    for (const auto& hiddenId : App::get().options().hiddenDriveRoots) {
        auto label = hiddenId;
        const auto labelIt = hiddenDriveLabels_.find(hiddenId);
        if (labelIt != hiddenDriveLabels_.end()) {
            label = labelIt->second;
        } else {
            const auto separator = hiddenId.find(':');
            if (separator != Glib::ustring::npos && separator + 1 < hiddenId.size()) {
                label = hiddenId.substr(separator + 1);
            }
        }

        auto* item = Gtk::manage(new Gtk::MenuItem(label));
        item->set_tooltip_text(hiddenId);
        item->signal_activate().connect(
            sigc::bind(sigc::mem_fun(*this, &PlacesBrowser::restoreHiddenDrive), hiddenId));
        hiddenDrivesMenu->append(*item);
    }

    hiddenDrivesMenu->show_all();
}

bool PlacesBrowser::onButtonPress (GdkEventButton* event)
{
    // Left-click: manually select the row and trigger selectionChanged
    // (we can't use signal_changed because hover_selection fires it on every motion)
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        Gtk::TreeModel::Path path;
        if (treeView->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path)) {
            treeView->get_selection()->select(path);
            selectionChanged();
            return true;
        }
    }

    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        Gtk::TreeModel::Path path;
        bool onRow = treeView->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path);
        bool isFavorite = false;
        bool isHideable = false;

        contextMenuHiddenId_.clear();
        contextMenuRoot_.clear();
        contextMenuIsFavorite_ = false;

        if (onRow) {
            treeView->get_selection()->select(path);
            auto iter = placesModel->get_iter(path);
            if (iter) {
                const int type = iter->get_value(placesColumns.type);
                isFavorite = type == 5;
                isHideable = type >= 1 && type <= 4
                    && !iter->get_value(placesColumns.hiddenId).empty();

                // Capture the target now: the menu actions run after the
                // menu closes, when hover selection may have moved on.
                contextMenuIsFavorite_ = isFavorite;
                contextMenuRoot_ = iter->get_value(placesColumns.root);
                if (isHideable) {
                    contextMenuHiddenId_ = iter->get_value(placesColumns.hiddenId);
                }
            }
        }

        rebuildHiddenDrivesMenu();
        const bool hasHiddenDrives = !App::get().options().hiddenDriveRoots.empty();

        // Show "Add" if we have a directory selected in the dir browser
        addMenuItem->set_visible(!lastSelectedDir.empty());
        // Show "Remove" only for favorites
        removeMenuItem->set_visible(isFavorite);
        hideDriveMenuItem->set_visible(isHideable);
        hiddenDrivesSeparator->set_visible(isHideable || hasHiddenDrives);
        hiddenDrivesMenuItem->set_visible(hasHiddenDrives);

        if (!lastSelectedDir.empty() || isFavorite || isHideable || hasHiddenDrives) {
            rightClickMenu->popup(event->button, event->time);
            return true;
        }
    }
    return false;
}

Glib::ustring PlacesBrowser::userHomeDir ()
{
#ifdef _WIN32

    // get_home_dir crashes on some Windows configurations,
    // so we rather use the safe native functions here.
    WCHAR pathW[MAX_PATH];
    if (SHGetSpecialFolderPathW (NULL, pathW, CSIDL_PERSONAL, false)) {

        char pathA[MAX_PATH];
        if (WideCharToMultiByte (CP_UTF8, 0, pathW, -1, pathA, MAX_PATH, 0, 0)) {

            return Glib::ustring (pathA);
        }
    }

    return Glib::ustring ("C:\\");

#else

    return Glib::get_home_dir ();

#endif
}

Glib::ustring PlacesBrowser::userPicturesDir ()
{
#ifdef _WIN32

    // get_user_special_dir crashes on some Windows configurations,
    // so we rather use the safe native functions here.
    WCHAR pathW[MAX_PATH];
    if (SHGetSpecialFolderPathW (NULL, pathW, CSIDL_MYPICTURES, false)) {

        char pathA[MAX_PATH];
        if (WideCharToMultiByte (CP_UTF8, 0, pathW, -1, pathA, MAX_PATH, 0, 0)) {

            return Glib::ustring (pathA);
        }
    }

    return Glib::ustring ("C:\\");

#else

    return Glib::get_user_special_dir (G_USER_DIRECTORY_PICTURES);

#endif
}

int PlacesBrowser::countPhotosInDir (const Glib::ustring& dirPath)
{
    int count = 0;

    try {
        Glib::Dir dir(dirPath);

        const auto& exts = App::get().options().parsedExtensionsSet;

        for (auto it = dir.begin(); it != dir.end(); ++it) {
            const Glib::ustring& fname = *it;
            auto dotpos = fname.find_last_of('.');

            if (dotpos != Glib::ustring::npos) {
                Glib::ustring ext = fname.substr(dotpos + 1).lowercase();

                if (exts.count(ext.raw())) {
                    ++count;
                }
            }
        }
    } catch (...) {
        // Directory may not be accessible
    }

    return count;
}

void PlacesBrowser::startPhotoCount ()
{
    if (countingActive_.exchange(true)) {
        return; // already counting
    }

    // Collect paths from model
    struct DirEntry {
        Glib::ustring path;
        Gtk::TreeModel::Path treePath;
    };
    std::vector<DirEntry> dirs;

    for (auto it = placesModel->children().begin(); it != placesModel->children().end(); ++it) {
        Glib::ustring root = (*it)[placesColumns.root];

        if (!root.empty() && !(*it)[placesColumns.rowSeparator]) {
            dirs.push_back({root, placesModel->get_path(it)});
        }
    }

    auto model = placesModel;
    auto cols = &placesColumns;
    auto activeFlag = &countingActive_;
    auto cache = &photoCountCache_;
    auto mtx = &photoCountMutex_;

    std::thread([dirs, model, cols, activeFlag, cache, mtx]() {
        for (const auto& entry : dirs) {
            int count = -1;

            {
                std::lock_guard<std::mutex> lock(*mtx);
                auto it = cache->find(entry.path);

                if (it != cache->end()) {
                    count = it->second;
                }
            }

            if (count < 0) {
                count = countPhotosInDir(entry.path);

                std::lock_guard<std::mutex> lock(*mtx);
                (*cache)[entry.path] = count;
            }

            Glib::ustring countStr = count > 0 ? "(" + std::to_string(count) + ")" : "";
            Gtk::TreeModel::Path tp = entry.treePath;

            Glib::signal_idle().connect_once([model, cols, tp, countStr]() {
                auto it = model->get_iter(tp);

                if (it) {
                    (*it)[cols->photoCount] = countStr;
                }
            });
        }

        activeFlag->store(false);
    }).detach();
}
