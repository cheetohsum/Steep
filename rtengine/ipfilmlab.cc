/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "array2D.h"
#include "boxblur.h"
#include "color.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "rt_math.h"

namespace rtengine
{

namespace
{

enum class StockClass {
    Custom,
    ColorNegative,
    Reversal,
    MotionNegative,
    Monochrome,
    Creative
};

struct FilmLabStock {
    const char* id;
    StockClass stockClass;
    float speed;
    float contrast;
    float toe;
    float shoulder;
    float saturation;
    float warmth;
    float tint;
    float redBias;
    float greenBias;
    float blueBias;
    float grain;
    float halation;
    float acutance;
};

// These are compact sensitometric descriptions, not baked creative grades.
// Stock, processing, output medium, and user grading remain independent below.
constexpr FilmLabStock STOCKS[] = {
    {"custom",           StockClass::Custom,         100.f, 1.00f, 0.00f, 0.00f, 1.00f,  0.00f,  0.00f,  0.00f,  0.00f,  0.00f, 0.00f, 0.00f, 0.00f},
    {"heritage_gold",    StockClass::ColorNegative,  200.f, 1.04f, 0.23f, 0.38f, 1.06f,  0.11f, -0.01f,  0.05f,  0.01f, -0.05f, 0.24f, 0.06f, 0.18f},
    {"porcelain_400",    StockClass::ColorNegative,  400.f, 0.96f, 0.28f, 0.52f, 0.93f,  0.03f,  0.02f,  0.02f,  0.00f, -0.02f, 0.22f, 0.04f, 0.12f},
    {"vivid_chrome",     StockClass::Reversal,         50.f, 1.22f, 0.08f, 0.14f, 1.27f, -0.01f,  0.01f,  0.00f,  0.02f, -0.01f, 0.10f, 0.01f, 0.27f},
    {"arctic",           StockClass::Reversal,        100.f, 1.12f, 0.10f, 0.18f, 1.10f, -0.08f, -0.01f, -0.04f,  0.01f,  0.05f, 0.14f, 0.01f, 0.22f},
    {"sovereign",        StockClass::ColorNegative,  160.f, 1.03f, 0.20f, 0.35f, 1.02f,  0.04f,  0.01f,  0.02f,  0.01f, -0.02f, 0.18f, 0.04f, 0.17f},
    {"golden_hour",      StockClass::ColorNegative,  200.f, 1.00f, 0.25f, 0.45f, 1.03f,  0.16f,  0.01f,  0.07f,  0.02f, -0.07f, 0.22f, 0.08f, 0.11f},
    {"twilight_160",     StockClass::MotionNegative, 160.f, 0.94f, 0.24f, 0.58f, 0.91f, -0.04f,  0.00f, -0.02f,  0.01f,  0.04f, 0.20f, 0.07f, 0.10f},
    {"nostalgia_200",    StockClass::ColorNegative,  200.f, 0.92f, 0.34f, 0.49f, 0.84f,  0.09f,  0.03f,  0.04f,  0.00f, -0.04f, 0.28f, 0.05f, 0.08f},
    {"desert_chrome",    StockClass::Reversal,         64.f, 1.15f, 0.12f, 0.17f, 1.17f,  0.10f, -0.02f,  0.05f,  0.01f, -0.05f, 0.12f, 0.02f, 0.24f},
    {"street_800",       StockClass::ColorNegative,  800.f, 1.07f, 0.30f, 0.44f, 0.91f, -0.01f,  0.01f, -0.01f,  0.01f,  0.01f, 0.48f, 0.05f, 0.20f},
    {"cinematic_500t",   StockClass::MotionNegative, 500.f, 0.92f, 0.26f, 0.63f, 0.88f, -0.07f,  0.00f, -0.04f,  0.00f,  0.05f, 0.30f, 0.18f, 0.09f},
    {"fade_bloom",       StockClass::Creative,        200.f, 0.84f, 0.47f, 0.58f, 0.76f,  0.08f,  0.04f,  0.04f,  0.00f, -0.03f, 0.35f, 0.17f, 0.03f},
    {"ember",            StockClass::Creative,        400.f, 1.08f, 0.22f, 0.39f, 1.05f,  0.17f,  0.03f,  0.08f,  0.01f, -0.07f, 0.30f, 0.13f, 0.16f},
    {"silver_gelatin",   StockClass::Monochrome,      400.f, 1.14f, 0.25f, 0.31f, 0.00f,  0.00f,  0.00f,  0.02f,  0.04f, -0.06f, 0.42f, 0.00f, 0.28f},
    {"analog_dream",     StockClass::Creative,        200.f, 0.89f, 0.40f, 0.62f, 0.82f,  0.07f,  0.05f,  0.04f, -0.01f, -0.02f, 0.39f, 0.14f, 0.04f},
    {"cinema_reveal_35", StockClass::MotionNegative, 250.f, 0.96f, 0.24f, 0.57f, 0.93f, -0.02f,  0.01f, -0.01f,  0.01f,  0.02f, 0.24f, 0.11f, 0.13f}
};

inline float smoothStep(float edge0, float edge1, float value)
{
    const float t = LIM((value - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

inline float luminance(float r, float g, float b)
{
    // ACES AP1 luminance coefficients.
    return 0.272229f * r + 0.674082f * g + 0.053689f * b;
}

inline const FilmLabStock& findStock(const Glib::ustring& id)
{
    for (const auto& stock : STOCKS) {
        if (id == stock.id) {
            return stock;
        }
    }

    return STOCKS[0];
}

inline Glib::ustring defaultProcess(StockClass stockClass)
{
    switch (stockClass) {
        case StockClass::Reversal:
            return "e6";
        case StockClass::MotionNegative:
            return "ecn2";
        case StockClass::Monochrome:
            return "bw";
        default:
            return "c41";
    }
}

inline float filmResponse(float value, float exposureBias, float contrast, float toe, float shoulder)
{
    value = std::max(value * std::exp2(exposureBias), 1e-7f);
    float stops = std::log2(value / 0.18f);

    if (stops < 0.f) {
        stops /= 1.f + toe * (-stops) * 0.18f;
    } else {
        stops /= 1.f + shoulder * stops * 0.20f;
    }

    return LIM(0.18f * std::exp2(stops * contrast), 0.f, 4.f);
}

inline float normalizedFilmResponse(
    float value,
    float exposureBias,
    float contrast,
    float toe,
    float shoulder,
    float blackResponse,
    float middleResponse)
{
    const float response = filmResponse(value, exposureBias, contrast, toe, shoulder);
    const float scale = 0.18f / std::max(middleResponse - blackResponse, 1e-6f);
    return LIM((response - blackResponse) * scale, 0.f, 4.f);
}

inline float filmCharacterScale(float strength)
{
    return 0.75f + 0.50f * LIM(strength, 0.f, 1.f);
}

// V2's highlight source, kept bit-for-bit because saved edits still render
// through it. Its threshold is in stops above middle grey; see the note on the
// V3 version below for why that could never fire.
inline float halationHighlightSourceV2(float luminanceValue, float peakValue, float thresholdStops)
{
    const float highlight = std::max(luminanceValue, peakValue * 0.62f);
    const float stops = std::log2(std::max(highlight, 1e-6f) / 0.18f);
    const float excess = std::max(stops - thresholdStops, 0.f);
    const float onset = smoothStep(thresholdStops, thresholdStops + 2.5f, stops);
    return onset * (1.f - std::exp2(-excess * 0.55f));
}

// Halation is light that made it through the emulsion, bounced off the film
// base and re-exposed the neighbouring grains, so what drives it is how far a
// highlight is past the point where the emulsion had anything left to record.
//
// This stage runs on a display-referred signal: rgbProc's filmlike_clip has
// already folded the scene into 0..1 (measured peak: exactly 1.0000), and the
// tone curve LUT is a second ceiling behind it, so the scene's real specular
// magnitude is gone before we are called. Keying the onset two stops ABOVE
// diffuse white - where this used to sit - left the source term at 0.026 for
// the brightest pixel the pipeline can deliver, which is why halation was
// invisible at every slider position and every stock. Key it to the top of the
// range we actually receive, and read intensity from how completely the pixel
// is blown instead of from a magnitude we no longer have.
inline float halationHighlightSource(float luminanceValue, float peakValue, float onset)
{
    const float highlight = std::max(luminanceValue, peakValue * 0.82f);
    const float blown = smoothStep(onset, 1.f, highlight);

    // Weight fully clipped pixels well above merely bright ones: those are the
    // ones that were many stops over in the scene, so a light source halates
    // and a white shirt barely does.
    return blown * blown * (0.35f + 0.65f * smoothStep(0.90f, 1.f, highlight));
}


// Three successive box blurs approximate a Gaussian of the same sigma. One box
// leaves square halos, and a halo around a point highlight is the single place
// that shows most plainly.
inline void halationBlur(
    array2D<float>& source,
    array2D<float>& destination,
    array2D<float>& scratch,
    int radius,
    int width,
    int height,
    bool multiThread)
{
    const int pass = std::max(1, static_cast<int>(radius * 0.577f + 0.5f));
    boxblur(static_cast<float**>(source), static_cast<float**>(destination), pass, width, height, multiThread);
    boxblur(static_cast<float**>(destination), static_cast<float**>(scratch), pass, width, height, multiThread);
    boxblur(static_cast<float**>(scratch), static_cast<float**>(destination), pass, width, height, multiThread);
}

inline void applySaturation(float& r, float& g, float& b, float saturation)
{
    const float y = luminance(r, g, b);
    r = y + (r - y) * saturation;
    g = y + (g - y) * saturation;
    b = y + (b - y) * saturation;
}

inline void applyVibrance(float& r, float& g, float& b, float vibrance)
{
    if (std::fabs(vibrance) < 0.001f) {
        return;
    }

    const float y = std::max(luminance(r, g, b), 0.f);
    const float range = std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
    const float chroma = LIM(range / (y + 0.12f), 0.f, 1.f);
    const float protection = 1.f - chroma * 0.72f;
    applySaturation(r, g, b, LIM(1.f + vibrance * protection * 0.55f, 0.35f, 1.65f));
}

inline void applyDyeCoupling(float& r, float& g, float& b, StockClass stockClass)
{
    float rr;
    float gg;
    float bb;

    switch (stockClass) {
        case StockClass::Reversal:
            rr = 1.035f * r - 0.020f * g - 0.015f * b;
            gg = -0.010f * r + 1.025f * g - 0.015f * b;
            bb = -0.010f * r - 0.018f * g + 1.028f * b;
            break;
        case StockClass::MotionNegative:
            rr = 0.970f * r + 0.026f * g + 0.004f * b;
            gg = 0.010f * r + 0.982f * g + 0.008f * b;
            bb = 0.006f * r + 0.026f * g + 0.968f * b;
            break;
        default:
            rr = 0.990f * r + 0.015f * g - 0.005f * b;
            gg = 0.008f * r + 0.986f * g + 0.006f * b;
            bb = -0.004f * r + 0.020f * g + 0.984f * b;
            break;
    }

    r = rr;
    g = gg;
    b = bb;
}





inline void hueVector(float hue, float& r, float& g, float& b)
{
    const float angle = hue * static_cast<float>(RT_PI) / 180.f;
    r = std::cos(angle);
    g = std::cos(angle - 2.f * static_cast<float>(RT_PI) / 3.f);
    b = std::cos(angle + 2.f * static_cast<float>(RT_PI) / 3.f);
}

inline void toCanonical(const LabImage* lab, int row, int col, const float inverse[3][3], float& r, float& g, float& b)
{
    float x;
    float y;
    float z;
    Color::Lab2XYZ(lab->L[row][col], lab->a[row][col], lab->b[row][col], x, y, z);
    Color::xyz2rgb(x, y, z, r, g, b, inverse);
    r = std::max(r / MAXVALF, 0.f);
    g = std::max(g / MAXVALF, 0.f);
    b = std::max(b / MAXVALF, 0.f);
}

constexpr int V3_DENSITY_LUT_SIZE = 8192;
constexpr int V3_OUTPUT_LUT_SIZE = 4096;
constexpr float V3_MAX_INPUT = 8.f;

// V4 samples the emulsion in log exposure. A linear grid spends nearly all
// of its entries above middle grey; scene-referred input reaches five stops
// past diffuse white, and film's toe lives eight stops under middle grey, so
// the axis that matters is stops. 8192 entries over 22 stops is 0.0027
// stops per entry.
constexpr float V4_STOPS_MIN = -14.f;
constexpr float V4_STOPS_MAX = 8.f;

// Diffuse white, in stops above middle grey.
constexpr float V3_WHITE_STOPS = 2.4739312f;

// The print is calibrated against one reference negative: a fixed density per
// stop, which is what the printer's own contrast is set for. A stock that is
// contrastier than this reference prints contrastier. Calibrating against each
// stock's *own* gamma cancelled the stock out exactly, which is why every
// stock used to measure the same contrast. 0.18 density/stop is C-41's ~0.60
// gamma per log10 exposure; the absolute value only decides where the emulsion
// sits inside [baseFog, maxDensity].
constexpr float V3_REFERENCE_GAMMA = 0.18f;

// Toe length the print's shadow expansion is tuned against. The expansion is
// there to undo the emulsion's toe, so it scales with how much toe there was.
constexpr float V3_REFERENCE_TOE = 0.26f;

// Stops below middle grey over which the print's shadow expansion ramps in.
constexpr float V3_PRINT_TOE_STOPS = 4.f;

// Where middle grey sits on Custom's straight line, in stops above base fog.
// Large enough that diffuse white and three stops of specular clear D-max,
// small enough that scene black reaches fog instead of floating above it.
constexpr float V3_CUSTOM_STRAIGHT_OFFSET = 9.5f;

// A negative's own gamma spans 0.84-1.22 across this table, but the medium it
// is printed onto compensates: Portra on RA-4 and Velvia projected land at
// similar system gammas, and what separates them is toe and shoulder shape and
// colour, not global contrast. Passing the stock's gamma through at full
// weight makes the soft stocks flatter than the source, which is the wash.
constexpr float V3_STOCK_CONTRAST_WEIGHT = 0.70f;


// V4, scene mode only: the input still carries real highlight magnitude, so
// the source term is the radiant energy past the onset - the light the
// emulsion could not absorb, which is what actually reaches the base and
// comes back. A streetlight five stops over exposes the halo many times
// harder than a white wall half a stop over; the "how blown is it" guesswork
// in halationHighlightSource exists only because the V2/V3 signal is clipped
// to 1.0 before the stage runs.
// Overall fraction of the excess radiance the V4 halo may deposit. Real
// anti-halation undercoats absorb the large majority of what reaches the
// base; the visible effect is a bright local fringe precisely BECAUSE the
// energy is small and concentrated at the contour.
constexpr float V4_HALATION_ENERGY = 1.25f;

inline float halationHighlightSourceV4(float luminanceValue, float peakValue, float onsetStops)
{
    const float highlight = std::max(luminanceValue, peakValue * 0.82f);
    const float onset = 0.18f * std::exp2(V3_WHITE_STOPS + onsetStops);
    const float excess = highlight / onset - 1.f;

    if (excess <= 0.f) {
        return 0.f;
    }

    // Three stops over the onset reads as nominal full strength; beyond that
    // the term keeps growing gently so the halo brightness still ranks light
    // sources, but a single blown streetlamp cannot swamp the blur pyramid.
    const float normalized = excess * (1.f / 7.f);
    return normalized / (1.f + 0.25f * normalized);
}

struct FilmLabV3Profile {
    std::array<float, 3> layerSpeed;
    std::array<float, 3> layerGamma;
    std::array<float, 3> layerToe;
    std::array<float, 3> layerShoulder;
    std::array<float, 9> spectral;
    std::array<float, 3> halationColor;
    float baseFog;
    float maxDensity;
    float coupling;
    float grainMidpoint;
    float grainWidth;
    float grainColor;
    float grainClumping;
    float halationInner;
    float halationOuter;
    float bloom;
    float acutance;
};

struct FilmLabV3Process {
    float speed;
    float gamma;
    float toe;
    float shoulder;
    float saturation;
    float grain;
    float fog;
};

struct FilmLabV3Output {
    float gamma;        // system gamma of negative plus print
    float toe;          // print's deep-shadow expansion
    float saturation;
    float warmth;
    std::array<float, 9> matrix;
    float softness;
};

struct FilmLabV3CurveBank {
    std::array<std::array<float, V3_DENSITY_LUT_SIZE + 1>, 3> density;
    std::array<std::array<float, V3_OUTPUT_LUT_SIZE + 1>, 3> output;
    std::array<float, 3> referenceDensity;
    float baseFog;
    float maxDensity;
    float outputIndexScale;
    bool stopsIndexed = false; // V4: density grid runs over log2 exposure

    float sampleDensity(int channel, float value) const
    {
        if (stopsIndexed) {
            const float stops = std::log2(std::max(value, 1e-7f) / 0.18f);
            const float coordinate = LIM(
                (stops - V4_STOPS_MIN) * (static_cast<float>(V3_DENSITY_LUT_SIZE) / (V4_STOPS_MAX - V4_STOPS_MIN)),
                0.f,
                static_cast<float>(V3_DENSITY_LUT_SIZE));
            const int index = std::min(static_cast<int>(coordinate), V3_DENSITY_LUT_SIZE - 1);
            return intp(coordinate - index, density[channel][index + 1], density[channel][index]);
        }

        const float coordinate = LIM(value, 0.f, V3_MAX_INPUT)
            * (static_cast<float>(V3_DENSITY_LUT_SIZE) / V3_MAX_INPUT);
        const int index = std::min(static_cast<int>(coordinate), V3_DENSITY_LUT_SIZE - 1);
        return intp(coordinate - index, density[channel][index + 1], density[channel][index]);
    }

    float sampleOutput(int channel, float value) const
    {
        const float coordinate = LIM((value - baseFog) * outputIndexScale, 0.f, static_cast<float>(V3_OUTPUT_LUT_SIZE));
        const int index = std::min(static_cast<int>(coordinate), V3_OUTPUT_LUT_SIZE - 1);
        return intp(coordinate - index, output[channel][index + 1], output[channel][index]);
    }
};

inline float softplus(float value, float softness)
{
    const float scaled = value / std::max(softness, 0.02f);
    if (scaled > 12.f) {
        return value;
    }
    if (scaled < -12.f) {
        return 0.f;
    }
    return softness * std::log1p(std::exp(scaled));
}

FilmLabV3Profile makeV3Profile(const FilmLabStock& stock)
{
    FilmLabV3Profile profile;
    profile.layerSpeed = {{stock.redBias, stock.greenBias, stock.blueBias}};
    profile.layerGamma = {{
        stock.contrast * 1.012f,
        stock.contrast,
        stock.contrast * 0.988f
    }};
    profile.layerToe = {{stock.toe * 1.05f, stock.toe, stock.toe * 0.94f}};
    profile.layerShoulder = {{stock.shoulder * 1.08f, stock.shoulder, stock.shoulder * 0.92f}};
    profile.spectral = {{
        0.955f, 0.038f, 0.007f,
        0.025f, 0.950f, 0.025f,
        0.006f, 0.055f, 0.939f
    }};
    profile.halationColor = {{0.80f, 0.17f, 0.03f}};
    profile.baseFog = 0.055f;
    profile.maxDensity = 2.55f;
    profile.coupling = 0.16f;
    profile.grainMidpoint = 0.48f;
    profile.grainWidth = 0.31f;
    profile.grainColor = 0.16f;
    profile.grainClumping = 0.28f;
    profile.halationInner = 0.54f;
    profile.halationOuter = 0.46f;
    profile.bloom = 0.025f;
    profile.acutance = stock.acutance;

    switch (stock.stockClass) {
        case StockClass::Reversal:
            profile.spectral = {{
                0.978f, 0.018f, 0.004f,
                0.012f, 0.976f, 0.012f,
                0.003f, 0.034f, 0.963f
            }};
            profile.baseFog = 0.025f;
            profile.maxDensity = 2.85f;
            profile.coupling = 0.08f;
            profile.grainColor = 0.10f;
            profile.grainClumping = 0.18f;
            profile.halationInner = 0.68f;
            profile.halationOuter = 0.32f;
            break;
        case StockClass::MotionNegative:
            profile.spectral = {{
                0.932f, 0.058f, 0.010f,
                0.034f, 0.934f, 0.032f,
                0.008f, 0.074f, 0.918f
            }};
            profile.halationColor = {{0.86f, 0.12f, 0.02f}};
            profile.baseFog = 0.065f;
            profile.maxDensity = 2.48f;
            profile.coupling = 0.22f;
            profile.grainColor = 0.13f;
            profile.grainClumping = 0.24f;
            profile.halationInner = 0.42f;
            profile.halationOuter = 0.58f;
            profile.bloom = 0.018f;
            break;
        case StockClass::Monochrome:
            profile.spectral = {{
                0.27f, 0.66f, 0.07f,
                0.27f, 0.66f, 0.07f,
                0.27f, 0.66f, 0.07f
            }};
            profile.halationColor = {{0.34f, 0.33f, 0.33f}};
            profile.baseFog = 0.075f;
            profile.maxDensity = 2.72f;
            profile.coupling = 0.f;
            profile.grainColor = 0.f;
            profile.grainClumping = 0.40f;
            break;
        case StockClass::Creative:
            profile.maxDensity = 2.35f;
            profile.coupling = 0.20f;
            profile.grainColor = 0.20f;
            profile.grainClumping = 0.36f;
            profile.bloom = 0.055f;
            break;
        case StockClass::Custom:
            profile.spectral = {{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f}};
            profile.halationColor = {{0.78f, 0.18f, 0.04f}};
            profile.baseFog = 0.05f;
            profile.maxDensity = 2.55f;
            profile.coupling = 0.f;
            profile.grainColor = 0.12f;
            profile.grainClumping = 0.24f;
            profile.bloom = 0.f;
            break;
        case StockClass::ColorNegative:
            break;
    }

    const float speedGrain = LIM(std::sqrt(std::max(stock.speed, 25.f) / 200.f), 0.55f, 2.25f);
    profile.grainWidth = LIM(profile.grainWidth * (0.94f + 0.07f * speedGrain), 0.20f, 0.48f);
    profile.grainClumping = LIM(profile.grainClumping + stock.grain * 0.16f, 0.08f, 0.58f);
    return profile;
}

FilmLabV3Process makeV3Process(const Glib::ustring& process)
{
    FilmLabV3Process profile = {0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f};
    if (process == "e6") {
        profile.gamma = 1.10f;
        profile.toe = -0.05f;
        profile.shoulder = -0.08f;
        profile.saturation = 1.09f;
        profile.grain = 0.78f;
        profile.fog = -0.012f;
    } else if (process == "ecn2") {
        profile.gamma = 0.91f;
        profile.shoulder = 0.16f;
        profile.saturation = 0.93f;
        profile.grain = 0.90f;
        profile.fog = 0.006f;
    } else if (process == "bw") {
        profile.gamma = 1.05f;
        profile.toe = 0.08f;
        profile.saturation = 0.f;
        profile.grain = 1.14f;
        profile.fog = 0.012f;
    }
    return profile;
}

// A negative is not a picture until something prints it, and the system gamma
// of negative plus print is what decides whether the result reads as a
// photograph or a flat scan. The first V3 set these to 1.00-1.12, which made
// the print undo the negative almost exactly: the stage could only ever
// subtract contrast, never add it.
FilmLabV3Output makeV3Output(const Glib::ustring& output)
{
    FilmLabV3Output profile = {
        1.24f, 0.16f, 1.f, 0.f,
        {{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f}},
        0.f
    };
    if (output == "ra4") {
        profile.gamma = 1.36f;
        profile.toe = 0.12f;
        profile.saturation = 1.035f;
        profile.warmth = 0.018f;
        profile.matrix = {{
            1.012f, -0.006f, -0.006f,
            -0.004f, 1.010f, -0.006f,
            -0.004f, 0.012f, 0.992f
        }};
        profile.softness = 0.08f;
    } else if (output == "labscan") {
        // Minilab inversion pipelines run contrastier and a touch richer
        // than a neutral Status-M read; the spectral side of the look lives
        // in the labscan receiver chain in makeV4PrintLUT.
        profile.gamma = 1.30f;
        profile.toe = 0.14f;
        profile.saturation = 1.05f;
        profile.matrix = {{
            1.006f, -0.004f, -0.002f,
            -0.003f, 1.008f, -0.005f,
            -0.002f, 0.006f, 0.996f
        }};
        profile.softness = 0.f;
    } else if (output == "projection") {
        profile.gamma = 1.46f;
        profile.toe = 0.10f;
        profile.saturation = 1.075f;
        profile.matrix = {{
            1.018f, -0.010f, -0.008f,
            -0.006f, 1.016f, -0.010f,
            -0.005f, -0.008f, 1.013f
        }};
        profile.softness = -0.05f;
    } else if (output == "cinema") {
        profile.gamma = 1.28f;
        profile.toe = 0.14f;
        profile.saturation = 0.965f;
        profile.warmth = 0.012f;
        profile.matrix = {{
            0.986f, 0.012f, 0.002f,
            0.008f, 0.984f, 0.008f,
            0.003f, 0.020f, 0.977f
        }};
        profile.softness = 0.12f;
    }
    return profile;
}

inline float v3DensityResponse(
    float value,
    float exposure,
    float gamma,
    float toe,
    float shoulder,
    float baseFog,
    float maxDensity,
    bool straight)
{
    const float exposed = std::max(value * std::exp2(exposure), 1e-8f);
    const float stops = std::log2(exposed / 0.18f);

    if (straight) {
        // Custom is not a film stock, so it gets no toe and no shoulder at
        // all: a pure straight line, which the print inverts exactly. Nothing
        // the user has not asked for happens.
        return LIM(baseFog + gamma * (stops + V3_CUSTOM_STRAIGHT_OFFSET), baseFog, maxDensity);
    }

    // A colour negative carries roughly six stops below middle grey before the
    // toe takes over. The first V3 put the toe at -2.75, which threw the
    // shadow range away and then had to invent it back, lifting blacks by two
    // stops. Placing it where real film puts it is most of the fix.
    const float toeStart = -6.20f + toe * 1.60f;
    const float shoulderStart = 3.60f - shoulder * 0.90f;
    const float toeSoftness = 0.55f + toe * 1.10f;
    const float shoulderSoftness = 0.55f + shoulder * 0.75f;
    const float density = baseFog + gamma * (
        softplus(stops - toeStart, toeSoftness)
        - softplus(stops - shoulderStart, shoulderSoftness));
    return LIM(density, baseFog, maxDensity);
}

// shape(0) = 0 and shape(1) = 1 for every k, so diffuse white lands on paper
// white by construction and the slope at middle grey is free to be the system
// gamma. k > 0 compresses highlights into a shoulder, k < 0 expands them.
inline float printShape(float t, float k)
{
    if (std::fabs(k) < 1e-6f) {
        return t;
    }
    return (1.f - std::exp(-k * t)) / (1.f - std::exp(-k));
}

inline float printShapeSlope(float t, float k)
{
    if (std::fabs(k) < 1e-6f) {
        return 1.f;
    }
    return k * std::exp(-k * t) / (1.f - std::exp(-k));
}

// How far past paper white the V4 print may reach, in output stops. The
// print shape above was built for input capped at diffuse white; fed real
// scene radiance its solved k can be small or negative, and the soft stocks
// then EXPAND super-white input without bound (twilight_160 measured 8x
// diffuse white at 2.5x linear). A real print cannot: just past the exposure
// that prints diffuse white the paper sits on Dmin and the sheet has nothing
// left to give. A third of a stop of sparkle is what survives in practice.
// Keep in step with PRINT_OVERSHOOT_STOPS in tools/filmsim_probe.py.
constexpr float V4_PRINT_OVERSHOOT_STOPS = 0.35f;

// k such that printShape'(0) == slope.
inline float solveShoulderK(float slope)
{
    if (std::fabs(slope - 1.f) < 1e-4f) {
        return 0.f;
    }
    float lo = slope > 1.f ? 0.f : -40.f;
    float hi = slope > 1.f ? 40.f : 0.f;
    for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float value = std::fabs(mid) < 1e-9f ? 1.f : mid / (1.f - std::exp(-mid));
        if (value < slope) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

// Integral of smoothstep(0, V3_PRINT_TOE_STOPS, t), so the print's shadow
// expansion is defined by its *slope* and that slope stays bounded and smooth.
// Scaling the value instead put a spike in the derivative at the knee, which
// blocked up shadows on the contrastier stocks.
inline float printToeIntegral(float t)
{
    const float u = std::min(t / V3_PRINT_TOE_STOPS, 1.f);
    const float u3 = u * u * u;
    return V3_PRINT_TOE_STOPS * (u3 - 0.5f * u3 * u) + std::max(t - V3_PRINT_TOE_STOPS, 0.f);
}

inline float printGrade(float stops, float systemGamma, float shoulderK, float whiteStops, float toe, float overshoot)
{
    if (stops >= 0.f) {
        if (overshoot > 0.f && stops > whiteStops) {
            // Paper saturation: value- and slope-continuous at diffuse
            // white, approaching paper white + overshoot asymptotically.
            const float slope = V3_WHITE_STOPS * printShapeSlope(1.f, shoulderK) / whiteStops;
            const float excess = stops - whiteStops;
            return V3_WHITE_STOPS + overshoot * (1.f - std::exp(-slope * excess / overshoot));
        }

        return V3_WHITE_STOPS * printShape(stops / whiteStops, shoulderK);
    }

    // Below middle grey the print keeps the emulsion's slope, then ramps in a
    // deep-shadow expansion that reaches full strength four stops down. That
    // is the paper's toe: normal mid tones, and blacks that land on black
    // rather than on base fog.
    return -systemGamma * (-stops + toe * printToeIntegral(-stops));
}

FilmLabV3CurveBank makeV3Curves(
    const FilmLabV3Profile& stock,
    const FilmLabV3Process& process,
    const FilmLabV3Output& output,
    const procparams::FilmPresetsParams& fp,
    bool straight,
    bool stopsIndexed)
{
    FilmLabV3CurveBank curves;
    curves.stopsIndexed = stopsIndexed;
    const float pushPull = LIM(static_cast<float>(fp.pushPull), -2.f, 3.f);
    const float exposure = LIM(static_cast<float>(fp.exposure), -4.f, 4.f) - pushPull * 0.32f;
    const float userContrast = LIM(1.f + fp.contrast / 240.f, 0.58f, 1.48f);
    const float userToe = fp.fade / 250.f;
    const float userShoulder = fp.rolloff / 210.f;
    const float channelShift[3] = {
        fp.redShift / 500.f,
        fp.greenShift / 500.f,
        fp.blueShift / 500.f
    };
    curves.baseFog = LIM(stock.baseFog + process.fog + std::max(fp.fade, 0) / 2200.f, 0.005f, 0.22f);
    curves.maxDensity = LIM(stock.maxDensity + pushPull * 0.07f - std::max(fp.fade, 0) / 900.f, 1.65f, 3.15f);
    curves.outputIndexScale = V3_OUTPUT_LUT_SIZE / std::max(curves.maxDensity - curves.baseFog, 1e-5f);

    float layerToe[3];
    float layerGamma[3];

    for (int channel = 0; channel < 3; ++channel) {
        const float gamma = LIM(
            V3_REFERENCE_GAMMA
            * std::pow(std::max(stock.layerGamma[channel] * process.gamma, 0.05f), V3_STOCK_CONTRAST_WEIGHT)
            * userContrast * (1.f + pushPull * 0.065f),
            0.06f,
            0.45f);
        const float toe = LIM(stock.layerToe[channel] + process.toe - pushPull * 0.025f + userToe, 0.f, 1.15f);
        const float shoulder = LIM(stock.layerShoulder[channel] + process.shoulder + userShoulder, 0.f, 1.45f);
        const float baselineExposure = stock.layerSpeed[channel] + process.speed;
        const float effectiveExposure = baselineExposure + exposure + channelShift[channel];
        layerToe[channel] = toe;
        layerGamma[channel] = gamma;

        for (int i = 0; i <= V3_DENSITY_LUT_SIZE; ++i) {
            const float value = stopsIndexed
                ? 0.18f * std::exp2(V4_STOPS_MIN + (V4_STOPS_MAX - V4_STOPS_MIN) * i / V3_DENSITY_LUT_SIZE)
                : V3_MAX_INPUT * i / V3_DENSITY_LUT_SIZE;
            curves.density[channel][i] = v3DensityResponse(
                value,
                effectiveExposure,
                gamma,
                toe,
                shoulder,
                curves.baseFog,
                curves.maxDensity,
                straight);
        }

        // The colour head's filtration: a per-channel offset that holds middle
        // grey neutral. It is an offset and not a slope, so per-layer gamma
        // differences survive as crossover instead of cancelling out - which
        // is what used to leave every stock rendering the same picture.
        curves.referenceDensity[channel] = v3DensityResponse(
            0.18f,
            baselineExposure,
            gamma,
            toe,
            shoulder,
            curves.baseFog,
            curves.maxDensity,
            straight);
    }

    // Where diffuse white lands after the emulsion, read off the green layer at
    // its baseline exposure so the user's own exposure is not silently undone,
    // and shared across channels so a layer's shoulder shows as highlight
    // colour rather than being balanced away.
    const float whiteDensity = v3DensityResponse(
        1.f,
        stock.layerSpeed[1] + process.speed,
        layerGamma[1],
        layerToe[1],
        LIM(stock.layerShoulder[1] + process.shoulder + userShoulder, 0.f, 1.45f),
        curves.baseFog,
        curves.maxDensity,
        straight);
    const float whiteStops = std::max(
        (whiteDensity - curves.referenceDensity[1]) / V3_REFERENCE_GAMMA,
        0.35f);
    const float systemGamma = straight ? 1.f : output.gamma;
    const float shoulderK = solveShoulderK(systemGamma * whiteStops / V3_WHITE_STOPS);

    // Paper saturation past diffuse white only exists for V4: it is the
    // print-side counterpart of scene-referred input, and Custom stays a
    // straight line by contract.
    const float overshoot = (stopsIndexed && !straight) ? V4_PRINT_OVERSHOOT_STOPS : 0.f;

    for (int channel = 0; channel < 3; ++channel) {
        const float printToe = straight
            ? 0.f
            : output.toe * LIM(layerToe[channel] / V3_REFERENCE_TOE, 0.15f, 2.f);

        for (int i = 0; i <= V3_OUTPUT_LUT_SIZE; ++i) {
            const float density = curves.baseFog
                + (curves.maxDensity - curves.baseFog) * i / V3_OUTPUT_LUT_SIZE;
            const float stops = (density - curves.referenceDensity[channel]) / V3_REFERENCE_GAMMA;
            curves.output[channel][i] = LIM(
                0.18f * std::exp2(printGrade(stops, systemGamma, shoulderK, whiteStops, printToe, overshoot)),
                0.f,
                V3_MAX_INPUT);
        }
    }
    return curves;
}

inline void toCanonicalV3(const LabImage* lab, int row, int col, const float inverse[3][3], float& r, float& g, float& b)
{
    float x;
    float y;
    float z;
    Color::Lab2XYZ(lab->L[row][col], lab->a[row][col], lab->b[row][col], x, y, z);
    Color::xyz2rgb(x, y, z, r, g, b, inverse);
    r /= MAXVALF;
    g /= MAXVALF;
    b /= MAXVALF;
}

// Reconstructs the V4 film exposure for one pixel. The film input is the
// unclipped rgbProc tap carried to canonical AP1, times whatever gain the
// Lab-domain tools (shadows/highlights, L curve, wavelets, ...) applied on
// top of rgbProc's output - so those edits survive as local dodges and burns
// while the tone curve and the out-of-gamut clip, which both sit between the
// tap and the snapshot, are excluded by construction. The ratio is softened
// so pixels the tone curve crushed to zero cannot explode it, and capped
// because a gain past 32x is an edit artefact, not light.
struct FilmLabV4SceneFetch {
    const Imagefloat* tap;
    const LabImage* snapshot;
    const LabImage* lab;
    const float (*labInverse)[3]; // XYZ -> AP1, shared with the V3 path
    float tapMatrix[3][3];        // working RGB -> AP1, 1/MAXVALF folded in

    void fetch(int row, int col, float& sceneR, float& sceneG, float& sceneB,
               float& labR, float& labG, float& labB, float* displayGain = nullptr) const
    {
        const float tr = tap->r(row, col);
        const float tg = tap->g(row, col);
        const float tb = tap->b(row, col);
        const float rawR = tapMatrix[0][0] * tr + tapMatrix[0][1] * tg + tapMatrix[0][2] * tb;
        const float rawG = tapMatrix[1][0] * tr + tapMatrix[1][1] * tg + tapMatrix[1][2] * tb;
        const float rawB = tapMatrix[2][0] * tr + tapMatrix[2][1] * tg + tapMatrix[2][2] * tb;

        toCanonicalV3(lab, row, col, labInverse, labR, labG, labB);

        float snapR;
        float snapG;
        float snapB;
        toCanonicalV3(snapshot, row, col, labInverse, snapR, snapG, snapB);

        constexpr float soften = 0.004f;
        const float gainR = LIM((labR + soften) / (snapR + soften), 0.f, 32.f);
        const float gainG = LIM((labG + soften) / (snapG + soften), 0.f, 32.f);
        const float gainB = LIM((labB + soften) / (snapB + soften), 0.f, 32.f);
        sceneR = std::max(rawR * gainR, 0.f);
        sceneG = std::max(rawG * gainG, 0.f);
        sceneB = std::max(rawB * gainB, 0.f);

        // The user's display-domain grading - tone curve, contrast, RGB
        // curves, HSV, colour toning - lives between the tap and the
        // snapshot, and the film exposure rightly ignores it (that is what
        // un-clips the highlights). But ignoring it entirely left every
        // tone control DEAD in V4: an S-curve no longer deepened shadows,
        // which read as the film "washing out dark areas". So the same
        // edits are measured here as per-channel gains against the clipped
        // tap and handed back to the caller to apply to the film's OUTPUT:
        // grading the print, exactly like the darkroom would.
        if (displayGain) {
            displayGain[0] = LIM((snapR + soften) / (LIM(rawR, 0.f, 1.f) + soften), 0.05f, 8.f);
            displayGain[1] = LIM((snapG + soften) / (LIM(rawG, 0.f, 1.f) + soften), 0.05f, 8.f);
            displayGain[2] = LIM((snapB + soften) / (LIM(rawB, 0.f, 1.f) + soften), 0.05f, 8.f);
        }
    }
};

inline void applyV3Spectral(const FilmLabV3Profile& profile, float r, float g, float b, float density[3])
{
    r = std::max(r, 0.f);
    g = std::max(g, 0.f);
    b = std::max(b, 0.f);
    density[0] = profile.spectral[0] * r + profile.spectral[1] * g + profile.spectral[2] * b;
    density[1] = profile.spectral[3] * r + profile.spectral[4] * g + profile.spectral[5] * b;
    density[2] = profile.spectral[6] * r + profile.spectral[7] * g + profile.spectral[8] * b;
}

// V4 layer-exposure crosstalk, derived rather than hand-tuned: class-plausible
// spectral sensitivity lobes integrated against a partition-of-unity
// reflectance basis under a 5500K illuminant, rows normalised so grey maps to
// equal layer exposures and the tone pipeline stays untouched. Generated by
// tools/v4_spectral_gen.py - edit the shapes there, rerun, and paste; the two
// must stay in step. The V3 matrices in makeV3Profile were nearly diagonal
// (0.93-0.98), which no real emulsion achieves; the crosstalk this restores
// is what the V4 couplers below then work against, exactly as in real film.
// How much of the full physical crosstalk each layer's record carries to
// the output. A real negative-print system recovers most of the
// exposure-stage overlap later: masking couplers and the paper's spectral
// response undo unwanted absorptions at print time. Until Track B's
// spectral print stage exists there is nothing downstream to do that work,
// so each matrix row is blended toward identity - "the crosstalk that
// survives the whole system". Raised from {0.42, 0.28, 0.42} once the
// spectral print chains landed; 0.68 measurably breaks (warm colours gain
// +3-4 dL and every output chain drifts - the pairwise couplers cannot
// counterbalance more without the full per-layer spectral H&D), so this
// is the ceiling for the current architecture. The green record keeps the least: the masking couplers'
// primary job in real film is protecting the magenta image, whose layer
// has the broadest sensitivity and would otherwise wash the magenta-green
// axis (measured here: -5 to -9 dC on the magenta patch, +3 dL on blue,
// both traced to green-row off-diagonals).
// Per-stock V4 character, expressed through the PHYSICAL dials the model
// now has instead of the legacy scalar table: interimage strength, mask
// quality, dye purity, and red/blue layer crossover. The green layer is
// never touched - the tone gates in tools/filmsim_probe.py model the green
// axis, and mid-grey neutrality re-calibrates per channel anyway, so
// crossover shows as shadow/highlight colour, exactly like real stock.
// Stocks stay intentionally fictional; these are class-plausible
// characters, not measurements of any branded product.
struct FilmLabV4Character {
    float couplingMul = 1.f;
    float maskEfficiencyMul = 1.f;
    float impurityMul = 1.f;
    float redGammaMul = 1.f;
    float blueGammaMul = 1.f;
    float redToeAdd = 0.f;
    float blueToeAdd = 0.f;
};

inline FilmLabV4Character makeV4Character(const Glib::ustring& preset)
{
    FilmLabV4Character c;

    if (preset == "heritage_gold") {          // consumer gold: friendly, golden
        c.couplingMul = 0.90f; c.maskEfficiencyMul = 0.95f; c.impurityMul = 1.15f;
        c.redGammaMul = 1.020f; c.blueGammaMul = 0.985f;
    } else if (preset == "porcelain_400") {   // portrait: gentle separations
        c.couplingMul = 0.85f; c.maskEfficiencyMul = 1.01f; c.impurityMul = 0.90f;
        c.redToeAdd = 0.02f;
    } else if (preset == "golden_hour") {     // warm keeper of low sun
        c.maskEfficiencyMul = 0.97f;
        c.redGammaMul = 1.022f; c.blueGammaMul = 0.982f;
    } else if (preset == "nostalgia_200") {   // aged consumer chemistry
        c.couplingMul = 0.80f; c.maskEfficiencyMul = 0.87f; c.impurityMul = 1.30f;
        c.blueToeAdd = 0.04f;
    } else if (preset == "street_800") {      // fast, punchy, blue shadows
        c.couplingMul = 1.10f; c.impurityMul = 1.10f;
        c.blueToeAdd = 0.05f; c.blueGammaMul = 1.012f;
    } else if (preset == "vivid_chrome") {    // the loud slide
        c.couplingMul = 1.15f; c.impurityMul = 0.80f;
    } else if (preset == "arctic") {          // cool, clinical slide
        c.couplingMul = 0.95f; c.blueGammaMul = 1.015f; c.redGammaMul = 0.990f;
    } else if (preset == "desert_chrome") {   // older warm chrome chemistry
        c.couplingMul = 0.90f; c.impurityMul = 1.20f;
        c.redGammaMul = 1.018f; c.blueGammaMul = 0.980f;
    } else if (preset == "twilight_160") {    // soft tungsten motion stock
        c.couplingMul = 0.90f; c.maskEfficiencyMul = 1.01f;
    } else if (preset == "cinematic_500t") {  // the night stock
        c.couplingMul = 1.05f; c.impurityMul = 1.10f;
    } else if (preset == "fade_bloom") {      // deliberately faded
        c.couplingMul = 0.80f; c.impurityMul = 1.30f;
    } else if (preset == "ember") {           // warm creative
        c.couplingMul = 1.05f; c.impurityMul = 1.10f;
        c.redGammaMul = 1.018f;
    } else if (preset == "analog_dream") {    // the dreamiest
        c.couplingMul = 0.75f; c.impurityMul = 1.40f;
        c.redToeAdd = 0.03f;
    }

    return c;
}

constexpr float V4_SPECTRAL_STRENGTH[3] = {0.55f, 0.37f, 0.55f};

inline std::array<float, 9> makeV4SpectralMatrix(StockClass stockClass)
{
    std::array<float, 9> full;

    switch (stockClass) {
        case StockClass::Reversal:
            full = {{
                0.8538f, 0.1359f, 0.0102f,
                0.1889f, 0.6391f, 0.1720f,
                0.0036f, 0.1058f, 0.8906f
            }};
            break;
        case StockClass::MotionNegative:
            full = {{
                0.7252f, 0.2483f, 0.0264f,
                0.3071f, 0.5491f, 0.1438f,
                0.0126f, 0.2016f, 0.7858f
            }};
            break;
        default: // ColorNegative and Creative
            full = {{
                0.7852f, 0.1968f, 0.0180f,
                0.2514f, 0.5929f, 0.1557f,
                0.0087f, 0.1761f, 0.8153f
            }};
            break;
    }

    // Rows of both endpoints sum to one, so the blend keeps grey mapping to
    // equal layer exposures without renormalising.
    for (int i = 0; i < 9; ++i) {
        const float identity = (i % 4 == 0) ? 1.f : 0.f;
        full[i] = identity + (full[i] - identity) * V4_SPECTRAL_STRENGTH[i / 3];
    }

    return full;
}

// DIR couplers, V4: development in one layer releases inhibitors that
// suppress development in the others. The Langmuir isotherm saturates the
// inhibitor release as density builds, and only the DIFFERENCE between
// layers acts - so neutral grey is untouched by construction (the colour
// head calibration stays valid), while any colour difference deepens:
// the interimage effect that real film uses to buy back the saturation its
// own spectral crosstalk costs. This replaces V3's zone-weighted density
// mixing, which was a static grade keyed to mean density.
constexpr float V4_COUPLER_K = 0.35f;
constexpr float V4_COUPLER_GAIN = 0.80f;

// The stock table's coupling constants were tuned for V3's desaturating
// zone-mix, where reversal wanted LESS of it. Real interimage runs the
// other way: E-6's inhibition is the strongest in the business - it is why
// slides stay saturated despite their dyes' unwanted absorptions - so the
// class factor rebalances the table for the V4 restorative couplers
// without touching V3.
inline float v4CouplerClassGain(StockClass stockClass)
{
    switch (stockClass) {
        case StockClass::Reversal:
            return 2.8f;
        case StockClass::MotionNegative:
            return 1.1f;
        default:
            return 1.0f;
    }
}

inline float v4LangmuirInhibitor(float density, float baseFog, float inverseRange)
{
    const float normalized = LIM((density - baseFog) * inverseRange, 0.f, 1.f);
    return normalized / (normalized + V4_COUPLER_K);
}

inline void applyV4Couplers(float density[3], float amount, float baseFog, float maxDensity)
{
    if (amount <= 0.0001f) {
        return;
    }

    const float range = std::max(maxDensity - baseFog, 1e-5f);
    const float inverseRange = 1.f / range;
    const float inhibitor[3] = {
        v4LangmuirInhibitor(density[0], baseFog, inverseRange),
        v4LangmuirInhibitor(density[1], baseFog, inverseRange),
        v4LangmuirInhibitor(density[2], baseFog, inverseRange)
    };
    const float gain = amount * V4_COUPLER_GAIN * range;

    // Pairwise inter-layer inhibition weights - per-layer DIR coupler
    // loading, a real emulsion-design dial. The green row is heaviest: the
    // green layer's broad sensitivity is what washes the magenta-green
    // axis, and it needs strong inhibition from BOTH neighbours to hold
    // magenta (two dense rivals push together). On a blue subject the two
    // pushes largely cancel, which a mean-differential formulation cannot
    // express - it either starves magenta or over-drives blue.
    constexpr float pairWeight[3][3] = {
        {0.f, 1.6f, 1.f},
        {2.4f, 0.f, 2.4f},
        {1.f, 1.6f, 0.f}
    };

    for (int channel = 0; channel < 3; ++channel) {
        float differential = 0.f;
        for (int other = 0; other < 3; ++other) {
            differential += pairWeight[channel][other] * (inhibitor[channel] - inhibitor[other]);
        }

        // A layer denser than its rivals is pushed further up, a thinner
        // one further down. The push may never take more than a fraction
        // of the headroom left toward fog or Dmax: development cannot
        // remove silver that never formed, and a hard clamp here parks
        // saturated colours in a regime where the gain constant stops
        // doing anything at all.
        float delta = gain * 0.5f * differential;
        const float headroom = delta > 0.f
            ? maxDensity - density[channel]
            : density[channel] - baseFog;
        const float limit = 0.55f * std::max(headroom, 0.f);

        if (delta > limit) {
            delta = limit;
        } else if (delta < -limit) {
            delta = -limit;
        }

        density[channel] += delta;
    }
}

// ---------------------------------------------------------------------------
// V4 spectral print-through (LUT-B v1).
//
// The developed image is three dye clouds, and dyes are not clean: cyan
// absorbs some green and blue it should pass, magenta eats a large bite of
// blue, yellow nibbles green. What an output medium reads through those
// dyes therefore couples the three records - differently for a scanner's
// narrow bands, RA-4 paper's far-red cyan channel, or a projector's broad
// view. Colour negative film corrects most of its own impurities with
// colored masking couplers: the coupler starts colored exactly like its
// dye's unwanted absorption and is consumed as dye forms, so unwanted
// absorption stays CONSTANT across exposure (that constant is the orange
// base, and the colour head's filtration eats it). Reversal has no mask,
// which is a real part of why slides render colour the way they do.
//
// Runtime shape: everything is baked into a small 3D LUT of per-channel
// ratios against the neutral axis - out_i = printLUT_i(d_i) * S_i(d) /
// S_i(d_i, d_i, d_i) - so on neutral input the ratio is exactly 1 and the
// V3-calibrated tone pipeline is preserved bit-for-bit by construction;
// only colour interactions are added. Tone gates in tools/filmsim_probe.py
// therefore need no counterpart of this; the empirical gate is the
// ColorChecker probe (tools/v4_color_probe.py).
// ---------------------------------------------------------------------------

constexpr int V4_PRINT_BINS = 36;        // 380..730nm, 10nm
constexpr int V4_PRINT_LUT_SIZE = 21;
constexpr float V4_PRINT_DYE_SCALE = 0.85f;

inline float v4SpectralGauss(float lambda, float mu, float sigma)
{
    const float d = (lambda - mu) / sigma;
    return std::exp(-0.5f * d * d);
}

struct FilmLabV4PrintLUT {
    bool active = false;
    float baseFog = 0.f;
    float indexScale = 0.f;
    std::vector<float> ratio; // SIZE^3 * 3, innermost = channel

    void sample(const float density[3], float out[3]) const
    {
        constexpr int N = V4_PRINT_LUT_SIZE;
        float fx[3];
        int i0[3];
        for (int c = 0; c < 3; ++c) {
            const float x = LIM((density[c] - baseFog) * indexScale, 0.f, static_cast<float>(N - 1));
            i0[c] = std::min(static_cast<int>(x), N - 2);
            fx[c] = x - i0[c];
        }
        for (int c = 0; c < 3; ++c) {
            float accum = 0.f;
            for (int corner = 0; corner < 8; ++corner) {
                const int di = corner & 1;
                const int dj = (corner >> 1) & 1;
                const int dk = (corner >> 2) & 1;
                const float w = (di ? fx[0] : 1.f - fx[0])
                    * (dj ? fx[1] : 1.f - fx[1])
                    * (dk ? fx[2] : 1.f - fx[2]);
                const int index = (((i0[0] + di) * N + (i0[1] + dj)) * N + (i0[2] + dk)) * 3 + c;
                accum += w * ratio[index];
            }
            out[c] = accum;
        }
    }
};

void makeV4PrintLUT(
    FilmLabV4PrintLUT& lut,
    const FilmLabV3CurveBank& curves,
    StockClass stockClass,
    const Glib::ustring& outputName,
    bool multiThread,
    float maskEfficiencyMul = 1.f,
    float impurityMul = 1.f)
{
    constexpr int N = V4_PRINT_LUT_SIZE;

    // Dye spectral densities, normalised to unit main peak. The unwanted
    // lobes are the whole point: they are what couples the records.
    // Channel order follows the records: 0 = red record -> cyan dye,
    // 1 = green -> magenta, 2 = blue -> yellow.
    struct Lobe { float weight, mu, sigma; };
    static const std::vector<Lobe> DYES[3] = {
        {{1.00f, 655.f, 45.f}, {0.20f, 545.f, 45.f}, {0.09f, 435.f, 35.f}},
        {{1.00f, 545.f, 42.f}, {0.32f, 435.f, 32.f}, {0.06f, 640.f, 50.f}},
        {{1.00f, 442.f, 38.f}, {0.06f, 545.f, 40.f}}
    };


    // Reversal's azomethine dyes are engineered much cleaner than a
    // negative's - they have to be, there is no mask and no print stage to
    // correct them. The impurity factor scales the unwanted lobes.
    float maskEfficiency;
    float dyeImpurity = 1.f;
    switch (stockClass) {
        case StockClass::Reversal:
            maskEfficiency = 0.f;
            dyeImpurity = 0.15f;
            break;
        case StockClass::MotionNegative:
            maskEfficiency = 0.95f;
            break;
        case StockClass::Creative:
            maskEfficiency = 0.75f;
            break;
        default:
            maskEfficiency = 0.95f;
            break;
    }

    maskEfficiency = LIM(maskEfficiency * maskEfficiencyMul, 0.f, 0.99f);
    dyeImpurity = LIM(dyeImpurity * impurityMul, 0.f, 2.f);

    // Output chain type. A scanner READS the negative through its own
    // bands; RA-4 and cinema print stock EXPOSE a second emulsion through
    // an enlarger lamp and are then viewed as reflection/projection; a
    // slide is VIEWED directly through the projector lamp by the eye.
    // The ratio-to-neutral factorisation needs each channel's signal to be
    // driven mostly by its own dye (per-channel separability). Narrow bands
    // give that; a full CIE-observer integration does not - X spans
    // 500-680nm, so every channel tracks overall brightness and the
    // per-axis normalisation degenerates into a brightness comparison that
    // crushed saturated colours 4x. So even the finished print is read
    // through densitometer-style bands, and projection is a broad-band
    // positive read.
    enum class PrintChain { Receiver, Paper };
    PrintChain chainType = PrintChain::Receiver;
    const bool positiveImage = stockClass == StockClass::Reversal;
    const bool projectionView = outputName == "projection";

    // Read bands: receiver bands for scanner chains, the print stock's
    // spectral sensitivities for paper chains, broad observer-ish bands for
    // direct viewing. They double as the masking couplers' "own band".
    float readMu[3] = {645.f, 545.f, 445.f};
    float readSigma[3] = {28.f, 28.f, 28.f};
    float colorGamma = 1.f;
    float paperGamma = 2.7f;
    float paperDmax = 2.4f;
    float lampTempK = 3200.f;
    float viewTempK = 5000.f;

    if (outputName == "ra4") {
        chainType = PrintChain::Paper;
        readMu[0] = 700.f; readMu[1] = 550.f; readMu[2] = 462.f;
        readSigma[0] = 16.f; readSigma[1] = 22.f; readSigma[2] = 22.f;
        paperGamma = 1.2f;
        paperDmax = 2.35f;
        colorGamma = 1.12f;
    } else if (outputName == "cinema") {
        // 2383-class print film: steeper, deeper, colder lamp at review.
        chainType = PrintChain::Paper;
        readMu[0] = 690.f; readMu[1] = 545.f; readMu[2] = 445.f;
        readSigma[0] = 18.f; readSigma[1] = 22.f; readSigma[2] = 22.f;
        paperGamma = 1.5f;
        paperDmax = 3.4f;
        colorGamma = 1.18f;
        viewTempK = 5400.f;
    } else if (outputName == "projection") {
        readMu[0] = 610.f; readMu[1] = 550.f; readMu[2] = 460.f;
        readSigma[0] = 45.f; readSigma[1] = 45.f; readSigma[2] = 40.f;
    } else if (outputName == "labscan") {
        // Minilab scanner: wider bands than Status M plus the colour
        // contrast its inversion pipeline is known for.
        readMu[0] = 640.f; readMu[1] = 550.f; readMu[2] = 460.f;
        readSigma[0] = 34.f; readSigma[1] = 34.f; readSigma[2] = 30.f;
        colorGamma = 1.18f;
    }

    // Paper (print stock) dyes: same chemistry family as the negative's but
    // purer - the print is the last chance to be clean.
    // Print-stock dyes are the cleanest in the chain - they are chosen for
    // exactly that - and their impurities land straight on the viewer with
    // nothing downstream to correct them, so small errors here read as
    // broad desaturation (first guess at 0.16/0.26 measured -15 dC across
    // the whole chart).
    // Narrow main lobes: real print dyes cut steeply outside their band -
    // a symmetric sigma-40 magenta reached 0.21 absorbance at a 615nm view
    // band and alone crushed every red's view ratio to 0.37.
    static const std::vector<Lobe> PAPER_DYES[3] = {
        {{1.00f, 660.f, 45.f}, {0.06f, 545.f, 42.f}, {0.02f, 435.f, 34.f}},
        {{1.00f, 545.f, 33.f}, {0.10f, 435.f, 30.f}, {0.02f, 640.f, 48.f}},
        {{1.00f, 445.f, 34.f}, {0.02f, 545.f, 38.f}}
    };

    // Densitometer-style bands the finished print is read through.
    constexpr float printViewMu[3] = {650.f, 535.f, 450.f};
    constexpr float printViewSigma[3] = {20.f, 22.f, 20.f};

    const auto planck = [](float lambda, float kelvin) {
        const float lm = lambda * 1e-9f;
        return 1.f / (lm * lm * lm * lm * lm * (std::exp(0.0143877688f / (lm * kelvin)) - 1.f));
    };

    // Tabulate spectra once.
    float dye[3][V4_PRINT_BINS];
    float coupler[3][V4_PRINT_BINS];
    float receiver[3][V4_PRINT_BINS];
    float paperDye[3][V4_PRINT_BINS];
    float lamp[V4_PRINT_BINS];
    float printView[3][V4_PRINT_BINS];
    for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
        const float lambda = 380.f + 10.f * bin;
        lamp[bin] = planck(lambda, lampTempK);
        for (int c = 0; c < 3; ++c) {
            printView[c][bin] = v4SpectralGauss(lambda, printViewMu[c], printViewSigma[c]) + 0.005f;
            float d = 0.f;
            bool main = true;
            for (const Lobe& lobe : DYES[c]) {
                d += lobe.weight * (main ? 1.f : dyeImpurity)
                    * v4SpectralGauss(lambda, lobe.mu, lobe.sigma);
                main = false;
            }
            dye[c][bin] = d;
            float pd = 0.f;
            for (const Lobe& lobe : PAPER_DYES[c]) {
                pd += lobe.weight * v4SpectralGauss(lambda, lobe.mu, lobe.sigma);
            }
            paperDye[c][bin] = pd;
            // The colored coupler is the dye's own absorption everywhere
            // OUTSIDE the band its record is read through - main-lobe tails
            // included, which the first cut missed (an unmasked magenta tail
            // at 445nm alone put +11 dC on the blue patch). Held before
            // development, consumed as dye forms.
            const float ownBand = v4SpectralGauss(lambda, readMu[c], readSigma[c] * 1.6f);
            coupler[c][bin] = maskEfficiency * d * (1.f - ownBand);
            // The 3% floor is band leakage plus flare: no real receiver
            // separates the records perfectly, and it is what keeps the
            // neutral-ratio contrast physically bounded. Paper sensitivities
            // are nearly leak-free - a tungsten enlarger lamp is so
            // red-heavy that even 3% of flat leak would swamp the blue
            // band's own signal and grey out every print (measured: -15 dC
            // across the chart).
            // Paper gets essentially none: under a tungsten enlarger even a
            // 0.2% flat leak collected 26% of a dense channel's exposure
            // (the band's own light is crushed by the negative's dye while
            // red floods through the leak) and greyed the whole print.
            const float leak = chainType == PrintChain::Paper ? 1e-4f : 0.03f;
            receiver[c][bin] = v4SpectralGauss(lambda, readMu[c], readSigma[c]) + leak;
        }
    }

    // Evaluate one node of the shared chain front half: negative
    // transmission for the given dye amounts.
    // A real colour negative spans ~2 density; the receiver chains were
    // calibrated at a gentler scale, so the paper chain gets the physical
    // one - its curve is built to accept it.
    const float chainDyeScale = chainType == PrintChain::Paper ? 2.0f : V4_PRINT_DYE_SCALE;

    const auto transmissionAt = [&](const float amount[3], float T[V4_PRINT_BINS]) {
        for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
            float spectralDensity = 0.f;
            for (int c = 0; c < 3; ++c) {
                spectralDensity += chainDyeScale
                    * (amount[c] * dye[c][bin] + (1.f - amount[c]) * coupler[c][bin]);
            }
            T[bin] = std::exp(-2.302585f * spectralDensity);
        }
    };

    // Paper chains need the enlarger filtration solved: per-channel paper
    // exposure at the NEUTRAL MID negative sets the logistic's pivot, which
    // is exactly what dialling the colour head's Y/M filters until a grey
    // card prints grey does.
    float paperPivot[3] = {0.f, 0.f, 0.f};

    if (chainType == PrintChain::Paper) {
        const float midAmount[3] = {0.5f, 0.5f, 0.5f};
        float T[V4_PRINT_BINS];
        transmissionAt(midAmount, T);
        for (int c = 0; c < 3; ++c) {
            float exposure = 0.f;
            for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
                exposure += T[bin] * lamp[bin] * receiver[c][bin];
            }
            paperPivot[c] = std::log10(std::max(exposure, 1e-12f));
        }
    }

    const float range = std::max(curves.maxDensity - curves.baseFog, 1e-5f);
    lut.baseFog = curves.baseFog;
    lut.indexScale = (N - 1) / range;
    lut.ratio.assign(static_cast<size_t>(N) * N * N * 3, 1.f);

    std::vector<float> signal(static_cast<size_t>(N) * N * N * 3);

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            for (int k = 0; k < N; ++k) {
                // The grid axes are output densities (bright = high), but
                // the POSITIVE image being viewed holds dye where light is
                // absent: a bright red area carries almost no cyan - it is
                // dense in magenta and yellow. Coupling dye to brightness
                // instead of darkness put every unwanted absorption on the
                // wrong colours (measured: blue patches gained chroma from
                // a magenta absorption that physically tempers them).
                // Dye convention is the class's chemistry: a slide holds dye
                // where light was ABSENT (positive image, so invert the
                // output-density axes), while a negative holds dye where the
                // record was DENSE - and the chain then re-inverts it, in
                // the scanner's software or in the paper itself. Feeding a
                // paper positive-convention dyes double-inverts and puts its
                // toe and shoulder on the wrong ends. Direct viewing only
                // makes sense for a positive, so that chain always views the
                // final positive image.
                const bool viewsPositive = positiveImage || projectionView;
                const float axis[3] = {
                    static_cast<float>(i) / (N - 1),
                    static_cast<float>(j) / (N - 1),
                    static_cast<float>(k) / (N - 1)
                };
                const float amount[3] = {
                    viewsPositive ? 1.f - axis[0] : axis[0],
                    viewsPositive ? 1.f - axis[1] : axis[1],
                    viewsPositive ? 1.f - axis[2] : axis[2]
                };
                float T[V4_PRINT_BINS];
                transmissionAt(amount, T);

                float s[3] = {0.f, 0.f, 0.f};

                if (chainType == PrintChain::Receiver) {
                    for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
                        s[0] += T[bin] * receiver[0][bin];
                        s[1] += T[bin] * receiver[1][bin];
                        s[2] += T[bin] * receiver[2][bin];
                    }
                    for (int c = 0; c < 3; ++c) {
                        if (!positiveImage) {
                            // The scanner reads the negative; its pipeline
                            // inverts each channel to deliver the positive.
                            s[c] = 1.f / std::max(s[c], 1e-6f);
                        }
                        if (colorGamma != 1.f) {
                            s[c] = std::pow(std::max(s[c], 1e-9f), colorGamma);
                        }
                    }
                } else {
                    // Expose the print stock through the enlarger, develop it
                    // on its logistic H&D, and view the dye stack it formed.
                    float paperDensity[3];
                    for (int c = 0; c < 3; ++c) {
                        float exposure = 0.f;
                        for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
                            exposure += T[bin] * lamp[bin] * receiver[c][bin];
                        }
                        const float logE = std::log10(std::max(exposure, 1e-12f)) - paperPivot[c];
                        // Negative-working paper darkens with exposure; a
                        // positive printed on positive-working stock needs
                        // the reversal-processed curve, which falls.
                        const float slope = positiveImage ? -paperGamma : paperGamma;
                        paperDensity[c] = paperDmax / (1.f + std::exp(-2.302585f * slope * logE));
                    }
                    for (int bin = 0; bin < V4_PRINT_BINS; ++bin) {
                        const float stack = paperDensity[0] * paperDye[0][bin]
                            + paperDensity[1] * paperDye[1][bin]
                            + paperDensity[2] * paperDye[2][bin];
                        const float reflect = std::exp(-2.302585f * stack);
                        s[0] += reflect * printView[0][bin];
                        s[1] += reflect * printView[1][bin];
                        s[2] += reflect * printView[2][bin];
                    }
                    for (int c = 0; c < 3; ++c) {
                        if (colorGamma != 1.f) {
                            s[c] = std::pow(std::max(s[c], 1e-9f), colorGamma);
                        }
                    }
                }

                const size_t base = ((static_cast<size_t>(i) * N + j) * N + k) * 3;
                for (int c = 0; c < 3; ++c) {
                    signal[base + c] = s[c];
                }
            }
        }
    }

    // Normalise against the neutral axis, per channel along its own axis, so
    // grey input always gets ratio 1 and only off-neutral behaviour remains.
    float diagonal[3][N];
    for (int m = 0; m < N; ++m) {
        const size_t base = ((static_cast<size_t>(m) * N + m) * N + m) * 3;
        for (int c = 0; c < 3; ++c) {
            diagonal[c][m] = std::max(signal[base + c], 1e-9f);
        }
    }

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            for (int k = 0; k < N; ++k) {
                const size_t base = ((static_cast<size_t>(i) * N + j) * N + k) * 3;
                const int axis[3] = {i, j, k};
                for (int c = 0; c < 3; ++c) {
                    lut.ratio[base + c] = LIM(
                        signal[base + c] / diagonal[c][axis[c]],
                        0.25f,
                        4.f);
                }
            }
        }
    }

    lut.active = true;
}

