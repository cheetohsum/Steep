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
#include "maskpaintdlg.h"

#include <algorithm>
#include <cmath>

#include "multilangmgr.h"
#include "../partnerthumb.h"

#ifdef RT_AI_MASKING
#include "rtengine/partnermaskstore.h"
#endif
#include "rtengine/array2D.h"

namespace
{

// The mask is drawn at this size at most: painting wants to be responsive
// more than it wants to be pixel-exact, and the strokes are stored as
// geometry, so what is replayed at full resolution is not what is shown here.
constexpr int MAX_CANVAS = 1100;

} // namespace

// ---------------------------------------------------------------------------
// MaskPaintCanvas — the picture with the mask over it, and a brush.
// ---------------------------------------------------------------------------

class MaskPaintCanvas final : public Gtk::DrawingArea
{
public:
    std::function<void()> onStrokes;   // a stroke was added, undone or cleared

    MaskPaintCanvas(const Glib::RefPtr<Gdk::Pixbuf>& picture,
                    const MaskPaintDlg::AutoMask& automatic,
                    const rtengine::MaskPaint& initial) :
        picture_(picture),
        paint_(initial)
    {
        set_size_request(520, 380);
        set_hexpand(true);
        set_vexpand(true);
        add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK
                   | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK | Gdk::LEAVE_NOTIFY_MASK);

        width_ = picture_ ? picture_->get_width() : 0;
        height_ = picture_ ? picture_->get_height() : 0;

        if (width_ <= 0 || height_ <= 0) {
            return;
        }

        // The automatic mask is resampled to the picture's own size, so every
        // buffer here shares one coordinate system.
        automatic_(width_, height_);

        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                float value = 0.f;

                if (automatic.valid()) {
                    const int sx = std::min(automatic.width - 1,
                                            static_cast<int>((x + 0.5) * automatic.width / width_));
                    const int sy = std::min(automatic.height - 1,
                                            static_cast<int>((y + 0.5) * automatic.height / height_));
                    value = automatic.values[static_cast<size_t>(sy) * automatic.width + sx];
                }

                automatic_[y][x] = value;
            }
        }

        baked_(width_, height_);
        shown_(width_, height_);
        rebake();
    }

    const rtengine::MaskPaint& paint() const
    {
        return paint_;
    }

    void setBrush(double radius, double hardness, double strength, bool add)
    {
        radius_ = radius;
        hardness_ = hardness;
        strength_ = strength;
        add_ = add;
        queue_draw();
    }

    void setShowMask(bool show)
    {
        showMask_ = show;
        queue_draw();
    }

    bool canUndo() const
    {
        return !paint_.strokes.empty();
    }

    void undo()
    {
        if (paint_.strokes.empty()) {
            return;
        }

        paint_.strokes.pop_back();
        rebake();
        queue_draw();

        if (onStrokes) {
            onStrokes();
        }
    }

    void clearAll()
    {
        if (paint_.strokes.empty()) {
            return;
        }

        paint_.strokes.clear();
        rebake();
        queue_draw();

        if (onStrokes) {
            onStrokes();
        }
    }

