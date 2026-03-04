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

namespace mcp {

class McpServer;

class McpDialog : public Gtk::Dialog {
public:
    McpDialog(Gtk::Window& parent, McpServer* server);
    ~McpDialog() override;

private:
    void updateStatus();
    void onStartStop();
    void onAutoConfig();
    void onCopyUrl();
    void onCopyConfig();
    void onAutoStartToggled();
    void buildToolList();

    McpServer* server_;
    Gtk::Label* statusLabel_;
    Gtk::Label* portLabel_;
    Gtk::Label* urlLabel_;
    Gtk::Button* startStopBtn_;
    Gtk::Button* connectBtn_;
    Gtk::Button* copyUrlBtn_;
    Gtk::Button* copyConfigBtn_;
    Gtk::CheckButton* autoStartCheck_;
    Gtk::TreeView* toolTreeView_;
    Glib::RefPtr<Gtk::ListStore> toolListStore_;
    sigc::connection timerConn_;

    struct ToolColumns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> description;
        ToolColumns() { add(name); add(description); }
    };
    ToolColumns toolColumns_;
};

} // namespace mcp
