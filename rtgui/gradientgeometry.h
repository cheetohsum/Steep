#pragma once

#include <algorithm>
#include <cmath>

namespace gradientgeometry {

inline bool symmetricResize(int shape, int gradientType, int shapeMethod)
{
    return (shape == 2 && gradientType == 1) || shapeMethod == 1 || shapeMethod == 3;
}

struct LinearBand {
    double width = 0, height = 0;
    double x = 0, y = 0;
    double right = 0, left = 0, bottom = 0, top = 0;
    double nx = 0, ny = -1;
    double transit = 60;

    double projection() const
    {
        const double rx = std::min(std::max(right, left), std::max(x, width - x));
        const double ry = std::min(std::max(bottom, top), std::max(y, height - y));
        return rx * std::abs(nx) + ry * std::abs(ny);
    }

    double halfWidth() const { return projection() * transit / 100.; }
};

// Move one transition edge, keeping the other edge and the mask's bounding
// rectangle fixed. Always calculate from mouse-down, never rounded UI values.
inline LinearBand resize(const LinearBand& start, int side, double pointerTravel)
{
    if (start.width <= 0 || start.height <= 0 || (side != -1 && side != 1)) return start;
    const double half = start.halfWidth();
    auto candidate = [&](double shift) {
        auto band = start;
        const double dx = shift * start.nx, dy = shift * start.ny;
        band.x += dx; band.y += dy;
        band.right -= dx; band.left += dx;
        band.bottom -= dy; band.top += dy;
        const double projection = band.projection();
        band.transit = projection > 0 ? 100. * (half + side * shift) / projection : -1.;
        return band;
    };
    auto valid = [](const LinearBand& b) {
        return b.x >= 0 && b.x <= b.width && b.y >= 0 && b.y <= b.height
            && b.right >= b.width / 1000. && b.right <= b.width * 1.5
            && b.left >= b.width / 1000. && b.left <= b.width * 1.5
            && b.bottom >= b.height / 1000. && b.bottom <= b.height * 1.5
            && b.top >= b.height / 1000. && b.top <= b.height * 1.5
            && b.transit >= 0.5 && b.transit <= 100.;
    };
    const double shift = pointerTravel * 0.5;
    auto result = candidate(shift);
    if (valid(result)) return result;
    double low = 0, high = 1;
    for (int i = 0; i < 48; ++i) {
        const double mid = (low + high) * 0.5;
        if (valid(candidate(shift * mid))) low = mid;
        else high = mid;
    }
    return candidate(shift * low);
}

} // namespace gradientgeometry
