#include "gradientgeometry.h"

#include <cstdlib>
#include <iostream>

namespace {
void near(double a, double b, const char* message)
{
    if (!std::isfinite(a) || std::abs(a - b) > 1e-7) {
        std::cerr << "FAIL " << message << ": " << a << " != " << b << '\n';
        std::exit(1);
    }
}
void anchored(const gradientgeometry::LinearBand& a, const gradientgeometry::LinearBand& b, int side)
{
    near(a.x - side * a.nx * a.halfWidth(), b.x - side * b.nx * b.halfWidth(), "fixed edge X");
    near(a.y - side * a.ny * a.halfWidth(), b.y - side * b.ny * b.halfWidth(), "fixed edge Y");
    near(a.x - a.left, b.x - b.left, "left bounding edge");
    near(a.x + a.right, b.x + b.right, "right bounding edge");
    near(a.y - a.top, b.y - b.top, "top bounding edge");
    near(a.y + a.bottom, b.y + b.bottom, "bottom bounding edge");
}
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
                a.nx = std::sin(angle * 3.141592653589793 / 180.);
                a.ny = -std::cos(angle * 3.141592653589793 / 180.);
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
}
