#include "gradientgeometry.h"

#include <cstdlib>
#include <iostream>

namespace {
void near(double a, double b, const char* message, double tolerance = 1e-7)
{
    if (!std::isfinite(a) || std::abs(a - b) > tolerance) {
        std::cerr << "FAIL " << message << ": " << a << " != " << b << '\n';
        std::exit(1);
    }
}
void check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}
void fixedEdge(const gradientgeometry::LinearBand& a, const gradientgeometry::LinearBand& b, int side)
{
    near(a.x - side * a.nx * a.halfWidth(), b.x - side * b.nx * b.halfWidth(), "fixed edge X");
    near(a.y - side * a.ny * a.halfWidth(), b.y - side * b.ny * b.halfWidth(), "fixed edge Y");
}
void anchored(const gradientgeometry::LinearBand& a, const gradientgeometry::LinearBand& b, int side)
{
    fixedEdge(a, b, side);
    near(a.x - a.left, b.x - b.left, "left bounding edge");
    near(a.x + a.right, b.x + b.right, "right bounding edge");
    near(a.y - a.top, b.y - b.top, "top bounding edge");
    near(a.y + a.bottom, b.y + b.bottom, "bottom bounding edge");
}
double radians(double degrees) { return degrees * 3.141592653589793 / 180.; }
}

