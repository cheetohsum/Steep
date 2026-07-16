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

inline float halationHighlightSource(float luminanceValue, float peakValue, float thresholdStops)
{
    const float highlight = std::max(luminanceValue, peakValue * 0.62f);
    const float stops = std::log2(std::max(highlight, 1e-6f) / 0.18f);
    const float excess = std::max(stops - thresholdStops, 0.f);
    const float onset = smoothStep(thresholdStops, thresholdStops + 2.5f, stops);
    return onset * (1.f - std::exp2(-excess * 0.55f));
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

inline std::uint32_t hash32(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

inline float hashNoise(int x, int y, std::uint32_t seed)
{
    const std::uint32_t h = hash32(
        static_cast<std::uint32_t>(x) * 0x1f123bb5u
        ^ static_cast<std::uint32_t>(y) * 0x5f356495u
        ^ seed);
    return static_cast<float>(h & 0x00ffffffu) * (2.f / 16777215.f) - 1.f;
}

inline float valueNoise(float x, float y, float cellSize, std::uint32_t seed)
{
    x /= cellSize;
    y /= cellSize;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;
    const float sx = fx * fx * (3.f - 2.f * fx);
    const float sy = fy * fy * (3.f - 2.f * fy);
    const float a = intp(sx, hashNoise(x0, y0, seed), hashNoise(x0 + 1, y0, seed));
    const float b = intp(sx, hashNoise(x0, y0 + 1, seed), hashNoise(x0 + 1, y0 + 1, seed));
    return intp(sy, a, b);
}

inline float filmLabGrain(float x, float y, float cellSize, std::uint32_t seed)
{
    const float fine = valueNoise(x, y, cellSize, seed);
    const float clump = valueNoise(x, y, cellSize * 2.7f, seed ^ 0x9e3779b9u);
    return fine * 0.72f + clump * 0.28f;
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
constexpr int V3_GRAIN_TILE_SIZE = 512;
constexpr int V3_GRAIN_TILE_MASK = V3_GRAIN_TILE_SIZE - 1;
constexpr int V3_GRAIN_TILE_SHIFT = 9;

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
    float contrast;
    float toe;
    float shoulder;
    float saturation;
    float warmth;
    std::array<float, 9> matrix;
    float softness;
};

struct FilmLabV3CurveBank {
    std::array<std::array<float, V3_DENSITY_LUT_SIZE + 1>, 3> density;
    std::array<std::array<float, V3_OUTPUT_LUT_SIZE + 1>, 3> output;
    std::array<float, 3> referenceDensity;
    std::array<float, 3> densitySlope;
    float baseFog;
    float maxDensity;
    float outputIndexScale;

    float sampleDensity(int channel, float value) const
    {
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

FilmLabV3Output makeV3Output(const Glib::ustring& output)
{
    FilmLabV3Output profile = {
        1.f, 0.08f, 0.12f, 1.f, 0.f,
        {{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f}},
        0.f
    };
    if (output == "ra4") {
        profile.contrast = 1.07f;
        profile.toe = 0.16f;
        profile.shoulder = 0.18f;
        profile.saturation = 1.035f;
        profile.warmth = 0.018f;
        profile.matrix = {{
            1.012f, -0.006f, -0.006f,
            -0.004f, 1.010f, -0.006f,
            -0.004f, 0.012f, 0.992f
        }};
        profile.softness = 0.08f;
    } else if (output == "projection") {
        profile.contrast = 1.12f;
        profile.toe = 0.10f;
        profile.shoulder = 0.10f;
        profile.saturation = 1.075f;
        profile.matrix = {{
            1.018f, -0.010f, -0.008f,
            -0.006f, 1.016f, -0.010f,
            -0.005f, -0.008f, 1.013f
        }};
        profile.softness = -0.05f;
    } else if (output == "cinema") {
        profile.contrast = 1.025f;
        profile.toe = 0.15f;
        profile.shoulder = 0.25f;
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
    float maxDensity)
{
    const float exposed = std::max(value * std::exp2(exposure), 1e-8f);
    const float stops = std::log2(exposed / 0.18f);
    const float toeStart = -2.75f + toe * 1.12f;
    const float shoulderStart = 3.35f - shoulder * 0.92f;
    const float toeSoftness = 0.44f + toe * 0.52f;
    const float shoulderSoftness = 0.58f + shoulder * 0.58f;
    const float density = baseFog + gamma * (
        softplus(stops - toeStart, toeSoftness)
        - softplus(stops - shoulderStart, shoulderSoftness));
    return LIM(density, baseFog, maxDensity);
}

FilmLabV3CurveBank makeV3Curves(
    const FilmLabV3Profile& stock,
    const FilmLabV3Process& process,
    const FilmLabV3Output& output,
    const procparams::FilmPresetsParams& fp)
{
    FilmLabV3CurveBank curves;
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

    for (int channel = 0; channel < 3; ++channel) {
        const float gamma = LIM(
            0.40f * stock.layerGamma[channel] * process.gamma * userContrast * (1.f + pushPull * 0.065f),
            0.20f,
            0.78f);
        const float toe = LIM(stock.layerToe[channel] + process.toe - pushPull * 0.025f + userToe, 0.f, 1.15f);
        const float shoulder = LIM(stock.layerShoulder[channel] + process.shoulder + userShoulder, 0.f, 1.45f);
        const float baselineExposure = stock.layerSpeed[channel] + process.speed;
        const float effectiveExposure = baselineExposure + exposure + channelShift[channel];

        for (int i = 0; i <= V3_DENSITY_LUT_SIZE; ++i) {
            const float value = V3_MAX_INPUT * i / V3_DENSITY_LUT_SIZE;
            curves.density[channel][i] = v3DensityResponse(
                value,
                effectiveExposure,
                gamma,
                toe,
                shoulder,
                curves.baseFog,
                curves.maxDensity);
        }

        const float baselineGamma = LIM(0.40f * stock.layerGamma[channel] * process.gamma, 0.20f, 0.78f);
        curves.referenceDensity[channel] = v3DensityResponse(
            0.18f,
            baselineExposure,
            baselineGamma,
            LIM(stock.layerToe[channel] + process.toe, 0.f, 1.15f),
            LIM(stock.layerShoulder[channel] + process.shoulder, 0.f, 1.45f),
            curves.baseFog,
            curves.maxDensity);
        curves.densitySlope[channel] = baselineGamma;

        for (int i = 0; i <= V3_OUTPUT_LUT_SIZE; ++i) {
            const float density = curves.baseFog
                + (curves.maxDensity - curves.baseFog) * i / V3_OUTPUT_LUT_SIZE;
            float stops = (density - curves.referenceDensity[channel])
                / std::max(curves.densitySlope[channel], 0.08f);
            if (stops < 0.f) {
                stops /= 1.f + output.toe * (-stops) * 0.15f;
            } else {
                stops /= 1.f + output.shoulder * stops * 0.15f;
            }
            curves.output[channel][i] = LIM(0.18f * std::exp2(stops * output.contrast), 0.f, V3_MAX_INPUT);
        }

        // Base-plus-fog is a density baseline in the negative, not display haze.
        // Remove D-min and re-anchor middle gray while retaining toe and shoulder shape.
        const float outputBlack = curves.output[channel][0];
        const float outputScale = 0.18f / std::max(0.18f - outputBlack, 1e-6f);
        for (int i = 0; i <= V3_OUTPUT_LUT_SIZE; ++i) {
            curves.output[channel][i] = LIM(
                (curves.output[channel][i] - outputBlack) * outputScale,
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

inline void applyV3Spectral(const FilmLabV3Profile& profile, float r, float g, float b, float density[3])
{
    r = std::max(r, 0.f);
    g = std::max(g, 0.f);
    b = std::max(b, 0.f);
    density[0] = profile.spectral[0] * r + profile.spectral[1] * g + profile.spectral[2] * b;
    density[1] = profile.spectral[3] * r + profile.spectral[4] * g + profile.spectral[5] * b;
    density[2] = profile.spectral[6] * r + profile.spectral[7] * g + profile.spectral[8] * b;
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

inline float v3TileNoise(array2D<float>& tile, int mask, int x, int y, int offsetX, int offsetY)
{
    const int blockX = x >> V3_GRAIN_TILE_SHIFT;
    const int blockY = y >> V3_GRAIN_TILE_SHIFT;
    const unsigned int tileX = static_cast<unsigned int>(x + blockY * 521 + offsetX) & mask;
    const unsigned int tileY = static_cast<unsigned int>(y + blockX * 733 + offsetY) & mask;
    return tile[tileY][tileX];
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
    bool multiThread)
{
    const FilmLabStock& stockRecord = findStock(fp.preset);
    const Glib::ustring processName = fp.process == "auto" ? defaultProcess(stockRecord.stockClass) : fp.process;
    const FilmLabV3Profile stock = makeV3Profile(stockRecord);
    const FilmLabV3Process process = makeV3Process(processName);
    const FilmLabV3Output output = makeV3Output(fp.output);
    const FilmLabV3CurveBank curves = makeV3Curves(stock, process, output, fp);

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

    const int scale = std::max(context.scale, 1);
    const int fullWidth = context.fullWidth > 0 ? context.fullWidth : lab->W * scale;
    const int fullHeight = context.fullHeight > 0 ? context.fullHeight : lab->H * scale;
    const int fullShort = std::max(1, std::min(fullWidth, fullHeight));
    const float strength = LIM(fp.strength / 100.f, 0.f, 1.f);
    const float characterScale = filmCharacterScale(strength);
    const float halation = LIM((stockRecord.halation + fp.halation / 155.f) * characterScale, 0.f, 0.92f);
    const float bloom = LIM((stock.bloom + fp.bloom / 180.f) * characterScale, 0.f, 0.65f);
    const float thresholdStops = LIM(2.1f + fp.halationThreshold / 75.f, 0.75f, 3.45f);
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
                toCanonicalV3(lab, sourceRow, sourceCol, inverse, r, g, b);
                const float y = std::max(luminance(r, g, b), 0.f);
                const float peak = std::max(r, std::max(g, b));
                halationSource[row][col] = halationHighlightSource(y, peak, thresholdStops);
            }
        }

        const float baseRadius = fullShort * (0.0011f + halation * 0.0020f) * halationSize;
        const int innerRadius = LIM(
            static_cast<int>(baseRadius / (scale * halationReduction) + 0.5f),
            1,
            48);
        const int outerRadius = LIM(
            static_cast<int>(baseRadius * 3.25f / (scale * halationReduction) + 0.5f),
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
                const float inner = std::max(halationInner[row][col] - source * 0.32f, 0.f);
                const float outer = std::max(halationOuter[row][col] - source * 0.18f, 0.f);
                halationInner[row][col] = halation * (inner * stock.halationInner + outer * stock.halationOuter);
                halationOuter[row][col] = bloom * (inner * 0.72f + outer * 0.28f);
            }
        }
        halationSource.free();
    }

    float grainCells = 2300.f;
    if (fp.format == "120") {
        grainCells = 3600.f;
    } else if (fp.format == "large") {
        grainCells = 5400.f;
    }
    const float grainSize = std::exp2(fp.grainSize / 100.f * 1.45f);
    const float grainCellSize = std::max(fullShort / grainCells * grainSize, 0.72f);
    const float inverseGrainCellSize = 1.f / grainCellSize;
    const float inverseDensityRange = 1.f / std::max(curves.maxDensity - curves.baseFog, 1e-5f);
    const float pushPull = LIM(static_cast<float>(fp.pushPull), -2.f, 3.f);
    const float grainAmount = LIM(
        (stockRecord.grain + fp.grain / 145.f)
        * process.grain
        * (1.f + std::max(pushPull, 0.f) * 0.15f)
        * characterScale,
        0.f,
        1.15f);
    const float grainColor = LIM(stock.grainColor + fp.grainColor / 220.f, 0.f, 0.72f);
    const float grainClumping = LIM(stock.grainClumping + fp.grainClumping / 250.f, 0.04f, 0.72f);
    const float previewAttenuation = 1.f / std::sqrt(static_cast<float>(scale));
    const std::uint32_t seed = context.imageSeed ? context.imageSeed : 1u;
    const float coupling = LIM(stock.coupling * (1.f + fp.layerCoupling / 120.f), 0.f, 0.58f);
    const float saturation = LIM(
        stockRecord.saturation * process.saturation * output.saturation * (1.f + fp.saturation / 160.f),
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
    array2D<float> grainTile;
    if (grainAmount > 0.001f) {
        grainTile(V3_GRAIN_TILE_SIZE, V3_GRAIN_TILE_SIZE);
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int row = 0; row < V3_GRAIN_TILE_SIZE; ++row) {
            for (int col = 0; col < V3_GRAIN_TILE_SIZE; ++col) {
                grainTile[row][col] = hashNoise(col, row, seed ^ 0x6a09e667u);
            }
        }
    }

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int row = 0; row < lab->H; ++row) {
        for (int col = 0; col < lab->W; ++col) {
            float sourceR;
            float sourceG;
            float sourceB;
            toCanonicalV3(lab, row, col, inverse, sourceR, sourceG, sourceB);
            float r = sourceR;
            float g = sourceG;
            float b = sourceB;

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
            applyV3DensityCoupling(density, stockRecord.stockClass, coupling, curves.baseFog, curves.maxDensity);

            if (grainAmount > 0.001f) {
                const float fullX = static_cast<float>(context.originX + col * scale);
                const float fullY = static_cast<float>(context.originY + row * scale);
                const float cellX = fullX * inverseGrainCellSize;
                const float cellY = fullY * inverseGrainCellSize;
                const int grainX = static_cast<int>(std::floor(cellX));
                const int grainY = static_cast<int>(std::floor(cellY));
                const float fine = v3TileNoise(grainTile, V3_GRAIN_TILE_MASK, grainX, grainY, 0, 0);
                if (scale > 1) {
                    // A preview pixel integrates several film grains. A single
                    // achromatic sample avoids resolving detail below its Nyquist limit.
                    const float previewNoise = fine * grainAmount * 0.028f * previewAttenuation;
                    density[0] += previewNoise;
                    density[1] += previewNoise;
                    density[2] += previewNoise;
                } else {
                    const float meanDensity = (density[0] + density[1] + density[2]) / 3.f;
                    const float normalizedDensity = LIM(
                        (meanDensity - curves.baseFog) * inverseDensityRange,
                        0.f,
                        1.f);
                    const float densityOffset = (normalizedDensity - stock.grainMidpoint) / std::max(stock.grainWidth, 0.08f);
                    float densityBell = std::max(1.f - 0.45f * densityOffset * densityOffset, 0.f);
                    densityBell *= densityBell;
                    const float densityVariance = 0.34f + 0.82f * densityBell + 0.18f * normalizedDensity;
                    const float amplitude = grainAmount * densityVariance * 0.034f;
                    const float cloud = v3TileNoise(
                        grainTile,
                        V3_GRAIN_TILE_MASK,
                        grainX / 4,
                        grainY / 4,
                        613,
                        1429);
                    const float common = fine * (1.f + cloud * grainClumping * 0.62f);
                    if (stockRecord.stockClass == StockClass::Monochrome || processName == "bw") {
                        density[0] += common * amplitude;
                        density[1] += common * amplitude;
                        density[2] += common * amplitude;
                    } else {
                        const float redGreen = v3TileNoise(grainTile, V3_GRAIN_TILE_MASK, grainX, grainY, 887, 271);
                        const float blueYellow = v3TileNoise(grainTile, V3_GRAIN_TILE_MASK, grainX, grainY, 1559, 941);
                        const float neutral = common * (1.f - grainColor);
                        density[0] += (neutral + (redGreen * 0.72f + blueYellow * 0.28f) * grainColor) * amplitude;
                        density[1] += (neutral + (-redGreen * 0.46f + blueYellow * 0.16f) * grainColor) * amplitude;
                        density[2] += (neutral - blueYellow * 0.78f * grainColor) * amplitude;
                    }
                }
                density[0] = LIM(density[0], curves.baseFog, curves.maxDensity);
                density[1] = LIM(density[1], curves.baseFog, curves.maxDensity);
                density[2] = LIM(density[2], curves.baseFog, curves.maxDensity);
            }

            r = curves.sampleOutput(0, density[0]);
            g = curves.sampleOutput(1, density[1]);
            b = curves.sampleOutput(2, density[2]);
            if (outputMatrixActive) {
                applyV3OutputMatrix(output, r, g, b);
            }

            if (stockRecord.stockClass == StockClass::Monochrome || processName == "bw") {
                const float mono = 0.272229f * r + 0.674082f * g + 0.053689f * b;
                r = mono * 1.004f;
                g = mono;
                b = mono * 0.993f;
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
    grainTile.free();

    const float detailGain = LIM(
        stock.acutance - output.softness - fp.outputSoftness / 150.f,
        -0.72f,
        0.58f) * strength * characterScale;
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
        filmPresetsV3(lab, fp, context, multiThread);
        return;
    }

    const FilmLabStock& stock = findStock(fp.preset);
    const Glib::ustring process = fp.process == "auto" ? defaultProcess(stock.stockClass) : fp.process;

    float processContrast = 1.f;
    float processToe = 0.f;
    float processShoulder = 0.f;
    float processSaturation = 1.f;
    float processGrain = 1.f;

    if (process == "e6") {
        processContrast = 1.10f;
        processToe = -0.05f;
        processShoulder = -0.08f;
        processSaturation = 1.10f;
        processGrain = 0.82f;
    } else if (process == "ecn2") {
        processContrast = 0.92f;
        processShoulder = 0.16f;
        processSaturation = 0.92f;
        processGrain = 0.92f;
    } else if (process == "bw") {
        processContrast = 1.05f;
        processToe = 0.08f;
        processSaturation = 0.f;
        processGrain = 1.16f;
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
    const float grain = LIM(
        (stock.grain + fp.grain / 150.f)
        * processGrain
        * (1.f + std::max(pushPull, 0.f) * 0.16f)
        * characterScale,
        0.f,
        1.15f);
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
                halationSource[row][col] = halationHighlightSource(y, peak, 1.9f);
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
    const int fullWidth = context.fullWidth > 0 ? context.fullWidth : lab->W * scale;
    const int fullHeight = context.fullHeight > 0 ? context.fullHeight : lab->H * scale;
    const int fullShort = std::max(1, std::min(fullWidth, fullHeight));
    float grainCells = 2200.f;
    if (fp.format == "120") {
        grainCells = 3400.f;
    } else if (fp.format == "large") {
        grainCells = 5000.f;
    }
    const float grainCellSize = std::max(fullShort / grainCells, 1.f);
    const std::uint32_t seed = context.imageSeed ? context.imageSeed : 1u;

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

            if (grain > 0.001f) {
                const float fullX = static_cast<float>(context.originX + col * scale);
                const float fullY = static_cast<float>(context.originY + row * scale);
                const float density = 1.f - smoothStep(0.12f, 0.88f, luminance(r, g, b));
                const float previewAttenuation = 1.f / std::sqrt(static_cast<float>(scale));
                const float monochromeNoise = filmLabGrain(fullX, fullY, grainCellSize, seed) * grain * (0.018f + density * 0.022f) * previewAttenuation;
                const float colorNoise = filmLabGrain(fullX, fullY, grainCellSize * 1.35f, seed ^ 0xa511e9b3u) * grain * 0.0045f * previewAttenuation;
                r = std::max(r + monochromeNoise + colorNoise, 0.f);
                g = std::max(g + monochromeNoise, 0.f);
                b = std::max(b + monochromeNoise - colorNoise, 0.f);
            }

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