// Density gain applied to inhibitor-diffusion differences at edges, and the
// physical reach of that diffusion in the emulsion. Both V4-only.
constexpr float V4_ADJACENCY_GAIN = 2.9f;
constexpr float V4_ADJACENCY_MM = 0.014f;

// Inhibitor-difference soft knee: a moderate edge passes nearly linearly, a
// black-to-white edge compresses instead of ringing. In density terms the
// extreme-edge fringe caps near 0.1 - the heavy end of what push-processed
// stock shows, and the outputSoftness slider scales it down from there.
constexpr float V4_ADJACENCY_KNEE = 0.09f;

inline float v4AdjacencySignal(const FilmLabV3CurveBank& curves, float luma)
{
    const float density = curves.sampleDensity(1, luma);
    const float inverseRange = 1.f / std::max(curves.maxDensity - curves.baseFog, 1e-5f);
    return v4LangmuirInhibitor(density, curves.baseFog, inverseRange);
}

inline void applyV3DensityCoupling(float density[3], StockClass stockClass, float amount, float baseFog, float maxDensity)
{
    if (stockClass == StockClass::Monochrome || amount <= 0.0001f) {
        return;
    }
    const float range = std::max(maxDensity - baseFog, 1e-5f);
    const float mean = (density[0] + density[1] + density[2]) / 3.f;
    const float normalized = LIM((mean - baseFog) / range, 0.f, 1.f);
    const float toeWeight = 1.f - smoothStep(0.18f, 0.46f, normalized);
    const float shoulderWeight = smoothStep(0.56f, 0.86f, normalized);
    const float midWeight = std::max(1.f - toeWeight - shoulderWeight, 0.f);
    const float weightSum = std::max(toeWeight + midWeight + shoulderWeight, 1e-5f);
    const float toe = toeWeight / weightSum;
    const float mid = midWeight / weightSum;
    const float shoulder = shoulderWeight / weightSum;

    float redFromGreen = 0.16f * toe + 0.09f * mid + 0.04f * shoulder;
    float blueFromGreen = 0.05f * toe + 0.11f * mid + 0.17f * shoulder;
    float greenBalance = 0.08f * toe + 0.06f * mid + 0.10f * shoulder;
    if (stockClass == StockClass::MotionNegative) {
        redFromGreen *= 1.22f;
        blueFromGreen *= 1.18f;
    } else if (stockClass == StockClass::Reversal) {
        redFromGreen *= 0.58f;
        blueFromGreen *= 0.52f;
        greenBalance *= 0.65f;
    }

    const float original[3] = {density[0], density[1], density[2]};
    density[0] += (original[1] - original[0]) * redFromGreen * amount;
    density[1] += ((original[0] + original[2]) * 0.5f - original[1]) * greenBalance * amount;
    density[2] += (original[1] - original[2]) * blueFromGreen * amount;
    density[0] = LIM(density[0], baseFog, maxDensity);
    density[1] = LIM(density[1], baseFog, maxDensity);
    density[2] = LIM(density[2], baseFog, maxDensity);
}


