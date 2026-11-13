/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The depth scale the "near" and "far" masks select from, and the band a
// mask takes out of it. Header-only and free of the engine's types so the
// arithmetic can be tested on its own (tools/depth_scale_tests.cc).
//
// The scale runs from 0 at the nearest thing in the frame to 1 at the
// farthest, in equal steps of distance *ratio*: halfway is the geometric
// middle distance, the way a lens's focus scale is marked.
//
// A band is {a, b, c, d}: full weight from the near limit b to the far limit
// c, fading to nothing over a feather on either side (a = b - feather,
// d = c + feather, so a may be below 0 and d above 1). All -1 stands for
// "automatic": near or far of the photo's own split.
namespace rtengine
{
namespace depthscale
{

using Range = std::array<double, 4>;

constexpr double kMaxFeather = 0.5;
constexpr double kDefaultFeather = 0.10;
// Farther than this many times the nearest distance is "the far end".
constexpr float kFarReach = 40.f;
// From here on the scale is the far end (sky, the horizon).
constexpr double kFarEnd = 0.97;

inline bool isAutomatic(const Range& range)
{
    return range[3] < 0.0;
}

inline Range makeRange(double nearLimit, double farLimit, double feather)
{
    const double n = std::max(0.0, std::min(1.0, nearLimit));
    const double f = std::max(n, std::min(1.0, farLimit));
    const double w = std::max(0.0, std::min(kMaxFeather, feather));
    return {{n - w, n, f, f + w}};
}

inline void splitRange(const Range& range, double& nearLimit, double& farLimit, double& feather)
{
    nearLimit = range[1];
    farLimit = range[2];
    feather = std::max(range[1] - range[0], range[3] - range[2]);
}

// 0..1 weight of a depth sample inside a band.
inline float weight(float d, const Range& v)
{
    const auto ease = [](double t) {
        t = std::max(0.0, std::min(1.0, t));
        return t * t * (3.0 - 2.0 * t);
    };
    const double n = v[1] <= v[0] ? (d >= v[1] ? 1.0 : 0.0) : ease((d - v[0]) / (v[1] - v[0]));
    const double f = v[3] <= v[2] ? (d <= v[2] ? 1.0 : 0.0) : ease((v[3] - d) / (v[3] - v[2]));
    return static_cast<float>(n * f);
}

// The automatic band of a near or far mask: everything this side of the
// split, with the default feather centred on it, so that the two are each
// other's complement.
inline Range preset(bool far, double split)
{
    const double half = kDefaultFeather / 2.0;
    return far ? makeRange(std::min(1.0, split + half), 1.0, kDefaultFeather)
               : makeRange(0.0, std::max(0.0, split - half), kDefaultFeather);
}

// "a;b;c;d" to a band; automatic when empty or malformed. Limits come back
// ordered and inside 0..1 and feathers no wider than allowed, whatever the
// text says.
inline Range parse(const std::string& text)
{
    const Range automatic{{-1.0, -1.0, -1.0, -1.0}};
    if (text.empty()) {
        return automatic;
    }
    Range parsed{};
    std::size_t start = 0;
    for (int k = 0; k < 4; ++k) {
        const std::size_t end = text.find(';', start);
        const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        char* stop = nullptr;
        const double value = std::strtod(part.c_str(), &stop);
        if (part.empty() || stop == part.c_str() || !std::isfinite(value)) {
            return automatic;
        }
        parsed[k] = value;
        if (end == std::string::npos) {
            if (k != 3) {
                return automatic;
            }
            break;
        }
        start = end + 1;
    }
    const double n = std::max(0.0, std::min(1.0, parsed[1]));
    const double f = std::max(n, std::min(1.0, parsed[2]));
    return {{n - std::max(0.0, std::min(kMaxFeather, parsed[1] - parsed[0])), n,
             f, f + std::max(0.0, std::min(kMaxFeather, parsed[3] - parsed[2]))}};
}

inline std::string format(const Range& range)
{
    if (isAutomatic(range)) {
        return std::string();
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.4f;%.4f;%.4f;%.4f", range[0], range[1], range[2], range[3]);
    return buffer;
}

// Inverse depth (the estimator's native output: large is near, 0 is
// infinity) to the scale.
//
// Read directly, inverse depth is a badly lopsided depth scale: half of it
// is spent between the nearest point and twice that distance, everything
// from a few metres to the horizon shares the last tenth, and one stray
// sample at either end squeezes the rest. So the scale is anchored at robust
// ends -- the 0.5% nearest and farthest samples aside, and no farther than
// kFarReach times the nearest distance -- and runs logarithmically between
// them.
struct Scale {
    float nearest = 1.f;    // inverse depth at 0
    float farthest = 0.5f;  // inverse depth at 1
    float span = 1.f;       // log(nearest / farthest), kept off zero

    float at(float inverseDepth) const
    {
        const float clamped = std::max(farthest, std::min(nearest, inverseDepth));
        return std::max(0.f, std::min(1.f, std::log(nearest / clamped) / span));
    }

    // How many times farther the far end is than the near end.
    float reach() const
    {
        return nearest / farthest;
    }
};

// `samples` is reordered. An empty set gives the default scale.
inline Scale fitScale(std::vector<float>& samples)
{
    Scale scale;
    if (samples.empty()) {
        return scale;
    }
    const auto rankAt = [&samples](double fraction) {
        const std::size_t k = std::min(samples.size() - 1, static_cast<std::size_t>(fraction * samples.size()));
        std::nth_element(samples.begin(), samples.begin() + k, samples.end());
        return samples[k];
    };
    scale.nearest = std::max(1e-6f, rankAt(0.995));
    scale.farthest = std::max(std::max(rankAt(0.005), scale.nearest / kFarReach), 1e-7f);
    // A flat subject (a wall square to the lens) has almost no range; keep
    // the scale from blowing its noise up to the full 0..1.
    scale.span = std::log(std::max(1.05f, scale.nearest / scale.farthest));
    return scale;
}

// Otsu's split of the scale's histogram (any number of equal bins over
// 0..1): where the depths fall apart into a near group and a far one. What
// sits at the far end is far by definition and stays out of the count: with
// it in, an open sky outweighs the whole scene and "near" comes out as
// everything that is not sky.
inline float splitOf(std::vector<double> histogram)
{
    const int bins = static_cast<int>(histogram.size());
    if (bins < 2) {
        return 0.5f;
    }
    const int farEnd = static_cast<int>(kFarEnd * bins);
    double count = 0.0, body = 0.0;
    for (int i = 0; i < bins; ++i) {
        count += histogram[i];
        if (i < farEnd) {
            body += histogram[i];
        }
    }
    if (count < 1.0) {
        return 0.5f;
    }
    if (body >= 0.05 * count) {
        std::fill(histogram.begin() + farEnd, histogram.end(), 0.0);
        count = body;
    }
    double total = 0.0;
    for (int i = 0; i < bins; ++i) {
        total += i * histogram[i];
    }
    // Across an empty stretch between two groups every cut scores the same;
    // the split goes in the middle of it, clear of both.
    double below = 0.0, belowSum = 0.0, bestVariance = -1.0;
    int split = bins / 2, lastBest = bins / 2;
    for (int i = 0; i < bins - 1; ++i) {
        below += histogram[i];
        belowSum += i * histogram[i];
        const double above = count - below;
        if (below <= 0.0 || above <= 0.0) {
            continue;
        }
        const double meanBelow = belowSum / below;
        const double meanAbove = (total - belowSum) / above;
        const double variance = below * above * (meanBelow - meanAbove) * (meanBelow - meanAbove);
        if (variance > bestVariance * (1.0 + 1e-9)) {
            bestVariance = variance;
            split = lastBest = i;
        } else if (variance >= bestVariance * (1.0 - 1e-9)) {
            lastBest = i;
        }
    }
    return ((split + lastBest) / 2.f + 1.f) / bins;
}

}
}
