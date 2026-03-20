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

#include <cmath>
#include <cstring>

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

// Film H&D characteristic curve with base fog, S-curve contrast, and shoulder
inline float filmCurve(float x, float gamma, float baseFog, float shoulder)
{
    // Soft-clip instead of hard clamp — preserves highlight gradations
    x = softClip(x);

    // Base fog: lift the black point
    if (baseFog > 0.0001f) {
        x = x * (1.f - baseFog) + baseFog;
    }

    // Power-function S-curve for film contrast
    // gamma > 1 = more contrast, gamma < 1 = flatter
    if (gamma > 1.001f || gamma < 0.999f) {
        if (x < 0.5f) {
            float t = x * 2.f;
            x = 0.5f * powf(t, gamma);
        } else {
            float t = (1.f - x) * 2.f;
            x = 1.f - 0.5f * powf(t, gamma);
        }
    }

    // Shoulder: compress highlights above knee point
    if (shoulder > 0.01f) {
        float knee = 1.f - shoulder * 0.4f;
        if (x > knee && knee < 0.999f) {
            float excess = (x - knee) / (1.f - knee);
            x = knee + (1.f - knee) * excess / (1.f + excess * shoulder * 2.f);
        }
    }

    return LIM(x, 0.f, 1.f);
}

// Smooth hermite interpolation
inline float smoothstep(float edge0, float edge1, float x)
{
    float t = LIM((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Hash-based per-pixel noise for film grain
inline float grainNoise(int x, int y, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = h ^ (h >> 16);
    return (static_cast<float>(h & 0xFFFFu) / 65535.f) * 2.f - 1.f;
}

// ================================================================
// 16 preset recipes: custom + 15 film stocks
// Values are INTENTIONALLY strong for visible, dramatic differences.
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

} // namespace


void ImProcFunctions::filmPresets(LabImage *lab, const procparams::FilmPresetsParams &fp)
{
    if (!fp.enabled || fp.strength == 0) {
        return;
    }

    const FilmRecipe* recipe = lookupRecipe(fp.preset);

    // User modifiers applied on top of recipe base values.
    // Multipliers are chosen so sliders at 100 produce DRAMATIC changes.
    const float userStrength = fp.strength / 100.f;

    // Contrast: slider 0 = recipe default, slider ±100 = ±0.6 gamma shift
    const float userContrast = fp.contrast / 100.f;

    // Saturation: slider 0 = 1x recipe, slider 100 = 2x, slider -100 = 0x
    const float userSatScale = 1.f + fp.saturation / 100.f;

    // Warmth/tint: slider ±100 = large visible color shift
    const float userWarmth = fp.warmth / 100.f;
    const float userTint = fp.tint / 100.f;

    // Fade: slider 0-100 maps to 0-0.15 additional base fog
    const float userFade = fp.fade / 100.f;

    // Rolloff: slider 0-100 maps to 0-0.8 additional shoulder
    const float userRolloff = fp.rolloff / 100.f;

    // Halation: slider 0-100 maps to 0-1.0
    const float userHalation = fp.halation / 100.f;

    // Channel shifts: slider ±100 = ±0.5 gain change
    const float userRedShift = fp.redShift / 100.f;
    const float userGreenShift = fp.greenShift / 100.f;
    const float userBlueShift = fp.blueShift / 100.f;

    // Grain: slider 0-100 maps to 0-1.0
    const float userGrain = fp.grain / 100.f;

    // Vibrance: slider -100..100 maps to -1..1
    const float userVibrance = fp.vibrance / 100.f;

    // === Compute effective parameters ===

    // Per-channel gains
    const float rGain = recipe->redGain + userRedShift * 0.5f;
    const float gGain = recipe->greenGain + userGreenShift * 0.5f;
    const float bGain = recipe->blueGain + userBlueShift * 0.5f;

    // Per-channel gammas (use recipe values directly — user adjusts via masterContrast)
    const float rGamma = recipe->redGamma;
    const float gGamma = recipe->greenGamma;
    const float bGamma = recipe->blueGamma;

    // Master contrast: recipe + user adjustment
    const float masterContrast = recipe->masterContrast + userContrast * 0.6f;

    // Base fog: recipe + user fade
    const float baseFog = recipe->baseFog + userFade * 0.15f;

    // Shoulder: recipe + user rolloff
    const float shoulder = recipe->shoulder + userRolloff * 0.8f;

    // Halation: user slider overrides recipe when > 0
    const float halation = (fp.halation > 0) ? userHalation : recipe->halation;

    // Hue-dependent saturation: recipe values scaled by user
    float hueSat[6];
    for (int i = 0; i < 6; ++i) {
        hueSat[i] = recipe->hueSat[i] * userSatScale;
    }

    // Shadow/highlight tinting: user overrides recipe when slider > 0
    const float shHueDeg = (fp.shadowTint > 0) ? static_cast<float>(fp.shadowHue) : recipe->shadowHue;
    const float shHue = shHueDeg * rtengine::RT_PI_F / 180.f;
    const float shTint = (fp.shadowTint > 0) ? fp.shadowTint / 100.f : recipe->shadowTint;

    const float hlHueDeg = (fp.highlightTint > 0) ? static_cast<float>(fp.highlightHue) : recipe->highlightHue;
    const float hlHue = hlHueDeg * rtengine::RT_PI_F / 180.f;
    const float hlTint = (fp.highlightTint > 0) ? fp.highlightTint / 100.f : recipe->highlightTint;

    // Warmth/tint: recipe + user (large multipliers for visible effect)
    const float totalWarmth = recipe->warmth + userWarmth * 0.5f;
    const float totalTint = recipe->tint + userTint * 0.5f;

    // Grain: user slider overrides recipe when > 0
    const float grain = (fp.grain > 0) ? userGrain : recipe->grain;

    // Vibrance: recipe + user adjustment
    const float vibrance = recipe->vibrance + userVibrance * 0.5f;

    // === Working space matrices for LAB <-> RGB ===
    const TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
    const float wp[3][3] = {
        {static_cast<float>(wprof[0][0]), static_cast<float>(wprof[0][1]), static_cast<float>(wprof[0][2])},
        {static_cast<float>(wprof[1][0]), static_cast<float>(wprof[1][1]), static_cast<float>(wprof[1][2])},
        {static_cast<float>(wprof[2][0]), static_cast<float>(wprof[2][1]), static_cast<float>(wprof[2][2])}
    };

    const TMatrix wiprof = ICCStore::getInstance()->workingSpaceInverseMatrix(params->icm.workingProfile);
    const float wip[3][3] = {
        {static_cast<float>(wiprof[0][0]), static_cast<float>(wiprof[0][1]), static_cast<float>(wiprof[0][2])},
        {static_cast<float>(wiprof[1][0]), static_cast<float>(wiprof[1][1]), static_cast<float>(wiprof[1][2])},
        {static_cast<float>(wiprof[2][0]), static_cast<float>(wiprof[2][1]), static_cast<float>(wiprof[2][2])}
    };

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

            // Normalize to 0-1 range
            R = LIM(R / MAXVALF, 0.f, 1.f);
            G = LIM(G / MAXVALF, 0.f, 1.f);
            B = LIM(B / MAXVALF, 0.f, 1.f);

            // === Stage 1: Per-channel film response (H&D curves) ===
            // Each channel has its own gain and gamma, modeling different
            // emulsion layer sensitivities
            R = filmCurve(R * rGain, rGamma, baseFog, shoulder);
            G = filmCurve(G * gGain, gGamma, baseFog, shoulder);
            B = filmCurve(B * bGain, bGamma, baseFog, shoulder);

            // === Stage 2: Dye coupling matrix ===
            // Simulates subtractive dye cross-contamination between layers
            {
                float R2 = recipe->dye[0][0] * R + recipe->dye[0][1] * G + recipe->dye[0][2] * B;
                float G2 = recipe->dye[1][0] * R + recipe->dye[1][1] * G + recipe->dye[1][2] * B;
                float B2 = recipe->dye[2][0] * R + recipe->dye[2][1] * G + recipe->dye[2][2] * B;
                // Soft-clip to preserve highlight gradations
                R = softClip(R2);
                G = softClip(G2);
                B = softClip(B2);
            }

            // === Stage 3: Master tone curve on luminance ===
            // Applies overall S-curve contrast while preserving hue
            if (masterContrast > 1.01f || masterContrast < 0.99f) {
                float lum = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                if (lum > 0.001f) {
                    float lumNew = filmCurve(lum, masterContrast, 0.f, 0.f);
                    float ratio = lumNew / lum;
                    R = softClip(R * ratio);
                    G = softClip(G * ratio);
                    B = softClip(B * ratio);
                }
            }

            // === Stage 4: Base fog & fade ===
            // Lifts black point, simulating unexposed emulsion density
            if (baseFog > 0.001f) {
                R = R * (1.f - baseFog) + baseFog;
                G = G * (1.f - baseFog) + baseFog;
                B = B * (1.f - baseFog) + baseFog;
            }

            // === Stage 5: Hue-dependent saturation + Vibrance ===
            // rgb2hsl expects [0,65535], hsl2rgb outputs [0,65535]
            {
                float R65 = R * MAXVALF, G65 = G * MAXVALF, B65 = B * MAXVALF;
                float h, s, l;
                Color::rgb2hsl(R65, G65, B65, h, s, l);

                if (s > 0.001f) {
                    // Hue-dependent saturation
                    float hueIdx = h * 6.f;
                    int sector = static_cast<int>(hueIdx) % 6;
                    float frac = hueIdx - static_cast<int>(hueIdx);
                    float satMul = intp(frac, hueSat[(sector + 1) % 6], hueSat[sector]);
                    s = LIM01(s * satMul);

                    // Vibrance: boost low-saturation colors more (film-like response)
                    if (std::fabs(vibrance) > 0.001f) {
                        float scale = 1.f + vibrance * (1.f - s);
                        s = LIM01(s * scale);
                    }

                    Color::hsl2rgb(h, s, l, R65, G65, B65);
                    R = R65 / MAXVALF;
                    G = G65 / MAXVALF;
                    B = B65 / MAXVALF;
                }
            }

            // === Stage 6: Shadow/highlight color cast ===
            // Luminance-preserving tinting with smooth transitions
            if (shTint > 0.001f || hlTint > 0.001f) {
                float lum = 0.2126f * R + 0.7152f * G + 0.0722f * B;

                if (shTint > 0.001f) {
                    float lumBefore = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                    float shadowW = 1.f - smoothstep(0.f, 0.4f, lum);
                    float shStr = shTint * shadowW * 0.25f;
                    float shCos = cosf(shHue);
                    float shSin = sinf(shHue);
                    R = softClip(R + shStr * (shCos * 0.6f + shSin * 0.2f));
                    G = softClip(G + shStr * (-shCos * 0.3f + shSin * 0.3f));
                    B = softClip(B + shStr * (-shCos * 0.3f - shSin * 0.5f));
                    // Restore luminance to prevent vignette
                    float lumAfter = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                    if (lumAfter > 0.001f) {
                        float ratio = lumBefore / lumAfter;
                        R *= ratio;
                        G *= ratio;
                        B *= ratio;
                    }
                }

                if (hlTint > 0.001f) {
                    float lumBefore = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                    float highW = smoothstep(0.6f, 1.f, lum);
                    float hlStr = hlTint * highW * 0.20f;
                    float hlCos = cosf(hlHue);
                    float hlSin = sinf(hlHue);
                    R = softClip(R + hlStr * (hlCos * 0.6f + hlSin * 0.2f));
                    G = softClip(G + hlStr * (-hlCos * 0.3f + hlSin * 0.3f));
                    B = softClip(B + hlStr * (-hlCos * 0.3f - hlSin * 0.5f));
                    // Restore luminance
                    float lumAfter = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                    if (lumAfter > 0.001f) {
                        float ratio = lumBefore / lumAfter;
                        R *= ratio;
                        G *= ratio;
                        B *= ratio;
                    }
                }
            }

            // === Stage 7: Halation ===
            // Bright light scatters through film base, creating warm glow
            if (halation > 0.001f) {
                float lum = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                if (lum > 0.5f) {
                    float glow = (lum - 0.5f) * 2.f * halation;
                    R = softClip(R + glow * 0.45f);
                    G = softClip(G + glow * 0.18f);
                    B = softClip(B - glow * 0.12f);
                }
            }

            // === Stage 8: Warmth/tint global color balance ===
            if (std::fabs(totalWarmth) > 0.001f || std::fabs(totalTint) > 0.001f) {
                R = softClip(R + totalWarmth * 0.08f + totalTint * 0.03f);
                G = softClip(G - totalTint * 0.04f);
                B = softClip(B - totalWarmth * 0.08f + totalTint * 0.02f);
            }

            // === Stage 9: Film grain ===
            // Luminance-dependent noise, stronger in midtones (like real film)
            if (grain > 0.001f) {
                float lum = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                float midWeight = 4.f * lum * (1.f - lum);
                if (midWeight < 0.f) midWeight = 0.f;
                float grainStr = grain * midWeight * 0.15f;
                // Mostly luminance grain with subtle per-channel variation
                float nR = grainNoise(j, i, 0x12345678u);
                float nG = grainNoise(j, i, 0x9ABCDEF0u);
                float nB = grainNoise(j, i, 0x56789ABCu);
                float lumaGrain = (nR + nG + nB) * (1.f / 3.f);
                R = softClip(R + (lumaGrain * 0.8f + nR * 0.2f) * grainStr);
                G = softClip(G + (lumaGrain * 0.8f + nG * 0.2f) * grainStr);
                B = softClip(B + (lumaGrain * 0.8f + nB * 0.2f) * grainStr);
            }

            // === Convert back to 0-65535 range ===
            R *= MAXVALF;
            G *= MAXVALF;
            B *= MAXVALF;

            // === RGB -> XYZ -> LAB ===
            Color::rgbxyz(R, G, B, X, Y, Z, wp);
            float newL, newA, newB_;
            Color::XYZ2Lab(X, Y, Z, newL, newA, newB_);

            // === Stage 9: Strength blending ===
            lab->L[i][j] = intp(userStrength, newL, origL);
            lab->a[i][j] = intp(userStrength, newA, origA);
            lab->b[i][j] = intp(userStrength, newB_, origB);
        }
    }
}

} // namespace rtengine
