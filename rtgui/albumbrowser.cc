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
#include "albumbrowser.h"

#include "cacheimagedata.h"
#include "cachemanager.h"
#include "guiutils.h"
#include "multilangmgr.h"
#include "options.h"
#include "pathutils.h"
#include "rtimage.h"
#include "thumbnail.h"

#include <fstream>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>

// Static signal: all AlbumBrowser instances connect to this.
// When one instance saves, it emits this signal so others reload.
sigc::signal<void, AlbumBrowser*> AlbumBrowser::albumsChangedOnDisk_;

AlbumBrowser::AlbumBrowser ()
    : nextNodeId_(0), selectedNodeId_(-1)
{
    set_orientation(Gtk::ORIENTATION_VERTICAL);

    // Connect to global change signal so we reload when another instance saves
    globalChangeConn_ = albumsChangedOnDisk_.connect(
        sigc::mem_fun(*this, &AlbumBrowser::onGlobalAlbumsChanged));

    // Header bar: "Albums" label + "+" dropdown button
    Gtk::Box* headerBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    headerBar->set_name("AlbumHeader");

    Gtk::Label* headerLabel = Gtk::manage(new Gtk::Label(M("ALBUM_HEADER")));
    headerLabel->set_halign(Gtk::ALIGN_START);
    headerLabel->set_margin_start(4);
    headerBar->pack_start(*headerLabel, Gtk::PACK_EXPAND_WIDGET);

    // "+" button with dropdown menu
    Gtk::MenuButton* addBtn = Gtk::manage(new Gtk::MenuButton());
    addBtn->set_name("AlbumAddBtn");
    addBtn->set_image(*Gtk::manage(new RTImage("add-place", Gtk::ICON_SIZE_MENU)));
    addBtn->set_relief(Gtk::RELIEF_NONE);
    addBtn->set_tooltip_text(M("ALBUM_CREATE_TOOLTIP"));

    addMenu_ = Gtk::manage(new Gtk::Menu());
    auto* miCreateAlbum = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
    miCreateAlbum->signal_activate().connect([this]() { createAlbum(); });
    addMenu_->append(*miCreateAlbum);

    auto* miCreateSmart = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
    miCreateSmart->signal_activate().connect([this]() { createSmartAlbum(); });
    addMenu_->append(*miCreateSmart);

    auto* miCreateFolder = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_FOLDER")));
    miCreateFolder->signal_activate().connect([this]() { createFolder(); });
    addMenu_->append(*miCreateFolder);

    addMenu_->show_all();
    addBtn->set_popup(*addMenu_);

    headerBar->pack_end(*addBtn, Gtk::PACK_SHRINK);

    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "#AlbumHeader { min-height: 0; padding: 0 4px; }"
        "#AlbumHeader label { font-size: 10px; font-weight: bold; padding: 2px 0; margin: 0; }"
        "#AlbumAddBtn { min-height: 0; min-width: 0; padding: 0; margin: 0; }"
        "#AlbumBrowserTree { font-size: 0.92em; }"
        "#AlbumBrowserTree header { min-height: 0; padding: 0; }"
        "#AlbumNewBtn { min-height: 0; min-width: 0; padding: 1px 4px; margin: 0; font-size: 0.85em; }"
    );
    headerBar->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    headerLabel->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    addBtn->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

    pack_start(*headerBar, Gtk::PACK_SHRINK, 0);

    // Separator
    Gtk::Separator* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    pack_start(*sep, Gtk::PACK_SHRINK, 0);

    // Scrolled tree view
    scrollw_ = Gtk::manage(new Gtk::ScrolledWindow());
    scrollw_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrollw_->set_propagate_natural_height(true);
    scrollw_->set_max_content_height(120);

    treeView_ = Gtk::manage(new Gtk::TreeView());
    treeView_->set_name("AlbumBrowserTree");
    treeView_->set_headers_visible(false);
    treeView_->set_can_focus(false);
    treeView_->get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);

    model_ = Gtk::TreeStore::create(columns_);
    treeView_->set_model(model_);

    // Name column — set to expand so album names aren't truncated
    Gtk::CellRendererText* nameCR = Gtk::manage(new Gtk::CellRendererText());
    nameCR->property_ellipsize() = Pango::ELLIPSIZE_END;
    Gtk::TreeView::Column* nameCol = Gtk::manage(new Gtk::TreeView::Column(""));
    nameCol->pack_start(*nameCR, true);
    nameCol->add_attribute(*nameCR, "text", columns_.name);
    nameCol->set_expand(true);  // <-- FIX: let name column take all available space
    nameCol->set_cell_data_func(*nameCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        bool isTarget = (*iter)[columns_.isTarget];
        int nodeType = (*iter)[columns_.nodeType];
        textCR->property_weight() = isTarget ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
        textCR->property_style() = (nodeType == static_cast<int>(AlbumNodeType::SMART_ALBUM))
            ? Pango::STYLE_ITALIC : Pango::STYLE_NORMAL;
    });
    treeView_->append_column(*nameCol);

    // Count column — fixed width, right-aligned
    Gtk::CellRendererText* countCR = Gtk::manage(new Gtk::CellRendererText());
    countCR->property_foreground() = "#888888";
    countCR->property_xalign() = 1.0;
    Gtk::TreeView::Column* countCol = Gtk::manage(new Gtk::TreeView::Column(""));
    countCol->pack_start(*countCR, false);
    countCol->set_expand(false);
    countCol->set_cell_data_func(*countCR, [this](Gtk::CellRenderer* cr, const Gtk::TreeModel::iterator& iter) {
        auto* textCR = static_cast<Gtk::CellRendererText*>(cr);
        int nodeType = (*iter)[columns_.nodeType];
        int count = (*iter)[columns_.count];
        if (nodeType != static_cast<int>(AlbumNodeType::FOLDER)) {
            textCR->property_text() = Glib::ustring::compose("%1", count);
        } else {
            textCR->property_text() = "";
        }
    });
    treeView_->append_column(*countCol);

    scrollw_->add(*treeView_);
    pack_start(*scrollw_, Gtk::PACK_SHRINK);
    set_margin_bottom(8);

    treeView_->signal_button_press_event().connect(sigc::mem_fun(*this, &AlbumBrowser::onButtonPress), false);
    treeView_->get_selection()->signal_changed().connect(sigc::mem_fun(*this, &AlbumBrowser::onSelectionChanged));

    loadAlbums();
    refreshTree();

    show_all();
}

