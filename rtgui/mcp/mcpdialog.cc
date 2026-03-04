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

#include "mcp/mcpdialog.h"
#include "mcp/mcpserver.h"
#include "mcp/mcptools.h"
#include "multilangmgr.h"
#include "options.h"
#include "rtengine/cJSON.h"

#include <glibmm/spawn.h>
#include <fstream>
#include <cstdlib>

namespace mcp {

McpDialog::~McpDialog() {
    timerConn_.disconnect();
}

McpDialog::McpDialog(Gtk::Window& parent, McpServer* server)
    : Gtk::Dialog("", parent, true)
    , server_(server)
{
    set_default_size(480, -1);
    set_resizable(true);

    // Use a headerbar for proper CSD theming
    auto* headerBar = Gtk::manage(new Gtk::HeaderBar());
    headerBar->set_title(M("MCP_DIALOG_TITLE"));
    headerBar->set_show_close_button(true);
    headerBar->set_decoration_layout(":close");
    set_titlebar(*headerBar);

    auto* content = get_content_area();
    content->set_spacing(8);
    content->set_margin_start(16);
    content->set_margin_end(16);
    content->set_margin_top(12);
    content->set_margin_bottom(12);

    // Description
    auto* descLabel = Gtk::manage(new Gtk::Label(M("MCP_DIALOG_DESC")));
    descLabel->set_line_wrap(true);
    descLabel->set_max_width_chars(60);
    descLabel->set_halign(Gtk::ALIGN_START);
    descLabel->set_xalign(0.0);
    content->pack_start(*descLabel, false, false);

    // Separator
    content->pack_start(*Gtk::manage(new Gtk::Separator()), false, false, 2);

    // Status grid (2-column layout, no colons)
    auto* grid = Gtk::manage(new Gtk::Grid());
    grid->set_column_spacing(12);
    grid->set_row_spacing(2);

    auto* statusTitle = Gtk::manage(new Gtk::Label());
    statusTitle->set_markup("<b>" + M("MCP_STATUS") + "</b>");
    statusTitle->set_halign(Gtk::ALIGN_END);
    statusLabel_ = Gtk::manage(new Gtk::Label(M("MCP_STATUS_STOPPED")));
    statusLabel_->set_halign(Gtk::ALIGN_START);
    grid->attach(*statusTitle, 0, 0);
    grid->attach(*statusLabel_, 1, 0);

    auto* portTitle = Gtk::manage(new Gtk::Label());
    portTitle->set_markup("<b>" + M("MCP_PORT") + "</b>");
    portTitle->set_halign(Gtk::ALIGN_END);
    portLabel_ = Gtk::manage(new Gtk::Label());
    portLabel_->set_halign(Gtk::ALIGN_START);
    grid->attach(*portTitle, 0, 1);
    grid->attach(*portLabel_, 1, 1);

    auto* urlTitle = Gtk::manage(new Gtk::Label());
    urlTitle->set_markup("<b>" + M("MCP_URL") + "</b>");
    urlTitle->set_halign(Gtk::ALIGN_END);
    urlLabel_ = Gtk::manage(new Gtk::Label());
    urlLabel_->set_halign(Gtk::ALIGN_START);
    urlLabel_->set_selectable(true);
    grid->attach(*urlTitle, 0, 2);
    grid->attach(*urlLabel_, 1, 2);

    content->pack_start(*grid, false, false);

    // Auto-start checkbox
    autoStartCheck_ = Gtk::manage(new Gtk::CheckButton(M("MCP_AUTO_START")));
    autoStartCheck_->set_active(App::get().options().mcpAutoStart);
    autoStartCheck_->signal_toggled().connect(sigc::mem_fun(*this, &McpDialog::onAutoStartToggled));
    content->pack_start(*autoStartCheck_, false, false, 2);

    // Buttons row
    auto* btnBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    btnBox->set_halign(Gtk::ALIGN_CENTER);

    startStopBtn_ = Gtk::manage(new Gtk::Button(M("MCP_START_SERVER")));
    startStopBtn_->signal_clicked().connect(sigc::mem_fun(*this, &McpDialog::onStartStop));
    btnBox->pack_start(*startStopBtn_, false, false);

    copyUrlBtn_ = Gtk::manage(new Gtk::Button(M("MCP_COPY_URL")));
    copyUrlBtn_->signal_clicked().connect(sigc::mem_fun(*this, &McpDialog::onCopyUrl));
    copyUrlBtn_->set_sensitive(false);
    btnBox->pack_start(*copyUrlBtn_, false, false);

    copyConfigBtn_ = Gtk::manage(new Gtk::Button(M("MCP_COPY_CONFIG")));
    copyConfigBtn_->signal_clicked().connect(sigc::mem_fun(*this, &McpDialog::onCopyConfig));
    copyConfigBtn_->set_sensitive(false);
    btnBox->pack_start(*copyConfigBtn_, false, false);

    connectBtn_ = Gtk::manage(new Gtk::Button(M("MCP_CONNECT_CLAUDE")));
    connectBtn_->signal_clicked().connect(sigc::mem_fun(*this, &McpDialog::onAutoConfig));
    connectBtn_->set_sensitive(false);
    btnBox->pack_start(*connectBtn_, false, false);

    content->pack_start(*btnBox, false, false);

    // Separator
    content->pack_start(*Gtk::manage(new Gtk::Separator()), false, false, 2);

    // Tools expander with scrolled list
    auto* toolExpander = Gtk::manage(new Gtk::Expander(M("MCP_TOOLS_LABEL")));
    toolExpander->set_expanded(false);

    toolListStore_ = Gtk::ListStore::create(toolColumns_);
    toolTreeView_ = Gtk::manage(new Gtk::TreeView(toolListStore_));
    toolTreeView_->set_headers_visible(true);
    toolTreeView_->set_enable_search(false);

    auto* nameCol = Gtk::manage(new Gtk::TreeViewColumn(M("MCP_TOOLS_NAME"), toolColumns_.name));
    nameCol->set_resizable(true);
    nameCol->set_min_width(140);
    toolTreeView_->append_column(*nameCol);

    auto* descCol = Gtk::manage(new Gtk::TreeViewColumn(M("MCP_TOOLS_DESC"), toolColumns_.description));
    descCol->set_resizable(true);
    descCol->set_expand(true);
    // Enable word wrap on the description cell
    auto* descRenderer = dynamic_cast<Gtk::CellRendererText*>(descCol->get_first_cell());
    if (descRenderer) {
        descRenderer->property_wrap_mode() = Pango::WRAP_WORD;
        descRenderer->property_wrap_width() = 280;
    }
    toolTreeView_->append_column(*descCol);

    buildToolList();

    auto* scrollWin = Gtk::manage(new Gtk::ScrolledWindow());
    scrollWin->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrollWin->set_min_content_height(300);
    scrollWin->add(*toolTreeView_);
    toolExpander->add(*scrollWin);

    content->pack_start(*toolExpander, true, true);

    // Update status periodically
    updateStatus();
    timerConn_ = Glib::signal_timeout().connect([this]() {
        updateStatus();
        return true;
    }, 1000);

    show_all_children();
}

void McpDialog::buildToolList() {
    toolListStore_->clear();
    auto tools = getToolDefinitions();
    for (auto& t : tools) {
        auto row = *(toolListStore_->append());
        row[toolColumns_.name] = t.name;
        row[toolColumns_.description] = t.description;
        if (t.inputSchema) cJSON_Delete(t.inputSchema);
    }
}

void McpDialog::updateStatus() {
    if (server_->isRunning()) {
        statusLabel_->set_markup("<span foreground='#73d216'>" + M("MCP_STATUS_RUNNING") + "</span>");
        portLabel_->set_text(std::to_string(server_->getPort()));
        std::string url = "http://localhost:" + std::to_string(server_->getPort()) + "/mcp";
        urlLabel_->set_text(url);
        startStopBtn_->set_label(M("MCP_STOP_SERVER"));
        copyUrlBtn_->set_sensitive(true);
        copyConfigBtn_->set_sensitive(true);
        connectBtn_->set_sensitive(true);
    } else {
        statusLabel_->set_markup("<span foreground='#cc0000'>" + M("MCP_STATUS_STOPPED") + "</span>");
        portLabel_->set_text("");
        urlLabel_->set_text("");
        startStopBtn_->set_label(M("MCP_START_SERVER"));
        copyUrlBtn_->set_sensitive(false);
        copyConfigBtn_->set_sensitive(false);
        connectBtn_->set_sensitive(false);
    }
}

void McpDialog::onStartStop() {
    if (server_->isRunning()) {
        server_->stop();
    } else {
        server_->start();
    }
    updateStatus();
}

void McpDialog::onAutoConfig() {
    if (!server_->isRunning()) return;

    std::string url = "http://localhost:" + std::to_string(server_->getPort()) + "/mcp";

    // Try to run claude mcp add command asynchronously
    try {
        std::string standardOutput, standardError;
        int exitStatus = 0;
        Glib::spawn_command_line_sync(
            "claude mcp add --transport http steep " + url,
            &standardOutput, &standardError, &exitStatus);

        if (exitStatus == 0) {
            Gtk::MessageDialog dlg(*this, M("MCP_CONNECT_SUCCESS"), false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
            dlg.set_secondary_text(M("MCP_CONNECT_SUCCESS_DESC"));
            dlg.run();
            return;
        }
    } catch (const Glib::SpawnError&) {
        // CLI not available, fall through to config file approach
    } catch (const Glib::ShellError&) {
        // CLI not available, fall through to config file approach
    }

    {
        // CLI not found or failed - write config file instead
        std::string home;
#ifdef _WIN32
        const char* userProfile = std::getenv("USERPROFILE");
        if (userProfile) home = userProfile;
#else
        const char* homeEnv = std::getenv("HOME");
        if (homeEnv) home = homeEnv;
#endif
        if (!home.empty()) {
            std::string configPath = home + "/.mcp.json";
            std::ofstream f(configPath);
            if (f.is_open()) {
                f << "{\n"
                  << "  \"mcpServers\": {\n"
                  << "    \"steep\": {\n"
                  << "      \"type\": \"http\",\n"
                  << "      \"url\": \"" << url << "\"\n"
                  << "    }\n"
                  << "  }\n"
                  << "}\n";
                f.close();

                Gtk::MessageDialog dlg(*this, M("MCP_CONFIG_WRITTEN"), false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
                dlg.set_secondary_text(M("MCP_CONFIG_WRITTEN_DESC") + "\n" + configPath);
                dlg.run();
                return;
            }
        }

        Gtk::MessageDialog dlg(*this, M("MCP_CONNECT_FAILED"), false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        dlg.set_secondary_text(M("MCP_CONNECT_FAILED_DESC") + "\n" + url);
        dlg.run();
    }
}

void McpDialog::onCopyUrl() {
    if (!server_->isRunning()) return;

    std::string url = "http://localhost:" + std::to_string(server_->getPort()) + "/mcp";
    auto clipboard = Gtk::Clipboard::get();
    clipboard->set_text(url);
}

void McpDialog::onAutoStartToggled() {
    App::get().mut_options().mcpAutoStart = autoStartCheck_->get_active();
}

void McpDialog::onCopyConfig() {
    if (!server_->isRunning()) return;

    std::string url = "http://localhost:" + std::to_string(server_->getPort()) + "/mcp";
    std::string config =
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"steep\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" + url + "\"\n"
        "    }\n"
        "  }\n"
        "}";

    auto clipboard = Gtk::Clipboard::get();
    clipboard->set_text(config);
}

} // namespace mcp
