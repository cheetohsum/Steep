#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace compositeui
{
struct DepthHistogram {
    static constexpr size_t bins = 128;
    std::array<double, bins> area{}, light{};
    size_t samples = 0;

    void add(float depth, float luminance)
    {
        if (!std::isfinite(depth) || depth < 0 || depth > 1) return;
        const size_t i = std::min(bins - 1, size_t(depth * bins));
        area[i] += 1;
        if (std::isfinite(luminance)) light[i] += std::max(0.f, luminance);
        ++samples;
    }
    std::array<double, bins> curve(bool brightness) const
    {
        const auto& input = brightness ? light : area;
        std::array<double, bins> result{};
        for (size_t i = 0; i < bins; ++i) {
            double weight = 0;
            for (int offset = -2; offset <= 2; ++offset) {
                const int j = int(i) + offset;
                if (j < 0 || j >= int(bins)) continue;
                const double w = 3 - std::abs(offset);
                result[i] += input[j] * w;
                weight += w;
            }
            result[i] /= weight;
        }
        const double peak = *std::max_element(result.begin(), result.end());
        if (peak > 0) for (auto& value : result) value /= peak;
        return result;
    }
};
} // namespace compositeui