int main()
{
    for (int method = 0; method < 4; ++method) {
        if (!gradientgeometry::symmetricResize(2, 1, method)) return 3;
        if (gradientgeometry::symmetricResize(0, 0, method) != (method == 1 || method == 3)) return 4;
    }
    for (double angle : {0., 90., -90., 180., 37., -143.}) {
        for (int side : {-1, 1}) {
            for (double scale : {1., 0.125}) {
                gradientgeometry::LinearBand a;
                a.width = 6000 * scale; a.height = 4000 * scale;
                a.x = 2800 * scale; a.y = 1900 * scale;
                a.left = a.x; a.right = a.width - a.x;
                a.top = a.y; a.bottom = a.height - a.y;
                a.nx = std::sin(radians(angle));
                a.ny = -std::cos(radians(angle));
                a.transit = 60.;
                const double travel = -side * 240 * scale;
                const auto b = gradientgeometry::resize(a, side, travel);
                anchored(a, b, side);
                near(b.halfWidth(), a.halfWidth() - 120 * scale, "dragged edge follows pointer");
                const auto restored = gradientgeometry::resize(b, side, -travel);
                near(restored.x, a.x, "round trip center X");
                near(restored.y, a.y, "round trip center Y");
                near(restored.transit, a.transit, "round trip softness");
                for (double extreme : {-1e6, 1e6}) {
                    const auto limited = gradientgeometry::resize(a, side, extreme);
                    anchored(a, limited, side);
                    if (limited.transit < 0.499999 || limited.transit > 100.000001) return 2;
                }
                // Mouse-down based calculation cannot accumulate per-frame rounding.
                for (int i = 0; i < 100; ++i) gradientgeometry::resize(a, side, travel * i / 100.);
                near(gradientgeometry::resize(a, side, travel).halfWidth(), b.halfWidth(), "repeatability");
            }
        }
    }
    std::cout << "PASS anchored horizontal/vertical/rotated gradients, both edges, bounds, zoom scales and round trips\n";

    // A gradient made by the Gradient chip used to start with every extent at
    // the sliders' 3000 limit and Softness 100. Any inward move pushed the far
    // side past the limit and was refused, so its bars could not be dragged
    // in at all. Both frame shapes, both bars, square and rotated.
    for (bool portrait : {false, true}) {
        for (double angle : {0., 90., 30., -120.}) {
            for (int side : {-1, 1}) {
                gradientgeometry::LinearBand a;
                a.width = portrait ? 4000 : 6000; a.height = portrait ? 6000 : 4000;
                a.x = a.width / 2; a.y = a.height / 2;
                a.left = a.right = a.width * 1.5;
                a.top = a.bottom = a.height * 1.5;
                a.nx = std::sin(radians(angle));
                a.ny = -std::cos(radians(angle));
                a.transit = 100.;
                const double inward = -side * 600.;
                const auto b = gradientgeometry::resize(a, side, inward);
                check(b.transit < 99.9, "a bar at the extent limit moves inwards");
                near(b.halfWidth(), a.halfWidth() - 300., "the bar follows the pointer in", 1e-6);
                fixedEdge(a, b, side);
                check(b.left <= a.width * 1.5 && b.right <= a.width * 1.5
                      && b.top <= a.height * 1.5 && b.bottom <= a.height * 1.5, "extents stay inside the sliders");
                // The far sides that were held stay outside the frame.
                check(b.x - b.left <= 0 && b.x + b.right >= b.width
                      && b.y - b.top <= 0 && b.y + b.bottom >= b.height, "the box still covers the frame");
                const auto back = gradientgeometry::resize(b, side, -inward);
                near(back.halfWidth(), a.halfWidth(), "and back out again", 1e-6);
            }
        }
    }
    // Frame-fitted (1000) box, the shape-switch default: narrow by 90%.
    {
        gradientgeometry::LinearBand a;
        a.width = 4000; a.height = 6000; a.x = 2000; a.y = 3000;
        a.left = a.right = 2000; a.top = a.bottom = 3000;
        a.nx = 0; a.ny = -1; a.transit = 60.;
        const auto b = gradientgeometry::resize(a, 1, -0.9 * 2 * a.halfWidth());
        anchored(a, b, 1);
        check(b.halfWidth() < 0.2 * a.halfWidth() && b.transit >= 0.5, "a frame-fitted gradient narrows to a sliver");
    }
    std::cout << "PASS gradients at the extent limit narrow and widen, portrait and landscape\n";

    // Grip drags, from mouse-down: a slow 1 px-per-event drag at 1:1 on a
    // 6000 px frame is a third of a unit per event, which rounding each event
    // threw away entirely.
    {
        gradientgeometry::SpotBox start;
        start.locX = start.locXL = 600; start.locY = start.locYT = 600;
        for (int handle : {3, 4, 5, 6}) {
            for (double direction : {-1., 1.}) {
                gradientgeometry::SpotBox box = start;
                const bool horizontal = handle == 3 || handle == 4;
                for (int px = 1; px <= 300; ++px) {
                    const double t = direction * px;
                    box = gradientgeometry::dragSpot(start, handle, horizontal ? t : 0, horizontal ? 0 : t, 6000, 4000, false);
                }
                const double units = direction * 300 * 2000. / (horizontal ? 6000 : 4000);
                const double sign = (handle == 3 || handle == 5) ? 1 : -1;
                const double got = handle == 3 ? box.locX : handle == 4 ? box.locXL : handle == 5 ? box.locY : box.locYT;
                near(got, 600 + sign * units, "a slow grip drag lands where the pointer did");
                // Only the dragged side moves in independent mode.
                near(handle == 3 ? box.locXL : box.locX, 600, "the opposite side stays put");
            }
        }
        const auto sym = gradientgeometry::dragSpot(start, 4, 900, 0, 6000, 4000, true);
        near(sym.locXL, 300, "shortening from the left grip");
        near(sym.locX, 300, "symmetric mode mirrors to the right");
        const auto floor = gradientgeometry::dragSpot(start, 3, -1e6, 0, 6000, 4000, false);
        near(floor.locX, 2, "a grip cannot pass the centre");
        const auto moved = gradientgeometry::dragSpot(start, 0, 300, -200, 6000, 4000, false);
        near(moved.centerX, 100, "move X");
        near(moved.centerY, -100, "move Y");
        near(moved.locX, 600, "moving keeps the size");
    }
    std::cout << "PASS grip drags follow the pointer without rounding loss, both ways, symmetric and clamped\n";

    // Band lines are cut to the picture, whatever its shape.
    {
        const double minLength = 600;
        // Portrait 4000x6000, horizontal bar through the middle: 0..4000, not 6000 either way.
        auto s = gradientgeometry::frameSegment(2000, 3000, 1, 0, 4000, 6000, minLength);
        near(std::min(s.x0, s.x1), 0, "portrait bar starts at the left edge");
        near(std::max(s.x0, s.x1), 4000, "portrait bar ends at the right edge");
        near(s.grabX, 2000, "grip stays at the anchor");
        // Landscape: 0..6000.
        s = gradientgeometry::frameSegment(3000, 1000, 1, 0, 6000, 4000, minLength);
        near(std::max(s.x0, s.x1) - std::min(s.x0, s.x1), 6000, "landscape bar spans the width");
        // Diagonal through the centre of a portrait frame: ends on the frame.
        const double c = std::cos(radians(30)), sn = std::sin(radians(30));
        s = gradientgeometry::frameSegment(2000, 3000, c, sn, 4000, 6000, minLength);
        for (double v : {s.x0, s.x1}) check(v >= -1e-6 && v <= 4000 + 1e-6, "diagonal ends inside horizontally");
        for (double v : {s.y0, s.y1}) check(v >= -1e-6 && v <= 6000 + 1e-6, "diagonal ends inside vertically");
        check(std::abs(s.x0) < 1e-6 || std::abs(s.x0 - 4000) < 1e-6 || std::abs(s.y0) < 1e-6 || std::abs(s.y0 - 6000) < 1e-6,
              "diagonal reaches the frame");
        // A bar that only grazes a corner (Softness 100 at 45 degrees) keeps a grabbable length.
        const double d = std::sqrt(0.5);
        s = gradientgeometry::frameSegment(4000, 0, d, d, 4000, 6000, minLength);
        near(std::hypot(s.x1 - s.x0, s.y1 - s.y0), minLength, "a grazing bar is still long enough to grab", 1e-6);
        // Anchored off to the side of the visible part: the grip is pulled onto it.
        s = gradientgeometry::frameSegment(-500, 3000, 1, 0, 4000, 6000, minLength);
        near(s.grabX, 0, "grip clamps onto the visible bar");
    }
    std::cout << "PASS band lines are cut to portrait and landscape frames, grazing bars stay grabbable\n";
}