private:
    Glib::RefPtr<Gdk::Pixbuf> picture_;
    rtengine::MaskPaint paint_;
    array2D<float> automatic_;   // the mask as it arrived
    array2D<float> baked_;       // plus every finished stroke
    array2D<float> shown_;       // plus the one being drawn
    int width_ = 0;
    int height_ = 0;

    double radius_ = 0.08;
    double hardness_ = 0.25;
    double strength_ = 1.0;
    bool add_ = true;
    bool showMask_ = true;

    bool drawing_ = false;
    rtengine::MaskStroke live_;
    double pointerX_ = -1.0;
    double pointerY_ = -1.0;
    bool pointerIn_ = false;

    // picture -> widget, from the last draw
    double sc_ = 1.0;
    double ox_ = 0.0;
    double oy_ = 0.0;

    void rebake()
    {
        if (width_ <= 0) {
            return;
        }

        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                baked_[y][x] = automatic_[y][x];
            }
        }

        paint_.apply(baked_, width_, height_, true);
        refreshShown();
    }

    void refreshShown()
    {
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                shown_[y][x] = baked_[y][x];
            }
        }

        if (drawing_ && !live_.points.empty()) {
            rtengine::MaskPaint one;
            one.strokes.push_back(live_);
            one.apply(shown_, width_, height_, true);
        }
    }

    // widget -> normalised picture coordinates
    bool toPicture(double x, double y, double& nx, double& ny) const
    {
        if (sc_ <= 0.0 || width_ <= 0) {
            return false;
        }

        nx = (x - ox_) / sc_ / width_;
        ny = (y - oy_) / sc_ / height_;
        return true;
    }

    void beginStroke(double x, double y, bool add)
    {
        double nx, ny;

        if (!toPicture(x, y, nx, ny)) {
            return;
        }

        drawing_ = true;
        live_ = rtengine::MaskStroke();
        live_.add = add;
        live_.radius = radius_;
        live_.hardness = hardness_;
        live_.strength = strength_;
        live_.points.push_back({nx, ny});
        refreshShown();
        queue_draw();
    }

    void extendStroke(double x, double y)
    {
        double nx, ny;

        if (!drawing_ || !toPicture(x, y, nx, ny)) {
            return;
        }

        // Skip points the brush has barely moved past: a stroke is stored in
        // the pp3, and a thousand points a pixel apart help nobody.
        const auto& last = live_.points.back();
        const double step = std::hypot((nx - last.x) * width_, (ny - last.y) * height_);

        if (step < std::max(1.0, radius_ * std::min(width_, height_) * 0.15)) {
            return;
        }

        live_.points.push_back({nx, ny});
        refreshShown();
        queue_draw();
    }

    void endStroke()
    {
        if (!drawing_) {
            return;
        }

        drawing_ = false;

        if (!live_.points.empty()) {
            paint_.strokes.push_back(live_);
            rebake();
        }

        live_.points.clear();
        queue_draw();

        if (onStrokes) {
            onStrokes();
        }
    }

    bool on_button_press_event(GdkEventButton* e) override
    {
        if (e->button != 1 && e->button != 3) {
            return false;
        }

        // The right button paints the other way round, which saves crossing
        // the dialog to flip the mode for every correction.
        beginStroke(e->x, e->y, e->button == 1 ? add_ : !add_);
        return true;
    }

    bool on_motion_notify_event(GdkEventMotion* e) override
    {
        pointerX_ = e->x;
        pointerY_ = e->y;
        pointerIn_ = true;

        if (drawing_) {
            extendStroke(e->x, e->y);
        } else {
            queue_draw();
        }

        return true;
    }

    bool on_button_release_event(GdkEventButton* e) override
    {
        if (e->button == 1 || e->button == 3) {
            endStroke();
            return true;
        }

        return false;
    }

    bool on_leave_notify_event(GdkEventCrossing*) override
    {
        pointerIn_ = false;
        queue_draw();
        return false;
    }

    bool on_scroll_event(GdkEventScroll* e) override
    {
        double steps = 0.0;

        if (e->direction == GDK_SCROLL_UP) {
            steps = 1.0;
        } else if (e->direction == GDK_SCROLL_DOWN) {
            steps = -1.0;
        } else if (e->direction == GDK_SCROLL_SMOOTH) {
            steps = -e->delta_y;
        }

        if (steps == 0.0 || !onBrushResize) {
            return false;
        }

        onBrushResize(std::pow(1.12, steps));
        return true;
    }

public:
    std::function<void(double)> onBrushResize;   // scroll wheel over the canvas

