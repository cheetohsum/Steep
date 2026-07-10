/*
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "array2D.h"
#include "boxblur.h"
#include "color.h"
#include "iccstore.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "rt_math.h"

namespace rtengine
{

namespace
{

struct FilmRecipe {
    const char* id;
    float redGain, greenGain, blueGain;       // Per-channel sensitivity (multiplier)
    float redGamma, greenGamma, blueGamma;    // Per-channel contrast (S-curve power)
    float dye[3][3];                          // Dye coupling matrix
    float masterContrast;                     // Overall S-curve steepness (1.0=linear)
    float baseFog;                            // Black point lift (0-1 range, 0.05 = visible)
    float shoulder;                           // Highlight compression (0-1 range)
    float hueSat[6];                          // Hue-dependent saturation [R,Y,G,C,B,M]
    float shadowHue, shadowTint;              // Shadow tinting (hue in degrees, tint 0-1)
    float highlightHue, highlightTint;        // Highlight tinting
    float halation;                           // Halation glow strength (0-1)
    float warmth, tint;                       // Global color balance shifts
    float grain;                              // Film grain strength (0-1)
    float vibrance;                           // Vibrance: boost low-saturation more (-1..1)
};

struct LumaWeights {
    float r;
    float g;
    float b;
};

inline LumaWeights makeLumaWeights(const float wp[3][3])
{
    const float sum = wp[1][0] + wp[1][1] + wp[1][2];
    if (std::fabs(sum) > 1e-6f) {
        const LumaWeights weights = {wp[1][0] / sum, wp[1][1] / sum, wp[1][2] / sum};
        return weights;
    }

    const LumaWeights fallback = {0.2126f, 0.7152f, 0.0722f};
    return fallback;
}

inline float rgbLuminance(float r, float g, float b, const LumaWeights& weights)
{
    return weights.r * r + weights.g * g + weights.b * b;
}

// Soft-clip: C1-continuous shoulder curve with knee at 0.9.
// Linear below knee, rational roll-off above, asymptotically approaches 1.0.
inline float softClip(float x)
{
    if (x <= 0.f) return 0.f;
    const float knee = 0.9f;
    if (x <= knee) return x;
    float t = x - knee;
    float range = 1.f - knee;  // 0.1
    return knee + range * t / (t + range);
}

inline float printDensityCoordinate(float lum)
{
    // Keep tonal decisions in a soft print shoulder instead of hard-clipping
    // every value at display white; this leaves room for highlight texture,
    // grain, and chroma convergence to roll off gradually.
    return softClip(std::max(lum, 0.f));
}

// Film H&D characteristic curve with base fog, S-curve contrast, and shoulder
inline float filmCurve(float x, float gamma, float baseFog, float shoulder)
{
    // Filmic toe/shoulder curve. Preserves scene highlight headroom.
    x = std::max(x, 0.f);

    // Gentle toe: film responds slowly near the exposure floor. Keep the
    // neutral custom curve neutral unless a recipe asks for film behavior.
    const float curveIntent = LIM(shoulder * 2.f + baseFog * 8.f + std::fabs(gamma - 1.f) * 0.5f, 0.f, 1.f);
    if (curveIntent > 0.001f) {
        const float toe = (0.018f + baseFog * 0.25f) * curveIntent;
        x = (x * (x + toe)) / (x + toe + toe * 0.75f);
    }

    // Power-function S-curve for film contrast
    // gamma > 1 = more contrast, gamma < 1 = flatter
    if ((gamma > 1.001f || gamma < 0.999f) && x < 1.f) {
        if (x < 0.5f) {
            float t = x * 2.f;
            x = 0.5f * powf(t, gamma);
        } else {
            float t = (1.f - x) * 2.f;
            x = 1.f - 0.5f * powf(t, gamma);
        }
    }

    // Shoulder: scene-referred highlight compression controlled by rolloff,
    // asymptotically approaching display white after contrast shaping.
    if (shoulder > 0.01f) {
        const float rolloff = LIM(shoulder, 0.f, 1.f);
        const float knee = 0.92f - rolloff * 0.28f;
        if (x > knee && knee < 0.999f) {
            const float t = (x - knee) / (1.f - knee);
            const float scale = 0.65f + rolloff * 1.1f;
            x = knee + (1.f - knee) * (1.f - expf(-t / scale));
        }
    } else if (x > 1.f) {
        x = 1.f;
    }

    // Base fog: lift the final black point after contrast shaping.
    if (baseFog > 0.0001f) {
        x = x * (1.f - baseFog) + baseFog;
    }

    return LIM(x, 0.f, 1.f);
}

inline float masterContrastCurve(float x, float gamma)
{
    if (x <= 1.f) {
        return filmCurve(x, gamma, 0.f, 0.f);
    }

    return x;
}

// Smooth hermite interpolation
inline float smoothstep(float edge0, float edge1, float x)
{
    float t = LIM((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

inline float chromaUpperBound(float lum, float r, float g, float b)
{
    return std::max(1.f, std::max(lum, std::max(r, std::max(g, b))));
}

inline float fitChromaScale(float lum, float r, float g, float b, float scale, float upper)
{
    if (scale <= 0.f) {
        return 0.f;
    }

    float maxScale = scale;
    const float channels[3] = {r, g, b};

    for (float channel : channels) {
        const float delta = channel - lum;
        if (delta > 1e-6f) {
            maxScale = std::min(maxScale, (upper - lum) / delta);
        } else if (delta < -1e-6f) {
            maxScale = std::min(maxScale, lum / -delta);
        }
    }

    return std::max(maxScale, 0.f);
}

inline void fitRgbToLuminance(float targetLum, float& r, float& g, float& b, const LumaWeights& weights)
{
    targetLum = std::max(targetLum, 0.f);
    const float currentLum = rgbLuminance(r, g, b, weights);
    const float lumaLift = targetLum - currentLum;
    r += lumaLift;
    g += lumaLift;
    b += lumaLift;

    const float upper = chromaUpperBound(targetLum, r, g, b);
    const float chromaScale = fitChromaScale(targetLum, r, g, b, 1.f, upper);

    r = LIM(targetLum + (r - targetLum) * chromaScale, 0.f, upper);
    g = LIM(targetLum + (g - targetLum) * chromaScale, 0.f, upper);
    b = LIM(targetLum + (b - targetLum) * chromaScale, 0.f, upper);
}

inline void scaleRgbChroma(float lum, float& r, float& g, float& b, float scale)
{
    lum = std::max(lum, 0.f);
    const float upper = chromaUpperBound(lum, r, g, b);
    scale = fitChromaScale(lum, r, g, b, scale, upper);
    r = LIM(lum + (r - lum) * scale, 0.f, upper);
    g = LIM(lum + (g - lum) * scale, 0.f, upper);
    b = LIM(lum + (b - lum) * scale, 0.f, upper);
}

inline float rgbSaturation(float r, float g, float b)
{
    const float maxChannel = std::max(r, std::max(g, b));
    const float minChannel = std::min(r, std::min(g, b));

    return (maxChannel - minChannel) / (maxChannel + 1e-6f);
}

inline float rgbChroma(float r, float g, float b)
{
    const float maxChannel = std::max(r, std::max(g, b));
    const float minChannel = std::min(r, std::min(g, b));

    return maxChannel - minChannel;
}

inline float neutralAxisWeight(float r, float g, float b)
{
    const float relativeSat = rgbSaturation(r, g, b);
    const float absoluteChroma = rgbChroma(r, g, b);
    const float neutralMetric = std::min(relativeSat, absoluteChroma * 16.f);

    return 1.f - smoothstep(0.006f, 0.075f, neutralMetric);
}

inline void applyLogDensityColorBias(float& r, float& g, float& b, float dr, float dg, float db, const LumaWeights& weights)
{
    const float targetLum = std::max(rgbLuminance(r, g, b, weights), 0.f);
    const float toe = 0.006f;

    r = std::max(std::exp(std::log(std::max(r, 0.f) + toe) + dr) - toe, 0.f);
    g = std::max(std::exp(std::log(std::max(g, 0.f) + toe) + dg) - toe, 0.f);
    b = std::max(std::exp(std::log(std::max(b, 0.f) + toe) + db) - toe, 0.f);
    fitRgbToLuminance(targetLum, r, g, b, weights);
}

inline float wrapHueDegrees(float hue)
{
    hue = std::fmod(hue, 360.f);
    return hue < 0.f ? hue + 360.f : hue;
}

inline float circularHueDistance(float h, float center)
{
    float d = std::fabs(h - center);
    return std::min(d, 1.f - d);
}

inline float resolveRecipeHue(float recipeHue, int userHue, float neutralUiHue)
{
    const float hue = wrapHueDegrees(static_cast<float>(userHue));
    float defaultDistance = std::fabs(hue - neutralUiHue);
    defaultDistance = std::min(defaultDistance, 360.f - defaultDistance);

    return defaultDistance > 0.5f ? hue : wrapHueDegrees(recipeHue);
}

inline float sliderPercent(int value)
{
    return LIM(static_cast<float>(value) / 100.f, -1.f, 1.f);
}

inline float sliderUnit(int value)
{
    return LIM(static_cast<float>(value) / 100.f, 0.f, 1.f);
}

inline void applyDensityDyeCoupling(const float dye[3][3], float& r, float& g, float& b, const LumaWeights& weights)
{
    const float targetLum = std::max(rgbLuminance(r, g, b, weights), 0.f);
    const float toe = 0.006f;
    const float logLum = std::log(targetLum + toe);

    const float dR = std::log(std::max(r, 0.f) + toe) - logLum;
    const float dG = std::log(std::max(g, 0.f) + toe) - logLum;
    const float dB = std::log(std::max(b, 0.f) + toe) - logLum;

    const float mR = dye[0][0] * dR + dye[0][1] * dG + dye[0][2] * dB;
    const float mG = dye[1][0] * dR + dye[1][1] * dG + dye[1][2] * dB;
    const float mB = dye[2][0] * dR + dye[2][1] * dG + dye[2][2] * dB;

    r = std::max(std::exp(logLum + mR) - toe, 0.f);
    g = std::max(std::exp(logLum + mG) - toe, 0.f);
    b = std::max(std::exp(logLum + mB) - toe, 0.f);
    fitRgbToLuminance(targetLum, r, g, b, weights);
}

inline void applyPrintDyeGamutLimit(float& r, float& g, float& b, float amount, const LumaWeights& weights)
{
    amount = LIM(amount, 0.f, 1.f);
    if (amount <= 0.0001f) {
        return;
    }

    const float targetLum = std::max(rgbLuminance(r, g, b, weights), 0.f);
    const float maxChannel = std::max(r, std::max(g, b));
    const float minChannel = std::min(r, std::min(g, b));
    if (maxChannel - minChannel <= 1e-5f) {
        return;
    }

    const float redPeak = smoothstep(0.06f, 0.48f, r - std::max(g, b));
    const float greenPeak = smoothstep(0.06f, 0.48f, g - std::max(r, b));
    const float bluePeak = smoothstep(0.06f, 0.48f, b - std::max(r, g));
    const float yellowPeak = smoothstep(0.06f, 0.46f, std::min(r, g) - b);
    const float cyanPeak = smoothstep(0.06f, 0.46f, std::min(g, b) - r);
    const float magentaPeak = smoothstep(0.06f, 0.46f, std::min(r, b) - g);

    float printR = r;
    float printG = g;
    float printB = b;

    // Print dyes do not form perfectly clean RGB primaries. Let dominant
    // colors pick up controlled companion-dye contamination before the final
    // chroma clamp, so saturated subjects look chemically limited instead of
    // merely radially desaturated.
    printG += redPeak * (r - g) * 0.14f;
    printB += redPeak * (r - b) * 0.03f;

    printR += greenPeak * (g - r) * 0.07f;
    printB += greenPeak * (g - b) * 0.06f;

    printG += bluePeak * (b - g) * 0.15f;
    printR += bluePeak * (b - r) * 0.03f;

    const float yellowLevel = std::min(r, g);
    const float cyanLevel = std::min(g, b);
    const float magentaLevel = std::min(r, b);
    printB += yellowPeak * (yellowLevel - b) * 0.06f;
    printR += cyanPeak * (cyanLevel - r) * 0.04f;
    printG += magentaPeak * (magentaLevel - g) * 0.09f;

    // Hot, high-purity colors on print do not stay on ideal RGB axes. Add a
    // small shoulder-weighted hue skew toward neighboring dye families before
    // the luminance-preserving fit.
    const float hotPurity = smoothstep(0.74f, 1.12f, maxChannel) * smoothstep(0.48f, 0.92f, maxChannel - minChannel);
    printG += redPeak * hotPurity * (r - g) * 0.06f;
    printB += redPeak * hotPurity * (r - b) * 0.012f;

    printR += greenPeak * hotPurity * (g - r) * 0.08f;
    printB += greenPeak * hotPurity * (g - b) * 0.025f;

    printG += bluePeak * hotPurity * (b - g) * 0.07f;
    printR += bluePeak * hotPurity * (b - r) * 0.018f;

    printB += yellowPeak * hotPurity * (yellowLevel - b) * 0.025f;
    printR += cyanPeak * hotPurity * (cyanLevel - r) * 0.018f;
    printG += magentaPeak * hotPurity * (magentaLevel - g) * 0.022f;

    r = intp(amount, printR, r);
    g = intp(amount, printG, g);
    b = intp(amount, printB, b);
    fitRgbToLuminance(targetLum, r, g, b, weights);
}

inline float grainUniformNoise(int x, int y, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = h ^ (h >> 16);
    return (static_cast<float>(h & 0xFFFFu) / 65535.f) * 2.f - 1.f;
}

inline float softLimitSigned(float x)
{
    const float knee = 0.92f;
    const float ax = std::fabs(x);
    if (ax <= knee) {
        return x;
    }

    const float range = 1.f - knee;
    const float t = ax - knee;
    const float limited = knee + range * t / (t + range);
    return x < 0.f ? -limited : limited;
}

// Hash-based noise for film grain texture synthesis. Use a lightly
// bell-shaped distribution with small tails instead of flat uniform noise;
// real grain tends to form clustered density variation, not video-like speckle.
inline float grainNoise(int x, int y, uint32_t seed)
{
    const float a = grainUniformNoise(x, y, seed);
    const float b = grainUniformNoise(x + 37, y - 17, seed ^ 0xA511E9B3u);
    const float c = grainUniformNoise(x - 23, y + 41, seed ^ 0x63D83595u);
    const float clustered = (a + b + c) * 0.42f;
    const float tail = a * std::fabs(a) * 0.06f;

    return softLimitSigned(clustered + tail);
}

// ================================================================
// 17 preset recipes: custom + 16 film stocks
// Values are tuned for visible, controlled differences.
// ================================================================
static const FilmRecipe recipes[] = {
    // 0: Custom — neutral identity baseline
    // All processing stages are no-ops at default slider values
    {
        "custom",
        1.0f, 1.0f, 1.0f,       // gains: neutral
        1.0f, 1.0f, 1.0f,       // gammas: linear (no S-curve)
        {{ 1.0f, 0.0f, 0.0f},   // dye: identity matrix
         { 0.0f, 1.0f, 0.0f},
         { 0.0f, 0.0f, 1.0f}},
        1.0f,                    // masterContrast: linear
        0.0f,                    // baseFog: none
        0.0f,                    // shoulder: none
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, // hueSat: uniform
        220.f, 0.0f,             // shadow: no tint
        40.f, 0.0f,              // highlight: no tint
        0.0f,                    // halation: none
        0.0f, 0.0f,             // warmth/tint: none
        0.0f,                    // grain: none
        0.0f,                    // vibrance: neutral
    },

    // 1: Heritage Gold — Kodachrome K-14
    // Deep saturated reds, warm golden highlights, blue-tinted shadows
    // Minimal halation (excellent anti-halation backing)
    {
        "heritage_gold",
        1.12f, 0.96f, 0.88f,    // Red boost, green/blue slightly suppressed
        1.15f, 1.06f, 0.94f,    // Red channel steeper, blue flatter
        {{ 1.10f,  0.03f, -0.06f},  // Cyan dye: red with slight blue absorption
         {-0.02f,  1.06f,  0.01f},  // Magenta dye: pure green
         {-0.03f,  0.01f,  1.08f}}, // Yellow dye: good blue
        1.25f,                   // Strong S-curve contrast
        0.008f,                  // Minimal base fog (clean blacks)
        0.28f,                   // Moderate shoulder (highlight roll-off)
        {1.35f, 1.20f, 0.88f, 0.90f, 1.10f, 1.06f}, // Reds punchy, greens muted
        220.f, 0.18f,            // Blue-purple shadow tint
        38.f, 0.22f,             // Warm golden highlight tint
        0.03f,                   // Minimal halation
        0.10f, -0.02f,          // Warm bias, slight green tint
        0.15f,                   // Fine grain (K-14 process)
        0.30f,                   // Strong vibrance (Kodachrome signature)
    },

    // 2: Porcelain 400 — Portra C-41
    // Soft, low contrast, beautiful skin tones, wide latitude
    {
        "porcelain_400",
        1.04f, 1.00f, 0.95f,    // Slight warm bias
        0.88f, 0.85f, 0.83f,    // Low gamma = flat/soft contrast
        {{ 1.03f,  0.02f, -0.02f},
         {-0.01f,  1.02f,  0.01f},
         { 0.01f, -0.01f,  1.04f}},
        0.85f,                   // Low contrast (sub-linear = flat)
        0.04f,                   // Noticeable base fog (lifted shadows)
        0.55f,                   // Strong shoulder (very soft highlights)
        {1.06f, 1.04f, 1.00f, 0.96f, 0.98f, 1.03f}, // Subtle, flattering
        30.f, 0.10f,             // Warm shadow tint
        45.f, 0.14f,             // Warm highlight tint
        0.03f,                   // Slight halation
        0.06f, 0.02f,           // Warm, slight magenta
        0.20f,                   // Visible but fine grain
        0.15f,                   // Gentle vibrance (Portra is subtle)
    },

    // 3: Vivid Chrome — Velvia E-6
    // Maximum saturation, punchy contrast, deep blacks
    {
        "vivid_chrome",
        1.10f, 1.05f, 1.08f,    // All channels modestly boosted
        1.30f, 1.22f, 1.25f,    // Steep gamma = high contrast
        {{ 1.12f,  0.04f, -0.03f},  // Moderate diagonal boost
         {-0.02f,  1.10f, -0.01f},
         {-0.03f,  0.01f,  1.11f}},
        1.35f,                   // Steep master contrast
        0.003f,                  // Almost no base fog (deep blacks)
        0.18f,                   // Modest shoulder
        {1.40f, 1.30f, 1.25f, 1.22f, 1.35f, 1.30f}, // All hues well saturated
        240.f, 0.12f,            // Cool shadow
        35.f, 0.08f,             // Warm highlight
        0.02f,                   // Minimal halation
        0.03f, -0.01f,          // Slightly warm
        0.10f,                   // Very fine grain (Velvia)
        0.40f,                   // Maximum vibrance (Velvia signature)
    },

    // 4: Arctic — Ektachrome E100VS/E-6
    // Cool, vivid, blue/cyan emphasis
    {
        "arctic",
        0.90f, 0.97f, 1.14f,    // Blue boosted, red suppressed
        1.12f, 1.08f, 1.05f,    // Moderate contrast
        {{ 1.00f, -0.03f,  0.04f},  // Cool cross-talk
         { 0.01f,  1.05f, -0.01f},
         { 0.04f,  0.02f,  1.10f}}, // Blue channel amplified
        1.20f,
        0.006f,
        0.24f,
        {0.92f, 0.85f, 1.10f, 1.35f, 1.38f, 1.15f}, // Cyan/blue boosted
        210.f, 0.25f,            // Cool shadow tint
        200.f, 0.15f,            // Cool highlight tint
        0.01f,
        -0.14f, 0.03f,          // Cool, slight magenta
        0.12f,                   // Fine grain
        0.25f,                   // Good vibrance
    },

    // 5: Sovereign — Provia E-6
    // Balanced, natural but vivid, professional
    {
        "sovereign",
        1.04f, 1.00f, 1.02f,    // Nearly neutral
        1.12f, 1.08f, 1.10f,    // Moderate S-curve
        {{ 1.06f,  0.02f, -0.01f},
         {-0.01f,  1.04f,  0.01f},
         { 0.01f, -0.01f,  1.07f}},
        1.18f,
        0.005f,
        0.25f,
        {1.20f, 1.14f, 1.08f, 1.10f, 1.18f, 1.14f}, // Moderate across all hues
        225.f, 0.12f,
        38.f, 0.10f,
        0.02f,
        0.02f, 0.00f,
        0.10f,                   // Fine grain (Provia)
        0.20f,                   // Balanced vibrance
    },

    // 6: Golden Hour — Ektar C-41
    // Very warm, saturated reds/yellows, cool blue shadows
    {
        "golden_hour",
        1.18f, 1.02f, 0.82f,    // Strong warm bias
        1.20f, 1.08f, 0.90f,    // Red steeper, blue flatter
        {{ 1.12f,  0.06f, -0.05f},  // Warm cross-talk
         {-0.01f,  1.05f, -0.02f},
         {-0.06f, -0.02f,  1.02f}}, // Blue channel slightly weakened
        1.25f,
        0.01f,
        0.28f,
        {1.35f, 1.30f, 0.82f, 0.85f, 0.90f, 1.10f}, // Reds/yellows punchy, greens muted
        215.f, 0.15f,            // Cool shadow
        32.f, 0.30f,             // Warm golden highlights
        0.05f,
        0.18f, -0.03f,          // Very warm
        0.12f,                   // Fine grain (Ektar)
        0.30f,                   // Strong vibrance
    },

    // 7: Twilight 160 — Gold 200 C-41
    // Warm amber bias, gentle contrast, nostalgic consumer film feel
    {
        "twilight_160",
        1.10f, 1.01f, 0.90f,    // Warm
        1.04f, 1.00f, 0.94f,    // Mild S-curve
        {{ 1.07f,  0.04f, -0.03f},
         { 0.01f,  1.04f,  0.01f},
         {-0.03f,  0.01f,  1.05f}},
        1.08f,
        0.03f,                   // Visible base fog (consumer film)
        0.38f,                   // Soft highlights
        {1.15f, 1.12f, 0.92f, 0.90f, 0.96f, 1.06f},
        35.f, 0.14f,             // Warm shadow
        48.f, 0.22f,             // Amber highlight
        0.04f,
        0.14f, 0.01f,
        0.18f,                   // Visible grain (consumer film)
        0.15f,                   // Moderate vibrance
    },

    // 8: Nostalgia 200 — Superia C-41
    // Green-shifted, slightly desaturated, warm base fog
    {
        "nostalgia_200",
        0.96f, 1.08f, 0.92f,    // Green channel dominant
        1.02f, 1.06f, 0.96f,
        {{ 1.03f,  0.03f, -0.01f},
         { 0.03f,  1.08f, -0.01f},  // Green dye amplified
         {-0.01f,  0.03f,  1.02f}},
        1.06f,
        0.035f,                  // Warm base fog
        0.32f,
        {1.00f, 0.96f, 1.12f, 0.92f, 0.90f, 0.96f}, // Greens boosted
        140.f, 0.14f,            // Green-ish shadow tint
        50.f, 0.10f,
        0.04f,
        0.08f, 0.04f,           // Warm with magenta tint
        0.22f,                   // Visible grain (Superia)
        0.10f,                   // Mild vibrance
    },

    // 9: Desert Chrome — Agfa Vista C-41
    // Orange/teal split, warm shadows, vivid
    {
        "desert_chrome",
        1.14f, 0.97f, 0.86f,    // Warm bias
        1.12f, 1.04f, 0.96f,
        {{ 1.10f,  0.05f, -0.05f},  // Orange push
         {-0.02f,  1.04f,  0.03f},
         {-0.04f, -0.01f,  1.07f}},
        1.18f,
        0.01f,
        0.28f,
        {1.30f, 1.25f, 0.86f, 0.95f, 1.14f, 1.08f}, // Orange/blue contrast
        30.f, 0.18f,             // Warm shadow
        28.f, 0.25f,             // Warm highlight
        0.06f,
        0.16f, -0.02f,
        0.20f,                   // Moderate grain (Agfa Vista)
        0.25f,                   // Good vibrance
    },

    // 10: Street 800 — Pushed Tri-X
    // High contrast near-monochrome, gritty, green shadow tint
    {
        "street_800",
        1.00f, 1.00f, 1.00f,    // Neutral gains (B&W doesn't care)
        1.35f, 1.35f, 1.35f,    // Steep gamma = hard contrast
        {{ 1.00f,  0.00f,  0.00f},   // Identity (desaturation via hueSat)
         { 0.00f,  1.00f,  0.00f},
         { 0.00f,  0.00f,  1.00f}},
        1.45f,                   // High contrast
        0.018f,                  // Slight base fog
        0.12f,                   // Hard highlights (minimal shoulder)
        {0.12f, 0.10f, 0.08f, 0.08f, 0.10f, 0.10f}, // Nearly monochrome
        130.f, 0.12f,            // Green-grey shadow tint
        50.f, 0.04f,
        0.02f,
        0.0f, 0.02f,            // Slight magenta
        0.45f,                   // Heavy grain (pushed Tri-X)
        0.0f,                    // N/A (near-monochrome)
    },

    // 11: Cinematic 500T — Cinestill ECN-2
    // Tungsten-balanced, teal shadows, warm halation glow
    {
        "cinematic_500t",
        0.86f, 0.96f, 1.14f,    // Blue/teal bias (tungsten film)
        1.10f, 1.06f, 1.00f,
        {{ 1.04f,  0.02f,  0.03f},
         { 0.01f,  1.06f,  0.00f},
         { 0.04f,  0.02f,  1.10f}},  // Blue cross-talk
        1.15f,
        0.012f,
        0.28f,
        {1.08f, 0.92f, 0.90f, 1.25f, 1.20f, 1.05f}, // Teal/cyan boosted, warm muted
        185.f, 0.30f,            // Teal shadow tint
        30.f, 0.20f,             // Warm highlight
        0.45f,                   // Strong halation (anti-halation layer removed!)
        -0.10f, 0.04f,          // Cool, slight magenta
        0.18f,                   // Moderate grain (Cinestill 500T)
        0.20f,                   // Decent vibrance
    },

    // 12: Fade & Bloom — Expired film
    // Degraded dyes, heavy base fog, magenta shift, low contrast
    {
        "fade_bloom",
        0.92f, 0.88f, 0.96f,    // Degraded, slight blue bias
        0.82f, 0.80f, 0.84f,    // Very flat (low gamma)
        {{ 0.90f,  0.06f,  0.05f},  // Degraded dye matrix (cross-contamination)
         { 0.05f,  0.88f,  0.05f},
         { 0.03f,  0.06f,  0.92f}},
        0.78f,                   // Low contrast
        0.08f,                   // Heavy base fog (lifted shadows)
        0.58f,                   // Very soft highlights
        {0.85f, 0.80f, 0.75f, 0.76f, 0.82f, 0.86f}, // Desaturated across board
        320.f, 0.20f,            // Magenta shadow tint
        50.f, 0.14f,
        0.10f,                   // Noticeable halation
        0.05f, 0.08f,           // Slight warm, magenta
        0.30f,                   // Heavy grain (expired film)
        -0.10f,                  // Reduced vibrance (degraded dyes)
    },

    // 13: Ember — Cross-processed (E-6 film in C-41 chemistry)
    // Wild color shifts, high contrast, psychedelic
    {
        "ember",
        1.18f, 0.82f, 1.15f,    // Red and blue up, green crushed
        1.25f, 0.85f, 1.22f,    // Red/blue steep, green flat
        {{ 1.12f,  0.10f, -0.06f},  // Cross-talk (wrong chemistry!)
         {-0.12f,  0.94f,  0.10f},
         { 0.06f, -0.10f,  1.14f}},
        1.30f,
        0.015f,
        0.20f,
        {1.40f, 1.10f, 1.30f, 0.78f, 1.22f, 1.35f}, // Wild hue-dependent sat
        280.f, 0.22f,            // Purple shadow
        55.f, 0.25f,             // Warm/amber highlight
        0.06f,
        0.08f, 0.06f,           // Warm, magenta
        0.25f,                   // Visible grain (cross-process)
        0.30f,                   // Strong vibrance (wild colors)
    },

    // 14: Silver Gelatin — Classic B&W silver print
    // Near-monochrome with subtle silver-blue tone
    {
        "silver_gelatin",
        1.03f, 1.00f, 1.06f,    // Slight blue sensitivity (ortho)
        1.25f, 1.25f, 1.25f,    // Strong S-curve (classic print contrast)
        {{ 1.00f,  0.00f,  0.00f},
         { 0.00f,  1.00f,  0.00f},
         { 0.00f,  0.00f,  1.00f}},
        1.30f,
        0.01f,
        0.22f,
        {0.06f, 0.04f, 0.03f, 0.04f, 0.08f, 0.05f}, // Extreme desaturation
        220.f, 0.06f,            // Subtle cool shadow
        40.f, 0.03f,             // Very subtle warm highlight
        0.01f,
        -0.02f, 0.04f,          // Cool, slight magenta (silver tone)
        0.35f,                   // Heavy grain (classic B&W print)
        0.0f,                    // N/A (near-monochrome)
    },

    // 15: Analog Dream — Lo-fi, dreamy, vintage
    // Warm, soft, hazy, ethereal
    {
        "analog_dream",
        1.08f, 1.00f, 0.92f,    // Warm
        0.90f, 0.87f, 0.84f,    // Very soft/flat
        {{ 1.04f,  0.03f,  0.01f},
         { 0.02f,  1.03f,  0.02f},
         { 0.01f,  0.03f,  1.04f}},
        0.88f,                   // Low contrast (dreamy)
        0.06f,                   // High base fog (hazy shadows)
        0.48f,                   // Strong shoulder (soft highlights)
        {1.08f, 1.06f, 0.90f, 0.88f, 0.96f, 1.03f},
        30.f, 0.16f,             // Warm shadow
        48.f, 0.22f,             // Warm highlight
        0.15f,                   // Noticeable halation (dreamy glow)
        0.12f, 0.02f,           // Warm
        0.28f,                   // Heavy grain (lo-fi)
        0.10f,                   // Mild vibrance
    },

    // 16: Cinema Reveal 35 — restrained modern 35mm print
    // Inspired by wide-gamut/log creative transforms: neutral color tracking,
    // soft highlight latitude, and gentle print warmth without heavy nostalgia.
    {
        "cinema_reveal_35",
        1.03f, 1.00f, 0.98f,    // Slight warm print bias, close to neutral
        1.08f, 1.04f, 1.02f,    // Mild layer contrast separation
        {{ 1.04f,  0.02f, -0.02f},
         { 0.00f,  1.03f,  0.01f},
         {-0.01f,  0.01f,  1.04f}},
        1.10f,                   // Polished but not crunchy
        0.006f,                  // Clean modern black floor
        0.42f,                   // Strong latitude with soft display rolloff
        {1.08f, 1.05f, 0.96f, 0.98f, 1.02f, 1.03f}, // Natural hues, tame greens
        220.f, 0.08f,            // Subtle cool shadow separation
        42.f, 0.12f,             // Warm print highlights
        0.04f,                   // Controlled halation
        0.04f, 0.00f,           // Gentle warmth, neutral tint
        0.12f,                   // Fine-grained modern scan feel
        0.08f,                   // Restrained vibrance
    },
};

static const int NUM_RECIPES = sizeof(recipes) / sizeof(recipes[0]);

const FilmRecipe* lookupRecipe(const Glib::ustring& preset)
{
    for (int i = 0; i < NUM_RECIPES; ++i) {
        if (preset == recipes[i].id) {
            return &recipes[i];
        }
    }
    return &recipes[0]; // fallback to custom
}

bool hasNeutralFilmControls(const procparams::FilmPresetsParams& fp)
{
    return fp.contrast == 0
        && fp.saturation == 0
        && fp.warmth == 0
        && fp.tint == 0
        && fp.fade <= 0
        && fp.rolloff <= 0
        && fp.shadowTint <= 0
        && fp.highlightTint <= 0
        && fp.halation <= 0
        && fp.redShift == 0
        && fp.greenShift == 0
        && fp.blueShift == 0
        && fp.grain <= 0
        && fp.vibrance == 0;
}

} // namespace


void ImProcFunctions::filmPresetsV1(LabImage *lab, const procparams::FilmPresetsParams &fp)
{
    if (!fp.enabled) {
        return;
    }

    const FilmRecipe* recipe = lookupRecipe(fp.preset);
    const bool customRecipe = recipe == &recipes[0];

    // User modifiers applied on top of recipe base values.
    // Multipliers keep the controls responsive, then effective values are
    // bounded below so extreme settings still resemble film/print behavior.
    const float userStrength = sliderUnit(fp.strength);
    if (userStrength <= 0.f) {
        return;
    }

    if (customRecipe && hasNeutralFilmControls(fp)) {
        return;
    }

    // Contrast: slider 0 = recipe default, slider +/-100 = +/-0.6 gamma shift
    const float userContrast = sliderPercent(fp.contrast);

    // Saturation: slider 0 = 1x recipe, slider 100 = 2x, slider -100 = 0x
    const float userSatScale = 1.f + sliderPercent(fp.saturation);

    // Warmth/tint: slider +/-100 = large visible color shift
    const float userWarmth = sliderPercent(fp.warmth);
    const float userTint = sliderPercent(fp.tint);

    // Fade: slider -100..100 trims or adds base fog around the recipe.
    const float userFade = sliderPercent(fp.fade);

    // Rolloff: slider -100..100 trims or adds shoulder around the recipe.
    const float userRolloff = sliderPercent(fp.rolloff);

    // Halation: slider -100..100 trims or adds glow around the recipe.
    const float userHalation = sliderPercent(fp.halation);

    // Channel shifts: slider +/-100 = +/-0.5 gain change
    const float userRedShift = sliderPercent(fp.redShift);
    const float userGreenShift = sliderPercent(fp.greenShift);
    const float userBlueShift = sliderPercent(fp.blueShift);

    // Grain: slider -100..100 trims or adds texture around the recipe.
    const float userGrain = sliderPercent(fp.grain);

    // Vibrance: slider -100..100 maps to -1..1
    const float userVibrance = sliderPercent(fp.vibrance);

    // === Compute effective parameters ===

    // Per-channel gains
    const float rGain = LIM(recipe->redGain + userRedShift * 0.5f, 0.25f, 1.75f);
    const float gGain = LIM(recipe->greenGain + userGreenShift * 0.5f, 0.25f, 1.75f);
    const float bGain = LIM(recipe->blueGain + userBlueShift * 0.5f, 0.25f, 1.75f);

    // Per-channel gammas (use recipe values directly — user adjusts via masterContrast)
    const float rGamma = recipe->redGamma;
    const float gGamma = recipe->greenGamma;
    const float bGamma = recipe->blueGamma;

    // Master contrast: recipe + user adjustment. Keep the printable density
    // curve away from synthetic extremes while preserving the full recipe range.
    const float masterContrast = LIM(recipe->masterContrast + userContrast * 0.6f, 0.35f, 1.85f);

    // Base fog: recipe + user fade. Split it across the emulsion response
    // and final base density so we do not double-apply the full lift.
    const float baseFog = LIM(recipe->baseFog + userFade * 0.15f, 0.f, 0.35f);
    const float curveFog = baseFog * 0.55f;
    const float printFog = baseFog * 0.45f;

    // Shoulder: recipe + user rolloff
    const float shoulder = LIM(recipe->shoulder + userRolloff * 0.8f, 0.f, 1.f);

    // Halation: recipe plus user trim/boost
    const float halation = LIM(recipe->halation + userHalation * 0.8f, 0.f, 1.f);

    // Hue-dependent saturation: recipe values scaled by user
    float hueSat[6];
    float meanHueSat = 0.f;
    for (int i = 0; i < 6; ++i) {
        hueSat[i] = recipe->hueSat[i] * userSatScale;
        meanHueSat += hueSat[i];
    }
    meanHueSat *= (1.f / 6.f);
    const float colorGrainIntent = smoothstep(0.22f, 0.85f, meanHueSat);
    const float monochromeIntent = 1.f - smoothstep(0.18f, 0.55f, meanHueSat);

    // Shadow/highlight tinting: recipe plus user trim/boost. Keep recipe hues
    // when users only trim/boost tint strength; use the hue sliders once they
    // move away from the neutral UI defaults.
    const float shHueDeg = resolveRecipeHue(recipe->shadowHue, fp.shadowHue, 220.f);
    const float shHue = shHueDeg * rtengine::RT_PI_F / 180.f;
    const float shTint = LIM(recipe->shadowTint + sliderPercent(fp.shadowTint) * 0.5f, 0.f, 1.f);

    const float hlHueDeg = resolveRecipeHue(recipe->highlightHue, fp.highlightHue, 40.f);
    const float hlHue = hlHueDeg * rtengine::RT_PI_F / 180.f;
    const float hlTint = LIM(recipe->highlightTint + sliderPercent(fp.highlightTint) * 0.5f, 0.f, 1.f);

    // Warmth/tint: recipe + user (large multipliers for visible effect)
    const float totalWarmth = LIM(recipe->warmth + userWarmth * 0.5f, -0.65f, 0.70f);
    const float totalTint = LIM(recipe->tint + userTint * 0.5f, -0.65f, 0.65f);

    // Grain: recipe plus user trim/boost
    const float grain = LIM(recipe->grain + userGrain * 0.8f, 0.f, 1.f);
    const float grainCloudMix = 0.48f + 0.22f * smoothstep(0.16f, 0.56f, grain);
    const float grainMottleMix = 0.14f + 0.16f * smoothstep(0.22f, 0.70f, grain);

    // Vibrance: recipe + user adjustment
    const float vibrance = LIM(recipe->vibrance + userVibrance * 0.5f, -0.60f, 0.90f);

    // Film/print density intent: used to make chroma compression appear only
    // when tone/color response asks for film behavior. Grain by itself should
    // remain texture-only so Custom + Grain does not become a hidden LUT.
    const float densityIntent = LIM(shoulder + baseFog * 4.f + std::fabs(masterContrast - 1.f) * 0.6f, 0.f, 1.f);
    const float textureAcutanceBase = LIM(
        (masterContrast - 1.f) * 0.035f
        + grain * 0.055f
        + densityIntent * 0.015f
        - halation * 0.050f
        - baseFog * 0.45f
        - shoulder * 0.010f,
        -0.055f,
        0.075f);
    const float textureMidAcutanceBase = LIM(
        (masterContrast - 1.f) * 0.025f
        + grain * 0.022f
        + densityIntent * 0.018f
        - halation * 0.060f
        - baseFog * 0.38f
        - shoulder * 0.018f,
        -0.050f,
        0.055f);
    const bool neutralCustomTexture = customRecipe && densityIntent <= 0.001f;
    const float textureAcutance = neutralCustomTexture ? 0.f : textureAcutanceBase;
    const float textureMidAcutance = neutralCustomTexture ? 0.f : textureMidAcutanceBase;
    const bool useLayerCurve = !customRecipe
        || curveFog > 0.001f
        || shoulder > 0.001f
        || std::fabs(rGamma - 1.f) > 0.001f
        || std::fabs(gGamma - 1.f) > 0.001f
        || std::fabs(bGamma - 1.f) > 0.001f;
    const bool useSimpleChannelGains = customRecipe
        && !useLayerCurve
        && (std::fabs(rGain - 1.f) > 0.001f || std::fabs(gGain - 1.f) > 0.001f || std::fabs(bGain - 1.f) > 0.001f);

    // === Working space matrices for LAB <-> RGB ===
    const TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const float wp[3][3] = {
        {static_cast<float>(wprof[0][0]), static_cast<float>(wprof[0][1]), static_cast<float>(wprof[0][2])},
        {static_cast<float>(wprof[1][0]), static_cast<float>(wprof[1][1]), static_cast<float>(wprof[1][2])},
        {static_cast<float>(wprof[2][0]), static_cast<float>(wprof[2][1]), static_cast<float>(wprof[2][2])}
    };
    const LumaWeights lumaWeights = makeLumaWeights(wp);

    const TMatrix wiprof = ICCStore::getInstance()->workingSpaceInverseMatrix(params->icm.workingProfile);
    const float wip[3][3] = {
        {static_cast<float>(wiprof[0][0]), static_cast<float>(wiprof[0][1]), static_cast<float>(wiprof[0][2])},
        {static_cast<float>(wiprof[1][0]), static_cast<float>(wiprof[1][1]), static_cast<float>(wiprof[1][2])},
        {static_cast<float>(wiprof[2][0]), static_cast<float>(wiprof[2][1]), static_cast<float>(wiprof[2][2])}
    };

    const bool useTextureDetail = std::fabs(textureAcutance) > 0.001f || std::fabs(textureMidAcutance) > 0.001f;
    const bool useHalation = halation > 0.001f;
    const bool useHalationBloom = halation > 0.06f;
    array2D<float> textureLum;
    array2D<float> textureDetail;
    array2D<float> textureMidDetail;
    array2D<float> halationMask;
    array2D<float> halationInner;
    array2D<float> halationOuter;
    array2D<float> halationBloom;

    if (useTextureDetail) {
        textureLum(lab->W, lab->H);
    }

    if (useHalation) {
        halationMask(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        halationInner(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        halationOuter(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        if (useHalationBloom) {
            halationBloom(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        }
    }

    if (useTextureDetail || useHalation) {

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16)
#endif
        for (int i = 0; i < lab->H; ++i) {
            for (int j = 0; j < lab->W; ++j) {
                float X, Y, Z;
                Color::Lab2XYZ(lab->L[i][j], lab->a[i][j], lab->b[i][j], X, Y, Z);
                float R, G, B;
                Color::xyz2rgb(X, Y, Z, R, G, B, wip);

                R = std::max(R / MAXVALF, 0.f);
                G = std::max(G / MAXVALF, 0.f);
                B = std::max(B / MAXVALF, 0.f);

                const float lum = rgbLuminance(R, G, B, lumaWeights);
                if (useTextureDetail) {
                    textureLum[i][j] = printDensityCoordinate(lum);
                }
                if (useHalation) {
                    const float maxChannel = std::max(R, std::max(G, B));
                    const float sourceSat = rgbSaturation(R, G, B);
                    const float channelPressure = maxChannel * (0.48f - 0.12f * smoothstep(0.40f, 1.f, sourceSat));
                    const float exposurePressure = std::max(lum, channelPressure);
                    const float highlight = smoothstep(0.75f, 2.f, exposurePressure);
                    const float redEmitter = smoothstep(0.04f, 0.55f, R - std::max(G, B));
                    const float amberEmitter = smoothstep(0.04f, 0.48f, std::min(R, G) - B);
                    const float warmEmitter = LIM(redEmitter + amberEmitter * 0.55f, 0.f, 1.f);
                    const float scatterColor = LIM(
                        1.f - 0.32f * smoothstep(0.45f, 0.98f, sourceSat) + 0.42f * warmEmitter,
                        0.58f,
                        1.22f);
                    halationMask[i][j] =
                        highlight * std::min(exposurePressure, 3.f) * (1.f / 3.f) * scatterColor;
                }
            }
        }
    }

    if (useTextureDetail) {
        textureDetail(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        const int shortSide = std::min(lab->W, lab->H);
        const int textureRadius = std::max(1, std::min(6, shortSide / 900));
        const int textureMidRadius = std::max(textureRadius + 1, std::min(18, textureRadius * 3 + shortSide / 1400));
        boxblur(static_cast<float**>(textureLum), static_cast<float**>(textureDetail), textureRadius, lab->W, lab->H, true);
        textureMidDetail(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        boxblur(static_cast<float**>(textureDetail), static_cast<float**>(textureMidDetail), textureMidRadius, lab->W, lab->H, true);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16)
#endif
        for (int i = 0; i < lab->H; ++i) {
            for (int j = 0; j < lab->W; ++j) {
                const float fineBlur = textureDetail[i][j];
                const float midBlur = textureMidDetail[i][j];
                textureDetail[i][j] = textureLum[i][j] - fineBlur;
                textureMidDetail[i][j] = fineBlur - midBlur;
            }
        }

        textureLum.free();
    }

    if (useHalation) {
        const int shortSide = std::min(lab->W, lab->H);
        const int innerRadius = std::max(1, std::min(12, shortSide / 600));
        const int outerRadius = std::max(innerRadius + 1, std::min(36, shortSide / 180));
        const int bloomRadius = std::max(outerRadius + 1, std::min(72, outerRadius * 2));
        boxblur(static_cast<float**>(halationMask), static_cast<float**>(halationInner), innerRadius, lab->W, lab->H, true);
        boxblur(static_cast<float**>(halationInner), static_cast<float**>(halationOuter), outerRadius, lab->W, lab->H, true);
        if (useHalationBloom) {
            boxblur(static_cast<float**>(halationOuter), static_cast<float**>(halationBloom), bloomRadius, lab->W, lab->H, true);
        }
        // Keep the source mask for Stage 7 so halation can be weighted toward
        // spill around bright edges instead of uniformly warming flat highlights.
    }

    array2D<float> grainCloud;
    array2D<float> grainMottle;
    if (grain > 0.001f) {
        array2D<float> grainSource(lab->W, lab->H);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16)
#endif
        for (int i = 0; i < lab->H; ++i) {
            for (int j = 0; j < lab->W; ++j) {
                grainSource[i][j] = grainNoise(j, i, 0x13579BDFu);
            }
        }

        grainCloud(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        grainMottle(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        const int shortSide = std::min(lab->W, lab->H);
        const int grainRadius = std::max(1, std::min(4, shortSide / 1800 + (grain > 0.32f ? 1 : 0) + (grain > 0.58f ? 1 : 0)));
        const int mottleRadius = std::max(
            grainRadius + 1,
            std::min(9, grainRadius * 2 + shortSide / 2400 + 1));
        boxblur(static_cast<float**>(grainSource), static_cast<float**>(grainCloud), grainRadius, lab->W, lab->H, true);
        boxblur(static_cast<float**>(grainSource), static_cast<float**>(grainMottle), mottleRadius, lab->W, lab->H, true);
        grainSource.free();
    }

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int i = 0; i < lab->H; ++i) {
        for (int j = 0; j < lab->W; ++j) {
            // Save originals for strength blending
            const float origL = lab->L[i][j];
            const float origA = lab->a[i][j];
            const float origB = lab->b[i][j];

            // --- LAB -> XYZ -> RGB ---
            float X, Y, Z;
            Color::Lab2XYZ(lab->L[i][j], lab->a[i][j], lab->b[i][j], X, Y, Z);
            float R, G, B;
            Color::xyz2rgb(X, Y, Z, R, G, B, wip);

            // Preserve scene-referred highlight headroom for the film shoulder
            // instead of clipping it before the response curve.
            R = std::max(R / MAXVALF, 0.f);
            G = std::max(G / MAXVALF, 0.f);
            B = std::max(B / MAXVALF, 0.f);
            const float sourceNeutrality = neutralAxisWeight(R, G, B);

            // === Stage 0.5: Texture/acutance exposure response ===
            // ARRI-style textures happen before the core color transform, so
            // model acutance as a small scene-density perturbation and let the
            // emulsion/print response decide how much survives the toe and
            // shoulder.
            if (textureDetail) {
                const float sceneLum = rgbLuminance(R, G, B, lumaWeights);
                const float displayLum = printDensityCoordinate(sceneLum);
                const float fineWindow = smoothstep(0.025f, 0.18f, displayLum) * (1.f - 0.55f * smoothstep(0.82f, 1.f, displayLum));
                const float midWindow = smoothstep(0.020f, 0.16f, displayLum) * (1.f - 0.65f * smoothstep(0.86f, 1.f, displayLum));
                const float detailDelta =
                    textureDetail[i][j] * textureAcutance * fineWindow
                    + textureMidDetail[i][j] * textureMidAcutance * midWindow;
                const float targetLum = std::max(sceneLum + detailDelta, 0.f);
                if (std::fabs(targetLum - sceneLum) > 1e-6f) {
                    fitRgbToLuminance(targetLum, R, G, B, lumaWeights);
                }
            }

            // === Stage 1: Per-channel film response (H&D curves) ===
            // Each channel has its own gain and gamma, modeling different
            // emulsion layer sensitivities
            if (useLayerCurve) {
                R = filmCurve(R * rGain, rGamma, curveFog, shoulder);
                G = filmCurve(G * gGain, gGamma, curveFog, shoulder);
                B = filmCurve(B * bGain, bGamma, curveFog, shoulder);
            } else if (useSimpleChannelGains) {
                R *= rGain;
                G *= gGain;
                B *= bGain;
            }

            // === Stage 2: Dye coupling matrix ===
            // Simulates subtractive dye cross-contamination between layers.
            // Apply it as exposure-neutral log-density chroma, closer to a
            // film scan / log look transform than a direct RGB brightness mix.
            if (!customRecipe) {
                applyDensityDyeCoupling(recipe->dye, R, G, B, lumaWeights);
            }

            // === Stage 3: Master tone curve on luminance ===
            // Applies overall S-curve contrast while preserving hue
            if (masterContrast > 1.01f || masterContrast < 0.99f) {
                float lum = rgbLuminance(R, G, B, lumaWeights);
                if (lum > 0.001f) {
                    float lumNew = masterContrastCurve(lum, masterContrast);
                    float ratio = lumNew / lum;
                    R *= ratio;
                    G *= ratio;
                    B *= ratio;
                    fitRgbToLuminance(lumNew, R, G, B, lumaWeights);
                }
            }

            // === Stage 4: Base fog & fade ===
            // Lifts black point, simulating unexposed emulsion density
            if (printFog > 0.001f) {
                R = R * (1.f - printFog) + printFog;
                G = G * (1.f - printFog) + printFog;
                B = B * (1.f - printFog) + printFog;
            }

            // === Stage 5: Hue-dependent saturation + Vibrance ===
            // Use HSL only to classify hue/saturation, then scale linear RGB
            // chroma around luminance so saturation changes do not drag exposure.
            {
                const float lum = rgbLuminance(R, G, B, lumaWeights);
                const float classifyScale = MAXVALF / std::max(1.f, std::max(R, std::max(G, B)));
                const float R65 = R * classifyScale;
                const float G65 = G * classifyScale;
                const float B65 = B * classifyScale;
                float h, s, hslL;
                Color::rgb2hsl(R65, G65, B65, h, s, hslL);
                static_cast<void>(hslL);
                // HSL is useful for hue, but near-white highlights can report
                // extreme saturation after normalization. Use channel purity as
                // an upper bound so hot colors keep film-like highlight behavior.
                s = std::min(LIM(s, 0.f, 1.f), rgbSaturation(R, G, B));

                if (s > 0.001f) {
                    const float sourceChroma = rgbChroma(R, G, B);
                    const float hueConfidence =
                        smoothstep(0.012f, 0.075f, sourceChroma)
                        * smoothstep(0.018f, 0.095f, s);

                    // Hue-dependent saturation
                    float hueIdx = h * 6.f;
                    int sector = static_cast<int>(hueIdx) % 6;
                    float frac = hueIdx - static_cast<int>(hueIdx);
                    float chromaScale = intp(frac, hueSat[(sector + 1) % 6], hueSat[sector]);

                    // Vibrance: boost low-saturation colors more (film-like response)
                    if (std::fabs(vibrance) > 0.001f) {
                        chromaScale *= std::max(0.f, 1.f + vibrance * (1.f - s));
                    }

                    // Low-confidence hue near the neutral axis should track
                    // like a grey ramp, not get assigned a recipe hue sector
                    // and vibrance boost. Strong subject colors pass through.
                    chromaScale = intp(hueConfidence, chromaScale, 1.f);

                    // Color styling from film stocks is most visible in the
                    // printable density range. Let the toe and shoulder move
                    // toward density compression instead of carrying full hue
                    // bias; keep near-monochrome recipes monochrome.
                    const float displayLum = printDensityCoordinate(lum);
                    const float colorResponseWindow = smoothstep(0.018f, 0.16f, displayLum) * (1.f - 0.42f * smoothstep(0.78f, 1.f, displayLum));
                    const float filmChromaResponse = 1.f - densityIntent * (1.f - colorResponseWindow) * (1.f - monochromeIntent);
                    chromaScale = 1.f + (chromaScale - 1.f) * filmChromaResponse;
                    if (monochromeIntent > 0.001f) {
                        // Near-monochrome stocks should behave like density
                        // records first; split toning and warmth later add the
                        // deliberate silver/print color, not source hue leakage.
                        chromaScale *= 1.f - monochromeIntent * 0.86f;
                    }

                    // Protect warm memory colors from turning into orange
                    // paint when a red/yellow-heavy stock and positive
                    // saturation/vibrance stack together.
                    if (chromaScale > 1.f) {
                        const float skinHue = 1.f - smoothstep(0.030f, 0.110f, circularHueDistance(h, 0.075f));
                        const float skinSat = smoothstep(0.08f, 0.30f, s) * (1.f - smoothstep(0.62f, 0.92f, s));
                        const float skinLum = smoothstep(0.12f, 0.40f, lum) * (1.f - smoothstep(0.78f, 0.98f, lum));
                        const float skinGuard = skinHue * skinSat * skinLum;
                        if (skinGuard > 0.001f) {
                            const float skinMaxBoost = 1.f + 0.20f * (1.f - 0.45f * densityIntent);
                            chromaScale = intp(skinGuard, std::min(chromaScale, skinMaxBoost), chromaScale);
                        }

                        // Keep common memory colors from becoming electronic:
                        // film stocks can bias skies and foliage, but strong
                        // boost should still leave them recognizably natural.
                        const float skyHue = 1.f - smoothstep(0.035f, 0.135f, circularHueDistance(h, 0.590f));
                        const float skySat = smoothstep(0.18f, 0.42f, s) * (1.f - smoothstep(0.86f, 1.f, s));
                        const float skyLum = smoothstep(0.28f, 0.58f, lum) * (1.f - smoothstep(0.96f, 1.f, lum));
                        const float skyGuard = skyHue * skySat * skyLum;
                        if (skyGuard > 0.001f) {
                            const float skyMaxBoost = 1.f + 0.26f * (1.f - 0.35f * densityIntent);
                            chromaScale = intp(skyGuard, std::min(chromaScale, skyMaxBoost), chromaScale);
                        }

                        const float foliageHue = 1.f - smoothstep(0.040f, 0.145f, circularHueDistance(h, 0.315f));
                        const float foliageSat = smoothstep(0.16f, 0.38f, s) * (1.f - smoothstep(0.82f, 1.f, s));
                        const float foliageLum = smoothstep(0.08f, 0.30f, lum) * (1.f - smoothstep(0.68f, 0.90f, lum));
                        const float foliageGuard = foliageHue * foliageSat * foliageLum;
                        if (foliageGuard > 0.001f) {
                            const float foliageMaxBoost = 1.f + 0.22f * (1.f - 0.30f * densityIntent);
                            chromaScale = intp(foliageGuard, std::min(chromaScale, foliageMaxBoost), chromaScale);
                        }
                    }

                    // Color negative and print stocks shed saturation in deep
                    // toe and compressed shoulder; high-purity colors also
                    // tighten slightly, avoiding neon edges after the print.
                    const float maxChannel = std::max(R, std::max(G, B));
                    const float shadowChroma = 1.f - (1.f - smoothstep(0.025f, 0.20f, displayLum)) * densityIntent * 0.18f;
                    const float highlightChroma = 1.f - smoothstep(0.72f, 0.98f, displayLum) * densityIntent * (0.16f + 0.24f * densityIntent);
                    const float purityGuard = 1.f - smoothstep(0.82f, 1.f, s) * densityIntent * (0.06f + 0.08f * smoothstep(0.65f, 1.f, displayLum));
                    const float channelHeadroom = 1.f - smoothstep(0.86f, 1.f, maxChannel) * densityIntent * (0.08f + 0.12f * smoothstep(0.70f, 1.f, s));
                    chromaScale *= shadowChroma * highlightChroma * purityGuard * channelHeadroom;
                    scaleRgbChroma(lum, R, G, B, chromaScale);
                }
            }

            // === Stage 5.5: Neutral-axis tracking ===
            // Production look transforms generally keep grey ramps stable and
            // let deliberate split-toning/warmth happen later in the chain.
            if (sourceNeutrality > 0.001f) {
                const float lum = rgbLuminance(R, G, B, lumaWeights);
                const float neutralHold = LIM(0.90f + 0.06f * densityIntent + 0.03f * monochromeIntent, 0.f, 0.98f);
                const float neutralScale = 1.f - sourceNeutrality * neutralHold;
                if (neutralScale < 0.999f) {
                    scaleRgbChroma(lum, R, G, B, neutralScale);
                }
            }

            // === Stage 6: Shadow/highlight color cast ===
            // Luminance-preserving log-density tinting with smooth transitions
            if (shTint > 0.001f || hlTint > 0.001f) {
                const float sceneLum = rgbLuminance(R, G, B, lumaWeights);
                const float tintLum = printDensityCoordinate(sceneLum);
                const float tintPurity = smoothstep(0.35f, 0.90f, rgbSaturation(R, G, B));
                const float neutralBias = 0.55f + 0.45f * sourceNeutrality;
                const float subjectTintWindow = neutralBias * (1.f - tintPurity * (0.22f + 0.30f * densityIntent) * (1.f - 0.35f * sourceNeutrality));

                if (shTint > 0.001f) {
                    float shadowW = 1.f - smoothstep(0.f, 0.4f, tintLum);
                    float shStr = shTint * shadowW * subjectTintWindow * 0.25f;
                    float shCos = cosf(shHue);
                    float shSin = sinf(shHue);
                    applyLogDensityColorBias(
                        R, G, B,
                        shStr * (shCos * 0.6f + shSin * 0.2f),
                        shStr * (-shCos * 0.3f + shSin * 0.3f),
                        shStr * (-shCos * 0.3f - shSin * 0.5f),
                        lumaWeights);
                }

                if (hlTint > 0.001f) {
                    float highW = smoothstep(0.6f, 1.f, tintLum);
                    float hlStr = hlTint * highW * subjectTintWindow * 0.20f;
                    float hlCos = cosf(hlHue);
                    float hlSin = sinf(hlHue);
                    applyLogDensityColorBias(
                        R, G, B,
                        hlStr * (hlCos * 0.6f + hlSin * 0.2f),
                        hlStr * (-hlCos * 0.3f + hlSin * 0.3f),
                        hlStr * (-hlCos * 0.3f - hlSin * 0.5f),
                        lumaWeights);
                }
            }

            // === Stage 7: Halation ===
            // Bright light scatters through film base, creating warm glow
            // that spills from highlights into nearby pixels.
            if (halation > 0.001f) {
                const float lum = rgbLuminance(R, G, B, lumaWeights);
                const float sourceGlow = halationMask ? halationMask[i][j] : 0.f;
                const float innerSpill = halationInner ? std::max(halationInner[i][j] - sourceGlow * 0.62f, 0.f) : 0.f;
                const float outerSpill = halationOuter ? std::max(halationOuter[i][j] - sourceGlow * 0.38f, 0.f) : 0.f;
                const float bloomSpill = halationBloom ? std::max(halationBloom[i][j] - sourceGlow * 0.18f, 0.f) : 0.f;
                const float localGlow = smoothstep(0.55f, 0.98f, lum) * halation * 0.045f;
                const float innerGlow = innerSpill * halation * 0.46f;
                const float outerGlow = outerSpill * halation * 0.28f;
                const float bloomGlow = halationBloom
                    ? bloomSpill * halation * 0.10f * smoothstep(0.06f, 0.30f, halation)
                    : 0.f;
                const float glow = localGlow + innerGlow + outerGlow + bloomGlow;
                if (glow > 0.0001f) {
                    const float dR = localGlow * 0.45f + innerGlow * 0.68f + outerGlow * 0.42f + bloomGlow * 0.30f;
                    const float dG = localGlow * 0.16f + innerGlow * 0.20f + outerGlow * 0.18f + bloomGlow * 0.07f;
                    const float dB = localGlow * 0.018f + innerGlow * 0.022f + outerGlow * 0.028f + bloomGlow * 0.008f;
                    const float targetLum = std::max(lum + rgbLuminance(dR, dG, dB, lumaWeights), 0.f);
                    R += dR;
                    G += dG;
                    B += dB;
                    fitRgbToLuminance(targetLum, R, G, B, lumaWeights);
                }
            }

            // === Stage 8: Warmth/tint global color balance ===
            if (std::fabs(totalWarmth) > 0.001f || std::fabs(totalTint) > 0.001f) {
                const float lum = printDensityCoordinate(rgbLuminance(R, G, B, lumaWeights));
                float densityWindow = smoothstep(0.015f, 0.18f, lum) * (1.f - 0.55f * smoothstep(0.82f, 1.f, lum));

                // Printer-light style balance is strongest along the neutral
                // and midtone density scale. Highly saturated subject colors
                // should retain more of their own dye direction.
                const float purity = smoothstep(0.42f, 0.95f, rgbSaturation(R, G, B));
                densityWindow *= 1.f - purity * (0.12f + 0.24f * densityIntent);

                applyLogDensityColorBias(
                    R, G, B,
                    densityWindow * (totalWarmth * 0.090f + totalTint * 0.035f),
                    densityWindow * (-totalTint * 0.045f),
                    densityWindow * (-totalWarmth * 0.090f + totalTint * 0.025f),
                    lumaWeights);
            }

            // === Stage 8.5: Print white convergence ===
            // After halation and color filtration, very bright values should
            // drift toward paper/display white instead of retaining saturated
            // color. Keep it density-intent driven so neutral custom stays
            // neutral and midtones keep their character.
            if (densityIntent > 0.001f) {
                float sceneLum = rgbLuminance(R, G, B, lumaWeights);
                float printLum = printDensityCoordinate(sceneLum);
                float printSat = rgbSaturation(R, G, B);
                float maxChannel = std::max(R, std::max(G, B));
                const float dyeGamutW = densityIntent
                    * smoothstep(0.32f, 0.90f, printSat)
                    * (0.35f + 0.65f * smoothstep(0.62f, 1.f, std::max(printLum, maxChannel)))
                    * (1.f - sourceNeutrality * 0.80f);
                if (dyeGamutW > 0.001f) {
                    applyPrintDyeGamutLimit(
                        R, G, B,
                        dyeGamutW * (0.18f + 0.16f * LIM(shoulder, 0.f, 1.f)),
                        lumaWeights);
                    sceneLum = rgbLuminance(R, G, B, lumaWeights);
                    printLum = printDensityCoordinate(sceneLum);
                    printSat = rgbSaturation(R, G, B);
                    maxChannel = std::max(R, std::max(G, B));
                }

                // Very deep print density cannot carry clean chroma. Apply
                // this after split/printer tinting so the toe feels like a
                // chemical floor, while still leaving enough color for toned
                // B&W and print-shadow character.
                const float blackW = (1.f - smoothstep(0.018f, 0.135f, printLum))
                    * densityIntent
                    * (0.40f + 0.15f * LIM(baseFog * 8.f, 0.f, 1.f));
                if (blackW > 0.001f) {
                    const float blackChroma = 1.f - blackW * (0.18f + 0.10f * LIM(shoulder, 0.f, 1.f));
                    scaleRgbChroma(sceneLum, R, G, B, blackChroma);
                    sceneLum = rgbLuminance(R, G, B, lumaWeights);
                    printLum = printDensityCoordinate(sceneLum);
                    printSat = rgbSaturation(R, G, B);
                    maxChannel = std::max(R, std::max(G, B));
                }

                const float neutralWhite = 1.f - 0.28f * smoothstep(0.35f, 0.95f, printSat);
                const float whiteW = smoothstep(0.82f, 0.995f, printLum) * densityIntent * neutralWhite;
                if (whiteW > 0.001f) {
                    const float whiteChroma = 1.f - whiteW * (0.22f + 0.22f * LIM(shoulder, 0.f, 1.f));
                    scaleRgbChroma(sceneLum, R, G, B, whiteChroma);
                }

                // Saturated colors that press a channel against display white
                // should tighten like print dye density, even when luminance is
                // below the white-convergence threshold.
                const float headroomW = smoothstep(0.88f, 1.f, maxChannel) * smoothstep(0.22f, 0.92f, printSat) * densityIntent;
                if (headroomW > 0.001f) {
                    const float densityChroma = 1.f - headroomW * (0.08f + 0.18f * LIM(shoulder, 0.f, 1.f));
                    scaleRgbChroma(sceneLum, R, G, B, densityChroma);
                }
            }

            // === Stage 9: Film grain ===
            // Mostly density variation with fine grain plus two clustered
            // cloud scales and subtler independent color-layer texture. Work
            // in a soft-log luminance domain so grain rides exposure like
            // emulsion density instead of behaving like additive display noise.
            if (grain > 0.001f) {
                const float sceneLum = rgbLuminance(R, G, B, lumaWeights);
                const float displayLum = printDensityCoordinate(sceneLum);
                const float midWeight = 4.f * displayLum * (1.f - displayLum);
                const float shadowDensity = 1.f + 0.32f * (1.f - smoothstep(0.10f, 0.58f, displayLum));
                const float highlightQuiet = 1.f - 0.50f * smoothstep(0.78f, 1.f, displayLum);
                const float densityWeight = (0.18f + 0.82f * midWeight) * shadowDensity * highlightQuiet;
                const float grainStr = grain * densityWeight * 0.13f;

                const float fine = grainNoise(j, i, 0x12345678u);
                const float cloud = grainCloud ? softLimitSigned(grainCloud[i][j] * 2.7f) : fine;
                const float mottle = grainMottle ? softLimitSigned(grainMottle[i][j] * 4.2f) : cloud;
                const float densityNoise =
                    fine * (1.f - grainCloudMix)
                    + (cloud * (1.f - grainMottleMix) + mottle * grainMottleMix) * grainCloudMix;
                const float toe = 0.012f + 0.010f * grain;
                const float targetLum = std::max(std::exp(std::log(std::max(sceneLum, 0.f) + toe) + densityNoise * grainStr) - toe, 0.f);

                if (sceneLum > 0.0001f) {
                    const float ratio = targetLum / sceneLum;
                    R *= ratio;
                    G *= ratio;
                    B *= ratio;
                    fitRgbToLuminance(targetLum, R, G, B, lumaWeights);
                } else {
                    R = targetLum;
                    G = targetLum;
                    B = targetLum;
                }

                const float nR = grainNoise(j, i, 0x9ABCDEF0u);
                const float nG = grainNoise(j, i, 0x56789ABCu);
                const float nB = grainNoise(j, i, 0x2468ACE0u);
                const float targetDisplayLum = printDensityCoordinate(targetLum);
                const float chromaWindow = smoothstep(0.025f, 0.20f, targetDisplayLum) * (1.f - 0.45f * smoothstep(0.76f, 1.f, targetDisplayLum));
                const float grainColorPurity = smoothstep(0.05f, 0.55f, rgbSaturation(R, G, B));
                const float neutralChromaGrain = 1.f - sourceNeutrality * (0.70f + 0.15f * densityIntent);
                const float chromaSubjectWindow = (0.35f + 0.65f * grainColorPurity) * LIM(neutralChromaGrain, 0.f, 1.f);
                const float chromaStr = grain * densityWeight * chromaWindow * colorGrainIntent * chromaSubjectWindow * 0.026f;
                R += nR * chromaStr;
                G += nG * chromaStr * 0.72f;
                B += nB * chromaStr * 0.90f;
                fitRgbToLuminance(targetLum, R, G, B, lumaWeights);
            }

            // === Convert back to 0-65535 range ===
            R *= MAXVALF;
            G *= MAXVALF;
            B *= MAXVALF;

            // === RGB -> XYZ -> LAB ===
            Color::rgbxyz(R, G, B, X, Y, Z, wp);
            float newL, newA, newB_;
            Color::XYZ2Lab(X, Y, Z, newL, newA, newB_);

            // === Stage 10: Strength blending ===
            lab->L[i][j] = intp(userStrength, newL, origL);
            lab->a[i][j] = intp(userStrength, newA, origA);
            lab->b[i][j] = intp(userStrength, newB_, origB);
        }
    }
}

} // namespace rtengine
