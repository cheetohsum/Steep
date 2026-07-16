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
#include "dirbrowser.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include "rtengine/rt_math.h"

#include "cachemanager.h"
#include "guiutils.h"
#include "rtimage.h"
#include "rtsurface.h"
#include "multilangmgr.h"
#include "options.h"
#include "thumbnail.h"

#include <thread>

// Subclass TreeView to intercept motion/leave events BEFORE the base class
// handler (which returns TRUE when hover_selection is enabled, consuming events).
// The Glib::ObjectBase("DirTreeView") call registers a new GType so gtkmm
// installs class closures that dispatch to our virtual function overrides.
class DirTreeView : public Gtk::TreeView {
public:
    DirTreeView() : Glib::ObjectBase("DirTreeView") {}

    std::function<bool(GdkEventMotion*)> onMotion;
    std::function<bool(GdkEventCrossing*)> onLeave;

    // Clip-reveal state: set by DirBrowser when expanding
    std::string revealParentPath;
    double revealFraction = 1.0;

protected:
    bool on_motion_notify_event(GdkEventMotion* event) override {
        if (onMotion) onMotion(event);
        return Gtk::TreeView::on_motion_notify_event(event);
    }
    bool on_leave_notify_event(GdkEventCrossing* event) override {
        if (onLeave) onLeave(event);
        return Gtk::TreeView::on_leave_notify_event(event);
    }
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override {
        if (!revealParentPath.empty() && revealFraction < 1.0) {
            Gtk::TreePath parentPath(revealParentPath);
            // Get the parent row's background area to find its bottom edge
            Gdk::Rectangle parentRect;
            get_background_area(parentPath, *get_column(0), parentRect);
            int revealTop = parentRect.get_y() + parentRect.get_height();

            // Find total children height by checking the last child
            auto parentIter = get_model()->get_iter(parentPath);
            int totalChildH = 0;
            if (parentIter && !parentIter->children().empty()) {
                auto lastChild = parentIter->children().end();
                --lastChild;
                Gdk::Rectangle lastRect;
                get_background_area(get_model()->get_path(lastChild), *get_column(0), lastRect);
                totalChildH = (lastRect.get_y() + lastRect.get_height()) - revealTop;
            }

            int revealH = static_cast<int>(totalChildH * revealFraction);
            int w = get_allocated_width();
            int h = get_allocated_height();

            cr->save();
            // Clip: everything above children area + revealed portion of children
            cr->rectangle(0, 0, w, revealTop + revealH);
            // Plus everything below the children area (other rows)
            if (revealTop + totalChildH < h) {
                cr->rectangle(0, revealTop + totalChildH, w, h - (revealTop + totalChildH));
            }
            cr->clip();
            bool result = Gtk::TreeView::on_draw(cr);
            cr->restore();
            return result;
        }
        return Gtk::TreeView::on_draw(cr);
    }
};

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif // _WIN32

namespace
{

std::vector<Glib::ustring> listSubDirs (const Glib::RefPtr<Gio::File>& dir, bool addHidden)
{
    std::vector<Glib::ustring> subDirs;

    try {

        const Glib::ustring dirPath = dir->get_path ();

        // CD-ROM with no disc inserted are reported, but do not exist.
        if (!Glib::file_test (dirPath, Glib::FILE_TEST_EXISTS)) {
            return subDirs;
        }

        auto enumerator = dir->enumerate_children ("standard::name,standard::type,standard::is-hidden");

        while (true) {
            try {
                auto file = enumerator->next_file ();
                if (!file) {
                    break;
                }

                const Glib::ustring fileName = file->get_name ();
                // The Windows GIO backend can occasionally omit standard::type even
                // when it was requested, and get_file_type() warns in that case.
                const bool isDir = file->has_attribute (G_FILE_ATTRIBUTE_STANDARD_TYPE)
                    ? file->get_file_type () == Gio::FILE_TYPE_DIRECTORY
                    : Glib::file_test (Glib::build_filename (dirPath, fileName), Glib::FILE_TEST_IS_DIR);

                if (!isDir) {
                    continue;
                }

                if (!addHidden
                        && file->has_attribute (G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
                        && file->is_hidden ()) {
                    continue;
                }

                subDirs.push_back (fileName);
            } catch (const Glib::Exception& exception) {

                if (rtengine::settings->verbose) {
                    std::cerr << exception.what().c_str() << std::endl;
                }

            }
        }

    } catch (const Glib::Exception& exception) {

        if (rtengine::settings->verbose) {
            std::cerr << "Failed to list subdirectories of \"" << dir->get_parse_name() << "\": " << exception.what () << std::endl;
        }

    }

    return subDirs;
}

}

DirBrowser::DirBrowser () : dirTreeModel(),
    dtColumns(),
    tvc(M("DIRBROWSER_FOLDERS")),

    openfolder("folder-open-small"),
    closedfolder("folder-closed-small"),
    icdrom("device-optical"),
    ifloppy("device-floppy"),
    ihdd("device-hdd"),
    inetwork("device-network"),
    iremovable("device-usb"),