AlbumBrowser::~AlbumBrowser ()
{
    globalChangeConn_.disconnect();
}

void AlbumBrowser::onGlobalAlbumsChanged (AlbumBrowser* sender)
{
    if (sender == this) return;  // Don't reload ourselves
    loadAlbums();
    refreshTree();
}

Glib::ustring AlbumBrowser::getAlbumsFilePath () const
{
    return Glib::build_filename(Options::rtdir, "albums");
}

void AlbumBrowser::loadAlbums ()
{
    nodes_.clear();
    targetAlbumName_.clear();
    nextNodeId_ = 0;

    Glib::ustring fpath = getAlbumsFilePath();

    try {
        Glib::KeyFile kf;
        if (!kf.load_from_file(fpath)) return;

        if (kf.has_key("Albums", "Target")) {
            targetAlbumName_ = kf.get_string("Albums", "Target");
        }

        // Try new format first (with Node_ sections)
        bool newFormat = false;
        for (const auto& group : kf.get_groups()) {
            if (group.substr(0, 5) == "Node_") {
                newFormat = true;
                break;
            }
        }

        if (newFormat) {
            for (const auto& group : kf.get_groups()) {
                if (group.substr(0, 5) != "Node_") continue;

                AlbumNode node;
                node.id = std::stoi(group.substr(5));
                if (node.id >= nextNodeId_) nextNodeId_ = node.id + 1;

                node.type = AlbumNodeType::ALBUM;
                if (kf.has_key(group, "Type")) {
                    Glib::ustring t = kf.get_string(group, "Type");
                    if (t == "folder") node.type = AlbumNodeType::FOLDER;
                    else if (t == "smart") node.type = AlbumNodeType::SMART_ALBUM;
                }

                node.name = kf.has_key(group, "Name") ? kf.get_string(group, "Name") : "";
                node.parentId = kf.has_key(group, "Parent") ? kf.get_integer(group, "Parent") : -1;

                if (node.type == AlbumNodeType::ALBUM && kf.has_key(group, "Files")) {
                    node.filePaths = kf.get_string_list(group, "Files");
                }

                if (node.type == AlbumNodeType::SMART_ALBUM) {
                    if (kf.has_key(group, "Rules")) {
                        node.rules = deserializeRules(kf.get_string(group, "Rules"));
                    }
                    node.matchAll = true;
                    if (kf.has_key(group, "MatchAll")) {
                        node.matchAll = kf.get_boolean(group, "MatchAll");
                    }
                }

                if (!node.name.empty()) {
                    nodes_.push_back(std::move(node));
                }
            }
        } else {
            // Legacy format: Album_0, Album_1, ...
            int count = 0;
            if (kf.has_key("Albums", "Count")) {
                count = kf.get_integer("Albums", "Count");
            }

            for (int i = 0; i < count; i++) {
                Glib::ustring section = Glib::ustring::compose("Album_%1", i);
                if (!kf.has_group(section)) continue;

                AlbumNode node;
                node.id = nextNodeId_++;
                node.type = AlbumNodeType::ALBUM;
                node.parentId = -1;
                node.matchAll = true;

                if (kf.has_key(section, "Name")) {
                    node.name = kf.get_string(section, "Name");
                }
                if (kf.has_key(section, "Files")) {
                    node.filePaths = kf.get_string_list(section, "Files");
                }
                if (!node.name.empty()) {
                    nodes_.push_back(std::move(node));
                }
            }
        }
    } catch (...) {
        // File doesn't exist yet or is malformed
    }
}

