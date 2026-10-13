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
#include "demotionlayers.h"
#include "effectpreviewstate.h"

#include "aimaskthumb.h"
#include "partnerthumb.h"
#include "maskpaintdlg.h"

#include "rtengine/colortemp.h"
#include "rtengine/doubleexposureblend.h"
#include "rtengine/demotion.h"
#include "rtengine/iccstore.h"
#include "rtengine/imagefloat.h"
#include "rtengine/aisegmentation.h"
#include "rtengine/aisubjectmodel.h"
#include "rtengine/settings.h"
#include "rtengine/partnerimagestore.h"
#include "rtengine/partnermaskstore.h"
#include "rtengine/partnerdepthstore.h"
#include "rtengine/samselector.h"
#include "depthrangewidget.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <condition_variable>
#include <iomanip>
#include <sstream>

#ifdef _OPENMP
#include <omp.h>
#endif
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
#include "rtengine/effectperf.h"

using rtengine::procparams::DoubleExposureParams;

// A scene plate: the engine's own preview-tier decode of one file
// (scene-linear working-profile RGB, full frame, HL blend, EXIF upright),
// area-averaged to a preview height, and for the base put into the coarse
// orientation the canvas is in. Values are 0..1 (white = 1). The picker
// composites on these so what it shows is what the canvas composites.
struct DEScenePlate {
    int w = 0;
    int h = 0;
    std::vector<float> rgb; // w * h * 3
};

struct DEMotionPlate : rtengine::demotion::Buffer {
    std::string key;
    std::shared_ptr<const DEScenePlate> source;
#ifdef RT_AI_MASKING
    std::shared_ptr<const rtengine::PartnerMask> mask;
    std::shared_ptr<const rtengine::PartnerDepth> depth;
    rtengine::DepthSelection depthSelect;
#endif
};
struct DEMotionRequest {
    std::shared_ptr<const DEScenePlate> source;
#ifdef RT_AI_MASKING
    std::shared_ptr<const rtengine::PartnerMask> mask;
    std::shared_ptr<const rtengine::PartnerDepth> depth;
    rtengine::DepthSelection depthSelect;
#endif
    rtengine::procparams::DEMotionParams params;
    std::string key;
    int edge = 768;
    bool quick = false;
};
struct DEMotionQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::function<void()> job;
    std::atomic<unsigned> revision{0};
    bool stop = false;
};

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

// Per-pixel encodes through 4096-entry tables with linear interpolation
// (error far below 1/255). The exact formulas stay in the engine; here they
// were the single largest cost of an interactive recomposite.
constexpr int ENC_LUT_N = 4096;

const std::vector<float>& srgbEncodeLut()
{
    static const std::vector<float> lut = []() {
        std::vector<float> v(ENC_LUT_N + 1);

        for (int i = 0; i <= ENC_LUT_N; ++i) {
            v[i] = linToSrgb(static_cast<float>(i) / ENC_LUT_N);
        }

        return v;
    }();
    return lut;
}

const std::vector<float>& gateEncodeLut()
{
    static const std::vector<float> lut = []() {
        std::vector<float> v(ENC_LUT_N + 1);

        for (int i = 0; i <= ENC_LUT_N; ++i) {
            v[i] = rtengine::deblend::gateEncode(static_cast<float>(i) / ENC_LUT_N);
        }

        return v;
    }();
    return lut;
}

inline float lutEncode(const std::vector<float>& lut, float v)
{
    v = std::min(std::max(v, 0.f), 1.f) * ENC_LUT_N;
    const int i = static_cast<int>(v);

    if (i >= ENC_LUT_N) {
        return lut[ENC_LUT_N];
    }

    const float f = v - i;
    return lut[i] * (1.f - f) + lut[i + 1] * f;
}

Glib::RefPtr<Gdk::Pixbuf> pixbufFromThumb(const Glib::ustring& path, int height, bool neutral, bool basePlate)
{
    return partnerthumb::load(path, height, neutral, basePlate);
}

// Tray chip styling (#DEChipControls, .de-chip) lives in
// themes/common/widgets.css, above every theme.

} // namespace


void rotatePlate(DEScenePlate& p, int deg)
{
    const int W = p.w, H = p.h;
    const int nw = (deg == 90 || deg == 270) ? H : W;
    const int nh = (deg == 90 || deg == 270) ? W : H;
    std::vector<float> out(static_cast<size_t>(nw) * nh * 3);

    for (int ny = 0; ny < nh; ++ny) {
        for (int nx = 0; nx < nw; ++nx) {
            int sx, sy;

            if (deg == 90) {          // clockwise, as PlanarWhateverData::rotate
                sx = ny;
                sy = H - 1 - nx;
            } else if (deg == 270) {
                sx = W - 1 - ny;
                sy = nx;
            } else {
                sx = W - 1 - nx;
                sy = H - 1 - ny;
            }

            const size_t si = (static_cast<size_t>(sy) * W + sx) * 3;
            const size_t di = (static_cast<size_t>(ny) * nw + nx) * 3;
            out[di] = p.rgb[si];
            out[di + 1] = p.rgb[si + 1];
            out[di + 2] = p.rgb[si + 2];
        }
    }

    p.w = nw;
    p.h = nh;
    p.rgb.swap(out);
}

void flipPlate(DEScenePlate& p, bool horizontal)
{
    const int W = p.w, H = p.h;

    if (horizontal) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W / 2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    std::swap(p.rgb[(static_cast<size_t>(y) * W + x) * 3 + c],
                              p.rgb[(static_cast<size_t>(y) * W + (W - 1 - x)) * 3 + c]);
                }
            }
        }
    } else {
        for (int y = 0; y < H / 2; ++y) {
            for (int x = 0; x < W; ++x) {
                for (int c = 0; c < 3; ++c) {
                    std::swap(p.rgb[(static_cast<size_t>(y) * W + x) * 3 + c],
                              p.rgb[(static_cast<size_t>(H - 1 - y) * W + x) * 3 + c]);
                }
            }
        }
    }
}

// Decode (or fetch from the engine's cache) one file's preview tier and
// reduce it to a plate `height` pixels tall. Runs on a decoder thread.
std::shared_ptr<const DEScenePlate> loadScenePlate(const Glib::ustring& path, int height,
                                                   const Glib::ustring& workingProfile,
                                                   const rtengine::ColorTemp* wbOverride,
                                                   int rotate, bool hflip, bool vflip)
{
    const auto partner = rtengine::PartnerImageStore::getInstance().getPartner(path, workingProfile, false, wbOverride);

    if (!partner || !partner->image) {
        return {};
    }

    const rtengine::Imagefloat& img = *partner->image;
    const int sw = img.getWidth();
    const int sh = img.getHeight();

    if (sw <= 0 || sh <= 0 || height <= 0) {
        return {};
    }

    const int dh = std::min(height, sh);
    const int dw = std::max(1, static_cast<int>(std::lround(static_cast<double>(sw) * dh / sh)));
    std::vector<double> acc(static_cast<size_t>(dw) * dh * 3, 0.0);
    std::vector<int> cnt(static_cast<size_t>(dw) * dh, 0);

    for (int y = 0; y < sh; ++y) {
        const int dy = std::min(dh - 1, static_cast<int>(static_cast<long long>(y) * dh / sh));

        for (int x = 0; x < sw; ++x) {
            const int dx = std::min(dw - 1, static_cast<int>(static_cast<long long>(x) * dw / sw));
            const size_t o = static_cast<size_t>(dy) * dw + dx;
            acc[o * 3] += img.r(y, x);
            acc[o * 3 + 1] += img.g(y, x);
            acc[o * 3 + 2] += img.b(y, x);
            ++cnt[o];
        }
    }

    auto plate = std::make_shared<DEScenePlate>();
    plate->w = dw;
    plate->h = dh;
    plate->rgb.resize(acc.size());

    for (size_t i = 0; i < cnt.size(); ++i) {
        const double n = cnt[i] > 0 ? cnt[i] : 1.0;

        for (int c = 0; c < 3; ++c) {
            plate->rgb[i * 3 + c] = static_cast<float>(std::max(0.0, acc[i * 3 + c] / n) / 65535.0);
        }
    }

    // Same order as the thumbnail pipeline: coarse rotation, then flips.
    if (rotate == 90 || rotate == 180 || rotate == 270) {
        rotatePlate(*plate, rotate);
    }

    if (hflip) {
        flipPlate(*plate, true);
    }

    if (vflip) {
        flipPlate(*plate, false);
    }

    return plate;
}

// Work queue shared with the decoder threads. Held by shared_ptr so the
// workers never touch the dialog itself — they only post results back
// through an idle handler, which re-checks the alive flag on the GUI thread.
struct DEThumbReq {
    Glib::ustring path;
    int height;
    bool neutral;
    bool basePlate; // styled base render with its own double exposure stripped
    bool scene;     // engine decode -> DEScenePlate instead of a pixbuf
    // Segment the partner and leave the result in PartnerMaskStore, so the
    // preview can read it without ever stalling a redraw on inference.
    bool maskWarm = false;
    int maskClass = 0;
    double maskFeather = 0.0;
    bool maskInvert = false;
    rtengine::MaskPaint maskPaint;
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

namespace
{

// Text for a drawing area, set in the widget's own (theme) font scaled by
// `factor`, so it follows the font and its size rather than a fixed px.
Glib::RefPtr<Pango::Layout> deTextLayout(Gtk::Widget& widget, const Glib::ustring& text,
                                         double factor, bool bold = false)
{
    Glib::RefPtr<Pango::Layout> layout = widget.create_pango_layout(text);
    Pango::FontDescription fd = widget.get_pango_context()->get_font_description();
    const int size = fd.get_size();

    if (size > 0) {
        if (fd.get_size_is_absolute()) {
            fd.set_absolute_size(size * factor);
        } else {
            fd.set_size(static_cast<int>(size * factor + 0.5));
        }
    }

    if (bold) {
        fd.set_weight(Pango::WEIGHT_BOLD);
    }

    layout->set_font_description(fd);
    return layout;
}

} // namespace

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
    /// Top of the row holding @p path, in grid pixels, or -1 when the path is
    /// not in the list being shown.
    int rowTopOf(const Glib::ustring& path) const
    {
        const auto it = indexByPath_.find(path);

        if (it == indexByPath_.end() || cols_ <= 0) {
            return -1;
        }

        return static_cast<int>(it->second / cols_) * cellH();
    }

    int rowHeight() const
    {
        return cellH();
    }

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
        const Gdk::RGBA accent = themeColor(*this, "steep_accent", Gdk::RGBA("#e2a93e"));
        const Gdk::RGBA onAccent = themeColor(*this, "steep_surface_0", Gdk::RGBA("#171410"));
        const Gdk::RGBA starColor = themeColor(*this, "steep_attention", Gdk::RGBA("#d9b54f"));

        // One layout per text role, reused across cells: only the text changes.
        auto captionLayout = deTextLayout(*this, "", 0.8);
        captionLayout->set_width(static_cast<int>((CELL_W - PAD - 4) * PANGO_SCALE));
        captionLayout->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
        auto badgeLayout = deTextLayout(*this, "", 0.9, true);
        auto starsLayout = deTextLayout(*this, "", 0.7);

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
                // accent selection outline + numbered badge (stack order);
                // the layer being adjusted gets a full-strength, thicker outline
                const bool adjusting = item.badge == selectedBadge_;

                if (adjusting) {
                    Gdk::Cairo::set_source_rgba(cr, accent);
                    cr->set_line_width(3.5);
                } else {
                    cr->set_source_rgba(accent.get_red(), accent.get_green(), accent.get_blue(), 0.75);
                    cr->set_line_width(2.0);
                }

                cr->rectangle(cx + 1, cy + 1, tw - 2, th - 2);
                cr->stroke();
                Gdk::Cairo::set_source_rgba(cr, accent);

                const double bx = cx + 13.0;
                const double by = cy + 13.0;
                cr->arc(bx, by, 10.0, 0, 2 * M_PI);
                cr->fill();

                Gdk::Cairo::set_source_rgba(cr, onAccent);
                badgeLayout->set_text(std::to_string(item.badge + 1));
                int nw = 0, nh = 0;
                badgeLayout->get_pixel_size(nw, nh);
                cr->move_to(bx - nw / 2.0, by - nh / 2.0);
                badgeLayout->show_in_cairo_context(cr);
            }

            // caption: basename (ellipsized to the cell) + pick flag + rank stars
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.85);
            captionLayout->set_text(Glib::path_get_basename(item.path));
            cr->move_to(cx + 2, cy + th + 2);
            captionLayout->show_in_cairo_context(cr);

            double markerX = cx + 2;
            const double markerY = cy + th + 24;

            if (item.pick == 1) {
                cr->set_source_rgb(0.50, 0.70, 0.82);
                cr->arc(markerX + 4, markerY - 3.5, 3.5, 0, 2 * M_PI);
                cr->fill();
                markerX += 13;
            }

            if (item.rank > 0) {
                Gdk::Cairo::set_source_rgba(cr, starColor);
                std::string stars;

                for (int s = 0; s < std::min(item.rank, 5); ++s) {
                    stars += "\xE2\x98\x85"; // ★
                }

                starsLayout->set_text(stars);
                int sw = 0, sh = 0;
                starsLayout->get_pixel_size(sw, sh);
                cr->move_to(markerX, markerY - 3.5 - sh / 2.0);
                starsLayout->show_in_cairo_context(cr);
            }
        }

        return true;
    }
};

// Needed in C++14: std::max binds these to a reference, which odr-uses them.
// See the same note in rtgui/thumbimageupdater.cc.
constexpr int DEThumbGrid::CELL_W;
constexpr int DEThumbGrid::THUMB_H;
constexpr int DEThumbGrid::TEXT_H;
constexpr int DEThumbGrid::PAD;


