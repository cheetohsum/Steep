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
    notifyListener();
}

bool ColorWheelArea::notifyListener()
{
    if (listener_) {
        listener_->colorWheelChanged(owner_, hue_, sat_);
    }
    return false; // for signal_timeout
}

void ColorWheelArea::updateFromMouse(double mx, double my)
{
    const Gtk::Allocation alloc = get_allocation();
    const int size = std::min(alloc.get_width(), alloc.get_height());
    const double cx = alloc.get_width() / 2.0;
    const double cy = alloc.get_height() / 2.0;
    const double radius = (size - 10) / 2.0; // 5px inset on each side

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
        const Gtk::Allocation alloc = get_allocation();
        const int size = std::min(alloc.get_width(), alloc.get_height());
        const double cx = alloc.get_width() / 2.0;
        const double cy = alloc.get_height() / 2.0;
        const double radius = (size - 10) / 2.0;

        double dx = event->x - cx;
        double dy = event->y - cy;
        double dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= radius + 5) { // slight tolerance
            isDragged_ = true;
            updateFromMouse(event->x, event->y);

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

        updateFromMouse(event->x, event->y);

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
// ColorWheel (wrapper with label + reset)
// ============================================================

ColorWheel::ColorWheel(const Glib::ustring& label)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2)
    , label_(label)
{
    set_name("ColorWheel");
    area_.setOwner(this);

    if (!label.empty()) {
        label_.set_halign(Gtk::ALIGN_CENTER);
        pack_start(label_, Gtk::PACK_SHRINK, 0);
    }

    pack_start(area_, Gtk::PACK_EXPAND_WIDGET, 0);

    // Reset button (double-click on wheel also resets)
    // No external reset button needed - double-click on wheel resets

    show_all_children();
}

void ColorWheel::setHueSat(double hue, double saturation, bool notify)
{
    area_.setHueSat(hue, saturation, notify);
}

void ColorWheel::reset()
{
    area_.reset();
}

bool ColorWheel::resetPressed(GdkEventButton* event)
{
    area_.reset();
    return true;
}
