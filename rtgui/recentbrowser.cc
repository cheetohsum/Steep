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

    // Built through the popup factory (GTK4 seam + Windows popup hygiene).
    recentPopup_ = std::make_unique<steepui::PopupMenu>();
    recentPopup_->attachTo(*recentButton);
    pack_start(*recentButton, Gtk::PACK_SHRINK);

    rebuildMenu();

    show_all ();
}

void RecentBrowser::rebuildMenu()
{
    recentPopup_->clear();

    const auto& folders = App::get().options().recentFolders;

    if (folders.empty()) {
        recentPopup_->addItem(M("MAIN_FRAME_RECENT"), nullptr)->set_sensitive(false);
    } else {
        for (const auto& folder : folders) {
            auto* item = recentPopup_->addItem(folder, [this, folder]() {
                selectRecent(folder);
            });
            item->set_tooltip_text(folder);
        }
    }
}

void RecentBrowser::popupMenuAt (Gtk::Widget& anchor)
{
    rebuildMenu();
    recentPopup_->popupAtWidget(anchor);
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
