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

#include <gtkmm.h>

#include "guiutils.h"
#include "steeppopup.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class RecentBrowser :
    public Gtk::Box
{
public:
    typedef sigc::slot<void, const Glib::ustring&> DirSelectionSlot;

private:
    Gtk::MenuButton* recentButton;
    std::unique_ptr<steepui::PopupMenu> recentPopup_;
    DirSelectionSlot selectDir;
    // Shown above the recent folders (Browse, Directory cleanup): this menu is
    // what the folder icon opens, on hover and on click.
    std::vector<std::pair<Glib::ustring, std::function<void()>>> leadingActions_;

    void rebuildMenu();
    void selectRecent(Glib::ustring dirname);

public:

    RecentBrowser ();

    void setDirSelector (const DirSelectionSlot& selectDir);

    void setLeadingActions (std::vector<std::pair<Glib::ustring, std::function<void()>>> actions)
    {
        leadingActions_ = std::move(actions);
        rebuildMenu();
    }

    void dirSelected (const Glib::ustring& dirname, const Glib::ustring& openfile);

    // Pop the recent-folders menu below an arbitrary anchor widget (used by
    // the folder header's hover-to-open behavior).
    void popupMenuAt (Gtk::Widget& anchor);
};

inline void RecentBrowser::setDirSelector (const RecentBrowser::DirSelectionSlot& selectDir)
{
    this->selectDir = selectDir;
}