private:
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override
    {
        const int w = get_allocated_width();
        const int h = get_allocated_height();

        cr->set_source_rgb(0.06, 0.06, 0.07);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        if (!picture_ || width_ <= 0) {
            return true;
        }

        sc_ = std::min(static_cast<double>(w) / width_, static_cast<double>(h) / height_);
        ox_ = (w - width_ * sc_) / 2.0;
        oy_ = (h - height_ * sc_) / 2.0;

        cr->save();
        cr->translate(ox_, oy_);
        cr->scale(sc_, sc_);
        Gdk::Cairo::set_source_pixbuf(cr, picture_, 0, 0);
        cr->paint();

        if (showMask_) {
            // Selected areas take a red wash; everything else is dimmed. A
            // tint alone reads poorly over a picture that is already warm or
            // already dark -- what makes a selection legible is the contrast
            // between what is in it and what is not.
            auto overlay = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, width_, height_);
            guint8* data = overlay->get_pixels();
            const int stride = overlay->get_rowstride();

            for (int y = 0; y < height_; ++y) {
                guint8* row = data + y * stride;

                for (int x = 0; x < width_; ++x) {
                    const float value = std::min(std::max(shown_[y][x], 0.f), 1.f);
                    guint8* px = row + x * 4;

                    // One continuous ramp from "not selected" to "fully
                    // selected". The previous version split at the half-way
                    // mark and jumped from a dim wash to a red one, so a
                    // brush with a long soft falloff drew what looked like a
                    // hard-edged blob -- the very thing the hardness control
                    // exists to avoid showing.
                    px[0] = static_cast<guint8>(10.f + 245.f * value);
                    px[1] = static_cast<guint8>(14.f + 31.f * value);
                    px[2] = static_cast<guint8>(22.f + 33.f * value);
                    px[3] = static_cast<guint8>(105.f + 55.f * value);
                }
            }

            Gdk::Cairo::set_source_pixbuf(cr, overlay, 0, 0);
            cr->paint();

            // A line on the half-way contour, so the boundary is visible even
            // where the picture underneath is busy.
            cr->save();
            cr->set_line_width(1.0 / std::max(sc_, 0.01));
            cr->set_source_rgba(1.0, 0.95, 0.6, 0.45);

            for (int y = 1; y < height_; ++y) {
                for (int x = 1; x < width_; ++x) {
                    const bool in = shown_[y][x] >= 0.5f;

                    if (in != (shown_[y][x - 1] >= 0.5f)) {
                        cr->move_to(x, y);
                        cr->line_to(x, y + 1);
                    }

                    if (in != (shown_[y - 1][x] >= 0.5f)) {
                        cr->move_to(x, y);
                        cr->line_to(x + 1, y);
                    }
                }
            }

            cr->stroke();
            cr->restore();
        }

        cr->restore();

        // Brush outline, so the size means something before the first stroke.
        if (pointerIn_ && sc_ > 0.0) {
            const double r = radius_ * std::min(width_, height_) * sc_;
            cr->arc(pointerX_, pointerY_, std::max(2.0, r), 0.0, 2.0 * G_PI);
            cr->set_source_rgba(0.0, 0.0, 0.0, 0.7);
            cr->set_line_width(3.0);
            cr->stroke_preserve();

            if (add_) {
                cr->set_source_rgb(0.4, 1.0, 0.5);
            } else {
                cr->set_source_rgb(1.0, 0.5, 0.4);
            }

            cr->set_line_width(1.0);
            cr->stroke();
        }

        return true;
    }
};

// ---------------------------------------------------------------------------
// MaskPaintDlg
// ---------------------------------------------------------------------------