namespace
{

#ifdef RT_AI_MASKING
// "Animals  4%" rather than "Animals". Which classes a partner actually
// contains is the first thing worth knowing before choosing one, and the
// numbers are a by-product of a segmentation that has already happened -- no
// coverage is ever a reason to run one.
Glib::ustring subjectEntryLabel(int row)
{
    static const char* keys[] = {
        "TP_DOUBLEEXPOSURE_SUBJECT_OFF",
        "TP_DOUBLEEXPOSURE_SUBJECT_SUBJECT",
        "TP_DOUBLEEXPOSURE_SUBJECT_PERSON",
        "TP_DOUBLEEXPOSURE_SUBJECT_SKY",
        "TP_DOUBLEEXPOSURE_SUBJECT_VEGETATION",
        "TP_DOUBLEEXPOSURE_SUBJECT_BUILDING",
        "TP_DOUBLEEXPOSURE_SUBJECT_VEHICLE",
        "TP_DOUBLEEXPOSURE_SUBJECT_ANIMAL",
        "TP_DOUBLEEXPOSURE_SUBJECT_POINTS"
    };

    return row >= 0 && row <= 8 ? M(keys[row]) : Glib::ustring();
}

// Row order in the list is not class order: row 0 is "the whole frame", which
// has no class to show.
int subjectRowClass(int row)
{
    static const rtengine::AISegClass classes[] = {
        rtengine::AISegClass::BACKGROUND,   // unused: row 0 is the whole frame
        rtengine::AISegClass::SUBJECT,
        rtengine::AISegClass::PERSON,
        rtengine::AISegClass::SKY,
        rtengine::AISegClass::VEGETATION,
        rtengine::AISegClass::BUILDING,
        rtengine::AISegClass::VEHICLE,
        rtengine::AISegClass::ANIMAL
    };

    return row >= 1 && row <= 7 ? static_cast<int>(classes[row]) : -1;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// DEBlendPreview — aspect-fit display of the composited preview pixbuf.
// ---------------------------------------------------------------------------

// The blend preview: paints the composite, and lets the selected exposure be
// placed by hand — drag inside its frame to move it, drag a corner handle or
// scroll to resize it about its centre, double-click to reset. Geometry is
// reported in composite-pixbuf pixels; the dialog converts to frame units.
constexpr double DE_HANDLE_PX = 6.0; // corner handle half-size, widget pixels

class DEBlendPreview final : public Gtk::DrawingArea
{
public:
    std::function<void(double, double)> onMove;  // drag delta since the last event
    std::function<void(double, double, bool)> onPick; // click-to-select: composite px, include
    std::function<void(double)> onScale;         // multiplicative size factor
    std::function<void(double)> onRotate;        // absolute angle in degrees
    std::function<void()> onReset;               // double-click
    std::function<void()> onGestureEnd;          // button released after a drag

    DEBlendPreview()
    {
        set_size_request(560, 420);
        set_hexpand(true);
        set_vexpand(true);
        add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK
                   | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK | Gdk::LEAVE_NOTIFY_MASK);
    }

    // While picking, clicks select instead of moving the frame.
    void setPickMode(bool on)
    {
        pickMode_ = on;
        setCursorName(on ? "crosshair" : "default");
        queue_draw();
    }

    void setComposite(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        composite_ = pixbuf;
        queue_draw();
    }

    // The base frame alone, for the Base and Split views.
    void setBase(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        base_ = pixbuf;
        queue_draw();
    }

    enum class View { RESULT, BASE, SPLIT };

    void setView(View view)
    {
        view_ = view;
        queue_draw();
    }

    // The selected layer's placed frame, in composite pixel coordinates (may
    // extend past the composite), turned by `angleDeg` about its centre.
    // Hidden when nothing is selected.
    void setSelectionFrame(bool visible, double x0, double y0, double x1, double y1, double angleDeg = 0.0)
    {
        frameVisible_ = visible;
        fx0_ = std::min(x0, x1);
        fx1_ = std::max(x0, x1);
        fy0_ = std::min(y0, y1);
        fy1_ = std::max(y0, y1);
        angle_ = angleDeg;
        queue_draw();
    }

private:
    enum class Drag { NONE, MOVE, SCALE, ROTATE };

    Glib::RefPtr<Gdk::Pixbuf> composite_;
    Glib::RefPtr<Gdk::Pixbuf> base_;
    View view_ = View::RESULT;
    bool frameVisible_ = false;
    bool pickMode_ = false;
    double fx0_ = 0.0, fy0_ = 0.0, fx1_ = 0.0, fy1_ = 0.0;
    double angle_ = 0.0; // degrees, clockwise, as the engine turns the layer
    // composite -> widget mapping from the last draw
    double sc_ = 1.0, ox_ = 0.0, oy_ = 0.0;
    Drag drag_ = Drag::NONE;
    double lastX_ = 0.0, lastY_ = 0.0;
    double scaleStartDist_ = 1.0;
    bool hoverHandle_ = false;
    bool hoverFrame_ = false;
    bool hoverRotate_ = false;

    double wx(double cx) const { return ox_ + cx * sc_; }
    double wy(double cy) const { return oy_ + cy * sc_; }
    double centreX() const { return wx((fx0_ + fx1_) * 0.5); }
    double centreY() const { return wy((fy0_ + fy1_) * 0.5); }

    // A turned frame is hit-tested by turning the pointer back instead: the
    // rest of the geometry then stays axis-aligned, as it was before layers
    // could rotate.
    void unrotate(double x, double y, double& lx, double& ly) const
    {
        const double cx = centreX(), cy = centreY();
        const double a = angle_ * M_PI / 180.0;
        const double dx = x - cx, dy = y - cy;
        lx = cx + dx * std::cos(a) + dy * std::sin(a);
        ly = cy - dx * std::sin(a) + dy * std::cos(a);
    }

    void rotatePoint(double lx, double ly, double& x, double& y) const
    {
        const double cx = centreX(), cy = centreY();
        const double a = angle_ * M_PI / 180.0;
        const double dx = lx - cx, dy = ly - cy;
        x = cx + dx * std::cos(a) - dy * std::sin(a);
        y = cy + dx * std::sin(a) + dy * std::cos(a);
    }

    // Grip for turning the frame, held off the top edge like a print being
    // squared up on the easel.
    void rotateGrip(double& gx, double& gy) const
    {
        rotatePoint((wx(fx0_) + wx(fx1_)) * 0.5, wy(fy0_) - 20.0, gx, gy);
    }

    bool hitRotate(double x, double y) const
    {
        if (!frameVisible_) {
            return false;
        }

        double gx, gy;
        rotateGrip(gx, gy);
        return std::hypot(x - gx, y - gy) <= DE_HANDLE_PX + 4.0;
    }

    bool hitHandle(double x, double y) const
    {
        if (!frameVisible_) {
            return false;
        }

        double lx, ly;
        unrotate(x, y, lx, ly);
        const double xs[2] = {wx(fx0_), wx(fx1_)};
        const double ys[2] = {wy(fy0_), wy(fy1_)};

        for (double hx : xs) {
            for (double hy : ys) {
                if (std::fabs(lx - hx) <= DE_HANDLE_PX + 3.0 && std::fabs(ly - hy) <= DE_HANDLE_PX + 3.0) {
                    return true;
                }
            }
        }

        return false;
    }

    bool hitFrame(double x, double y) const
    {
        if (!frameVisible_) {
            return false;
        }

        double lx, ly;
        unrotate(x, y, lx, ly);
        return lx >= wx(fx0_) && lx <= wx(fx1_) && ly >= wy(fy0_) && ly <= wy(fy1_);
    }

    // Angle that puts the grip under the pointer.
    double angleAt(double x, double y) const
    {
        return std::atan2(y - centreY(), x - centreX()) * 180.0 / M_PI + 90.0;
    }

    void setCursorName(const char* name)
    {
        const auto win = get_window();

        if (win) {
            win->set_cursor(Gdk::Cursor::create(get_display(), name));
        }
    }

    void updateHover(double x, double y)
    {
        const bool rotate = hitRotate(x, y);
        const bool handle = !rotate && hitHandle(x, y);
        const bool frame = !rotate && !handle && hitFrame(x, y);

        if (handle != hoverHandle_ || frame != hoverFrame_ || rotate != hoverRotate_) {
            hoverHandle_ = handle;
            hoverFrame_ = frame;
            hoverRotate_ = rotate;
            setCursorName(rotate ? "grab" : (handle ? "nwse-resize" : (frame ? "move" : "default")));
            queue_draw();
        }
    }

    bool on_button_press_event(GdkEventButton* e) override
    {
        if (pickMode_ && composite_ && sc_ > 0.0 && (e->button == 1 || e->button == 3)
                && e->type == GDK_BUTTON_PRESS) {
            if (onPick) {
                onPick((e->x - ox_) / sc_, (e->y - oy_) / sc_,
                       e->button == 1 && !(e->state & GDK_SHIFT_MASK));
            }

            return true;
        }

        if (e->button != 1) {
            return false;
        }

        if (e->type == GDK_2BUTTON_PRESS) {
            drag_ = Drag::NONE;

            if (frameVisible_ && onReset) {
                onReset();
            }

            return true;
        }

        if (!frameVisible_) {
            return false;
        }

        lastX_ = e->x;
        lastY_ = e->y;

        if (hitRotate(e->x, e->y)) {
            drag_ = Drag::ROTATE;
            setCursorName("grabbing");
            return true;
        }

        if (hitHandle(e->x, e->y)) {
            drag_ = Drag::SCALE;
            scaleStartDist_ = std::max(1.0, std::hypot(e->x - centreX(), e->y - centreY()));
            return true;
        }

        if (hitFrame(e->x, e->y)) {
            drag_ = Drag::MOVE;
            return true;
        }

        return false;
    }

    bool on_motion_notify_event(GdkEventMotion* e) override
    {
        if (drag_ == Drag::MOVE) {
            if (onMove && sc_ > 0.0) {
                onMove((e->x - lastX_) / sc_, (e->y - lastY_) / sc_);
            }

            lastX_ = e->x;
            lastY_ = e->y;
            return true;
        }

        if (drag_ == Drag::SCALE) {
            const double d = std::max(1.0, std::hypot(e->x - centreX(), e->y - centreY()));

            if (onScale) {
                onScale(d / scaleStartDist_);
            }

            scaleStartDist_ = d;
            return true;
        }

        if (drag_ == Drag::ROTATE) {
            if (onRotate) {
                double a = angleAt(e->x, e->y);

                // Shift snaps to the quarter turns, for squaring a frame up
                // against the horizon.
                if (e->state & GDK_SHIFT_MASK) {
                    a = std::round(a / 15.0) * 15.0;
                }

                while (a > 180.0) {
                    a -= 360.0;
                }

                while (a < -180.0) {
                    a += 360.0;
                }

                onRotate(a);
            }

            return true;
        }

        updateHover(e->x, e->y);
        return false;
    }

    bool on_button_release_event(GdkEventButton* e) override
    {
        if (e->button == 1 && drag_ != Drag::NONE) {
            drag_ = Drag::NONE;

            if (onGestureEnd) {
                onGestureEnd();
            }

            updateHover(e->x, e->y);
            return true;
        }

        return false;
    }

    bool on_scroll_event(GdkEventScroll* e) override
    {
        if (!frameVisible_ || !onScale) {
            return false;
        }

        double steps = 0.0;

        if (e->direction == GDK_SCROLL_UP) {
            steps = 1.0;
        } else if (e->direction == GDK_SCROLL_DOWN) {
            steps = -1.0;
        } else if (e->direction == GDK_SCROLL_SMOOTH) {
            steps = -e->delta_y;
        }

        if (steps == 0.0) {
            return false;
        }

        onScale(std::pow(1.06, steps));
        return true;
    }

    bool on_leave_notify_event(GdkEventCrossing*) override
    {
        if (drag_ == Drag::NONE && (hoverHandle_ || hoverFrame_ || hoverRotate_)) {
            hoverHandle_ = hoverFrame_ = hoverRotate_ = false;
            setCursorName("default");
            queue_draw();
        }

        return false;
    }

    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const int w = get_allocated_width();
        const int h = get_allocated_height();

        Gdk::Cairo::set_source_rgba(cr, themeColor(*this, "steep_surface_0", Gdk::RGBA("#0f0f12")));
        cr->rectangle(0, 0, w, h);
        cr->fill();

        if (!composite_) {
            const auto style = get_style_context();
            const Gdk::RGBA fg = style->get_color(Gtk::STATE_FLAG_NORMAL);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
            auto layout = deTextLayout(*this, M("DOUBLEEXPOSURE_PREVIEW_EMPTY"), 1.0);
            int tw = 0, th = 0;
            layout->get_pixel_size(tw, th);
            cr->move_to((w - tw) / 2.0, (h - th) / 2.0);
            layout->show_in_cairo_context(cr);
            return true;
        }

        sc_ = std::min(static_cast<double>(w) / composite_->get_width(),
                       static_cast<double>(h) / composite_->get_height());
        const double dw = composite_->get_width() * sc_;
        const double dh = composite_->get_height() * sc_;
        ox_ = (w - dw) / 2.0;
        oy_ = (h - dh) / 2.0;

        cr->save();
        cr->translate(ox_, oy_);
        cr->scale(sc_, sc_);
        Gdk::Cairo::set_source_pixbuf(cr, composite_, 0, 0);
        cr->paint();

        // Base and Split show the base frame alone over all or the left half
        // of the result; it may be another tier, so it is fitted to the same
        // rectangle.
        if (base_ && view_ != View::RESULT) {
            const double cw = composite_->get_width();
            const double ch = composite_->get_height();
            cr->save();

            if (view_ == View::SPLIT) {
                cr->rectangle(0, 0, cw * 0.5, ch);
                cr->clip();
            }

            cr->scale(cw / base_->get_width(), ch / base_->get_height());
            Gdk::Cairo::set_source_pixbuf(cr, base_, 0, 0);
            cr->paint();
            cr->restore();
        }

        cr->restore();

        if (base_ && view_ == View::SPLIT) {
            Gdk::Cairo::set_source_rgba(cr, themeColor(*this, "steep_accent", Gdk::RGBA("#d6a650")));
            cr->set_line_width(1.0);
            cr->move_to(std::round(ox_ + dw * 0.5) + 0.5, oy_);
            cr->line_to(std::round(ox_ + dw * 0.5) + 0.5, oy_ + dh);
            cr->stroke();
        }

        if (!frameVisible_) {
            return true;
        }

        // Selected exposure's frame: dark underline so it reads on any
        // content, then a light dashed line, then the corner handles.
        const double x0 = wx(fx0_), y0 = wy(fy0_), x1 = wx(fx1_), y1 = wy(fy1_);
        cr->save();
        cr->rectangle(0, 0, w, h);
        cr->clip();
        cr->set_line_join(Cairo::LINE_JOIN_MITER);

        // Everything below is drawn in the frame's own orientation, which is
        // also how the hit tests read it (see unrotate).
        if (angle_ != 0.0) {
            cr->translate(centreX(), centreY());
            cr->rotate(angle_ * M_PI / 180.0);
            cr->translate(-centreX(), -centreY());
        }

        // The grip that turns the frame, on a short stalk off the top edge.
        {
            const double gx = (x0 + x1) * 0.5;
            const double gy = y0 - 20.0;
            cr->move_to(gx, y0);
            cr->line_to(gx, gy);
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.55);
            cr->set_line_width(3.0);
            cr->stroke_preserve();
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.85);
            cr->set_line_width(1.0);
            cr->stroke();

            cr->arc(gx, gy, DE_HANDLE_PX, 0.0, 2.0 * M_PI);
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.7);
            cr->set_line_width(3.0);
            cr->stroke_preserve();
            cr->set_source_rgb(hoverRotate_ || drag_ == Drag::ROTATE ? 1.0 : 1.0,
                               hoverRotate_ || drag_ == Drag::ROTATE ? 0.80 : 1.0,
                               hoverRotate_ || drag_ == Drag::ROTATE ? 0.25 : 1.0);
            cr->fill();
        }

        cr->rectangle(x0, y0, x1 - x0, y1 - y0);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.55);
        cr->set_line_width(3.0);
        cr->stroke_preserve();
        cr->set_source_rgba(1.0, 1.0, 1.0, hoverFrame_ || drag_ == Drag::MOVE ? 1.0 : 0.85);
        cr->set_line_width(1.0);
        std::vector<double> dash = {6.0, 4.0};
        cr->set_dash(dash, 0.0);
        cr->stroke();
        cr->unset_dash();

        const double xs[2] = {x0, x1};
        const double ys[2] = {y0, y1};

        for (double hx : xs) {
            for (double hy : ys) {
                cr->rectangle(hx - DE_HANDLE_PX, hy - DE_HANDLE_PX, 2.0 * DE_HANDLE_PX, 2.0 * DE_HANDLE_PX);
                cr->set_source_rgba(0.0, 0.0, 0.0, 0.7);
                cr->set_line_width(3.0);
                cr->stroke_preserve();

                if (hoverHandle_ || drag_ == Drag::SCALE) {
                    cr->set_source_rgb(1.0, 0.80, 0.25);
                } else {
                    cr->set_source_rgb(1.0, 1.0, 1.0);
                }

                cr->fill();
            }
        }

        cr->restore();
        return true;
    }
};

// ---------------------------------------------------------------------------
// DoubleExposureDlg
// ---------------------------------------------------------------------------

// A collapsible section the way the tool panel draws one: the shared
// SectionHeader (chevron + label) over a revealer holding the content.
// set_expanded keeps the Gtk::Expander name the dialog was written against.
class DESection final : public Gtk::Box
{
public:
    explicit DESection(const Glib::ustring& label) :
        Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0),
        header_(Gtk::manage(new SectionHeader(label))),
        revealer_(Gtk::manage(new Gtk::Revealer()))
    {
        header_->set_halign(Gtk::ALIGN_FILL);
        header_->set_hexpand(true);
        header_->setOnToggled([this](bool expanded) { revealer_->set_reveal_child(expanded); });
        revealer_->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        revealer_->set_reveal_child(false);
        pack_start(*header_, Gtk::PACK_SHRINK);
        pack_start(*revealer_, Gtk::PACK_SHRINK);
    }

    void setContent(Gtk::Widget& content)
    {
        revealer_->add(content);
    }

    void set_expanded(bool expanded)
    {
        header_->setExpanded(expanded);
        revealer_->set_reveal_child(expanded);
    }

    void setSummary(const Glib::ustring& summary)
    {
        header_->setSummary(summary);
    }

private:
    SectionHeader* header_;
    Gtk::Revealer* revealer_;
};

void DoubleExposureDlg::adjusterChanged(Adjuster* a, double /*newval*/)
{
    if (a == baseEvScale_ || a == latitudeScale_) {
        blendControlChanged();
    } else if (a == motionShutter_ || a == motionSpeed_ || a == motionDirection_
               || a == motionNoise_ || a == motionComplexity_) {
        motionControlChanged();
    } else {
        layerControlChanged();
    }
}

