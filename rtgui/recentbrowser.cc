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
#include "recentbrowser.h"
#include "multilangmgr.h"
#include "options.h"

using namespace rtengine;

RecentBrowser::RecentBrowser ()
{
    set_orientation(Gtk::ORIENTATION_HORIZONTAL);
    set_name("RecentFolders");

    recentButton = Gtk::manage(new Gtk::MenuButton());
    recentButton->set_name("RecentFoldersButton");
    recentButton->set_label(M("MAIN_FRAME_RECENT_SHORT").lowercase());
    recentButton->set_relief(Gtk::RELIEF_NONE);
    recentButton->set_tooltip_text(M("MAIN_FRAME_RECENT"));

    recentMenu = Gtk::manage(new Gtk::Menu());
    recentButton->set_popup(*recentMenu);
    pack_start(*recentButton, Gtk::PACK_SHRINK);

    rebuildMenu();

    show_all ();
}

void RecentBrowser::rebuildMenu()
{
    for (auto* child : recentMenu->get_children()) {
        recentMenu->remove(*child);
    }

    const auto& folders = App::get().options().recentFolders;

    if (folders.empty()) {
        auto* emptyItem = Gtk::manage(new Gtk::MenuItem(M("MAIN_FRAME_RECENT")));
        emptyItem->set_sensitive(false);
        recentMenu->append(*emptyItem);
    } else {
        for (const auto& folder : folders) {
            auto* item = Gtk::manage(new Gtk::MenuItem(folder));
            item->set_tooltip_text(folder);
            item->signal_activate().connect(
                sigc::bind(sigc::mem_fun(*this, &RecentBrowser::selectRecent), folder));
            recentMenu->append(*item);
        }
    }

    recentMenu->show_all();
}

void RecentBrowser::selectRecent(Glib::ustring dirname)
{
    if (selectDir && !dirname.empty()) {
        selectDir(dirname);
    }
}

void RecentBrowser::dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile)
{
    auto& options = App::get().mut_options();
    ssize_t numFolders = options.recentFolders.size();
    ssize_t i = -1;

    if(numFolders > 0) { // search entry and move to top if it exists
        for(i = 0; i < numFolders; ++i) {
            if(options.recentFolders[i] == dirname) {
                break;
            }
        }

        if(i > 0) {
            if(i < numFolders) {
                options.recentFolders.erase(options.recentFolders.begin() + i);
            }

            options.recentFolders.insert(options.recentFolders.begin(), dirname);
        }
    } else {
        options.recentFolders.insert(options.recentFolders.begin(), dirname);
    }

    rebuildMenu();
}