    expandSuccess(false),
    reuseLoadedDirs_(false)
#ifdef _WIN32
    , volumes(0)
#endif
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);
    dirtree = Gtk::manage ( new DirTreeView() );
    scrolledwindow4 = Gtk::manage ( new Gtk::ScrolledWindow() );
    crt.property_ellipsize() = Pango::ELLIPSIZE_END;
    crt.property_max_width_chars() = 22;

//   dirtree->set_flags(Gtk::CAN_FOCUS);
    dirtree->set_headers_visible(false);
    dirtree->set_rules_hint(false);
    dirtree->set_reorderable(false);
    dirtree->set_enable_search(false);
    dirtree->set_show_expanders(false);
    dirtree->set_level_indentation(10);
    dirtree->set_hover_selection(true);
    dirtree->set_activate_on_single_click(true);
    scrolledwindow4->set_can_focus(true);
    scrolledwindow4->set_shadow_type(Gtk::SHADOW_NONE);
    scrolledwindow4->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scrolledwindow4->set_min_content_height(150);
    scrolledwindow4->property_window_placement().set_value(Gtk::CORNER_TOP_LEFT);
    scrolledwindow4->add(*dirtree);

    dirtree->set_name("DirBrowserTree");

    pack_start (*scrolledwindow4);
    dirtree->show ();
    scrolledwindow4->show ();

    // Hover thumbnail popup
    popupVisible_ = false;
    hoverSession_ = 0;
    hoverPopup_ = new Gtk::Window(Gtk::WINDOW_POPUP);
    hoverPopup_->set_type_hint(Gdk::WINDOW_TYPE_HINT_TOOLTIP);
    hoverPopup_->set_name("DirHoverPopup");

    hoverBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 3));
    for (int i = 0; i < 5; i++) {
        hoverImages_[i] = Gtk::manage(new Gtk::Image());
        hoverImages_[i]->set_size_request(48, 48);
        hoverImages_[i]->set_no_show_all(true);
        hoverBox_->pack_start(*hoverImages_[i], Gtk::PACK_SHRINK);
    }
    hoverPopup_->add(*hoverBox_);
    hoverBox_->show();

    // Hook into DirTreeView's virtual method overrides for motion/leave
    dirtree->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    dirtree->onMotion = [this](GdkEventMotion* event) { return onMotionNotify(event); };
    dirtree->onLeave = [this](GdkEventCrossing* event) { return onLeaveNotify(event); };

    // Hide popup on scroll
    scrolledwindow4->get_vadjustment()->signal_value_changed().connect([this]() {
        hideHoverPopup();
    });
}

DirBrowser::~DirBrowser()
{
    chevronAnimConn_.disconnect();
    hideHoverPopup();
    delete hoverPopup_;
    idle_register.destroy();
}

// Generate rotated chevron pixbufs: frame 0 = right (0°), frame count = down (90°)
static std::vector<Glib::RefPtr<Gdk::Pixbuf>> generateChevronFrames(int count)
{
    const int sz = 16;
    const double cx = sz / 2.0, cy = sz / 2.0;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> frames;
    frames.reserve(count + 1);
    for (int i = 0; i <= count; ++i) {
        auto surface = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, sz, sz);
        auto cr = Cairo::Context::create(surface);
        double angle = (M_PI / 2.0) * i / count; // 0 to 90°
        cr->translate(cx, cy);
        cr->rotate(angle);
        cr->translate(-cx, -cy);
        cr->set_source_rgba(0.6, 0.6, 0.6, 1.0);
        cr->set_line_width(1.5);
        cr->set_line_cap(Cairo::LINE_CAP_ROUND);
        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
        cr->move_to(6, 3.5);
        cr->line_to(10.5, 8);
        cr->line_to(6, 12.5);
        cr->stroke();
        frames.push_back(Gdk::Pixbuf::create(surface, 0, 0, sz, sz));
    }
    return frames;
}