void AlbumBrowser::saveAlbums ()
{
    Glib::KeyFile kf;

    kf.set_string("Albums", "Target", targetAlbumName_);

    for (const auto& node : nodes_) {
        Glib::ustring section = Glib::ustring::compose("Node_%1", node.id);

        Glib::ustring typeStr = "album";
        if (node.type == AlbumNodeType::FOLDER) typeStr = "folder";
        else if (node.type == AlbumNodeType::SMART_ALBUM) typeStr = "smart";

        kf.set_string(section, "Type", typeStr);
        kf.set_string(section, "Name", node.name);
        kf.set_integer(section, "Parent", node.parentId);

        if (node.type == AlbumNodeType::ALBUM && !node.filePaths.empty()) {
            Glib::ArrayHandle<Glib::ustring> arr(node.filePaths);
            kf.set_string_list(section, "Files", arr);
        }

        if (node.type == AlbumNodeType::SMART_ALBUM) {
            kf.set_string(section, "Rules", serializeRules(node.rules));
            kf.set_boolean(section, "MatchAll", node.matchAll);
        }
    }

    try {
        Glib::ustring data = kf.to_data();
        Glib::ustring fpath = getAlbumsFilePath();
        std::ofstream f(fpath.c_str());
        f << data;
    } catch (...) {}

    // Notify all other AlbumBrowser instances to reload
    albumsChangedOnDisk_.emit(this);
}

void AlbumBrowser::refreshTree ()
{
    model_->clear();
    addChildrenToTree(-1, nullptr);
    treeView_->expand_all();
}

void AlbumBrowser::addChildrenToTree (int parentId, const Gtk::TreeModel::Row* parentRow)
{
    for (const auto& node : nodes_) {
        if (node.parentId != parentId) continue;

        Gtk::TreeModel::Row row;
        if (parentRow) {
            row = *(model_->append(parentRow->children()));
        } else {
            row = *(model_->append());
        }

        row[columns_.name] = node.name;
        row[columns_.count] = getAlbumFileCount(node);
        row[columns_.nodeType] = static_cast<int>(node.type);
        row[columns_.isTarget] = (node.name == targetAlbumName_ && node.type == AlbumNodeType::ALBUM);
        row[columns_.nodeId] = node.id;

        // Recurse for folders
        if (node.type == AlbumNodeType::FOLDER) {
            addChildrenToTree(node.id, &row);
        }
    }
}

int AlbumBrowser::getAlbumFileCount (const AlbumNode& node) const
{
    if (node.type == AlbumNodeType::ALBUM) {
        return static_cast<int>(node.filePaths.size());
    }
    return 0;
}