inline float v3SkinConfidence(float r, float g, float b)
{
    const float y = std::max(luminance(r, g, b), 1e-5f);
    const float redGreen = (r - g) / y;
    const float greenBlue = (g - b) / y;
    const float chroma = (std::max(r, std::max(g, b)) - std::min(r, std::min(g, b))) / y;
    const float warm = smoothStep(0.015f, 0.20f, redGreen)
        * smoothStep(-0.015f, 0.16f, greenBlue)
        * (1.f - smoothStep(0.62f, 1.10f, chroma));
    const float tonal = smoothStep(0.025f, 0.10f, y) * (1.f - smoothStep(1.05f, 2.2f, y));
    return warm * tonal;
}

inline void applyV3SkinProtection(float sourceR, float sourceG, float sourceB, float& r, float& g, float& b, float amount)
{
    if (amount <= 0.0001f || sourceR <= sourceG * 0.98f || sourceG <= sourceB * 0.91f) {
        return;
    }
    const float confidence = v3SkinConfidence(sourceR, sourceG, sourceB);
    const float blend = LIM(amount * confidence * 0.52f, 0.f, 0.52f);
    if (blend <= 0.0001f) {
        return;
    }
    const float sourceY = std::max(luminance(sourceR, sourceG, sourceB), 1e-5f);
    const float outputY = std::max(luminance(r, g, b), 1e-5f);
    const float sourceChroma[3] = {
        sourceR / sourceY - 1.f,
        sourceG / sourceY - 1.f,
        sourceB / sourceY - 1.f
    };
    float outputChroma[3] = {
        r / outputY - 1.f,
        g / outputY - 1.f,
        b / outputY - 1.f
    };
    for (int channel = 0; channel < 3; ++channel) {
        outputChroma[channel] = intp(blend, sourceChroma[channel], outputChroma[channel]);
    }
    r = std::max(outputY * (1.f + outputChroma[0]), 0.f);
    g = std::max(outputY * (1.f + outputChroma[1]), 0.f);
    b = std::max(outputY * (1.f + outputChroma[2]), 0.f);
}

