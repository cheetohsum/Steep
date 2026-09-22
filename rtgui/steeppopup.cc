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
#include "rtimage.h"
#include "steeppopup.h"
#include "multilangmgr.h"

namespace steepui
{

void styleListPopover(Gtk::Popover& popover, Gtk::ScrolledWindow& scroll, Gtk::ListBox& list)
{
    popover.set_position(Gtk::POS_BOTTOM);
    popover.get_style_context()->add_class("SteepListPopover");
    scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll.set_max_content_height(420);
    scroll.set_propagate_natural_height(true);
    scroll.set_min_content_width(240);
    scroll.set_max_content_width(380);
    scroll.set_propagate_natural_width(true);
    list.get_style_context()->add_class("SteepPopupList");
    list.set_selection_mode(Gtk::SELECTION_BROWSE);
    list.set_activate_on_single_click(true);
}

ListPopover::ListPopover(Gtk::Widget& anchor) : Gtk::Popover(anchor)
{
    styleListPopover(*this, scroll_, list_);
    scroll_.add(list_);
    add(scroll_);
    connections_.push_back(list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        const auto it = actions_.find(row);
        auto action = it == actions_.end() ? std::function<void()>() : it->second.activate;
        if (action) action();
    }));
    list_.add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
    connections_.push_back(list_.signal_motion_notify_event().connect([this](GdkEventMotion* event) {
        keyboardNavigation_ = false;
        hover(list_.get_row_at_y(static_cast<int>(event->y)));
        return false;
    }, false));
    connections_.push_back(list_.signal_leave_notify_event().connect([this](GdkEventCrossing* event) {
        if (event->detail != GDK_NOTIFY_INFERIOR) hover(nullptr);
        return false;
    }, false));
    connections_.push_back(list_.signal_key_press_event().connect([this](GdkEventKey*) {
        keyboardNavigation_ = true;
        return false;
    }, false));
    connections_.push_back(list_.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (keyboardNavigation_) hover(row);
    }));
    connections_.push_back(signal_show().connect([this]() { hovered_ = nullptr; keyboardNavigation_ = false; }));
    connections_.push_back(signal_hide().connect([this]() { hover(nullptr); }));
}

ListPopover::~ListPopover()
{
    for (auto& connection : connections_) connection.disconnect();
}

void ListPopover::hover(Gtk::ListBoxRow* row)
{
    if (row == hovered_) return;
    hovered_ = row;
    const auto it = actions_.find(row);
    if (it != actions_.end() && it->second.preview) it->second.preview();
    else previewLeft_.emit();
}

Gtk::ListBoxRow* ListPopover::addItem(const Glib::ustring& text, std::function<void()> activate,
                                    std::function<void()> preview)
{
    auto* row = Gtk::manage(new Gtk::ListBoxRow());
    auto* label = Gtk::manage(new Gtk::Label(text));
    label->set_xalign(0);
    row->add(*label);
    list_.append(*row);
    actions_[row] = {std::move(activate), std::move(preview)};
    return row;
}

void ListPopover::addSeparator()
{
    auto* row = addItem("", {});
    row->remove();
    row->add(*Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL)));
    row->set_activatable(false);
    row->set_selectable(false);
    row->get_style_context()->add_class("separator");
}

void ListPopover::clear()
{
    hovered_ = nullptr;
    actions_.clear();
    for (auto* child : list_.get_children()) list_.remove(*child);
}

namespace {
Gtk::Label* menuLabel(Gtk::Widget* widget)
{
    if (auto* label = dynamic_cast<Gtk::Label*>(widget)) return label;
    if (auto* container = dynamic_cast<Gtk::Container*>(widget)) {
        for (auto* child : container->get_children()) {
            if (auto* label = menuLabel(child)) return label;
        }
    }
    return nullptr;
}
}

MenuListPopover::MenuListPopover(Gtk::Widget& anchor, Gtk::Menu& model) :
    ListPopover(anchor), model_(model)
{
    model_.reference();
    show_ = signal_show().connect([this]() {
        parents_.clear();
        showPage(model_);
    });
    hide_ = signal_hide().connect([this]() { navigation_.disconnect(); });
}