void DirBrowser::fillDirTree ()
{
    //Create the Tree model:
    dirTreeModel = Gtk::TreeStore::create(dtColumns);
    dirTreeModel->set_sort_func(
        dtColumns.filename,
        [this](const Gtk::TreeModel::iterator& first, const Gtk::TreeModel::iterator& second) -> int
        {
            const Glib::ustring firstName = first ? first->get_value(dtColumns.filename) : Glib::ustring();
            const Glib::ustring secondName = second ? second->get_value(dtColumns.filename) : Glib::ustring();

            if (firstName == secondName) {
                return 0;
            }

            if (firstName.empty()) {
                return 1;
            }

            if (secondName.empty()) {
                return -1;
            }

            return firstName.raw().compare(secondName.raw()) < 0 ? -1 : 1;
        });
    dirtree->set_model (dirTreeModel);

    fillRoot ();

    // Generate chevron rotation frames (Cairo-drawn)
    chevronFrames_ = generateChevronFrames(6);
    chevronRightPixbuf_ = chevronFrames_.front();
    chevronDownPixbuf_ = chevronFrames_.back();

    // Chevron expand/collapse indicator
    Gtk::CellRendererPixbuf* chevronCR = Gtk::manage(new Gtk::CellRendererPixbuf());
    chevronCR->property_ypad() = 0;
    chevronCR->property_xpad() = 0;
    tvc.pack_start(*chevronCR, false);
    tvc.set_cell_data_func(*chevronCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* pbCR = static_cast<Gtk::CellRendererPixbuf*>(cr);
        auto path = dirTreeModel->get_path(iter);
        if (path.empty()) {
            pbCR->property_pixbuf() = chevronRightPixbuf_;
            return;
        }
        auto pathStr = path.to_string();
        auto it = chevronAnimFrame_.find(pathStr);
        if (it != chevronAnimFrame_.end()) {
            int frame = std::max(0, std::min(it->second, static_cast<int>(chevronFrames_.size()) - 1));
            pbCR->property_pixbuf() = chevronFrames_[frame];
        } else {
            bool expanded = dirtree->row_expanded(path);
            pbCR->property_pixbuf() = expanded ? chevronDownPixbuf_ : chevronRightPixbuf_;
        }
    });

    tvc.pack_start (crt);
    tvc.add_attribute(crt, "text", dtColumns.filename);

    Gtk::CellRendererText* countCR = Gtk::manage(new Gtk::CellRendererText());
    countCR->property_foreground() = "#888888";
    countCR->property_xalign() = 1.0;
    countCR->property_ypad() = 0;
    countCR->property_scale() = 0.8;
    countCR->property_xpad() = 8;
    tvc.pack_end(*countCR, false);
    tvc.add_attribute(*countCR, "text", dtColumns.photoCount);

    dirtree->append_column(tvc);

    const auto& options = App::get().options();
    tvc.set_sort_order(options.dirBrowserSortType);
    tvc.set_sort_column(dtColumns.filename);
    tvc.set_sort_indicator(true);
    tvc.set_clickable();

    dirTreeModel->set_sort_column(dtColumns.filename, options.dirBrowserSortType);

    crt.property_ypad() = 0;
    dirtree->signal_row_expanded().connect(sigc::mem_fun(*this, &DirBrowser::row_expanded));
    dirtree->signal_row_collapsed().connect(sigc::mem_fun(*this, &DirBrowser::row_collapsed));
    dirtree->signal_row_activated().connect(sigc::mem_fun(*this, &DirBrowser::row_activated));
    dirTreeModel->signal_sort_column_changed().connect(sigc::mem_fun(*this, &DirBrowser::on_sort_column_changed));
}

#ifdef _WIN32
void DirBrowser::addRoot (char letter)
{

    char volume[4];
    volume[0] = letter;
    strcpy (volume + 1, ":\\");

    Gtk::TreeModel::iterator root = dirTreeModel->append();
    root->set_value (dtColumns.filename, Glib::ustring(volume));
    root->set_value (dtColumns.dirname, Glib::ustring(volume));
    root->set_value (dtColumns.childrenLoaded, false);

    int type = GetDriveType (volume);

    if (type == DRIVE_CDROM) {
        root->set_value (dtColumns.icon_name, icdrom);
    } else if (type == DRIVE_REMOVABLE) {
        if (letter - 'A' < 2) {
            root->set_value (dtColumns.icon_name, ifloppy);
        } else {
            root->set_value (dtColumns.icon_name, iremovable);
        }
    } else if (type == DRIVE_REMOTE) {
        root->set_value (dtColumns.icon_name, inetwork);
    } else if (type == DRIVE_FIXED) {
        root->set_value (dtColumns.icon_name, ihdd);
    }

    Gtk::TreeModel::iterator child = dirTreeModel->append (root->children());
    child->set_value (dtColumns.filename, Glib::ustring("foo"));
    child->set_value (dtColumns.childrenLoaded, false);
}

void DirBrowser::updateDirTreeRoot ()
{

    for (Gtk::TreeModel::iterator i = dirTreeModel->children().begin(); i != dirTreeModel->children().end(); ++i) {
        updateDirTree (i);
    }
}

void DirBrowser::updateDirTree (const Gtk::TreeModel::iterator& iter)
{

    if (dirtree->row_expanded (dirTreeModel->get_path (iter))) {
        updateDir (iter);

        for (Gtk::TreeModel::iterator i = iter->children().begin(); i != iter->children().end(); ++i) {
            updateDirTree (i);
        }
    }
}

void DirBrowser::updateVolumes ()
{

    unsigned int nvolumes = GetLogicalDrives ();

    if (nvolumes != volumes) {
        GThreadLock lock;

        for (int i = 0; i < 32; i++)
            if (((volumes >> i) & 1) && !((nvolumes >> i) & 1)) { // volume i has been deleted
                for (Gtk::TreeModel::iterator iter = dirTreeModel->children().begin(); iter != dirTreeModel->children().end(); ++iter)
                    if (iter->get_value (dtColumns.filename).c_str()[0] - 'A' == i) {
                        dirTreeModel->erase (iter);
                        break;
                    }
            } else if (!((volumes >> i) & 1) && ((nvolumes >> i) & 1)) {
                addRoot ('A' + i);    // volume i has been added
            }

        volumes = nvolumes;
    }
}