inline void applyV3ColorBalance(float& r, float& g, float& b, const float multipliers[3])
{
    const float originalY = std::max(luminance(r, g, b), 1e-6f);
    r *= multipliers[0];
    g *= multipliers[1];
    b *= multipliers[2];
    const float adjustedY = std::max(luminance(r, g, b), 1e-6f);
    const float scale = originalY / adjustedY;
    r *= scale;
    g *= scale;
    b *= scale;
}

inline void applyV3ZoneTint(
    float& r,
    float& g,
    float& b,
    const float shadowVector[3],
    const float highlightVector[3],
    float shadowStrength,
    float highlightStrength)
{
    const float y = std::max(luminance(r, g, b), 0.f);
    const float shadow = 1.f - smoothStep(0.10f, 0.48f, y);
    const float highlight = smoothStep(0.40f, 0.96f, y);
    const float middle = std::max(1.f - shadow - highlight, 0.f);
    const float sum = std::max(shadow + middle + highlight, 1e-5f);
    const float shadowWeight = shadow / sum;
    const float highlightWeight = highlight / sum;

    const float shadowAmount = shadowStrength * shadowWeight;
    const float highlightAmount = highlightStrength * highlightWeight;
    const float originalY = std::max(luminance(r, g, b), 1e-6f);
    constexpr float LOG2_TO_LINEAR = 0.69314718f;
    r *= std::max(1.f + LOG2_TO_LINEAR * (shadowVector[0] * shadowAmount + highlightVector[0] * highlightAmount), 0.25f);
    g *= std::max(1.f + LOG2_TO_LINEAR * (shadowVector[1] * shadowAmount + highlightVector[1] * highlightAmount), 0.25f);
    b *= std::max(1.f + LOG2_TO_LINEAR * (shadowVector[2] * shadowAmount + highlightVector[2] * highlightAmount), 0.25f);
    const float adjustedY = std::max(luminance(r, g, b), 1e-6f);
    const float scale = originalY / adjustedY;
    r *= scale;
    g *= scale;
    b *= scale;
}

