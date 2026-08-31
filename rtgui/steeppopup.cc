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
#include "steeppopup.h"

namespace steepui
{

PopupMenu::PopupMenu()
{
    // Windows freeze-until-configure hygiene: a popup window whose
    // move/resize request the OS coalesces to a no-op never gets its
    // confirming configure event; the repaint freeze then leaks into every
    // later popup that reuses the window. A fresh native window cannot be
    // frozen, so destroy the menu's window shortly after each close.
    // A short timeout, deliberately not a default-priority idle — those can
    // starve for over a second under load and silently skip the guard.
    menu_.signal_hide().connect([this]() {
        unfreezeTimer_.disconnect();
        unfreezeTimer_ = Glib::signal_timeout().connect([this]() -> bool {
            if (menu_.get_visible()) {
                return false; // reopened; the next hide re-arms
            }
            Gtk::Widget* top = menu_.get_toplevel();
            if (top && top->get_realized() && !top->get_mapped()) {
                gtk_widget_unrealize(GTK_WIDGET(top->gobj()));
            }
            return false;
        }, 30);
    });
}

PopupMenu::~PopupMenu()
{
    unfreezeTimer_.disconnect();
}

Gtk::MenuItem* PopupMenu::addItem(const Glib::ustring& label, std::function<void()> onActivate)
{
    auto* item = Gtk::manage(new Gtk::MenuItem(label));
    if (onActivate) {
        item->signal_activate().connect([cb = std::move(onActivate)]() { cb(); });
    }
    menu_.append(*item);
    return item;
}

void PopupMenu::addSeparator()
{
    menu_.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
}

std::vector<Gtk::RadioMenuItem*> PopupMenu::addRadioGroup(const std::vector<Glib::ustring>& labels,
                                                          int activeIndex,
                                                          std::function<void(int)> onSelect)
{
    std::vector<Gtk::RadioMenuItem*> items;
    items.reserve(labels.size());

    Gtk::RadioButtonGroup group;
    for (const auto& label : labels) {
        auto* item = Gtk::manage(new Gtk::RadioMenuItem(group, label));
        menu_.append(*item);
        items.push_back(item);
    }

    // Apply the initial state BEFORE connecting, so building is silent.
    if (activeIndex >= 0 && activeIndex < static_cast<int>(items.size())) {
        items[activeIndex]->set_active(true);
    }

    if (onSelect) {
        for (size_t i = 0; i < items.size(); ++i) {
            Gtk::RadioMenuItem* item = items[i];
            item->signal_activate().connect([item, i, onSelect]() {
                if (item->get_active()) {
                    onSelect(static_cast<int>(i));
                }
            });
        }
    }

    return items;
}

void PopupMenu::clear()
{
    for (auto* child : menu_.get_children()) {
        menu_.remove(*child);
    }
}

void PopupMenu::popupAtPointer(const GdkEvent* trigger)
{
    menu_.show_all();
    menu_.popup_at_pointer(trigger);
}

void PopupMenu::popupAtWidget(Gtk::Widget& anchor)
{
    menu_.show_all();
    menu_.popup_at_widget(&anchor, Gdk::GRAVITY_SOUTH_WEST, Gdk::GRAVITY_NORTH_WEST, nullptr);
}

void PopupMenu::attachTo(Gtk::MenuButton& button)
{
    menu_.show_all();
    button.set_popup(menu_);
}

} // namespace steepui
