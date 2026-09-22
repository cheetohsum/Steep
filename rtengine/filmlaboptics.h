#pragma once

#include <algorithm>
#include <cmath>

namespace rtengine {
namespace filmoptics {

// Small reserve for display conversion and mixing with an already-clipped base.
constexpr float printWhite = 0.99f;

inline float sizeMultiplier(int size)
{
    return std::exp2(std::max(-100, std::min(100, size)) / 100.f * 1.35f);
}

inline float highlightWeight(float light, float onset)
{
    const float x = std::max(light, 0.f) / std::max(onset, 0.001f);
    // Continuous highlight emphasis, with no discontinuity at display white.
    return x / (1.f + x);
}

inline float printHighlightGain(float displayLuma)
{
    constexpr float knee = 0.72f;
    if (displayLuma <= knee) {
        return 1.f;
    }
    // Value/slope continuous at the knee, asymptotic to display white.
    const float headroom = printWhite - knee;
    const float excess = displayLuma - knee;
    const float mapped = knee + headroom * (excess / (headroom + excess));
    return mapped / displayLuma;
}

inline void mapPrintHighlights(float& r, float& g, float& b)
{
    // Linear sRGB. Roll off luminance, then smoothly fit chroma into its
    // remaining headroom. Peak-only scaling locks hot lights into colored
    // plateaus; channel-by-channel clipping instead creates hue reversals.
    const float y = std::max(0.f, 0.2126729f * r + 0.7151522f * g + 0.0721750f * b);
    if (y <= 1e-6f) {
        r = g = b = 0.f;
        return;
    }
    const float gain = printHighlightGain(y);
    r *= gain;
    g *= gain;
    b *= gain;
    const float mappedY = y * gain;
    const float high = std::max(r, std::max(g, b));
    const float low = std::min(r, std::min(g, b));
    const float extent = std::max((high - mappedY) / std::max(printWhite - mappedY, 1e-6f),
                                  (mappedY - low) / std::max(mappedY, 1e-6f));
    if (extent > 0.85f) {
        const float compressed = 0.85f + 0.15f * (1.f - std::exp(-(extent - 0.85f) / 0.15f));
        const float chromaScale = compressed / extent;
        r = mappedY + (r - mappedY) * chromaScale;
        g = mappedY + (g - mappedY) * chromaScale;
        b = mappedY + (b - mappedY) * chromaScale;
    }
}

inline int supportPixels(int fullShort, float frameShortMM, int size, int scale)
{
    // Four sigma of the widest optical kernel, plus resampling support.
    return static_cast<int>(std::ceil(4.f * 0.38f * fullShort / frameShortMM
        * sizeMultiplier(size) / std::max(scale, 1))) + 4;
}

} // namespace filmoptics
} // namespace rtengine
