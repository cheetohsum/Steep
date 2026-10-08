#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace rtengine
{
// Depth-range selection maths shared by Double Exposure's depth confinement
// (the widget, the store and the engine), so a range reads the same everywhere.
namespace composite
{
inline float unit(float v) { return std::max(0.f, std::min(1.f, v)); }
inline float ease(float t, double shape)
{
    t = unit(t);
    // Monotonic endpoint-preserving curve; shape 1 gives a smoothstep fade.
    const float u = std::pow(t, std::max(.1, std::min(10., shape)));
    return u * u * (3.f - 2.f * u);
}
inline float depthWeight(float d, const std::array<double, 4> &v, double nearShape, double farShape,
                         bool invert)
{
    if (!std::isfinite(d) || d < 0 || d > 1)
        return 0;
    float n = v[1] <= v[0] ? float(d >= v[1]) : ease((d - v[0]) / (v[1] - v[0]), nearShape);
    float f = v[3] <= v[2] ? float(d <= v[2]) : ease((v[3] - d) / (v[3] - v[2]), farShape);
    const float a = n * f;
    return invert ? 1 - a : a;
}
// Display-only near/middle/far colors, shared by the depth ruler and composite overlay.
inline std::array<float, 3> depthColor(float depth)
{
    const std::array<float, 3> near = {{.98f, .56f, .25f}}, middle = {{.15f, .8f, .62f}},
                               far = {{.37f, .55f, .98f}};
    const float t = unit(depth) * 2;
    const auto &a = t <= 1 ? near : middle;
    const auto &b = t <= 1 ? middle : far;
    const float weight = t <= 1 ? t : t - 1;
    return {{a[0] + (b[0] - a[0]) * weight, a[1] + (b[1] - a[1]) * weight, a[2] + (b[2] - a[2]) * weight}};
}
} // namespace composite
} // namespace rtengine
