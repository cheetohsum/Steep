#pragma once

#include <algorithm>
#include <cmath>

#include "array2D.h"

namespace rtengine
{
namespace masksampling
{
// x/y are full-image pixel-edge coordinates. Preview pixels cover step source pixels.
inline float atPixel(const array2D<float>& mask, int width, int height,
                     double x, double y, int fullWidth, int fullHeight, int step = 1)
{
    if (width <= 0 || height <= 0 || fullWidth <= 0 || fullHeight <= 0 || step <= 0 ||
        !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= fullWidth || y >= fullHeight)
        return 0.f;

    // Match the half-pixel convention used to generate/resample the AI mask.
    // A final partial preview pixel still samples inside the original image.
    const double centerX = x + .5 * std::min(double(step), fullWidth - x);
    const double centerY = y + .5 * std::min(double(step), fullHeight - y);
    const double px = std::max(0., std::min(double(width - 1), centerX * width / fullWidth - .5));
    const double py = std::max(0., std::min(double(height - 1), centerY * height / fullHeight - .5));
    const int x0 = int(px), y0 = int(py);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float fx = float(px - x0), fy = float(py - y0);
    const float top = mask[y0][x0] + fx * (mask[y0][x1] - mask[y0][x0]);
    const float bottom = mask[y1][x0] + fx * (mask[y1][x1] - mask[y1][x0]);
    return top + fy * (bottom - top);
}
} // namespace masksampling
} // namespace rtengine