DoubleExposureDlg::DoubleExposureDlg(Gtk::Window* parent, const Glib::ustring& baseImagePath,
                                     const DoubleExposureParams& initial,
                                     const BrowserFilter* browserFilter,
                                     const Glib::ustring& browserCurrentDir) :
    Gtk::Dialog(M("DOUBLEEXPOSURE_DIALOG_TITLE_SHORT"), true),
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
    motionQueue_(std::make_shared<DEMotionQueue>()),
    previewQueue_(std::make_shared<DEMotionQueue>()),
    filterRefreshPending_(false),
    visibleThumbsPending_(false)
{
    for (const auto& motionQueue : {motionQueue_, previewQueue_}) {
    detachQuietly(std::thread([motionQueue] {
#ifdef _OPENMP
        omp_set_num_threads(2);
#endif
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(motionQueue->mutex);
                motionQueue->wake.wait(lock, [&] { return motionQueue->stop || bool(motionQueue->job); });
                if (motionQueue->stop) return;
                job = std::move(motionQueue->job);
                motionQueue->job = {};
            }
            job();
        }
    }));
    }
    initSceneContext();

    if (browserFilter) {
        browserFilter_ = *browserFilter;
    }

    if (parent) {
        set_transient_for(*parent);
    }

    set_default_size(1380, 900);

    Gtk::Box* content = get_content_area();


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
        // A rebuilt list is a fresh chance to land on the base image: it may
        // be absent from the selects index and present in the folder, or the
        // other way about.
        pendingScrollToBase_ = true;

        if (globalBtn_->get_active()) {
            scopeChanged(true);
        } else if (!folderBtn_->get_active()) {
            globalBtn_->set_active(true);
        }
    });

    Gtk::Box* scopeBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    // Kept as toggle buttons rather than a radio group: clicking the already
    // active folder button opens the folder chooser, which a radio button
    // would swallow. The handlers above keep exactly one of them down.
    makeSegmented(*scopeBox);
    scopeBox->pack_start(*folderBtn_, Gtk::PACK_SHRINK);
    scopeBox->pack_start(*globalBtn_, Gtk::PACK_SHRINK);
    toolbar->pack_start(*scopeBox, Gtk::PACK_SHRINK);

    // Seed pick/star filters from the last picker session; the first time
    // (nothing saved yet) from the browser tab's current filter state.
    bool seedPicked = false;
    int seedMinStars = 0;
    const Options& savedOpts = App::get().options();

    if (savedOpts.dePickerMinStars >= 0) {
        seedPicked = savedOpts.dePickerPickedOnly;
        seedMinStars = std::min(5, savedOpts.dePickerMinStars);
    } else if (haveBrowserFilter_) {
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
    // The picker column is narrow: tags, spinner and count take a second row.
    Gtk::Box* toolbar2 = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    photoTagFilter_ = Gtk::manage(new PhotoTagFilter());
    if (haveBrowserFilter_) photoTagFilter_->setSelection(browserFilter_.tagFilter, browserFilter_.tagMatchAll, browserFilter_.tagUntagged);
    photoTagFilter_->changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::applyFilter));
    toolbar2->pack_start(*photoTagFilter_, Gtk::PACK_SHRINK);
    photoTagFilter_->setEditAction(M("TAGS_EDIT_LAYERS"), [this]() {
        std::vector<Glib::ustring> paths;
        for (const auto& layer : params_.layers) paths.push_back(layer.path);
        editPhotoTags(this, paths);
    });

    spinner_ = Gtk::manage(new Gtk::Spinner());
    toolbar2->pack_start(*spinner_, Gtk::PACK_SHRINK);

    countLabel_ = Gtk::manage(new Gtk::Label(""));
    countLabel_->set_halign(Gtk::ALIGN_END);
    countLabel_->get_style_context()->add_class("dim-label");
    toolbar2->pack_end(*countLabel_, Gtk::PACK_SHRINK);

    // --- three columns: picker | preview + stack | inspector ---
    Gtk::Paned* split = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    split->set_position(360);

    grid_ = Gtk::manage(new DEThumbGrid());
    grid_->signalToggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::itemToggled));

    gridScroll_ = Gtk::manage(new Gtk::ScrolledWindow());
    gridScroll_->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    gridScroll_->add(*grid_);
    Gtk::Box* picker = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    picker->set_name("DEPicker");
    picker->pack_start(*toolbar, Gtk::PACK_SHRINK);
    picker->pack_start(*toolbar2, Gtk::PACK_SHRINK);
    picker->pack_start(*gridScroll_, Gtk::PACK_EXPAND_WIDGET);
    split->pack1(*picker, false, false);

    // Scrolling (and any resize that changes the page geometry) brings new
    // cells into view: fill those, and only those.
    const auto gridAdjustment = gridScroll_->get_vadjustment();
    gridAdjustment->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::scheduleVisibleThumbs));
    gridAdjustment->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::scheduleVisibleThumbs));

    Gtk::Box* right = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));

    preview_ = Gtk::manage(new DEBlendPreview());
    preview_->onMove = [this](double dx, double dy) { onPreviewMove(dx, dy); };
    preview_->onPick = [this](double cx, double cy, bool include) { onPreviewPick(cx, cy, include); };
    preview_->onScale = [this](double factor) { onPreviewScale(factor); };
    preview_->onRotate = [this](double degrees) { onPreviewRotate(degrees); };
    preview_->onReset = [this]() { onPreviewReset(); };
    right->set_name("DEInspector");
    right->set_margin_start(10);
    right->set_margin_end(10);

    Gtk::Box* centre = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    centre->set_margin_start(6);
    centre->set_margin_end(6);
    Gtk::Box* viewBar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Box* viewSeg = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    makeSegmented(*viewSeg);
    viewSeg->set_tooltip_text(M("DOUBLEEXPOSURE_PREVIEW_HINT"));
    Gtk::RadioButton::Group viewGroup;
    const std::pair<const char*, DEBlendPreview::View> views[] = {
        {"DOUBLEEXPOSURE_VIEW_RESULT", DEBlendPreview::View::RESULT},
        {"DOUBLEEXPOSURE_VIEW_BASE", DEBlendPreview::View::BASE},
        {"DOUBLEEXPOSURE_VIEW_SPLIT", DEBlendPreview::View::SPLIT},
    };

    for (const auto& v : views) {
        Gtk::RadioButton* b = Gtk::manage(new Gtk::RadioButton(viewGroup, M(v.first)));
        b->set_mode(false);
        const DEBlendPreview::View view = v.second;
        b->signal_toggled().connect([this, b, view]() {
            if (b->get_active()) {
                preview_->setView(view);
            }
        });
        viewSeg->pack_start(*b, Gtk::PACK_SHRINK);
    }

    viewBar->pack_start(*viewSeg, Gtk::PACK_SHRINK);
    centre->pack_start(*viewBar, Gtk::PACK_SHRINK);
    centre->pack_start(*preview_, Gtk::PACK_EXPAND_WIDGET);

    highRes_ = Gtk::manage(new Gtk::CheckButton(M("DOUBLEEXPOSURE_HIGHRES")));
    highRes_->set_halign(Gtk::ALIGN_END);
    highRes_->set_active(App::get().options().dePickerHighRes); // before the handler is wired
    highRes_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::highResToggled));
    viewBar->pack_end(*highRes_, Gtk::PACK_SHRINK);

    // The tool panel's own setting pill: name and value drawn inside the
    // track, so two or three fit side by side and keep this panel short
    // enough to leave the preview the room it deserves. Right-click resets
    // to the default given here; every pill reports to adjusterChanged().
    auto makeCell = [this](const Glib::ustring& label, Adjuster*& outAdj, double lo, double hi, double step, double def) {
        outAdj = Gtk::manage(new Adjuster(label, lo, hi, step, def));
        outAdj->setAdjusterListener(this);
        outAdj->set_hexpand(true);
        outAdj->set_valign(Gtk::ALIGN_CENTER);
        return outAdj;
    };

    // Cells stacked one to a row, as the tool panel lays its sliders out:
    // the inspector is narrow and every pill gets the full width.
    auto makeRow = [](std::initializer_list<Gtk::Widget*> cells) {
        Gtk::Box* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));

        for (Gtk::Widget* cell : cells) {
            row->pack_start(*cell, Gtk::PACK_SHRINK);
        }

        return row;
    };

    // The inspector: one section per question asked of the selected layer,
    // each saying when closed what it is doing; the stack's own settings sit
    // at the foot.
    const auto makeSection = [](const Glib::ustring& title, Gtk::Box*& box) {
        DESection* section = Gtk::manage(new DESection(title));
        box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
        box->set_margin_top(4);
        box->set_margin_bottom(8);
        section->setContent(*box);
        return section;
    };
    Gtk::Box* blendBox = nullptr;
    Gtk::Box* selectBox = nullptr;
    Gtk::Box* revealBox = nullptr;
    blendSection_ = makeSection(M("TP_DOUBLEEXPOSURE_SEC_BLEND"), blendBox);
    selectSection_ = makeSection(M("TP_DOUBLEEXPOSURE_SEC_SELECT"), selectBox);
    revealSection_ = makeSection(M("TP_DOUBLEEXPOSURE_SEC_REVEAL"), revealBox);
    blendSection_->set_expanded(true);

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
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_ADAPTIVE"));
    blendMethod_->append(M("TP_DOUBLEEXPOSURE_BLEND_NORMAL"));
    blendMethod_->set_active(0);
    blendMethod_->setPreferredWidth(180, 230);
    blendMethod_->connect(blendMethod_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged)));
    (void)blendLab;
    blendRow->pack_start(*blendMethod_, Gtk::PACK_EXPAND_WIDGET);

    autoGain_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_AUTOGAIN")));
    autoGain_->set_active(params_.autoGain);
    autoGain_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_AUTOGAIN_TOOLTIP"));
    autoGain_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));
    blendBox->pack_start(*blendRow, Gtk::PACK_SHRINK);

    Gtk::Widget* baseEvCell = makeCell(M("TP_DOUBLEEXPOSURE_BASEEV"), baseEvScale_, -4.0, 4.0, 0.05, 0.0);
    baseEvScale_->setValue(params_.baseEv);
    Gtk::Widget* latitudeCell = makeCell(M("TP_DOUBLEEXPOSURE_LATITUDE"), latitudeScale_, 0.0, 100.0, 1.0, 50.0);
    latitudeScale_->setValue(params_.highlightLatitude);
    latitudeScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_LATITUDE_TOOLTIP"));

    layerLabel_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_START));
    layerLabel_->set_xalign(0.f);
    layerLabel_->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    layerLabel_->get_style_context()->add_class("de-layer-title");

    // The selected layer, named, with what can be done to it as a whole.
    Gtk::Box* layerHeader = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 2));
    layerHeader->set_margin_top(8);
    layerHeader->pack_start(*layerLabel_, Gtk::PACK_EXPAND_WIDGET);
    layerDevelop_ = Gtk::manage(new Gtk::Button());
    layerDevelop_->set_image(*Gtk::manage(new RTImage("edit-small", Gtk::ICON_SIZE_BUTTON)));
    layerDevelop_->set_relief(Gtk::RELIEF_NONE);
    layerDevelop_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDIT"));
    layerDevelop_->signal_clicked().connect([this]() {
        if (selectedLayer_ < params_.layers.size()) {
            editRequestPath_ = params_.layers[selectedLayer_].path;
            response(Gtk::RESPONSE_OK);
        }
    });
    layerHeader->pack_start(*layerDevelop_, Gtk::PACK_SHRINK);
    layerRemove_ = Gtk::manage(new Gtk::Button());
    layerRemove_->set_image(*Gtk::manage(new RTImage("cancel-small", Gtk::ICON_SIZE_BUTTON)));
    layerRemove_->set_relief(Gtk::RELIEF_NONE);
    layerRemove_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_REMOVE"));
    layerRemove_->signal_clicked().connect([this]() {
        if (selectedLayer_ < params_.layers.size()) {
            removeLayer(selectedLayer_);
        }
    });
    layerHeader->pack_start(*layerRemove_, Gtk::PACK_SHRINK);

    Gtk::Widget* evCell = makeCell(M("TP_DOUBLEEXPOSURE_EV"), layerEvScale_, -4.0, 4.0, 0.05, 0.0);
    Gtk::Widget* opacityCell = makeCell(M("TP_DOUBLEEXPOSURE_OPACITY"), layerOpacityScale_, 0.0, 100.0, 1.0, 100.0);
    Gtk::Widget* gateCell = makeCell(M("TP_DOUBLEEXPOSURE_GATE_STRENGTH"), gateStrengthScale_, 0.0, 100.0, 1.0, 0.0);
    gateStrengthScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_GATE_TOOLTIP"));
    blendBox->pack_start(*makeRow({evCell, opacityCell}), Gtk::PACK_SHRINK);
    revealBox->pack_start(*gateCell, Gtk::PACK_SHRINK);

    // Comparative bright/dark only: how wide the hand-over between frames is.
    // Edge blend sits beside it: both are about how one frame gives way to
    // another, one in tone and one in place.
    Gtk::Widget* softnessCell = makeCell(M("TP_DOUBLEEXPOSURE_SOFTNESS"), softnessScale_, 0.0, 2.0, 0.05, 0.5);
    softnessScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SOFTNESS_TOOLTIP"));
    Gtk::Widget* edgeCell = makeCell(M("TP_DOUBLEEXPOSURE_EDGEFEATHER"), edgeFeatherScale_, 0.0, 100.0, 1.0, 35.0);
    edgeFeatherScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDGEFEATHER_TOOLTIP"));
    blendBox->pack_start(*makeRow({softnessCell, edgeCell}), Gtk::PACK_SHRINK);

    motionExpander_ = Gtk::manage(new DESection(M("DE_MOTION")));
    auto motionBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    auto motionHeader = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    motionPath_ = Gtk::manage(new MyComboBoxText());
    for (const auto key : {"DE_MOTION_OFF", "DE_MOTION_LINE", "DE_MOTION_WAVE", "DE_MOTION_ZIGZAG",
                           "DE_MOTION_SWIRL", "DE_MOTION_ZOOM_IN", "DE_MOTION_ZOOM_OUT",
                           "DE_MOTION_HANDHELD", "DE_MOTION_WANDER"}) motionPath_->append(M(key));
    motionPath_->setPreferredWidth(130, 190);
    motionPath_->set_active(0);
    motionPath_->connect(motionPath_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::motionControlChanged)));
    motionHeader->pack_start(*motionPath_, Gtk::PACK_EXPAND_WIDGET);
    motionRandomize_ = Gtk::manage(new Gtk::Button());
    motionRandomize_->set_image(*Gtk::manage(new RTImage("refresh-modern", Gtk::ICON_SIZE_BUTTON)));
    motionRandomize_->set_relief(Gtk::RELIEF_NONE);
    motionRandomize_->set_tooltip_text(M("DE_MOTION_RANDOMIZE"));
    motionRandomize_->signal_clicked().connect([this] {
        if (selectedLayer_ >= params_.layers.size()) return;
        auto& m = params_.layers[selectedLayer_].motion;
        m.seed = rtengine::demotion::nextSeed(m.seed);
        schedulePreviewUpdate();
    });
    motionHeader->pack_start(*motionRandomize_, Gtk::PACK_SHRINK);
    motionSpinner_ = Gtk::manage(new Gtk::Spinner());
    motionSpinner_->set_no_show_all(true);
    motionHeader->pack_start(*motionSpinner_, Gtk::PACK_SHRINK);
    motionBox->pack_start(*motionHeader, Gtk::PACK_SHRINK);
    auto shutterCell = makeCell(M("DE_MOTION_SHUTTER"), motionShutter_, .01, 4, .01, .25);
    auto speedCell = makeCell(M("DE_MOTION_SPEED"), motionSpeed_, 0, 100, 1, 32);
    motionShutter_->set_tooltip_text(M("DE_MOTION_SHUTTER_TOOLTIP"));
    motionSpeed_->set_tooltip_text(M("DE_MOTION_SPEED_TOOLTIP"));
    motionBox->pack_start(*makeRow({shutterCell, speedCell}), Gtk::PACK_SHRINK);
    auto directionCell = makeCell(M("DE_MOTION_DIRECTION"), motionDirection_, -180, 180, 1, 0);
    auto noiseCell = makeCell(M("DE_MOTION_NOISE"), motionNoise_, 0, 100, 1, 12);
    motionNoise_->set_tooltip_text(M("DE_MOTION_NOISE_TOOLTIP"));
    motionBox->pack_start(*makeRow({directionCell, noiseCell}), Gtk::PACK_SHRINK);
    motionBox->pack_start(*makeCell(M("DE_MOTION_COMPLEXITY"), motionComplexity_, 1, 8, 1, 3), Gtk::PACK_SHRINK);
    motionExpander_->setContent(*motionBox);

    // Placement of the selected exposure over the base frame; the preview
    // drives the same three values by dragging.
    // Where the exposure sits, turns and mirrors. Behind a header and closed
    // to begin with: most of the work here is choosing frames and setting
    // their tone, and the preview drags all three of these anyway.
    adjustExpander_ = Gtk::manage(new DESection(M("TP_DOUBLEEXPOSURE_SEC_PLACE")));
    Gtk::Box* adjustBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    adjustExpander_->setContent(*adjustBox);
    adjustExpander_->set_expanded(false);

    Gtk::Widget* offXCell = makeCell(M("TP_DOUBLEEXPOSURE_OFFSETX"), offsetXScale_, -150.0, 150.0, 0.5, 0.0);
    offsetXScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    Gtk::Widget* offYCell = makeCell(M("TP_DOUBLEEXPOSURE_OFFSETY"), offsetYScale_, -150.0, 150.0, 0.5, 0.0);
    offsetYScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    Gtk::Widget* sizeCell = makeCell(M("TP_DOUBLEEXPOSURE_SCALE"), scaleScale_, 10.0, 400.0, 1.0, 100.0);
    scaleScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    adjustBox->pack_start(*makeRow({offXCell, offYCell, sizeCell}), Gtk::PACK_SHRINK);

    // Rotation, with quarter turns to hand and a mirror toggle. The preview's
    // grip drives the same value.
    Gtk::Box* rotRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Widget* rotScaleRow = makeCell(M("TP_DOUBLEEXPOSURE_ROTATE"), rotateScale_, -180.0, 180.0, 0.5, 0.0);
    rotateScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_ROTATE_TOOLTIP"));
    rotRow->pack_start(*rotScaleRow, Gtk::PACK_EXPAND_WIDGET);
    rotateLeft_ = Gtk::manage(new Gtk::Button());
    rotateLeft_->set_image(*Gtk::manage(new RTImage("rotate-left-small", Gtk::ICON_SIZE_BUTTON)));
    rotateLeft_->set_relief(Gtk::RELIEF_NONE);
    rotateLeft_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_ROTATE_LEFT"));
    rotateLeft_->signal_clicked().connect([this]() { nudgeRotation(-90.0); });
    rotRow->pack_start(*rotateLeft_, Gtk::PACK_SHRINK);
    rotateRight_ = Gtk::manage(new Gtk::Button());
    rotateRight_->set_image(*Gtk::manage(new RTImage("rotate-right-small", Gtk::ICON_SIZE_BUTTON)));
    rotateRight_->set_relief(Gtk::RELIEF_NONE);
    rotateRight_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_ROTATE_RIGHT"));
    rotateRight_->signal_clicked().connect([this]() { nudgeRotation(90.0); });
    rotRow->pack_start(*rotateRight_, Gtk::PACK_SHRINK);
    rotRow->set_valign(Gtk::ALIGN_END);
    adjustBox->pack_start(*rotRow, Gtk::PACK_SHRINK);

    patternExpander_ = Gtk::manage(new DESection(M("TP_DOUBLEEXPOSURE_PATTERN")));
    Gtk::Box* patternBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
    patternExpander_->setContent(*patternBox);
    patternExpander_->set_expanded(false);

    Gtk::Box* patRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label* patLab = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_PATTERN"), Gtk::ALIGN_START));
    patLab->set_size_request(150, -1);
    patLab->set_xalign(0.f);
    patternMethod_ = Gtk::manage(new MyComboBoxText());
    patternMethod_->append(M("TP_DOUBLEEXPOSURE_PATTERN_OFF"));
    patternMethod_->append(M("TP_DOUBLEEXPOSURE_PATTERN_REPEAT"));
    patternMethod_->append(M("TP_DOUBLEEXPOSURE_PATTERN_MIRROR"));
    patternMethod_->append(M("TP_DOUBLEEXPOSURE_PATTERN_RADIAL"));
    patternMethod_->set_active(0);
    patternMethod_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_TOOLTIP"));
    patternMethod_->setPreferredWidth(120, 180);
    patternMethod_->connect(patternMethod_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged)));
    patRow->pack_start(*patLab, Gtk::PACK_SHRINK);
    patRow->pack_start(*patternMethod_, Gtk::PACK_EXPAND_WIDGET);
    patternUpright_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_PATTERN_UPRIGHT")));
    patternUpright_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_UPRIGHT_TOOLTIP"));
    patternUpright_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    patRow->pack_start(*patternUpright_, Gtk::PACK_SHRINK);
    patternBox->pack_start(*patRow, Gtk::PACK_SHRINK);

    Gtk::Widget* spacingCell = makeCell(M("TP_DOUBLEEXPOSURE_PATTERN_SPACING"), patternSpacingScale_, -90.0, 200.0, 1.0, 0.0);
    patternSpacingScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_SPACING_TOOLTIP"));
    Gtk::Widget* staggerCell = makeCell(M("TP_DOUBLEEXPOSURE_PATTERN_STAGGER"), patternStaggerScale_, 0.0, 100.0, 1.0, 0.0);
    patternStaggerScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_STAGGER_TOOLTIP"));
    gridRow_ = makeRow({spacingCell, staggerCell});
    patternBox->pack_start(*gridRow_, Gtk::PACK_SHRINK);

    // Radial's own pair: how many copies, and how wide a ring they stand on.
    Gtk::Widget* countCell = makeCell(M("TP_DOUBLEEXPOSURE_PATTERN_COUNT"), patternCountScale_, 1.0, 24.0, 1.0, 6.0);
    patternCountScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_COUNT_TOOLTIP"));
    Gtk::Widget* diameterCell = makeCell(M("TP_DOUBLEEXPOSURE_PATTERN_DIAMETER"), patternDiameterScale_, 0.0, 200.0, 1.0, 60.0);
    patternDiameterScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_DIAMETER_TOOLTIP"));
    // Selection feather rides beside the ring controls: with a cut-out motif
    // the two are adjusted together. It stays when the pattern is a grid (or
    // off) and the ring controls step aside, so the row is shown whenever
    // either half of it has something to say.
    Gtk::Widget* subjFeatherCell = makeCell(M("TP_DOUBLEEXPOSURE_SUBJECT_FEATHER"), subjectFeatherScale_, 0.0, 100.0, 1.0, 25.0);
    subjectFeatherScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_FEATHER_TOOLTIP"));
    Gtk::Widget* twistCell = makeCell(M("TP_DOUBLEEXPOSURE_PATTERN_TWIST"), patternTwistScale_, -180.0, 180.0, 1.0, 0.0);
    patternTwistScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PATTERN_TWIST_TOOLTIP"));
    ringCountCell_ = countCell;
    ringDiameterCell_ = diameterCell;
    subjectFeatherCell_ = subjFeatherCell;
    radialRow_ = makeRow({countCell, diameterCell, subjFeatherCell});
    patternBox->pack_start(*radialRow_, Gtk::PACK_SHRINK);
    twistRow_ = makeRow({twistCell});
    patternBox->pack_start(*twistRow_, Gtk::PACK_SHRINK);

    flipH_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_FLIPH")));
    flipH_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_FLIPH_TOOLTIP"));
    flipH_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    adjustBox->pack_start(*flipH_, Gtk::PACK_SHRINK);


    // Subject selection, segmented on the partner itself. Hidden outright
    // when this build has no segmentation model rather than shown dead.
    subjectRow_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    Gtk::Box* subjectOpts = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 10));
    Gtk::Box* subjectBtns = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    Gtk::Label* subjLab = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_SUBJECT"), Gtk::ALIGN_START));
    subjLab->set_size_request(150, -1);
    subjLab->set_xalign(0.f);
    subjectMethod_ = Gtk::manage(new MyComboBoxText());
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_OFF"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_SUBJECT"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_PERSON"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_SKY"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_VEGETATION"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_BUILDING"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_VEHICLE"));
    subjectMethod_->append(M("TP_DOUBLEEXPOSURE_SUBJECT_ANIMAL"));
    subjectMethod_->set_active(0);
    subjectMethod_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_TOOLTIP"));

