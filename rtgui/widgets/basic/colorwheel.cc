/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2024 RawTherapee team
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
#include "colorwheel.h"

#include <cmath>
#include <cstdio>

#include "multilangmgr.h"
#include "options.h"

#include "rtengine/color.h"
#include "rtengine/rt_math.h"

using namespace rtengine;

// ============================================================
// ColorWheelArea
// ============================================================

ColorWheelArea::ColorWheelArea()
    : Gtk::DrawingArea()
    , hue_(0)
    , sat_(0)
    , owner_(nullptr)
    , listener_(nullptr)
    , isDragged_(false)
    , edited_(false)
    , dragX_(0)
    , dragY_(0)
    , lastMouseX_(0)
    , lastMouseY_(0)
    , cachedSize_(0)
{
    set_can_focus(false);
    add_events(Gdk::EXPOSURE_MASK | Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
    set_name("ColorWheel");
    get_style_context()->add_class("drawingarea");
}

void ColorWheelArea::setHueSat(double hue, double saturation, bool notify)
{
    hue_ = hue;
    sat_ = saturation;
    queue_draw();
    if (notify) {
        notifyListener();
    }
}

void ColorWheelArea::reset()
{
    hue_ = 0;
    sat_ = 0;
    edited_ = false;
    queue_draw();

    // Resets that start on the wheel itself (double-click, right-click) have
    // nothing else to refresh the wrapper's dot and entry, which would
    // otherwise keep reading the old saturation.
    if (owner_) {
        owner_->updateIndicatorFromArea();
    }

    notifyListener();
}

bool ColorWheelArea::notifyListener()
{
    if (listener_) {
        listener_->colorWheelChanged(owner_, hue_, sat_);
    }
    return false; // for signal_timeout
}

void ColorWheelArea::wheelGeometry(double& cx, double& cy, double& radius) const
{
    const Gtk::Allocation alloc = get_allocation();
    const int size = std::min(alloc.get_width(), alloc.get_height());
    cx = alloc.get_width() / 2.0;
    cy = alloc.get_height() / 2.0;
    radius = (size - 10) / 2.0; // 5px inset on each side
}

// Seed the virtual drag point. A plain drag starts under the pointer, so the
// puck jumps to the press as it always has. A fine drag starts at the puck
// instead: shift means "nudge what is already set", and jumping first would
// discard that value before the first pixel of movement.
void ColorWheelArea::beginDrag(double mx, double my, bool fine)
{
    lastMouseX_ = mx;
    lastMouseY_ = my;

    if (fine) {
        double cx, cy, radius;
        wheelGeometry(cx, cy, radius);
        const double hueRad = hue_ * rtengine::RT_PI / 180.0;
        dragX_ = cx + sat_ * radius * std::cos(hueRad);
        dragY_ = cy - sat_ * radius * std::sin(hueRad);
    } else {
        dragX_ = mx;
        dragY_ = my;
    }
}

void ColorWheelArea::updateFromMouse(double mx, double my)
{
    double cx, cy, radius;
    wheelGeometry(cx, cy, radius);

    double dx = mx - cx;
    double dy = -(my - cy); // flip Y so up is positive

    double dist = std::sqrt(dx * dx + dy * dy);
    double newSat = std::min(dist / radius, 1.0);

    double angle = std::atan2(dy, dx); // radians
    double newHue = angle * 180.0 / rtengine::RT_PI;
    if (newHue < 0) {
        newHue += 360.0;
    }

    hue_ = newHue;
    sat_ = newSat;
    edited_ = true;

    // Update the parent ColorWheel's indicator
    if (owner_) {
        static_cast<ColorWheel*>(owner_)->updateIndicatorFromArea();
    }
}

bool ColorWheelArea::on_draw(const Cairo::RefPtr<Cairo::Context>& cr)
{
    const Gtk::Allocation alloc = get_allocation();
    const int w = alloc.get_width();
    const int h = alloc.get_height();
    const int size = std::min(w, h);
    const double cx = w / 2.0;
    const double cy = h / 2.0;
    const double radius = (size - 10) / 2.0;

    // Render background
    auto style = get_style_context();
    style->render_background(cr, 0, 0, w, h);

    // Rebuild cached wheel surface if size changed
    if (!cachedWheel_ || cachedSize_ != size) {
        cachedSize_ = size;
        cachedWheel_ = Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, w, h);
        auto wcr = Cairo::Context::create(cachedWheel_);

        // Draw color wheel pixel-by-pixel
        for (int py = 0; py < h; ++py) {
            for (int px = 0; px < w; ++px) {
                double dx = px - cx;
                double dy = -(py - cy);
                double dist = std::sqrt(dx * dx + dy * dy);

                if (dist <= radius) {
                    double angle = std::atan2(dy, dx);
                    double hueNorm = angle / (2.0 * rtengine::RT_PI);
                    if (hueNorm < 0) hueNorm += 1.0;

                    double satNorm = dist / radius;

                    float r, g, b;
                    rtengine::Color::hsv2rgb01(
                        static_cast<float>(hueNorm),
                        static_cast<float>(satNorm),
                        0.65f,
                        r, g, b
                    );

                    wcr->set_source_rgb(r, g, b);
                    wcr->rectangle(px, py, 1, 1);
                    wcr->fill();
                }
            }
        }

        // Draw circular border
        wcr->set_source_rgba(0.3, 0.3, 0.3, 0.8);
        wcr->set_line_width(1.5);
        wcr->arc(cx, cy, radius, 0, 2 * rtengine::RT_PI);
        wcr->stroke();
    }

    // Paint cached wheel
    cr->set_source(cachedWheel_, 0, 0);
    cr->paint();

    // Draw center crosshair
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.6);
    cr->set_line_width(1.0);
    cr->move_to(cx - 5, cy);
    cr->line_to(cx + 5, cy);
    cr->move_to(cx, cy - 5);
    cr->line_to(cx, cy + 5);
    cr->stroke();

    // Draw puck at current hue/sat position
    double hueRad = hue_ * rtengine::RT_PI / 180.0;
    double puckX = cx + sat_ * radius * std::cos(hueRad);
    double puckY = cy - sat_ * radius * std::sin(hueRad);

    // Puck shadow
    cr->set_source_rgba(0, 0, 0, 0.3);
    cr->arc(puckX + 1, puckY + 1, 7, 0, 2 * rtengine::RT_PI);
    cr->fill();

    // Puck fill with selected color
    {
        float r, g, b;
        float hueNorm = static_cast<float>(hue_ / 360.0);
        rtengine::Color::hsv2rgb01(hueNorm, static_cast<float>(sat_), 0.65f, r, g, b);
        cr->set_source_rgb(r, g, b);
        cr->arc(puckX, puckY, 6, 0, 2 * rtengine::RT_PI);
        cr->fill();
    }

    // Puck white outline
    cr->set_source_rgb(1, 1, 1);
    cr->set_line_width(2.0);
    cr->arc(puckX, puckY, 6, 0, 2 * rtengine::RT_PI);
    cr->stroke();

    return false;
}