AlbumNode* AlbumBrowser::findNode (int id)
{
    for (auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

AlbumNode* AlbumBrowser::findNodeByName (const Glib::ustring& name)
{
    for (auto& n : nodes_) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

void AlbumBrowser::createAlbum (int parentId)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_CREATE_ALBUM"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring name = entry.get_text().c_str();
        name = name.c_str();
        if (!name.empty()) {
            AlbumNode node;
            node.id = nextNodeId_++;
            node.type = AlbumNodeType::ALBUM;
            node.name = name;
            node.parentId = parentId;
            node.matchAll = true;
            nodes_.push_back(std::move(node));
            if (targetAlbumName_.empty()) {
                targetAlbumName_ = name;
            }
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::createSmartAlbum (int parentId)
{
    Glib::ustring name;
    std::vector<SmartAlbumRule> rules;
    bool matchAll = true;

    if (showSmartAlbumDialog(name, rules, matchAll, false)) {
        AlbumNode node;
        node.id = nextNodeId_++;
        node.type = AlbumNodeType::SMART_ALBUM;
        node.name = name;
        node.parentId = parentId;
        node.rules = std::move(rules);
        node.matchAll = matchAll;
        nodes_.push_back(std::move(node));
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::createFolder (int parentId)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_CREATE_FOLDER"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring name = entry.get_text().c_str();
        name = name.c_str();
        if (!name.empty()) {
            AlbumNode node;
            node.id = nextNodeId_++;
            node.type = AlbumNodeType::FOLDER;
            node.name = name;
            node.parentId = parentId;
            node.matchAll = true;
            nodes_.push_back(std::move(node));
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::renameNode ()
{
    if (selectedNodeId_ < 0) return;

    AlbumNode* node = findNode(selectedNodeId_);
    if (!node) return;

    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return;

    Gtk::Dialog dlg(M("ALBUM_RENAME"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    Gtk::Entry entry;
    entry.set_text(node->name);
    entry.set_activates_default(true);
    dlg.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK, 8);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        Glib::ustring newName = entry.get_text().c_str();
        if (!newName.empty() && newName != node->name) {
            if (targetAlbumName_ == node->name) {
                targetAlbumName_ = newName;
            }
            if (selectedAlbumName_ == node->name) {
                selectedAlbumName_ = newName;
            }
            node->name = newName;
            saveAlbums();
            refreshTree();
        }
    }
}

void AlbumBrowser::deleteNode ()
{
    if (selectedNodeId_ < 0) return;

    // Collect all descendant IDs (for folders)
    std::vector<int> toDelete;
    std::function<void(int)> collectChildren = [&](int id) {
        toDelete.push_back(id);
        for (const auto& n : nodes_) {
            if (n.parentId == id) {
                collectChildren(n.id);
            }
        }
    };
    collectChildren(selectedNodeId_);

    // Check if target album is being deleted
    for (int id : toDelete) {
        AlbumNode* n = findNode(id);
        if (n && n->name == targetAlbumName_) {
            targetAlbumName_.clear();
        }
    }

    // Remove all nodes
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
            [&toDelete](const AlbumNode& n) {
                return std::find(toDelete.begin(), toDelete.end(), n.id) != toDelete.end();
            }),
        nodes_.end()
    );

    selectedAlbumName_.clear();
    selectedNodeId_ = -1;
    saveAlbums();
    refreshTree();

    // Clear filter
    std::set<std::string> empty;
    albumSelectedSignal_.emit(empty);
}

void AlbumBrowser::setTargetAlbum ()
{
    if (selectedAlbumName_.empty()) return;
    targetAlbumName_ = selectedAlbumName_;
    saveAlbums();
    refreshTree();
}

void AlbumBrowser::addCurrentImage ()
{
    if (selectedNodeId_ < 0 || !getCurrentFilePath_) return;

    AlbumNode* node = findNode(selectedNodeId_);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    Glib::ustring filePath = getCurrentFilePath_();
    if (!filePath.empty()) {
        addFileToAlbum(node->name, filePath);
    }
}

void AlbumBrowser::editSmartAlbumRules ()
{
    if (selectedNodeId_ < 0) return;

    AlbumNode* node = findNode(selectedNodeId_);
    if (!node || node->type != AlbumNodeType::SMART_ALBUM) return;

    Glib::ustring name = node->name;
    std::vector<SmartAlbumRule> rules = node->rules;
    bool matchAll = node->matchAll;

    if (showSmartAlbumDialog(name, rules, matchAll, true)) {
        node->name = name;
        node->rules = std::move(rules);
        node->matchAll = matchAll;
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::addFileToTargetAlbum (const Glib::ustring& filePath)
{
    if (targetAlbumName_.empty() || filePath.empty()) return;
    addFileToAlbum(targetAlbumName_, filePath);
}

void AlbumBrowser::addFileToAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath)
{
    AlbumNode* node = findNodeByName(albumName);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    // Don't add duplicates
    for (const auto& f : node->filePaths) {
        if (f == filePath) return;
    }

    node->filePaths.push_back(filePath);
    saveAlbums();
    refreshTree();
}

void AlbumBrowser::removeFileFromAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath)
{
    AlbumNode* node = findNodeByName(albumName);
    if (!node || node->type != AlbumNodeType::ALBUM) return;

    auto it = std::find(node->filePaths.begin(), node->filePaths.end(), filePath);
    if (it != node->filePaths.end()) {
        node->filePaths.erase(it);
        saveAlbums();
        refreshTree();
    }
}

void AlbumBrowser::onSelectionChanged ()
{
    auto iter = treeView_->get_selection()->get_selected();
    if (iter) {
        int nodeId = (*iter)[columns_.nodeId];
        Glib::ustring name = (*iter)[columns_.name];
        if (nodeId == selectedNodeId_) {
            // Clicking same node again -> deselect
            treeView_->get_selection()->unselect_all();
            selectedAlbumName_.clear();
            selectedNodeId_ = -1;
            std::set<std::string> empty;
            albumSelectedSignal_.emit(empty);
            albumViewSignal_.emit("", std::vector<Glib::ustring>());
            return;
        }

        selectedNodeId_ = nodeId;
        selectedAlbumName_ = name;

        AlbumNode* node = findNode(nodeId);
        if (!node) return;

        if (node->type == AlbumNodeType::ALBUM) {
            // Build whitelist from album's files
            std::set<std::string> whitelist;
            for (const auto& f : node->filePaths) {
                whitelist.insert(std::string(f.c_str()));
            }
            albumSelectedSignal_.emit(whitelist);
            albumViewSignal_.emit(node->name, node->filePaths);
        } else if (node->type == AlbumNodeType::SMART_ALBUM) {
            // Run evaluation in background thread to avoid UI freeze
            AlbumNode nodeCopy = *node;
            std::thread([this, nodeCopy]() {
                auto matchingFiles = evaluateSmartAlbum(nodeCopy);
                Glib::signal_idle().connect_once([this, matchingFiles]() {
                    std::set<std::string> whitelist;
                    for (const auto& f : matchingFiles) {
                        whitelist.insert(std::string(f.c_str()));
                    }
                    albumSelectedSignal_.emit(whitelist);
                    albumViewSignal_.emit(selectedAlbumName_, matchingFiles);
                });
            }).detach();
        } else {
            // Folders - toggle expand/collapse
            auto path = model_->get_path(iter);
            if (treeView_->row_expanded(path)) {
                treeView_->collapse_row(path);
            } else {
                treeView_->expand_row(path, false);
            }
            selectedAlbumName_.clear();
            selectedNodeId_ = -1;
            treeView_->get_selection()->unselect_all();
        }
    } else {
        selectedAlbumName_.clear();
        selectedNodeId_ = -1;
        std::set<std::string> empty;
        albumSelectedSignal_.emit(empty);
        albumViewSignal_.emit("", std::vector<Glib::ustring>());
    }
}

bool AlbumBrowser::onButtonPress (GdkEventButton* event)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        Gtk::TreeModel::Path path;
        if (treeView_->get_path_at_pos(static_cast<int>(event->x), static_cast<int>(event->y), path)) {
            treeView_->get_selection()->select(path);
            auto iter = model_->get_iter(path);
            if (iter) {
                int nodeId = (*iter)[columns_.nodeId];
                selectedNodeId_ = nodeId;
                selectedAlbumName_ = (*iter)[columns_.name];
                showContextMenu(event, nodeId);
                return true;
            }
        } else {
            // Right-click on empty area
            showContextMenu(event, -1);
            return true;
        }
    }
    return false;
}

void AlbumBrowser::showContextMenu (GdkEventButton* event, int nodeId)
{
    auto* menu = Gtk::manage(new Gtk::Menu());

    if (nodeId < 0) {
        // Empty area
        auto* mi1 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
        mi1->signal_activate().connect([this]() { createAlbum(); });
        menu->append(*mi1);

        auto* mi2 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
        mi2->signal_activate().connect([this]() { createSmartAlbum(); });
        menu->append(*mi2);

        auto* mi3 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_FOLDER")));
        mi3->signal_activate().connect([this]() { createFolder(); });
        menu->append(*mi3);
    } else {
        AlbumNode* node = findNode(nodeId);
        if (!node) return;

        if (node->type == AlbumNodeType::FOLDER) {
            auto* mi1 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_ALBUM")));
            mi1->signal_activate().connect([this, nodeId]() { createAlbum(nodeId); });
            menu->append(*mi1);

            auto* mi2 = Gtk::manage(new Gtk::MenuItem(M("ALBUM_CREATE_SMART")));
            mi2->signal_activate().connect([this, nodeId]() { createSmartAlbum(nodeId); });
            menu->append(*mi2);

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        } else if (node->type == AlbumNodeType::ALBUM) {
            auto* miAdd = Gtk::manage(new Gtk::MenuItem(M("ALBUM_ADD_CURRENT")));
            miAdd->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::addCurrentImage));
            menu->append(*miAdd);

            auto* miTarget = Gtk::manage(new Gtk::MenuItem(M("ALBUM_SET_TARGET")));
            miTarget->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::setTargetAlbum));
            menu->append(*miTarget);

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        } else if (node->type == AlbumNodeType::SMART_ALBUM) {
            auto* miEdit = Gtk::manage(new Gtk::MenuItem(M("ALBUM_EDIT_RULES")));
            miEdit->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::editSmartAlbumRules));
            menu->append(*miEdit);

            menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

            auto* miRename = Gtk::manage(new Gtk::MenuItem(M("ALBUM_RENAME")));
            miRename->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::renameNode));
            menu->append(*miRename);

            auto* miDelete = Gtk::manage(new Gtk::MenuItem(M("ALBUM_DELETE")));
            miDelete->signal_activate().connect(sigc::mem_fun(*this, &AlbumBrowser::deleteNode));
            menu->append(*miDelete);
        }
    }

    menu->show_all();
    menu->popup(event->button, event->time);
}

