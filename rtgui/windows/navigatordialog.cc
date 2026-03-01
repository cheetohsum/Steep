/*
 *  This file is part of RawTherapee.
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
#include "navigatordialog.h"

#include "../navigator.h"
#include "../toolpanel.h"

NavigatorDialog::NavigatorDialog(Gtk::Window& parent, Navigator* nav)
{
    set_transient_for(parent);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
    set_name("NavigatorDialog");
    set_default_size(300, 350);
    set_skip_taskbar_hint(true);

    auto headerBar = Gtk::manage(new Gtk::HeaderBar());
    headerBar->set_title(M("MAIN_MSG_NAVIGATOR"));
    headerBar->set_show_close_button(true);
    set_titlebar(*headerBar);

    add(*nav);
}

void NavigatorDialog::toggleVisibility()
{
    if (is_visible()) {
        hide();
    } else {
        show_all();
        present();
    }
}

bool NavigatorDialog::on_delete_event(GdkEventAny* /*event*/)
{
    hide();
    return true;
}