void ColorWheelArea::on_style_updated()
{
    cachedWheel_.clear();
    cachedSize_ = 0;
    queue_draw();
}

bool ColorWheelArea::on_button_press_event(GdkEventButton* event)
{
    // Right-click anywhere on the wheel drops the puck back to the centre.
    // Deliberately not limited to inside the circle: it is a discard gesture,
    // so being forgiving about aim costs nothing.
    if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
        if (delayconn_.connected()) {
            delayconn_.disconnect();
        }

        isDragged_ = false;
        reset();
        return true;
    }

    if (event->button != 1) {
        return false;
    }

    if (event->type == GDK_2BUTTON_PRESS) {
        // Double-click resets
        reset();
        return true;
    }

    if (event->type == GDK_BUTTON_PRESS) {
        // Check if click is inside the wheel
        double cx, cy, radius;
        wheelGeometry(cx, cy, radius);

        double dx = event->x - cx;
        double dy = event->y - cy;
        double dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= radius + 5) { // slight tolerance
            const bool fine = event->state & GDK_SHIFT_MASK;
            isDragged_ = true;
            beginDrag(event->x, event->y, fine);

            if (fine) {
                // Nothing has moved yet, so there is nothing to report.
                return true;
            }

            updateFromMouse(dragX_, dragY_);

            const auto& options = App::get().options();
            if (options.adjusterMinDelay == 0) {
                notifyListener();
            } else {
                delayconn_ = Glib::signal_timeout().connect(
                    sigc::mem_fun(*this, &ColorWheelArea::notifyListener),
                    options.adjusterMinDelay);
            }
            queue_draw();
            return true;
        }
    }
    return false;
}

bool ColorWheelArea::on_button_release_event(GdkEventButton* event)
{
    if (isDragged_) {
        isDragged_ = false;
        notifyListener();
        return true;
    }
    return false;
}

bool ColorWheelArea::on_motion_notify_event(GdkEventMotion* event)
{
    if (isDragged_) {
        if (delayconn_.connected()) {
            delayconn_.disconnect();
        }

        // Shift advances the puck at a quarter of the pointer's speed. The
        // scale is applied per motion event, so it can be taken up and dropped
        // part-way through a drag without the puck lurching.
        const double speed = (event->state & GDK_SHIFT_MASK) ? 0.25 : 1.0;
        dragX_ += (event->x - lastMouseX_) * speed;
        dragY_ += (event->y - lastMouseY_) * speed;
        lastMouseX_ = event->x;
        lastMouseY_ = event->y;

        // Pin the virtual point to the wheel. Left to run free it would drift
        // arbitrarily far outside while the puck sat clamped at the rim, and
        // the drag would then feel dead until the pointer travelled all the
        // way back.
        {
            double cx, cy, radius;
            wheelGeometry(cx, cy, radius);
            const double dx = dragX_ - cx;
            const double dy = dragY_ - cy;
            const double dist = std::sqrt(dx * dx + dy * dy);

            if (dist > radius && dist > 0.0) {
                dragX_ = cx + dx * radius / dist;
                dragY_ = cy + dy * radius / dist;
            }
        }

        updateFromMouse(dragX_, dragY_);

        const auto& options = App::get().options();
        if (options.adjusterMinDelay == 0) {
            notifyListener();
        } else {
            delayconn_ = Glib::signal_timeout().connect(
                sigc::mem_fun(*this, &ColorWheelArea::notifyListener),
                options.adjusterMinDelay);
        }
        queue_draw();
        return true;
    }
    return false;
}

