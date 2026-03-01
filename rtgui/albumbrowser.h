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

#include <functional>
#include <set>
#include <vector>

#include <gtkmm.h>

enum class AlbumNodeType { FOLDER = 0, ALBUM = 1, SMART_ALBUM = 2 };

struct SmartAlbumRule {
    enum Field { RATING, COLOR_LABEL, FILE_TYPE, CAMERA, LENS, ISO, FOCAL_LEN, APERTURE, EDITED };
    enum Op { GTE, LTE, EQ, NEQ, CONTAINS };
    Field field;
    Op op;
    Glib::ustring value;
};

struct AlbumNode {
    int id;
    AlbumNodeType type;
    Glib::ustring name;
    int parentId;  // -1 = root
    std::vector<Glib::ustring> filePaths;   // ALBUM only
    std::vector<SmartAlbumRule> rules;       // SMART_ALBUM only
    bool matchAll;  // true = AND, false = OR for smart album rules
};

// For backward compatibility
struct AlbumEntry {
    Glib::ustring name;
    std::vector<Glib::ustring> filePaths;
};

class CacheImageData;
class Thumbnail;

class AlbumBrowser : public Gtk::Box
{
public:
    typedef sigc::signal<void, const std::set<std::string>&> AlbumSelectedSignal;
    typedef sigc::signal<void, const Glib::ustring&, const std::vector<Glib::ustring>&> AlbumViewSignal;

    AlbumBrowser ();
    ~AlbumBrowser () override;

    void loadAlbums ();
    void saveAlbums ();

    void addFileToTargetAlbum (const Glib::ustring& filePath);
    void addFileToAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath);
    void removeFileFromAlbum (const Glib::ustring& albumName, const Glib::ustring& filePath);

    void setCurrentFilePathGetter (std::function<Glib::ustring()> getter) { getCurrentFilePath_ = std::move(getter); }
    void setCurrentDirectory (const Glib::ustring& dir) { currentDirectory_ = dir; }
    Glib::ustring getTargetAlbumName () const { return targetAlbumName_; }
    Glib::ustring getSelectedAlbumName () const { return selectedAlbumName_; }
    AlbumSelectedSignal albumSelected () const { return albumSelectedSignal_; }
    AlbumViewSignal albumViewRequested () const { return albumViewSignal_; }

    std::vector<AlbumNode>& getNodes () { return nodes_; }

    // For backward compatibility
    std::vector<AlbumEntry> getAlbums () const;

private:
    class AlbumColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<int> count;
        Gtk::TreeModelColumn<int> nodeType;  // 0=folder, 1=album, 2=smart
        Gtk::TreeModelColumn<bool> isTarget;
        Gtk::TreeModelColumn<int> nodeId;
        AlbumColumns () { add(name); add(count); add(nodeType); add(isTarget); add(nodeId); }
    };

    AlbumColumns columns_;
    Gtk::ScrolledWindow* scrollw_;
    Gtk::TreeView* treeView_;
    Glib::RefPtr<Gtk::TreeStore> model_;
    Gtk::Menu* addMenu_;  // dropdown for "+" button

    std::vector<AlbumNode> nodes_;
    int nextNodeId_;
    Glib::ustring targetAlbumName_;
    Glib::ustring selectedAlbumName_;
    int selectedNodeId_;

    AlbumSelectedSignal albumSelectedSignal_;
    AlbumViewSignal albumViewSignal_;
    std::function<Glib::ustring()> getCurrentFilePath_;
    Glib::ustring currentDirectory_;

    // Global signal: emitted by any instance when albums change on disk.
    // All instances connect and reload when they hear it.
    static sigc::signal<void, AlbumBrowser*> albumsChangedOnDisk_;
    sigc::connection globalChangeConn_;
    void onGlobalAlbumsChanged (AlbumBrowser* sender);

    Glib::ustring getAlbumsFilePath () const;
    void refreshTree ();
    void addChildrenToTree (int parentId, const Gtk::TreeModel::Row* parentRow);

    // Create actions
    void createAlbum (int parentId = -1);
    void createSmartAlbum (int parentId = -1);
    void createFolder (int parentId = -1);

    // Album operations
    void renameNode ();
    void deleteNode ();
    void setTargetAlbum ();
    void addCurrentImage ();
    void editSmartAlbumRules ();

    // Smart album dialog
    bool showSmartAlbumDialog (Glib::ustring& name, std::vector<SmartAlbumRule>& rules, bool& matchAll, bool isEdit = false);

    void onSelectionChanged ();
    bool onButtonPress (GdkEventButton* event);
    void showContextMenu (GdkEventButton* event, int nodeId);

    AlbumNode* findNode (int id);
    AlbumNode* findNodeByName (const Glib::ustring& name);
    int getAlbumFileCount (const AlbumNode& node) const;

    // Smart album evaluation
    std::vector<Glib::ustring> evaluateSmartAlbum (const AlbumNode& node);

    // Serialization helpers
    static Glib::ustring serializeRules (const std::vector<SmartAlbumRule>& rules);
    static std::vector<SmartAlbumRule> deserializeRules (const Glib::ustring& str);
};
