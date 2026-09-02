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

#include "rtengine/colortemp.h"
#include "rtengine/doubleexposureblend.h"
#include "rtengine/iccstore.h"
#include "rtengine/imagefloat.h"
#include "rtengine/partnerimagestore.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
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

// Needed in C++14: std::max binds these to a reference, which odr-uses them.
// See the same note in rtgui/thumbimageupdater.cc.
constexpr int DEThumbGrid::CELL_W;
constexpr int DEThumbGrid::THUMB_H;
constexpr int DEThumbGrid::TEXT_H;
constexpr int DEThumbGrid::PAD;

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
    std::function<void(double)> onScale;         // multiplicative size factor
    std::function<void()> onReset;               // double-click
    std::function<void()> onGestureEnd;          // button released after a drag

    DEBlendPreview()
    {
        set_size_request(420, 300);
        set_hexpand(true);
        set_vexpand(true);
        add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK
                   | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK | Gdk::LEAVE_NOTIFY_MASK);
    }

    void setComposite(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
    {
        composite_ = pixbuf;
        queue_draw();
    }

    // The selected layer's placed frame, in composite pixel coordinates (may
    // extend past the composite). Hidden when nothing is selected.
    void setSelectionFrame(bool visible, double x0, double y0, double x1, double y1)
    {
        frameVisible_ = visible;
        fx0_ = std::min(x0, x1);
        fx1_ = std::max(x0, x1);
        fy0_ = std::min(y0, y1);
        fy1_ = std::max(y0, y1);
        queue_draw();
    }

private:
    enum class Drag { NONE, MOVE, SCALE };

    Glib::RefPtr<Gdk::Pixbuf> composite_;
    bool frameVisible_ = false;
    double fx0_ = 0.0, fy0_ = 0.0, fx1_ = 0.0, fy1_ = 0.0;
    // composite -> widget mapping from the last draw
    double sc_ = 1.0, ox_ = 0.0, oy_ = 0.0;
    Drag drag_ = Drag::NONE;
    double lastX_ = 0.0, lastY_ = 0.0;
    double scaleStartDist_ = 1.0;
    bool hoverHandle_ = false;
    bool hoverFrame_ = false;

    double wx(double cx) const { return ox_ + cx * sc_; }
    double wy(double cy) const { return oy_ + cy * sc_; }
    double centreX() const { return wx((fx0_ + fx1_) * 0.5); }
    double centreY() const { return wy((fy0_ + fy1_) * 0.5); }

    bool hitHandle(double x, double y) const
    {
        if (!frameVisible_) {
            return false;
        }

        const double xs[2] = {wx(fx0_), wx(fx1_)};
        const double ys[2] = {wy(fy0_), wy(fy1_)};

        for (double hx : xs) {
            for (double hy : ys) {
                if (std::fabs(x - hx) <= DE_HANDLE_PX + 3.0 && std::fabs(y - hy) <= DE_HANDLE_PX + 3.0) {
                    return true;
                }
            }
        }

        return false;
    }

    bool hitFrame(double x, double y) const
    {
        return frameVisible_ && x >= wx(fx0_) && x <= wx(fx1_) && y >= wy(fy0_) && y <= wy(fy1_);
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
        const bool handle = hitHandle(x, y);
        const bool frame = !handle && hitFrame(x, y);

        if (handle != hoverHandle_ || frame != hoverFrame_) {
            hoverHandle_ = handle;
            hoverFrame_ = frame;
            setCursorName(handle ? "nwse-resize" : (frame ? "move" : "default"));
            queue_draw();
        }
    }

    bool on_button_press_event(GdkEventButton* e) override
    {
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
        if (drag_ == Drag::NONE && (hoverHandle_ || hoverFrame_)) {
            hoverHandle_ = hoverFrame_ = false;
            setCursorName("default");
            queue_draw();
        }

        return false;
    }

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
        cr->restore();

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
    initSceneContext();

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
    preview_->onMove = [this](double dx, double dy) { onPreviewMove(dx, dy); };
    preview_->onScale = [this](double factor) { onPreviewScale(factor); };
    preview_->onReset = [this]() { onPreviewReset(); };
    right->pack_start(*preview_, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label* previewHint = Gtk::manage(new Gtk::Label(M("DOUBLEEXPOSURE_PREVIEW_HINT")));
    previewHint->set_xalign(0.f);
    previewHint->set_line_wrap(true);
    previewHint->get_style_context()->add_class("dim-label");
    right->pack_start(*previewHint, Gtk::PACK_SHRINK);

    highRes_ = Gtk::manage(new Gtk::CheckButton(M("DOUBLEEXPOSURE_HIGHRES")));
    highRes_->set_halign(Gtk::ALIGN_END);
    highRes_->set_active(App::get().options().dePickerHighRes); // before the handler is wired
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

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_LATITUDE"), latitudeScale_, 0.0, 100.0, 1.0, params_.highlightLatitude), Gtk::PACK_SHRINK);
    latitudeScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_LATITUDE_TOOLTIP"));
    latitudeScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::blendControlChanged));

    Gtk::Separator* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    right->pack_start(*sep, Gtk::PACK_SHRINK);

    layerLabel_ = Gtk::manage(new Gtk::Label("", Gtk::ALIGN_START));
    layerLabel_->set_xalign(0.f);
    right->pack_start(*layerLabel_, Gtk::PACK_SHRINK);

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_EV"), layerEvScale_, -4.0, 4.0, 0.05, 0.0), Gtk::PACK_SHRINK);
    layerEvScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_OPACITY"), layerOpacityScale_, 0.0, 100.0, 1.0, 100.0), Gtk::PACK_SHRINK);
    layerOpacityScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_GATE_STRENGTH"), gateStrengthScale_, 0.0, 100.0, 1.0, 0.0), Gtk::PACK_SHRINK);
    gateStrengthScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_GATE_TOOLTIP"));
    gateStrengthScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    // Comparative bright/dark only: how wide the hand-over between frames is.
    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_SOFTNESS"), softnessScale_, 0.0, 2.0, 0.05, 0.5), Gtk::PACK_SHRINK);
    softnessScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_SOFTNESS_TOOLTIP"));
    softnessScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    // Placement of the selected exposure over the base frame; the preview
    // drives the same three values by dragging.
    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_OFFSETX"), offsetXScale_, -150.0, 150.0, 0.5, 0.0), Gtk::PACK_SHRINK);
    offsetXScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    offsetXScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_OFFSETY"), offsetYScale_, -150.0, 150.0, 0.5, 0.0), Gtk::PACK_SHRINK);
    offsetYScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    offsetYScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));
    right->pack_start(*makeScaleRow(M("TP_DOUBLEEXPOSURE_SCALE"), scaleScale_, 10.0, 400.0, 1.0, 100.0), Gtk::PACK_SHRINK);
    scaleScale_->set_tooltip_text(M("TP_DOUBLEEXPOSURE_PLACEMENT_TOOLTIP"));
    scaleScale_->signal_value_changed().connect(sigc::mem_fun(*this, &DoubleExposureDlg::layerControlChanged));

    resetPlacement_ = Gtk::manage(new Gtk::Button(M("TP_DOUBLEEXPOSURE_PLACEMENT_RESET")));
    resetPlacement_->set_halign(Gtk::ALIGN_END);
    resetPlacement_->signal_clicked().connect(sigc::mem_fun(*this, &DoubleExposureDlg::onPreviewReset));
    right->pack_start(*resetPlacement_, Gtk::PACK_SHRINK);

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

    // Resume where the picker was last closed: scope and grid scroll. The
    // global button's handler runs the scan for that scope.
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
    if (pendingScrollRestore_ < 0.0 || !gridScroll_) {
        return;
    }

    const auto adj = gridScroll_->get_vadjustment();

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
    scenePaths.insert(scenePaths.end(), layerPaths.begin(), layerPaths.end());
    requestScenePlates(scenePaths, height);
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
            queue.emplace_front(DEThumbReq{*it, height, neutral, basePlate, false});
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
            for (;;) {
                Glib::ustring path;
                int height = 0;

                bool neutral = false;
                bool basePlate = false;
                bool scene = false;

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
                    source.pop_front();
                }

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
            thumbQueue_->preview.emplace_front(DEThumbReq{*it, height, false, false, true});
        }
    }

    pumpThumbQueue();
}

