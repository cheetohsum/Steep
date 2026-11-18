/*
 *  This file is part of RawTherapee / Steep.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include "dircleanupwindow.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <glib/gstdio.h>

#include "albumbrowser.h"
#include "backgroundtasks.h"
#include "cachemanager.h"
#include "dirbrowser.h"
#include "guiutils.h"
#include "multilangmgr.h"
#include "options.h"
#include "phototags.h"
#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <shellapi.h>
#endif
#include "thumbnail.h"
#include "rtengine/iimage.h"

using namespace dircleanup;

namespace
{

DirCleanupWindow* instance = nullptr;

// One folder (or one group of files) per job, never more than one of the I/O
// lane's two threads: thumbnails and the edit pane's strips keep the other.
steep::SerialQueue& scanQueue()
{
    static steep::SerialQueue* const q = new steep::SerialQueue(steep::Lane::IO);
    return *q;
}

steep::SerialQueue& moveQueue()
{
    static steep::SerialQueue* const q = new steep::SerialQueue(steep::Lane::IO);
    return *q;
}

std::string journalDir()
{
    return Glib::build_filename(Options::rtdir, "cleanup");
}

std::string indexFile()
{
    return Glib::build_filename(Options::cacheBaseDir, "cleanup-index.tsv");
}

const char* monthAbbrev(int m)
{
    static const char* const n[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return n[(m - 1) % 12];
}

// "Jul 8–28", "Jul 30 – Aug 2", "Jul 8"; the year only when it is not obvious.
Glib::ustring dateRange(int first, int last)
{
    int y1, m1, d1, y2, m2, d2;
    dayToYMD(first, y1, m1, d1);
    dayToYMD(last, y2, m2, d2);
    char buf[64];
    if (first == last) {
        std::snprintf(buf, sizeof(buf), "%s %d", monthAbbrev(m1), d1);
    } else if (y1 == y2 && m1 == m2) {
        std::snprintf(buf, sizeof(buf), "%s %d–%d", monthAbbrev(m1), d1, d2);
    } else if (y1 == y2) {
        std::snprintf(buf, sizeof(buf), "%s %d – %s %d", monthAbbrev(m1), d1, monthAbbrev(m2), d2);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %d %d – %s %d %d", monthAbbrev(m1), d1, y1, monthAbbrev(m2), d2, y2);
    }
    return buf;
}

Glib::ustring humanBytes(long long b)
{
    char buf[32];
    if (b >= (1LL << 30)) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", b / double(1LL << 30));
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f MB", b / double(1LL << 20));
    }
    return buf;
}

// Explorer with the photo selected (other platforms: its folder).
void revealInFileManager(const std::string& path)
{
#ifdef _WIN32
    std::string p = path;
    std::replace(p.begin(), p.end(), '/', '\\');
    const std::string argument = "/select,\"" + p + "\"";
    if (wchar_t* wide = reinterpret_cast<wchar_t*>(g_utf8_to_utf16(argument.c_str(), -1, nullptr, nullptr, nullptr))) {
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", wide, nullptr, SW_SHOWNORMAL);
        g_free(wide);
        if (reinterpret_cast<INT_PTR>(r) > 32) {
            return;
        }
    }
#endif
    try {
        Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(Glib::path_get_dirname(path)));
    } catch (...) {}
}

Glib::ustring captureText(const Group& g)
{
    if (g.day < 0) {
        return Glib::ustring();
    }
    int y, m, d;
    dayToYMD(g.day, y, m, d);
    char buf[48];
    if (g.minute >= 0) {
        std::snprintf(buf, sizeof(buf), "%s %d %d, %02d:%02d", monthAbbrev(m), d, y, g.minute / 60, g.minute % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %d %d", monthAbbrev(m), d, y);
    }
    return buf;
}

bool isMediaName(const std::string& base)
{
    const size_t dot = base.find('.');
    return dot != std::string::npos && dot == base.rfind('.');
}

bool moveOne(const std::string& from, const std::string& to, bool isDir)
{
    if (isDir) {
        return g_rename(from.c_str(), to.c_str()) == 0;
    }
    try {
        // Across drives Gio copies, then deletes the source.
        return Gio::File::create_for_path(from)->move(Gio::File::create_for_path(to), Gio::FILE_COPY_NONE);
    } catch (...) {
        return false;
    }
}

} // namespace

struct DirCleanupWindow::State {
    std::atomic<int> generation{0};
    std::mutex mutex;
    // path -> "size\tmtime\tday\tminute": dates already read, so a rescan
    // costs a stat per photo.
    std::map<std::string, std::string> index;
    bool indexLoaded = false;
    bool indexDirty = false;
    std::vector<FolderScan> scans;
    int total = 0;
    int done = 0;
};

sigc::signal<void>& DirCleanupWindow::filesMoved ()
{
    static sigc::signal<void> sig;
    return sig;
}

void DirCleanupWindow::open (Gtk::Window* parent)
{
    if (!instance) {
        instance = new DirCleanupWindow();
    }
    if (parent) {
        instance->set_transient_for(*parent);
    }
    instance->present();
    if (instance->suggestions_.empty() && !instance->busy_) {
        instance->startScan();
    }
}

DirCleanupWindow::DirCleanupWindow ()
    : state_(std::make_shared<State>())
{
    set_title(M("DIRCLEANUP_TITLE"));
    set_name("DirCleanupWindow");
    set_default_size(900, 880);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_DIALOG);

    auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    box->set_border_width(10);

    progress_ = Gtk::manage(new Gtk::ProgressBar());
    progress_->set_show_text(true);
    progress_->set_no_show_all(true);
    box->pack_start(*progress_, Gtk::PACK_SHRINK);

    store_ = Gtk::ListStore::create(cols_);
    view_ = Gtk::manage(new Gtk::TreeView(store_));
    view_->set_headers_visible(true);
    view_->set_enable_search(false);
    view_->set_has_tooltip(true);
    view_->signal_query_tooltip().connect(sigc::mem_fun(*this, &DirCleanupWindow::onQueryTooltip));
    view_->get_selection()->set_mode(Gtk::SELECTION_SINGLE);
    view_->get_selection()->signal_changed().connect(sigc::mem_fun(*this, &DirCleanupWindow::showThumbs));
    view_->signal_button_press_event().connect(sigc::mem_fun(*this, &DirCleanupWindow::onListButton), false);

    auto* toggle = Gtk::manage(new Gtk::CellRendererToggle());
    toggle->signal_toggled().connect([this](const Glib::ustring& path) {
        auto it = store_->get_iter(path);
        if (it && (*it)[cols_.movable]) {
            (*it)[cols_.active] = !(*it)[cols_.active];
            lastResult_.clear();
            updateSummary();
        }
    });
    view_->append_column("", *toggle);
    view_->get_column(0)->add_attribute(toggle->property_active(), cols_.active);
    view_->get_column(0)->add_attribute(toggle->property_activatable(), cols_.movable);
    view_->get_column(0)->add_attribute(toggle->property_sensitive(), cols_.movable);

    auto addText = [this](const Glib::ustring& title, const Gtk::TreeModelColumn<Glib::ustring>& col,
                          float xalign, bool expand, bool dim) {
        auto* cr = Gtk::manage(new Gtk::CellRendererText());
        cr->property_xalign() = xalign;
        cr->property_xpad() = 6;
        if (expand) {
            cr->property_ellipsize() = Pango::ELLIPSIZE_END;
        }
        if (dim) {
            cr->property_foreground_rgba() = themeColor(*this, "steep_text_dim", Gdk::RGBA("#8e8e96"));
        }
        auto* column = Gtk::manage(new Gtk::TreeViewColumn(title));
        column->pack_start(*cr, true);
        column->add_attribute(cr->property_markup(), col);
        column->set_expand(expand);
        column->set_resizable(expand);
        column->set_alignment(xalign);
        view_->append_column(*column);
        return column;
    };
    fromCol_ = addText(M("DIRCLEANUP_COL_FROM"), cols_.from, 0.f, true, true);
    addText("", cols_.arrow, 0.5f, false, true);
    toCol_ = addText(M("DIRCLEANUP_COL_TO"), cols_.to, 0.f, true, false);
    addText(M("DIRCLEANUP_COL_PHOTOS"), cols_.count, 1.f, false, false);
    addText(M("DIRCLEANUP_COL_DATES"), cols_.dates, 0.f, false, true);
    dotsCol_ = addText(M("DIRCLEANUP_COL_CONFIDENCE"), cols_.dots, 0.5f, false, false);
    reasonCol_ = addText(M("DIRCLEANUP_COL_REASON"), cols_.reason, 0.5f, false, true);

    auto* scroll = Gtk::manage(new Gtk::ScrolledWindow());
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->add(*view_);

    empty_ = Gtk::manage(new Gtk::Label());
    empty_->get_style_context()->add_class("dim-label");

    stack_ = Gtk::manage(new Gtk::Stack());
    stack_->add(*scroll, "list");
    stack_->add(*empty_, "empty");

    // The photos the clicked row would move.
    thumbs_ = Gtk::manage(new Gtk::FlowBox());
    thumbs_->set_selection_mode(Gtk::SELECTION_NONE);
    thumbs_->set_homogeneous(true);
    thumbs_->set_max_children_per_line(64);
    thumbs_->set_row_spacing(4);
    thumbs_->set_column_spacing(4);
    thumbs_->set_valign(Gtk::ALIGN_START);
    thumbScroll_ = Gtk::manage(new Gtk::ScrolledWindow());
    thumbScroll_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    thumbScroll_->add(*thumbs_);
    thumbScroll_->set_size_request(-1, 220);

    // Conflict compare: this copy | the other copy, above the strip.
    auto* cmpBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    cmpBox->set_name("CleanupCompare");
    cmpBox->set_border_width(8);
    auto* sides = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
    sides->set_halign(Gtk::ALIGN_CENTER);
    for (int k = 0; k < 2; ++k) {
        auto* column = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
        cmpImg_[k] = Gtk::manage(new Gtk::Image());
        cmpImg_[k]->set_size_request(360, 240);
        cmpSide_[k] = Gtk::manage(new Gtk::EventBox());
        cmpSide_[k]->set_name("CleanupCompareSide");
        cmpSide_[k]->add(*cmpImg_[k]);
        cmpSide_[k]->add_events(Gdk::BUTTON_PRESS_MASK);
        cmpSide_[k]->signal_button_press_event().connect([this, k](GdkEventButton* ev) -> bool {
            if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS) {
                chooseSide(k, (ev->state & GDK_SHIFT_MASK) != 0);
                return true;
            }
            return false;
        });
        auto* caption = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        caption->set_halign(Gtk::ALIGN_CENTER);
        cmpName_[k] = Gtk::manage(new Gtk::Label());
        cmpName_[k]->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
        cmpName_[k]->set_max_width_chars(28);
        cmpNewer_[k] = Gtk::manage(new Gtk::Label("\u25cf"));
        cmpNewer_[k]->set_name("CleanupNewerDot");
        cmpNewer_[k]->set_no_show_all(true);
        cmpPin_[k] = Gtk::manage(new Gtk::ToggleButton(M("DIRCLEANUP_ALWAYS")));
        cmpPin_[k]->set_relief(Gtk::RELIEF_NONE);
        cmpPin_[k]->get_style_context()->add_class("smallbutton");
        cmpPin_[k]->signal_toggled().connect([this, k]() {
            if (comparePinSync_ || thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
                return;
            }
            const auto& s = suggestions_[thumbRow_];
            const std::string mine = k == 0 ? s.source : s.dest;
            const std::string other = k == 0 ? s.dest : s.source;
            if (cmpPin_[k]->get_active()) {
                setFolderPreference(mine, other);
            } else {
                clearFolderPreference(mine, mine);
            }
            if (compareIndex_ >= 0) {
                showCompare(compareIndex_, false);
            }
        });
        caption->pack_start(*cmpName_[k], Gtk::PACK_SHRINK);
        caption->pack_start(*cmpNewer_[k], Gtk::PACK_SHRINK);
        caption->pack_start(*cmpPin_[k], Gtk::PACK_SHRINK);
        column->pack_start(*cmpSide_[k], Gtk::PACK_SHRINK);
        column->pack_start(*caption, Gtk::PACK_SHRINK);
        sides->pack_start(*column, Gtk::PACK_SHRINK);
    }
    auto* diffRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    cmpDiff_ = Gtk::manage(new Gtk::Label());
    cmpDiff_->get_style_context()->add_class("dim-label");
    cmpDiff_->set_ellipsize(Pango::ELLIPSIZE_END);
    cmpProgress_ = Gtk::manage(new Gtk::Label());
    cmpProgress_->get_style_context()->add_class("dim-label");
    auto* prevBtn = Gtk::manage(new Gtk::Button("\u2039"));
    auto* nextBtn = Gtk::manage(new Gtk::Button("\u203a"));
    for (auto* b : {prevBtn, nextBtn}) {
        b->set_relief(Gtk::RELIEF_NONE);
        b->get_style_context()->add_class("smallbutton");
    }
    prevBtn->signal_clicked().connect([this]() { stepConflict(-1); });
    nextBtn->signal_clicked().connect([this]() { stepConflict(+1); });
    diffRow->pack_start(*cmpDiff_, Gtk::PACK_EXPAND_WIDGET);
    diffRow->pack_start(*cmpProgress_, Gtk::PACK_SHRINK);
    diffRow->pack_start(*prevBtn, Gtk::PACK_SHRINK);
    diffRow->pack_start(*nextBtn, Gtk::PACK_SHRINK);
    cmpBox->pack_start(*sides, Gtk::PACK_SHRINK);
    cmpBox->pack_start(*diffRow, Gtk::PACK_SHRINK);
    compareRev_ = Gtk::manage(new Gtk::Revealer());
    compareRev_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    compareRev_->add(*cmpBox);
    compareRev_->set_reveal_child(false);
    signal_key_press_event().connect(sigc::mem_fun(*this, &DirCleanupWindow::onWindowKey), false);

    auto* bottom = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    bottom->pack_start(*compareRev_, Gtk::PACK_SHRINK);
    bottom->pack_start(*thumbScroll_, Gtk::PACK_EXPAND_WIDGET);

    auto* paned = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_VERTICAL));
    paned->pack1(*stack_, true, false);
    paned->pack2(*bottom, false, false);
    paned->set_position(300);
    box->pack_start(*paned, Gtk::PACK_EXPAND_WIDGET);

    auto* bar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    selectAllBtn_ = Gtk::manage(new Gtk::Button(M("DIRCLEANUP_DESELECTALL")));
    selectAllBtn_->set_relief(Gtk::RELIEF_NONE);
    summary_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_START));
    summary_->get_style_context()->add_class("dim-label");
    rescanBtn_ = Gtk::manage(new Gtk::Button());
    rescanBtn_->set_image_from_icon_name("view-refresh-symbolic", Gtk::ICON_SIZE_BUTTON);
    rescanBtn_->set_tooltip_text(M("DIRCLEANUP_RESCAN"));
    rescanBtn_->set_relief(Gtk::RELIEF_NONE);
    undoBtn_ = Gtk::manage(new Gtk::Button(M("DIRCLEANUP_UNDO")));
    moveBtn_ = Gtk::manage(new Gtk::Button());
    moveBtn_->get_style_context()->add_class("suggested-action");
    suggestedBtn_ = Gtk::manage(new Gtk::Button(M("DIRCLEANUP_SELECTSUGGESTED")));
    suggestedBtn_->set_relief(Gtk::RELIEF_NONE);
    bar->pack_start(*suggestedBtn_, Gtk::PACK_SHRINK);
    dupBtn_ = Gtk::manage(new Gtk::Button(M("DIRCLEANUP_SELECTDUPS")));
    dupBtn_->set_relief(Gtk::RELIEF_NONE);
    bar->pack_start(*dupBtn_, Gtk::PACK_SHRINK);
    bar->pack_start(*selectAllBtn_, Gtk::PACK_SHRINK);
    bar->pack_start(*summary_, Gtk::PACK_EXPAND_WIDGET);
    bar->pack_start(*rescanBtn_, Gtk::PACK_SHRINK);
    bar->pack_start(*undoBtn_, Gtk::PACK_SHRINK);
    bar->pack_start(*moveBtn_, Gtk::PACK_SHRINK);
    box->pack_start(*bar, Gtk::PACK_SHRINK);

    selectAllBtn_->signal_clicked().connect([this]() {
        bool any = false;
        for (const auto& row : store_->children()) {
            any = any || row[cols_.active];
        }
        for (const auto& row : store_->children()) {
            row[cols_.active] = !any && !suggestions_[row[cols_.index]].duplicate;
        }
        lastResult_.clear();
        updateSummary();
    });
    dupBtn_->signal_clicked().connect([this]() {
        for (const auto& row : store_->children()) {
            if (suggestions_[row[cols_.index]].duplicate) {
                row[cols_.active] = true;
            }
        }
        lastResult_.clear();
        updateSummary();
    });
    suggestedBtn_->signal_clicked().connect([this]() {
        for (const auto& row : store_->children()) {
            const auto& s = suggestions_[row[cols_.index]];
            if (!s.duplicate && s.dots() == 3) {
                row[cols_.active] = true;
            }
        }
        lastResult_.clear();
        updateSummary();
    });
    rescanBtn_->signal_clicked().connect(sigc::mem_fun(*this, &DirCleanupWindow::startScan));
    moveBtn_->signal_clicked().connect(sigc::mem_fun(*this, &DirCleanupWindow::startMove));
    undoBtn_->signal_clicked().connect(sigc::mem_fun(*this, &DirCleanupWindow::startUndo));

    add(*box);
    box->show_all();
    updateSummary();
}

bool DirCleanupWindow::onQueryTooltip (int x, int y, bool keyboard, const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
    if (keyboard) {
        return false;
    }
    int bx = 0, by = 0;
    view_->convert_widget_to_bin_window_coords(x, y, bx, by);
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    if (!view_->get_path_at_pos(bx, by, path, column, cx, cy) || !column) {
        return false;
    }
    auto it = store_->get_iter(path);
    if (!it) {
        return false;
    }
    const auto& s = suggestions_[(*it)[cols_.index]];
    Glib::ustring text;
    if (column == fromCol_) {
        text = s.source;
    } else if (s.duplicate && (column == toCol_ || column == reasonCol_ || column == dotsCol_)) {
        text = M("DIRCLEANUP_DUPLICATE") + "\n" + s.dest;
        int conflicts = 0, unresolved = 0, differ = 0;
        for (const auto& g : s.groups) {
            const auto it = dupInfo_.find(lowerAscii(g.primary));
            if (it == dupInfo_.end()) {
                continue;
            }
            differ += it->second.mediaDiffers;
            if (!it->second.conflicts.empty()) {
                ++conflicts;
                unresolved += resolutionFor(g, s.dest) == 0;
            }
        }
        if (conflicts) {
            text += "\n" + Glib::ustring::compose(M("DIRCLEANUP_UNRESOLVED"), conflicts)
                  + (unresolved < conflicts ? Glib::ustring::compose(" (%1 \u2713)", conflicts - unresolved) : "");
        }
        if (differ) {
            text += "\n" + Glib::ustring::compose(M("DIRCLEANUP_MEDIADIFF"), differ);
        }
        for (const auto& pref : App::get().options().cleanupPreferFolders) {
            if (lowerAscii(pref.raw()) == lowerAscii(s.source) || lowerAscii(pref.raw()) == lowerAscii(s.dest)) {
                text += "\n" + Glib::ustring::compose(M("DIRCLEANUP_PREFERRING"), Glib::path_get_basename(pref));
            }
        }
    } else if (column == toCol_) {
        text = s.createDest ? M("DIRCLEANUP_CREATES") + "\n" + s.dest : Glib::ustring(s.dest);
        if (s.crossDrive) {
            text += "\n" + M("DIRCLEANUP_CROSSDRIVE");
        }
    } else if (column == reasonCol_) {
        text = s.reason == Reason::Sequence ? M("DIRCLEANUP_REASON_SEQ")
             : s.reason == Reason::FileName ? M("DIRCLEANUP_REASON_NAME") : M("DIRCLEANUP_REASON_DATE");
    } else if (column == dotsCol_) {
        text = Glib::ustring::compose("%1%%", static_cast<int>(std::lround(s.score * 100)));
    } else {
        return false;
    }
    tooltip->set_text(text);
    view_->set_tooltip_cell(tooltip, &path, column, nullptr);
    return true;
}

void DirCleanupWindow::showThumbs ()
{
    hideCompare();
    autoCompare_ = false;
    const int gen = ++(*thumbLive_);
    thumbImages_.clear();
    thumbTiles_.clear();
    thumbBadges_.clear();
    thumbRow_ = -1;
    for (auto* child : thumbs_->get_children()) {
        thumbs_->remove(*child);   // GTK destroys the managed child
    }
    auto it = view_->get_selection()->get_selected();
    if (!it || busy_) {
        return;
    }
    const int index = (*it)[cols_.index];
    if (index < 0 || index >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& groups = suggestions_[index].groups;
    constexpr int kTile = 96;
    const size_t shown = std::min<size_t>(groups.size(), 400);

    thumbImages_.clear();
    thumbTiles_.clear();
    thumbBadges_.clear();
    thumbRow_ = index;
    const bool dupRow = suggestions_[index].duplicate;
    for (size_t i = 0; i < shown; ++i) {
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(kTile, kTile);
        auto* overlay = Gtk::manage(new Gtk::Overlay());
        overlay->add(*img);
        auto* badge = Gtk::manage(new Gtk::Label());
        badge->set_halign(Gtk::ALIGN_END);
        badge->set_valign(Gtk::ALIGN_START);
        badge->set_name("CleanupConflictBadge");
        badge->set_no_show_all(true);
        overlay->add_overlay(*badge);
        auto* tile = Gtk::manage(new Gtk::EventBox());
        tile->add(*overlay);
        const std::string path = groups[i].primary;
        const int row = index;
        const size_t gi = i;
        tile->signal_button_press_event().connect([this, path, row, gi, dupRow](GdkEventButton* ev) -> bool {
            if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS && dupRow) {
                showCompare(static_cast<int>(gi));
                return false;
            }
            if (ev->button != 3 || ev->type != GDK_BUTTON_PRESS) {
                return false;
            }
            if (row >= static_cast<int>(suggestions_.size()) || gi >= suggestions_[row].groups.size()) {
                return true;
            }
            const auto& s = suggestions_[row];
            const auto& g = s.groups[gi];
            thumbMenu_.reset(new Gtk::Menu());
            auto addItem = [this](const Glib::ustring& label, std::function<void()> act) {
                auto* item = Gtk::manage(new Gtk::MenuItem(label));
                item->signal_activate().connect(act);
                thumbMenu_->append(*item);
            };
            if (dupRow) {
                const auto it = dupInfo_.find(lowerAscii(g.primary));
                if (it != dupInfo_.end() && !it->second.conflicts.empty()) {
                    const std::string key = lowerAscii(g.primary);
                    const std::string src = s.source, dst = s.dest;
                    addItem(Glib::ustring::compose(M("DIRCLEANUP_KEEPEDITS"), Glib::path_get_basename(src)),
                            [this, key]() { conflictChoice_[key] = 1; refreshDupDisplay(); });
                    addItem(Glib::ustring::compose(M("DIRCLEANUP_KEEPEDITS"), Glib::path_get_basename(dst)),
                            [this, key]() { conflictChoice_[key] = 2; refreshDupDisplay(); });
                    addItem(Glib::ustring::compose(M("DIRCLEANUP_PREFER"), Glib::path_get_basename(src)),
                            [this, src, dst]() { setFolderPreference(src, dst); });
                    addItem(Glib::ustring::compose(M("DIRCLEANUP_PREFER"), Glib::path_get_basename(dst)),
                            [this, src, dst]() { setFolderPreference(dst, src); });
                    thumbMenu_->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
                }
                const std::string other = Glib::build_filename(s.dest, Glib::path_get_basename(path));
                addItem(M("DIRCLEANUP_OPENOTHER"), [other]() { revealInFileManager(other); });
            }
            addItem(M("FILEBROWSER_POPUPOPENSOURCEFOLDER"), [path]() { revealInFileManager(path); });
            thumbMenu_->show_all();
            thumbMenu_->popup_at_pointer(reinterpret_cast<GdkEvent*>(ev));
            return true;
        });
        thumbs_->add(*tile);
        thumbImages_.push_back(img);
        tile->set_name("CleanupTile");
        thumbTiles_.push_back(tile);
        thumbBadges_.push_back(badge);
    }
    thumbs_->show_all();
    autoCompare_ = dupRow;
    refreshDupDisplay();

    // One thumbnail per job on the cleanup's own queue; a newer click
    // drops whatever is still waiting.
    std::shared_ptr<std::atomic<int>> live = thumbLive_;
    DirCleanupWindow* self = this;
    for (size_t i = 0; i < shown; ++i) {
        const Glib::ustring path = groups[i].primary;
        scanQueue().enqueue([self, gen, live, path, i]() {
            if (gen != live->load()) {
                return;
            }
            Glib::RefPtr<Gdk::Pixbuf> pixbuf;
            try {
                Thumbnail* thm = cacheMgr->getEntry(path);
                if (thm) {
                    double s = 1.0;
                    rtengine::IImage8* img = thm->processThumbImage(kTile, s);
                    if (img) {
                        pixbuf = Gdk::Pixbuf::create_from_data(img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                                                               img->getWidth(), img->getHeight(),
                                                               img->getWidth() * 3)->copy();
                        delete img;
                    }
                    thm->decreaseRef();
                }
            } catch (...) {}
            if (!pixbuf) {
                return;
            }
            // Fit inside the tile.
            const int box = kTile;
            const double f = std::min(double(box) / pixbuf->get_width(), double(box) / pixbuf->get_height());
            if (f < 1.0) {
                pixbuf = pixbuf->scale_simple(std::max(1, int(pixbuf->get_width() * f)),
                                              std::max(1, int(pixbuf->get_height() * f)), Gdk::INTERP_BILINEAR);
            }
            Glib::signal_idle().connect_once([self, gen, live, pixbuf, i]() {
                if (instance != self || gen != live->load()) {
                    return;
                }
                if (i < self->thumbImages_.size()) {
                    self->thumbImages_[i]->set(pixbuf);
                }
            });
        });
    }
}

DirCleanupWindow::~DirCleanupWindow ()
{
    ++state_->generation;
}

bool DirCleanupWindow::on_delete_event (GdkEventAny*)
{
    // Closing stops a scan in progress; a move in progress runs to the end
    // of its batch (each group is journaled, so Undo still covers it).
    if (!busy_ || progress_->get_visible()) {
        ++state_->generation;
        busy_ = false;
    }
    hide();
    return true;
}

void DirCleanupWindow::setBusy (bool busy)
{
    busy_ = busy;
    rescanBtn_->set_sensitive(!busy);
    view_->set_sensitive(!busy);
    updateSummary();
}

bool DirCleanupWindow::hasUndo () const
{
    try {
        Glib::Dir dir(journalDir());
        for (const auto& n : dir) {
            const std::string name = n;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tsv") == 0
                && name.find(".undone") == std::string::npos) {
                return true;
            }
        }
    } catch (...) {}
    return false;
}

void DirCleanupWindow::updateSummary ()
{
    int photos = 0, moves = 0, clears = 0;
    long long bytes = 0;
    bool anyDup = false;
    for (const auto& row : store_->children()) {
        if (row[cols_.index] >= static_cast<int>(suggestions_.size())) {
            continue;
        }
        const auto& s = suggestions_[row[cols_.index]];
        anyDup = anyDup || s.duplicate;
        if (row[cols_.active]) {
            photos += static_cast<int>(s.groups.size());
            (s.duplicate ? clears : moves) += static_cast<int>(s.groups.size());
            bytes += s.bytes;
        }
    }
    summary_->set_text(photos ? Glib::ustring::compose("%1 · %2", photos, humanBytes(bytes)) : lastResult_);
    moveBtn_->set_label(clears == 0 ? Glib::ustring::compose(M("DIRCLEANUP_MOVE"), moves)
                      : moves == 0 ? Glib::ustring::compose(M("DIRCLEANUP_CLEAR"), clears)
                      : Glib::ustring::compose(M("DIRCLEANUP_MOVECLEAR"), moves, clears));
    dupBtn_->set_visible(anyDup);
    dupBtn_->set_sensitive(!busy_);
    selectAllBtn_->set_label(photos ? M("DIRCLEANUP_DESELECTALL") : M("DIRCLEANUP_SELECTALL"));
    selectAllBtn_->set_sensitive(!busy_ && !suggestions_.empty());
    suggestedBtn_->set_sensitive(!busy_ && !suggestions_.empty());
    moveBtn_->set_sensitive(!busy_ && photos > 0);
    undoBtn_->set_sensitive(!busy_ && hasUndo());
}

void DirCleanupWindow::startScan ()
{
    const int gen = ++state_->generation;
    suggestions_.clear();
    store_->clear();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->scans.clear();
        state_->total = 0;
        state_->done = 0;
    }

    std::vector<std::string> roots;
    for (const auto& f : App::get().options().favoriteDirs) {
        roots.push_back(f.raw());
    }
    if (roots.empty()) {
        empty_->set_text(M("DIRCLEANUP_NOPLACES"));
        stack_->set_visible_child("empty");
        return;
    }

    setBusy(true);
    progress_->show();
    progress_->set_fraction(0.0);
    progress_->set_text("");
    stack_->set_visible_child("list");

    std::set<std::string> exts;
    for (const auto& e : App::get().options().parsedExtensionsSet) {
        exts.insert(lowerAscii(e));
    }

    auto st = state_;
    DirCleanupWindow* self = this;
    scanQueue().enqueue([st, gen, roots, exts, self]() {
        if (gen != st->generation) {
            return;
        }
        if (!st->indexLoaded) {
            std::ifstream in(indexFile());
            std::string line;
            while (std::getline(in, line)) {
                const size_t tab = line.find('\t');
                if (tab != std::string::npos) {
                    st->index[line.substr(0, tab)] = line.substr(tab + 1);
                }
            }
            st->indexLoaded = true;
        }
        const auto folders = listFolders(roots, 3);
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            st->total = static_cast<int>(folders.size());
        }

        // Dates: the index, then the thumbnail cache, then the file's own Exif.
        const DateReader reader = [st](const std::string& p, int& y, int& m, int& d, int& hh, int& mm) -> bool {
            GStatBuf sb;
            if (g_stat(p.c_str(), &sb) != 0) {
                return false;
            }
            const std::string stamp = std::to_string(static_cast<long long>(sb.st_size)) + "\t"
                                    + std::to_string(static_cast<long long>(sb.st_mtime));
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                const auto it = st->index.find(p);
                if (it != st->index.end() && it->second.compare(0, stamp.size(), stamp) == 0) {
                    int day = -1, minute = -1;
                    if (std::sscanf(it->second.c_str() + stamp.size(), "\t%d\t%d", &day, &minute) == 2 && day >= 0) {
                        dayToYMD(day, y, m, d);
                        hh = minute >= 0 ? minute / 60 : -1;
                        mm = minute >= 0 ? minute % 60 : -1;
                        return true;
                    }
                }
            }
            bool ok = false;
            const std::string md5 = cacheMgr->getMD5(p);
            if (!md5.empty()) {
                ok = readCacheDateTime(cacheMgr->getCacheFileName("data", p, ".txt", md5).raw(), y, m, d, hh, mm);
            }
            if (!ok) {
                ok = exifDateFast(p, y, m, d, hh, mm);
            }
            if (ok) {
                const int minute = hh >= 0 && mm >= 0 ? hh * 60 + mm : -1;
                std::lock_guard<std::mutex> lock(st->mutex);
                st->index[p] = stamp + "\t" + std::to_string(dayNumber(y, m, d)) + "\t" + std::to_string(minute);
                st->indexDirty = true;
            }
            return ok;
        };

        if (folders.empty()) {
            Glib::signal_idle().connect_once([st, gen, self]() {
                if (gen == st->generation && instance == self) {
                    self->scanFinished();
                }
            });
            return;
        }

        for (const auto& f : folders) {
            const std::string dir = f.first;
            const std::string root = f.second;
            scanQueue().enqueue([st, gen, dir, root, exts, reader, self]() {
                if (gen != st->generation) {
                    return;
                }
                FolderScan scan = scanFolder(dir, root, exts, reader);
                inferRunDates(scan);
                bool last = false;
                int done = 0, total = 0;
                {
                    std::lock_guard<std::mutex> lock(st->mutex);
                    st->scans.push_back(std::move(scan));
                    done = ++st->done;
                    total = st->total;
                    last = done >= total;
                }
                Glib::signal_idle().connect_once([st, gen, self, done, total, last]() {
                    if (gen != st->generation || instance != self) {
                        return;
                    }
                    self->progress_->set_fraction(total ? double(done) / total : 1.0);
                    self->progress_->set_text(Glib::ustring::compose("%1 / %2", done, total));
                    if (last) {
                        self->scanFinished();
                    }
                });
            });
        }
    });
}

void DirCleanupWindow::scanFinished ()
{
    progress_->hide();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        suggestions_ = suggest(state_->scans);
    }
    fillList();
    setBusy(false);

    // Keep the dates for next time.
    auto st = state_;
    scanQueue().enqueue([st]() {
        std::string data;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (!st->indexDirty) {
                return;
            }
            for (const auto& kv : st->index) {
                data += kv.first + "\t" + kv.second + "\n";
            }
            st->indexDirty = false;
        }
        g_mkdir_with_parents(Options::cacheBaseDir.c_str(), 0755);
        g_file_set_contents(indexFile().c_str(), data.c_str(), data.size(), nullptr);
    });
}

namespace
{
std::string rowKey(const Suggestion& s)
{
    return lowerAscii(s.source) + "|" + lowerAscii(s.dest) + (s.duplicate ? "|=" : "");
}
}

void DirCleanupWindow::fillList (bool keepTicks)
{
    std::set<std::string> ticked;
    if (keepTicks) {
        for (const auto& row : store_->children()) {
            if (row[cols_.active]) {
                ticked.insert(rowKey(previousSuggestions_[row[cols_.index]]));
            }
        }
    }
    previousSuggestions_ = suggestions_;
    store_->clear();
    for (size_t i = 0; i < suggestions_.size(); ++i) {
        const auto& s = suggestions_[i];
        auto row = *store_->append();
        row[cols_.movable] = true;
        row[cols_.active] = ticked.count(rowKey(s)) > 0;
        row[cols_.from] = Glib::Markup::escape_text(Glib::path_get_basename(s.source));
        row[cols_.arrow] = "\u2192";
        Glib::ustring to = Glib::Markup::escape_text(Glib::path_get_basename(s.dest));
        if (s.createDest) {
            to = "<b>+</b> " + to;
        }
        if (s.crossDrive) {
            to += "  <span alpha=\"55%\">" + Glib::Markup::escape_text(driveOf(s.dest)) + "</span>";
        }
        row[cols_.to] = to;
        row[cols_.count] = std::to_string(s.groups.size());
        row[cols_.dates] = Glib::Markup::escape_text(dateRange(s.firstDay, s.lastDay));
        const int dots = s.dots();
        Glib::ustring dm;
        for (int k = 0; k < 3; ++k) {
            dm += k < dots ? "\u25cf" : "<span alpha=\"25%\">\u25cf</span>";
        }
        row[cols_.dots] = dm;
        row[cols_.reason] = s.duplicate ? "=" : s.reason == Reason::Sequence ? "#"
                          : s.reason == Reason::FileName ? "Aa" : "\u25f7";
        if (s.duplicate) {
            row[cols_.dots] = "";
            row[cols_.to] = "<span alpha=\"55%\">" + Glib::ustring(row[cols_.to]) + "</span>";
        }
        row[cols_.index] = static_cast<int>(i);
    }
    analyzeDuplicates();
    showThumbs();
    if (suggestions_.empty()) {
        empty_->set_text(M("DIRCLEANUP_NONE"));
        stack_->set_visible_child("empty");
    } else {
        stack_->set_visible_child("list");
    }
    updateSummary();
}

namespace
{

struct MoveJob {
    Group group;
    std::string dest;
    bool create = false;
    bool duplicate = false;   // the same photo is already in `dest`: merge, then recycle this copy
    int resolve = 0;          // edits differ: 1 keep this copy's, 2 keep the destination's, 0 leave the photo
};

struct MoveResult {
    std::vector<std::pair<Glib::ustring, Glib::ustring>> media;   // photo paths, for tags/albums
    bool moved = false;
    bool conflict = false;   // left alone: the copies' edits differ and nothing says which wins
    Group after;   // the group at its new place
};

struct Journal {
    std::mutex mutex;
    std::string path;
    void write(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        FILE* f = g_fopen(path.c_str(), "ab");
        if (f) {
            std::fputs((line + "\n").c_str(), f);
            std::fflush(f);
            std::fclose(f);
        }
    }
};

// Same bytes? Sizes, then 64 KB at the start, middle and end.
bool sameContent(const std::string& a, const std::string& b)
{
    GStatBuf sa, sb;
    if (g_stat(a.c_str(), &sa) != 0 || g_stat(b.c_str(), &sb) != 0 || sa.st_size != sb.st_size) {
        return false;
    }
    FILE* fa = g_fopen(a.c_str(), "rb");
    FILE* fb = g_fopen(b.c_str(), "rb");
    bool same = fa && fb;
    const long long size = static_cast<long long>(sa.st_size);
    const long long block = 64 * 1024;
    std::vector<char> ba(block), bb(block);
    for (long long off : {0LL, std::max(0LL, size / 2 - block / 2), std::max(0LL, size - block)}) {
        if (!same) {
            break;
        }
        same = fseeko64(fa, off, SEEK_SET) == 0 && fseeko64(fb, off, SEEK_SET) == 0;
        const size_t na = same ? std::fread(ba.data(), 1, block, fa) : 0;
        const size_t nb = same ? std::fread(bb.data(), 1, block, fb) : 0;
        same = same && na == nb && std::memcmp(ba.data(), bb.data(), na) == 0;
    }
    if (fa) std::fclose(fa);
    if (fb) std::fclose(fb);
    return same;
}

// The photo already sits in `dest`. Carry over whatever only this copy has
// (edits, sidecars, a missing RAW or JPEG), then send the now-redundant
// files to the Recycle Bin. If both copies hold different versions of the
// same file (two different .pp3 edits), nothing is touched.
MoveResult mergeDuplicate(const MoveJob& job, Journal& journal)
{
    MoveResult result;
    const Group& g = job.group;
    std::vector<std::string> all = g.files;
    all.insert(all.end(), g.dirs.begin(), g.dirs.end());

    std::vector<std::pair<std::string, std::string>> toMove;
    std::vector<std::pair<std::string, std::string>> toTrash;     // (redundant copy, its twin in dest)
    std::vector<std::pair<std::string, std::string>> toReplace;   // ours wins: (ours, theirs to recycle)
    for (const auto& f : all) {
        const std::string target = Glib::build_filename(job.dest, Glib::path_get_basename(f));
        const bool isDir = Glib::file_test(f, Glib::FILE_TEST_IS_DIR);
        if (!Glib::file_test(target, Glib::FILE_TEST_EXISTS)) {
            if (isDir && driveOf(f) != driveOf(job.dest)) {
                return result;
            }
            toMove.emplace_back(f, target);
        } else if (!isDir && sameContent(f, target)) {
            toTrash.emplace_back(f, target);
        } else if (isDir || isMediaName(Glib::path_get_basename(f)) || job.resolve == 0) {
            // A photo file that differs, or edits with no winner: leave both.
            result.conflict = !isDir && !isMediaName(Glib::path_get_basename(f));
            return result;
        } else if (job.resolve == 2) {
            toTrash.emplace_back(f, target);      // theirs wins: ours goes to the bin
        } else {
            toReplace.emplace_back(f, target);    // ours wins: theirs goes to the bin, ours takes its place
        }
    }
    // The photo itself must be a true twin before anything goes to the bin.
    bool primaryTwin = false;
    for (const auto& t : toTrash) {
        primaryTwin = primaryTwin || t.first == g.primary;
    }
    if (!primaryTwin) {
        return result;
    }

    std::vector<std::pair<std::string, std::string>> md5s;
    for (const auto& f : g.files) {
        if (isMediaName(Glib::path_get_basename(f))) {
            md5s.emplace_back(f, cacheMgr->getMD5(f));
        }
    }
    for (const auto& r : toReplace) {
        bool trashed = false;
        try {
            trashed = Gio::File::create_for_path(r.second)->trash();
        } catch (...) {}
        if (!trashed) {
            return result;
        }
        journal.write("trash\t" + r.second + "\t" + r.first);
        if (!moveOne(r.first, r.second, false)) {
            return result;
        }
        journal.write("move\t" + r.first + "\t" + r.second);
    }
    for (const auto& mv : toMove) {
        if (!moveOne(mv.first, mv.second, Glib::file_test(mv.first, Glib::FILE_TEST_IS_DIR))) {
            return result;
        }
        journal.write("move\t" + mv.first + "\t" + mv.second);
    }
    for (const auto& t : toTrash) {
        bool trashed = false;
        try {
            trashed = Gio::File::create_for_path(t.first)->trash();
        } catch (...) {}
        if (trashed) {
            journal.write("trash\t" + t.first + "\t" + t.second);
        }
    }
    for (const auto& m : md5s) {
        const std::string to = Glib::build_filename(job.dest, Glib::path_get_basename(m.first));
        bool moved = false;
        for (const auto& mv : toMove) {
            moved = moved || mv.first == m.first;
        }
        if (moved && !m.second.empty()) {
            cacheMgr->renameEntry(m.first, m.second, to);
        }
        result.media.emplace_back(m.first, to);   // tags and albums follow to the kept copy
    }
    result.moved = true;   // result.after stays empty: dest already lists this photo
    return result;
}

MoveResult moveGroup(const MoveJob& job, Journal& journal)
{
    if (job.duplicate) {
        return mergeDuplicate(job, journal);
    }
    MoveResult result;
    const Group& g = job.group;
    if (!Glib::file_test(g.primary, Glib::FILE_TEST_EXISTS)) {
        return result;   // gone since the scan
    }
    const bool cross = driveOf(g.dir) != driveOf(job.dest);
    if (cross && !g.dirs.empty()) {
        return result;   // a folder can't be moved across drives in one step
    }
    if (!Glib::file_test(job.dest, Glib::FILE_TEST_IS_DIR)) {
        if (!job.create || g_mkdir_with_parents(job.dest.c_str(), 0755) != 0) {
            return result;
        }
        journal.write("mkdir\t" + job.dest);
    }

    // New names: the group's stem, or stem_N when anything would collide.
    const auto targetFor = [&](const std::string& from, const std::string& stem) {
        const std::string base = Glib::path_get_basename(from);
        const std::string rest = base.size() >= g.stem.size() ? base.substr(g.stem.size()) : base;
        return Glib::build_filename(job.dest, stem + rest);
    };
    std::vector<std::string> all = g.files;
    all.insert(all.end(), g.dirs.begin(), g.dirs.end());
    std::string stem = g.stem;
    for (int n = 1;; ++n) {
        bool clash = false;
        for (const auto& f : all) {
            clash = clash || Glib::file_test(targetFor(f, stem), Glib::FILE_TEST_EXISTS);
        }
        if (!clash) {
            break;
        }
        if (n == 1) {
            // The same photo already there: leave both alone.
            GStatBuf a, b;
            const std::string t = targetFor(g.primary, stem);
            if (g_stat(g.primary.c_str(), &a) == 0 && g_stat(t.c_str(), &b) == 0 && a.st_size == b.st_size) {
                return result;
            }
        }
        if (n > 50) {
            return result;
        }
        stem = g.stem + "_" + std::to_string(n);
    }

    // Cache identities before the files move.
    std::vector<std::pair<std::string, std::string>> md5s;
    for (const auto& f : g.files) {
        if (isMediaName(Glib::path_get_basename(f))) {
            md5s.emplace_back(f, cacheMgr->getMD5(f));
        }
    }

    std::vector<std::pair<std::string, std::string>> done;
    bool failed = false;
    for (const auto& f : all) {
        const bool isDir = Glib::file_test(f, Glib::FILE_TEST_IS_DIR);
        const std::string to = targetFor(f, stem);
        if (!moveOne(f, to, isDir)) {
            failed = true;
            break;
        }
        journal.write("move\t" + f + "\t" + to);
        done.emplace_back(f, to);
    }
    if (failed) {
        // All or nothing per photo: put back what already moved. (Not
        // journaled: Undo skips a "move" whose target no longer exists.)
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const bool isDir = Glib::file_test(it->second, Glib::FILE_TEST_IS_DIR);
            moveOne(it->second, it->first, isDir);
        }
        return result;
    }

    for (const auto& m : md5s) {
        const std::string to = targetFor(m.first, stem);
        if (!m.second.empty()) {
            cacheMgr->renameEntry(m.first, m.second, to);
        }
        result.media.emplace_back(m.first, to);
    }
    result.moved = true;
    result.after = g;
    result.after.dir = job.dest;
    result.after.stem = stem;
    result.after.primary = targetFor(g.primary, stem);
    for (auto& f : result.after.files) {
        f = targetFor(f, stem);
    }
    for (auto& d : result.after.dirs) {
        d = targetFor(d, stem);
    }
    return result;
}

std::string timestampName()
{
    const std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&now));
    return buf;
}

void applyRenames(const std::vector<std::pair<Glib::ustring, Glib::ustring>>& moves)
{
    if (moves.empty()) {
        return;
    }
    Glib::ustring error;
    PhotoTags::instance().renamePaths(moves, error);
    AlbumBrowser::renamePaths(moves);
    std::set<std::string> dirs;
    for (const auto& mv : moves) {
        dirs.insert(Glib::path_get_dirname(mv.first));
        dirs.insert(Glib::path_get_dirname(mv.second));
    }
    for (const auto& d : dirs) {
        DirBrowser::folderFlagsChanged().emit(d);
    }
}

} // namespace

void DirCleanupWindow::startMove ()
{
    std::vector<MoveJob> jobs;
    for (const auto& row : store_->children()) {
        if (!row[cols_.active] || !row[cols_.movable]) {
            continue;
        }
        const auto& s = suggestions_[row[cols_.index]];
        for (const auto& g : s.groups) {
            jobs.push_back({g, s.dest, s.createDest, s.duplicate, s.duplicate ? resolutionFor(g, s.dest) : 0});
        }
    }
    if (jobs.empty()) {
        return;
    }
    // Thumbnails read the same photos the mover is about to take away.
    hideCompare();
    autoCompare_ = false;
    ++(*thumbLive_);
    thumbImages_.clear();
    thumbTiles_.clear();
    thumbBadges_.clear();
    thumbRow_ = -1;
    for (auto* child : thumbs_->get_children()) {
        thumbs_->remove(*child);
    }
    setBusy(true);
    moveBtn_->set_sensitive(false);
    undoBtn_->set_sensitive(false);

    g_mkdir_with_parents(journalDir().c_str(), 0755);
    auto journal = std::make_shared<Journal>();
    journal->path = Glib::build_filename(journalDir(), timestampName() + ".tsv");

    struct Progress {
        std::mutex mutex;
        std::vector<std::pair<Glib::ustring, Glib::ustring>> media;
        std::vector<std::pair<Group, Group>> groups;   // before, after
        int moved = 0, skipped = 0, conflicts = 0, finished = 0, total = 0;
    };
    auto prog = std::make_shared<Progress>();
    prog->total = static_cast<int>(jobs.size());
    DirCleanupWindow* self = this;

    // Queued behind any thumbnail still loading (same queue, one at a time):
    // nothing reads a photo while it moves.
    scanQueue().enqueue([jobs, journal, prog, self]() {
    for (const auto& job : jobs) {
        moveQueue().enqueue([job, journal, prog, self]() {
            const MoveResult r = moveGroup(job, *journal);
            int finished = 0, total = 0, moved = 0, skipped = 0, conflicts = 0;
            std::vector<std::pair<Glib::ustring, Glib::ustring>> media;
            std::vector<std::pair<Group, Group>> groups;
            {
                std::lock_guard<std::mutex> lock(prog->mutex);
                (r.moved ? prog->moved : r.conflict ? prog->conflicts : prog->skipped)++;
                prog->media.insert(prog->media.end(), r.media.begin(), r.media.end());
                if (r.moved) {
                    prog->groups.emplace_back(job.group, r.after);
                }
                finished = ++prog->finished;
                total = prog->total;
                if (finished == total) {
                    media.swap(prog->media);
                    groups.swap(prog->groups);
                    moved = prog->moved;
                    skipped = prog->skipped;
                    conflicts = prog->conflicts;
                }
            }
            Glib::signal_idle().connect_once([self, finished, total, moved, skipped, conflicts, media, groups]() {
                if (instance != self) {
                    applyRenames(media);
                    return;
                }
                self->moveBtn_->set_label(Glib::ustring::compose("%1 / %2", finished, total));
                if (finished == total) {
                    applyRenames(media);
                    self->moveFinished(moved, skipped, groups);
                    if (conflicts) {
                        self->lastResult_ += Glib::ustring::compose("   \u2260 %1", conflicts);
                        self->updateSummary();
                    }
                }
            });
        });
    }
    });
}

void DirCleanupWindow::moveFinished (int moved, int skipped,
                                     const std::vector<std::pair<Group, Group>>& movedGroups)
{
    filesMoved().emit();
    lastResult_ = Glib::ustring::compose("✓ %1%2", moved,
                  skipped ? Glib::ustring::compose("   ✕ %1", skipped) : Glib::ustring());

    // Patch the scan instead of rescanning: each moved photo leaves its
    // folder and joins the destination (a new folder gets its own entry).
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& scans = state_->scans;
        const auto findScan = [&scans](const std::string& path) -> FolderScan* {
            const std::string key = lowerAscii(path);
            for (auto& f : scans) {
                if (lowerAscii(f.path) == key) {
                    return &f;
                }
            }
            return nullptr;
        };
        for (const auto& mv : movedGroups) {
            const Group& before = mv.first;
            const Group& after = mv.second;
            std::string root;
            if (FolderScan* src = findScan(before.dir)) {
                root = src->root;
                const std::string key = lowerAscii(before.primary);
                src->groups.erase(std::remove_if(src->groups.begin(), src->groups.end(),
                    [&key](const Group& g) { return lowerAscii(g.primary) == key; }), src->groups.end());
            }
            if (after.dir.empty()) {
                continue;   // merged into a copy the destination already lists
            }
            FolderScan* dst = findScan(after.dir);
            if (!dst) {
                FolderScan created;
                created.path = after.dir;
                created.name = Glib::path_get_basename(after.dir);
                created.parent = Glib::path_get_dirname(after.dir);
                created.root = root;
                created.info = parseFolderName(created.name);
                scans.push_back(std::move(created));
                dst = &scans.back();
            }
            dst->groups.push_back(after);
            // The date index follows the files (a move keeps size and time).
            for (size_t k = 0; k < before.files.size() && k < after.files.size(); ++k) {
                const auto it = state_->index.find(before.files[k]);
                if (it != state_->index.end()) {
                    state_->index[after.files[k]] = it->second;
                    state_->index.erase(it);
                    state_->indexDirty = true;
                }
            }
        }
        suggestions_ = suggest(scans);
    }
    fillList(true);   // rebuild the rows before anything reads them against the new list
    setBusy(false);
}

void DirCleanupWindow::startUndo ()
{
    // The newest batch not yet undone.
    std::string latest;
    try {
        Glib::Dir dir(journalDir());
        for (const auto& n : dir) {
            const std::string name = n;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tsv") == 0
                && name.find(".undone") == std::string::npos && name > latest) {
                latest = name;
            }
        }
    } catch (...) {}
    if (latest.empty()) {
        return;
    }
    setBusy(true);
    const std::string path = Glib::build_filename(journalDir(), latest);
    DirCleanupWindow* self = this;
    moveQueue().enqueue([path, self]() {
        std::vector<std::pair<std::string, std::string>> moves;
        std::vector<std::string> made;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.compare(0, 5, "move\t") == 0) {
                const size_t tab = line.find('\t', 5);
                if (tab != std::string::npos) {
                    moves.emplace_back(line.substr(5, tab - 5), line.substr(tab + 1));
                }
            } else if (line.compare(0, 6, "mkdir\t") == 0) {
                made.push_back(line.substr(6));
            }
        }
        in.close();

        std::vector<std::pair<Glib::ustring, Glib::ustring>> media;
        int undone = 0;
        for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
            const std::string& from = it->first;   // original place
            const std::string& to = it->second;    // where it went
            if (!Glib::file_test(to, Glib::FILE_TEST_EXISTS) || Glib::file_test(from, Glib::FILE_TEST_EXISTS)) {
                continue;
            }
            const bool isDir = Glib::file_test(to, Glib::FILE_TEST_IS_DIR);
            const bool media1 = !isDir && isMediaName(Glib::path_get_basename(to));
            const std::string md5 = media1 ? cacheMgr->getMD5(to) : std::string();
            if (moveOne(to, from, isDir)) {
                ++undone;
                if (media1) {
                    if (!md5.empty()) {
                        cacheMgr->renameEntry(to, md5, from);
                    }
                    media.emplace_back(to, from);
                }
            }
        }
        for (auto it = made.rbegin(); it != made.rend(); ++it) {
            g_rmdir(it->c_str());   // only succeeds when empty
        }
        g_rename(path.c_str(), (path.substr(0, path.size() - 4) + ".undone.tsv").c_str());

        Glib::signal_idle().connect_once([self, media, undone]() {
            applyRenames(media);
            if (instance != self) {
                return;
            }
            self->setBusy(false);
            filesMoved().emit();
            self->lastResult_ = Glib::ustring::compose("↶ %1", undone);
            self->startScan();
        });
    });
}

// ---------------------------------------------------------------------------
// Duplicates: what each one is, and which edits win when the copies differ
// ---------------------------------------------------------------------------

namespace
{

bool wholeFileEqual(const std::string& a, const std::string& b)
{
    gchar* da = nullptr;
    gchar* db = nullptr;
    gsize la = 0, lb = 0;
    const bool ok = g_file_get_contents(a.c_str(), &da, &la, nullptr) && g_file_get_contents(b.c_str(), &db, &lb, nullptr);
    const bool same = ok && la == lb && std::memcmp(da, db, la) == 0;
    g_free(da);
    g_free(db);
    return same;
}

// Which sections of two processing profiles differ: "Exposure, Crop, Rank".
std::string pp3Sections(const std::string& a, const std::string& b)
{
    std::vector<std::string> names;
    try {
        Glib::KeyFile ka, kb;
        ka.load_from_file(a);
        kb.load_from_file(b);
        std::set<std::string> groups;
        for (const auto& g : ka.get_groups()) groups.insert(g.raw());
        for (const auto& g : kb.get_groups()) groups.insert(g.raw());
        for (const auto& g : groups) {
            if (g == "Version") {
                continue;
            }
            std::set<std::string> keys;
            if (ka.has_group(g)) for (const auto& k : ka.get_keys(g)) keys.insert(k.raw());
            if (kb.has_group(g)) for (const auto& k : kb.get_keys(g)) keys.insert(k.raw());
            for (const auto& k : keys) {
                const std::string va = ka.has_group(g) && ka.has_key(g, k) ? ka.get_value(g, k).raw() : std::string();
                const std::string vb = kb.has_group(g) && kb.has_key(g, k) ? kb.get_value(g, k).raw() : std::string();
                if (va == vb) {
                    continue;
                }
                if (g == "General") {
                    if (k == "Rank" || k == "PickLabel" || k == "ColorLabel") {
                        names.push_back(k + " " + (va.empty() ? "-" : va) + "/" + (vb.empty() ? "-" : vb));
                    }
                    continue;
                }
                names.push_back(g);
                break;
            }
        }
    } catch (...) {
        return std::string();
    }
    std::string out;
    for (size_t i = 0; i < names.size() && i < 6; ++i) {
        out += (i ? ", " : "") + names[i];
    }
    if (names.size() > 6) {
        out += ", …";
    }
    return out;
}

DirCleanupWindow::DupInfo analyzeDuplicate(const Group& g, const std::string& dest)
{
    DirCleanupWindow::DupInfo d;
    for (const auto& f : g.files) {
        const std::string base = Glib::path_get_basename(f);
        const std::string target = Glib::build_filename(dest, base);
        if (!Glib::file_test(target, Glib::FILE_TEST_EXISTS)) {
            d.movesOver.push_back(base);
            continue;
        }
        if (isMediaName(base)) {
            // Size only here (the full check runs before anything is cleared).
            GStatBuf sa, sb;
            const bool same = g_stat(f.c_str(), &sa) == 0 && g_stat(target.c_str(), &sb) == 0 && sa.st_size == sb.st_size;
            if (same) {
                d.sameMedia = d.sameMedia || f == g.primary;
            } else {
                d.mediaDiffers = true;
            }
            continue;
        }
        if (!wholeFileEqual(f, target)) {
            d.conflicts.emplace_back(f, target);
            const size_t n = base.size();
            if (n > 4 && lowerAscii(base.substr(n - 4)) == ".pp3") {
                const std::string sec = pp3Sections(f, target);
                d.sections += (d.sections.empty() || sec.empty() ? "" : ", ") + sec;
            }
            GStatBuf sa, sb;
            if (g_stat(f.c_str(), &sa) == 0 && g_stat(target.c_str(), &sb) == 0 && sa.st_mtime != sb.st_mtime) {
                d.newer = sa.st_mtime > sb.st_mtime ? 1 : -1;
            }
        }
    }
    for (const auto& dir : g.dirs) {
        if (!Glib::file_test(Glib::build_filename(dest, Glib::path_get_basename(dir)), Glib::FILE_TEST_EXISTS)) {
            d.movesOver.push_back(Glib::path_get_basename(dir));
        }
    }
    return d;
}

} // namespace

void DirCleanupWindow::analyzeDuplicates ()
{
    const int gen = ++dupGen_;
    DirCleanupWindow* self = this;
    for (const auto& s : suggestions_) {
        if (!s.duplicate) {
            continue;
        }
        for (const auto& g : s.groups) {
            const std::string key = lowerAscii(g.primary);
            if (dupInfo_.count(key)) {
                continue;
            }
            const Group grp = g;
            const std::string dest = s.dest;
            scanQueue().enqueue([self, gen, grp, dest, key]() {
                const DupInfo info = analyzeDuplicate(grp, dest);
                Glib::signal_idle().connect_once([self, gen, key, info]() {
                    if (instance != self || gen != self->dupGen_) {
                        return;
                    }
                    self->dupInfo_[key] = info;
                    if (!self->dupRefreshConn_.connected()) {
                        self->dupRefreshConn_ = Glib::signal_timeout().connect([self]() {
                            self->refreshDupDisplay();
                            return false;
                        }, 150);
                    }
                });
            });
        }
    }
    refreshDupDisplay();
}

int DirCleanupWindow::resolutionFor (const Group& g, const std::string& dest) const
{
    const auto it = conflictChoice_.find(lowerAscii(g.primary));
    if (it != conflictChoice_.end()) {
        return it->second;
    }
    // Folder preferences: the one set most recently wins.
    const auto& prefs = App::get().options().cleanupPreferFolders;
    int srcAt = -1, dstAt = -1;
    for (size_t i = 0; i < prefs.size(); ++i) {
        const std::string p = lowerAscii(prefs[i].raw());
        if (p == lowerAscii(g.dir)) srcAt = static_cast<int>(i);
        if (p == lowerAscii(dest)) dstAt = static_cast<int>(i);
    }
    if (srcAt < 0 && dstAt < 0) {
        return 0;
    }
    return srcAt > dstAt ? 1 : 2;
}

void DirCleanupWindow::setFolderPreference (const std::string& folder, const std::string& other)
{
    auto& prefs = App::get().mut_options().cleanupPreferFolders;
    const std::string key = lowerAscii(folder);
    prefs.erase(std::remove_if(prefs.begin(), prefs.end(),
        [&key](const Glib::ustring& p) { return lowerAscii(p.raw()) == key; }), prefs.end());
    prefs.push_back(folder);
    try {
        Options::saveAsync();
    } catch (...) {}
    refreshDupDisplay();
    updateSummary();
}

void DirCleanupWindow::clearFolderPreference (const std::string& a, const std::string& b)
{
    auto& prefs = App::get().mut_options().cleanupPreferFolders;
    const std::string ka = lowerAscii(a), kb = lowerAscii(b);
    prefs.erase(std::remove_if(prefs.begin(), prefs.end(), [&](const Glib::ustring& p) {
        const std::string k = lowerAscii(p.raw());
        return k == ka || k == kb;
    }), prefs.end());
    try {
        Options::saveAsync();
    } catch (...) {}
    refreshDupDisplay();
}

Glib::ustring DirCleanupWindow::dupTooltip (const Group& g, const Suggestion& s) const
{
    Glib::ustring text = Glib::path_get_basename(g.primary);
    const Glib::ustring when = captureText(g);
    if (!when.empty()) {
        text += "\n" + when;
    }
    if (!s.duplicate) {
        return text;
    }
    text += "\n" + M("DIRCLEANUP_DUP_ALSOIN") + " " + s.dest;
    const auto it = dupInfo_.find(lowerAscii(g.primary));
    if (it == dupInfo_.end()) {
        return text;
    }
    const DupInfo& d = it->second;
    if (!d.conflicts.empty()) {
        Glib::ustring line = Glib::ustring::compose(M("DIRCLEANUP_DUP_EDITSDIFFER"),
                                                    d.sections.empty() ? Glib::ustring("pp3") : Glib::ustring(d.sections));
        if (d.newer) {
            line += " · " + (d.newer > 0 ? M("DIRCLEANUP_DUP_NEWERHERE") : M("DIRCLEANUP_DUP_NEWERTHERE"));
        }
        text += "\n" + line;
        const int r = resolutionFor(g, s.dest);
        text += "\n" + (r == 0 ? M("DIRCLEANUP_DUP_UNRESOLVED")
                       : Glib::ustring::compose(M("DIRCLEANUP_DUP_KEEPING"),
                                                Glib::path_get_basename(r == 1 ? s.source : s.dest)));
    }
    if (!d.movesOver.empty()) {
        Glib::ustring list;
        for (size_t i = 0; i < d.movesOver.size() && i < 4; ++i) {
            list += (i ? ", " : "") + d.movesOver[i];
        }
        text += "\n" + Glib::ustring::compose(M("DIRCLEANUP_DUP_MOVESOVER"), list);
    }
    if (d.mediaDiffers) {
        text += "\n" + Glib::ustring::compose(M("DIRCLEANUP_MEDIADIFF"), 1);
    }
    if (d.conflicts.empty() && d.movesOver.empty() && !d.mediaDiffers) {
        text += "\n" + M("DIRCLEANUP_DUP_IDENTICAL");
    }
    return text;
}

void DirCleanupWindow::refreshDupDisplay ()
{
    // Rows: "=" for clean duplicates, "≠N" while N photos have differing edits
    // and nothing says which to keep.
    for (const auto& row : store_->children()) {
        const int idx = row[cols_.index];
        if (idx >= static_cast<int>(suggestions_.size()) || !suggestions_[idx].duplicate) {
            continue;
        }
        const auto& s = suggestions_[idx];
        int unresolved = 0;
        for (const auto& g : s.groups) {
            const auto it = dupInfo_.find(lowerAscii(g.primary));
            if (it != dupInfo_.end() && (it->second.mediaDiffers
                                        || (!it->second.conflicts.empty() && resolutionFor(g, s.dest) == 0))) {
                ++unresolved;
            }
        }
        row[cols_.reason] = unresolved
            ? "<span foreground=\"" + themeColorHex(*view_, "steep_attention", Gdk::RGBA("#e0a03c"))
              + "\">" + Glib::ustring::compose("≠%1", unresolved) + "</span>"
            : Glib::ustring("=");
    }

    // The strip: hover text, and a badge on photos whose edits differ.
    if (thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& s = suggestions_[thumbRow_];
    for (size_t i = 0; i < thumbTiles_.size() && i < s.groups.size(); ++i) {
        const auto& g = s.groups[i];
        thumbTiles_[i]->set_tooltip_text(dupTooltip(g, s));
        Gtk::Label* badge = thumbBadges_[i];
        const auto it = s.duplicate ? dupInfo_.find(lowerAscii(g.primary)) : dupInfo_.end();
        if (it == dupInfo_.end() || (it->second.conflicts.empty() && !it->second.mediaDiffers)) {
            badge->hide();
            continue;
        }
        const int r = it->second.mediaDiffers ? 0 : resolutionFor(g, s.dest);
        auto ctx = badge->get_style_context();
        ctx->remove_class("open");
        ctx->remove_class("resolved");
        badge->set_text(r == 0 ? "≠" : r == 1 ? "A" : "B");
        ctx->add_class(r == 0 ? "open" : "resolved");
        badge->show();
    }

    // The compare follows: open on the row's first conflict once known,
    // and keep an open one's captions current.
    if (compareIndex_ >= 0) {
        showCompare(compareIndex_, false);
    } else if (autoCompare_ && !busy_) {
        const int first = nextUnresolved(-1, +1);
        if (first >= 0) {
            autoCompare_ = false;
            showCompare(first);
        }
    }
}

bool DirCleanupWindow::onListButton (GdkEventButton* ev)
{
    if (ev->button != 3 || ev->type != GDK_BUTTON_PRESS) {
        return false;
    }
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    if (!view_->get_path_at_pos(static_cast<int>(ev->x), static_cast<int>(ev->y), path, column, cx, cy)) {
        return false;
    }
    auto it = store_->get_iter(path);
    if (!it || (*it)[cols_.index] >= static_cast<int>(suggestions_.size())) {
        return false;
    }
    const auto& s = suggestions_[(*it)[cols_.index]];
    if (!s.duplicate) {
        return false;
    }
    const std::string src = s.source, dst = s.dest;
    listMenu_.reset(new Gtk::Menu());
    auto addItem = [this](const Glib::ustring& label, std::function<void()> act) {
        auto* item = Gtk::manage(new Gtk::MenuItem(label));
        item->signal_activate().connect(act);
        listMenu_->append(*item);
    };
    addItem(Glib::ustring::compose(M("DIRCLEANUP_PREFER"), Glib::path_get_basename(src)),
            [this, src, dst]() { setFolderPreference(src, dst); });
    addItem(Glib::ustring::compose(M("DIRCLEANUP_PREFER"), Glib::path_get_basename(dst)),
            [this, src, dst]() { setFolderPreference(dst, src); });
    addItem(M("DIRCLEANUP_CLEARPREF"), [this, src, dst]() { clearFolderPreference(src, dst); });
    listMenu_->show_all();
    listMenu_->popup_at_pointer(reinterpret_cast<GdkEvent*>(ev));
    return true;
}

// ---------------------------------------------------------------------------
// Conflict compare: this copy | the other copy, each with its own edits
// ---------------------------------------------------------------------------

int DirCleanupWindow::nextUnresolved (int from, int dir) const
{
    if (thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
        return -1;
    }
    const auto& s = suggestions_[thumbRow_];
    const int n = static_cast<int>(std::min(s.groups.size(), thumbTiles_.size()));
    for (int step = 1; step <= n; ++step) {
        const int i = ((from + dir * step) % n + n) % n;
        const auto it = dupInfo_.find(lowerAscii(s.groups[i].primary));
        if (it != dupInfo_.end() && !it->second.mediaDiffers && !it->second.conflicts.empty()
                && resolutionFor(s.groups[i], s.dest) == 0) {
            return i;
        }
    }
    return -1;
}

void DirCleanupWindow::hideCompare ()
{
    ++(*compareLive_);
    if (compareIndex_ >= 0 && compareIndex_ < static_cast<int>(thumbTiles_.size())) {
        thumbTiles_[compareIndex_]->get_style_context()->remove_class("current");
    }
    compareIndex_ = -1;
    if (compareRev_) {
        compareRev_->set_reveal_child(false);
    }
}

void DirCleanupWindow::showCompare (int gi, bool reload)
{
    if (busy_ || thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& s = suggestions_[thumbRow_];
    if (!s.duplicate || gi < 0 || gi >= static_cast<int>(s.groups.size())) {
        return;
    }
    const auto& g = s.groups[gi];
    const auto info = dupInfo_.find(lowerAscii(g.primary));
    if (info == dupInfo_.end() || (info->second.conflicts.empty() && !info->second.mediaDiffers)) {
        return;
    }

    if (compareIndex_ >= 0 && compareIndex_ < static_cast<int>(thumbTiles_.size())) {
        thumbTiles_[compareIndex_]->get_style_context()->remove_class("current");
    }
    compareIndex_ = gi;
    if (gi < static_cast<int>(thumbTiles_.size())) {
        thumbTiles_[gi]->get_style_context()->add_class("current");
    }

    // Captions: folder, a dot on the newer side, "Always" for that folder.
    const std::string folders[2] = {s.source, s.dest};
    const auto& prefs = App::get().options().cleanupPreferFolders;
    comparePinSync_ = true;
    for (int k = 0; k < 2; ++k) {
        cmpName_[k]->set_text(Glib::path_get_basename(folders[k]));
        cmpName_[k]->set_tooltip_text(folders[k]);
        cmpNewer_[k]->set_visible(k == 0 ? info->second.newer > 0 : info->second.newer < 0);
        bool pinned = false;
        for (const auto& p : prefs) {
            pinned = pinned || lowerAscii(p.raw()) == lowerAscii(folders[k]);
        }
        cmpPin_[k]->set_active(pinned);
        cmpPin_[k]->set_tooltip_text(Glib::ustring::compose(M("DIRCLEANUP_PREFER"), Glib::path_get_basename(folders[k])));
    }
    comparePinSync_ = false;

    // Which sections differ; ≠ alone when the photo files themselves differ.
    if (info->second.mediaDiffers) {
        cmpDiff_->set_text("≠");
    } else {
        std::string sec = info->second.sections;
        for (size_t p = sec.find(", "); p != std::string::npos; p = sec.find(", ", p + 3)) {
            sec.replace(p, 2, " · ");
        }
        cmpDiff_->set_text(sec);
    }

    int total = 0, resolved = 0;
    for (size_t i = 0; i < s.groups.size(); ++i) {
        const auto it = dupInfo_.find(lowerAscii(s.groups[i].primary));
        if (it != dupInfo_.end() && !it->second.conflicts.empty()) {
            ++total;
            resolved += resolutionFor(s.groups[i], s.dest) != 0;
        }
    }
    cmpProgress_->set_text(Glib::ustring::compose("%1 / %2", resolved, total));

    // The chosen side wears the accent ring, the other steps back.
    const int r = info->second.mediaDiffers ? 0 : resolutionFor(g, s.dest);
    for (int k = 0; k < 2; ++k) {
        auto ctx = cmpSide_[k]->get_style_context();
        const bool chosen = r == (k == 0 ? 1 : 2);
        if (chosen) {
            ctx->add_class("chosen");
        } else {
            ctx->remove_class("chosen");
        }
        cmpSide_[k]->set_opacity(r != 0 && !chosen ? 0.45 : 1.0);
        cmpSide_[k]->set_sensitive(!info->second.mediaDiffers);
    }
    compareRev_->set_reveal_child(true);

    if (!reload) {
        return;
    }
    const int gen = ++(*compareLive_);
    std::shared_ptr<std::atomic<int>> live = compareLive_;
    DirCleanupWindow* self = this;
    const std::string paths[2] = {g.primary, Glib::build_filename(s.dest, Glib::path_get_basename(g.primary))};
    for (int k = 0; k < 2; ++k) {
        cmpImg_[k]->clear();
        const std::string path = paths[k];
        scanQueue().enqueue([self, gen, live, path, k]() {
            if (gen != live->load()) {
                return;
            }
            constexpr int kBox = 360;
            Glib::RefPtr<Gdk::Pixbuf> pixbuf;
            try {
                Thumbnail* thm = cacheMgr->getEntry(path);
                if (thm) {
                    double sc = 1.0;
                    rtengine::IImage8* img = thm->processThumbImage(kBox, sc);
                    if (img) {
                        pixbuf = Gdk::Pixbuf::create_from_data(img->getData(), Gdk::COLORSPACE_RGB, false, 8,
                                                               img->getWidth(), img->getHeight(),
                                                               img->getWidth() * 3)->copy();
                        delete img;
                    }
                    thm->decreaseRef();
                }
            } catch (...) {}
            if (!pixbuf) {
                return;
            }
            const double f = std::min(double(kBox) / pixbuf->get_width(), double(kBox) / pixbuf->get_height());
            if (f < 1.0) {
                pixbuf = pixbuf->scale_simple(std::max(1, int(pixbuf->get_width() * f)),
                                              std::max(1, int(pixbuf->get_height() * f)), Gdk::INTERP_BILINEAR);
            }
            Glib::signal_idle().connect_once([self, gen, live, pixbuf, k]() {
                if (instance == self && gen == live->load()) {
                    self->cmpImg_[k]->set(pixbuf);
                }
            });
        });
    }
}

void DirCleanupWindow::chooseSide (int side, bool wholeRow)
{
    if (compareIndex_ < 0 || thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& s = suggestions_[thumbRow_];
    if (compareIndex_ >= static_cast<int>(s.groups.size())) {
        return;
    }
    const auto& g = s.groups[compareIndex_];
    const auto info = dupInfo_.find(lowerAscii(g.primary));
    if (info == dupInfo_.end() || info->second.mediaDiffers) {
        return;
    }
    const std::string key = lowerAscii(g.primary);
    const int want = side == 0 ? 1 : 2;

    if (!wholeRow) {
        const auto it = conflictChoice_.find(key);
        if (it != conflictChoice_.end() && it->second == want) {
            conflictChoice_.erase(it);   // clicking the chosen side again clears it
            refreshDupDisplay();
            updateSummary();
            showCompare(compareIndex_, false);
            return;
        }
        conflictChoice_[key] = want;
    } else {
        for (const auto& other : s.groups) {
            const auto oi = dupInfo_.find(lowerAscii(other.primary));
            if (oi != dupInfo_.end() && !oi->second.conflicts.empty() && !oi->second.mediaDiffers
                    && resolutionFor(other, s.dest) == 0) {
                conflictChoice_[lowerAscii(other.primary)] = want;
            }
        }
        conflictChoice_[key] = want;
    }
    refreshDupDisplay();
    updateSummary();

    const int next = nextUnresolved(compareIndex_, +1);
    if (next >= 0) {
        showCompare(next);
    } else {
        showCompare(compareIndex_, false);   // all decided: stay, showing the choice
    }
}

void DirCleanupWindow::stepConflict (int dir)
{
    if (compareIndex_ < 0 || thumbRow_ < 0 || thumbRow_ >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& s = suggestions_[thumbRow_];
    const int n = static_cast<int>(std::min(s.groups.size(), thumbTiles_.size()));
    for (int step = 1; step <= n; ++step) {
        const int i = ((compareIndex_ + dir * step) % n + n) % n;
        const auto it = dupInfo_.find(lowerAscii(s.groups[i].primary));
        if (it != dupInfo_.end() && (!it->second.conflicts.empty() || it->second.mediaDiffers)) {
            showCompare(i);
            return;
        }
    }
}

bool DirCleanupWindow::onWindowKey (GdkEventKey* ev)
{
    if (!compareRev_ || !compareRev_->get_reveal_child() || compareIndex_ < 0) {
        return false;
    }
    const bool shift = (ev->state & GDK_SHIFT_MASK) != 0;
    switch (ev->keyval) {
        case GDK_KEY_1: case GDK_KEY_KP_1: case GDK_KEY_exclam: case GDK_KEY_Left:
            chooseSide(0, shift || ev->keyval == GDK_KEY_exclam);
            return true;
        case GDK_KEY_2: case GDK_KEY_KP_2: case GDK_KEY_at: case GDK_KEY_Right:
            chooseSide(1, shift || ev->keyval == GDK_KEY_at);
            return true;
        case GDK_KEY_Up:
            stepConflict(-1);
            return true;
        case GDK_KEY_Down:
            stepConflict(+1);
            return true;
        case GDK_KEY_BackSpace:
            if (thumbRow_ >= 0 && thumbRow_ < static_cast<int>(suggestions_.size())
                    && compareIndex_ < static_cast<int>(suggestions_[thumbRow_].groups.size())) {
                conflictChoice_.erase(lowerAscii(suggestions_[thumbRow_].groups[compareIndex_].primary));
                refreshDupDisplay();
                updateSummary();
                showCompare(compareIndex_, false);
            }
            return true;
        case GDK_KEY_Escape:
            hideCompare();
            return true;
        default:
            return false;
    }
}
