#pragma once
#include <array>
#include <cmath>
#include <functional>
#include <gtkmm.h>
#include "compositedepthhistogram.h"
#include "rtengine/compositemath.h"

namespace compositeui
{
// Four ordered handles over a depth histogram: near fade start, near full,
// far full, far fade end. Shared by the compositor's layers and Double
// Exposure's depth confinement so a range reads the same in both.
class DepthRange final : public Gtk::DrawingArea
{
  public:
    std::array<double, 4> values = {{0, 0, 1, 1}};
    double nearShape = 1, farShape = 1;
    bool inverted = false;
    DepthHistogram histogram;
    std::function<void()> begin, changed;
    int active = -1;
    DepthRange()
    {
        set_size_request(-1, 80);
        add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK);
    }
    bool on_draw(const Cairo::RefPtr<Cairo::Context> &cr) override
    {
        using rtengine::composite::depthColor;
        using rtengine::composite::depthWeight;
        const double w = std::max(1, get_allocated_width() - 24), h = 52;
        auto gradient = Cairo::LinearGradient::create(12, 0, w + 12, 0);
        for (float d : {0.f, .5f, 1.f}) {
            const auto c = depthColor(d);
            gradient->add_color_stop_rgba(d, c[0], c[1], c[2], .3);
        }
        cr->set_source(gradient);
        cr->rectangle(12, 12, w, h);
        cr->fill();
        if (histogram.samples) {
            const auto drawCurve = [&](bool brightness) {
                const auto curve = histogram.curve(brightness);
                cr->move_to(12, 12 + h);
                for (size_t i = 0; i < curve.size(); ++i)
                    cr->line_to(12 + w * i / (curve.size() - 1), 12 + h * (1 - .9 * curve[i]));
                if (brightness) {
                    cr->set_source_rgba(.96, .97, 1, .8);
                    cr->set_line_width(1.2);
                    cr->stroke();
                } else {
                    cr->line_to(12 + w, 12 + h);
                    cr->close_path();
                    cr->set_source_rgba(.88, .91, .96, .32);
                    cr->fill();
                }
            };
            drawCurve(false);
            drawCurve(true);
        }
        cr->set_source_rgba(.45, .85, .74, .9);
        cr->move_to(12, h + 12);
        for (int x = 0; x <= int(w); ++x)
            cr->line_to(x + 12, 12 + h * (1 - depthWeight(float(x) / w, values, nearShape, farShape, inverted)));
        cr->set_line_width(1.5);
        cr->stroke();
        for (int i = 0; i < 4; ++i) {
            cr->set_source_rgb(.9, .93, .93);
            cr->set_line_width(1);
            double x = 12 + values[i] * w;
            cr->move_to(x, 10);
            cr->line_to(x, 66);
            cr->stroke();
            cr->rectangle(x - 3, i % 2 ? 68 : 3, 6, 7);
            cr->fill();
        }
        return true;
    }
    bool on_button_press_event(GdkEventButton *e) override
    {
        double nearest = 1e9;
        for (int i = 0; i < 4; ++i) {
            double d = std::abs(e->x - 12 - values[i] * (get_allocated_width() - 24));
            if (d < nearest || (d == nearest && (e->y > 40) == bool(i % 2))) {
                nearest = d;
                active = i;
            }
        }
        if (begin)
            begin();
        return true;
    }
    bool on_motion_notify_event(GdkEventMotion *e) override
    {
        if (active < 0)
            return false;
        double v = (e->x - 12) / (get_allocated_width() - 24);
        values[active] =
            std::max(active ? values[active - 1] : 0., std::min(active < 3 ? values[active + 1] : 1., v));
        queue_draw();
        if (changed)
            changed();
        return true;
    }
    bool on_button_release_event(GdkEventButton *) override
    {
        active = -1;
        return true;
    }
};
} // namespace compositeui