#ifdef RT_AI_MASKING
    // A second renderer after the text, filled per row: the list says which
    // classes are in this partner far better by showing them than by
    // reporting percentages next to their names.
    subjectMethod_->pack_start(subjectTileCell_, false);
    subjectMethod_->set_cell_data_func(subjectTileCell_,
                                       sigc::mem_fun(*this, &DoubleExposureDlg::renderSubjectTile));
#endif
    subjectMethod_->setPreferredWidth(120, 180);
    subjectMethod_->connect(subjectMethod_->signal_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged)));
    (void)subjLab;
    subjectRow_->pack_start(*subjectMethod_, Gtk::PACK_SHRINK);
    subjectRow_->pack_start(*subjectOpts, Gtk::PACK_SHRINK);
    subjectRow_->pack_start(*subjectBtns, Gtk::PACK_SHRINK);
    subjectInvert_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_SUBJECT_INVERT")));
    subjectInvert_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_INVERT_TOOLTIP"));
    subjectInvert_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    subjectOpts->pack_start(*subjectInvert_, Gtk::PACK_SHRINK);
    subjectCrop_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_SUBJECT_CROP")));
    subjectCrop_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_CROP_TOOLTIP"));
    subjectCrop_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    subjectOpts->pack_start(*subjectCrop_, Gtk::PACK_SHRINK);
    subjectEdit_ = Gtk::manage(new Gtk::Button(M("TP_DOUBLEEXPOSURE_SUBJECT_EDIT")));
    subjectEdit_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SUBJECT_EDIT_TOOLTIP"));
    subjectEdit_->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposureDlg::openMaskEditor));
    subjectBtns->pack_start(*subjectEdit_, Gtk::PACK_EXPAND_WIDGET);
    // Click-to-select: while down, clicks on the preview become include and
    // exclude prompts for this exposure's own picture.
    subjectPick_ = Gtk::manage(new Gtk::ToggleButton(M("TP_DOUBLEEXPOSURE_PICK")));
    subjectPick_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PICK_TOOLTIP"));
    subjectPick_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::syncPickMode));
    subjectBtns->pack_start(*subjectPick_, Gtk::PACK_EXPAND_WIDGET);
    // The partner as its own edits render it, in place of the neutral decode.
    asEdited_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_ASEDITED")));
    asEdited_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_ASEDITED_TOOLTIP"));
    asEdited_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    selectBox->pack_start(*subjectRow_, Gtk::PACK_SHRINK);

    // Depth confinement, directly under the selection: an exposure can be
    // kept to what is near in its own picture, or to what is far. The range
    // widget and its maths are the compositor's, so a range reads the same
    // in both tools. Hidden, like the subject row, when no model is installed.
    depthExpander_ = Gtk::manage(new DESection(M("TP_DOUBLEEXPOSURE_DEPTH")));
    depthExpander_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_TOOLTIP"));
    Gtk::Box* depthBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
    Gtk::Box* depthHeader = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    depthEnable_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_DEPTH_ENABLE")));
    depthEnable_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_ENABLE_TOOLTIP"));
    depthEnable_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    depthHeader->pack_start(*depthEnable_, Gtk::PACK_SHRINK);
    depthInvert_ = Gtk::manage(new Gtk::CheckButton(M("TP_DOUBLEEXPOSURE_DEPTH_INVERT")));
    depthInvert_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_INVERT_TOOLTIP"));
    depthInvert_->signal_toggled().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    depthHeader->pack_start(*depthInvert_, Gtk::PACK_SHRINK);
    depthStatus_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_END));
    depthStatus_->get_style_context()->add_class("dim-label");
    depthStatus_->set_ellipsize(Pango::ELLIPSIZE_END);
    depthHeader->pack_end(*depthStatus_, Gtk::PACK_SHRINK);
    depthBox->pack_start(*depthHeader, Gtk::PACK_SHRINK);
    depthRange_ = Gtk::manage(new compositeui::DepthRange());
    depthRange_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_RANGE_TOOLTIP"));
    depthRange_->changed = [this] { depthRangeChanged(); };
    depthBox->pack_start(*depthRange_, Gtk::PACK_SHRINK);
    Gtk::Widget* depthNearCell = makeCell(M("TP_DOUBLEEXPOSURE_DEPTH_NEAR"), depthNearShape_, 0.1, 10.0, 0.1, 1.0);
    depthNearShape_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_SHAPE_TOOLTIP"));
    Gtk::Widget* depthFarCell = makeCell(M("TP_DOUBLEEXPOSURE_DEPTH_FAR"), depthFarShape_, 0.1, 10.0, 0.1, 1.0);
    depthFarShape_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_DEPTH_SHAPE_TOOLTIP"));
    depthBox->pack_start(*makeRow({depthNearCell, depthFarCell}), Gtk::PACK_SHRINK);
    depthExpander_->setContent(*depthBox);
    depthExpander_->set_expanded(false);
    selectBox->pack_start(*depthExpander_, Gtk::PACK_SHRINK);

    if (!rtengine::PartnerDepthStore::getInstance().available()) {
        depthExpander_->set_no_show_all(true);
        depthExpander_->hide();
    }


#ifdef RT_AI_MASKING
    const bool haveSegmentation = rtengine::getAISegmentationEngine().isInitialized();
#else
    const bool haveSegmentation = false;
#endif

    haveSegmentation_ = haveSegmentation;

    if (!haveSegmentation) {
        subjectRow_->set_no_show_all(true);
        subjectRow_->hide();

        if (!rtengine::PartnerDepthStore::getInstance().available()) {
            selectSection_->set_no_show_all(true);
            selectSection_->hide();
        }
    }

    resetPlacement_ = Gtk::manage(new Gtk::Button(M("TP_DOUBLEEXPOSURE_PLACEMENT_RESET")));
    resetPlacement_->set_halign(Gtk::ALIGN_END);
    resetPlacement_->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposureDlg::onPreviewReset));
    adjustBox->pack_start(*resetPlacement_, Gtk::PACK_SHRINK);

    // Inspector order: the layer, then Blend / Select / Reveal / Place /
    // Pattern / Motion; the whole stack last.
    right->pack_start(*layerHeader, Gtk::PACK_SHRINK);
    asEdited_->set_margin_bottom(4);
    right->pack_start(*asEdited_, Gtk::PACK_SHRINK);

    for (DESection* section : {blendSection_, selectSection_, revealSection_, adjustExpander_, patternExpander_, motionExpander_}) {
        right->pack_start(*section, Gtk::PACK_SHRINK);
    }

    Gtk::Label* stackHeading = Gtk::manage(new Gtk::Label(M("TP_DOUBLEEXPOSURE_SEC_STACK"), Gtk::ALIGN_START));
    stackHeading->get_style_context()->add_class("tool-section-label");
    stackHeading->set_margin_top(14);
    right->pack_start(*stackHeading, Gtk::PACK_SHRINK);
    right->pack_start(*autoGain_, Gtk::PACK_SHRINK);
    right->pack_start(*baseEvCell, Gtk::PACK_SHRINK);
    right->pack_start(*latitudeCell, Gtk::PACK_SHRINK);

    Gtk::ScrolledWindow* inspector = Gtk::manage(new Gtk::ScrolledWindow());
    inspector->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    inspector->set_size_request(340, -1);
    inspector->add(*right);

    Gtk::Box* stage = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
    stage->pack_start(*centre, Gtk::PACK_EXPAND_WIDGET);
    stage->pack_start(*inspector, Gtk::PACK_SHRINK);
    split->pack2(*stage, true, false);
    content->pack_start(*split, Gtk::PACK_EXPAND_WIDGET);

    // --- tray: base plate + ordered overlay chips ---
    trayBox_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    trayBox_->set_tooltip_text(M("DOUBLEEXPOSURE_ORDER_TOOLTIP"));
    Gtk::ScrolledWindow* trayScroll = Gtk::manage(new Gtk::ScrolledWindow());
    trayScroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER);
    trayScroll->set_min_content_height(44);
    trayScroll->add(*trayBox_);
    auto trayRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    addMotion_ = Gtk::manage(new Gtk::Button(M("DE_MOTION")));
    addMotion_->set_image(*Gtk::manage(new RTImage("add-small", Gtk::ICON_SIZE_BUTTON)));
    addMotion_->set_always_show_image(true);
    addMotion_->set_tooltip_text(M("DE_MOTION_ADD_TOOLTIP"));
    addMotion_->set_valign(Gtk::ALIGN_CENTER);
    addMotion_->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposureDlg::addMotionLayer));
    trayRow->pack_start(*trayScroll, Gtk::PACK_EXPAND_WIDGET);
    trayRow->pack_end(*addMotion_, Gtk::PACK_SHRINK);
    centre->pack_start(*trayRow, Gtk::PACK_SHRINK);

    applyDialogTemplate(*this, M("GENERAL_OK"));

    show_all_children();
    spinner_->hide();

    rebuildTray();
    syncLayerControls();
    requestPreviewThumbs();

    // Resume where the picker was last closed: scope and grid scroll. The
    // global button's handler runs the scan for that scope.
    // Where the picker opens: on the photograph being edited if the list
    // contains it, otherwise back where it was last closed.
    pendingScrollToBase_ = true;
    pendingScrollRestore_ = savedOpts.dePickerScroll > 0.0 ? savedOpts.dePickerScroll : -1.0;

    if (savedOpts.dePickerGlobalScope) {
        globalBtn_->set_active(true);
    } else {
        startScan(false);
    }
}

void DoubleExposureDlg::savePickerState()
{
    Options& o = App::get().mut_options();
    o.dePickerGlobalScope = globalScope_;
    o.dePickerPickedOnly = pickedFilter_->get_active();
    o.dePickerMinStars = std::max(0, starsFilter_->get_active_row_number());
    o.dePickerHighRes = highRes_->get_active();
    o.dePickerScroll = gridScroll_ ? gridScroll_->get_vadjustment()->get_value() : 0.0;
    Options::save();
}