inline void applyV3OutputMatrix(const FilmLabV3Output& output, float& r, float& g, float& b)
{
    const float rr = output.matrix[0] * r + output.matrix[1] * g + output.matrix[2] * b;
    const float gg = output.matrix[3] * r + output.matrix[4] * g + output.matrix[5] * b;
    const float bb = output.matrix[6] * r + output.matrix[7] * g + output.matrix[8] * b;
    r = rr;
    g = gg;
    b = bb;
}

inline float sampleV3Plane(array2D<float>& plane, int width, int height, float x, float y)
{
    x = LIM(x, 0.f, static_cast<float>(width - 1));
    y = LIM(y, 0.f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - x0;
    const float fy = y - y0;
    const float top = intp(fx, plane[y0][x1], plane[y0][x0]);
    const float bottom = intp(fx, plane[y1][x1], plane[y1][x0]);
    return intp(fy, bottom, top);
}

void filmPresetsV3(
    LabImage* lab,
    const procparams::FilmPresetsParams& fp,
    const FilmLabContext& context,
    bool multiThread,
    const float (*tapToCanonical)[3])
{
    const FilmLabStock& stockRecord = findStock(fp.preset);
    const Glib::ustring processName = fp.process == "auto" ? defaultProcess(stockRecord.stockClass) : fp.process;
    FilmLabV3Profile stock = makeV3Profile(stockRecord);
    const FilmLabV3Process process = makeV3Process(processName);
    const FilmLabV3Output output = makeV3Output(fp.output);
    const bool straightCurve = stockRecord.stockClass == StockClass::Custom;
    const bool stopsIndexed = fp.modelVersion >= 4;

    FilmLabV4Character character;

    if (stopsIndexed
            && stockRecord.stockClass != StockClass::Custom
            && stockRecord.stockClass != StockClass::Monochrome) {
        stock.spectral = makeV4SpectralMatrix(stockRecord.stockClass);
        character = makeV4Character(fp.preset);
        stock.coupling *= character.couplingMul;
        stock.layerGamma[0] *= character.redGammaMul;
        stock.layerGamma[2] *= character.blueGammaMul;
        stock.layerToe[0] += character.redToeAdd;
        stock.layerToe[2] += character.blueToeAdd;
    }

    const FilmLabV3CurveBank curves = makeV3Curves(stock, process, output, fp, straightCurve, stopsIndexed);

    FilmLabV4PrintLUT printLUT;
    if (stopsIndexed && !straightCurve && stockRecord.stockClass != StockClass::Monochrome) {
        makeV4PrintLUT(printLUT, curves, stockRecord.stockClass, fp.output, multiThread,
                       character.maskEfficiencyMul, character.impurityMul);
    }

    const TMatrix canonicalMatrix = ICCStore::getInstance()->workingSpaceMatrix("ACESp1");
    const TMatrix canonicalInverse = ICCStore::getInstance()->workingSpaceInverseMatrix("ACESp1");
    float matrix[3][3];
    float inverse[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            matrix[row][col] = static_cast<float>(canonicalMatrix[row][col]);
            inverse[row][col] = static_cast<float>(canonicalInverse[row][col]);
        }
    }

    // V4 scene mode needs the tap, the rgbProc snapshot, and matching
    // geometry; anything missing degrades to the V3 display-referred input,
    // which keeps the first frame after enabling and any stale-buffer state
    // rendering instead of failing.
    const bool sceneMode = fp.modelVersion >= 4
        && tapToCanonical != nullptr
        && context.sceneTap != nullptr
        && context.rgbSnapshot != nullptr
        && context.sceneTap->getWidth() == lab->W
        && context.sceneTap->getHeight() == lab->H
        && context.rgbSnapshot->W == lab->W
        && context.rgbSnapshot->H == lab->H;

    FilmLabV4SceneFetch sceneFetch;
    if (sceneMode) {
        sceneFetch.tap = context.sceneTap;
        sceneFetch.snapshot = context.rgbSnapshot;
        sceneFetch.lab = lab;
        sceneFetch.labInverse = inverse;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                sceneFetch.tapMatrix[row][col] = tapToCanonical[row][col];
            }
        }
    }

    const int scale = std::max(context.scale, 1);
    const int fullWidth = context.fullWidth > 0 ? context.fullWidth : lab->W * scale;
    const int fullHeight = context.fullHeight > 0 ? context.fullHeight : lab->H * scale;
    const int fullShort = std::max(1, std::min(fullWidth, fullHeight));
    const float strength = LIM(fp.strength / 100.f, 0.f, 1.f);
    const float characterScale = filmCharacterScale(strength);

    // Motion-picture stock carries a remjet anti-halation backing that soaks
    // up nearly all of the light before it can bounce off the base, so
    // developed in its native ECN-2 bath it barely halates. Strip the remjet
    // and cross-process it in C-41 - the CineStill workflow - and the same
    // emulsion produces the famous red glow. V4 keys this off the process
    // the user already selects; V2/V3 keep their stock table as tuned.
    float remjetFactor = 1.f;
    if (stopsIndexed && stockRecord.stockClass == StockClass::MotionNegative) {
        remjetFactor = processName == "c41" ? 1.30f : 0.30f;
    }

    const float halation = LIM((stockRecord.halation + fp.halation / 155.f) * characterScale * remjetFactor, 0.f, 0.92f);
    const float bloom = LIM((stock.bloom + fp.bloom / 180.f) * characterScale, 0.f, 0.65f);
    // Onset as a fraction of diffuse white, not stops above it - see
    // halationHighlightSource. Positive slider still means "trigger later".
    const float halationOnset = LIM(0.78f + fp.halationThreshold / 420.f, 0.45f, 0.97f);
    // Scene mode has real magnitudes again, so the onset returns to stops
    // relative to diffuse white: the slider spans +/- two stops.
    const float halationOnsetStopsV4 = fp.halationThreshold / 50.f;
    const float halationSize = std::exp2(fp.halationSize / 100.f * 1.35f);
    const float halationWarmth = LIM(1.f + fp.halationColor / 160.f, 0.35f, 1.65f);

    array2D<float> halationSource;
    array2D<float> halationInner;
    array2D<float> halationOuter;
    int halationReduction = 1;
    int halationWidth = 0;
    int halationHeight = 0;
    if (halation > 0.001f || bloom > 0.001f) {
        halationReduction = scale <= 1 ? 4 : (scale <= 2 ? 2 : 1);
        halationWidth = std::max(1, (lab->W + halationReduction - 1) / halationReduction);
        halationHeight = std::max(1, (lab->H + halationReduction - 1) / halationReduction);
        halationSource(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);
        halationInner(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);
        halationOuter(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);
        array2D<float> halationScratch(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);

#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < halationHeight; ++row) {
            for (int col = 0; col < halationWidth; ++col) {
                // Average the block rather than sampling its centre. Point
                // sampling made the highlight mask depend on which pixels the
                // decimation happened to land on, so the halo shimmered and
                // moved as the user changed zoom.
                const int rowStart = row * halationReduction;
                const int colStart = col * halationReduction;
                const int rowEnd = std::min(rowStart + halationReduction, lab->H);
                const int colEnd = std::min(colStart + halationReduction, lab->W);
                float accumulated = 0.f;
                int samples = 0;
                for (int sourceRow = rowStart; sourceRow < rowEnd; ++sourceRow) {
                    for (int sourceCol = colStart; sourceCol < colEnd; ++sourceCol) {
                        float r;
                        float g;
                        float b;

                        if (sceneMode) {
                            float labR;
                            float labG;
                            float labB;
                            sceneFetch.fetch(sourceRow, sourceCol, r, g, b, labR, labG, labB);
                            const float y = std::max(luminance(r, g, b), 0.f);
                            const float peak = std::max(r, std::max(g, b));
                            accumulated += halationHighlightSourceV4(y, peak, halationOnsetStopsV4);
                        } else {
                            toCanonicalV3(lab, sourceRow, sourceCol, inverse, r, g, b);
                            const float y = std::max(luminance(r, g, b), 0.f);
                            const float peak = std::max(r, std::max(g, b));
                            accumulated += halationHighlightSource(y, peak, halationOnset);
                        }

                        ++samples;
                    }
                }
                halationSource[row][col] = samples > 0 ? accumulated / samples : 0.f;
            }
        }

        // Halation spreads much further than this used to allow: on a 4000px
        // frame the old coefficients gave a 5px core and a 16px tail, which
        // reads as a thin outline rather than a glow. Scattering through the
        // base and back puts real bleed tens of pixels out at that scale.
        float innerRadiusPixels;
        float outerRadiusPixels;

        if (stopsIndexed) {
            // The halo is a property of the film, not of the frame: light
            // that pierces the emulsion reflects off the base at the
            // critical-angle band and re-exposes a RING ~140um out, and a
            // weaker multiple-bounce tail reaches ~480um. Converting through
            // the frame's physical short side means a 6x7 or 4x5 frame shows
            // a proportionally tighter halo than 35mm at the same print
            // size, which is exactly why large format looks "cleaner".
            const float frameShortMM = fp.format == "120" ? 56.f
                : fp.format == "large" ? 95.f : 24.f;
            const float pixelsPerMM = fullShort / frameShortMM;
            innerRadiusPixels = 0.140f * pixelsPerMM * halationSize;
            outerRadiusPixels = 0.480f * pixelsPerMM * halationSize;
        } else {
            const float baseRadius = fullShort * (0.0035f + halation * 0.0075f) * halationSize;
            innerRadiusPixels = baseRadius;
            outerRadiusPixels = baseRadius * 3.25f;
        }

        const int innerRadius = LIM(
            static_cast<int>(innerRadiusPixels / (scale * halationReduction) + 0.5f),
            1,
            48);
        const int outerRadius = LIM(
            static_cast<int>(outerRadiusPixels / (scale * halationReduction) + 0.5f),
            innerRadius + 1,
            96);
        halationBlur(halationSource, halationInner, halationScratch, innerRadius, halationWidth, halationHeight, multiThread);
        halationBlur(halationSource, halationOuter, halationScratch, outerRadius, halationWidth, halationHeight, multiThread);
        halationScratch.free();

