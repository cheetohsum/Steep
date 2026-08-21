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
#include "doubleexposuredlg.h"

#include "partnerthumb.h"

#include "rtengine/doubleexposureblend.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <glibmm/miscutils.h>

#include "cacheimagedata.h"
#include "cachemanager.h"
#include "imagescanhelpers.h"
#include "multilangmgr.h"
#include "options.h"
#include "pathutils.h"
#include "rtimage.h"
#include "selectsindex.h"
#include "thumbnail.h"

#include "rtengine/iimage.h"

using rtengine::procparams::DoubleExposureParams;

namespace
{

// winpthreads can fail pthread_detach with ESRCH when the thread has already
// finished, and std::thread::detach turns that into an uncaught
// std::system_error — i.e. terminate(). Detaching a finished thread is a
// no-op, so treat the failure as success.
void detachQuietly(std::thread&& worker)
{
    try {
        worker.detach();
    } catch (const std::system_error&) {
    }
}

constexpr int GRID_THUMB_H = 110;
constexpr int PREVIEW_THUMB_H = 380;
constexpr int PREVIEW_THUMB_HI = 1000;
constexpr size_t MAX_LAYERS = 8;

// Thumbnail decoding is expensive (cache read or generation from the raw,
// then a full thumb pipeline run), so it runs on a small fixed pool rather
// than one detached thread per request burst — a global scan used to spawn
// dozens of them and starve the GUI thread.
constexpr int THUMB_WORKERS = 3;
// Fast scrolling can outrun the workers; keep only the most recent viewport
// requests so the queue can never grow without bound.
constexpr size_t MAX_GRID_QUEUE = 192;

// sRGB <-> linear helpers for the approximate dialog preview. The real
// composite happens scene-linearly in the engine; this only needs to be
// perceptually faithful at picking time.
float srgbToLin(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linToSrgb(float v)
{
    v = std::max(0.f, std::min(1.f, v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

Glib::RefPtr<Gdk::Pixbuf> pixbufFromThumb(const Glib::ustring& path, int height, bool neutral)
{
    return partnerthumb::load(path, height, neutral);
}

// The tray chips need styling that no theme can be trusted to carry: the
// four overlaid controls must be small enough to fit on a thumbnail, and
// the selected chip needs a border an image cannot hide. Injected at
// application priority so it works under any theme (adjuster.cc pattern).
void ensureChipCss()
{
    static bool done = false;

    if (done) {
        return;
    }

    done = true;

    try {
        auto css = Gtk::CssProvider::create();
        css->load_from_data(
            "#DEChipControls button { padding: 0 3px; margin: 0; min-width: 10px; min-height: 14px; }"
            "button.de-chip { padding: 1px; margin: 0; }"
            "button.de-chip-selected { border: 2px solid #E8A33D; border-radius: 3px; padding: 0; }");
        Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css,
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
    } catch (...) {
    }
}

} // namespace

// Work queue shared with the decoder threads. Held by shared_ptr so the
// workers never touch the dialog itself — they only post results back
// through an idle handler, which re-checks the alive flag on the GUI thread.
struct DEThumbReq {
    Glib::ustring path;
    int height;
    bool neutral;
};

struct DEThumbQueue {
    std::mutex mutex;
    // Preview-pane renders are what the user is waiting on after a click, so
    // they jump ahead of the grid backlog.
    std::deque<DEThumbReq> preview;
    std::deque<DEThumbReq> grid;
    int activeWorkers = 0;
    bool stopped = false;
};

// ---------------------------------------------------------------------------
// DEThumbGrid — a lightweight thumbnail grid with click-order selection
// badges, drawn directly (PreviewStrip pattern).
// ---------------------------------------------------------------------------

class DEThumbGrid final : public Gtk::DrawingArea
{
public:
    struct Item {
        Glib::ustring path;
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        int rank = 0;
        int pick = 0;
        int badge = -1; // 0-based stack index when selected
    };

    sigc::signal<void, const Glib::ustring&>& signalToggled()
    {
        return signalToggled_;
    }

    DEThumbGrid()
    {
        add_events(Gdk::BUTTON_PRESS_MASK);
        set_hexpand(true);
        set_vexpand(true);
    }

    void setItems(std::vector<Item> items)
    {
        items_ = std::move(items);
        indexByPath_.clear();
        indexByPath_.reserve(items_.size());

        for (size_t i = 0; i < items_.size(); ++i) {
            indexByPath_.emplace(items_[i].path.raw(), i);
        }

        relayout();
        queue_draw();
    }

    // Called once per decoded thumbnail, so it must not be O(items) and must
    // not repaint the whole grid — only the one cell changed.
    void setItemPixbuf(const Glib::ustring& path, const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        const auto found = indexByPath_.find(path.raw());

        if (found == indexByPath_.end()) {
            return;
        }

        const size_t index = found->second;
        items_[index].pixbuf = pixbuf;

        const int col = static_cast<int>(index) % cols_;
        const int row = static_cast<int>(index) / cols_;
        queue_draw_area(col * CELL_W, row * cellH(), CELL_W, cellH());
    }

    // Paths of cells intersecting the [top, bottom] band (widget coordinates)
    // that still have no thumbnail.
    std::vector<Glib::ustring> pathsNeedingPixbuf(double top, double bottom) const
    {
        std::vector<Glib::ustring> needed;

        if (items_.empty() || bottom < top) {
            return needed;
        }

        const int rows = (static_cast<int>(items_.size()) + cols_ - 1) / cols_;
        const int firstRow = std::max(0, static_cast<int>(std::floor((top - PAD / 2.0) / cellH())));
        const int lastRow = std::min(rows - 1, static_cast<int>(std::floor((bottom - PAD / 2.0) / cellH())));

        for (int row = firstRow; row <= lastRow; ++row) {
            for (int col = 0; col < cols_; ++col) {
                const size_t index = static_cast<size_t>(row) * cols_ + col;

                if (index >= items_.size()) {
                    break;
                }

                if (!items_[index].pixbuf) {
                    needed.push_back(items_[index].path);
                }
            }
        }

        return needed;
    }

    void setBadges(const std::vector<rtengine::procparams::DoubleExposureParams::Layer>& layers)
    {
        for (auto& item : items_) {
            item.badge = -1;

            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].path == item.path) {
                    item.badge = static_cast<int>(i);
                    break;
                }
            }
        }

        queue_draw();
    }

    // 0-based stack index of the layer currently selected for adjustment.
    void setSelectedBadge(int badgeIndex)
    {
        selectedBadge_ = badgeIndex;
        queue_draw();
    }

private:
    static constexpr int CELL_W = 152;
    static constexpr int THUMB_H = 102;
    static constexpr int TEXT_H = 30;
    static constexpr int PAD = 6;

    std::vector<Item> items_;
    std::unordered_map<std::string, size_t> indexByPath_;
    sigc::signal<void, const Glib::ustring&> signalToggled_;
    int cols_ = 1;
    int selectedBadge_ = -1;

    int cellH() const
    {
        return THUMB_H + TEXT_H + PAD;
    }

    void relayout()
    {
        const int width = std::max(get_allocated_width(), CELL_W);
        cols_ = std::max(1, width / CELL_W);
        const int rows = items_.empty() ? 1 : (static_cast<int>(items_.size()) + cols_ - 1) / cols_;
        set_size_request(-1, rows * cellH() + PAD);
    }

    void on_size_allocate(Gtk::Allocation& allocation) override
    {
        Gtk::DrawingArea::on_size_allocate(allocation);
        const int newCols = std::max(1, allocation.get_width() / CELL_W);

        if (newCols != cols_) {
            relayout();
        }
    }

    bool on_button_press_event(GdkEventButton* event) override
    {
        if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
            const int col = static_cast<int>(event->x) / CELL_W;
            const int row = (static_cast<int>(event->y) - PAD / 2) / cellH();

            if (col >= 0 && col < cols_ && row >= 0) {
                const size_t index = static_cast<size_t>(row) * cols_ + col;

                if (index < items_.size()) {
                    signalToggled_.emit(items_[index].path);
                }
            }

            return true;
        }

        return false;
    }

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const auto style = get_style_context();
        style->render_background(cr, 0, 0, get_allocated_width(), get_allocated_height());

