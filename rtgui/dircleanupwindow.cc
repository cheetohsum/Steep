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
#include "multilangmgr.h"
#include "options.h"
#include "phototags.h"
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
    set_default_size(820, 640);
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

    auto* toggle = Gtk::manage(new Gtk::CellRendererToggle());
    toggle->signal_toggled().connect([this](const Glib::ustring& path) {
        auto it = store_->get_iter(path);
        if (it) {
            (*it)[cols_.active] = !(*it)[cols_.active];
            lastResult_.clear();
            updateSummary();
        }
    });
    view_->append_column("", *toggle);
    view_->get_column(0)->add_attribute(toggle->property_active(), cols_.active);

    auto addText = [this](const Glib::ustring& title, const Gtk::TreeModelColumn<Glib::ustring>& col,
                          float xalign, bool expand, bool dim) {
        auto* cr = Gtk::manage(new Gtk::CellRendererText());
        cr->property_xalign() = xalign;
        cr->property_xpad() = 6;
        if (expand) {
            cr->property_ellipsize() = Pango::ELLIPSIZE_END;
        }
        if (dim) {
            cr->property_foreground_rgba() = Gdk::RGBA("rgba(160,160,160,1)");
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
    thumbScroll_->set_size_request(-1, 120);

    auto* paned = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_VERTICAL));
    paned->pack1(*stack_, true, false);
    paned->pack2(*thumbScroll_, false, false);
    paned->set_position(380);
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
            row[cols_.active] = !any;
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
    const int gen = ++(*thumbLive_);
    for (auto* child : thumbs_->get_children()) {
        thumbs_->remove(*child);   // GTK destroys the managed child
    }
    auto it = view_->get_selection()->get_selected();
    if (!it) {
        return;
    }
    const int index = (*it)[cols_.index];
    if (index < 0 || index >= static_cast<int>(suggestions_.size())) {
        return;
    }
    const auto& groups = suggestions_[index].groups;
    constexpr int kTile = 96;
    const size_t shown = std::min<size_t>(groups.size(), 400);

    std::vector<Gtk::Image*> images;
    for (size_t i = 0; i < shown; ++i) {
        auto* img = Gtk::manage(new Gtk::Image());
        img->set_size_request(kTile, kTile);
        img->set_tooltip_text(Glib::path_get_basename(groups[i].primary));
        thumbs_->add(*img);
        images.push_back(img);
    }
    thumbs_->show_all();

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
                auto children = self->thumbs_->get_children();
                if (i < children.size()) {
                    if (auto* fbc = dynamic_cast<Gtk::FlowBoxChild*>(children[i])) {
                        if (auto* img = dynamic_cast<Gtk::Image*>(fbc->get_child())) {
                            img->set(pixbuf);
                        }
                    }
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
    int photos = 0;
    long long bytes = 0;
    for (const auto& row : store_->children()) {
        if (row[cols_.active]) {
            const auto& s = suggestions_[row[cols_.index]];
            photos += static_cast<int>(s.groups.size());
            bytes += s.bytes;
        }
    }
    summary_->set_text(photos ? Glib::ustring::compose("%1 · %2", photos, humanBytes(bytes)) : lastResult_);
    moveBtn_->set_label(Glib::ustring::compose(M("DIRCLEANUP_MOVE"), photos));
    selectAllBtn_->set_label(photos ? M("DIRCLEANUP_DESELECTALL") : M("DIRCLEANUP_SELECTALL"));
    selectAllBtn_->set_sensitive(!busy_ && !suggestions_.empty());
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

void DirCleanupWindow::fillList ()
{
    store_->clear();
    for (size_t i = 0; i < suggestions_.size(); ++i) {
        const auto& s = suggestions_[i];
        auto row = *store_->append();
        row[cols_.active] = s.dots() == 3;
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
        row[cols_.reason] = s.reason == Reason::Sequence ? "#" : s.reason == Reason::FileName ? "Aa" : "\u25f7";
        row[cols_.index] = static_cast<int>(i);
    }
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
};

struct MoveResult {
    std::vector<std::pair<Glib::ustring, Glib::ustring>> media;   // photo paths, for tags/albums
    bool moved = false;
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

MoveResult moveGroup(const MoveJob& job, Journal& journal)
{
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
        if (!row[cols_.active]) {
            continue;
        }
        const auto& s = suggestions_[row[cols_.index]];
        for (const auto& g : s.groups) {
            jobs.push_back({g, s.dest, s.createDest});
        }
    }
    if (jobs.empty()) {
        return;
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
        int moved = 0, skipped = 0, finished = 0, total = 0;
    };
    auto prog = std::make_shared<Progress>();
    prog->total = static_cast<int>(jobs.size());
    DirCleanupWindow* self = this;

    for (const auto& job : jobs) {
        moveQueue().enqueue([job, journal, prog, self]() {
            const MoveResult r = moveGroup(job, *journal);
            int finished = 0, total = 0, moved = 0, skipped = 0;
            std::vector<std::pair<Glib::ustring, Glib::ustring>> media;
            {
                std::lock_guard<std::mutex> lock(prog->mutex);
                (r.moved ? prog->moved : prog->skipped)++;
                prog->media.insert(prog->media.end(), r.media.begin(), r.media.end());
                finished = ++prog->finished;
                total = prog->total;
                if (finished == total) {
                    media.swap(prog->media);
                    moved = prog->moved;
                    skipped = prog->skipped;
                }
            }
            Glib::signal_idle().connect_once([self, finished, total, moved, skipped, media]() {
                if (instance != self) {
                    applyRenames(media);
                    return;
                }
                self->moveBtn_->set_label(Glib::ustring::compose("%1 / %2", finished, total));
                if (finished == total) {
                    applyRenames(media);
                    self->moveFinished(moved, skipped);
                }
            });
        });
    }
}

void DirCleanupWindow::moveFinished (int moved, int skipped)
{
    setBusy(false);
    filesMoved().emit();
    lastResult_ = Glib::ustring::compose("✓ %1%2", moved,
                  skipped ? Glib::ustring::compose("   ✕ %1", skipped) : Glib::ustring());
    // Fresh suggestions from the new layout.
    startScan();
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
