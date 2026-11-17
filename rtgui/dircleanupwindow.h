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
            add(active); add(from); add(arrow); add(to); add(count); add(dates); add(dots); add(reason); add(index);
        }
    };

    struct State;

    void startScan ();
    void scanFinished ();
    void fillList ();
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
    bool busy_ = false;
    Glib::ustring lastResult_;   // moved / skipped counts after a batch, until the next pick
};
