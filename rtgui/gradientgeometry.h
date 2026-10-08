#pragma once

#include <algorithm>
#include <cmath>

namespace gradientgeometry {

inline bool symmetricResize(int shape, int gradientType, int shapeMethod)
{
    return (shape == 2 && gradientType == 1) || shapeMethod == 1 || shapeMethod == 3;
}

// The extent sliders stop at 3000, one and a half frames from the centre.
constexpr double kMaxExtent = 1.5;

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
        // A side already at the sliders' limit is held there instead of
        // refusing the move. With the centre inside the frame such a side is
        // at least half a frame outside it, where it neither bounds anything
        // nor enters the projection (which is capped at the frame). Refusing
        // is what left a gradient created at 3000 impossible to narrow.
        band.right = std::min(band.right - dx, start.width * kMaxExtent);
        band.left = std::min(band.left + dx, start.width * kMaxExtent);
        band.bottom = std::min(band.bottom - dy, start.height * kMaxExtent);
        band.top = std::min(band.top + dy, start.height * kMaxExtent);
        const double projection = band.projection();
        band.transit = projection > 0 ? 100. * (half + side * shift) / projection : -1.;
        return band;
    };
    auto valid = [](const LinearBand& b) {
        return b.x >= 0 && b.x <= b.width && b.y >= 0 && b.y <= b.height
            && b.right >= b.width / 1000. && b.left >= b.width / 1000.
            && b.bottom >= b.height / 1000. && b.top >= b.height / 1000.
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

// A spot's position and extents in its stored units: centre -1000..1000 and
// extents 2..3000, both in thousandths of half the frame on that axis.
struct SpotBox {
    double centerX = 0, centerY = 0;
    double locX = 150, locXL = 150, locY = 150, locYT = 150;
};

// Where a drag of one of the spot's grips leaves it. handle is the geometry
// slot: 0-2 the centre and outline (move), 3 right, 4 left, 5 bottom, 6 top.
// The pointer travel is measured from mouse-down in image pixels, so nothing
// is lost to rounding however slowly the grip moves; adding each motion
// event's rounded delta dropped every step under half a unit, which at 1:1
// is three pixels of a 6000 px frame -- the grip simply did not follow.
inline SpotBox dragSpot(const SpotBox& start, int handle, double travelX, double travelY,
                        double width, double height, bool symmetric)
{
    SpotBox box = start;
    if (width <= 0 || height <= 0) return box;
    const double ux = travelX * 2000. / width;
    const double uy = travelY * 2000. / height;
    const auto extent = [](double v) { return std::min(std::max(v, 2.), kMaxExtent * 2000.); };
    const auto centre = [](double v) { return std::min(std::max(v, -1000.), 1000.); };

    switch (handle) {
        case 0: case 1: case 2:
            box.centerX = centre(start.centerX + ux);
            box.centerY = centre(start.centerY + uy);
            break;
        case 3:
            box.locX = extent(start.locX + ux);
            if (symmetric) box.locXL = box.locX;
            break;
        case 4:
            box.locXL = extent(start.locXL - ux);
            if (symmetric) box.locX = box.locXL;
            break;
        case 5:
            box.locY = extent(start.locY + uy);
            if (symmetric) box.locYT = box.locY;
            break;
        case 6:
            box.locYT = extent(start.locYT - uy);
            if (symmetric) box.locY = box.locYT;
            break;
        default:
            break;
    }
    return box;
}

struct Segment {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double grabX = 0, grabY = 0; // the point nearest to where the line was anchored
};

// The piece of the line through (px, py) along the unit direction (dx, dy)
// that crosses the picture. Band lines used to run a fixed length either
// side of the spot, clipped only by the window, so on a portrait photo they
// stretched across the grey on both sides as if the frame were landscape. A
// line that only grazes a corner is grown to minLength about its middle so
// it can still be taken hold of.
inline Segment frameSegment(double px, double py, double dx, double dy,
                            double width, double height, double minLength)
{
    double lo = -1e300, hi = 1e300;
    const auto clip = [&](double p, double d, double limit) {
        if (std::abs(d) < 1e-12) {
            if (p < 0 || p > limit) { lo = 1; hi = 0; }
            return;
        }
        double a = (0 - p) / d, b = (limit - p) / d;
        if (a > b) std::swap(a, b);
        lo = std::max(lo, a);
        hi = std::min(hi, b);
    };
    clip(px, dx, width);
    clip(py, dy, height);
    if (lo > hi) {
        // Misses the frame: centre the grip on the frame's middle instead.
        lo = hi = (width / 2 - px) * dx + (height / 2 - py) * dy;
    }
    if (hi - lo < minLength) {
        const double mid = (lo + hi) / 2;
        lo = mid - minLength / 2;
        hi = mid + minLength / 2;
    }
    Segment s;
    s.x0 = px + lo * dx; s.y0 = py + lo * dy;
    s.x1 = px + hi * dx; s.y1 = py + hi * dy;
    const double grab = std::min(std::max(0., lo), hi);
    s.grabX = px + grab * dx; s.grabY = py + grab * dy;
    return s;
}

} // namespace gradientgeometry
