#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace rtengine { namespace repairmath {

inline float smoothstep(float t) {
    t = std::max(0.f, std::min(1.f, t));
    return t * t * (3.f - 2.f * t);
}

inline float feather(float distance, float radius, float outer) {
    if (distance <= radius) return 1.f;
    if (distance >= outer || outer <= radius) return 0.f;
    return smoothstep((outer - distance) / (outer - radius));
}

// Area integration when shrinking, pixel-centred interpolation when growing.
// Separable weights preserve gradients even at fractional resampling ratios.
inline void resample(const float* src, int sw, int sh, float* dst, int dw, int dh) {
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, size_t(sw) * sh * sizeof(float));
        return;
    }
    const auto sample = [](const float* p, int stride, int n, int i, int out) {
        const double scale = double(n) / out;
        if (out < n) {
            const double a = i * scale, b = (i + 1) * scale;
            double sum = 0.;
            for (int j = int(a); j < std::min(n, int(std::ceil(b))); ++j) {
                sum += p[j * stride] * (std::min(b, double(j + 1)) - std::max(a, double(j)));
            }
            return float(sum / scale);
        }
        const double x = std::max(0., std::min(double(n - 1), (i + .5) * scale - .5));
        const int j = int(x);
        return float(p[j * stride] + (p[std::min(j + 1, n - 1) * stride] - p[j * stride]) * (x - j));
    };
    std::vector<float> tmp(size_t(dw) * sh);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < dw; ++x) tmp[size_t(y) * dw + x] = sample(src + size_t(y) * sw, 1, sw, x, dw);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) dst[size_t(y) * dw + x] = sample(tmp.data() + x, dw, sh, y, dh);
}

inline float overlapWeight(int p, int size, bool startEdge, bool endEdge, int overlap) {
    const float left = startEdge ? 1.f : smoothstep(float(p + 1) / overlap);
    const float right = endEdge ? 1.f : smoothstep(float(size - p) / overlap);
    return left * right;
}

// Delta is already alpha weighted at source resolution. Pixels outside the
// patch contribute zero, but still count in the view pixel's area.
inline float averageDelta(const std::vector<float>& delta, int pw, int ph,
                          int px, int py, int skip, int imageW, int imageH,
                          int imageX, int imageY) {
    const int nx = std::max(0, std::min(skip, imageW - imageX));
    const int ny = std::max(0, std::min(skip, imageH - imageY));
    if (!nx || !ny) return 0.f;
    double sum = 0.;
    for (int y = std::max(0, py); y < std::min(ph, py + ny); ++y)
        for (int x = std::max(0, px); x < std::min(pw, px + nx); ++x)
            sum += delta[size_t(y) * pw + x];
    return float(sum / (nx * ny));
}

}}