Gtk::SizeRequestMode ColorWheelArea::get_request_mode_vfunc() const
{
    return Gtk::SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

void ColorWheelArea::get_preferred_width_vfunc(int& minimum_width, int& natural_width) const
{
    minimum_width = 40;
    natural_width = 80;
}

void ColorWheelArea::get_preferred_height_for_width_vfunc(int width, int& minimum_height, int& natural_height) const
{
    minimum_height = natural_height = width; // square aspect ratio
}

// ============================================================
// ColorWheel (wrapper with label + indicator + editable entry)
// ============================================================

ColorWheel::ColorWheel(const Glib::ustring& label)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2)
    , label_(label)
    , indicatorBox_(Gtk::ORIENTATION_HORIZONTAL, 3)
    , updatingIndicator_(false)
{
    set_name("ColorWheel");
    area_.setOwner(this);
    // Shift-to-refine and right-click-to-reset are invisible otherwise.
    area_.set_tooltip_text(M("COLORWHEEL_TOOLTIP"));

    if (!label.empty()) {
        label_.set_halign(Gtk::ALIGN_CENTER);
        pack_start(label_, Gtk::PACK_SHRINK, 0);
    }

    pack_start(area_, Gtk::PACK_EXPAND_WIDGET, 0);

    // Indicator row: colored dot + text entry
    indicatorBox_.set_halign(Gtk::ALIGN_CENTER);

    dotLabel_.set_use_markup(true);
    dotLabel_.set_can_focus(false);
    indicatorBox_.pack_start(dotLabel_, Gtk::PACK_SHRINK, 0);

    entry_.set_width_chars(3);
    entry_.set_max_width_chars(3);
    entry_.set_max_length(3);
    entry_.set_alignment(0.5); // center text
    entry_.set_name("ColorWheelEntry");
    entry_.set_text("0");
    entry_.signal_activate().connect(
        sigc::mem_fun(*this, &ColorWheel::onEntryActivate));
    entry_.signal_focus_out_event().connect(
        sigc::mem_fun(*this, &ColorWheel::onEntryFocusOut));
    indicatorBox_.pack_start(entry_, Gtk::PACK_SHRINK, 0);

    pack_start(indicatorBox_, Gtk::PACK_SHRINK, 2);

    show_all_children();
    updateIndicator();
}

void ColorWheel::setHueSat(double hue, double saturation, bool notify)
{
    area_.setHueSat(hue, saturation, notify);
    updateIndicator();
}

void ColorWheel::reset()
{
    area_.reset();
    updateIndicator();
}

void ColorWheel::updateIndicator()
{
    updatingIndicator_ = true;

    double sat = area_.getSaturation();
    double hue = area_.getHue();

    // Update entry text: sat 0..1 -> display 0..100
    int value = static_cast<int>(std::round(sat * 100.0));
    entry_.set_text(Glib::ustring::format(value));

    // Update colored dot
    if (sat < 0.01) {
        dotLabel_.set_markup(
            "<span foreground='#555555' size='small'>\xe2\x97\x8f</span>");
    } else {
        float r, g, b;
        float hueNorm = static_cast<float>(hue / 360.0);
        rtengine::Color::hsv2rgb01(hueNorm, static_cast<float>(sat), 0.65f, r, g, b);

        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
            static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255));

        dotLabel_.set_markup(Glib::ustring::compose(
            "<span foreground='%1' size='small'>\xe2\x97\x8f</span>",
            Glib::ustring(hex)));
    }

    updatingIndicator_ = false;
}

void ColorWheel::onEntryActivate()
{
    applyEntryValue();
}

bool ColorWheel::onEntryFocusOut(GdkEventFocus*)
{
    applyEntryValue();
    return false;
}

void ColorWheel::applyEntryValue()
{
    if (updatingIndicator_) {
        return;
    }

    // Parse entry text as integer 0-100
    int val = 0;
    try {
        val = std::stoi(entry_.get_text().raw());
    } catch (...) {
        // Invalid input — reset to current value
        updateIndicator();
        return;
    }

    val = std::max(0, std::min(100, val));

    double newSat = val / 100.0;
    double curHue = area_.getHue();

    // If saturation was 0 and user types a value, default hue to 0
    if (area_.getSaturation() < 0.01 && newSat > 0.01) {
        curHue = 0;
    }

    area_.setHueSat(curHue, newSat, true);
    area_.setEdited(true);
    updateIndicator();
}

bool ColorWheel::resetPressed(GdkEventButton* event)
{
    area_.reset();
    updateIndicator();
    return true;
}
