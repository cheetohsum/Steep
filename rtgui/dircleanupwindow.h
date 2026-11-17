/*
 *  This file is part of RawTherapee / Steep.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <gtkmm.h>

#include "dircleanupcore.h"

// Directory cleanup: scans the starred places, lists suggested moves
// (source folder -> month folder), moves what the user ticks, and keeps a
// journal so the last batch can be undone. Opened from a right-click on the
// folders icon in the browser sidebar.
class DirCleanupWindow final : public Gtk::Window
{
public:
    static void open (Gtk::Window* parent);

    // Emitted on the GUI thread after photos were moved (or moved back).
    static sigc::signal<void>& filesMoved ();

    ~DirCleanupWindow () override;

private:
    DirCleanupWindow ();

    struct Columns : public Gtk::TreeModelColumnRecord {
        Gtk::TreeModelColumn<bool> active;
        Gtk::TreeModelColumn<bool> movable;
        Gtk::TreeModelColumn<Glib::ustring> from;
        Gtk::TreeModelColumn<Glib::ustring> arrow;
        Gtk::TreeModelColumn<Glib::ustring> to;
        Gtk::TreeModelColumn<Glib::ustring> count;
        Gtk::TreeModelColumn<Glib::ustring> dates;
        Gtk::TreeModelColumn<Glib::ustring> dots;
        Gtk::TreeModelColumn<Glib::ustring> reason;
        Gtk::TreeModelColumn<int> index;
        Columns ()
        {
            add(active); add(movable); add(from); add(arrow); add(to); add(count); add(dates); add(dots); add(reason); add(index);
        }
    };

    struct State;

public:
    // What a duplicate photo is, next to its twin in the destination.
    struct DupInfo {
        bool sameMedia = false;                        // the photo file(s) match in size
        bool mediaDiffers = false;                     // a photo file with the same name differs
        std::vector<std::string> movesOver;            // only this copy has them
        std::vector<std::pair<std::string, std::string>> conflicts;   // (here, there) edits that differ
        std::string sections;                          // which edit sections differ
        int newer = 0;                                 // +1 this copy's edits newer, -1 the other's
    };

private:

    void startScan ();
    void scanFinished ();
    void fillList (bool keepTicks = false);
    void updateSummary ();
    void startMove ();
    void moveFinished (int moved, int skipped,
                       const std::vector<std::pair<dircleanup::Group, dircleanup::Group>>& movedGroups);
    void startUndo ();
    bool hasUndo () const;
    void setBusy (bool busy);
    bool on_delete_event (GdkEventAny* event) override;
    bool onQueryTooltip (int x, int y, bool keyboard, const Glib::RefPtr<Gtk::Tooltip>& tooltip);
    void showThumbs ();
    void analyzeDuplicates ();
    void refreshDupDisplay ();
    // 0 unresolved, 1 keep this copy's edits, 2 keep the destination's
    int resolutionFor (const dircleanup::Group& g, const std::string& dest) const;
    void setFolderPreference (const std::string& folder, const std::string& other);
    void clearFolderPreference (const std::string& a, const std::string& b);
    Glib::ustring dupTooltip (const dircleanup::Group& g, const dircleanup::Suggestion& s) const;
    bool onListButton (GdkEventButton* ev);

    Columns cols_;
    Glib::RefPtr<Gtk::ListStore> store_;
    Gtk::TreeView* view_ = nullptr;
    Gtk::ProgressBar* progress_ = nullptr;
    Gtk::Label* summary_ = nullptr;
    Gtk::Label* empty_ = nullptr;
    Gtk::Button* moveBtn_ = nullptr;
    Gtk::Button* undoBtn_ = nullptr;
    Gtk::Button* rescanBtn_ = nullptr;
    Gtk::Stack* stack_ = nullptr;
    Gtk::Button* selectAllBtn_ = nullptr;
    Gtk::Button* suggestedBtn_ = nullptr;
    Gtk::Button* dupBtn_ = nullptr;
    std::map<std::string, DupInfo> dupInfo_;       // lower-case primary path -> analysis
    std::map<std::string, int> conflictChoice_;    // lower-case primary path -> 1 / 2
    int dupGen_ = 0;
    sigc::connection dupRefreshConn_;
    int thumbRow_ = -1;                             // suggestion the strip shows
    std::vector<Gtk::EventBox*> thumbTiles_;
    std::vector<Gtk::Label*> thumbBadges_;
    std::unique_ptr<Gtk::Menu> listMenu_;
    Gtk::FlowBox* thumbs_ = nullptr;
    Gtk::ScrolledWindow* thumbScroll_ = nullptr;
    Gtk::TreeViewColumn* fromCol_ = nullptr;
    Gtk::TreeViewColumn* toCol_ = nullptr;
    Gtk::TreeViewColumn* dotsCol_ = nullptr;
    Gtk::TreeViewColumn* reasonCol_ = nullptr;
    std::vector<Gtk::Image*> thumbImages_;       // index-aligned with the shown groups
    std::unique_ptr<Gtk::Menu> thumbMenu_;
    std::shared_ptr<std::atomic<int>> thumbLive_ = std::make_shared<std::atomic<int>>(0);

    std::shared_ptr<State> state_;
    std::vector<dircleanup::Suggestion> suggestions_;
    std::vector<dircleanup::Suggestion> previousSuggestions_;   // what the list rows index into
    bool busy_ = false;
    Glib::ustring lastResult_;   // moved / skipped counts after a batch, until the next pick
};
