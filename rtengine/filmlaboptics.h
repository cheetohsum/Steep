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

// Scene luminance, in cd/m2, at which the sensor clipped, from the exposure the
// camera used: the saturation-based speed definition (H_sat = 78 / S lux s,
// H = 0.65 L t / N^2) gives 120 N^2 / (t S), and raw files keep about half a
// stop above that. 0 when the exposure is not known.
inline float clipLuminance(float fNumber, float shutterSeconds, float iso)
{
    if (!(fNumber > 0.f) || !(shutterSeconds > 0.f) || !(iso > 0.f)) {
        return 0.f;
    }
    return 170.f * fNumber * fNumber / (shutterSeconds * iso);
}

// How many stops above the clip a burnt-out light is taken to be. Lamps,
// signs and lit windows sit in the thousands of cd/m2 whatever the exposure,
// so the darker the scene the camera exposed for, the further over they are:
// half a stop in daylight (where what clips is mostly white cloud and
// paint), three at night - well short of what such lights measure, on
// purpose: it is a guess, and an over-estimate is far uglier than an
// under-estimate. One and a half when the exposure is unknown. `amount` is the
// Light sources slider: -100 takes the file as recorded, +100 doubles the
// stops.
inline float emitterStops(float clipLuminanceCdM2, int amount)
{
    constexpr float emitter = 5000.f;
    const float stops = clipLuminanceCdM2 > 0.f
        ? std::max(0.5f, std::min(3.f, std::log2(emitter / clipLuminanceCdM2)))
        : 1.5f;
    return stops * (1.f + std::max(-100, std::min(100, amount)) / 100.f);
}

// How sure a clipped area is a light and not an over-exposed surface, from
// what surrounds it: an area whose unclipped neighbours are within a stop of
// the clip is a wall or a sky running out of range (0); one standing four
// stops clear of everything near it is emitting (1).
inline float emitterConfidence(float surroundOverClip)
{
    const float stopsBelow = -std::log2(std::max(surroundOverClip, 1e-4f));
    const float t = std::max(0.f, std::min(1.f, (stopsBelow - 1.f) / 3.f));
    return t * t * (3.f - 2.f * t);
}

// The larger a clipped light is in the frame, the less of the estimate it
// gets: a point is an unresolved lamp, a broad disc is a frosted globe or a
// lit panel, whose surface is far less bright than a bare source, and its
// area already multiplies the light it sends into the base. `clippedShare`
// is the clipped fraction of the surroundings (4% of the short side).
inline float emitterSizeFactor(float clippedShare)
{
    return 1.f / (1.f + std::max(clippedShare, 0.f) / 0.1f);
}

// Emulsion sharpness. A film layer's modulation transfer is close to
// 1 / (1 + (f / f50)^2): it loses fine detail gently and keeps a long tail,
// unlike a Gaussian. Two Gaussians of equal weight, the second three times
// as wide, follow that curve within a few percent out to 2.5 f50.
// `f50` is the spatial frequency, in cycles/mm, the layer holds at 50%.
constexpr float emulsionSkirt = 3.f;

inline float emulsionSigmaMM(float f50)
{
    return 0.0994f / std::max(f50, 1.f);
}

inline float emulsionMTF(float cyclesPerMM, float f50)
{
    const float sigma = emulsionSigmaMM(f50);
    const float x = 2.f * 9.8696044f * sigma * sigma * cyclesPerMM * cyclesPerMM;
    return 0.5f * std::exp(-x) + 0.5f * std::exp(-x * emulsionSkirt * emulsionSkirt);
}

// The Film sharpness slider as a factor on f50: 0 keeps the stock's own,
// -100 halves it, and towards +100 it rises without bound (no loss).
inline float emulsionSharpnessScale(int amount)
{
    const float v = std::max(-100, std::min(100, amount)) / 100.f;
    return v < 0.f ? std::exp2(v) : 1.f / std::max(1.f - v, 1e-3f);
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
