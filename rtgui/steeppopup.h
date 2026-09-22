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
#pragma once

#include <gtkmm.h>

#include <functional>
#include <map>
#include <vector>

namespace steepui
{

// Shared scrollable surface for film/grading presets and settings lists.
void styleListPopover(Gtk::Popover& popover, Gtk::ScrolledWindow& scroll, Gtk::ListBox& list);

class ListPopover : public Gtk::Popover
{
public:
    explicit ListPopover(Gtk::Widget& anchor);
    ~ListPopover() override;
    Gtk::ListBoxRow* addItem(const Glib::ustring& label, std::function<void()> activate,
                            std::function<void()> preview = {});
    void addSeparator();
    void clear();
    Gtk::ListBox& list() { return list_; }
    sigc::signal<void>& signal_preview_left() { return previewLeft_; }

private:
    Gtk::ScrolledWindow scroll_;
    Gtk::ListBox list_;
    struct Actions { std::function<void()> activate, preview; };
    std::map<Gtk::ListBoxRow*, Actions> actions_;
    Gtk::ListBoxRow* hovered_ = nullptr;
    bool keyboardNavigation_ = false;
    sigc::signal<void> previewLeft_;
    std::vector<sigc::connection> connections_;
    void hover(Gtk::ListBoxRow* row);
};

// Present existing copy-filter actions without duplicating their state or
// clipboard logic. Navigation stays inside one scrollable popover.
class MenuListPopover final : public ListPopover
{
public:
    MenuListPopover(Gtk::Widget& anchor, Gtk::Menu& model);
    ~MenuListPopover() override;
private:
    Gtk::Menu& model_;
    std::vector<Gtk::Menu*> parents_;
    std::vector<sigc::connection> bindings_;
    sigc::connection navigation_;
    sigc::connection show_, hide_;
    void showPage(Gtk::Menu& menu);
    void navigate(Gtk::Menu& menu);
};

/** Popup menu builder — the designated home for GtkMenu usage.
 *
 *  GTK4 removes GtkMenu; new popup surface area goes through this class so
 *  the eventual popover rebuild happens here alone (docs/gtk4-readiness.md).
 *  Every menu built here also gets the Windows freeze-until-configure fix
 *  for free: the popup's native window is destroyed shortly after each
 *  close, so no popup can inherit a frozen backing window (see the long
 *  comment in filebrowser.cc for the pathology).
 *
 *  GUI thread only. The builder owns its Gtk::Menu; keep the PopupMenu
 *  alive as long as the menu can be shown.
 */
class PopupMenu
{
public:
    PopupMenu();
    ~PopupMenu();

    PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;

    /// Plain activatable item.
    Gtk::MenuItem* addItem(const Glib::ustring& label, std::function<void()> onActivate);

    /// The same with an icon ahead of a left-aligned label, which is how the
    /// rest of the application's menus read. @p iconName is an RTImage name.
    Gtk::MenuItem* addItem(const Glib::ustring& iconName, const Glib::ustring& label,
                           std::function<void()> onActivate);

    void addSeparator();

    /** Exclusive radio row set. @p activeIndex is applied before handlers
     *  connect, so building never fires @p onSelect; afterwards it fires
     *  once per user activation (idempotent handlers recommended — a
     *  programmatic set_active on a returned item re-emits activate). */
    std::vector<Gtk::RadioMenuItem*> addRadioGroup(const std::vector<Glib::ustring>& labels,
                                                   int activeIndex,
                                                   std::function<void(int)> onSelect);

    /// Remove every item (for menus rebuilt before each popup).
    void clear();

    /// Popup at the pointer, from an event handler.
    void popupAtPointer(const GdkEvent* trigger);

    /// Popup anchored below a widget.
    void popupAtWidget(Gtk::Widget& anchor);

    /// Install as a MenuButton's popup.
    void attachTo(Gtk::MenuButton& button);

    Gtk::Menu& menu() { return menu_; }

private:
    Gtk::Menu menu_;
    sigc::connection unfreezeTimer_;
};

} // namespace steepui