#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < halationHeight; ++row) {
            for (int col = 0; col < halationWidth; ++col) {
                const float source = halationSource[row][col];

                if (stopsIndexed) {
                    // Film's halation is an edge phenomenon, not a veil: the
                    // anti-halation layer eats most of the light, and what
                    // bounces lands in a band just OUTSIDE the highlight's
                    // contour. Subtracting nearly all of the source's own
                    // footprint from the ring blur leaves exactly that band;
                    // the multiple-bounce tail keeps only a quarter of the
                    // weight so a frame full of blown bokeh gets red fringes
                    // on every blob instead of one milky global glow - which
                    // is what an unbudgeted two-Gaussian mix produced.
                    const float ring = std::max(halationInner[row][col] - source * 0.85f, 0.f);
                    const float tail = std::max(halationOuter[row][col] - source * 0.30f, 0.f);
                    halationInner[row][col] = halation * V4_HALATION_ENERGY
                        * (ring * stock.halationInner + tail * stock.halationOuter * 0.25f);
                    halationOuter[row][col] = bloom * (ring * 0.72f + tail * 0.28f);
                } else {
                    const float inner = std::max(halationInner[row][col] - source * 0.32f, 0.f);
                    const float outer = std::max(halationOuter[row][col] - source * 0.18f, 0.f);
                    halationInner[row][col] = halation * (inner * stock.halationInner + outer * stock.halationOuter);
                    halationOuter[row][col] = bloom * (inner * 0.72f + outer * 0.28f);
                }
            }
        }
        halationSource.free();
    }

    // V4 adjacency: the lateral half of the DIR coupler story. The blurred
    // inhibitor field is built from the same pre-halation scene luminance the
    // main loop reads, so local-minus-blurred is an unbiased edge signal.
    array2D<float> adjacencyBlur;
    float adjacencyGain = 0.f;
    const float adjacencyStrength = LIM(
        stock.acutance - output.softness - fp.outputSoftness / 150.f,
        -0.72f,
        0.85f) * strength * characterScale;

    if (stopsIndexed && adjacencyStrength > 0.002f && lab->W > 4 && lab->H > 4) {
        const float frameShortMM = fp.format == "120" ? 56.f
            : fp.format == "large" ? 95.f : 24.f;
        const float sigmaPixels = V4_ADJACENCY_MM * (fullShort / frameShortMM) / scale;

        // Below half a pixel the inhibitors diffuse inside one pixel's own
        // footprint and there is nothing to resolve - which is also why the
        // effect fades out of a zoomed-out preview, exactly like grain.
        if (sigmaPixels >= 0.5f) {
            adjacencyGain = adjacencyStrength * V4_ADJACENCY_GAIN
                * std::max(curves.maxDensity - curves.baseFog, 1e-5f);
            array2D<float> adjacencySource(lab->W, lab->H);
            adjacencyBlur(lab->W, lab->H, ARRAY2D_CLEAR_DATA);

#ifdef _OPENMP
            #pragma omp parallel for schedule(static) if (multiThread)
#endif
            for (int row = 0; row < lab->H; ++row) {
                for (int col = 0; col < lab->W; ++col) {
                    float r;
                    float g;
                    float b;

                    if (sceneMode) {
                        float labR;
                        float labG;
                        float labB;
                        sceneFetch.fetch(row, col, r, g, b, labR, labG, labB);
                    } else {
                        toCanonicalV3(lab, row, col, inverse, r, g, b);
                    }

                    adjacencySource[row][col] = v4AdjacencySignal(curves, std::max(luminance(r, g, b), 0.f));
                }
            }

            const int radius = LIM(static_cast<int>(sigmaPixels * 1.7f + 0.5f), 1, 6);
            boxblur(static_cast<float**>(adjacencySource), static_cast<float**>(adjacencyBlur), radius, lab->W, lab->H, multiThread);
        }
    }

    // Grain is deliberately NOT applied by the film stage any more: it is
    // the standalone Grain tool's job (Effects > Grain), so the two never
    // stack and the user has one place to control texture.
    const float coupling = LIM(stock.coupling * (1.f + fp.layerCoupling / 120.f), 0.f, 0.58f);
    // V4 grows its colour separation physically - interimage couplers and
    // dye absorptions read through the print - so the stock table's legacy
    // saturation scalar hands most of its excess back. This is the Phase 5
    // direction: colour from the machinery, not from a multiplier.
    // ... except for reversal: its spectral viewing stage runs the other
    // way (unmasked dyes temper colour), so its scalar stays as tuned.
    const float stockSaturation = (stopsIndexed && stockRecord.stockClass != StockClass::Reversal)
        ? 1.f + (stockRecord.saturation - 1.f) * 0.35f
        : stockRecord.saturation;
    const float saturation = LIM(
        stockSaturation * process.saturation * output.saturation * (1.f + fp.saturation / 160.f),
        0.f,
        1.72f);
    const float vibrance = LIM(fp.vibrance / 100.f, -1.f, 1.f);
    const float warmth = stockRecord.warmth + output.warmth + fp.warmth / 430.f;
    const float tint = stockRecord.tint + fp.tint / 520.f;
    const float skinProtection = LIM(fp.skinProtection / 100.f, 0.f, 1.f);
    const bool colorBalanceActive = std::fabs(warmth) > 0.0001f || std::fabs(tint) > 0.0001f;
    const float colorBalance[3] = {
        std::exp2(warmth + tint * 0.32f),
        std::exp2(-tint * 0.28f),
        std::exp2(-warmth + tint * 0.15f)
    };
    const float shadowStrength = fp.shadowTint / 520.f;
    const float highlightStrength = fp.highlightTint / 520.f;
    const bool zoneTintActive = std::fabs(shadowStrength) > 0.0001f || std::fabs(highlightStrength) > 0.0001f;
    float shadowVector[3];
    float highlightVector[3];
    hueVector(static_cast<float>(fp.shadowHue), shadowVector[0], shadowVector[1], shadowVector[2]);
    hueVector(static_cast<float>(fp.highlightHue), highlightVector[0], highlightVector[1], highlightVector[2]);
    const bool outputMatrixActive = fp.output != "scan";
    const bool saturationActive = std::fabs(saturation - 1.f) > 0.0001f;
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int row = 0; row < lab->H; ++row) {
        for (int col = 0; col < lab->W; ++col) {
            float sourceR;
            float sourceG;
            float sourceB;
            float r;
            float g;
            float b;

            float displayGain[3] = {1.f, 1.f, 1.f};

            if (sceneMode) {
                // sourceR/G/B stay display-referred: they are the blend base
                // for strength < 100 and the skin-protection reference, i.e.
                // what the user sees with the film stage switched off.
                sceneFetch.fetch(row, col, r, g, b, sourceR, sourceG, sourceB, displayGain);
            } else {
                toCanonicalV3(lab, row, col, inverse, sourceR, sourceG, sourceB);
                r = sourceR;
                g = sourceG;
                b = sourceB;
            }

            // Must match what the adjacency pre-pass stored: scene
            // luminance before halation is added.
            float adjacencySourceLuma = 0.f;
            if (adjacencyBlur) {
                adjacencySourceLuma = std::max(luminance(r, g, b), 0.f);
            }

            if (halationInner) {
                const float lowX = (col + 0.5f) / halationReduction - 0.5f;
                const float lowY = (row + 0.5f) / halationReduction - 0.5f;
                const float halationEnergy = sampleV3Plane(halationInner, halationWidth, halationHeight, lowX, lowY);
                const float neutralBloom = sampleV3Plane(halationOuter, halationWidth, halationHeight, lowX, lowY);
                r += halationEnergy * stock.halationColor[0] * halationWarmth + neutralBloom;
                g += halationEnergy * stock.halationColor[1] + neutralBloom;
                b += halationEnergy * stock.halationColor[2] / halationWarmth + neutralBloom;
            }

            float layerExposure[3];
            if (stockRecord.stockClass == StockClass::Custom) {
                layerExposure[0] = std::max(r, 0.f);
                layerExposure[1] = std::max(g, 0.f);
                layerExposure[2] = std::max(b, 0.f);
            } else {
                applyV3Spectral(stock, r, g, b, layerExposure);
            }
            float density[3] = {
                curves.sampleDensity(0, layerExposure[0]),
                curves.sampleDensity(1, layerExposure[1]),
                curves.sampleDensity(2, layerExposure[2])
            };
            if (stopsIndexed) {
                if (stockRecord.stockClass != StockClass::Monochrome) {
                    applyV4Couplers(density, coupling * v4CouplerClassGain(stockRecord.stockClass), curves.baseFog, curves.maxDensity);
                }
            } else {
                applyV3DensityCoupling(density, stockRecord.stockClass, coupling, curves.baseFog, curves.maxDensity);
            }

            if (adjacencyBlur) {
                // Interimage inhibitors also diffuse laterally: a region
                // denser than its ~14um neighbourhood developed against less
                // inhibition than the blurred average says it should have,
                // so its density rises, and a thinner neighbour falls. That
                // is film's real edge effect - the acutance the V3 model
                // faked with an unsharp mask on L.
                const float local = v4AdjacencySignal(curves, adjacencySourceLuma);
                const float diff = local - adjacencyBlur[row][col];
                const float compressed = diff / (1.f + std::fabs(diff) / V4_ADJACENCY_KNEE);
                const float correction = adjacencyGain * compressed;
                density[0] = LIM(density[0] + correction, curves.baseFog, curves.maxDensity);
                density[1] = LIM(density[1] + correction, curves.baseFog, curves.maxDensity);
                density[2] = LIM(density[2] + correction, curves.baseFog, curves.maxDensity);
            }

            r = curves.sampleOutput(0, density[0]);
            g = curves.sampleOutput(1, density[1]);
            b = curves.sampleOutput(2, density[2]);

            if (printLUT.active) {
                float spectralRatio[3];
                printLUT.sample(density, spectralRatio);
                r *= spectralRatio[0];
                g *= spectralRatio[1];
                b *= spectralRatio[2];
            }

            if (outputMatrixActive) {
                applyV3OutputMatrix(output, r, g, b);
            }

            if (stockRecord.stockClass == StockClass::Monochrome || processName == "bw") {
                const float mono = 0.272229f * r + 0.674082f * g + 0.053689f * b;

                if (stopsIndexed) {
                    // Silver-print image tone: a warmtone paper's fine
                    // silver warms the mid-dark densities, while paper-base
                    // highlights and the deepest blacks stay neutral.
                    const float tone = LIM(mono, 0.f, 1.f);
                    const float warm = 0.016f
                        * smoothStep(0.02f, 0.22f, tone)
                        * (1.f - smoothStep(0.45f, 0.95f, tone));
                    r = mono * (1.f + warm);
                    g = mono;
                    b = mono * (1.f - 1.35f * warm);
                } else {
                    r = mono * 1.004f;
                    g = mono;
                    b = mono * 0.993f;
                }
            } else {
                if (saturationActive) {
                    applySaturation(r, g, b, saturation);
                }
                applyVibrance(r, g, b, vibrance);
            }
            if (colorBalanceActive) {
                applyV3ColorBalance(r, g, b, colorBalance);
            }
            if (zoneTintActive) {
                applyV3ZoneTint(r, g, b, shadowVector, highlightVector, shadowStrength, highlightStrength);
            }
            applyV3SkinProtection(sourceR, sourceG, sourceB, r, g, b, skinProtection);

            // Grade the print: the user's tone curve and display-domain
            // colour edits act on the film's output, so an S-curve deepens
            // the rendered shadows exactly as dialled instead of being
            // silently discarded with the clipped input.
            if (sceneMode) {
                r *= displayGain[0];
                g *= displayGain[1];
                b *= displayGain[2];
            }

            r = std::max(sourceR + (r - sourceR) * strength, 0.f);
            g = std::max(sourceG + (g - sourceG) * strength, 0.f);
            b = std::max(sourceB + (b - sourceB) * strength, 0.f);
            r = LIM(r, 0.f, V3_MAX_INPUT) * MAXVALF;
            g = LIM(g, 0.f, V3_MAX_INPUT) * MAXVALF;
            b = LIM(b, 0.f, V3_MAX_INPUT) * MAXVALF;
            float x;
            float y;
            float z;
            Color::rgbxyz(r, g, b, x, y, z, matrix);
            Color::XYZ2Lab(x, y, z, lab->L[row][col], lab->a[row][col], lab->b[row][col]);
        }
    }

    halationSource.free();
    halationInner.free();
    halationOuter.free();

    float detailGain = LIM(
        stock.acutance - output.softness - fp.outputSoftness / 150.f,
        -0.72f,
        0.58f) * strength * characterScale;

    // V4 sharpens through the diffused-inhibitor adjacency effect in the
    // density domain instead; only the softening direction of this block
    // (a print's diffusion, outputSoftness < 0 territory) still applies.
    if (stopsIndexed) {
        detailGain = std::min(detailGain, 0.f);
    }

    if (std::fabs(detailGain) > 0.002f && lab->W > 2 && lab->H > 2) {
        array2D<float> blurredL(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        const int radius = LIM(static_cast<int>((1.25f + std::max(detailGain * -2.f, 0.f)) / scale + 0.5f), 1, 3);
        boxblur(lab->L, static_cast<float**>(blurredL), radius, lab->W, lab->H, multiThread);
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < lab->H; ++row) {
            for (int col = 0; col < lab->W; ++col) {
                const float detail = LIM(lab->L[row][col] - blurredL[row][col], -3200.f, 3200.f);
                lab->L[row][col] = LIM(lab->L[row][col] + detail * detailGain, 0.f, MAXVALF);
            }
        }
    }
}

} // namespace

