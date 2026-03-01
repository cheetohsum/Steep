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
#include "historydialog.h"

#include "../history.h"
#include "../toolpanel.h"

HistoryDialog::HistoryDialog(Gtk::Window& parent, History* hist)
{
    set_transient_for(parent);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
    set_name("HistoryDialog");
    set_default_size(350, 500);
    set_skip_taskbar_hint(true);

    auto headerBar = Gtk::manage(new Gtk::HeaderBar());
    headerBar->set_title(M("HISTORY_LABEL"));
    headerBar->set_show_close_button(true);
    set_titlebar(*headerBar);

    add(*hist);
}

void HistoryDialog::toggleVisibility()
{
    if (is_visible()) {
        hide();
    } else {
        show_all();
        present();
    }
}

bool HistoryDialog::on_delete_event(GdkEventAny* /*event*/)
{
    hide();
    return true;
}