        const Gdk::RGBA fg = style->get_color(Gtk::STATE_FLAG_NORMAL);

        // Only paint the rows the damage region touches: a global scan can
        // put thousands of cells in the grid, and every caption costs a
        // basename allocation plus cairo glyph work.
        double clipX1, clipY1, clipX2, clipY2;
        cr->get_clip_extents(clipX1, clipY1, clipX2, clipY2);

        const int rows = (static_cast<int>(items_.size()) + cols_ - 1) / cols_;
        const int firstRow = std::max(0, static_cast<int>(std::floor((clipY1 - PAD / 2.0) / cellH())));
        const int lastRow = std::min(rows - 1, static_cast<int>(std::floor((clipY2 - PAD / 2.0) / cellH())));
        const size_t firstItem = static_cast<size_t>(firstRow) * cols_;
        const size_t lastItem = lastRow < firstRow
            ? firstItem
            : std::min(items_.size(), static_cast<size_t>(lastRow + 1) * cols_);

        for (size_t i = firstItem; i < lastItem; ++i) {
            const Item& item = items_[i];
            const int col = static_cast<int>(i) % cols_;
            const int row = static_cast<int>(i) / cols_;
            const double cx = col * CELL_W + PAD / 2.0;
            const double cy = row * cellH() + PAD / 2.0;
            const double tw = CELL_W - PAD;
            const double th = THUMB_H;

            // thumb slot background
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
            cr->rectangle(cx, cy, tw, th);
            cr->fill();

            if (item.pixbuf) {
                const double sc = std::min(tw / item.pixbuf->get_width(), th / item.pixbuf->get_height());
                const double dw = item.pixbuf->get_width() * sc;
                const double dh = item.pixbuf->get_height() * sc;
                const double dx = cx + (tw - dw) / 2.0;
                const double dy = cy + (th - dh) / 2.0;

                cr->save();
                cr->translate(dx, dy);
                cr->scale(sc, sc);
                Gdk::Cairo::set_source_pixbuf(cr, item.pixbuf, 0, 0);
                cr->paint();
                cr->restore();
            }

            if (item.badge >= 0) {
                // amber selection outline + numbered badge (stack order);
                // the layer being adjusted gets a brighter, thicker outline
                const bool adjusting = item.badge == selectedBadge_;

                if (adjusting) {
                    cr->set_source_rgb(1.0, 0.84, 0.42);
                    cr->set_line_width(3.5);
                } else {
                    cr->set_source_rgb(0.886, 0.663, 0.243);
                    cr->set_line_width(2.0);
                }

                cr->rectangle(cx + 1, cy + 1, tw - 2, th - 2);
                cr->stroke();
                cr->set_source_rgb(0.886, 0.663, 0.243);

                const double bx = cx + 13.0;
                const double by = cy + 13.0;
                cr->arc(bx, by, 10.0, 0, 2 * M_PI);
                cr->fill();

                cr->set_source_rgb(0.09, 0.08, 0.06);
                cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_BOLD);
                cr->set_font_size(12.0);
                const std::string num = std::to_string(item.badge + 1);
                Cairo::TextExtents ext;
                cr->get_text_extents(num, ext);
                cr->move_to(bx - ext.width / 2.0 - ext.x_bearing, by - ext.height / 2.0 - ext.y_bearing);
                cr->show_text(num);
            }

            // caption: basename + rank stars + pick flag
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.85);
            cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
            cr->set_font_size(10.0);

            std::string caption = Glib::path_get_basename(item.path);

            if (caption.size() > 22) {
                caption = caption.substr(0, 19) + "...";
            }

            cr->move_to(cx + 2, cy + th + 12);
            cr->show_text(caption);

            double markerX = cx + 2;
            const double markerY = cy + th + 24;

            if (item.pick == 1) {
                cr->set_source_rgb(0.50, 0.70, 0.82);
                cr->arc(markerX + 4, markerY - 3.5, 3.5, 0, 2 * M_PI);
                cr->fill();
                markerX += 13;
            }

            if (item.rank > 0) {
                cr->set_source_rgb(0.85, 0.71, 0.31);
                cr->set_font_size(9.0);
                std::string stars;

                for (int s = 0; s < std::min(item.rank, 5); ++s) {
                    stars += "\xE2\x98\x85"; // ★
                }

                cr->move_to(markerX, markerY);
                cr->show_text(stars);
            }
        }

        return true;
    }
};

// ---------------------------------------------------------------------------
// DEBlendPreview — aspect-fit display of the composited preview pixbuf.
// ---------------------------------------------------------------------------

class DEBlendPreview final : public Gtk::DrawingArea
{
public:
    DEBlendPreview()
    {
        set_size_request(420, 300);
        set_hexpand(true);
        set_vexpand(true);
    }

    void setComposite(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        composite_ = pixbuf;
        queue_draw();
    }

private:
    Glib::RefPtr<Gdk::Pixbuf> composite_;

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const int w = get_allocated_width();
        const int h = get_allocated_height();

        cr->set_source_rgb(0.06, 0.06, 0.07);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        if (!composite_) {
            const auto style = get_style_context();
            const Gdk::RGBA fg = style->get_color(Gtk::STATE_FLAG_NORMAL);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
            cr->select_font_face("sans-serif", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
            cr->set_font_size(12.0);
            const std::string msg = M("DOUBLEEXPOSURE_PREVIEW_EMPTY");
            Cairo::TextExtents ext;
            cr->get_text_extents(msg, ext);
            cr->move_to((w - ext.width) / 2.0, (h - ext.height) / 2.0);
            cr->show_text(msg);
            return true;
        }

        const double sc = std::min(static_cast<double>(w) / composite_->get_width(),
                                   static_cast<double>(h) / composite_->get_height());
        const double dw = composite_->get_width() * sc;
        const double dh = composite_->get_height() * sc;

        cr->save();
        cr->translate((w - dw) / 2.0, (h - dh) / 2.0);
        cr->scale(sc, sc);
        Gdk::Cairo::set_source_pixbuf(cr, composite_, 0, 0);
        cr->paint();
        cr->restore();

        return true;
    }
};

// ---------------------------------------------------------------------------
// DoubleExposureDlg
// ---------------------------------------------------------------------------

