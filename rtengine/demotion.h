#pragma once
#include "demotionparams.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rtengine
{
namespace demotion
{
using Params = procparams::DEMotionParams;
using Path = Params::Path;
struct Pose {
    float x = 0, y = 0, c = 1, s = 0, invZoom = 1;
};
struct Trajectory {
    int width = 1;
    std::vector<Pose> knots;
    bool empty() const { return knots.empty(); }
};
struct Pixel {
    float r = 0, g = 0, b = 0, a = 0;
};
inline uint32_t hash(uint32_t n)
{
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    return n ^ (n >> 16);
}
inline int nextSeed(int seed) { return int(hash(uint32_t(seed)) % 2147483646U) + 1; }
inline float noise(float t, uint32_t seed)
{
    const int k = int(std::floor(t));
    const float f = t - k, s = f * f * (3 - 2 * f);
    const auto n = [&](int i) { return float(hash(uint32_t(i) ^ seed) & 0xffffU) / 32767.5f - 1.f; };
    return n(k) + s * (n(k + 1) - n(k));
}
inline Pose pose(const Params &p, float t)
{
    constexpr float pi = 3.14159265358979323846f;
    const float travel = std::min(4., std::max(0., p.speed * p.shutter / 100));
    const float complexity = std::max(1., std::min(8., p.complexity));
    const float jitter = std::max(0., std::min(1., p.noise / 100));
    float x = t, y = 0, angle = 0, zoom = 0;
    const auto n = [&](float v, uint32_t salt) {
        return noise(v, uint32_t(p.seed) ^ salt) - noise(0, uint32_t(p.seed) ^ salt);
    };
    switch (p.path) {
    case Path::WAVE:
        y = .22f * std::sin(t * 2 * pi * complexity);
        break;
    case Path::ZIGZAG:
        y = .22f * (2 / pi) * std::asin(std::sin(t * 2 * pi * complexity));
        break;
    case Path::SWIRL:
        x = 0;
        angle = t * 2 * pi;
        break;
    case Path::ZOOM_IN:
        x = 0;
        zoom = t;
        break;
    case Path::ZOOM_OUT:
        x = 0;
        zoom = -t;
        break;
    case Path::HANDHELD:
        x = .3f * n(t * complexity, 0x381aU);
        y = .3f * n(t * complexity, 0x61b3U);
        angle = .12f * n(t * complexity, 0x753cU);
        break;
    case Path::WANDER:
        x = .45f * t + .5f * n(t * complexity, 0x84dbU);
        y = .5f * n(t * complexity, 0x1fe3U);
        break;
    default:
        break;
    }
    x += jitter * .35f * n(t * (complexity + 3), 0xdeadU);
    y += jitter * .35f * n(t * (complexity + 3), 0xbeefU);
    angle += jitter * .08f * n(t * complexity, 0xabcdU);
    const float direction = float(p.direction) * pi / 180;
    Pose result;
    result.x = travel * (x * std::cos(direction) - y * std::sin(direction));
    result.y = travel * (x * std::sin(direction) + y * std::cos(direction));
    result.c = std::cos(angle * travel);
    result.s = std::sin(angle * travel);
    result.invZoom = std::exp(-zoom * travel);
    return result;
}
inline Trajectory trajectory(const Params &p, int width, int maxIntervals)
{
    if (p.path == Path::OFF || p.speed <= 0 || p.shutter <= 0)
        return {};
    // These are interval boundaries, not separate exposures. The sample loop
    // integrates between them; limiting path storage must not leave holes in light trails.
    float distance = 0;
    auto previous = pose(p, 0);
    for (int i = 1; i <= 128; ++i) {
        const auto q = pose(p, i / 128.f);
        distance +=
            std::hypot(q.x - previous.x, q.y - previous.y) +
            .7f * (std::hypot(q.c - previous.c, q.s - previous.s) + std::abs(q.invZoom - previous.invZoom));
        previous = q;
    }
    const int count = std::max(1, std::min(maxIntervals, std::max(12, int(std::ceil(distance * width * 1.25f)))));
    Trajectory result;
    result.width = std::max(1, width);
    result.knots.reserve(count + 1);
    for (int i = 0; i <= count; ++i)
        result.knots.push_back(pose(p, float(i) / count));
    return result;
}
template <class Read> Pixel sample(const Trajectory &path, float u, float v, float aspect, Read read)
{
    if (path.empty())
        return read(u, v);
    const auto map = [&](const Pose &q) {
        const float x = u - .5f - q.x, y = (v - .5f) / aspect - q.y;
        return std::array<float, 2>{{(q.c * x + q.s * y) * q.invZoom + .5f,
                                     (-q.s * x + q.c * y) * q.invZoom * aspect + .5f}};
    };
    double r = 0, g = 0, b = 0, alpha = 0;
    auto previous = map(path.knots.front());
    for (size_t i = 1; i < path.knots.size(); ++i) {
        const auto next = map(path.knots[i]);
        const float du = next[0] - previous[0], dv = next[1] - previous[1];
        const float su = previous[0], sv = previous[1];
        previous = next;
        // Clip shutter time to observed image coverage before doing expensive
        // reads. Missing time stays transparent instead of extending edge pixels.
        float begin = 0, end = 1;
        const auto clip = [&](float start, float delta) {
            if (std::abs(delta) < 1e-12f) return start >= 0 && start <= 1;
            const float a = -start / delta, z = (1 - start) / delta;
            begin = std::max(begin, std::min(a, z));
            end = std::min(end, std::max(a, z));
            return end > begin;
        };
        if (!clip(su, du) || !clip(sv, dv))
            continue;
        // Half-pixel temporal footprints join fine highlights continuously,
        // even when a long path exceeds the number of stored intervals.
        const float length = (end - begin) * path.width * std::max(std::abs(du), std::abs(dv) / aspect);
        const int count = std::max(1, int(std::ceil(length * 2)));
        const float dt = (end - begin) / count;
        const double weight = double(dt) / (path.knots.size() - 1);
        for (int k = 0; k < count; ++k) {
            const float t = begin + (k + .5f) * dt;
            const Pixel p = read(std::max(0.f, std::min(1.f, su + t * du)),
                                 std::max(0.f, std::min(1.f, sv + t * dv)));
            const double a = p.a * weight;
            r += p.r * a;
            g += p.g * a;
            b += p.b * a;
            alpha += a;
        }
    }
    if (alpha <= 1e-8) return {};
    return {float(r / alpha), float(g / alpha), float(b / alpha), float(alpha)};
}
inline Pixel mix(Pixel a, const Pixel &b, float t)
{
    const float wa = a.a * (1 - t), wb = b.a * t, w = wa + wb;
    if (w <= 1e-8f)
        return {};
    return {(a.r * wa + b.r * wb) / w, (a.g * wa + b.g * wb) / w, (a.b * wa + b.b * wb) / w, w};
}
struct Buffer {
    int w = 0, h = 0;
    std::vector<Pixel> pixels;
    Pixel at(float u, float v) const
    {
        const float fx = std::max(0.f, std::min(float(w - 1), u * w - .5f));
        const float fy = std::max(0.f, std::min(float(h - 1), v * h - .5f));
        const int x = int(fx), y = int(fy), nx = std::min(x + 1, w - 1), ny = std::min(y + 1, h - 1);
        return mix(mix(pixels[size_t(y) * w + x], pixels[size_t(y) * w + nx], fx - x),
                   mix(pixels[size_t(ny) * w + x], pixels[size_t(ny) * w + nx], fx - x), fy - y);
    }
};
} // namespace demotion
} // namespace rtengine