int updateVolumesUI (void* br)
{
    (static_cast<DirBrowser*>(br))->updateVolumes ();
    return 1;
}

#endif

void DirBrowser::fillRoot ()
{

#ifdef _WIN32
    volumes = GetLogicalDrives ();

    for (int i = 0; i < 32; i++)
        if ((volumes >> i) & 1) {
            addRoot ('A' + i);
        }

    // since sigc++ is not thread safe, we have to use the glib function
    g_timeout_add (5000, updateVolumesUI, this);
#else
    Gtk::TreeModel::Row rootRow = *(dirTreeModel->append());
    rootRow[dtColumns.filename] = "/";
    rootRow[dtColumns.dirname] = "/";
    rootRow[dtColumns.childrenLoaded] = false;
    Gtk::TreeModel::Row childRow = *(dirTreeModel->append(rootRow.children()));
    childRow[dtColumns.filename] = "foo";
    childRow[dtColumns.childrenLoaded] = false;
#endif
}

void DirBrowser::on_sort_column_changed() const
{
    auto& options = App::get().mut_options();
    options.dirBrowserSortType = tvc.get_sort_order();
}

void DirBrowser::browseForFolder ()
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::FileChooserDialog fc(*toplevel, M("DIRBROWSER_BROWSE"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    fc.set_name("RTFileChooser");
    fc.set_default_size(780, 520);

    // Modern styling for the file chooser dialog
    auto fcCss = Gtk::CssProvider::create();
    fcCss->load_from_data(
        "#RTFileChooser {"
        "  border-radius: 8px;"
        "}"
        "#RTFileChooser headerbar {"
        "  border-radius: 8px 8px 0 0;"
        "  padding: 4px 10px;"
        "}"
        "#RTFileChooser placessidebar {"
        "  background: alpha(@theme_base_color, 0.6);"
        "  border-right: 1px solid alpha(@borders, 0.3);"
        "  padding: 4px 0;"
        "}"
        "#RTFileChooser placessidebar row {"
        "  padding: 4px 8px;"
        "  margin: 1px 4px;"
        "  border-radius: 6px;"
        "}"
        "#RTFileChooser placessidebar row:selected {"
        "  border-radius: 6px;"
        "}"
        "#RTFileChooser treeview {"
        "  padding: 2px;"
        "}"
        "#RTFileChooser treeview header button {"
        "  padding: 4px 8px;"
        "  font-weight: 600;"
        "}"
        "#RTFileChooser .dialog-action-box {"
        "  padding: 8px 12px;"
        "  border-top: 1px solid alpha(@borders, 0.3);"
        "}"
        "#RTFileChooser .dialog-action-box button {"
        "  border-radius: 6px;"
        "  padding: 6px 20px;"
        "  min-height: 0;"
        "}"
        "#RTFileChooser .dialog-action-box button:last-child {"
        "  background: alpha(@theme_selected_bg_color, 0.7);"
        "  color: @theme_selected_fg_color;"
        "}"
        "#RTFileChooser .dialog-action-box button:last-child:hover {"
        "  background: @theme_selected_bg_color;"
        "}"
        "#RTFileChooser .path-bar button {"
        "  border-radius: 4px;"
        "  padding: 2px 6px;"
        "  margin: 1px;"
        "}"
    );
    auto screen = fc.get_screen();
    Gtk::StyleContext::add_provider_for_screen(
        screen, fcCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

    fc.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    fc.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    fc.set_default_response(Gtk::RESPONSE_OK);

    int result = fc.run();
    Gtk::StyleContext::remove_provider_for_screen(screen, fcCss);

    if (result == Gtk::RESPONSE_OK) {
        Glib::ustring dir = fc.get_filename();
        if (!dir.empty()) {
            open(dir);
        }
    }
}

void DirBrowser::row_expanded (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path)
{

    expandSuccess = false;

    if (reuseLoadedDirs_ && iter->get_value(dtColumns.childrenLoaded)) {
        expandSuccess = true;

        if (iter->get_value(dtColumns.icon_name) == closedfolder || iter->get_value(dtColumns.icon_name) == "") {
            iter->set_value(dtColumns.icon_name, openfolder);
        }

        return;
    }

    // We will disable model's sorting because it decreases speed of inserting new items
    // in list tree dramatically. Therefore will do:
    // 1) Disable sorting in model
    // 2) Manually sort data in the order determined by the options
    // 3) Enable sorting in model again for UI (sorting by click on header)
    int prevSortColumn;
    Gtk::SortType prevSortType;
    dirTreeModel->get_sort_column_id(prevSortColumn, prevSortType);
    dirTreeModel->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);

    const auto& options = App::get().options();
    auto dir = Gio::File::create_for_path (iter->get_value (dtColumns.dirname));
    auto subDirs = listSubDirs (dir, options.fbShowHidden);

    Gtk::TreeNodeChildren children = iter->children();
    std::list<Gtk::TreeIter> forErase(children.begin(), children.end());

    std::sort (subDirs.begin (), subDirs.end (), [&](const Glib::ustring& firstDir, const Glib::ustring& secondDir)
    {
        switch (options.dirBrowserSortType) {
        default:
        case Gtk::SORT_ASCENDING:
            return firstDir.raw().compare(secondDir.raw()) < 0;
        case Gtk::SORT_DESCENDING:
            return firstDir.raw().compare(secondDir.raw()) > 0;
        }
    });

    for (auto it = subDirs.begin(), end = subDirs.end(); it != end; ++it) {
        addDir(iter, *it);
    }

    for (auto it = forErase.begin(), end = forErase.end(); it != end; ++it) {
        dirTreeModel->erase(*it);
    }

    dirTreeModel->set_sort_column(prevSortColumn, prevSortType);

    expandSuccess = true;

    // Update row icon (only if row icon is not a volume one or is empty)
    if (iter->get_value(dtColumns.icon_name) == closedfolder || iter->get_value(dtColumns.icon_name) == "") {
        iter->set_value(dtColumns.icon_name, openfolder);
    }

    Glib::RefPtr<Gio::FileMonitor> monitor = dir->monitor_directory(Gio::FileMonitorFlags::FILE_MONITOR_WATCH_MOVES);
    iter->set_value (dtColumns.monitor, monitor);
    monitor->signal_changed().connect (sigc::bind(sigc::mem_fun(*this, &DirBrowser::file_changed), iter, dir->get_parse_name()));
    iter->set_value(dtColumns.childrenLoaded, true);

    countPhotosInChildren(iter);
}

void DirBrowser::row_collapsed (const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path)
{
    // Update row icon (only if row icon is not a volume one)
    if (iter->get_value(dtColumns.icon_name) == openfolder) {
        iter->set_value(dtColumns.icon_name, closedfolder);
    }
}

void DirBrowser::updateDir (const Gtk::TreeModel::iterator& iter)
{

    // first test if some files are deleted
    bool change = true;

    while (change) {
        change = false;

        for (Gtk::TreeModel::iterator it = iter->children().begin(); it != iter->children().end(); ++it)
            if (!Glib::file_test (it->get_value (dtColumns.dirname), Glib::FILE_TEST_EXISTS)
                    || !Glib::file_test (it->get_value (dtColumns.dirname), Glib::FILE_TEST_IS_DIR)) {
                GThreadLock lock;
                dirTreeModel->erase (it);
                change = true;
                break;
            }
    }

    // test if new files are created
    auto dir = Gio::File::create_for_path (iter->get_value (dtColumns.dirname));
    auto subDirs = listSubDirs (dir, App::get().options().fbShowHidden);

    for (size_t i = 0; i < subDirs.size(); i++) {
        bool found = false;

        for (Gtk::TreeModel::iterator it = iter->children().begin(); it != iter->children().end() && !found ; ++it) {
            found = (it->get_value (dtColumns.filename) == subDirs[i]);
        }

        if (!found) {
            GThreadLock lock;
            addDir (iter, subDirs[i]);
        }
    }

    iter->set_value(dtColumns.childrenLoaded, true);
}

void DirBrowser::addDir (const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirname)
{

    Gtk::TreeModel::iterator child = dirTreeModel->append(iter->children());
    child->set_value (dtColumns.filename, dirname);
    child->set_value (dtColumns.icon_name, closedfolder);
    Glib::ustring fullname = Glib::build_filename (iter->get_value (dtColumns.dirname), dirname);
    child->set_value (dtColumns.dirname, fullname);
    child->set_value (dtColumns.childrenLoaded, false);
    Gtk::TreeModel::iterator fooRow = dirTreeModel->append(child->children());
    fooRow->set_value (dtColumns.filename, Glib::ustring("foo"));
    fooRow->set_value (dtColumns.childrenLoaded, false);
}

void DirBrowser::row_activated (const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column)
{
    if (path.empty()) {
        return;
    }

    Gtk::TreeModel::iterator iter = dirTreeModel->get_iter(path);
    if (!iter) {
        return;
    }

    Glib::ustring dname = iter->get_value (dtColumns.dirname);

    if (Glib::file_test (dname, Glib::FILE_TEST_IS_DIR)) {
        dirSelectionSignal (dname, Glib::ustring());

        expandAnimConn_.disconnect();
        bool wasExpanded = dirtree->row_expanded(path);

        if (wasExpanded) {
            // Start chevron collapse animation
            auto pathStr = path.to_string();
            auto it = chevronAnimFrame_.find(pathStr);
            chevronAnimFrame_[pathStr] = it != chevronAnimFrame_.end()
                ? it->second : static_cast<int>(chevronFrames_.size()) - 1;
            chevronAnimExpanding_[pathStr] = false;
            revealParentPath_.clear();
            dirtree->revealParentPath.clear();
            dirtree->revealFraction = 1.0;
            startChevronAnim();

            // Collapse with animation (fast: 120ms)
            expandAnimStartH_ = scrolledwindow4->get_allocated_height();
            dirtree->collapse_row(path);
            int minH = 0, natH = 0;
            dirtree->get_preferred_height(minH, natH);
            expandAnimTargetH_ = std::max(natH + 4, scrolledwindow4->get_min_content_height());
            // Clamp to current if natural is larger (scrolled content)
            if (expandAnimTargetH_ >= expandAnimStartH_) {
                // Content is still larger than viewport, no height animation needed
                return;
            }
            expandAnimExpanding_ = false;
            expandAnimFraction_ = 0.0;
            scrolledwindow4->set_min_content_height(expandAnimTargetH_);
            scrolledwindow4->set_max_content_height(expandAnimStartH_);
            scrolledwindow4->set_propagate_natural_height(true);
            expandAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
                expandAnimFraction_ += 16.0 / 120.0; // 120ms collapse
                if (expandAnimFraction_ >= 1.0) {
                    scrolledwindow4->set_max_content_height(-1);
                    return false;
                }
                double t = expandAnimFraction_ * expandAnimFraction_; // ease-in-quad
                int h = expandAnimStartH_ + static_cast<int>((expandAnimTargetH_ - expandAnimStartH_) * t);
                scrolledwindow4->set_max_content_height(h);
                return true;
            }, 16);
        } else {
            // Start chevron expand animation + child reveal
            auto pathStr = path.to_string();
            auto it = chevronAnimFrame_.find(pathStr);
            chevronAnimFrame_[pathStr] = it != chevronAnimFrame_.end()
                ? it->second : 0;
            chevronAnimExpanding_[pathStr] = true;
            revealParentPath_ = pathStr;
            revealAlpha_ = 0.0;
            dirtree->revealParentPath = pathStr;
            dirtree->revealFraction = 0.0;
            startChevronAnim();

            // Expand with animation (smooth: 200ms)
            expandAnimStartH_ = scrolledwindow4->get_allocated_height();
            dirtree->expand_row(path, false);
            int minH = 0, natH = 0;
            dirtree->get_preferred_height(minH, natH);
            expandAnimTargetH_ = natH + 4;
            if (expandAnimTargetH_ <= expandAnimStartH_) {
                // Content fits, no animation needed
                return;
            }
            expandAnimExpanding_ = true;
            expandAnimFraction_ = 0.0;
            scrolledwindow4->set_max_content_height(expandAnimStartH_);
            scrolledwindow4->set_propagate_natural_height(true);
            expandAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
                expandAnimFraction_ += 16.0 / 200.0; // 200ms expand
                if (expandAnimFraction_ >= 1.0) {
                    scrolledwindow4->set_max_content_height(-1);
                    return false;
                }
                double t = 1.0 - std::pow(1.0 - expandAnimFraction_, 3); // ease-out-cubic
                int h = expandAnimStartH_ + static_cast<int>((expandAnimTargetH_ - expandAnimStartH_) * t);
                scrolledwindow4->set_max_content_height(h);
                return true;
            }, 16);
        }
    }
}