DoubleExposureDlg::DoubleExposureDlg(Gtk::Window* parent, const Glib::ustring& baseImagePath,
                                     const DoubleExposureParams& initial,
                                     const BrowserFilter* browserFilter,
                                     const Glib::ustring& browserCurrentDir) :
    Gtk::Dialog(M("DOUBLEEXPOSURE_DIALOG_TITLE"), true),
    baseImagePath_(baseImagePath),
    folderDir_(baseImagePath.empty() ? Glib::ustring() : Glib::ustring(Glib::path_get_dirname(baseImagePath))),
    browserCurrentDir_(browserCurrentDir),
    params_(initial),
    haveBrowserFilter_(browserFilter != nullptr),
    selectedLayer_(0),
    syncingControls_(false),
    globalScope_(false),
    folderScanned_(false),
    globalScanned_(false),
    folderScanRunning_(false),
    globalScanRunning_(false),
    folderScanGen_(0),
    globalScanGen_(0),
    alive_(std::make_shared<std::atomic<bool>>(true)),
    thumbQueue_(std::make_shared<DEThumbQueue>()),
    filterRefreshPending_(false),
    visibleThumbsPending_(false)
{
    if (browserFilter) {
        browserFilter_ = *browserFilter;
    }

    if (parent) {
        set_transient_for(*parent);
    }

    set_default_size(1060, 680);

    Gtk::Box* content = get_content_area();
    content->set_spacing(8);
    content->set_border_width(10);

    // --- toolbar: scope + filters + count ---
    Gtk::Box* toolbar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));

    folderBtn_ = Gtk::manage(new Gtk::ToggleButton(M("DOUBLEEXPOSURE_SCOPE_FOLDER")));
    folderBtn_->set_tooltip_text(M("DOUBLEEXPOSURE_SCOPE_FOLDER_TOOLTIP"));
    globalBtn_ = Gtk::manage(new Gtk::ToggleButton());
    globalBtn_->set_image(*Gtk::manage(new RTImage("globe-small", Gtk::ICON_SIZE_BUTTON)));
    globalBtn_->set_tooltip_text(M("DOUBLEEXPOSURE_SCOPE_GLOBAL_TOOLTIP"));
    folderBtn_->set_active(true);
    folderBtn_->signal_toggled().connect([this]() {
        if (folderBtn_->get_active()) {
            scopeChanged(false);
        } else if (!globalBtn_->get_active()) {
            // clicking the already-active folder button: keep it active and
            // offer a different source folder instead
            folderBtn_->set_active(true);
            openFolderChooser();
        }
    });
    globalBtn_->signal_toggled().connect([this]() {
        if (globalBtn_->get_active()) {
            scopeChanged(true);
        } else if (!folderBtn_->get_active()) {
            globalBtn_->set_active(true);
        }
    });

    Gtk::Box* scopeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    scopeBox->get_style_context()->add_class("linked");
    scopeBox->pack_start(*folderBtn_, Gtk::PACK_SHRINK);
    scopeBox->pack_start(*globalBtn_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*scopeBox, Gtk::PACK_SHRINK);

    // Seed pick/star filters from the browser tab's current filter state.
    bool seedPicked = false;
    int seedMinStars = 0;

    if (haveBrowserFilter_) {
        seedPicked = browserFilter_.showPicked && !browserFilter_.showUnflagged;

        if (!browserFilter_.showRanked[0]) {
            for (int r = 1; r <= 5; ++r) {
                if (browserFilter_.showRanked[r]) {
                    seedMinStars = r;
                    break;
                }
            }
        }
    }

    pickedFilter_ = Gtk::manage(new Gtk::ToggleButton(M("DOUBLEEXPOSURE_FILTER_PICKED")));
    pickedFilter_->set_tooltip_text(M("FILEBROWSER_SHOWPICKEDHINT"));
    pickedFilter_->set_active(seedPicked);
    pickedFilter_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::applyFilter));
    toolbar->pack_start(*pickedFilter_, Gtk::PACK_SHRINK);

    starsFilter_ = Gtk::manage(new MyComboBoxText());
    starsFilter_->append(M("DOUBLEEXPOSURE_FILTER_ANYSTARS"));

    for (int i = 1; i <= 5; ++i) {
        starsFilter_->append(Glib::ustring::compose("\xE2\x98\x85 %1+", i));
    }

    starsFilter_->set_active(seedMinStars);
    starsFilter_->setPreferredWidth(100, 130);
    starsFilter_->connect(starsFilter_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::applyFilter)));
    toolbar->pack_start(*starsFilter_, Gtk::PACK_SHRINK);

    spinner_ = Gtk::manage(new Gtk::Spinner());
    toolbar->pack_start(*spinner_, Gtk::PACK_SHRINK);

    countLabel_ = Gtk::manage(new Gtk::Label(""));
    countLabel_->set_halign(Gtk::ALIGN_END);
    toolbar->pack_end(*countLabel_, Gtk::PACK_SHRINK);

    content->pack_start(*toolbar, Gtk::PACK_SHRINK);

    // --- main split: grid | preview + controls ---
    Gtk::Paned* split = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    split->set_position(480);

    grid_ = Gtk::manage(new DEThumbGrid());
    grid_->signalToggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::itemToggled));

    gridScroll_ = Gtk::manage(new Gtk::ScrolledWindow());
    gridScroll_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    gridScroll_->add(*grid_);
    split->pack1(*gridScroll_, true, false);

    // Scrolling (and any resize that changes the page geometry) brings new
    // cells into view: fill those, and only those.
    const auto gridAdjustment = gridScroll_->get_vadjustment();
    gridAdjustment->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::scheduleVisibleThumbs));
    gridAdjustment->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::scheduleVisibleThumbs));

    Gtk::Box* right = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));

    preview_ = Gtk::manage(new DEBlendPreview());
    right->pack_start(*preview_, Gtk::PACK_EXPAND_WIDGET);

    highRes_ = Gtk::manage(new Gtk::CheckButton(M("DOUBLEEXPOSURE_HIGHRES")));
    highRes_->set_halign(Gtk::ALIGN_END);
    highRes_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::highResToggled));
    right->pack_start(*highRes_, Gtk::PACK_SHRINK);

    // blend controls
    auto makeScaleRow = [](const Glib::ustring& label, Gtk::Scale*& outScale, double lo, double hi, double step, double value) {
        Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        Gtk::Label* lab = Gtk::manage(new Gtk::Label(label, Gtk::ALIGN_START));
        lab->set_size_request(150, -1);
        lab->set_xalign(0.f);
        outScale = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
        outScale->set_range(lo, hi);
        outScale->set_increments(step, step * 5);
        outScale->set_value(value);
        outScale->set_draw_value(true);
        outScale->set_value_pos(Gtk::POS_RIGHT);
        outScale->set_digits(step < 1.0 ? 2 : 0);
        outScale->set_hexpand(true);
        row->pack_start(*lab, Gtk::PACK_SHRINK);
        row->pack_start(*outScale, Gtk::PACK_EXPAND_WIDGET);
        return row;
    };

    Gtk::Box* blendRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label* blendLab = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_BLEND"), Gtk::ALIGN_START));
    blendLab->set_size_request(150, -1);
    blendLab->set_xalign(0.f);
    blendMethod_ = Gtk::manage(new MyComboBoxText());
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_ADD"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_SCREEN"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_MULTIPLY"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_LIGHTEN"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_DARKEN"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_DIFFERENCE"));
    blendMethod_->set_active(0);
    blendMethod_->setPreferredWidth(180, 230);
    blendMethod_->connect(blendMethod_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged)));
    blendRow->pack_start(*blendLab, Gtk::PACK_SHRINK);
    blendRow->pack_start(*blendMethod_, Gtk::PACK_EXPAND_WIDGET);
    right->pack_start(*blendRow, Gtk::PACK_SHRINK);

    autoGain_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_AUTOGAIN")));
    autoGain_->set_active(params_.autoGain);
    autoGain_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_AUTOGAIN_TOOLTIP"));
    autoGain_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));
    right->pack_start(*autoGain_, Gtk::PACK_SHRINK);

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_BASEEV"), baseEvScale_, -4.0, 4.0, 0.05, params_.baseEv), Gtk::PACK_SHRINK);
    baseEvScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));

    Gtk::Separator* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    right->pack_start(*sep, Gtk::PACK_SHRINK);

    layerLabel_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_START));
    layerLabel_->set_xalign(0.f);
    right->pack_start(*layerLabel_, Gtk::PACK_SHRINK);

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_EV"), layerEvScale_, -4.0, 4.0, 0.05, 0.0), Gtk::PACK_SHRINK);
    layerEvScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_OPACITY"), layerOpacityScale_, 0.0, 100.0, 1.0, 100.0), Gtk::PACK_SHRINK);
    layerOpacityScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_GATE_STRENGTH"), gateStrengthScale_, 0.0, 100.0, 1.0, 25.0), Gtk::PACK_SHRINK);
    gateStrengthScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_GATE_TOOLTIP"));
    gateStrengthScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    split->pack2(*right, true, false);
    content->pack_start(*split, Gtk::PACK_EXPAND_WIDGET);

    // --- tray: base plate + ordered overlay chips ---
    trayBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    trayBox_->set_tooltip_text(M("DOUBLEEXPOSURE_ORDER_TOOLTIP"));
    Gtk::ScrolledWindow* trayScroll = Gtk::manage(new Gtk::ScrolledWindow());
    trayScroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER);
    trayScroll->set_min_content_height(44);
    trayScroll->add(*trayBox_);
    content->pack_start(*trayScroll, Gtk::PACK_SHRINK);

    add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    set_default_response(Gtk::RESPONSE_OK);

    show_all_children();
    spinner_->hide();

    rebuildTray();
    syncLayerControls();
    requestPreviewThumbs();
    startScan(false);
}