MenuListPopover::~MenuListPopover()
{
    navigation_.disconnect();
    show_.disconnect();
    hide_.disconnect();
    for (auto& binding : bindings_) binding.disconnect();
    model_.unreference();
}

void MenuListPopover::navigate(Gtk::Menu& menu)
{
    navigation_.disconnect();
    navigation_ = Glib::signal_idle().connect([this, &menu]() {
        if (get_visible()) showPage(menu);
        return false;
    });
}

void MenuListPopover::showPage(Gtk::Menu& menu)
{
    for (auto& binding : bindings_) binding.disconnect();
    bindings_.clear();
    clear();
    if (!parents_.empty()) {
        auto* back = addItem(M("GENERAL_BACK"), [this]() {
            auto* parent = parents_.back();
            parents_.pop_back();
            navigate(*parent);
        });
        back->remove();
        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        box->pack_start(*Gtk::manage(new RTImage("arrow-left-small", Gtk::ICON_SIZE_MENU)), Gtk::PACK_SHRINK);
        box->pack_start(*Gtk::manage(new Gtk::Label(M("GENERAL_BACK"))), Gtk::PACK_SHRINK);
        back->add(*box);
        addSeparator();
    }
    for (auto* child : menu.get_children()) {
        auto* item = dynamic_cast<Gtk::MenuItem*>(child);
        if (!item || !item->get_visible()) continue;
        if (dynamic_cast<Gtk::SeparatorMenuItem*>(item)) { addSeparator(); continue; }
        auto* sourceLabel = menuLabel(item);
        const Glib::ustring text = sourceLabel ? sourceLabel->get_text() : item->get_label();
        auto* check = dynamic_cast<Gtk::CheckMenuItem*>(item);
        auto* submenu = dynamic_cast<Gtk::Menu*>(item->get_submenu());
        auto* row = addItem(text, [this, item, check, submenu, &menu]() {
            if (submenu) { parents_.push_back(&menu); navigate(*submenu); }
            else if (check) check->set_active(!check->get_active());
            else {
                const bool keepOpen = item->get_style_context()->has_class("keep-open");
                if (!keepOpen) popdown();
                item->activate();
            }
        });
        row->set_sensitive(item->get_sensitive());
        row->remove();
        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 7));
        auto* label = Gtk::manage(new Gtk::Label(text));
        label->set_xalign(0);
        label->set_max_width_chars(32);
        label->set_ellipsize(Pango::ELLIPSIZE_END);
        box->pack_start(*label, Gtk::PACK_EXPAND_WIDGET);
        if (sourceLabel) {
            bindings_.push_back(sourceLabel->property_label().signal_changed().connect([sourceLabel, label]() {
                label->set_text(sourceLabel->get_text());
            }));
        }
        if (check) {
            auto* toggle = Gtk::manage(new Gtk::CheckButton());
            toggle->set_active(check->get_active());
            box->pack_end(*toggle, Gtk::PACK_SHRINK);
            toggle->signal_toggled().connect([check, toggle]() { check->set_active(toggle->get_active()); });
            bindings_.push_back(check->signal_toggled().connect([check, toggle]() {
                toggle->set_active(check->get_active());
            }));
        } else if (submenu) {
            box->pack_end(*Gtk::manage(new RTImage("arrow-right-small", Gtk::ICON_SIZE_MENU)), Gtk::PACK_SHRINK);
        }
        row->add(*box);
    }
    show_all_children();
}

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

Gtk::MenuItem* PopupMenu::addItem(const Glib::ustring& iconName, const Glib::ustring& label,
                                  std::function<void()> onActivate)
{
    Gtk::MenuItem* item = addItem(label, std::move(onActivate));

    if (iconName.empty()) {
        return item;
    }

    // Same row shape as the mask menus: icon, gap, label hard against it.
    Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label* text = Gtk::manage(new Gtk::Label(label));
    text->set_halign(Gtk::ALIGN_START);
    row->pack_start(*Gtk::manage(new RTImage(iconName)), Gtk::PACK_SHRINK);
    row->pack_start(*text, Gtk::PACK_EXPAND_WIDGET);

    if (item->get_child()) {
        item->remove();
    }

    item->add(*row);
    row->show_all();
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