void DoubleExposureDlg::onSceneLoaded(const Glib::ustring& path, int height, std::shared_ptr<const DEScenePlate> plate)
{
    pendingThumbs_.erase(path + Glib::ustring::compose("|%1|scene", height));

    if (!plate) {
        return;
    }

    (height == PREVIEW_THUMB_HI ? sceneHiPix_ : scenePix_)[path] = std::move(plate);
    updatePreview();
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
        softnessScale_->set_sensitive(false);
        offsetXScale_->set_sensitive(false);
        offsetYScale_->set_sensitive(false);
        scaleScale_->set_sensitive(false);
        resetPlacement_->set_sensitive(false);
        blendMethod_->set_sensitive(false);
    } else {
        const auto& layer = params_.layers[selectedLayer_];
        layerLabel_->set_text(Glib::ustring::compose("%1 %2 \xE2\x80\x94 %3",
                              M("TP_DOUBLEEXPOSURE_LAYER"), selectedLayer_ + 1,
                              Glib::path_get_basename(layer.path)));
        const bool comparative = layer.blendMode == DoubleExposureParams::BlendMode::LIGHTEN
                                 || layer.blendMode == DoubleExposureParams::BlendMode::DARKEN;
        layerEvScale_->set_sensitive(true);
        layerOpacityScale_->set_sensitive(true);
        gateStrengthScale_->set_sensitive(true);
        softnessScale_->set_sensitive(comparative && layer.compare == DoubleExposureParams::Compare::LUMINANCE);
        blendMethod_->set_sensitive(true);
        layerEvScale_->set_value(layer.ev);
        layerOpacityScale_->set_value(layer.opacity);
        gateStrengthScale_->set_value(layer.gateStrength);
        softnessScale_->set_value(layer.softness);
        offsetXScale_->set_sensitive(true);
        offsetYScale_->set_sensitive(true);
        scaleScale_->set_sensitive(true);
        resetPlacement_->set_sensitive(true);
        offsetXScale_->set_value(layer.offsetX);
        offsetYScale_->set_value(layer.offsetY);
        scaleScale_->set_value(layer.scale);
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
    params_.layers[selectedLayer_].softness = softnessScale_->get_value();
    params_.layers[selectedLayer_].offsetX = offsetXScale_->get_value();
    params_.layers[selectedLayer_].offsetY = offsetYScale_->get_value();
    params_.layers[selectedLayer_].scale = scaleScale_->get_value();
    schedulePreviewUpdate();
}