MaskPaintDlg::MaskPaintDlg(Gtk::Window* parent,
                           const Glib::ustring& title,
                           const Glib::RefPtr<Gdk::Pixbuf>& picture,
                           const AutoMask& automatic,
                           const rtengine::MaskPaint& initial) :
    Gtk::Dialog(title, true)
{
    if (parent) {
        set_transient_for(*parent);
    }

    set_default_size(1000, 760);

    Glib::RefPtr<Gdk::Pixbuf> shown = picture;

    if (shown && std::max(shown->get_width(), shown->get_height()) > MAX_CANVAS) {
        const double k = static_cast<double>(MAX_CANVAS) / std::max(shown->get_width(), shown->get_height());
        shown = shown->scale_simple(std::max(1, static_cast<int>(shown->get_width() * k)),
                                    std::max(1, static_cast<int>(shown->get_height() * k)),
                                    Gdk::INTERP_BILINEAR);
    }

    Gtk::Box* content = get_content_area();
    content->set_spacing(6);
    content->set_border_width(6);

    canvas_ = Gtk::manage(new MaskPaintCanvas(shown, automatic, initial));
    canvas_->onStrokes = [this]() { onStrokesChanged(); };
    canvas_->onBrushResize = [this](double factor) {
        sizeScale_->set_value(std::min(std::max(sizeScale_->get_value() * factor, 0.5), 50.0));
    };
    content->pack_start(*canvas_, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Label* hint = Gtk::manage(new Gtk::Label(M("MASKPAINT_HINT"), Gtk::ALIGN_START));
    hint->set_xalign(0.f);
    hint->get_style_context()->add_class("dim-label");
    content->pack_start(*hint, Gtk::PACK_SHRINK);

    auto makeCell = [](const Glib::ustring& label, Gtk::Scale*& outScale,
                       double lo, double hi, double step, double value) {
        Gtk::Box* cell = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
        Gtk::Label* lab = Gtk::manage(new Gtk::Label(label, Gtk::ALIGN_START));
        lab->set_xalign(0.f);
        lab->get_style_context()->add_class("dim-label");
        outScale = Gtk::manage(new Gtk::Scale(Gtk::ORIENTATION_HORIZONTAL));
        outScale->set_range(lo, hi);
        outScale->set_increments(step, step * 5);
        outScale->set_value(value);
        outScale->set_draw_value(true);
        outScale->set_value_pos(Gtk::POS_RIGHT);
        outScale->set_digits(step < 1.0 ? 1 : 0);
        outScale->set_hexpand(true);
        cell->pack_start(*lab, Gtk::PACK_SHRINK);
        cell->pack_start(*outScale, Gtk::PACK_EXPAND_WIDGET);
        cell->set_hexpand(true);
        return cell;
    };

    Gtk::Box* brushRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12));
    brushRow->pack_start(*makeCell(M("MASKPAINT_SIZE"), sizeScale_, 0.5, 50.0, 0.5, 8.0), Gtk::PACK_EXPAND_WIDGET);
    brushRow->pack_start(*makeCell(M("MASKPAINT_HARDNESS"), hardnessScale_, 0.0, 100.0, 0.5, 25.0), Gtk::PACK_EXPAND_WIDGET);
    brushRow->pack_start(*makeCell(M("MASKPAINT_STRENGTH"), strengthScale_, 5.0, 100.0, 0.5, 100.0), Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*brushRow, Gtk::PACK_SHRINK);

    Gtk::Box* modeRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 8));
    includeBtn_ = Gtk::manage(new Gtk::RadioButton(M("MASKPAINT_INCLUDE")));
    excludeBtn_ = Gtk::manage(new Gtk::RadioButton(M("MASKPAINT_EXCLUDE")));
    excludeBtn_->join_group(*includeBtn_);
    includeBtn_->set_active(true);
    modeRow->pack_start(*includeBtn_, Gtk::PACK_SHRINK);
    modeRow->pack_start(*excludeBtn_, Gtk::PACK_SHRINK);

    showMask_ = Gtk::manage(new Gtk::CheckButton(M("MASKPAINT_SHOWMASK")));
    showMask_->set_active(true);
    modeRow->pack_start(*showMask_, Gtk::PACK_SHRINK);

    countLabel_ = Gtk::manage(new Gtk::Label(""));
    countLabel_->get_style_context()->add_class("dim-label");
    modeRow->pack_start(*countLabel_, Gtk::PACK_EXPAND_WIDGET);

    undoBtn_ = Gtk::manage(new Gtk::Button(M("MASKPAINT_UNDO")));
    clearBtn_ = Gtk::manage(new Gtk::Button(M("MASKPAINT_CLEAR")));
    modeRow->pack_end(*clearBtn_, Gtk::PACK_SHRINK);
    modeRow->pack_end(*undoBtn_, Gtk::PACK_SHRINK);
    content->pack_start(*modeRow, Gtk::PACK_SHRINK);

    sizeScale_->signal_value_changed().connect(sigc::mem_fun(*this, &MaskPaintDlg::onBrushChanged));
    hardnessScale_->signal_value_changed().connect(sigc::mem_fun(*this, &MaskPaintDlg::onBrushChanged));
    strengthScale_->signal_value_changed().connect(sigc::mem_fun(*this, &MaskPaintDlg::onBrushChanged));
    includeBtn_->signal_toggled().connect(sigc::mem_fun(*this, &MaskPaintDlg::onBrushChanged));
    showMask_->signal_toggled().connect([this]() { canvas_->setShowMask(showMask_->get_active()); });
    undoBtn_->signal_clicked().connect(sigc::mem_fun(*this, &MaskPaintDlg::undo));
    clearBtn_->signal_clicked().connect(sigc::mem_fun(*this, &MaskPaintDlg::clearAll));

    add_button(M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
    add_button(M("GENERAL_OK"), Gtk::RESPONSE_OK);
    set_default_response(Gtk::RESPONSE_OK);

    onBrushChanged();
    updateCounts();
    show_all_children();
}