// ---- Smart Album Dialog ----

bool AlbumBrowser::showSmartAlbumDialog (Glib::ustring& name, std::vector<SmartAlbumRule>& rules,
                                          bool& matchAll, bool isEdit)
{
    Gtk::Window* toplevel = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!toplevel) return false;

    Gtk::Dialog dlg(M("ALBUM_SMART_DIALOG_TITLE"), *toplevel, true);
    dlg.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    dlg.set_default_size(420, -1);

    auto* content = dlg.get_content_area();

    // Name entry
    Gtk::Box* nameRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    nameRow->pack_start(*Gtk::manage(new Gtk::Label(M("ALBUM_SMART_NAME"))), Gtk::PACK_SHRINK);
    Gtk::Entry* nameEntry = Gtk::manage(new Gtk::Entry());
    nameEntry->set_text(name);
    nameRow->pack_start(*nameEntry, Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*nameRow, Gtk::PACK_SHRINK, 4);

    // Match mode
    Gtk::Box* matchRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    Gtk::ComboBoxText* matchCombo = Gtk::manage(new Gtk::ComboBoxText());
    matchCombo->append(M("ALBUM_SMART_MATCH_ALL"));
    matchCombo->append(M("ALBUM_SMART_MATCH_ANY"));
    matchCombo->set_active(matchAll ? 0 : 1);
    matchRow->pack_start(*Gtk::manage(new Gtk::Label("Match:")), Gtk::PACK_SHRINK);
    matchRow->pack_start(*matchCombo, Gtk::PACK_SHRINK);
    content->pack_start(*matchRow, Gtk::PACK_SHRINK, 4);

    // Rule list
    Gtk::Box* rulesBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));

    std::vector<Glib::ustring> fieldNames = {
        M("ALBUM_SMART_FIELD_RATING"),
        M("ALBUM_SMART_FIELD_COLOR"),
        M("ALBUM_SMART_FIELD_FILETYPE"),
        M("ALBUM_SMART_FIELD_CAMERA"),
        M("ALBUM_SMART_FIELD_LENS"),
        M("ALBUM_SMART_FIELD_ISO"),
        M("ALBUM_SMART_FIELD_FOCAL"),
        M("ALBUM_SMART_FIELD_APERTURE"),
        M("ALBUM_SMART_FIELD_EDITED")
    };

    std::vector<Glib::ustring> opNames = {">=", "<=", "is", "is not", "contains"};

    struct RuleRow {
        Gtk::Box* box;
        Gtk::ComboBoxText* fieldCombo;
        Gtk::ComboBoxText* opCombo;
        Gtk::Entry* valueEntry;
    };

    auto ruleRows = std::make_shared<std::vector<RuleRow>>();

    auto addRuleRow = [&](const SmartAlbumRule* existingRule) {
        RuleRow rr;
        rr.box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));

        rr.fieldCombo = Gtk::manage(new Gtk::ComboBoxText());
        for (const auto& fn : fieldNames) {
            rr.fieldCombo->append(fn);
        }
        rr.fieldCombo->set_active(existingRule ? static_cast<int>(existingRule->field) : 0);
        rr.box->pack_start(*rr.fieldCombo, Gtk::PACK_SHRINK);

        rr.opCombo = Gtk::manage(new Gtk::ComboBoxText());
        for (const auto& on : opNames) {
            rr.opCombo->append(on);
        }
        rr.opCombo->set_active(existingRule ? static_cast<int>(existingRule->op) : 0);
        rr.box->pack_start(*rr.opCombo, Gtk::PACK_SHRINK);

        rr.valueEntry = Gtk::manage(new Gtk::Entry());
        rr.valueEntry->set_width_chars(10);
        if (existingRule) rr.valueEntry->set_text(existingRule->value);
        rr.box->pack_start(*rr.valueEntry, Gtk::PACK_EXPAND_WIDGET);

        Gtk::Button* removeBtn = Gtk::manage(new Gtk::Button("x"));
        removeBtn->set_relief(Gtk::RELIEF_NONE);
        auto rowPtr = rr.box;
        removeBtn->signal_clicked().connect([rulesBox, rowPtr, ruleRows]() {
            rulesBox->remove(*rowPtr);
            ruleRows->erase(
                std::remove_if(ruleRows->begin(), ruleRows->end(),
                    [rowPtr](const RuleRow& r) { return r.box == rowPtr; }),
                ruleRows->end()
            );
        });
        rr.box->pack_end(*removeBtn, Gtk::PACK_SHRINK);

        ruleRows->push_back(rr);
        rulesBox->pack_start(*rr.box, Gtk::PACK_SHRINK);
        rr.box->show_all();
    };

    for (const auto& rule : rules) {
        addRuleRow(&rule);
    }
    if (rules.empty()) {
        addRuleRow(nullptr);
    }

    content->pack_start(*rulesBox, Gtk::PACK_SHRINK, 4);

    Gtk::Button* addRuleBtn = Gtk::manage(new Gtk::Button(M("ALBUM_SMART_ADD_RULE")));
    addRuleBtn->signal_clicked().connect([&addRuleRow]() { addRuleRow(nullptr); });
    content->pack_start(*addRuleBtn, Gtk::PACK_SHRINK, 4);

    dlg.show_all_children();

    if (dlg.run() == Gtk::RESPONSE_OK) {
        name = nameEntry->get_text().c_str();
        if (name.empty()) return false;

        matchAll = (matchCombo->get_active_row_number() == 0);

        rules.clear();
        for (const auto& rr : *ruleRows) {
            SmartAlbumRule rule;
            rule.field = static_cast<SmartAlbumRule::Field>(rr.fieldCombo->get_active_row_number());
            rule.op = static_cast<SmartAlbumRule::Op>(rr.opCombo->get_active_row_number());
            rule.value = rr.valueEntry->get_text();
            if (!rule.value.empty()) {
                rules.push_back(rule);
            }
        }
        return true;
    }

    return false;
}

