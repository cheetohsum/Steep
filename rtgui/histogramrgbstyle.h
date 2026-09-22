#pragma once

#include <array>
#include <cairomm/context.h>
#include <cairomm/pattern.h>

namespace histogramstyle {

// Match the curve editor's RGB hues. Screen compositing on dark backgrounds
// gives bounded light mixing instead of additive clipping.
constexpr std::array<std::array<double, 3>, 3> colors{{
    {{.85, .20, .20}}, {{.20, .75, .20}}, {{.25, .35, .85}}
}};

template<typename Trace>
void drawRGB(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height,
             const std::array<bool, 3>& enabled, bool lightTheme, Trace trace)
{
    cr->save();
    cr->rectangle(0, 0, width, height);
    cr->clip();
    cr->set_antialias(Cairo::ANTIALIAS_GRAY);
    cr->set_line_join(Cairo::LINE_JOIN_ROUND);
    cr->set_line_cap(Cairo::LINE_CAP_ROUND);
    cairo_set_operator(cr->cobj(), lightTheme ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_SCREEN);

    for (int channel : {2, 1, 0}) {
        if (!enabled[channel]) continue;
        const auto& c = colors[channel];
        auto density = Cairo::LinearGradient::create(0, 0, 0, height);
        density->add_color_stop_rgba(0, c[0], c[1], c[2], lightTheme ? .34 : .40);
        density->add_color_stop_rgba(.65, c[0], c[1], c[2], .25);
        density->add_color_stop_rgba(1, c[0], c[1], c[2], .14);
        cr->begin_new_path();
        trace(channel, true);
        cr->set_source(density);
        cr->fill();
    }

    // A small multi-width contour glow gives a halo without blur buffers,
    // GPU dependencies, or any change to histogram bin heights.
    for (int channel : {2, 1, 0}) {
        if (!enabled[channel]) continue;
        const auto& c = colors[channel];
        for (const auto& pass : {std::array<double, 2>{{5, .045}},
                                std::array<double, 2>{{2.5, .10}},
                                std::array<double, 2>{{1, .62}}}) {
            cr->begin_new_path();
            trace(channel, false);
            cr->set_source_rgba(c[0], c[1], c[2], pass[1]);
            cr->set_line_width(pass[0]);
            cr->stroke();
        }
    }
    cr->restore();
}

} // namespace histogramstyle