std::uint32_t ImProcFunctions::filmLabSeed(const Glib::ustring& filename)
{
    std::uint32_t hash = 2166136261u;
    const std::string bytes = filename.raw();
    for (const unsigned char value : bytes) {
        hash ^= value;
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

void ImProcFunctions::filmPresets(
    LabImage* lab,
    const procparams::FilmPresetsParams& fp,
    const FilmLabContext& context)
{
    if (!lab || !fp.enabled || fp.strength <= 0) {
        return;
    }

    if (fp.modelVersion < 2) {
        filmPresetsV1(lab, fp);
        return;
    }

    if (fp.modelVersion >= 3) {
        // The tap is captured in the user's working profile at rgbProc's
        // 0..65535 scale; fold profile -> XYZ -> AP1 and the normalisation
        // into one matrix so scene mode costs one multiply per channel.
        float tapToCanonical[3][3];
        bool haveTapMatrix = false;

        if (fp.modelVersion >= 4 && context.sceneTap && context.rgbSnapshot) {
            const TMatrix workingMatrix = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
            const TMatrix canonicalInverse = ICCStore::getInstance()->workingSpaceInverseMatrix("ACESp1");

            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    double sum = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        sum += canonicalInverse[row][k] * workingMatrix[k][col];
                    }
                    tapToCanonical[row][col] = static_cast<float>(sum) / MAXVALF;
                }
            }

            haveTapMatrix = true;
        }

        filmPresetsV3(lab, fp, context, multiThread, haveTapMatrix ? tapToCanonical : nullptr);
        return;
    }

    const FilmLabStock& stock = findStock(fp.preset);
    const Glib::ustring process = fp.process == "auto" ? defaultProcess(stock.stockClass) : fp.process;

    float processContrast = 1.f;
    float processToe = 0.f;
    float processShoulder = 0.f;
    float processSaturation = 1.f;

    if (process == "e6") {
        processContrast = 1.10f;
        processToe = -0.05f;
        processShoulder = -0.08f;
        processSaturation = 1.10f;
    } else if (process == "ecn2") {
        processContrast = 0.92f;
        processShoulder = 0.16f;
        processSaturation = 0.92f;
    } else if (process == "bw") {
        processContrast = 1.05f;
        processToe = 0.08f;
        processSaturation = 0.f;
    }

    float outputContrast = 1.f;
    float outputSaturation = 1.f;
    float outputWarmth = 0.f;
    float outputShoulder = 0.f;
    if (fp.output == "ra4") {
        outputContrast = 1.06f;
        outputSaturation = 1.04f;
        outputWarmth = 0.025f;
    } else if (fp.output == "projection") {
        outputContrast = 1.10f;
        outputSaturation = 1.08f;
        outputShoulder = -0.04f;
    } else if (fp.output == "cinema") {
        outputContrast = 1.04f;
        outputSaturation = 0.96f;
        outputWarmth = 0.018f;
        outputShoulder = 0.08f;
    }

    const float pushPull = LIM(static_cast<float>(fp.pushPull), -2.f, 3.f);
    const float exposure = LIM(static_cast<float>(fp.exposure), -4.f, 4.f) - pushPull * 0.33f;
    const float contrast = LIM(
        stock.contrast * processContrast * outputContrast
        * (1.f + pushPull * 0.075f)
        * (1.f + fp.contrast / 250.f),
        0.55f,
        1.65f);
    const float toe = LIM(stock.toe + processToe - pushPull * 0.025f + fp.fade / 260.f, 0.f, 0.85f);
    const float shoulder = LIM(stock.shoulder + processShoulder + outputShoulder + fp.rolloff / 220.f, 0.f, 1.1f);
    const float saturation = LIM(
        stock.saturation * processSaturation * outputSaturation * (1.f + fp.saturation / 150.f),
        0.f,
        1.75f);
    const float warmth = stock.warmth + outputWarmth + fp.warmth / 420.f;
    const float tint = stock.tint + fp.tint / 500.f;
    const float strength = LIM(fp.strength / 100.f, 0.f, 1.f);
    const float characterScale = filmCharacterScale(strength);
    const float halation = LIM((stock.halation + fp.halation / 180.f) * characterScale, 0.f, 0.82f);
    const float vibrance = LIM(fp.vibrance / 100.f, -1.f, 1.f);
    const float acutance = LIM(stock.acutance, 0.f, 0.42f);

    const float channelBaselineExposure[3] = {
        stock.redBias + fp.redShift / 500.f,
        stock.greenBias + fp.greenShift / 500.f,
        stock.blueBias + fp.blueShift / 500.f
    };
    const float channelExposure[3] = {
        exposure + channelBaselineExposure[0],
        exposure + channelBaselineExposure[1],
        exposure + channelBaselineExposure[2]
    };
    float blackResponse[3];
    float middleResponse[3];
    for (int channel = 0; channel < 3; ++channel) {
        blackResponse[channel] = filmResponse(0.f, channelExposure[channel], contrast, toe, shoulder);
        middleResponse[channel] = filmResponse(0.18f, channelBaselineExposure[channel], contrast, toe, shoulder);
    }

    // AP1 is a fixed, scene-linear processing space. This makes a Film Lab
    // recipe independent of the user's selected working profile.
    const TMatrix canonicalMatrix = ICCStore::getInstance()->workingSpaceMatrix("ACESp1");
    const TMatrix canonicalInverse = ICCStore::getInstance()->workingSpaceInverseMatrix("ACESp1");
    float matrix[3][3];
    float inverse[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            matrix[row][col] = static_cast<float>(canonicalMatrix[row][col]);
            inverse[row][col] = static_cast<float>(canonicalInverse[row][col]);
        }
    }

    array2D<float> halationSource;
    array2D<float> halationInner;
    array2D<float> halationOuter;
    int halationReduction = 1;
    int halationWidth = 0;
    int halationHeight = 0;
    if (halation > 0.001f) {
        const int previewScale = std::max(context.scale, 1);
        halationReduction = previewScale <= 1 ? 4 : (previewScale <= 2 ? 2 : 1);
        halationWidth = std::max(1, (lab->W + halationReduction - 1) / halationReduction);
        halationHeight = std::max(1, (lab->H + halationReduction - 1) / halationReduction);
        halationSource(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);
        halationInner(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);
        halationOuter(halationWidth, halationHeight, ARRAY2D_CLEAR_DATA);

#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < halationHeight; ++row) {
            for (int col = 0; col < halationWidth; ++col) {
                const int sourceRow = std::min(row * halationReduction + halationReduction / 2, lab->H - 1);
                const int sourceCol = std::min(col * halationReduction + halationReduction / 2, lab->W - 1);
                float r;
                float g;
                float b;
                toCanonical(lab, sourceRow, sourceCol, inverse, r, g, b);
                const float y = luminance(r, g, b);
                const float peak = std::max(r, std::max(g, b));
                halationSource[row][col] = halationHighlightSourceV2(y, peak, 1.9f);
            }
        }

        const int fullShort = std::max(1, std::min(
            context.fullWidth > 0 ? context.fullWidth : lab->W * previewScale,
            context.fullHeight > 0 ? context.fullHeight : lab->H * previewScale));
        const float baseRadius = fullShort * (0.00085f + halation * 0.0013f);
        const int innerRadius = LIM(
            static_cast<int>(baseRadius / (previewScale * halationReduction) + 0.5f),
            1,
            40);
        const int outerRadius = LIM(
            static_cast<int>(baseRadius * 3.4f / (previewScale * halationReduction) + 0.5f),
            innerRadius + 1,
            96);
        boxblur(static_cast<float**>(halationSource), static_cast<float**>(halationInner), innerRadius, halationWidth, halationHeight, multiThread);
        boxblur(static_cast<float**>(halationSource), static_cast<float**>(halationOuter), outerRadius, halationWidth, halationHeight, multiThread);