// ---- Serialization ----

Glib::ustring AlbumBrowser::serializeRules (const std::vector<SmartAlbumRule>& rules)
{
    Glib::ustring result;
    for (size_t i = 0; i < rules.size(); i++) {
        if (i > 0) result += ";";
        result += Glib::ustring::compose("%1:%2:%3",
            static_cast<int>(rules[i].field),
            static_cast<int>(rules[i].op),
            rules[i].value);
    }
    return result;
}

std::vector<SmartAlbumRule> AlbumBrowser::deserializeRules (const Glib::ustring& str)
{
    std::vector<SmartAlbumRule> rules;
    if (str.empty()) return rules;

    std::string s = str.c_str();
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, ';')) {
        size_t pos1 = token.find(':');
        if (pos1 == std::string::npos) continue;
        size_t pos2 = token.find(':', pos1 + 1);
        if (pos2 == std::string::npos) continue;

        SmartAlbumRule rule;
        rule.field = static_cast<SmartAlbumRule::Field>(std::stoi(token.substr(0, pos1)));
        rule.op = static_cast<SmartAlbumRule::Op>(std::stoi(token.substr(pos1 + 1, pos2 - pos1 - 1)));
        rule.value = token.substr(pos2 + 1);
        rules.push_back(rule);
    }
    return rules;
}