// --- interactive placement from the preview ---

void DoubleExposureDlg::syncPlacementControls()
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    const auto& layer = params_.layers[selectedLayer_];
    syncingControls_ = true;
    offsetXScale_->set_value(layer.offsetX);
    offsetYScale_->set_value(layer.offsetY);
    scaleScale_->set_value(layer.scale);
    syncingControls_ = false;
}

// Motion events arrive faster than the composite can be rebuilt at the
// high-res tier; coalesce them into one recomposite per idle.
void DoubleExposureDlg::schedulePreviewUpdate()
{
    auto alive = alive_;

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

void DoubleExposureDlg::onPreviewReset()
{
    if (selectedLayer_ >= params_.layers.size()) {
        return;
    }

    auto& layer = params_.layers[selectedLayer_];
    layer.offsetX = 0.0;
    layer.offsetY = 0.0;
    layer.scale = 100.0;
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
                                 || layer.blendMode == DoubleExposureParams::BlendMode::DARKEN;
        softnessScale_->set_sensitive(comparative && layer.compare == DoubleExposureParams::Compare::LUMINANCE);
    }

    params_.autoGain = autoGain_->get_active();
    params_.baseEv = baseEvScale_->get_value();
    params_.highlightLatitude = latitudeScale_->get_value();

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

    const std::vector<float>& srgbEnc = srgbEncodeLut();
    const std::vector<float>& gateEnc = gateEncodeLut();

    float srgbLut[256];

    for (int i = 0; i < 256; ++i) {
        srgbLut[i] = srgbToLin(i / 255.f);
    }

    const auto workingToSrgb = [this](float& r, float& g, float& b) {
        const double rr = workingToSrgb_[0][0] * r + workingToSrgb_[0][1] * g + workingToSrgb_[0][2] * b;
        const double gg = workingToSrgb_[1][0] * r + workingToSrgb_[1][1] * g + workingToSrgb_[1][2] * b;
        const double bb = workingToSrgb_[2][0] * r + workingToSrgb_[2][1] * g + workingToSrgb_[2][2] * b;
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
    PreviewCache& pc = previewCache_;

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
    const std::vector<float>& lin = pc.lin;
    constexpr int TBINS = 256;
    const float* toneCurve = pc.toneCurve;
    const float satFactor = pc.satFactor;

    // --- layers -----------------------------------------------------------
    struct LayerPix {
        std::shared_ptr<const DEScenePlate> plate; // engine decode (preferred)
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
        float sx = 1.f; // engine cover-fit scales, from the two full-frame aspects
        float sy = 1.f;
        float offX = 0.f; // placement, fraction of the base frame
        float offY = 0.f;
        float scale = 1.f;
        bool placed = false;
    };

    std::vector<LayerPix> layerPix;
    int addLayers = 0;
    bool allPlates = true;

    for (const auto& layer : params_.layers) {
        if (!layer.enabled) {
            continue;
        }

        LayerPix lp;
        lp.plate = findPlate(layer.path);
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
        lp.offX = static_cast<float>(layer.offsetX) / 100.f;
        lp.offY = static_cast<float>(layer.offsetY) / 100.f;
        lp.scale = std::max(0.01f, static_cast<float>(layer.scale) / 100.f);
        lp.placed = lp.offX != 0.f || lp.offY != 0.f || lp.scale != 1.f;

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

    // Film shoulder on the finished stack, same as the engine (white = 1
    // here), referenced to one frame's white: the auto-gain factor is undone
    // around the shoulder so the control works with metering on or off.
    const float latitude = std::min(std::max(static_cast<float>(params_.highlightLatitude) / 100.f, 0.f), 1.f);
    const bool applyShoulder = latitude > 0.f;
    const float knee = rtengine::deblend::latitudeKnee(latitude);
    const float shoulderWhite = autoGainFactor;

    // Scene mode: every input is an engine decode, so the composite runs in
    // the edit's working profile exactly as the engine does and is converted
    // to sRGB only for display. Otherwise everything stays in the
    // thumbnails' sRGB-linear space.
    const bool sceneMode = static_cast<bool>(basePlate) && allPlates && !layerPix.empty();
    const bool sceneFaithful = pc.haveTransfer && !layerPix.empty();

    // Cover-fit scales for a layer of the given aspect (engine cover math
    // reduced to the two full-frame aspect ratios).
    const auto coverScales = [baseAspect](float layerAspect, float& sx, float& sy) {
        if (baseAspect >= layerAspect) {
            sx = 1.f;
            sy = layerAspect / baseAspect;
        } else {
            sx = baseAspect / layerAspect;
            sy = 1.f;
        }
    };

    for (auto& lp : layerPix) {
        const float layerAspect = lp.srcH > 0 ? static_cast<float>(lp.srcW) / lp.srcH : baseAspect;
        coverScales(layerAspect, lp.sx, lp.sy);
    }

    // Remember the preview -> full-frame map for the drag handlers, and hand
    // the selected exposure's placed frame to the preview for its overlay.
    previewNx0_ = nx0;
    previewNxs_ = nxs;
    previewNy0_ = ny0;
    previewNys_ = nys;

    if (selectedLayer_ < params_.layers.size()) {
        const auto& sel = params_.layers[selectedLayer_];
        const std::shared_ptr<const DEScenePlate> selPlate = findPlate(sel.path);
        const Glib::RefPtr<Gdk::Pixbuf> selPix = findIn(neutralHiPix_, neutralPix_, sel.path);
        float selAspect = baseAspect;

        if (selPlate && selPlate->h > 0) {
            selAspect = static_cast<float>(selPlate->w) / selPlate->h;
        } else if (selPix && selPix->get_height() > 0) {
            selAspect = static_cast<float>(selPix->get_width()) / selPix->get_height();
        }

        float sx = 1.f, sy = 1.f;
        coverScales(selAspect, sx, sy);
        const double scale = std::max(0.01, sel.scale / 100.0);
        const double offX = sel.offsetX / 100.0, offY = sel.offsetY / 100.0;
        const double nxa = 0.5 + offX - 0.5 * scale / sx, nxb = 0.5 + offX + 0.5 * scale / sx;
        const double nya = 0.5 + offY - 0.5 * scale / sy, nyb = 0.5 + offY + 0.5 * scale / sy;
        preview_->setSelectionFrame(true, (nxa - nx0) / nxs, (nya - ny0) / nys, (nxb - nx0) / nxs, (nyb - ny0) / nys);
    } else {
        preview_->setSelectionFrame(false, 0, 0, 0, 0);
    }

    // --- composite ----------------------------------------------------------
    // Interactive passes compute every other pixel in both directions and
    // replicate; the settled pass computes them all. Output stays w x h so
    // the overlay and drag coordinates never change.
    const int step = quick ? 2 : 1;
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
        const int y = by * step;
        const float ny = ny0 + (y + 0.5f) * nys;
        const int nby = (sceneFaithful && nbH > 0) ? std::max(0, std::min(static_cast<int>(ny * nbH), nbH - 1)) : 0;

        for (int x = 0; x < w; x += step) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            const float nx = nx0 + (x + 0.5f) * nxs;

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
                const float un = (nx - 0.5f - lp.offX) * lp.sx / lp.scale + 0.5f;
                const float vn = (ny - 0.5f - lp.offY) * lp.sy / lp.scale + 0.5f;
                float coverage = 1.f;

                if (lp.placed) {
                    const float aaX = std::max(nxs * lp.sx / lp.scale, 1e-6f);
                    const float aaY = std::max(nys * lp.sy / lp.scale, 1e-6f);
                    const float ex = std::min(un, 1.f - un) / aaX;
                    const float ey = std::min(vn, 1.f - vn) / aaY;
                    coverage = std::min(std::max(std::min(ex, ey) + 0.5f, 0.f), 1.f);

                    if (coverage <= 0.f) {
                        continue;
                    }
                }

                int lx = static_cast<int>(un * lp.srcW);
                int ly = static_cast<int>(vn * lp.srcH);
                lx = std::max(0, std::min(lx, lp.srcW - 1));
                ly = std::max(0, std::min(ly, lp.srcH - 1));

                float pr, pg, pb;

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

                pr *= lp.gain;
                pg *= lp.gain;
                pb *= lp.gain;

                float cr, cg, cb;
                rtengine::deblend::blend(lp.mode, lp.compare, lp.softness, 1.f, r, g, b, pr, pg, pb, cr, cg, cb);

                float wgt = lp.opacity * coverage;

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

    preview_->setComposite(result);
}