#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < halationHeight; ++row) {
            for (int col = 0; col < halationWidth; ++col) {
                const float source = halationSource[row][col];
                halationInner[row][col] = std::max(halationInner[row][col] - source * 0.42f, 0.f) * halation;
                halationOuter[row][col] = std::max(halationOuter[row][col] - source * 0.18f, 0.f) * halation;
            }
        }
        halationSource.free();
    }

    const int scale = std::max(context.scale, 1);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int row = 0; row < lab->H; ++row) {
        for (int col = 0; col < lab->W; ++col) {
            const float originalL = lab->L[row][col];
            const float originalA = lab->a[row][col];
            const float originalB = lab->b[row][col];

            float r;
            float g;
            float b;
            toCanonical(lab, row, col, inverse, r, g, b);

            if (halationInner) {
                const float lowX = (col + 0.5f) / halationReduction - 0.5f;
                const float lowY = (row + 0.5f) / halationReduction - 0.5f;
                const float inner = sampleV3Plane(halationInner, halationWidth, halationHeight, lowX, lowY);
                const float outer = sampleV3Plane(halationOuter, halationWidth, halationHeight, lowX, lowY);
                r += inner * 0.42f + outer * 0.28f;
                g += inner * 0.10f + outer * 0.035f;
                b += inner * 0.012f + outer * 0.006f;
            }

            r = normalizedFilmResponse(r, channelExposure[0], contrast, toe, shoulder, blackResponse[0], middleResponse[0]);
            g = normalizedFilmResponse(g, channelExposure[1], contrast, toe, shoulder, blackResponse[1], middleResponse[1]);
            b = normalizedFilmResponse(b, channelExposure[2], contrast, toe, shoulder, blackResponse[2], middleResponse[2]);

            if (stock.stockClass != StockClass::Custom) {
                applyDyeCoupling(r, g, b, stock.stockClass);
            }
            applySaturation(r, g, b, saturation);
            applyVibrance(r, g, b, vibrance);

            if (process == "bw" || stock.stockClass == StockClass::Monochrome) {
                const float mono = 0.30f * r + 0.59f * g + 0.11f * b;
                r = mono * 1.008f;
                g = mono;
                b = mono * 0.988f;
            }

            r *= 1.f + warmth + tint * 0.35f;
            g *= 1.f - tint * 0.30f;
            b *= 1.f - warmth + tint * 0.18f;

            const float y = luminance(r, g, b);
            float shadowR;
            float shadowG;
            float shadowB;
            float highlightR;
            float highlightG;
            float highlightB;
            hueVector(static_cast<float>(fp.shadowHue), shadowR, shadowG, shadowB);
            hueVector(static_cast<float>(fp.highlightHue), highlightR, highlightG, highlightB);
            const float shadowWeight = (1.f - smoothStep(0.08f, 0.52f, y)) * fp.shadowTint / 900.f;
            const float highlightWeight = smoothStep(0.42f, 0.92f, y) * fp.highlightTint / 900.f;
            r += shadowR * shadowWeight + highlightR * highlightWeight;
            g += shadowG * shadowWeight + highlightG * highlightWeight;
            b += shadowB * shadowWeight + highlightB * highlightWeight;

            r = LIM(r, 0.f, 4.f) * MAXVALF;
            g = LIM(g, 0.f, 4.f) * MAXVALF;
            b = LIM(b, 0.f, 4.f) * MAXVALF;
            float x;
            float yy;
            float z;
            Color::rgbxyz(r, g, b, x, yy, z, matrix);
            float newL;
            float newA;
            float newB;
            Color::XYZ2Lab(x, yy, z, newL, newA, newB);
            lab->L[row][col] = intp(strength, newL, originalL);
            lab->a[row][col] = intp(strength, newA, originalA);
            lab->b[row][col] = intp(strength, newB, originalB);
        }
    }

    halationSource.free();
    halationInner.free();
    halationOuter.free();

    // Acutance is applied after the emulsion and output stages.
    if (acutance > 0.001f && lab->W > 2 && lab->H > 2) {
        array2D<float> blurredL(lab->W, lab->H, ARRAY2D_CLEAR_DATA);

        const int radius = LIM(static_cast<int>(1.4f / scale + 0.5f), 1, 3);
        boxblur(lab->L, static_cast<float**>(blurredL), radius, lab->W, lab->H, multiThread);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
        for (int row = 0; row < lab->H; ++row) {
            for (int col = 0; col < lab->W; ++col) {
                const float detail = LIM(lab->L[row][col] - blurredL[row][col], -3200.f, 3200.f);
                lab->L[row][col] = LIM(lab->L[row][col] + detail * acutance * strength * characterScale, 0.f, MAXVALF);
            }
        }
    }
}

} // namespace rtengine