// ---- Smart album evaluation ----

namespace {

bool matchesRule (const SmartAlbumRule& rule, const CacheImageData& cfs)
{
    double numVal = 0;
    Glib::ustring strVal;

    switch (rule.field) {
        case SmartAlbumRule::RATING:    numVal = cfs.rating; break;
        case SmartAlbumRule::COLOR_LABEL: numVal = cfs.colorLabel; break;
        case SmartAlbumRule::ISO:       numVal = cfs.iso; break;
        case SmartAlbumRule::FOCAL_LEN: numVal = cfs.focalLen; break;
        case SmartAlbumRule::APERTURE:  numVal = cfs.fnumber; break;
        case SmartAlbumRule::CAMERA:    strVal = cfs.camMake + " " + cfs.camModel; break;
        case SmartAlbumRule::LENS:      strVal = cfs.lens; break;
        case SmartAlbumRule::FILE_TYPE: strVal = cfs.filetype; break;
        case SmartAlbumRule::EDITED:    numVal = cfs.recentlySaved ? 1 : 0; break;
    }

    double ruleNum = 0;
    try { ruleNum = std::stod(std::string(rule.value.c_str())); } catch (...) {}

    bool isStringField = (rule.field == SmartAlbumRule::CAMERA ||
                          rule.field == SmartAlbumRule::LENS ||
                          rule.field == SmartAlbumRule::FILE_TYPE);

    if (isStringField) {
        Glib::ustring lhs = strVal.lowercase();
        Glib::ustring rhs = Glib::ustring(rule.value).lowercase();
        switch (rule.op) {
            case SmartAlbumRule::EQ:       return lhs == rhs;
            case SmartAlbumRule::NEQ:      return lhs != rhs;
            case SmartAlbumRule::CONTAINS: return lhs.find(rhs) != Glib::ustring::npos;
            default: return false;
        }
    } else {
        switch (rule.op) {
            case SmartAlbumRule::GTE: return numVal >= ruleNum;
            case SmartAlbumRule::LTE: return numVal <= ruleNum;
            case SmartAlbumRule::EQ:  return numVal == ruleNum;
            case SmartAlbumRule::NEQ: return numVal != ruleNum;
            default: return false;
        }
    }
}

std::vector<Glib::ustring> listImageFiles (const Glib::ustring& dirPath)
{
    std::vector<Glib::ustring> result;
    try {
        auto dir = Gio::File::create_for_path(dirPath);
        if (!dir->query_exists()) return result;

        const auto& exts = App::get().options().parsedExtensionsSet;
        auto enumerator = dir->enumerate_children("standard::name,standard::type");
        while (auto info = enumerator->next_file()) {
            if (info->get_file_type() != Gio::FILE_TYPE_REGULAR) continue;
            Glib::ustring name = info->get_name();
            auto dotPos = name.rfind('.');
            if (dotPos == Glib::ustring::npos) continue;
            std::string ext = Glib::ustring(name.substr(dotPos + 1)).lowercase();
            if (exts.count(ext)) {
                result.push_back(Glib::build_filename(dirPath, name));
            }
        }
    } catch (...) {}
    return result;
}

} // anonymous namespace