MaskPaintDlg::~MaskPaintDlg() = default;

rtengine::MaskPaint MaskPaintDlg::getResult() const
{
    return canvas_->paint();
}

void MaskPaintDlg::onBrushChanged()
{
    // Size is a percentage of the frame's short side, which is what the
    // stroke stores, so a brush keeps its size across resolutions.
    canvas_->setBrush(sizeScale_->get_value() / 100.0,
                      hardnessScale_->get_value() / 100.0,
                      strengthScale_->get_value() / 100.0,
                      includeBtn_->get_active());
}

void MaskPaintDlg::onStrokesChanged()
{
    updateCounts();
}

void MaskPaintDlg::undo()
{
    canvas_->undo();
}

void MaskPaintDlg::clearAll()
{
    canvas_->clearAll();
}

void MaskPaintDlg::updateCounts()
{
    const size_t n = canvas_->paint().strokes.size();
    countLabel_->set_text(n == 0 ? M("MASKPAINT_NOSTROKES")
                                 : Glib::ustring::compose(M("MASKPAINT_STROKES"), n));
    undoBtn_->set_sensitive(n > 0);
    clearBtn_->set_sensitive(n > 0);
}

namespace maskpaint
{

bool refine(Gtk::Window* parent, const Glib::ustring& imagePath,
            const MaskPaintDlg::AutoMask& automatic, rtengine::MaskPaint& paint)
{
    if (imagePath.empty()) {
        return false;
    }

    // Neutral framing: the engine finds masks on the upright, uncropped frame,
    // so that is the picture the strokes have to be painted on.
    Glib::RefPtr<Gdk::Pixbuf> picture = partnerthumb::load(imagePath, 900, true, false);

    if (!picture) {
        return false;
    }

    MaskPaintDlg dialog(parent, M("MASKPAINT_TITLE"), picture, automatic, paint);

    if (dialog.run() != Gtk::RESPONSE_OK) {
        return false;
    }

    const rtengine::MaskPaint result = dialog.getResult();

    if (result == paint) {
        return false;
    }

    paint = result;
    return true;
}

#ifdef RT_AI_MASKING
MaskPaintDlg::AutoMask fromPartner(const std::shared_ptr<const rtengine::PartnerMask>& mask)
{
    MaskPaintDlg::AutoMask out;

    if (!mask || !mask->valid()) {
        return out;
    }

    out.width = mask->width;
    out.height = mask->height;
    out.values.resize(static_cast<size_t>(out.width) * out.height);

    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            out.values[static_cast<size_t>(y) * out.width + x] = mask->mask[y][x];
        }
    }

    return out;
}
#endif

} // namespace maskpaint