void DoubleExposureDlg::restoreScroll(bool final)
{
    if ((pendingScrollRestore_ < 0.0 && !pendingScrollToBase_) || !gridScroll_) {
        return;
    }

    const auto adj = gridScroll_->get_vadjustment();

    // The base image takes precedence over the remembered position while the
    // list still might contain it. Results stream in, so a lookup that fails
    // now can succeed on the next pass; only the final pass gives up on it.
    if (pendingScrollToBase_ && grid_) {
        const int rowTop = grid_->rowTopOf(baseImagePath_);

        if (rowTop >= 0) {
            // A third from the top rather than flush against it: the frames
            // shot just before this one are as likely to be wanted as the
            // ones after, and they are the rows above.
            const double lead = adj->get_page_size() / 3.0;
            const double target = std::max(0.0, rowTop - lead);
            const double limit = std::max(0.0, adj->get_upper() - adj->get_page_size());
            const double landed = std::min(target, limit);

            adj->set_value(landed);
            lastRestoredScroll_ = landed;

            // Only settle once the grid has stopped growing under us, or the
            // row will move after we have already scrolled to it.
            if (final || landed >= target - 0.5) {
                pendingScrollToBase_ = false;
                pendingScrollRestore_ = -1.0;
            }

            return;
        }

        if (final) {
            // Not in this list -- global mode showing the selects index, say.
            // Fall through to the remembered position from here on.
            pendingScrollToBase_ = false;
        } else {
            return;
        }
    }

    if (pendingScrollRestore_ < 0.0) {
        return;
    }

    if (lastRestoredScroll_ >= 0.0 && std::fabs(adj->get_value() - lastRestoredScroll_) > 1.0) {
        // The user scrolled while results were still streaming in: theirs wins.
        pendingScrollRestore_ = -1.0;
        return;
    }

    const double maxValue = std::max(0.0, adj->get_upper() - adj->get_page_size());
    const double value = std::min(pendingScrollRestore_, maxValue);
    adj->set_value(value);
    lastRestoredScroll_ = value;

    if (final || value >= pendingScrollRestore_ - 0.5) {
        pendingScrollRestore_ = -1.0;
    }
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

    // The composite itself runs on the engine's decodes.
    std::vector<Glib::ustring> scenePaths;
    scenePaths.push_back(baseImagePath_);

    for (const auto& layer : params_.layers) {
        if (layer.enabled && !layer.path.empty()) {
            scenePaths.push_back(layer.source());
        }
    }

    requestScenePlates(scenePaths, height);
    requestPartnerMasks();
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
    for (const auto& queue : {motionQueue_, previewQueue_}) {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->stop = true;
        ++queue->revision;
        queue->job = {};
        queue->wake.notify_one();
    }
    *alive_ = false;
    previewSettle_.disconnect();
    savePickerState(); // the widgets outlive this body

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

        // Results are complete: settle the saved scroll on the final geometry.
        if (pendingScrollRestore_ >= 0.0) {
            auto alive = alive_;
            Glib::signal_idle().connect_once([this, alive]() {
                if (*alive) {
                    restoreScroll(true);
                }
            });
        }
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
            // The base's styled preview render is the plate the dialog
            // composites onto: strip the image's own double exposure from it.
            const bool basePlate = !neutral && !isGrid && *it == baseImagePath_;
            queue.emplace_front(DEThumbReq{*it, height, neutral, basePlate, false, false, 0, 0.0, false, {}});
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

        const Glib::ustring scenePath = baseImagePath_;
        const Glib::ustring sceneProfile = workingProfile_;
        const std::shared_ptr<const rtengine::ColorTemp> sceneWb = baseWb_;
        const int sceneRotate = baseCoarseRotate_;
        const bool sceneHflip = baseHflip_;
        const bool sceneVflip = baseVflip_;

        detachQuietly(std::thread([this, queue, alive, scenePath, sceneProfile, sceneWb, sceneRotate, sceneHflip, sceneVflip]() {
#ifdef _OPENMP
            omp_set_num_threads(2);
#endif
            for (;;) {
                Glib::ustring path;
                int height = 0;

                bool neutral = false;
                bool basePlate = false;
                bool scene = false;
                bool maskWarm = false;
                int maskClass = 0;
                double maskFeather = 0.0;
                bool maskInvert = false;
                rtengine::MaskPaint maskPaint;

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
                    basePlate = source.front().basePlate;
                    scene = source.front().scene;
                    maskWarm = source.front().maskWarm;
                    maskClass = source.front().maskClass;
                    maskFeather = source.front().maskFeather;
                    maskInvert = source.front().maskInvert;
                    maskPaint = source.front().maskPaint;
                    source.pop_front();
                }

#ifdef RT_AI_MASKING
                if (maskWarm && maskClass < 0) {
                    // No class: this one is only after the numbers the class
                    // list is labelled with.
                    rtengine::PartnerMaskStore::getInstance().warmCoverage(path, sceneProfile, true);

                    Glib::signal_idle().connect_once([this, alive, path]() {
                        if (!*alive) {
                            return;
                        }

                        onCoverageMeasured(path);
                    });
                    continue;
                }

                if (maskWarm) {
                    if (rtengine::settings->verbose) {
                        std::fprintf(stderr, "[dePickerWarm] %s class=%d feather=%.0f\n",
                                     path.c_str(), maskClass, maskFeather);
                    }

                    rtengine::PartnerMaskStore::getInstance().getMask(
                        path, sceneProfile,
                        static_cast<DoubleExposureParams::MaskClass>(maskClass),
                        maskFeather, maskInvert, maskPaint, true);

                    Glib::signal_idle().connect_once([this, alive]() {
                        if (!*alive) {
                            return;
                        }

                        schedulePreviewUpdate();
                    });
                    continue;
                }
#endif

                if (scene) {
                    // Only the base takes the edit's white balance and coarse
                    // orientation: the engine samples partners upright in
                    // their own camera balance.
                    const bool isBase = path == scenePath;
                    std::shared_ptr<const DEScenePlate> plate = loadScenePlate(
                        path, height, sceneProfile, isBase ? sceneWb.get() : nullptr,
                        isBase ? sceneRotate : 0, isBase && sceneHflip, isBase && sceneVflip);

                    Glib::signal_idle().connect_once([this, alive, path, height, plate]() {
                        if (!*alive) {
                            return;
                        }

                        onSceneLoaded(path, height, plate);
                    });
                    continue;
                }

                Glib::RefPtr<Gdk::Pixbuf> pixbuf = pixbufFromThumb(path, height, neutral, basePlate);

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

// Segmentation is the one input the preview cannot compute on the spot, so
// the stack's masks are warmed on the decoder pool and the preview reads
// whatever is cached. Until one arrives its layer simply renders unmasked.
void DoubleExposureDlg::requestPartnerMasks()
{
#ifdef RT_AI_MASKING
    if (!rtengine::getAISegmentationEngine().isInitialized()) {
        return;
    }

    std::vector<DEThumbReq> needed;

    for (const auto& layer : params_.layers) {
        if (!layer.enabled || layer.path.empty()
                || layer.maskClass == DoubleExposureParams::MaskClass::OFF) {
            continue;
        }

        if (layer.maskClass == DoubleExposureParams::MaskClass::POINTS && !layer.maskPaint.hasPrompts()) {
            continue;
        }

        if (rtengine::PartnerMaskStore::getInstance().peekMask(layer.source(), workingProfile_,
                layer.maskClass, layer.maskFeather, layer.maskInvert, layer.maskPaint)) {
            continue;
        }

        const Glib::ustring key = layer.source() + Glib::ustring::compose("|mask|%1|%2|%3|%4",
                                  static_cast<int>(layer.maskClass),
                                  static_cast<int>(std::lround(layer.maskFeather)),
                                  layer.maskInvert ? 1 : 0, layer.maskPaint.hash());

        if (pendingThumbs_.count(key)) {
            continue;
        }

        pendingThumbs_.insert(key);
        needed.push_back(DEThumbReq{layer.source(), 0, false, false, false, true,
                                    static_cast<int>(layer.maskClass), layer.maskFeather,
                                    layer.maskInvert, layer.maskPaint});
    }

    if (needed.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(thumbQueue_->mutex);

        for (auto it = needed.rbegin(); it != needed.rend(); ++it) {
            thumbQueue_->preview.emplace_front(*it);
        }
    }

    pumpThumbQueue();
#endif
}

// A partner's class coverage is only known once it has been segmented, and
// until this existed nothing segmented a partner until a class had already
// been picked -- so the numbers meant to help make that choice only appeared
// after it had been made. One segmentation per file, on the same pool as the
// thumbnails, and the list relabels itself when it lands.
void DoubleExposureDlg::requestCoverage(const Glib::ustring& path)
{
#ifdef RT_AI_MASKING
    if (path.empty() || !rtengine::getAISegmentationEngine().isInitialized()) {
        return;
    }

    if (rtengine::PartnerMaskStore::getInstance().hasCoverage(path, workingProfile_)) {
        return;
    }

    // The subject model loads a few seconds after startup, and a reading taken
    // before it did is worth taking again -- hence its state in the key.
    const Glib::ustring key = path + "|coverage|"
                              + (rtengine::getAISubjectEngine().isInitialized() ? "1" : "0");

    if (pendingThumbs_.count(key)) {
        return;
    }

    pendingThumbs_.insert(key);

    {
        std::lock_guard<std::mutex> lock(thumbQueue_->mutex);
        thumbQueue_->preview.emplace_back(
            DEThumbReq{path, 0, false, false, false, true, -1, 0.0, false, {}});
    }

    pumpThumbQueue();
#endif
}

void DoubleExposureDlg::onCoverageMeasured(const Glib::ustring& path)
{
#ifdef RT_AI_MASKING
    // Only the layer on show is labelled, so a measurement for any other one
    // just sits in the store until its layer is selected.
    if (selectedLayer_ < params_.layers.size() && params_.layers[selectedLayer_].path == path) {
        refreshSubjectLabels(path);
    }

#endif
}

void DoubleExposureDlg::requestScenePlates(const std::vector<Glib::ustring>& paths, int height)
{
    std::vector<Glib::ustring> needed;
    const auto& map = height == PREVIEW_THUMB_HI ? sceneHiPix_ : scenePix_;

    for (const auto& path : paths) {
        if (path.empty()) {
            continue;
        }

        const Glib::ustring key = path + Glib::ustring::compose("|%1|scene", height);

        if (pendingThumbs_.count(key) || map.count(path)) {
            continue;
        }

        pendingThumbs_.insert(key);
        needed.push_back(path);
    }

    if (needed.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(thumbQueue_->mutex);

        for (auto it = needed.rbegin(); it != needed.rend(); ++it) {
            thumbQueue_->preview.emplace_front(DEThumbReq{*it, height, false, false, true, false, 0, 0.0, false, {}});
        }
    }

    pumpThumbQueue();
}

void DoubleExposureDlg::onSceneLoaded(const Glib::ustring& path, int height, std::shared_ptr<const DEScenePlate> plate)
{
    pendingThumbs_.erase(path + Glib::ustring::compose("|%1|scene", height));

    if (!plate || !usesSource(path)) {
        return;
    }

    (height == PREVIEW_THUMB_HI ? sceneHiPix_ : scenePix_)[path] = std::move(plate);
    schedulePreviewUpdate();
}

// What the scene plates must match about the base edit: its working profile
// (so the store's cache entries are the very ones the engine uses), a fixed
// white balance when the edit has one, and its coarse orientation. Mirrors
// the coordinator's WB resolution; auto methods fall back to the camera
// balance because only the engine can compute them.
void DoubleExposureDlg::initSceneContext()
{
    workingProfile_ = "ProPhoto";

    Thumbnail* thm = baseImagePath_.empty() ? nullptr : CacheManager::getInstance()->getEntry(baseImagePath_);

    if (thm) {
        const rtengine::procparams::ProcParams bpp = thm->getProcParamsCopy();

        if (!bpp.icm.workingProfile.empty()) {
            workingProfile_ = bpp.icm.workingProfile;
        }

        baseCoarseRotate_ = bpp.coarse.rotate;
        baseHflip_ = bpp.coarse.hflip;
        baseVflip_ = bpp.coarse.vflip;

        const Glib::ustring method = bpp.wb.method;

        if (!bpp.wb.enabled) {
            baseWb_ = std::make_shared<const rtengine::ColorTemp>();
        } else if (method != "Camera" && method.substr(0, 4) != "auto") {
            baseWb_ = std::make_shared<const rtengine::ColorTemp>(bpp.wb.temperature, bpp.wb.green, bpp.wb.equal,
                                                                  std::string(method), bpp.wb.observer);
        }

        thm->decreaseRef();
    }

    // working profile -> sRGB, both D50-adapted matrices in the ICC store
    const rtengine::ICCStore* store = rtengine::ICCStore::getInstance();
    const rtengine::TMatrix wp = store->workingSpaceMatrix(workingProfile_);
    const rtengine::TMatrix isrgb = store->workingSpaceInverseMatrix("sRGB");

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double v = 0.0;

            for (int k = 0; k < 3; ++k) {
                v += isrgb[i][k] * wp[k][j];
            }

            workingToSrgb_[i][j] = v;
        }
    }
}

void DoubleExposureDlg::onThumbLoaded(const Glib::ustring& path, int height, bool neutral, Glib::RefPtr<Gdk::Pixbuf> pixbuf)
{
    pendingThumbs_.erase(path + Glib::ustring::compose("|%1|%2", height, neutral ? 1 : 0));

    if (!pixbuf) {
        return;
    }
    if (height != GRID_THUMB_H && !usesSource(path)) return;

    if (height == GRID_THUMB_H) {
        gridPix_[path] = pixbuf;
        grid_->setItemPixbuf(path, pixbuf);
        gridUse_[path] = ++gridClock_;
    } else if (neutral) {
        (height == PREVIEW_THUMB_HI ? neutralHiPix_ : neutralPix_)[path] = pixbuf;
        schedulePreviewUpdate();
        rebuildTray();
    } else {
        (height == PREVIEW_THUMB_HI ? hiPix_ : bigPix_)[path] = pixbuf;
        schedulePreviewUpdate();
        rebuildTray();
    }
    trimPreviewCaches();
}

bool DoubleExposureDlg::usesSource(const Glib::ustring& path) const
{
    return path == baseImagePath_ || std::any_of(params_.layers.begin(), params_.layers.end(),
                                               [&](const DoubleExposureParams::Layer& l) { return l.path == path || l.source() == path; });
}

void DoubleExposureDlg::trimPreviewCaches()
{
    const auto removeUnused = [this](auto& entries) {
        for (auto i = entries.begin(); i != entries.end();)
            if (!usesSource(i->first)) i = entries.erase(i); else ++i;
    };
    removeUnused(bigPix_); removeUnused(hiPix_); removeUnused(neutralPix_); removeUnused(neutralHiPix_);
    removeUnused(scenePix_); removeUnused(sceneHiPix_);
    size_t bytes = 0;
    for (const auto& entry : gridPix_) bytes += size_t(entry.second->get_rowstride()) * entry.second->get_height();
    const auto adjustment = gridScroll_->get_vadjustment();
    const double top = adjustment->get_value(), page = std::max(1., adjustment->get_page_size());
    while ((bytes > 32U * 1024U * 1024U || gridPix_.size() > 512) && !gridPix_.empty()) {
        auto oldest = gridPix_.end();
        for (auto i = gridPix_.begin(); i != gridPix_.end(); ++i) {
            const int y = grid_->rowTopOf(i->first);
            if (y >= 0 && y >= top - page && y <= top + 2 * page) continue;
            if (oldest == gridPix_.end() || gridUse_[i->first] < gridUse_[oldest->first]) oldest = i;
        }
        if (oldest == gridPix_.end()) break;
        bytes -= size_t(oldest->second->get_rowstride()) * oldest->second->get_height();
        grid_->setItemPixbuf(oldest->first, {});
        gridUse_.erase(oldest->first);
        gridPix_.erase(oldest);
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

        if (photoTagFilter_ && !PhotoTags::instance().matches(item.path, photoTagFilter_->selected(),
                photoTagFilter_->matchAll(), photoTagFilter_->untagged())) continue;

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

    if (pendingScrollRestore_ >= 0.0) {
        auto alive = alive_;
        Glib::signal_idle().connect_once([this, alive]() {
            if (*alive) {
                restoreScroll(false);
            }
        });
    }
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
    for (const auto& item : gridPix_) {
        const int y = grid_->rowTopOf(item.first);
        if (y >= 0 && y >= value - page && y <= value + 2 * page) gridUse_[item.first] = ++gridClock_;
    }
    trimPreviewCaches();

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
    layer.motion = {}; // ordinary photo additions must not inherit a motion-only recipe
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

void DoubleExposureDlg::addMotionLayer()
{
    const int selected = demotionui::addMotionLayer(params_, baseImagePath_);
    if (selected < 0) return;
    selectedLayer_ = size_t(selected);
    grid_->setBadges(params_.layers);
    grid_->setSelectedBadge(selected);
    rebuildTray();
    syncLayerControls();
    motionExpander_->set_expanded(true);
    requestPreviewThumbs();
    schedulePreviewUpdate();
}

void DoubleExposureDlg::motionControlChanged()
{
    if (syncingControls_ || selectedLayer_ >= params_.layers.size()) return;
    auto& m = params_.layers[selectedLayer_].motion;
    m.path = static_cast<rtengine::demotion::Path>(std::max(0, motionPath_->get_active_row_number()));
    m.shutter = motionShutter_->getValue();
    m.speed = motionSpeed_->getValue();
    m.direction = motionDirection_->getValue();
    m.noise = motionNoise_->getValue();
    m.complexity = motionComplexity_->getValue();
    const bool enabled = m.path != rtengine::demotion::Path::OFF;
    for (auto scale : {motionShutter_, motionSpeed_, motionDirection_, motionNoise_, motionComplexity_}) scale->set_sensitive(enabled);
    motionRandomize_->set_sensitive(enabled && (m.noise > 0 || m.path == rtengine::demotion::Path::HANDHELD || m.path == rtengine::demotion::Path::WANDER));
    schedulePreviewUpdate();
}

void DoubleExposureDlg::requestMotionPreviews(const std::vector<DEMotionRequest>& requests)
{
    const bool quick = !requests.empty() && requests.front().quick;
    std::string batchKey;
    for (const auto& request : requests) batchKey += request.key + "\n";
    if (batchKey == pendingMotionKey_) return;
    pendingMotionKey_ = batchKey;
    auto queue = motionQueue_;
    const unsigned revision = ++queue->revision;
    auto life = alive_;
    motionSpinner_->show();
    motionSpinner_->start();
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->job = [this, requests, batchKey, queue, revision, life, quick] {
        rtengine::EffectPerfScope timing(quick ? "motion-draft" : "motion-settled");
        std::vector<std::shared_ptr<const DEMotionPlate>> results;
        std::string error;
        try {
            for (const auto& request : requests) {
                if (!*life || revision != queue->revision) return;
                auto result = std::make_shared<DEMotionPlate>();
                result->key = request.key;
                result->source = request.source;
#ifdef RT_AI_MASKING
                result->mask = request.mask;
                result->depth = request.depth;
                result->depthSelect = request.depthSelect;
#endif
                const auto& src = *request.source;
                const float ratio = std::min(1.f, float(request.edge) / std::max(src.w, src.h));
                result->w = std::max(1, int(src.w * ratio));
                result->h = std::max(1, int(src.h * ratio));
                result->pixels.resize(size_t(result->w) * result->h);
                const auto path = rtengine::demotion::trajectory(request.params, request.quick ? result->w : src.w, 192);
                const auto read = [&](float u, float v) {
                    const float fx = std::max(0.f, std::min(float(src.w - 1), u * src.w - .5f));
                    const float fy = std::max(0.f, std::min(float(src.h - 1), v * src.h - .5f));
                    const int x = int(fx), y = int(fy), nx = std::min(x + 1, src.w - 1), ny = std::min(y + 1, src.h - 1);
                    const float dx = fx - x, dy = fy - y;
                    const auto channel = [&](int c) {
                        return (1 - dy) * ((1 - dx) * src.rgb[(size_t(y) * src.w + x) * 3 + c] + dx * src.rgb[(size_t(y) * src.w + nx) * 3 + c]) +
                               dy * ((1 - dx) * src.rgb[(size_t(ny) * src.w + x) * 3 + c] + dx * src.rgb[(size_t(ny) * src.w + nx) * 3 + c]);
                    };
                    rtengine::demotion::Pixel p{channel(0), channel(1), channel(2), 1};
#ifdef RT_AI_MASKING
                    if (request.mask) p.a = request.mask->sample(u * request.mask->fullWidth, v * request.mask->fullHeight);
                    if (request.depth) p.a *= request.depthSelect.weight(request.depth->sample(u * request.depth->fullWidth, v * request.depth->fullHeight));
#endif
                    return p;
                };
#ifdef _OPENMP
                #pragma omp parallel for schedule(dynamic, 4)
#endif
                for (int y = 0; y < result->h; ++y) {
                    if (!*life || revision != queue->revision) continue;
                    for (int x = 0; x < result->w; ++x) {
                        result->pixels[size_t(y) * result->w + x] = rtengine::demotion::sample(
                            path, (x + .5f) / result->w, (y + .5f) / result->h, float(src.w) / src.h, read);
                    }
                }
                results.push_back(result);
            }
        } catch (const std::exception& e) { error = e.what(); }
          catch (const Glib::Error& e) { error = e.what(); }
          catch (...) { error = M("DE_MOTION_FAILED"); }
        if (!*life || revision != queue->revision) return;
        Glib::signal_idle().connect_once([this, life, queue, revision, batchKey, results, error, quick] {
            if (!*life || revision != queue->revision) return;
            pendingMotionKey_.clear();
            motionSpinner_->stop();
            motionSpinner_->hide();
            if (!error.empty()) {
                countLabel_->set_text(M("DE_MOTION_FAILED"));
                countLabel_->set_tooltip_text(error);
                return;
            }
            for (const auto& result : results) motionCache_.push_back(result);
            size_t bytes = 0;
            for (const auto& cached : motionCache_) bytes += cached->pixels.size() * sizeof(rtengine::demotion::Pixel);
            while (bytes > 96U * 1024U * 1024U && motionCache_.size() > results.size()) {
                bytes -= motionCache_.front()->pixels.size() * sizeof(rtengine::demotion::Pixel);
                motionCache_.erase(motionCache_.begin());
            }
            updatePreview(quick);
        });
    };
    queue->wake.notify_one();
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
    trimPreviewCaches();

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

    const auto smallButton = [](const char* icon) {
        Gtk::Button* b = Gtk::manage(new Gtk::Button());
        b->set_image(*Gtk::manage(new RTImage(icon, Gtk::ICON_SIZE_BUTTON)));
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

        if (params_.layers[i].motion.path != rtengine::demotion::Path::OFF) {
            auto badge = Gtk::manage(new Gtk::Label(M("DE_MOTION")));
            badge->get_style_context()->add_class("de-motion-badge");
            badge->set_halign(Gtk::ALIGN_CENTER);
            badge->set_valign(Gtk::ALIGN_START);
            chip->add_overlay(*badge);
            chip->set_overlay_pass_through(*badge, true);
        }

        // Its place in the stack, as the picker's badges number it.
        auto number = Gtk::manage(new Gtk::Label(Glib::ustring::format(i + 1)));
        number->get_style_context()->add_class("de-chip-number");
        number->set_halign(Gtk::ALIGN_START);
        number->set_valign(Gtk::ALIGN_START);
        chip->add_overlay(*number);
        chip->set_overlay_pass_through(*number, true);

        // compact controls over the thumbnail: reorder, develop, remove
        Gtk::Box* controls = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
        controls->set_name("DEChipControls");
        controls->set_halign(Gtk::ALIGN_CENTER);
        controls->set_valign(Gtk::ALIGN_END);

        Gtk::Button* left = smallButton("arrow-left-small");
        left->set_sensitive(i > 0);
        left->signal_clicked().connect([this, idx]() { moveLayer(idx, -1); });
        controls->pack_start(*left, Gtk::PACK_SHRINK);

        Gtk::Button* rightBtn = smallButton("arrow-right-small");
        rightBtn->set_sensitive(i + 1 < params_.layers.size());
        rightBtn->signal_clicked().connect([this, idx]() { moveLayer(idx, 1); });
        controls->pack_start(*rightBtn, Gtk::PACK_SHRINK);

        Gtk::Button* edit = smallButton("edit-small");
        edit->set_tooltip_text(M("TP_DOUBLEEXPOSURE_EDIT"));
        edit->signal_clicked().connect([this, path]() {
            editRequestPath_ = path;
            response(Gtk::RESPONSE_OK);
        });
        controls->pack_start(*edit, Gtk::PACK_SHRINK);

        Gtk::Button* close = smallButton("cancel-small");
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
    const bool haveLayer = selectedLayer_ < params_.layers.size();
    addMotion_->set_sensitive(params_.layers.size() < MAX_LAYERS);
    motionExpander_->set_sensitive(haveLayer);
    const auto motion = haveLayer ? params_.layers[selectedLayer_].motion : rtengine::demotion::Params{};
    motionPath_->set_active(static_cast<int>(motion.path));
    motionShutter_->setValue(motion.shutter);
    motionSpeed_->setValue(motion.speed);
    motionDirection_->setValue(motion.direction);
    motionNoise_->setValue(motion.noise);
    motionComplexity_->setValue(motion.complexity);
    for (auto scale : {motionShutter_, motionSpeed_, motionDirection_, motionNoise_, motionComplexity_})
        scale->set_sensitive(haveLayer && motion.path != rtengine::demotion::Path::OFF);
    motionRandomize_->set_sensitive(haveLayer && motion.path != rtengine::demotion::Path::OFF &&
        (motion.noise > 0 || motion.path == rtengine::demotion::Path::HANDHELD || motion.path == rtengine::demotion::Path::WANDER));

    if (params_.layers.empty() || selectedLayer_ >= params_.layers.size()) {
        layerLabel_->set_text(M("DOUBLEEXPOSURE_NOLAYER"));
        layerDevelop_->set_sensitive(false);
        layerRemove_->set_sensitive(false);

        for (DESection* section : {blendSection_, selectSection_, revealSection_, adjustExpander_, patternExpander_, motionExpander_}) {
            section->setSummary("");
        }

        layerEvScale_->set_sensitive(false);
        layerOpacityScale_->set_sensitive(false);
        gateStrengthScale_->set_sensitive(false);
        softnessScale_->set_sensitive(false);
        offsetXScale_->set_sensitive(false);
        offsetYScale_->set_sensitive(false);
        scaleScale_->set_sensitive(false);
        rotateScale_->set_sensitive(false);
        rotateLeft_->set_sensitive(false);
        rotateRight_->set_sensitive(false);
        flipH_->set_sensitive(false);
        patternMethod_->set_sensitive(false);
        patternSpacingScale_->set_sensitive(false);
        patternStaggerScale_->set_sensitive(false);
        patternCountScale_->set_sensitive(false);
        patternDiameterScale_->set_sensitive(false);
        edgeFeatherScale_->set_sensitive(false);
        subjectMethod_->set_sensitive(false);
        subjectInvert_->set_sensitive(false);
        subjectCrop_->set_sensitive(false);
        subjectEdit_->set_sensitive(false);
        subjectFeatherScale_->set_sensitive(false);
        subjectPick_->set_sensitive(false);
        asEdited_->set_sensitive(false);
        resetPlacement_->set_sensitive(false);
        blendMethod_->set_sensitive(false);
        syncDepthControls();
    } else {
        const auto& layer = params_.layers[selectedLayer_];
        layerLabel_->set_text(Glib::ustring::compose("%1 \xC2\xB7 %2", selectedLayer_ + 1,
                              Glib::path_get_basename(layer.path)));
        layerDevelop_->set_sensitive(true);
        layerRemove_->set_sensitive(true);
        const bool comparative = layer.blendMode == DoubleExposureParams::BlendMode::LIGHTEN
                                 || layer.blendMode == DoubleExposureParams::BlendMode::DARKEN
                                 || layer.blendMode == DoubleExposureParams::BlendMode::ADAPTIVE;
        layerEvScale_->set_sensitive(true);
        layerOpacityScale_->set_sensitive(true);
        gateStrengthScale_->set_sensitive(true);
        softnessScale_->set_sensitive(layer.blendMode == DoubleExposureParams::BlendMode::ADAPTIVE ||
            (comparative && layer.compare == DoubleExposureParams::Compare::LUMINANCE));
        blendMethod_->set_sensitive(true);
        layerEvScale_->setValue(layer.ev);
        layerOpacityScale_->setValue(layer.opacity);
        gateStrengthScale_->setValue(layer.gateStrength);
        softnessScale_->setValue(layer.softness);
        offsetXScale_->set_sensitive(true);
        offsetYScale_->set_sensitive(true);
        scaleScale_->set_sensitive(true);
        resetPlacement_->set_sensitive(true);
        offsetXScale_->setValue(layer.offsetX);
        offsetYScale_->setValue(layer.offsetY);
        scaleScale_->setValue(layer.scale);

        // Spacing and stagger belong to the grid patterns, the ring controls
        // to the radial one; only one pair is ever on show.
        const bool tiled = layer.pattern != DoubleExposureParams::Pattern::OFF;
        rotateScale_->set_sensitive(true);
        rotateLeft_->set_sensitive(true);
        rotateRight_->set_sensitive(true);
        flipH_->set_sensitive(true);
        patternMethod_->set_sensitive(true);
        showPatternRows(layer.pattern, layer.maskClass != DoubleExposureParams::MaskClass::OFF);
        rotateScale_->setValue(layer.rotate);
        flipH_->set_active(layer.flipH);
        patternMethod_->set_active(static_cast<int>(layer.pattern));
        patternSpacingScale_->setValue(layer.patternSpacing);
        patternStaggerScale_->setValue(layer.patternStagger);
        patternCountScale_->setValue(layer.patternCount);
        patternDiameterScale_->setValue(layer.patternDiameter);
        patternUpright_->set_active(layer.patternUpright);
        patternTwistScale_->setValue(layer.patternTwist);
        edgeFeatherScale_->set_sensitive(true);
        edgeFeatherScale_->setValue(layer.edgeFeather);

        // A patterned exposure opens its own section: settings that are doing
        // something should not be hidden behind a closed header.
        if (tiled) {
            patternExpander_->set_expanded(true);
        }

        const bool masked = layer.maskClass != DoubleExposureParams::MaskClass::OFF;
#ifdef RT_AI_MASKING
        refreshSubjectLabels(layer.path);
#endif
        subjectMethod_->set_sensitive(true);
        subjectInvert_->set_sensitive(masked);
        subjectCrop_->set_sensitive(masked);
        subjectEdit_->set_sensitive(masked);
        subjectFeatherScale_->set_sensitive(masked);
#ifndef RT_AI_MASKING
        // With masking built in, refreshSubjectLabels above rebuilt the list
        // and selected this layer's class within it; the row number here is
        // no longer the class number.
        subjectMethod_->set_active(static_cast<int>(layer.maskClass));
#endif
        subjectInvert_->set_active(layer.maskInvert);
        subjectCrop_->set_active(layer.cropToSubject);
        subjectFeatherScale_->setValue(layer.maskFeather);
        blendMethod_->set_active(static_cast<int>(layer.blendMode));
        asEdited_->set_sensitive(true);
        asEdited_->set_active(layer.asEdited);

        // What each closed section is doing for this layer.
        blendSection_->setSummary(blendMethod_->get_active_text());
        selectSection_->setSummary(masked ? subjectMethod_->get_active_text()
                                   : (layer.depthEnabled ? M("TP_DOUBLEEXPOSURE_DEPTH") : M("TP_DOUBLEEXPOSURE_SUM_WHOLE")));
        revealSection_->setSummary(layer.gateStrength <= 0
                                   ? M("TP_DOUBLEEXPOSURE_SUM_OFF")
                                   : Glib::ustring::format(static_cast<int>(std::lround(layer.gateStrength))));
        const bool placed = layer.offsetX != 0 || layer.offsetY != 0 || layer.scale != 100 || layer.rotate != 0 || layer.flipH;
        adjustExpander_->setSummary(placed
                                    ? Glib::ustring::compose("%1%%", static_cast<int>(std::lround(layer.scale)))
                                    : M("TP_DOUBLEEXPOSURE_SUM_FIT"));
        patternExpander_->setSummary(tiled ? patternMethod_->get_active_text() : M("TP_DOUBLEEXPOSURE_SUM_OFF"));
        motionExpander_->setSummary(layer.motion.path == rtengine::demotion::Path::OFF
                                    ? M("TP_DOUBLEEXPOSURE_SUM_OFF") : motionPath_->get_active_text());
        subjectPick_->set_sensitive(layer.maskClass == DoubleExposureParams::MaskClass::POINTS);

        if (depthExpander_) {
            depthEnable_->set_active(layer.depthEnabled);
            depthInvert_->set_active(layer.depthInvert);
            depthNearShape_->setValue(layer.depthNearShape);
            depthFarShape_->setValue(layer.depthFarShape);
            depthRange_->values = layer.depthRange;
            depthRange_->nearShape = layer.depthNearShape;
            depthRange_->farShape = layer.depthFarShape;

            // A confined exposure opens its own section, as a patterned one does.
            if (layer.depthEnabled) {
                depthExpander_->set_expanded(true);
            }

            syncDepthControls();
            requestPartnerDepth();
        }
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

    params_.layers[selectedLayer_].ev = layerEvScale_->getValue();
    params_.layers[selectedLayer_].opacity = layerOpacityScale_->getValue();
    params_.layers[selectedLayer_].gateStrength = gateStrengthScale_->getValue();
    params_.layers[selectedLayer_].softness = softnessScale_->getValue();
    params_.layers[selectedLayer_].offsetX = offsetXScale_->getValue();
    params_.layers[selectedLayer_].offsetY = offsetYScale_->getValue();
    params_.layers[selectedLayer_].scale = scaleScale_->getValue();
    params_.layers[selectedLayer_].rotate = rotateScale_->getValue();
    params_.layers[selectedLayer_].flipH = flipH_->get_active();
    const int patternRow = patternMethod_->get_active_row_number();
    params_.layers[selectedLayer_].pattern =
        static_cast<DoubleExposureParams::Pattern>(patternRow < 0 ? 0 : patternRow);
    params_.layers[selectedLayer_].patternSpacing = patternSpacingScale_->getValue();
    params_.layers[selectedLayer_].patternStagger = patternStaggerScale_->getValue();
    params_.layers[selectedLayer_].patternCount = patternCountScale_->getValue();
    params_.layers[selectedLayer_].patternDiameter = patternDiameterScale_->getValue();
    params_.layers[selectedLayer_].patternUpright = patternUpright_->get_active();
    params_.layers[selectedLayer_].patternTwist = patternTwistScale_->getValue();
    params_.layers[selectedLayer_].edgeFeather = edgeFeatherScale_->getValue();
    // Back through the map: absent classes are left out of the list, so the
    // row the user picked is not the class it names.
    const int subjectRow = subjectMethod_->get_active_row_number();
    const int subjectClassRow = subjectRow >= 0 && subjectRow < static_cast<int>(subjectRows_.size())
                                ? subjectRows_[subjectRow] : 0;
    params_.layers[selectedLayer_].maskClass =
        static_cast<DoubleExposureParams::MaskClass>(subjectClassRow);
    params_.layers[selectedLayer_].maskInvert = subjectInvert_->get_active();
    params_.layers[selectedLayer_].cropToSubject = subjectCrop_->get_active();
    params_.layers[selectedLayer_].maskFeather = subjectFeatherScale_->getValue();

    if (asEdited_ && params_.layers[selectedLayer_].asEdited != asEdited_->get_active()) {
        // A different source: its plates, masks and depth all have to come again.
        params_.layers[selectedLayer_].asEdited = asEdited_->get_active();
        requestPreviewThumbs();
    }

    if (subjectPick_) {
        const bool points = params_.layers[selectedLayer_].maskClass == DoubleExposureParams::MaskClass::POINTS;
        subjectPick_->set_sensitive(points);

        if (!points && subjectPick_->get_active()) {
            subjectPick_->set_active(false);
        }
    }

    if (depthExpander_) {
        params_.layers[selectedLayer_].depthEnabled = depthEnable_->get_active();
        params_.layers[selectedLayer_].depthInvert = depthInvert_->get_active();
        params_.layers[selectedLayer_].depthNearShape = depthNearShape_->getValue();
        params_.layers[selectedLayer_].depthFarShape = depthFarShape_->getValue();
        params_.layers[selectedLayer_].depthRange = depthRange_->values;
        depthRange_->nearShape = params_.layers[selectedLayer_].depthNearShape;
        depthRange_->farShape = params_.layers[selectedLayer_].depthFarShape;
    }

    showPatternRows(params_.layers[selectedLayer_].pattern,
                    params_.layers[selectedLayer_].maskClass != DoubleExposureParams::MaskClass::OFF);
    const bool masked = params_.layers[selectedLayer_].maskClass != DoubleExposureParams::MaskClass::OFF;
    subjectInvert_->set_sensitive(masked);
    subjectCrop_->set_sensitive(masked);
    subjectEdit_->set_sensitive(masked);
    subjectFeatherScale_->set_sensitive(masked);

    requestPartnerMasks();
    syncDepthControls();
    requestPartnerDepth();
    schedulePreviewUpdate();
}

// --- interactive placement from the preview ---

// Only the chosen pattern's own controls are on show, and the selection
// feather only when a selection is being used.
void DoubleExposureDlg::showPatternRows(DoubleExposureParams::Pattern pattern, bool masked)
{
    const bool grid = pattern == DoubleExposureParams::Pattern::REPEAT
                      || pattern == DoubleExposureParams::Pattern::MIRROR;
    const bool radial = pattern == DoubleExposureParams::Pattern::RADIAL;
    const bool feather = masked && haveSegmentation_;

    gridRow_->set_visible(grid);
    ringCountCell_->set_visible(radial);
    ringDiameterCell_->set_visible(radial);
    twistRow_->set_visible(grid || radial);
    subjectFeatherCell_->set_visible(feather);
    radialRow_->set_visible(radial || feather);
    patternSpacingScale_->set_sensitive(grid);
    patternStaggerScale_->set_sensitive(grid);
    patternCountScale_->set_sensitive(radial);
    patternDiameterScale_->set_sensitive(radial);
    patternTwistScale_->set_sensitive(grid || radial);
    patternUpright_->set_visible(radial);
}

void DoubleExposureDlg::syncPlacementControls()
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    const auto& layer = params_.layers[selectedLayer_];
    syncingControls_ = true;
    offsetXScale_->setValue(layer.offsetX);
    offsetYScale_->setValue(layer.offsetY);
    scaleScale_->setValue(layer.scale);
    rotateScale_->setValue(layer.rotate);
    syncingControls_ = false;
}

// Motion events arrive faster than the composite can be rebuilt at the
// high-res tier; coalesce them into one recomposite per idle.
void DoubleExposureDlg::schedulePreviewUpdate()
{
    auto alive = alive_;
    ++previewQueue_->revision;
    set_response_sensitive(Gtk::RESPONSE_OK, false);

    if (!previewUpdatePending_) {
        previewUpdatePending_ = true;

        Glib::signal_idle().connect_once([this, alive]() {
            if (!*alive) {
                return;
            }

            previewUpdatePending_ = false;
            updatePreview(true);
        });
    }

    // Full-quality pass once the control has been still for a moment.
    previewSettle_.disconnect();
    previewSettle_ = Glib::signal_timeout().connect([this, alive]() {
        if (*alive) {
            updatePreview(false);
        }

        return false;
    }, 220);
}

void DoubleExposureDlg::onPreviewMove(double dx, double dy)
{
    if (selectedLayer_ >= params_.layers.size() || previewNxs_ <= 0.0 || previewNys_ <= 0.0) {
        return;
    }

    // composite pixels -> percent of the base's full frame
    auto& layer = params_.layers[selectedLayer_];
    layer.offsetX = std::min(std::max(layer.offsetX + dx * previewNxs_ * 100.0, -150.0), 150.0);
    layer.offsetY = std::min(std::max(layer.offsetY + dy * previewNys_ * 100.0, -150.0), 150.0);
    syncPlacementControls();
    schedulePreviewUpdate();
}

// A click on the preview, carried back through the selected exposure's
// placement into its partner's own frame, becomes an include or exclude
// prompt among the layer's strokes; the mask is then selected again.
void DoubleExposureDlg::onPreviewPick(double cx, double cy, bool include)
{
    if (selectedLayer_ >= params_.layers.size() || !previewSelValid_ || previewNxs_ <= 0.0 || previewNys_ <= 0.0) {
        return;
    }

    auto& layer = params_.layers[selectedLayer_];

    if (layer.maskClass != DoubleExposureParams::MaskClass::POINTS) {
        return;
    }

    const float nx = static_cast<float>(previewNx0_ + cx * previewNxs_);
    const float ny = static_cast<float>(previewNy0_ + cy * previewNys_);
    const auto hit = rtengine::deplace::place(previewSelFrame_, nx, ny / std::max(previewBaseAspect_, 1e-6f),
                                              static_cast<float>(previewNxs_));

    if (!hit.present) {
        return;
    }

    rtengine::MaskStroke prompt;
    prompt.kind = rtengine::MaskStroke::Kind::POINT;
    prompt.add = include;
    prompt.radius = 0.0;
    prompt.points.push_back({std::min(std::max(static_cast<double>(hit.u), 0.0), 1.0),
                             std::min(std::max(static_cast<double>(hit.v * previewSelAspect_), 0.0), 1.0)});
    layer.maskPaint.strokes.push_back(prompt);
    requestPartnerMasks();
    schedulePreviewUpdate();
}

void DoubleExposureDlg::syncPickMode()
{
    if (!subjectPick_ || !preview_) {
        return;
    }

    preview_->setPickMode(subjectPick_->get_active());
}

void DoubleExposureDlg::onPreviewScale(double factor)
{
    if (selectedLayer_ >= params_.layers.size() || factor <= 0.0) {
        return;
    }

    auto& layer = params_.layers[selectedLayer_];
    layer.scale = std::min(std::max(layer.scale * factor, 10.0), 400.0);
    syncPlacementControls();
    schedulePreviewUpdate();
}

// One row's tile. The partner was segmented for its coverage; the tiles came
// out of the same pass, so this costs a lookup.
void DoubleExposureDlg::renderSubjectTile(const Gtk::TreeModel::const_iterator& iter)
{
#ifdef RT_AI_MASKING
    subjectTileCell_.property_pixbuf() = Glib::RefPtr<Gdk::Pixbuf>();

    if (!iter || selectedLayer_ >= params_.layers.size()) {
        return;
    }

    const auto model = subjectMethod_->get_model();

    if (!model) {
        return;
    }

    const Gtk::TreeModel::Path path = model->get_path(iter);

    if (path.empty()) {
        return;
    }

    const int row = path[0] < static_cast<int>(subjectRows_.size())
                    ? subjectRows_[path[0]] : -1;
    const int cls = subjectRowClass(row);

    if (cls < 0) {
        return;
    }

    const auto view = rtengine::PartnerMaskStore::getInstance().getClassView(
                          params_.layers[selectedLayer_].path, workingProfile_);

    if (view) {
        subjectTileCell_.property_pixbuf() =
            aimaskthumb::fromPartnerView(*view, cls, 0.3f, 30, 22);
    }

#endif
}

// The class list shows where each class lands in this partner, once it has
// been segmented; before that it is plain names.
void DoubleExposureDlg::refreshSubjectLabels(const Glib::ustring& path)
{
#ifdef RT_AI_MASKING
    requestCoverage(path);

    const auto view = rtengine::PartnerMaskStore::getInstance().getClassView(path, workingProfile_);
    const int chosen = selectedLayer_ < params_.layers.size()
                       ? static_cast<int>(params_.layers[selectedLayer_].maskClass) : 0;

    subjectMethod_->block(true);
    subjectMethod_->remove_all();
    subjectRows_.clear();

    for (int row = 0; row <= 8; ++row) {
        // "The whole frame" always stands, and so does whatever this layer is
        // already set to. Otherwise a class is offered only while it might be
        // there: unmeasured, or measured and actually present. A partner with
        // no animal in it should not offer to cut one out. Click-to-select
        // stands whenever its model is installed.
        bool offer = row == 0 || row == chosen
                     || (row == 8 ? rtengine::SamSelector::getInstance().available() : !view);

        if (!offer && row != 8) {
            const int cls = subjectRowClass(row);
            offer = cls >= 0 && cls < static_cast<int>(view->coverage.size())
                    && view->coverage[cls] > 0.f;
        }

        if (offer) {
            subjectRows_.push_back(row);
            subjectMethod_->append(subjectEntryLabel(row));
        }
    }

    const auto at = std::find(subjectRows_.begin(), subjectRows_.end(), chosen);
    subjectMethod_->set_active(at == subjectRows_.end()
                               ? 0 : static_cast<int>(at - subjectRows_.begin()));
    subjectMethod_->block(false);
#endif
}

void DoubleExposureDlg::openMaskEditor()
{
#ifdef RT_AI_MASKING
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    auto& layer = params_.layers[selectedLayer_];

    if (layer.maskClass == DoubleExposureParams::MaskClass::OFF) {
        return;
    }

    // Computed here rather than waited for: the user has just asked to see
    // the selection, so paying for it on the spot is the least surprising
    // thing to do, and it warms the cache the preview reads.
    // The click prompts stay with the strokes: the automatic mask shown
    // underneath is the one they select, and they ride along unchanged.
    rtengine::MaskPaint promptsOnly;

    for (const auto& stroke : layer.maskPaint.strokes) {
        if (stroke.kind == rtengine::MaskStroke::Kind::POINT) {
            promptsOnly.strokes.push_back(stroke);
        }
    }

    auto mask = rtengine::PartnerMaskStore::getInstance().getMask(
                    layer.source(), workingProfile_, layer.maskClass, layer.maskFeather,
                    layer.maskInvert, promptsOnly, true);

    rtengine::MaskPaint paint = layer.maskPaint;

    if (!maskpaint::refine(this, layer.path, maskpaint::fromPartner(mask), paint)) {
        return;
    }

    layer.maskPaint = paint;
    requestPartnerMasks();
    schedulePreviewUpdate();
#endif
}

// Depth maps are estimated once per partner on a worker; the preview reads
// the store cache-only, exactly as it does for segmentation masks, so a
// redraw never waits on the model. The histogram beneath the range handles
// is measured at the same time from the partner's own scene plate.
void DoubleExposureDlg::requestPartnerDepth()
{
#ifdef RT_AI_MASKING
    auto& store = rtengine::PartnerDepthStore::getInstance();

    if (!store.available()) {
        return;
    }

    for (const auto& layer : params_.layers) {
        if (!layer.enabled || !layer.depthEnabled || layer.path.empty()) {
            continue;
        }

        if (store.peekDepth(layer.source(), workingProfile_) || depthPending_.count(layer.source())) {
            continue;
        }

        depthPending_.insert(layer.source());
        auto alive = alive_;
        const Glib::ustring path = layer.source();
        const Glib::ustring profile = workingProfile_;

        detachQuietly(std::thread([this, alive, path, profile]() {
#ifdef _OPENMP
            omp_set_num_threads(2);
#endif
            auto depth = rtengine::PartnerDepthStore::getInstance().getDepth(path, profile);
            compositeui::DepthHistogram histogram;

            if (depth && depth->valid()) {
                auto partner = rtengine::PartnerImageStore::getInstance().getPartner(path, profile, false);
                const rtengine::Imagefloat* image = partner && partner->image ? partner->image.get() : nullptr;

                for (int y = 0; y < depth->height; ++y) {
                    for (int x = 0; x < depth->width; ++x) {
                        float luminance = 0.f;

                        if (image && image->getWidth() > 0 && image->getHeight() > 0) {
                            const int ix = std::min(image->getWidth() - 1, x * image->getWidth() / depth->width);
                            const int iy = std::min(image->getHeight() - 1, y * image->getHeight() / depth->height);
                            luminance = (0.2126f * image->r(iy, ix) + 0.7152f * image->g(iy, ix)
                                         + 0.0722f * image->b(iy, ix)) / 65535.f;
                        }

                        histogram.add(depth->depth[y][x], luminance);
                    }
                }
            }

            Glib::signal_idle().connect_once([this, alive, path, histogram]() {
                if (!*alive) {
                    return;
                }

                depthPending_.erase(path);
                depthHistograms_[path] = histogram;
                syncDepthControls();
                schedulePreviewUpdate();
            });
        }));
    }
#endif
}

void DoubleExposureDlg::syncDepthControls()
{
    if (!depthExpander_) {
        return;
    }

    const bool haveLayer = selectedLayer_ < params_.layers.size();
    const DoubleExposureParams::Layer* layer = haveLayer ? &params_.layers[selectedLayer_] : nullptr;
    const bool on = layer && layer->depthEnabled;
    depthEnable_->set_sensitive(haveLayer);
    depthInvert_->set_sensitive(on);
    depthNearShape_->set_sensitive(on);
    depthFarShape_->set_sensitive(on);
    depthRange_->set_sensitive(on);
    depthRange_->inverted = on && layer->depthInvert;
    depthRange_->histogram = {};
    Glib::ustring status;

#ifdef RT_AI_MASKING
    if (on) {
        auto& store = rtengine::PartnerDepthStore::getInstance();
        const auto found = depthHistograms_.find(layer->source());

        if (found != depthHistograms_.end()) {
            depthRange_->histogram = found->second;
        }

        if (!store.available()) {
            status = M("TP_DOUBLEEXPOSURE_DEPTH_UNAVAILABLE");
        } else if (!store.peekDepth(layer->source(), workingProfile_)) {
            status = M("TP_DOUBLEEXPOSURE_DEPTH_WORKING");
        }
    }
#endif

    depthStatus_->set_text(status);
    depthRange_->queue_draw();
}

void DoubleExposureDlg::depthRangeChanged()
{
    if (syncingControls_ || !depthRange_ || selectedLayer_ >= params_.layers.size()) {
        return;
    }

    params_.layers[selectedLayer_].depthRange = depthRange_->values;
    schedulePreviewUpdate();
}

void DoubleExposureDlg::onPreviewRotate(double degrees)
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    params_.layers[selectedLayer_].rotate = std::min(std::max(degrees, -180.0), 180.0);
    syncPlacementControls();
    schedulePreviewUpdate();
}

// Quarter turns, wrapping the long way round rather than stopping at the end
// of the slider.
void DoubleExposureDlg::nudgeRotation(double degrees)
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    double a = params_.layers[selectedLayer_].rotate + degrees;

    while (a > 180.0) {
        a -= 360.0;
    }

    while (a < -180.0) {
        a += 360.0;
    }

    params_.layers[selectedLayer_].rotate = a;
    syncPlacementControls();
    schedulePreviewUpdate();
}

void DoubleExposureDlg::onPreviewReset()
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    auto& layer = params_.layers[selectedLayer_];
    layer.offsetX = 0.0;
    layer.offsetY = 0.0;
    layer.scale = 100.0;
    layer.rotate = 0.0;
    syncPlacementControls();
    schedulePreviewUpdate();
}

void DoubleExposureDlg::blendControlChanged()
{
    if (syncingControls_) {
        return;
    }

    if (selectedLayer_ < params_.layers.size()) {
        const int blendRow = blendMethod_->get_active_row_number();
        auto& layer = params_.layers[selectedLayer_];
        layer.blendMode = static_cast<DoubleExposureParams::BlendMode>(blendRow < 0 ? 0 : blendRow);
        const bool comparative = layer.blendMode == DoubleExposureParams::BlendMode::LIGHTEN
                                 || layer.blendMode == DoubleExposureParams::BlendMode::DARKEN
                                 || layer.blendMode == DoubleExposureParams::BlendMode::ADAPTIVE;
        softnessScale_->set_sensitive(layer.blendMode == DoubleExposureParams::BlendMode::ADAPTIVE ||
            (comparative && layer.compare == DoubleExposureParams::Compare::LUMINANCE));
    }

    params_.autoGain = autoGain_->get_active();
    params_.baseEv = baseEvScale_->getValue();
    params_.highlightLatitude = latitudeScale_->getValue();

    bool anyAdd = false;

    for (const auto& layer : params_.layers) {
        if (layer.enabled && layer.blendMode == DoubleExposureParams::BlendMode::ADD) {
            anyAdd = true;
            break;
        }
    }

    autoGain_->set_sensitive(anyAdd);

    schedulePreviewUpdate();
}

void DoubleExposureDlg::updatePreview(bool quick)
{
    const auto queue = previewQueue_;
    const unsigned revision = ++queue->revision;
    set_response_sensitive(Gtk::RESPONSE_OK, false);
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

    const auto findPlate = [this, wantHi](const Glib::ustring& path) -> std::shared_ptr<const DEScenePlate> {
        if (wantHi) {
            const auto hi = sceneHiPix_.find(path);

            if (hi != sceneHiPix_.end() && hi->second) {
                return hi->second;
            }
        }

        const auto it = scenePix_.find(path);
        return it != scenePix_.end() ? it->second : std::shared_ptr<const DEScenePlate>();
    };

    const Glib::RefPtr<Gdk::Pixbuf> base = findIn(hiPix_, bigPix_, baseImagePath_);
    preview_->setBase(base);

    if (!base) {
        preview_->setSelectionFrame(false, 0, 0, 0, 0);
        preview_->setComposite({});
        return;
    }

    const int w = base->get_width();
    const int h = base->get_height();

    if (w <= 0 || h <= 0) {
        preview_->setSelectionFrame(false, 0, 0, 0, 0);
        preview_->setComposite({});
        return;
    }

    // The scene reference the composite runs on. Preferred: the engine's own
    // decode of the base (scene plate). Until it arrives: the neutral
    // thumbnail render; failing that, the styled base itself.
    const std::shared_ptr<const DEScenePlate> basePlate = findPlate(baseImagePath_);
    const Glib::RefPtr<Gdk::Pixbuf> neutralBase = findIn(neutralHiPix_, neutralPix_, baseImagePath_);
    const DEScenePlate* nbPlate = basePlate ? basePlate.get() : nullptr;

    float srgbLut[256];

    for (int i = 0; i < 256; ++i) {
        srgbLut[i] = srgbToLin(i / 255.f);
    }

    std::array<std::array<double, 3>, 3> matrix;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) matrix[i][j] = workingToSrgb_[i][j];
    const auto workingToSrgb = [matrix](float& r, float& g, float& b) {
        const double rr = matrix[0][0] * r + matrix[0][1] * g + matrix[0][2] * b;
        const double gg = matrix[1][0] * r + matrix[1][1] * g + matrix[1][2] * b;
        const double bb = matrix[2][0] * r + matrix[2][1] * g + matrix[2][2] * b;
        r = static_cast<float>(std::max(rr, 0.0));
        g = static_cast<float>(std::max(gg, 0.0));
        b = static_cast<float>(std::max(bb, 0.0));
    };

    // Neutral base geometry (plate or thumb), needed by the cache rebuild and
    // by the composite.
    const guint8* nbData = nullptr;
    int nbStride = 0, nbCh = 3, nbW = 0, nbH = 0;

    if (nbPlate) {
        nbW = nbPlate->w;
        nbH = nbPlate->h;
    } else if (neutralBase) {
        nbData = neutralBase->get_pixels();
        nbStride = neutralBase->get_rowstride();
        nbCh = neutralBase->get_n_channels();
        nbW = neutralBase->get_width();
        nbH = neutralBase->get_height();
    }

    // Neutral base sample at plate/thumb pixel (nbx, nby) in sRGB-linear.
    const auto sampleNeutralSrgb = [&](int nbx, int nby, float& r, float& g, float& b) {
        if (nbPlate) {
            const size_t o = (static_cast<size_t>(nby) * nbW + nbx) * 3;
            r = nbPlate->rgb[o];
            g = nbPlate->rgb[o + 1];
            b = nbPlate->rgb[o + 2];
            workingToSrgb(r, g, b);
        } else {
            const guint8* np = nbData + nby * nbStride + nbx * nbCh;
            r = srgbLut[np[0]];
            g = srgbLut[np[1]];
            b = srgbLut[np[2]];
        }
    };

    // --- base-only work, cached across layer edits ---------------------
    if (previewCache_->styledRef != base || previewCache_->neutralRef != neutralBase ||
        previewCache_->plateRef != basePlate || previewCache_->wantHi != wantHi ||
        previewCache_->w != w || previewCache_->h != h) {
        previewCache_ = std::make_shared<PreviewCache>();
    }
    PreviewCache& pc = *previewCache_;

    if (pc.styledRef != base || pc.neutralRef != neutralBase || pc.plateRef != basePlate
            || pc.wantHi != wantHi || pc.w != w || pc.h != h) {
        pc.styledRef = base;
        pc.neutralRef = neutralBase;
        pc.plateRef = basePlate;
        pc.wantHi = wantHi;
        pc.w = w;
        pc.h = h;

        // linearize the styled base (the user's edit — the display reference)
        pc.lin.assign(static_cast<size_t>(w) * h * 3, 0.f);
        {
            const guint8* baseData = base->get_pixels();
            const int baseStride = base->get_rowstride();
            const int baseChannels = base->get_n_channels();

            for (int y = 0; y < h; ++y) {
                const guint8* row = baseData + y * baseStride;

                for (int x = 0; x < w; ++x) {
                    const size_t o = (static_cast<size_t>(y) * w + x) * 3;
                    pc.lin[o] = srgbLut[row[x * baseChannels]];
                    pc.lin[o + 1] = srgbLut[row[x * baseChannels + 1]];
                    pc.lin[o + 2] = srgbLut[row[x * baseChannels + 2]];
                }
            }
        }

        // geometry: replicate the engine's mapping. The engine composites on
        // the base's FULL (uncropped, coarse-rotated) frame and cover-fits the
        // partner's full frame over it; the preview shows the base's crop
        // window into that frame. In normalized full-frame coordinates the
        // map reduces to the two full-frame aspects plus the crop rectangle.
        pc.nx0 = 0.f;
        pc.nxs = 1.f / w;
        pc.ny0 = 0.f;
        pc.nys = 1.f / h;
        pc.baseAspect = static_cast<float>(w) / h;

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
                pc.baseAspect = static_cast<float>(fullW) / fullH;

                if (bpp.crop.enabled && bpp.crop.w > 0 && bpp.crop.h > 0) {
                    pc.nx0 = static_cast<float>(bpp.crop.x) / fullW;
                    pc.nxs = (static_cast<float>(bpp.crop.w) / w) / fullW;
                    pc.ny0 = static_cast<float>(bpp.crop.y) / fullH;
                    pc.nys = (static_cast<float>(bpp.crop.h) / h) / fullH;
                }
            }

            bthm->decreaseRef();
        }

        // Look transfer, measured from (neutral -> styled) pixel pairs of the
        // base plate: ONE monotone luminance curve over perceptual bins, read
        // back with linear interpolation, plus one saturation factor. Both
        // sides in sRGB-linear.
        constexpr int TBINS = 256;
        pc.haveTransfer = nbW > 0 && nbH > 0;
        pc.satFactor = 1.f;

        if (pc.haveTransfer) {
            double sumS[TBINS] = {0.0};
            double cnt[TBINS] = {0.0};
            double chromaS = 0.0, chromaN = 0.0;

            for (int y = 0; y < h; ++y) {
                const float ny = pc.ny0 + (y + 0.5f) * pc.nys;
                const int nby = std::max(0, std::min(static_cast<int>(ny * nbH), nbH - 1));

                for (int x = 0; x < w; ++x) {
                    const float nx = pc.nx0 + (x + 0.5f) * pc.nxs;
                    const int nbx = std::max(0, std::min(static_cast<int>(nx * nbW), nbW - 1));
                    const size_t o = (static_cast<size_t>(y) * w + x) * 3;

                    float nr, ng, nb;
                    sampleNeutralSrgb(nbx, nby, nr, ng, nb);
                    const float sr = pc.lin[o], sg = pc.lin[o + 1], sb = pc.lin[o + 2];
                    const float yn = rtengine::deblend::lum709(nr, ng, nb);
                    const float ys = rtengine::deblend::lum709(sr, sg, sb);
                    const int bin = std::min(TBINS - 1, static_cast<int>(rtengine::deblend::gateEncode(yn) * TBINS));
                    sumS[bin] += ys;
                    cnt[bin] += 1.0;
                    chromaS += std::fabs(sr - ys) + std::fabs(sg - ys) + std::fabs(sb - ys);
                    chromaN += std::fabs(nr - yn) + std::fabs(ng - yn) + std::fabs(nb - yn);
                }
            }

            float* toneCurve = pc.toneCurve;
            int firstMeasured = -1, lastMeasured = -1;

            for (int bin = 0; bin < TBINS; ++bin) {
                if (cnt[bin] > 8.0) {
                    toneCurve[bin] = static_cast<float>(sumS[bin] / cnt[bin]);

                    if (firstMeasured < 0) {
                        firstMeasured = bin;
                    }

                    lastMeasured = bin;
                } else {
                    toneCurve[bin] = -1.f;
                }
            }

            if (firstMeasured < 0) {
                for (int bin = 0; bin < TBINS; ++bin) {
                    const float enc = (bin + 0.5f) / TBINS;
                    toneCurve[bin] = enc <= 0.04045f ? enc / 12.92f : std::pow((enc + 0.055f) / 1.055f, 2.4f);
                }
            } else {
                for (int bin = 0; bin < firstMeasured; ++bin) {
                    toneCurve[bin] = toneCurve[firstMeasured];
                }

                for (int bin = lastMeasured + 1; bin < TBINS; ++bin) {
                    toneCurve[bin] = toneCurve[lastMeasured];
                }

                int prev = firstMeasured;

                for (int bin = firstMeasured + 1; bin <= lastMeasured; ++bin) {
                    if (toneCurve[bin] < 0.f) {
                        continue;
                    }

                    for (int k = prev + 1; k < bin; ++k) {
                        const float t = static_cast<float>(k - prev) / (bin - prev);
                        toneCurve[k] = toneCurve[prev] * (1.f - t) + toneCurve[bin] * t;
                    }

                    prev = bin;
                }

                for (int bin = 1; bin < TBINS; ++bin) {
                    toneCurve[bin] = std::max(toneCurve[bin], toneCurve[bin - 1]);
                }
            }

            pc.satFactor = chromaN > 1e-9 ? static_cast<float>(std::min(2.0, chromaS / chromaN)) : 1.f;
        }
    }

    const float nx0 = pc.nx0, nxs = pc.nxs, ny0 = pc.ny0, nys = pc.nys;
    const float baseAspect = pc.baseAspect;

    // --- layers -----------------------------------------------------------
    struct LayerPix {
        std::shared_ptr<const DEScenePlate> plate; // engine decode (preferred)
        std::shared_ptr<const DEMotionPlate> motion;
        Glib::RefPtr<Gdk::Pixbuf> pix;             // neutral thumb (fallback)
        int srcW = 0;
        int srcH = 0;
        float gain = 1.f;
        float opacity = 1.f;
        DoubleExposureParams::BlendMode mode = DoubleExposureParams::BlendMode::ADD;
        DoubleExposureParams::Compare compare = DoubleExposureParams::Compare::LUMINANCE;
        float softness = 0.f;
        bool gateOnLayer = false;
        float gateLow = 0.f;
        float gateHigh = 0.f;
        float gateFeather = 0.f;
        float gateStrength = 0.f;
        // Cover fit, placement, rotation and tiling. Built in units where the
        // base frame is one wide, so deplace::map does exactly what it does
        // for the engine and the two renders cannot disagree.
        rtengine::deplace::Frame frame;
        float srcAspect = 1.f;
#ifdef RT_AI_MASKING
        std::shared_ptr<const rtengine::PartnerMask> mask;
        std::shared_ptr<const rtengine::PartnerDepth> depth;
        rtengine::DepthSelection depthSelect;
#endif
    };

    std::vector<LayerPix> layerPix;
    previewSelValid_ = false;
    int addLayers = 0;
    bool allPlates = true;
    std::vector<DEMotionRequest> motionRequests;
    bool waitingMotionSource = false;

    for (const auto& layer : params_.layers) {
        if (!layer.enabled) {
            continue;
        }

        LayerPix lp;
        lp.plate = findPlate(layer.source());
        lp.pix = findIn(neutralHiPix_, neutralPix_, layer.path);

        if (!lp.plate && !lp.pix) {
            continue;
        }

        if (!lp.plate) {
            allPlates = false;
        }

        lp.srcW = lp.plate ? lp.plate->w : lp.pix->get_width();
        lp.srcH = lp.plate ? lp.plate->h : lp.pix->get_height();
        lp.gain = static_cast<float>(std::pow(2.0, layer.ev));
        lp.opacity = static_cast<float>(layer.opacity) / 100.f;
        lp.mode = layer.blendMode;
        lp.compare = layer.compare;
        lp.softness = std::max(static_cast<float>(layer.softness), 0.f);
        lp.gateOnLayer = layer.gateSource == DoubleExposureParams::GateSource::LAYER;
        lp.gateLow = static_cast<float>(layer.gateLow) / 100.f;
        lp.gateHigh = static_cast<float>(layer.gateHigh) / 100.f;
        lp.gateFeather = static_cast<float>(layer.gateFeather) / 100.f;
        lp.gateStrength = static_cast<float>(layer.gateStrength) / 100.f;
        lp.srcAspect = lp.srcH > 0 ? static_cast<float>(lp.srcW) / lp.srcH : baseAspect;
        lp.frame.baseW = 1.f;
        lp.frame.baseH = 1.f / baseAspect;
        lp.frame.srcX0 = 0.f;
        lp.frame.srcY0 = 0.f;
        lp.frame.srcW = 1.f;
        lp.frame.srcH = 1.f / lp.srcAspect;
        bool cropped = false;

#ifdef RT_AI_MASKING
        // Cache-only: a redraw never waits on inference. requestPartnerMasks
        // warms these on the decoder pool and asks for another pass.
        lp.mask = rtengine::PartnerMaskStore::getInstance().peekMask(
                      layer.source(), workingProfile_, layer.maskClass, layer.maskFeather,
                      layer.maskInvert, layer.maskPaint);

        if (layer.cropToSubject && lp.mask && lp.mask->hasBounds() && lp.mask->fullWidth > 0) {
            // The engine's source rect, in the picker's width-normalised units.
            const float toUnit = 1.f / lp.mask->fullWidth;
            lp.frame.srcX0 = lp.mask->x0 * toUnit;
            lp.frame.srcY0 = lp.mask->y0 * toUnit;
            lp.frame.srcW = (lp.mask->x1 - lp.mask->x0) * toUnit;
            lp.frame.srcH = (lp.mask->y1 - lp.mask->y0) * toUnit;
            cropped = true;
        }

        if (layer.depthEnabled) {
            // Cache-only as well; requestPartnerDepth fills it and redraws.
            lp.depth = rtengine::PartnerDepthStore::getInstance().peekDepth(layer.source(), workingProfile_);
            lp.depthSelect.range = layer.depthRange;
            lp.depthSelect.nearShape = layer.depthNearShape;
            lp.depthSelect.farShape = layer.depthFarShape;
            lp.depthSelect.invert = layer.depthInvert;
        }
#endif

        if (layer.motion.path != rtengine::demotion::Path::OFF && layer.motion.shutter > 0 && layer.motion.speed > 0) {
            if (!lp.plate) {
                waitingMotionSource = true;
                requestScenePlates({layer.source()}, wantHi ? PREVIEW_THUMB_HI : PREVIEW_THUMB_H);
            } else {
                DEMotionRequest request;
                request.source = lp.plate;
                request.params = layer.motion;
                request.edge = quick ? 384 : 768;
                request.quick = quick;
                std::ostringstream key;
                key << std::setprecision(17) << lp.plate.get() << ':' << int(layer.motion.path) << ':'
                    << layer.motion.shutter << ':' << layer.motion.speed << ':' << layer.motion.direction << ':'
                    << layer.motion.noise << ':' << layer.motion.complexity << ':' << layer.motion.seed << ':' << request.edge;
#ifdef RT_AI_MASKING
                request.mask = lp.mask;
                request.depth = lp.depth;
                request.depthSelect = lp.depthSelect;
                key << ':' << lp.mask.get() << ':' << lp.depth.get();
                if (lp.depth)
                    key << ':' << lp.depthSelect.range[0] << ':' << lp.depthSelect.range[1] << ':'
                        << lp.depthSelect.range[2] << ':' << lp.depthSelect.range[3] << ':'
                        << lp.depthSelect.nearShape << ':' << lp.depthSelect.farShape << ':' << lp.depthSelect.invert;
#endif
                request.key = key.str();
                for (const auto& cached : motionCache_) if (cached->key == request.key) { lp.motion = cached; break; }
                if (!lp.motion && std::none_of(motionRequests.begin(), motionRequests.end(), [&](const DEMotionRequest& r) {
                    return r.key == request.key;
                })) motionRequests.push_back(request);
            }
        }

        lp.frame.invCover = 1.f / std::max(lp.frame.baseW / lp.frame.srcW, lp.frame.baseH / lp.frame.srcH);
        rtengine::deplace::applyLayer(lp.frame, layer, cropped);

        if (selectedLayer_ < params_.layers.size() && &layer == &params_.layers[selectedLayer_]) {
            previewSelFrame_ = lp.frame;
            previewSelAspect_ = lp.srcAspect;
            previewSelValid_ = true;
        }

        if (lp.mode == DoubleExposureParams::BlendMode::ADD) {
            ++addLayers;
        }

        layerPix.push_back(lp);
    }

    if (!motionRequests.empty() || waitingMotionSource) {
        set_response_sensitive(Gtk::RESPONSE_OK, false);
        if (!motionRequests.empty()) requestMotionPreviews(motionRequests);
        return;
    }
    if (!pendingMotionKey_.empty()) {
        ++motionQueue_->revision;
        pendingMotionKey_.clear();
        motionSpinner_->stop();
        motionSpinner_->hide();
    }

    // The film gain follows how many frames actually land on each pixel, as
    // the engine does: metering the base down for a layer that is absent here
    // would leave a step at its edge.
    const bool meterFrames = params_.autoGain && addLayers > 0;
    const bool adaptiveStack = std::any_of(layerPix.begin(), layerPix.end(), [](const LayerPix& l) {
        return l.mode == DoubleExposureParams::BlendMode::ADAPTIVE;
    });
    const float baseEvGain = static_cast<float>(std::pow(2.0, params_.baseEv));

    // Film shoulder on the finished stack, same as the engine (white = 1
    // here), referenced to one frame's white: the auto-gain factor is undone
    // around the shoulder so the control works with metering on or off.
    const float latitude = std::min(std::max(static_cast<float>(params_.highlightLatitude) / 100.f, 0.f), 1.f);
    const bool applyShoulder = latitude > 0.f;
    const float knee = rtengine::deblend::latitudeKnee(latitude);

    // Scene mode: every input is an engine decode, so the composite runs in
    // the edit's working profile exactly as the engine does and is converted
    // to sRGB only for display. Otherwise everything stays in the
    // thumbnails' sRGB-linear space.
    const bool sceneMode = static_cast<bool>(basePlate) && allPlates && !layerPix.empty();
    const bool sceneFaithful = pc.haveTransfer && !layerPix.empty();

    // Remember the preview -> full-frame map for the drag handlers, and hand
    // the selected exposure's placed frame to the preview for its overlay.
    previewNx0_ = nx0;
    previewNxs_ = nxs;
    previewNy0_ = ny0;
    previewNys_ = nys;
    previewBaseAspect_ = baseAspect;

    if (selectedLayer_ < params_.layers.size()) {
        const auto& sel = params_.layers[selectedLayer_];
        const std::shared_ptr<const DEScenePlate> selPlate = findPlate(sel.source());
        const Glib::RefPtr<Gdk::Pixbuf> selPix = findIn(neutralHiPix_, neutralPix_, sel.path);
        float selAspect = baseAspect;

        if (selPlate && selPlate->h > 0) {
            selAspect = static_cast<float>(selPlate->w) / selPlate->h;
        } else if (selPix && selPix->get_height() > 0) {
            selAspect = static_cast<float>(selPix->get_width()) / selPix->get_height();
        }

        // The frame's own extent, before rotation: the overlay turns it.
        const double sx = baseAspect >= selAspect ? 1.0 : baseAspect / selAspect;
        const double sy = baseAspect >= selAspect ? selAspect / baseAspect : 1.0;
        const double scale = std::max(0.01, sel.scale / 100.0);
        const double offX = sel.offsetX / 100.0, offY = sel.offsetY / 100.0;
        const double nxa = 0.5 + offX - 0.5 * scale / sx, nxb = 0.5 + offX + 0.5 * scale / sx;
        const double nya = 0.5 + offY - 0.5 * scale / sy, nyb = 0.5 + offY + 0.5 * scale / sy;
        preview_->setSelectionFrame(true, (nxa - nx0) / nxs, (nya - ny0) / nys, (nxb - nx0) / nxs, (nyb - ny0) / nys,
                                    sel.rotate);
    } else {
        preview_->setSelectionFrame(false, 0, 0, 0, 0, 0.0);
    }

    // Render from immutable inputs; no worker reads widgets or mutable dialog state.
    const auto cache = previewCache_;
    const auto life = alive_;
    std::lock_guard<std::mutex> queueLock(queue->mutex);
    queue->job = [this, life, queue, revision, cache, layerPix, basePlate, neutralBase,
                  workingToSrgb, quick, w, h, nx0, nxs, ny0, nys, baseAspect,
                  nbW, nbH, nbStride, nbCh, srgbLut, sceneFaithful, sceneMode,
                  meterFrames, adaptiveStack, baseEvGain, applyShoulder, knee] {
    if (!*life || queue->revision != revision) return;
    try {
    rtengine::EffectPerfScope timing(quick ? "de-draft" : "de-settled", w, h);
    const auto& lin = cache->lin;
    const float* toneCurve = cache->toneCurve;
    const float satFactor = cache->satFactor;
    constexpr int TBINS = 256;
    const auto& srgbEnc = srgbEncodeLut();
    const auto& gateEnc = gateEncodeLut();
    const auto* nbPlate = basePlate.get();
    const auto* nbData = neutralBase ? neutralBase->get_pixels() : nullptr;
    // --- composite ----------------------------------------------------------
    // Interactive passes compute every other pixel in both directions and
    // replicate; the settled pass computes them all. Output stays w x h so
    // the overlay and drag coordinates never change.
    const int step = quick ? 2 : 1;
    // Vertical base coordinates go to the frame in physical units, so a
    // rotation is a real rotation and not one skewed by the frame's aspect.
    const float invBaseAspect = 1.f / baseAspect;
    auto result = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, w, h);
    guint8* outData = result->get_pixels();
    const int outStride = result->get_rowstride();
    const int blockRows = (h + step - 1) / step;
    const LayerPix* layers = layerPix.data();
    const int nLayers = static_cast<int>(layerPix.size());



#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int by = 0; by < blockRows; ++by) {
        if (!*life || (!quick && queue->revision != revision)) continue;
        std::vector<rtengine::deplace::Placed> hits(std::max(1, nLayers));
        std::vector<rtengine::demotion::Pixel> motionPixels(std::max(1, nLayers));
        const int y = by * step;
        const float ny = ny0 + (y + 0.5f) * nys;
        const int nby = (sceneFaithful && nbH > 0) ? std::max(0, std::min(static_cast<int>(ny * nbH), nbH - 1)) : 0;

        for (int x = 0; x < w; x += step) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            const float nx = nx0 + (x + 0.5f) * nxs;

            // Pass one: where does each layer land, and how much of it.
            float framesHere = 0.f;

            for (int li = 0; li < nLayers; ++li) {
                rtengine::deplace::Placed& hit = hits[li];
                hit = rtengine::deplace::place(layers[li].frame, nx, ny * invBaseAspect, nxs);
                if (hit.present && layers[li].motion) {
                    const auto& lp = layers[li];
                    auto p = lp.motion->at(hit.u, hit.v * lp.srcAspect);
                    p.a *= hit.coverage;
                    if (hit.mix > 0) {
                        auto q = lp.motion->at(hit.u2, hit.v2 * lp.srcAspect);
                        q.a *= hit.coverage2;
                        p = rtengine::demotion::mix(p, q, hit.mix);
                    }
                    motionPixels[li] = p;
                }

                if (hit.present && layers[li].mode == DoubleExposureParams::BlendMode::ADD) {
                    framesHere += layers[li].motion ? motionPixels[li].a * layers[li].opacity
                        : hit.coverage + hit.mix * (hit.coverage2 - hit.coverage);
                }
            }

            const float gainFactor = meterFrames ? 1.f / (1.f + framesHere) : 1.f;
            const float baseGain = baseEvGain * gainFactor;
            const float shoulderWhite = adaptiveStack ? 1.f : gainFactor;

            float r, g, b;

            if (sceneFaithful) {
                const int nbx = std::max(0, std::min(static_cast<int>(nx * nbW), nbW - 1));

                if (nbPlate) {
                    const size_t no = (static_cast<size_t>(nby) * nbW + nbx) * 3;
                    r = nbPlate->rgb[no];
                    g = nbPlate->rgb[no + 1];
                    b = nbPlate->rgb[no + 2];

                    if (!sceneMode) {
                        workingToSrgb(r, g, b);
                    }
                } else {
                    const guint8* np = nbData + nby * nbStride + nbx * nbCh;
                    r = srgbLut[np[0]];
                    g = srgbLut[np[1]];
                    b = srgbLut[np[2]];
                }

                r *= baseGain;
                g *= baseGain;
                b *= baseGain;
            } else {
                r = lin[o] * baseGain;
                g = lin[o + 1] * baseGain;
                b = lin[o + 2] * baseGain;
            }

            for (int li = 0; li < nLayers; ++li) {
                const LayerPix& lp = layers[li];
                const rtengine::deplace::Placed& hit = hits[li];

                if (!hit.present) {
                    continue;
                }

                float coverage = hit.coverage + hit.mix * (hit.coverage2 - hit.coverage);

                // Back to 0..1 of the source rect: the frame works in units
                // where the source is one wide and 1/aspect tall.
                // One read of the layer at source coordinates (u, v), which
                // the frame gives in units where the source is one wide.
                const auto readLayer = [&](float u, float v, float& pr, float& pg, float& pb) {
                    int lx = static_cast<int>(u * lp.srcW);
                    int ly = static_cast<int>(v * lp.srcAspect * lp.srcH);
                    lx = std::max(0, std::min(lx, lp.srcW - 1));
                    ly = std::max(0, std::min(ly, lp.srcH - 1));

                    if (lp.plate && (sceneMode || !lp.pix)) {
                        const size_t lo = (static_cast<size_t>(ly) * lp.srcW + lx) * 3;
                        pr = lp.plate->rgb[lo];
                        pg = lp.plate->rgb[lo + 1];
                        pb = lp.plate->rgb[lo + 2];

                        if (!sceneMode) {
                            workingToSrgb(pr, pg, pb);
                        }
                    } else {
                        const guint8* lrow = lp.pix->get_pixels() + ly * lp.pix->get_rowstride();
                        const int lch = lp.pix->get_n_channels();
                        pr = srgbLut[lrow[lx * lch]];
                        pg = srgbLut[lrow[lx * lch + 1]];
                        pb = srgbLut[lrow[lx * lch + 2]];
                    }
                };

                float pr, pg, pb;
                if (lp.motion) {
                    const auto& p = motionPixels[li];
                    pr = p.r; pg = p.g; pb = p.b; coverage = p.a;
                    if (!sceneMode) workingToSrgb(pr, pg, pb);
                } else {
                    readLayer(hit.u, hit.v, pr, pg, pb);
                }

                if (hit.mix > 0.f && !lp.motion) {
                    // Hand over to the neighbouring radial copy, as the engine
                    // does, or the picker would show a boundary the render
                    // no longer has.
                    float nr, ng, nb;
                    readLayer(hit.u2, hit.v2, nr, ng, nb);
                    pr += hit.mix * (nr - pr);
                    pg += hit.mix * (ng - pg);
                    pb += hit.mix * (nb - pb);
                }

                const float gain = lp.mode == DoubleExposureParams::BlendMode::ADD
                                   ? lp.gain * gainFactor : lp.gain;
                pr *= gain;
                pg *= gain;
                pb *= gain;

                float cr, cg, cb;
                rtengine::deblend::blend(lp.mode, lp.compare, lp.softness, 1.f, r, g, b, pr, pg, pb, cr, cg, cb);

                float wgt = lp.opacity * coverage;

#ifdef RT_AI_MASKING
                if (lp.mask && !lp.motion) {
                    const auto maskAt = [&](float u, float v) {
                        return lp.mask->sample(u * lp.mask->fullWidth,
                                               v * lp.srcAspect * lp.mask->fullHeight);
                    };
                    float m = maskAt(hit.u, hit.v);

                    if (hit.mix > 0.f) {
                        m += hit.mix * (maskAt(hit.u2, hit.v2) - m);
                    }

                    wgt *= m;

                    if (wgt <= 0.f) {
                        continue;
                    }
                }

                if (lp.depth && !lp.motion) {
                    const auto depthAt = [&](float u, float v) {
                        return lp.depthSelect.weight(lp.depth->sample(u * lp.depth->fullWidth,
                                                                      v * lp.srcAspect * lp.depth->fullHeight));
                    };
                    float d = depthAt(hit.u, hit.v);

                    if (hit.mix > 0.f) {
                        d += hit.mix * (depthAt(hit.u2, hit.v2) - d);
                    }

                    wgt *= d;

                    if (wgt <= 0.f) {
                        continue;
                    }
                }
#endif

                if (lp.gateStrength > 0.f) {
                    const float lum = lp.gateOnLayer ? rtengine::deblend::lum709(pr, pg, pb)
                                                     : rtengine::deblend::lum709(r, g, b);
                    wgt *= rtengine::deblend::gateWeight(lp.gateStrength,
                                                         rtengine::deblend::gateWindow(lutEncode(gateEnc, lum), lp.gateLow, lp.gateHigh, lp.gateFeather));
                }

                r += wgt * (cr - r);
                g += wgt * (cg - g);
                b += wgt * (cb - b);
            }

            if (applyShoulder) {
                r = shoulderWhite * rtengine::deblend::shoulder(std::max(r, 0.f) / shoulderWhite, knee);
                g = shoulderWhite * rtengine::deblend::shoulder(std::max(g, 0.f) / shoulderWhite, knee);
                b = shoulderWhite * rtengine::deblend::shoulder(std::max(b, 0.f) / shoulderWhite, knee);
            }

            if (sceneMode) {
                workingToSrgb(r, g, b);
            }

            if (sceneFaithful) {
                // Back to the edit's look: the plate's luminance curve as a
                // gain on the composited pixel (hue kept), then its saturation.
                r = std::max(r, 0.f);
                g = std::max(g, 0.f);
                b = std::max(b, 0.f);
                const float yv = rtengine::deblend::lum709(r, g, b);
                const float pos = std::min(std::max(lutEncode(gateEnc, yv) * TBINS - 0.5f, 0.f), static_cast<float>(TBINS - 1));
                const int i0 = static_cast<int>(pos);
                const int i1 = std::min(i0 + 1, TBINS - 1);
                const float f = pos - i0;
                const float ys = toneCurve[i0] * (1.f - f) + toneCurve[i1] * f;
                const float gain = ys / std::max(yv, 1e-6f);
                r *= gain;
                g *= gain;
                b *= gain;
                const float yo = rtengine::deblend::lum709(r, g, b);
                r = yo + (r - yo) * satFactor;
                g = yo + (g - yo) * satFactor;
                b = yo + (b - yo) * satFactor;
            }

            const guint8 pr8 = static_cast<guint8>(lutEncode(srgbEnc, r) * 255.f + 0.5f);
            const guint8 pg8 = static_cast<guint8>(lutEncode(srgbEnc, g) * 255.f + 0.5f);
            const guint8 pb8 = static_cast<guint8>(lutEncode(srgbEnc, b) * 255.f + 0.5f);

            const int yEnd = std::min(h, y + step);
            const int xEnd = std::min(w, x + step);

            for (int yy = y; yy < yEnd; ++yy) {
                guint8* outRow = outData + yy * outStride;

                for (int xx = x; xx < xEnd; ++xx) {
                    outRow[xx * 3] = pr8;
                    outRow[xx * 3 + 1] = pg8;
                    outRow[xx * 3 + 2] = pb8;
                }
            }
        }
    }

    if (!*life || (!quick && queue->revision != revision)) return;
    Glib::signal_idle().connect_once([this, life, queue, revision, result, quick] {
        if (!*life || !effectpreview::canPublish(quick, revision, queue->revision, presentedPreviewRevision_)) return;
        presentedPreviewRevision_ = revision;
        preview_->setComposite(result);
        set_response_sensitive(Gtk::RESPONSE_OK, !quick);
    });
    } catch (const std::exception& error) {
        const std::string message = error.what();
        Glib::signal_idle().connect_once([this, life, queue, revision, message] {
            if (!*life || queue->revision != revision) return;
            countLabel_->set_text(message);
            set_response_sensitive(Gtk::RESPONSE_OK, false);
        });
    } catch (...) {
        Glib::signal_idle().connect_once([this, life, queue, revision] {
            if (!*life || queue->revision != revision) return;
            countLabel_->set_text(M("TP_DOUBLEEXPOSURE_PREVIEW_FAILED"));
            set_response_sensitive(Gtk::RESPONSE_OK, false);
        });
    }
    };
    queue->wake.notify_one();
}
