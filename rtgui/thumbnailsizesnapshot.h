#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

// Published by thumbnail workers; layout must not wait for RAW processing.
class ThumbnailSizeSnapshot
{
public:
    void publish(int width, int height, float preciseRatio = -1.f)
    {
        const double ratio = preciseRatio > 0.f ? preciseRatio
            : (height > 0 ? static_cast<double>(width) / height : 0.0);
        if (std::isfinite(ratio) && ratio > 0.0) {
            ratio_.store(ratio, std::memory_order_relaxed);
        }
    }

    void fit(int& width, int& height, int maxWidth) const
    {
        height = std::max(1, height);
        maxWidth = std::max(1, maxWidth);
        const double requested = ratio_.load(std::memory_order_relaxed) * height;
        if (requested > maxWidth) {
            height = std::max(1, static_cast<int>(height * (maxWidth / requested)));
            width = maxWidth;
        } else {
            width = std::max(1, static_cast<int>(requested));
        }
    }

private:
    std::atomic<double> ratio_{1.0};
};