void DirBrowser::startChevronAnim()
{
    if (chevronAnimConn_.connected()) return;
    chevronAnimConn_ = Glib::signal_timeout().connect([this]() -> bool {
        // Advance chevron frames
        for (auto it = chevronAnimFrame_.begin(); it != chevronAnimFrame_.end(); ) {
            bool expanding = chevronAnimExpanding_[it->first];
            if (expanding) {
                it->second++;
                if (it->second >= static_cast<int>(chevronFrames_.size()) - 1) {
                    it->second = static_cast<int>(chevronFrames_.size()) - 1;
                    chevronAnimExpanding_.erase(it->first);
                    it = chevronAnimFrame_.erase(it);
                    continue;
                }
            } else {
                it->second--;
                if (it->second <= 0) {
                    it->second = 0;
                    chevronAnimExpanding_.erase(it->first);
                    it = chevronAnimFrame_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Advance reveal fraction (clip-based reveal)
        if (revealAlpha_ < 1.0) {
            revealAlpha_ += 16.0 / 150.0;
            if (revealAlpha_ >= 1.0) {
                revealAlpha_ = 1.0;
                revealParentPath_.clear();
                dirtree->revealParentPath.clear();
                dirtree->revealFraction = 1.0;
            } else {
                // Ease-out-cubic for smooth deceleration
                double t = 1.0 - std::pow(1.0 - revealAlpha_, 3);
                dirtree->revealFraction = t;
            }
        }

        dirtree->queue_draw();

        bool done = chevronAnimFrame_.empty() && revealAlpha_ >= 1.0;
        return !done;
    }, 16);
}

Gtk::TreePath DirBrowser::expandToDir (const Glib::ustring& absDirPath)
{

    Gtk::TreeModel::Path path;
    path.push_back(0);

    char* dcpy = strdup (absDirPath.c_str());
    char* dir = strtok (dcpy, "/\\");
#ifdef _WIN32
    int count = 0;
#endif
    expandSuccess = true;

#ifndef _WIN32
    Gtk::TreeModel::iterator j = dirTreeModel->get_iter (path);
    path.up ();
    path.push_back (0);
    row_expanded(j, path);
    path.push_back (0);
#endif

    while (dir) {
        Glib::ustring dirstr = dir;
#ifdef _WIN32

        if (count == 0) {
            dirstr = dirstr + "\\";
        }

#endif
        Gtk::TreeModel::iterator i = dirTreeModel->get_iter (path);
        int ix = 0;

        while (i && expandSuccess) {
            Gtk::TreeModel::Row crow = *i;
            Glib::ustring str = crow[dtColumns.filename];
#ifdef _WIN32

            if (str.casefold() == dirstr.casefold()) {
#else

            if (str == dirstr) {
#endif
                path.up ();
                path.push_back (ix);
                row_expanded(i, path);
                path.push_back (0);
                break;
            }

            ++ix;
            ++i;
        }
#ifdef _WIN32
        count++;
#endif
        dir = strtok(nullptr, "/\\");
    }

    free(dcpy);

    path.up ();
    if (!path.empty()) {
        dirtree->expand_to_path (path);
    }

    return path;
}

void DirBrowser::open (const Glib::ustring& dirname, const Glib::ustring& fileName, bool collapseTree)
{

    if (collapseTree) {
        dirtree->collapse_all ();
    }

    // WARNING & TODO: One should test here if the directory/file has R/W access permission to avoid crash

    Glib::RefPtr<Gio::File> dir = Gio::File::create_for_path(dirname);

    if( !dir->query_exists()) {
        return;
    }

    Glib::ustring absDirPath = dir->get_parse_name ();
    reuseLoadedDirs_ = !collapseTree;
    Gtk::TreePath path = expandToDir (absDirPath);
    reuseLoadedDirs_ = false;
    if (!path.empty()) {
        dirtree->scroll_to_row (path);
        dirtree->get_selection()->select (path);
    }
    Glib::ustring absFilePath;

    if (!fileName.empty()) {
        absFilePath = Glib::build_filename (absDirPath, fileName);
    }

    dirSelectionSignal (absDirPath, absFilePath);
}

void DirBrowser::file_changed (const Glib::RefPtr<Gio::File>& file, const Glib::RefPtr<Gio::File>& other_file, Gio::FileMonitorEvent event_type, const Gtk::TreeModel::iterator& iter, const Glib::ustring& dirName)
{
    // file is the file that is/was in the monitored directory. other_file is
    // null if only one file is involved (create/delete events), the file that
    // is/was in another directory, or the new name for a renamed file. We want
    // to inspect the file type of the changed file, so we decide which file to
    // use based on the event type.
    const Glib::RefPtr<Gio::File> current_file =
        (event_type == Gio::FILE_MONITOR_EVENT_MOVED ||
            event_type == Gio::FILE_MONITOR_EVENT_RENAMED ||
            event_type == Gio::FILE_MONITOR_EVENT_MOVED_OUT)
            ? other_file
            : file;

    // No need to update the directory if the even type is not rename, move,
    // create, or delete, or if the file is not a directory.
    if (!current_file ||
        event_type == Gio::FILE_MONITOR_EVENT_CHANGED ||
        event_type == Gio::FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event_type == Gio::FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED ||
        event_type == Gio::FILE_MONITOR_EVENT_PRE_UNMOUNT ||
        (event_type != Gio::FILE_MONITOR_EVENT_DELETED &&
            event_type != Gio::FILE_MONITOR_EVENT_UNMOUNTED &&
            !Glib::file_test(current_file->get_path(), Glib::FILE_TEST_IS_DIR))) {
        return;
    }

    updateDir (iter);
}

void DirBrowser::selectDir (Glib::ustring dir)
{

    open (dir, "");
}

bool DirBrowser::onMotionNotify(GdkEventMotion* event)
{
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cell_x, cell_y;

    if (dirtree->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y),
                                  path, column, cell_x, cell_y)) {
        if (hoveredPath_.empty() || path != hoveredPath_) {
            hoveredPath_ = path;

            if (popupVisible_) {
                // Popup already showing — update immediately for new row
                showHoverPopup(path);
            } else {
                // Start/restart delay timer — fires for whichever row is
                // current when it expires (not the row at connect time)
                hoverTimer_.disconnect();
                hoverTimer_ = Glib::signal_timeout().connect([this]() -> bool {
                    if (!hoveredPath_.empty()) {
                        showHoverPopup(hoveredPath_);
                    }
                    return false;
                }, 300);
            }
        }
    } else {
        hideHoverPopup();
    }
    return false;  // don't consume — let hover_selection work
}

bool DirBrowser::onLeaveNotify(GdkEventCrossing*)
{
    hideHoverPopup();
    return false;
}

void DirBrowser::showHoverPopup(const Gtk::TreeModel::Path& path)
{
    if (path.empty() || !dirTreeModel) return;

    auto iter = dirTreeModel->get_iter(path);
    if (!iter) return;

    Glib::ustring dirname = (*iter)[dtColumns.dirname];
    if (dirname.empty()) return;

    // Position popup to the right of the sidebar, aligned with the row
    Gdk::Rectangle cellArea;
    dirtree->get_cell_area(path, *dirtree->get_column(0), cellArea);

    int root_x = 0, root_y = 0;
    auto binWin = dirtree->get_bin_window();
    if (binWin) {
        int bwx, bwy;
        binWin->get_origin(bwx, bwy);
        root_x = bwx;
        root_y = bwy + cellArea.get_y();
    }

    // Place to the right of the entire DirBrowser widget
    int sidebarW = get_allocated_width();
    int popupX = root_x + sidebarW + 4;
    int popupY = root_y;

    // Clear all images
    for (int i = 0; i < 5; i++) {
        hoverImages_[i]->clear();
        hoverImages_[i]->hide();
    }

    hoverPopup_->move(popupX, popupY);
    hoverPopup_->show();
    popupVisible_ = true;

    int session = ++hoverSession_;
    std::thread(&DirBrowser::loadHoverThumbnails, this, dirname, session).detach();
}

void DirBrowser::hideHoverPopup()
{
    hoverTimer_.disconnect();
    hoveredPath_ = Gtk::TreeModel::Path();
    if (popupVisible_) {
        hoverPopup_->hide();
        popupVisible_ = false;
    }
    ++hoverSession_;
}

void DirBrowser::loadHoverThumbnails(const Glib::ustring& dirname, int session)
{
    std::vector<Glib::ustring> imageFiles;
    try {
        Glib::Dir dir(dirname);
        const auto& exts = App::get().options().parsedExtensionsSet;
        for (auto it = dir.begin(); it != dir.end() && imageFiles.size() < 5; ++it) {
            const Glib::ustring& fname = *it;
            auto dotpos = fname.find_last_of('.');
            if (dotpos != Glib::ustring::npos) {
                Glib::ustring ext = fname.substr(dotpos + 1).lowercase();
                if (exts.count(ext.raw())) {
                    imageFiles.push_back(Glib::build_filename(dirname, fname));
                }
            }
        }
    } catch (...) {
        return;
    }

    if (imageFiles.empty()) {
        Glib::signal_idle().connect_once([this, session]() {
            if (session != hoverSession_) return;
            hideHoverPopup();
        });
        return;
    }

    for (size_t i = 0; i < imageFiles.size(); i++) {
        if (session != hoverSession_) return;

        const Glib::ustring& fpath = imageFiles[i];
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;

        try {
            Thumbnail* thm = CacheManager::getInstance()->getEntry(fpath);
            if (thm) {
                double scale = 1.0;
                rtengine::IImage8* img = thm->processThumbImage(48, scale);
                if (img) {
                    auto pb = Gdk::Pixbuf::create_from_data(
                        img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                        img->getWidth(), img->getHeight(), img->getWidth() * 3);
                    pixbuf = pb->copy();
                    delete img;
                }
                thm->decreaseRef();
            }
        } catch (...) {}

        if (!pixbuf) continue;

        int idx = static_cast<int>(i);
        Glib::signal_idle().connect_once([this, session, idx, pixbuf]() {
            if (session != hoverSession_ || !popupVisible_) return;
            hoverImages_[idx]->set(pixbuf);
            hoverImages_[idx]->show();
        });
    }
}

void DirBrowser::countPhotosInChildren (const Gtk::TreeModel::iterator& parent)
{
    struct Entry {
        Glib::ustring dirname;
        Glib::ustring pathStr;
    };
    std::vector<Entry> entries;

    for (auto child = parent->children().begin(); child != parent->children().end(); ++child) {
        Glib::ustring dirname = (*child)[dtColumns.dirname];
        if (!dirname.empty()) {
            entries.push_back({dirname, dirTreeModel->get_path(child).to_string()});
        }
    }

    // DirBrowser is created once and lives for the lifetime of the app,
    // so capturing `this` in the idle callback is safe. Avoid capturing a
    // Glib::RefPtr<Gtk::TreeStore> into the background thread — a crash
    // observed there (refptr copy ctor reading 0xffffffffffffffff) appeared
    // to come from the captured RefPtr's internal pointer being clobbered,
    // so we just use the stable `this` on the main thread instead.
    DirBrowser* self = this;

    std::thread([entries, self]() {
        struct Result {
            Glib::ustring pathStr;
            Glib::ustring countStr;
        };
        auto results = std::make_shared<std::vector<Result>>();
        results->reserve(entries.size());

        for (const auto& entry : entries) {
            int count = 0;
            try {
                Glib::Dir dir(entry.dirname);
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
            } catch (...) {}

            results->push_back({entry.pathStr,
                                count > 0 ? Glib::ustring(std::to_string(count)) : Glib::ustring()});
        }

        Glib::signal_idle().connect_once([self, results]() {
            for (const auto& r : *results) {
                try {
                    Gtk::TreeModel::Path path(r.pathStr);
                    auto iter = self->dirTreeModel->get_iter(path);
                    if (iter) {
                        (*iter)[self->dtColumns.photoCount] = r.countStr;
                    }
                } catch (...) {}
            }
        });
    }).detach();
}