// Request preview-pane renders of the base plate and every layer at the
// resolution matching the high-res toggle.
void DoubleExposureDlg::requestPreviewThumbs()
{
    const int height = highRes_ && highRes_->get_active() ? PREVIEW_THUMB_HI : PREVIEW_THUMB_H;
    // The base is needed twice: styled (what the user's edit looks like) and
    // neutral (the scene-linear reference the engine actually composites on).
    requestThumbs({baseImagePath_}, height, false);
    requestThumbs({baseImagePath_}, height, true);

    std::vector<Glib::ustring> layerPaths;

    for (const auto& layer : params_.layers) {
        layerPaths.push_back(layer.path);
    }

    requestThumbs(layerPaths, height, true);
}

void DoubleExposureDlg::highResToggled()
{
    requestPreviewThumbs();
    updatePreview();
}

void DoubleExposureDlg::openFolderChooser()
{
    Gtk::FileChooserDialog chooser(*this, M("DOUBLEEXPOSURE_PICK_FOLDER"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    chooser.add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    chooser.add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);

    if (!folderDir_.empty()) {
        chooser.set_current_folder(folderDir_);
    }

    if (chooser.run() == Gtk::RESPONSE_OK && !chooser.get_filename().empty()) {
        folderDir_ = chooser.get_filename();
        folderBtn_->set_label(Glib::path_get_basename(folderDir_));
        folderScanned_ = false;
        // A still-running scan of the previous folder is obsolete: the
        // generation bump in startScan discards its late posts.
        folderScanRunning_ = false;
        folderItems_.clear();
        startScan(false);
    }
}

DoubleExposureDlg::~DoubleExposureDlg()
{
    *alive_ = false;

    // Workers only touch the queue after this point; the idle handlers they
    // may already have posted bail out on the alive flag.
    std::lock_guard<std::mutex> lock(thumbQueue_->mutex);
    thumbQueue_->stopped = true;
    thumbQueue_->preview.clear();
    thumbQueue_->grid.clear();
}

DoubleExposureParams DoubleExposureDlg::getResult() const
{
    if (!params_.layers.empty()) {
        rememberStickyLayer(selectedLayer_ < params_.layers.size()
                            ? params_.layers[selectedLayer_]
                            : params_.layers.back());
    }

    return params_;
}

const std::vector<DoubleExposureDlg::ScanItem>& DoubleExposureDlg::activeItems() const
{
    return globalScope_ ? globalItems_ : folderItems_;
}

// --- scanning ---

void DoubleExposureDlg::startScan(bool global)
{
    if ((global && (globalScanned_ || globalScanRunning_)) || (!global && (folderScanned_ || folderScanRunning_))) {
        applyFilter();
        return;
    }

    int generation;

    if (global) {
        globalScanRunning_ = true;
        globalItems_.clear();
        generation = ++globalScanGen_;
    } else {
        folderScanRunning_ = true;
        folderItems_.clear();
        generation = ++folderScanGen_;
    }

    spinner_->show();
    spinner_->start();
    applyFilter();

    const Glib::ustring basePath = baseImagePath_;
    auto alive = alive_;

    std::vector<Glib::ustring> dirs;

    std::vector<std::pair<Glib::ustring, SelectsIndex::Entry>> indexEntries;

    if (global) {
        // "Global selects" = the persistent selects index (instant, complete
        // across every folder ever flagged) plus a reconciling walk over
        // favorites, recents, the browser's folder, and this dialog's folder.
        indexEntries = SelectsIndex::getInstance().snapshot();

        std::set<Glib::ustring> dirSet;

        for (const auto& d : App::get().options().favoriteDirs) {
            dirSet.insert(d);
        }

        for (const auto& d : App::get().options().recentFolders) {
            dirSet.insert(d);
        }

        if (!basePath.empty()) {
            dirSet.insert(Glib::path_get_dirname(basePath));
        }

        if (!browserCurrentDir_.empty()) {
            dirSet.insert(browserCurrentDir_);
        }

        if (!folderDir_.empty()) {
            dirSet.insert(folderDir_);
        }

        for (const auto& item : indexEntries) {
            dirSet.insert(Glib::path_get_dirname(item.first));
        }

        dirs.assign(dirSet.begin(), dirSet.end());
    } else if (!folderDir_.empty()) {
        dirs.push_back(folderDir_);
    }

    const bool recurse = global && App::get().options().globalScanSubfolders;

    // Inherit the browser tab's filetype filter (empty = all types).
    std::set<std::string> filetypes;

    if (haveBrowserFilter_) {
        filetypes = browserFilter_.filetypeFilter;
    }

    detachQuietly(std::thread([this, alive, global, generation, basePath, dirs, filetypes, indexEntries, recurse]() {
        std::vector<ScanItem> batch;
        std::set<std::string> seen;

        const auto flushBatch = [&]() {
            if (batch.empty()) {
                return;
            }

            std::vector<ScanItem> ready;
            ready.swap(batch);
            // Stream results in small batches so the grid fills as we go,
            // even inside one large directory.
            Glib::signal_idle().connect_once([this, alive, global, generation, ready]() {
                if (!*alive) {
                    return;
                }

                onScanPartial(global, generation, ready);
            });
        };

        // Index pass first: instant results from every folder ever flagged.
        for (const auto& item : indexEntries) {
            if (!*alive) {
                return;
            }

            const Glib::ustring& fpath = item.first;

            if (fpath == basePath || !seen.insert(fpath.raw()).second) {
                continue;
            }

            if (!filetypes.empty()) {
                const std::string ext = getExtension(fpath).uppercase();

                if (filetypes.find(ext) == filetypes.end()) {
                    continue;
                }
            }

            if (!Glib::file_test(fpath, Glib::FILE_TEST_EXISTS)) {
                SelectsIndex::getInstance().forget(fpath);
                continue;
            }

            ScanItem scanItem;
            scanItem.path = fpath;
            scanItem.rank = std::max(0, std::min(item.second.rating, 5));
            scanItem.pick = item.second.pick;
            batch.push_back(std::move(scanItem));

            if (batch.size() >= 48) {
                flushBatch();
            }
        }

        flushBatch();

        for (const auto& dir : dirs) {
            const auto files = recurse
                ? imagescan::listImageFilesRecursive(dir, 3)
                : imagescan::listImageFiles(dir);

            for (const auto& fpath : files) {
                if (fpath == basePath || !seen.insert(fpath.raw()).second) {
                    continue; // the base plate cannot double-expose with itself
                }

                if (!filetypes.empty()) {
                    const std::string ext = getExtension(fpath).uppercase();

                    if (filetypes.find(ext) == filetypes.end()) {
                        continue;
                    }
                }

                CacheImageData cid;
                const bool haveCache = imagescan::loadCacheDataFast(fpath, cid);

                if (!haveCache) {
                    if (global) {
                        // Unknown to the cache: only a sidecar can make this
                        // file a select.
                        if (!Glib::file_test(fpath + ".pp3", Glib::FILE_TEST_EXISTS)) {
                            continue;
                        }

                        imagescan::loadPP3OverlaysFast(fpath, cid);
                    } else {
                        if (!imagescan::loadExifForFile(fpath, cid)) {
                            continue;
                        }

                        imagescan::loadPP3OverlaysFast(fpath, cid);
                    }
                } else if (!global) {
                    // Folder scope honors sidecar tweaks with one merged parse.
                    imagescan::loadPP3OverlaysFast(fpath, cid);
                } else if (cid.pickLabel == 0 && cid.rating == 0
                           && Glib::file_test(fpath + ".pp3", Glib::FILE_TEST_EXISTS)) {
                    // The cache can lag the sidecar for flags/ranks: verify
                    // apparent non-selects against the sidecar when one exists.
                    imagescan::loadPP3OverlaysFast(fpath, cid);
                }

                if (global && (haveCache || cid.rating > 0 || cid.pickLabel != 0 || cid.colorLabel > 0)) {
                    // Converge the selects index as we go (no-op when unchanged).
                    SelectsIndex::getInstance().note(fpath, cid.rating, cid.pickLabel, cid.colorLabel);
                }

                ScanItem item;
                item.path = fpath;
                item.rank = std::max(0, std::min(cid.rating, 5));
                item.pick = cid.pickLabel;
                batch.push_back(std::move(item));

                if (batch.size() >= 48) {
                    flushBatch();
                }
            }

            if (!*alive) {
                return;
            }

            flushBatch();
        }

        Glib::signal_idle().connect_once([this, alive, global, generation]() {
            if (!*alive) {
                return;
            }

            onScanDone(global, generation);
        });
    }));
}

void DoubleExposureDlg::onScanPartial(bool global, int generation, const std::vector<ScanItem>& items)
{
    if (generation != (global ? globalScanGen_ : folderScanGen_)) {
        return; // stale scan (scope re-scanned meanwhile)
    }

    auto& target = global ? globalItems_ : folderItems_;
    target.insert(target.end(), items.begin(), items.end());

    if (global == globalScope_) {
        scheduleFilterRefresh();
    }
}

void DoubleExposureDlg::onScanDone(bool global, int generation)
{
    if (generation != (global ? globalScanGen_ : folderScanGen_)) {
        return;
    }

    if (global) {
        globalScanned_ = true;
        globalScanRunning_ = false;
    } else {
        folderScanned_ = true;
        folderScanRunning_ = false;
    }

    if (!folderScanRunning_ && !globalScanRunning_) {
        spinner_->stop();
        spinner_->hide();
    }

    if (global == globalScope_) {
        filterRefreshPending_ = false; // supersede any coalesced rebuild
        applyFilter();
    }
}

void DoubleExposureDlg::requestThumbs(const std::vector<Glib::ustring>& paths, int height, bool neutral)
{
    std::vector<Glib::ustring> needed;

    for (const auto& path : paths) {
        const Glib::ustring key = path + Glib::ustring::compose("|%1|%2", height, neutral ? 1 : 0);

        if (pendingThumbs_.count(key)) {
            continue;
        }

        const auto& map = height == GRID_THUMB_H ? gridPix_
                          : (neutral ? (height == PREVIEW_THUMB_HI ? neutralHiPix_ : neutralPix_)
                                     : (height == PREVIEW_THUMB_HI ? hiPix_ : bigPix_));

        if (map.count(path)) {
            continue;
        }

        pendingThumbs_.insert(key);
        needed.push_back(path);
    }

    if (needed.empty()) {
        return;
    }

    const bool isGrid = height == GRID_THUMB_H;

    {
        std::lock_guard<std::mutex> lock(thumbQueue_->mutex);
        auto& queue = isGrid ? thumbQueue_->grid : thumbQueue_->preview;

        // Newest request first: whatever the viewport shows now matters more
        // than what it showed two scroll events ago.
        for (auto it = needed.rbegin(); it != needed.rend(); ++it) {
            queue.emplace_front(DEThumbReq{*it, height, neutral});
        }

        while (thumbQueue_->grid.size() > MAX_GRID_QUEUE) {
            // Dropped requests must leave pendingThumbs_ too, or the cell
            // could never be requested again. Safe here: the GUI thread owns
            // pendingThumbs_ and this runs on it.
            const auto& stale = thumbQueue_->grid.back();
            pendingThumbs_.erase(stale.path + Glib::ustring::compose("|%1|%2", stale.height, stale.neutral ? 1 : 0));
            thumbQueue_->grid.pop_back();
        }
    }

    pumpThumbQueue();
}

// Tops the decoder pool back up to THUMB_WORKERS while work is queued.
void DoubleExposureDlg::pumpThumbQueue()
{
    auto queue = thumbQueue_;
    auto alive = alive_;

    std::lock_guard<std::mutex> lock(queue->mutex);
    const size_t queued = queue->preview.size() + queue->grid.size();

    while (queue->activeWorkers < THUMB_WORKERS && static_cast<size_t>(queue->activeWorkers) < queued) {
        ++queue->activeWorkers;

        detachQuietly(std::thread([this, queue, alive]() {
            for (;;) {
                Glib::ustring path;
                int height = 0;

                bool neutral = false;

                {
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    auto& source = !queue->preview.empty() ? queue->preview : queue->grid;

                    if (queue->stopped || source.empty()) {
                        --queue->activeWorkers;
                        return;
                    }

                    path = source.front().path;
                    height = source.front().height;
                    neutral = source.front().neutral;
                    source.pop_front();
                }

                Glib::RefPtr<Gdk::Pixbuf> pixbuf = pixbufFromThumb(path, height, neutral);

                Glib::signal_idle().connect_once([this, alive, path, height, neutral, pixbuf]() {
                    if (!*alive) {
                        return;
                    }

                    onThumbLoaded(path, height, neutral, pixbuf);
                });
            }
        }));
    }
}

void DoubleExposureDlg::onThumbLoaded(const Glib::ustring& path, int height, bool neutral, Glib::RefPtr<Gdk::Pixbuf> pixbuf)
{
    pendingThumbs_.erase(path + Glib::ustring::compose("|%1|%2", height, neutral ? 1 : 0));

    if (!pixbuf) {
        return;
    }

    if (height == GRID_THUMB_H) {
        gridPix_[path] = pixbuf;
        grid_->setItemPixbuf(path, pixbuf);
    } else if (neutral) {
        (height == PREVIEW_THUMB_HI ? neutralHiPix_ : neutralPix_)[path] = pixbuf;
        updatePreview();
        rebuildTray();
    } else {
        (height == PREVIEW_THUMB_HI ? hiPix_ : bigPix_)[path] = pixbuf;
        updatePreview();
        rebuildTray();
    }
}

// --- UI logic ---

void DoubleExposureDlg::scopeChanged(bool global)
{
    globalScope_ = global;

    // Drop grid decodes queued for the scope we are leaving, so the incoming
    // viewport is not stuck behind a backlog nobody can see any more.
    {
        std::lock_guard<std::mutex> lock(thumbQueue_->mutex);

        for (const auto& stale : thumbQueue_->grid) {
            pendingThumbs_.erase(stale.path + Glib::ustring::compose("|%1|%2", stale.height, stale.neutral ? 1 : 0));
        }

        thumbQueue_->grid.clear();
    }

    if (global) {
        folderBtn_->set_active(false);
        startScan(true);
    } else {
        globalBtn_->set_active(false);
        startScan(false);
    }

    applyFilter();
}

void DoubleExposureDlg::applyFilter()
{
    const bool pickedOnly = pickedFilter_->get_active();
    const int minStars = std::max(0, starsFilter_->get_active_row_number());

    std::vector<DEThumbGrid::Item> visible;
    const auto& source = activeItems();

    for (const auto& item : source) {
        if (pickedOnly && item.pick != 1) {
            continue;
        }

        if (minStars > 0 && item.rank < minStars) {
            continue;
        }

        DEThumbGrid::Item gi;
        gi.path = item.path;
        gi.rank = item.rank;
        gi.pick = item.pick;

        const auto pix = gridPix_.find(item.path);

        if (pix != gridPix_.end()) {
            gi.pixbuf = pix->second;
        }

        visible.push_back(std::move(gi));
    }

    countLabel_->set_text(Glib::ustring::compose(M("DOUBLEEXPOSURE_COUNT"), visible.size(), source.size()));

    grid_->setItems(std::move(visible));
    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(params_.layers.empty() ? -1 : static_cast<int>(selectedLayer_));
    // Deferred: the grid has just been resized, so the scroll adjustment
    // still reports the previous page geometry.
    scheduleVisibleThumbs();
}

// Queues thumbnail decodes for the visible band of the grid plus a screenful
// above and below, so scrolling finds cells already filled.
void DoubleExposureDlg::requestVisibleThumbs()
{
    if (!gridScroll_ || !grid_) {
        return;
    }

    const auto adjustment = gridScroll_->get_vadjustment();
    const double page = adjustment && adjustment->get_page_size() > 0.0
        ? adjustment->get_page_size()
        : gridScroll_->get_allocated_height();
    const double value = adjustment ? adjustment->get_value() : 0.0;

    requestThumbs(grid_->pathsNeedingPixbuf(value - page, value + 2.0 * page), GRID_THUMB_H);
}

void DoubleExposureDlg::scheduleVisibleThumbs()
{
    if (visibleThumbsPending_) {
        return;
    }

    visibleThumbsPending_ = true;
    auto alive = alive_;

    // Short debounce: a scroll drag emits value_changed continuously, and
    // each burst would otherwise re-walk the grid.
    Glib::signal_timeout().connect_once([this, alive]() {
        if (!*alive) {
            return;
        }

        visibleThumbsPending_ = false;
        requestVisibleThumbs();
    }, 60);
}

void DoubleExposureDlg::scheduleFilterRefresh()
{
    if (filterRefreshPending_) {
        return;
    }

    filterRefreshPending_ = true;
    auto alive = alive_;

    // Streamed scan batches arrive every few milliseconds; rebuilding the
    // whole grid for each one is quadratic in the result count.
    Glib::signal_timeout().connect_once([this, alive]() {
        if (!*alive) {
            return;
        }

        if (filterRefreshPending_) {
            filterRefreshPending_ = false;
            applyFilter();
        }
    }, 200);
}

void DoubleExposureDlg::itemToggled(const Glib::ustring& path)
{
    // Clicking a layer that is already in the stack selects it for
    // adjustment; removal happens on the tray chips.
    for (size_t i = 0; i < params_.layers.size(); ++i) {
        if (params_.layers[i].path == path) {
            selectLayer(i);
            return;
        }
    }

    if (params_.layers.size() >= MAX_LAYERS) {
        return;
    }

    // A new exposure inherits the tuned effect instead of resetting it:
    // from the selected layer when stacking another, or from the last
    // removed/applied layer when swapping the photo out.
    DoubleExposureParams::Layer layer;

    if (selectedLayer_ < params_.layers.size()) {
        layer = params_.layers[selectedLayer_];
    } else if (!params_.layers.empty()) {
        layer = params_.layers.back();
    } else if (haveStickyLayer()) {
        layer = stickyLayer();
    }

    layer.path = path;
    layer.enabled = true;
    params_.layers.push_back(layer);
    selectedLayer_ = params_.layers.size() - 1;

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    requestThumbs({path}, highRes_->get_active() ? PREVIEW_THUMB_HI : PREVIEW_THUMB_H, true);
    updatePreview();
}

void DoubleExposureDlg::moveLayer(size_t index, int direction)
{
    const int target = static_cast<int>(index) + direction;

    if (index >= params_.layers.size() || target < 0 || target >= static_cast<int>(params_.layers.size())) {
        return;
    }

    std::swap(params_.layers[index], params_.layers[target]);
    selectedLayer_ = static_cast<size_t>(target);

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    updatePreview();
}

// Last layer settings the user parted with (removed or applied) — the
// template for the next added exposure, surviving across dialogs so a new
// base image starts from the same effect.
namespace
{
rtengine::procparams::DoubleExposureParams::Layer stickyLayer_;
bool haveStickyLayer_ = false;
}

bool DoubleExposureDlg::haveStickyLayer()
{
    return haveStickyLayer_;
}

const rtengine::procparams::DoubleExposureParams::Layer& DoubleExposureDlg::stickyLayer()
{
    return stickyLayer_;
}

void DoubleExposureDlg::rememberStickyLayer(const rtengine::procparams::DoubleExposureParams::Layer& layer)
{
    stickyLayer_ = layer;
    haveStickyLayer_ = true;
}

void DoubleExposureDlg::removeLayer(size_t index)
{
    if (index >= params_.layers.size()) {
        return;
    }

    rememberStickyLayer(params_.layers[index]);
    params_.layers.erase(params_.layers.begin() + index);

    if (selectedLayer_ >= params_.layers.size() && selectedLayer_ > 0) {
        selectedLayer_ = params_.layers.size() - 1;
    }

    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(params_.layers.empty() ? -1 : static_cast<int>(selectedLayer_));
    rebuildTray();
    syncLayerControls();
    updatePreview();
}

void DoubleExposureDlg::selectLayer(size_t index)
{
    if (index < params_.layers.size()) {
        selectedLayer_ = index;
        grid_->setSelectedBadge(static_cast<int>(selectedLayer_));
        rebuildTray();
        syncLayerControls();
    }
}

void DoubleExposureDlg::rebuildTray()
{
    ensureChipCss();

    for (Gtk::Widget* child : trayBox_->get_children()) {
        trayBox_->remove(*child);
    }

    constexpr int TRAY_H = 48;

    const auto trayThumb = [this](const Glib::ustring& path, bool neutral) -> Glib::RefPtr<Gdk::Pixbuf> {
        const auto& hiMap = neutral ? neutralHiPix_ : hiPix_;
        const auto& stdMap = neutral ? neutralPix_ : bigPix_;
        Glib::RefPtr<Gdk::Pixbuf> pix;
        auto it = hiMap.find(path);

        if (it != hiMap.end() && it->second) {
            pix = it->second;
        }

        if (!pix) {
            it = stdMap.find(path);

            if (it != stdMap.end() && it->second) {
                pix = it->second;
            }
        }

        if (!pix) {
            return {};
        }

        const int w = std::max(1, pix->get_width() * TRAY_H / std::max(1, pix->get_height()));
        return pix->scale_simple(w, TRAY_H, Gdk::INTERP_BILINEAR);
    };

    const auto smallButton = [](const char* glyph) {
        Gtk::Button* b = Gtk::manage(new Gtk::Button(glyph));
        b->set_relief(Gtk::RELIEF_NONE);
        b->set_focus_on_click(false);
        return b;
    };

    // base plate chip: the thumbnail is the label
    {
        Gtk::EventBox* baseChip = Gtk::manage(new Gtk::EventBox());
        const auto pix = trayThumb(baseImagePath_, false);

        if (pix) {
            baseChip->add(*Gtk::manage(new Gtk::Image(pix)));
        } else {
            baseChip->add(*Gtk::manage(new Gtk::Label(M("DOUBLEEXPOSURE_BASEPLATE"))));
        }

        baseChip->set_tooltip_text(Glib::ustring::compose("%1 \xC2\xB7 %2",
                                   M("DOUBLEEXPOSURE_BASEPLATE"), Glib::path_get_basename(baseImagePath_)));
        trayBox_->pack_start(*baseChip, Gtk::PACK_SHRINK);
    }

    for (size_t i = 0; i < params_.layers.size(); ++i) {
        Gtk::Label* arrow = Gtk::manage(new Gtk::Label("\xE2\x86\x92")); // arrow
        arrow->get_style_context()->add_class("dim-label");
        trayBox_->pack_start(*arrow, Gtk::PACK_SHRINK);

        const size_t idx = i;
        const Glib::ustring path = params_.layers[i].path;

        Gtk::Overlay* chip = Gtk::manage(new Gtk::Overlay());

        Gtk::Button* face = Gtk::manage(new Gtk::Button());
        face->set_relief(Gtk::RELIEF_NONE);
        face->get_style_context()->add_class("de-chip");
        // Wide enough for the overlaid controls even on portrait thumbs.
        face->set_size_request(88, -1);
        const auto pix = trayThumb(path, true);

        if (pix) {
            face->set_image(*Gtk::manage(new Gtk::Image(pix)));
            face->set_always_show_image(true);
        } else {
            face->set_label(Glib::ustring::compose("%1 \xC2\xB7 %2", i + 1, Glib::path_get_basename(path)));
        }

        face->set_tooltip_text(Glib::path_get_basename(path));

        if (i == selectedLayer_) {
            // amber border: the layer currently bound to the adjustment
            // sliders (suggested-action is invisible behind a thumbnail)
            face->get_style_context()->add_class("de-chip-selected");
        }

        face->signal_clicked().connect([this, idx]() { selectLayer(idx); });
        chip->add(*face);

        // compact controls over the thumbnail: reorder, develop, remove
        Gtk::Box* controls = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        controls->set_name("DEChipControls");
        controls->set_halign(Gtk::ALIGN_CENTER);
        controls->set_valign(Gtk::ALIGN_END);

        Gtk::Button* left = smallButton("\xE2\x97\x82"); // left triangle
        left->set_sensitive(i > 0);
        left->signal_clicked().connect([this, idx]() { moveLayer(idx, -1); });
        controls->pack_start(*left, Gtk::PACK_SHRINK);

        Gtk::Button* rightBtn = smallButton("\xE2\x96\xB8"); // right triangle
        rightBtn->set_sensitive(i + 1 < params_.layers.size());
        rightBtn->signal_clicked().connect([this, idx]() { moveLayer(idx, 1); });
        controls->pack_start(*rightBtn, Gtk::PACK_SHRINK);

        Gtk::Button* edit = smallButton("\xE2\x9C\x8E"); // pencil
        edit->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDIT"));
        edit->signal_clicked().connect([this, path]() {
            editRequestPath_ = path;
            response(Gtk::RESPONSE_OK);
        });
        controls->pack_start(*edit, Gtk::PACK_SHRINK);

        Gtk::Button* close = smallButton("\xE2\x9C\x95"); // multiply sign
        close->set_tooltip_text(M("TP_DOUBLEEXPOSURE_REMOVE"));
        close->signal_clicked().connect([this, idx]() { removeLayer(idx); });
        controls->pack_start(*close, Gtk::PACK_SHRINK);

        chip->add_overlay(*controls);
        trayBox_->pack_start(*chip, Gtk::PACK_SHRINK);
    }

    trayBox_->show_all();
}

void DoubleExposureDlg::syncLayerControls()
{
    syncingControls_ = true;

    if (params_.layers.empty() || selectedLayer_ >= params_.layers.size()) {
        layerLabel_->set_text(M("DOUBLEEXPOSURE_NOLAYER"));
        layerEvScale_->set_sensitive(false);
        layerOpacityScale_->set_sensitive(false);
        gateStrengthScale_->set_sensitive(false);
        blendMethod_->set_sensitive(false);
    } else {
        const auto& layer = params_.layers[selectedLayer_];
        layerLabel_->set_text(Glib::ustring::compose("%1 %2 \xE2\x80\x94 %3",
                              M("TP_DOUBLEEXPOSURE_LAYER"), selectedLayer_ + 1,
                              Glib::path_get_basename(layer.path)));
        layerEvScale_->set_sensitive(true);
        layerOpacityScale_->set_sensitive(true);
        gateStrengthScale_->set_sensitive(true);
        blendMethod_->set_sensitive(true);
        layerEvScale_->set_value(layer.ev);
        layerOpacityScale_->set_value(layer.opacity);
        gateStrengthScale_->set_value(layer.gateStrength);
        blendMethod_->set_active(static_cast<int>(layer.blendMode));
    }

    bool anyAdd = false;

    for (const auto& layer : params_.layers) {
        if (layer.enabled && layer.blendMode == DoubleExposureParams::BlendMode::ADD) {
            anyAdd = true;
            break;
        }
    }

    autoGain_->set_sensitive(anyAdd);

    syncingControls_ = false;
}

void DoubleExposureDlg::layerControlChanged()
{
    if (syncingControls_ || selectedLayer_ >= params_.layers.size()) {
        return;
    }

    params_.layers[selectedLayer_].ev = layerEvScale_->get_value();
    params_.layers[selectedLayer_].opacity = layerOpacityScale_->get_value();
    params_.layers[selectedLayer_].gateStrength = gateStrengthScale_->get_value();
    updatePreview();
}

void DoubleExposureDlg::blendControlChanged()
{
    if (syncingControls_) {
        return;
    }

    if (selectedLayer_ < params_.layers.size()) {
        const int blendRow = blendMethod_->get_active_row_number();
        params_.layers[selectedLayer_].blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
    }

    params_.autoGain = autoGain_->get_active();
    params_.baseEv = baseEvScale_->get_value();

    bool anyAdd = false;

    for (const auto& layer : params_.layers) {
        if (layer.enabled && layer.blendMode == DoubleExposureParams::BlendMode::ADD) {
            anyAdd = true;
            break;
        }
    }

    autoGain_->set_sensitive(anyAdd);

    updatePreview();
}

void DoubleExposureDlg::updatePreview()
{
    // Prefer the high-res render when the toggle is on and it has arrived;
    // fall back to the standard tier so the preview never goes blank.
    const bool wantHi = highRes_ && highRes_->get_active();

    using PixMap = std::map<Glib::ustring, Glib::RefPtr<Gdk::Pixbuf>>;
    const auto findIn = [wantHi](const PixMap& hiMap, const PixMap& stdMap,
                                 const Glib::ustring& path) -> Glib::RefPtr<Gdk::Pixbuf> {
        if (wantHi) {
            const auto hi = hiMap.find(path);

            if (hi != hiMap.end() && hi->second) {
                return hi->second;
            }
        }

        const auto it = stdMap.find(path);
        return it != stdMap.end() ? it->second : Glib::RefPtr<Gdk::Pixbuf>();
    };

    const Glib::RefPtr<Gdk::Pixbuf> base = findIn(hiPix_, bigPix_, baseImagePath_);

    if (!base) {
        preview_->setComposite({});
        return;
    }

    const int w = base->get_width();
    const int h = base->get_height();

    if (w <= 0 || h <= 0) {
        preview_->setComposite({});
        return;
    }

    // The neutral base render is the scene-linear stand-in the engine
    // actually composites on (the styled thumb has the user's tone edits
    // baked in, which would corrupt min/max crossovers and gate decisions).
    const Glib::RefPtr<Gdk::Pixbuf> neutralBase = findIn(neutralHiPix_, neutralPix_, baseImagePath_);

    float srgbLut[256];

    for (int i = 0; i < 256; ++i) {
        srgbLut[i] = srgbToLin(i / 255.f);
    }

    // linearize the styled base (the user's edit — the display reference)
    std::vector<float> lin(static_cast<size_t>(w) * h * 3);

    const guint8* baseData = base->get_pixels();
    const int baseStride = base->get_rowstride();
    const int baseChannels = base->get_n_channels();

    for (int y = 0; y < h; ++y) {
        const guint8* row = baseData + y * baseStride;

        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            lin[o] = srgbLut[row[x * baseChannels]];
            lin[o + 1] = srgbLut[row[x * baseChannels + 1]];
            lin[o + 2] = srgbLut[row[x * baseChannels + 2]];
        }
    }

    struct LayerPix {
        const Glib::RefPtr<Gdk::Pixbuf> pix;
        float gain;
        float opacity;
        DoubleExposureParams::BlendMode mode;
        bool gateOnLayer;
        float gateLow;
        float gateHigh;
        float gateFeather;
        float gateStrength;
        float sx = 1.f; // engine cover-fit scales, from the two full-frame aspects
        float sy = 1.f;
    };

    std::vector<LayerPix> layerPix;
    int addLayers = 0;

    for (const auto& layer : params_.layers) {
        if (!layer.enabled) {
            continue;
        }

        const Glib::RefPtr<Gdk::Pixbuf> pix = findIn(neutralHiPix_, neutralPix_, layer.path);

        if (!pix) {
            continue;
        }

        LayerPix lp {
            pix,
            static_cast<float>(std::pow(2.0, layer.ev)),
            static_cast<float>(layer.opacity) / 100.f,
            layer.blendMode,
            layer.gateSource == DoubleExposureParams::GateSource::LAYER,
            static_cast<float>(layer.gateLow) / 100.f,
            static_cast<float>(layer.gateHigh) / 100.f,
            static_cast<float>(layer.gateFeather) / 100.f,
            static_cast<float>(layer.gateStrength) / 100.f
        };

        if (lp.mode == DoubleExposureParams::BlendMode::ADD) {
            ++addLayers;
        }

        layerPix.push_back(lp);
    }

    const float autoGainFactor = (params_.autoGain && addLayers > 0)
                                 ? 1.f / static_cast<float>(addLayers + 1)
                                 : 1.f;

    for (auto& lp : layerPix) {
        if (lp.mode == DoubleExposureParams::BlendMode::ADD) {
            lp.gain *= autoGainFactor;
        }
    }

    const float baseGain = static_cast<float>(std::pow(2.0, params_.baseEv)) * autoGainFactor;

    // --- geometry: replicate the engine's mapping instead of cover-fitting
    // the two thumbnails against each other. The engine composites on the
    // base's FULL (uncropped, coarse-rotated) frame and cover-fits the
    // partner's full frame over it; the preview shows the base's crop window
    // into that frame. In normalized full-frame coordinates the whole map
    // reduces to the two full-frame aspect ratios plus the crop rectangle.
    float nx0 = 0.f, nxs = 1.f / w;
    float ny0 = 0.f, nys = 1.f / h;
    float baseAspect = static_cast<float>(w) / h;

    {
        Thumbnail* bthm = CacheManager::getInstance()->getEntry(baseImagePath_);

        if (bthm) {
            const rtengine::procparams::ProcParams bpp = bthm->getProcParamsCopy();
            const CacheImageData* cid = bthm->getCacheImageData();
            int fullW = cid ? cid->width : -1;
            int fullH = cid ? cid->height : -1;

            if (bpp.coarse.rotate == 90 || bpp.coarse.rotate == 270) {
                const int t = fullW;
                fullW = fullH;
                fullH = t;
            }

            if (fullW > 0 && fullH > 0) {
                baseAspect = static_cast<float>(fullW) / fullH;

                if (bpp.crop.enabled && bpp.crop.w > 0 && bpp.crop.h > 0) {
                    nx0 = static_cast<float>(bpp.crop.x) / fullW;
                    nxs = (static_cast<float>(bpp.crop.w) / w) / fullW;
                    ny0 = static_cast<float>(bpp.crop.y) / fullH;
                    nys = (static_cast<float>(bpp.crop.h) / h) / fullH;
                }
            }

            bthm->decreaseRef();
        }
    }

    for (auto& lp : layerPix) {
        // The neutral layer thumb spans the partner's full upright frame, so
        // its aspect is the engine's partner aspect.
        const float layerAspect = lp.pix->get_height() > 0
                                  ? static_cast<float>(lp.pix->get_width()) / lp.pix->get_height()
                                  : baseAspect;

        if (baseAspect >= layerAspect) {
            lp.sx = 1.f;
            lp.sy = layerAspect / baseAspect;
        } else {
            lp.sx = baseAspect / layerAspect;
            lp.sy = 1.f;
        }
    }

    // Scene-faithful mode: composite on the neutral base exactly like the
    // engine, then reapply the edit's look as per-channel transfer curves
    // measured from (neutral -> styled) pixel pairs. Falls back to the styled
    // base while the neutral render is still loading.
    const bool sceneFaithful = static_cast<bool>(neutralBase) && !layerPix.empty();

    const guint8* nbData = nullptr;
    int nbStride = 0, nbCh = 3, nbW = 0, nbH = 0;

    constexpr int TBINS = 64;
    float toneRatio[3][TBINS];

    if (sceneFaithful) {
        nbData = neutralBase->get_pixels();
        nbStride = neutralBase->get_rowstride();
        nbCh = neutralBase->get_n_channels();
        nbW = neutralBase->get_width();
        nbH = neutralBase->get_height();

        double sumS[3][TBINS] = {{0.0}};
        double sumN[3][TBINS] = {{0.0}};

        for (int y = 0; y < h; ++y) {
            const float ny = ny0 + (y + 0.5f) * nys;
            const int nby = std::max(0, std::min(static_cast<int>(ny * nbH), nbH - 1));

            for (int x = 0; x < w; ++x) {
                const float nx = nx0 + (x + 0.5f) * nxs;
                const int nbx = std::max(0, std::min(static_cast<int>(nx * nbW), nbW - 1));
                const guint8* np = nbData + nby * nbStride + nbx * nbCh;
                const size_t o = (static_cast<size_t>(y) * w + x) * 3;

                for (int c = 0; c < 3; ++c) {
                    const float n = srgbLut[np[c]];
                    const int bin = std::min(TBINS - 1, static_cast<int>(n * TBINS));
                    sumS[c][bin] += lin[o + c];
                    sumN[c][bin] += n;
                }
            }
        }

        for (int c = 0; c < 3; ++c) {
            for (int bin = 0; bin < TBINS; ++bin) {
                toneRatio[c][bin] = sumN[c][bin] > 1e-6 ? static_cast<float>(sumS[c][bin] / sumN[c][bin]) : -1.f;
            }

            // Fill unpopulated bins from their nearest measured neighbor so
            // composited values outside the base's range still get a ratio.
            float last = -1.f;

            for (int bin = 0; bin < TBINS; ++bin) {
                if (toneRatio[c][bin] < 0.f) {
                    toneRatio[c][bin] = last;
                } else {
                    last = toneRatio[c][bin];
                }
            }

            last = 1.f;

            for (int bin = TBINS - 1; bin >= 0; --bin) {
                if (toneRatio[c][bin] < 0.f) {
                    toneRatio[c][bin] = last;
                } else {
                    last = toneRatio[c][bin];
                }
            }
        }
    }

    auto result = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, w, h);
    guint8* outData = result->get_pixels();
    const int outStride = result->get_rowstride();

    for (int y = 0; y < h; ++y) {
        guint8* outRow = outData + y * outStride;
        const float ny = ny0 + (y + 0.5f) * nys;
        const int nby = sceneFaithful ? std::max(0, std::min(static_cast<int>(ny * nbH), nbH - 1)) : 0;

        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            const float nx = nx0 + (x + 0.5f) * nxs;

            float r, g, b;

            if (sceneFaithful) {
                const int nbx = std::max(0, std::min(static_cast<int>(nx * nbW), nbW - 1));
                const guint8* np = nbData + nby * nbStride + nbx * nbCh;
                r = srgbLut[np[0]] * baseGain;
                g = srgbLut[np[1]] * baseGain;
                b = srgbLut[np[2]] * baseGain;
            } else {
                r = lin[o] * baseGain;
                g = lin[o + 1] * baseGain;
                b = lin[o + 2] * baseGain;
            }

            for (const auto& lp : layerPix) {
                // nearest sample of the layer thumb through the engine's map
                const int lw = lp.pix->get_width();
                const int lh = lp.pix->get_height();
                const float un = (nx - 0.5f) * lp.sx + 0.5f;
                const float vn = (ny - 0.5f) * lp.sy + 0.5f;
                int lx = static_cast<int>(un * lw);
                int ly = static_cast<int>(vn * lh);
                lx = std::max(0, std::min(lx, lw - 1));
                ly = std::max(0, std::min(ly, lh - 1));

                const guint8* lrow = lp.pix->get_pixels() + ly * lp.pix->get_rowstride();
                const int lch = lp.pix->get_n_channels();
                float pr = srgbLut[lrow[lx * lch]] * lp.gain;
                float pg = srgbLut[lrow[lx * lch + 1]] * lp.gain;
                float pb = srgbLut[lrow[lx * lch + 2]] * lp.gain;

                float cr, cg, cb;
                rtengine::deblend::blend(lp.mode, 1.f, r, g, b, pr, pg, pb, cr, cg, cb);

                float wgt = lp.opacity;

                if (lp.gateStrength > 0.f) {
                    const float lum = lp.gateOnLayer ? rtengine::deblend::lum709(pr, pg, pb)
                                                     : rtengine::deblend::lum709(r, g, b);
                    wgt *= rtengine::deblend::gateWeight(lp.gateStrength,
                                                         rtengine::deblend::gateWindow(rtengine::deblend::gateEncode(lum), lp.gateLow, lp.gateHigh, lp.gateFeather));
                }

                r += wgt * (cr - r);
                g += wgt * (cg - g);
                b += wgt * (cb - b);
            }

            if (sceneFaithful) {
                // Back to the edit's look: per-channel transfer measured from
                // the styled render.
                r = std::max(r, 0.f);
                g = std::max(g, 0.f);
                b = std::max(b, 0.f);
                r *= toneRatio[0][std::min(TBINS - 1, static_cast<int>(r * TBINS))];
                g *= toneRatio[1][std::min(TBINS - 1, static_cast<int>(g * TBINS))];
                b *= toneRatio[2][std::min(TBINS - 1, static_cast<int>(b * TBINS))];
            }

            outRow[x * 3] = static_cast<guint8>(linToSrgb(r) * 255.f + 0.5f);
            outRow[x * 3 + 1] = static_cast<guint8>(linToSrgb(g) * 255.f + 0.5f);
            outRow[x * 3 + 2] = static_cast<guint8>(linToSrgb(b) * 255.f + 0.5f);
        }
    }

    preview_->setComposite(result);
}