namespace {

// Try to load CacheImageData from the cache file without creating a Thumbnail.
// Returns true if cache data was found and loaded.
bool loadCacheDataForFile (const Glib::ustring& fpath, CacheImageData& cid)
{
    std::string md5 = CacheManager::getMD5(fpath);
    if (md5.empty()) return false;

    Glib::ustring cacheName = cacheMgr->getCacheFileName("data", fpath, ".txt", md5);
    return cid.load(cacheName) == 0 && cid.supported;
}

// Lightweight EXIF-only read. Does NOT generate thumbnails.
bool loadExifForFile (const Glib::ustring& fpath, CacheImageData& cid)
{
    try {
        std::unique_ptr<rtengine::FramesMetaData> meta(
            rtengine::FramesMetaData::fromFile(fpath));
        if (!meta) return false;

        cid.exifValid = meta->hasExif();
        if (cid.exifValid) {
            cid.fnumber = meta->getFNumber();
            cid.shutter = meta->getShutterSpeed();
            cid.focalLen = meta->getFocalLen();
            cid.iso = meta->getISOSpeed();
            cid.lens = meta->getLens();
            cid.camMake = meta->getMake();
            cid.camModel = meta->getModel();
            cid.rating = meta->getRating();
            cid.colorLabel = meta->getColorLabel();
        }
        cid.filetype = getExtension(fpath).lowercase();
        cid.supported = true;
        return true;
    } catch (...) {
        return false;
    }
}

} // anonymous namespace

std::vector<Glib::ustring> AlbumBrowser::evaluateSmartAlbum (const AlbumNode& node)
{
    std::vector<Glib::ustring> matches;
    if (node.type != AlbumNodeType::SMART_ALBUM || node.rules.empty()) return matches;

    // Build list of directories to scan: favorites + current directory
    std::set<Glib::ustring> dirsToScan;
    for (const auto& d : App::get().options().favoriteDirs) {
        dirsToScan.insert(d);
    }
    if (!currentDirectory_.empty()) {
        dirsToScan.insert(currentDirectory_);
    }

    for (const auto& dir : dirsToScan) {
        auto files = listImageFiles(dir);
        for (const auto& fpath : files) {
            CacheImageData cid;

            // Fast path: try loading from cache file (no thumbnail generation)
            if (!loadCacheDataForFile(fpath, cid)) {
                // Slow path: read EXIF directly (still much cheaper than thumbnail)
                if (!loadExifForFile(fpath, cid)) continue;
            }

            bool match = node.matchAll;

            for (const auto& rule : node.rules) {
                bool ruleResult = matchesRule(rule, cid);
                if (node.matchAll) {
                    match = match && ruleResult;
                    if (!match) break;
                } else {
                    match = match || ruleResult;
                    if (match) break;
                }
            }

            if (match) {
                matches.push_back(fpath);
            }
        }
    }
    return matches;
}

// ---- Backward compatibility ----

std::vector<AlbumEntry> AlbumBrowser::getAlbums () const
{
    std::vector<AlbumEntry> result;
    for (const auto& node : nodes_) {
        if (node.type == AlbumNodeType::ALBUM) {
            AlbumEntry e;
            e.name = node.name;
            e.filePaths = node.filePaths;
            result.push_back(std::move(e));
        }
    }
    return result;
}
