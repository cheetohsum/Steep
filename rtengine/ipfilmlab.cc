/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include <algorithm>
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
    const float halation = LIM(stock.halation + fp.halation / 180.f, 0.f, 0.72f);
    const float grain = LIM((stock.grain + fp.grain / 150.f) * processGrain * (1.f + std::max(pushPull, 0.f) * 0.16f), 0.f, 1.f);
    const float vibrance = LIM(fp.vibrance / 100.f, -1.f, 1.f);
    const float acutance = LIM(stock.acutance, 0.f, 0.42f);
    const float strength = LIM(fp.strength / 100.f, 0.f, 1.f);

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
    array2D<float> halationBlur;
    if (halation > 0.001f) {
        halationSource(lab->W, lab->H, ARRAY2D_CLEAR_DATA);
        halationBlur(lab->W, lab->H, ARRAY2D_CLEAR_DATA);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
        for (int row = 0; row < lab->H; ++row) {
            for (int col = 0; col < lab->W; ++col) {
                float r;
                float g;
                float b;
                toCanonical(lab, row, col, inverse, r, g, b);
                const float y = luminance(r, g, b);
                const float peak = std::max(r, std::max(g, b));
                halationSource[row][col] = smoothStep(0.62f, 1.8f, std::max(y, peak * 0.62f)) * std::min(peak, 3.f) / 3.f;
            }
        }

        const int fullShort = std::max(1, std::min(
            context.fullWidth > 0 ? context.fullWidth : lab->W * std::max(context.scale, 1),
            context.fullHeight > 0 ? context.fullHeight : lab->H * std::max(context.scale, 1)));
        const int radius = LIM(static_cast<int>(fullShort * (0.0016f + halation * 0.0022f) / std::max(context.scale, 1) + 0.5f), 1, 64);
        boxblur(static_cast<float**>(halationSource), static_cast<float**>(halationBlur), radius, lab->W, lab->H, multiThread);
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

            if (halationBlur) {
                const float spill = std::max(halationBlur[row][col] - halationSource[row][col] * 0.38f, 0.f) * halation;
                r += spill * 0.34f;
                g += spill * 0.095f;
                b += spill * 0.018f;
            }

            r = filmResponse(r, exposure + stock.redBias + fp.redShift / 500.f, contrast, toe, shoulder);
            g = filmResponse(g, exposure + stock.greenBias + fp.greenShift / 500.f, contrast, toe, shoulder);
            b = filmResponse(b, exposure + stock.blueBias + fp.blueShift / 500.f, contrast, toe, shoulder);

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
    halationBlur.free();

    // Acutance is applied after the emulsion and output stages. Keeping these
    // planes scoped after halation caps peak scratch memory at two float planes.
    if (acutance > 0.001f && lab->W > 2 && lab->H > 2) {
        array2D<float> sourceL(lab->W, lab->H);
        array2D<float> blurredL(lab->W, lab->H, ARRAY2D_CLEAR_DATA);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
        for (int row = 0; row < lab->H; ++row) {
            for (int col = 0; col < lab->W; ++col) {
                sourceL[row][col] = lab->L[row][col];
            }
        }

        const int radius = LIM(static_cast<int>(1.4f / scale + 0.5f), 1, 3);
        boxblur(static_cast<float**>(sourceL), static_cast<float**>(blurredL), radius, lab->W, lab->H, multiThread);

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
        for (int row = 0; row < lab->H; ++row) {
            for (int col = 0; col < lab->W; ++col) {
                const float detail = LIM(sourceL[row][col] - blurredL[row][col], -3200.f, 3200.f);
                lab->L[row][col] = LIM(lab->L[row][col] + detail * acutance * strength, 0.f, MAXVALF);
            }
        }
    }
}

} // namespace rtengine
