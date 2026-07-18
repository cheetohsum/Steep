/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *  Copyright (c) 2011 Oliver Duis <www.oliverduis.de>
 *  Copyright (c) 2011 Michael Ezra <www.michaelezra.com>
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
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glibmm/ustring.h>

#include "filebrowser.h"

#include "autoedit.h"
#include "batchqueue.h"
#include "clipboard.h"
#include "filepanel.h"
#include "inspector.h"
#include "multilangmgr.h"
#include "options.h"
#include "paramsedited.h"
#include "profilestorecombobox.h"
#include "procparamchangers.h"
#include "rtimage.h"
#include "threadutils.h"
#include "thumbnail.h"
#include "thumbimageupdater.h"

#include "rtengine/dfmanager.h"
#include "rtengine/ffmanager.h"
#include "rtengine/iimage.h"
#include "rtengine/imagesource.h"
#include "rtengine/perspectivecorrection.h"
#include "rtengine/procparams.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#endif

namespace
{

using QuickWarmClock = std::chrono::steady_clock;

std::atomic<unsigned> quickPreviewCacheWarmGeneration{0};
std::mutex fileBrowserPerfLogMutex;

using AutoEditMode = SteepAutoEditMode;

enum class AutoGradeScene {
    Neutral,
    Portrait,
    GoldenHour,
    Landscape,
    Night,
    Urban
};

struct AutoGradeFeatures {
    bool valid = false;
    AutoGradeScene scene = AutoGradeScene::Neutral;
    double medianLuma = 0.5;
    double dynamicRange = 0.5;
    double shadowFraction = 0.0;
    double highlightFraction = 0.0;
    double saturation = 0.0;
    double warmFraction = 0.0;
    double brightWarmFraction = 0.0;
    double coolFraction = 0.0;
    double skinFraction = 0.0;
    double skinSaturation = 0.0;
    double centerSkinFraction = 0.0;
    double skyFraction = 0.0;
    double foliageFraction = 0.0;
    double edgeDensity = 0.0;
    double strongEdgeFraction = 0.0; // share of neighbor gradients > 0.09 (real edges)
    double clippedFraction = 0.0;    // share of pixels with luma > 0.985 (true clips)
    unsigned iso = 100;
};

const char* autoGradeSceneName(AutoGradeScene scene)
{
    switch (scene) {
        case AutoGradeScene::Portrait: return "portrait";
        case AutoGradeScene::GoldenHour: return "golden-hour";
        case AutoGradeScene::Landscape: return "landscape";
        case AutoGradeScene::Night: return "night";
        case AutoGradeScene::Urban: return "urban";
        case AutoGradeScene::Neutral: return "neutral";
    }
    return "neutral";
}

const char* autoEditModeLabel(AutoEditMode mode)
{
    switch (mode) {
        case AutoEditMode::Grade: return "FILEBROWSER_POPUPAUTOGRADE";
        case AutoEditMode::GradeFilm: return "FILEBROWSER_POPUPFILMLAB";
        case AutoEditMode::GradedFilm: return "FILEBROWSER_POPUPAUTOGRADEFILM";
        case AutoEditMode::Neutral: return "FILEBROWSER_POPUPAUTOEDITNEUTRAL";
    }
    return "FILEBROWSER_POPUPAUTOEDITNEUTRAL";
}

const char* autoEditModeName(AutoEditMode mode)
{
    switch (mode) {
        case AutoEditMode::Grade: return "grade";
        case AutoEditMode::GradeFilm: return "film-lab";
        case AutoEditMode::GradedFilm: return "graded-film-lab";
        case AutoEditMode::Neutral: return "neutral";
    }
    return "neutral";
}

double hueDegrees(double red, double green, double blue, double maximum, double chroma)
{
    if (chroma <= 1e-8) {
        return 0.0;
    }

    double hue = 0.0;
    if (maximum == red) {
        hue = 60.0 * std::fmod((green - blue) / chroma, 6.0);
    } else if (maximum == green) {
        hue = 60.0 * ((blue - red) / chroma + 2.0);
    } else {
        hue = 60.0 * ((red - green) / chroma + 4.0);
    }
    return hue < 0.0 ? hue + 360.0 : hue;
}

AutoGradeFeatures analyzeSteepAutoGrade(Thumbnail& thumbnail)
{
    AutoGradeFeatures features;
    if (const auto* cache = thumbnail.getCacheImageData()) {
        features.iso = std::max(cache->iso, 1u);
    }

    rtengine::procparams::ProcParams neutral;
    neutral.setDefaults();
    double scale = 1.0;
    std::unique_ptr<rtengine::IImage8> image(thumbnail.processFullThumbImage(neutral, 224, scale));
    if (!image || !image->getData() || image->getWidth() < 8 || image->getHeight() < 8) {
        return features;
    }

    const int width = image->getWidth();
    const int height = image->getHeight();
    const auto* pixels = image->getData();
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::array<size_t, 64> histogram{};
    std::vector<float> luma(pixelCount, 0.f);
    size_t shadows = 0;
    size_t highlights = 0;
    size_t clipped = 0;
    size_t warm = 0;
    size_t brightWarm = 0;
    size_t cool = 0;
    size_t skin = 0;
    size_t centerSkin = 0;
    size_t centerPixels = 0;
    size_t sky = 0;
    size_t foliage = 0;
    double saturationSum = 0.0;
    double skinSaturationSum = 0.0;

    for (int y = 0; y < height; ++y) {
        const double ny = static_cast<double>(y) / std::max(height - 1, 1);
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const double red = pixels[index * 3] / 255.0;
            const double green = pixels[index * 3 + 1] / 255.0;
            const double blue = pixels[index * 3 + 2] / 255.0;
            const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            const double maximum = std::max({red, green, blue});
            const double minimum = std::min({red, green, blue});
            const double chroma = maximum - minimum;
            const double saturation = maximum > 1e-8 ? chroma / maximum : 0.0;
            const double hue = hueDegrees(red, green, blue, maximum, chroma);
            const double channelSum = std::max(red + green + blue, 1e-8);
            const double redShare = red / channelSum;
            const double greenShare = green / channelSum;
            const double blueShare = blue / channelSum;
            const double nx = static_cast<double>(x) / std::max(width - 1, 1);
            const bool inCenter = nx >= 0.24 && nx <= 0.76 && ny >= 0.16 && ny <= 0.86;
            const bool isSkin = hue >= 7.0 && hue <= 50.0
                && saturation >= 0.10 && saturation <= 0.58
                && luminance >= 0.16 && luminance <= 0.92
                && red > green * 1.01 && green >= blue * 0.92
                && redShare >= 0.35 && redShare <= 0.52
                && greenShare >= 0.25 && greenShare <= 0.40
                && blueShare >= 0.14 && blueShare <= 0.31;

            luma[index] = static_cast<float>(luminance);
            histogram[std::min<size_t>(histogram.size() - 1, luminance * histogram.size())]++;
            saturationSum += saturation;
            shadows += luminance < 0.16;
            highlights += luminance > 0.84;
            clipped += luminance > 0.985;
            warm += saturation > 0.12 && (hue <= 70.0 || hue >= 340.0);
            brightWarm += luminance > 0.55 && saturation > 0.12 && (hue <= 70.0 || hue >= 340.0);
            cool += saturation > 0.12 && hue >= 170.0 && hue <= 270.0;
            skin += isSkin;
            skinSaturationSum += isSkin ? saturation : 0.0;
            centerSkin += inCenter && isSkin;
            centerPixels += inCenter;
            sky += ny < 0.58 && hue >= 175.0 && hue <= 255.0 && saturation > 0.14 && luminance > 0.28;
            foliage += hue >= 62.0 && hue <= 168.0 && saturation > 0.18 && luminance > 0.10 && luminance < 0.84;
        }
    }

    auto percentile = [&](double fraction) {
        const size_t target = static_cast<size_t>(std::round(fraction * (pixelCount - 1)));
        size_t accumulated = 0;
        for (size_t i = 0; i < histogram.size(); ++i) {
            accumulated += histogram[i];
            if (accumulated > target) {
                return (i + 0.5) / histogram.size();
            }
        }
        return 1.0;
    };

    double edgeSum = 0.0;
    size_t edgeSamples = 0;
    size_t strongEdges = 0;
    for (int y = 1; y < height; ++y) {
        for (int x = 1; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const double dh = std::abs(luma[index] - luma[index - 1]);
            const double dv = std::abs(luma[index] - luma[index - width]);
            edgeSum += dh + dv;
            strongEdges += (dh > 0.09) + (dv > 0.09);
            edgeSamples += 2;
        }
    }

    const double count = static_cast<double>(pixelCount);
    features.valid = true;
    features.medianLuma = percentile(0.5);
    features.dynamicRange = percentile(0.9) - percentile(0.1);
    features.shadowFraction = shadows / count;
    features.highlightFraction = highlights / count;
    features.saturation = saturationSum / count;
    features.warmFraction = warm / count;
    features.brightWarmFraction = brightWarm / count;
    features.coolFraction = cool / count;
    features.skinFraction = skin / count;
    features.skinSaturation = skin ? skinSaturationSum / skin : 0.0;
    features.centerSkinFraction = centerPixels ? static_cast<double>(centerSkin) / centerPixels : 0.0;
    features.skyFraction = sky / count;
    features.foliageFraction = foliage / count;
    features.edgeDensity = edgeSamples ? edgeSum / edgeSamples : 0.0;
    features.strongEdgeFraction = edgeSamples ? static_cast<double>(strongEdges) / edgeSamples : 0.0;
    features.clippedFraction = clipped / count;

    if (features.skinFraction > 0.045
            && features.centerSkinFraction > 0.075
            && features.skinSaturation < 0.44
            && (features.saturation < 0.33 || features.dynamicRange > 0.28)) {
        features.scene = AutoGradeScene::Portrait;
    } else if (features.brightWarmFraction > 0.085
            && features.warmFraction > features.coolFraction * 1.22) {
        features.scene = AutoGradeScene::GoldenHour;
    } else if (features.medianLuma < 0.29 && features.shadowFraction > 0.38) {
        features.scene = AutoGradeScene::Night;
    } else if ((features.skyFraction > 0.07 || features.foliageFraction > 0.13)
            && features.skinFraction < 0.035) {
        features.scene = AutoGradeScene::Landscape;
    } else if (features.edgeDensity > 0.075 && features.foliageFraction < 0.10) {
        features.scene = AutoGradeScene::Urban;
    }

    return features;
}

void restoreSteepAutoEditGeometry(
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& target)
{
    // Auto Edit owns the tonal recipe, but framing and alignment belong to the
    // photographer. Preserve them when the neutral profile is constructed.
    target.crop = source.crop;
    target.coarse = source.coarse;
    target.commonTrans = source.commonTrans;
    target.rotate = source.rotate;
    target.perspective = source.perspective;
}

// Start Auto Edit from a known neutral profile so repeated runs and previously
// edited images produce the same result.
void applySteepAutoEdit(
    Thumbnail& thumbnail,
    const AutoGradeFeatures& features,
    rtengine::procparams::ProcParams& params)
{
    constexpr double AUTO_EDIT_RESPONSE = 0.60;
    params.setDefaults();

    auto& tone = params.toneCurve;
    tone.autoexp = true;
    tone.clip = 0.02;
    tone.hrenabled = false;
    tone.method = "Coloropp";
    tone.expcomp = std::numeric_limits<double>::quiet_NaN();
    thumbnail.applyAutoExp(params);

    const bool measuredExposure = std::isfinite(tone.expcomp);
    if (!measuredExposure) {
        tone.expcomp = 0.20;
        tone.brightness = 2;
        tone.contrast = 0;
        tone.black = 0;
        tone.hlcompr = 35;
        tone.hlcomprthresh = 0;
    }

    // Give normally exposed and dark frames a modest lift while respecting
    // the highlight compression selected by histogram analysis.
    const double exposureLift = tone.expcomp > 0.0
        ? (tone.hlcompr < 55 ? 0.05 : 0.0)
        : 0.0;
    tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp + exposureLift));

    // Histogram auto exposure is the primary decision. Scene luminance only
    // supplies a restrained correction so night remains night and bright
    // frames retain highlight headroom.
    if (features.valid) {
        double intent = 0.0;
        if (features.medianLuma < 0.19 && features.highlightFraction < 0.09) {
            intent = features.scene == AutoGradeScene::Night ? 0.02 : 0.12;
        } else if (features.medianLuma < 0.32 && features.highlightFraction < 0.14) {
            intent = features.scene == AutoGradeScene::Night ? 0.0 : 0.05;
        } else if (features.medianLuma > 0.61 || features.highlightFraction > 0.24) {
            intent = -0.24;
        }
        tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp + intent));
        tone.hlcompr = std::max(tone.hlcompr, static_cast<int>(std::round(
            std::min(78.0, 35.0 + features.highlightFraction * 160.0))));
    }
    if (tone.expcomp > 0.0) {
        tone.expcomp *= AUTO_EDIT_RESPONSE;
    }
    tone.brightness = std::max(0, std::min(35, static_cast<int>(std::round(
        std::max(0, tone.brightness + 2) * AUTO_EDIT_RESPONSE))));
    tone.contrast = std::max(0, std::min(40, static_cast<int>(std::round(
        std::max(0, tone.contrast + 5) * AUTO_EDIT_RESPONSE))));
    tone.autoexp = false;
    tone.curve = {DCT_Linear};
    tone.curve2 = {DCT_Linear};
    tone.curveR = {DCT_Linear};
    tone.curveG = {DCT_Linear};
    tone.curveB = {DCT_Linear};
    tone.curveMode = rtengine::procparams::ToneCurveMode::STD;
    tone.curveMode2 = rtengine::procparams::ToneCurveMode::STD;
    tone.shcompr = 50;
    tone.hlcompr = std::max(tone.hlcompr, tone.expcomp > 0.25 ? 55 : 35);
    tone.hlbl = 0;
    tone.hlth = 1.0;
    tone.histmatching = false;
    tone.fromHistMatching = false;
    tone.clampOOG = true;

    int highlightRecovery = 16;
    int shadowLift = 1;
    if (features.valid) {
        highlightRecovery = std::max(12, std::min(38, static_cast<int>(std::round(
            12.0 + features.highlightFraction * 72.0 + std::max(0.0, tone.expcomp) * 5.0))));

        double lift = std::max(0.0, (features.shadowFraction - 0.28) * 12.0)
            + std::max(0.0, (0.25 - features.medianLuma) * 10.0);
        if (features.highlightFraction > 0.14) {
            lift *= 0.40;
        }
        if (features.medianLuma > 0.44) {
            lift *= 0.20;
        }
        shadowLift = std::max(0, std::min(4, static_cast<int>(std::round(lift))));

        if (features.scene == AutoGradeScene::Night) {
            shadowLift = std::min(shadowLift, 3);
        } else if (features.highlightFraction > 0.24 || features.medianLuma > 0.61) {
            shadowLift = 0;
        }
    }

    auto& shadowsHighlights = params.sh;
    shadowsHighlights.enabled = highlightRecovery != 0 || shadowLift != 0;
    shadowsHighlights.highlights = highlightRecovery;
    shadowsHighlights.htonalwidth = 70;
    shadowsHighlights.shadows = shadowLift;
    shadowsHighlights.stonalwidth = 24 + shadowLift * 2;
    shadowsHighlights.radius = 40;
    shadowsHighlights.lab = false;

    // Artistic S-curve emphasis on top of the base recipe. A colorist working
    // a flat file takes contrast from two places: deeper blacks when the scan
    // is flat, and brighter upper-mids when there is genuine highlight
    // headroom. Frames whose statistics allow it get up to an extra ~15% on
    // either end; frames that cannot take it (clipped highlights, shadow-heavy
    // night frames, faces) are left on the restrained base curve.
    double toeBoost = 0.0;   // extra pull-down of the shadow point (0..0.15)
    double liftBoost = 0.0;  // extra push-up of the bright point (0..0.15)
    if (features.valid) {
        // Right side: only brighten when few pixels already sit near clipping.
        const double headroom = std::max(0.0, (0.12 - features.highlightFraction) / 0.12);
        liftBoost = 0.15 * std::min(1.0, headroom);
        if (features.medianLuma > 0.55) {
            liftBoost *= 0.5;  // high-key frame: restraint, keep the airy top
        }
        if (features.scene == AutoGradeScene::Portrait) {
            liftBoost = std::min(liftBoost, 0.08);  // skin brights must not race to clipping
        } else if (features.scene == AutoGradeScene::Night) {
            liftBoost *= 0.7;  // sparkle in the lights without blooming them
        }

        // Left side: deepen blacks on flat files; ease off when shadows
        // already dominate the frame.
        const double flatness = std::max(0.0, (0.62 - features.dynamicRange) / 0.62);
        const double shadowRoom = std::max(0.0, (0.40 - features.shadowFraction) / 0.40);
        toeBoost = 0.15 * shadowRoom * (0.5 + 0.5 * flatness);
        if (features.scene == AutoGradeScene::Night) {
            toeBoost *= 0.4;   // night keeps its shadow detail and mood
        } else if (features.scene == AutoGradeScene::Portrait) {
            toeBoost *= 0.75;  // gentle falloff on faces
        }
    }

    constexpr double SHADOW_IN = 0.098654708520179366;
    constexpr double LOWER_MID_IN = 0.20673525015387323;
    constexpr double BRIGHT_IN = 0.51569506726457359;
    double shadowOut = SHADOW_IN + AUTO_EDIT_RESPONSE * (0.058295964125560533 - SHADOW_IN);
    const double lowerMidOut = LOWER_MID_IN + AUTO_EDIT_RESPONSE * (0.21570386001934416 - LOWER_MID_IN);
    double brightOut = BRIGHT_IN + AUTO_EDIT_RESPONSE * (0.73094170403587488 - BRIGHT_IN);
    const double topOut = 1.0 + AUTO_EDIT_RESPONSE * (0.99103139013452912 - 1.0);

    shadowOut *= 1.0 - toeBoost;
    brightOut = std::min(0.985, brightOut * (1.0 + liftBoost));

    auto& curves = params.rgbCurves;
    curves.enabled = true;
    curves.lumamode = false;
    curves.mastercurve = {
        DCT_Spline,
        0.0, 0.0,
        SHADOW_IN, shadowOut,
        LOWER_MID_IN, lowerMidOut,
        BRIGHT_IN, brightOut,
        1.0, topOut
    };
    curves.rcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
    curves.gcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
    curves.bcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
}

void applySteepAutoGrade(
    const AutoGradeFeatures& features,
    rtengine::procparams::ProcParams& params)
{
    auto& grade = params.colorGrading;
    grade = rtengine::procparams::ColorGradingParams();
    grade.enabled = true;
    grade.shadowsHue = 218.0;
    grade.shadowsSat = 0.085;
    grade.shadowsLum = 1.0;
    grade.midtonesHue = 32.0;
    grade.midtonesSat = 0.030;
    grade.midtonesLum = 1.0;
    grade.highlightsHue = 42.0;
    grade.highlightsSat = 0.075;
    grade.highlightsLum = -1.0;
    grade.blending = 70.0;

    switch (features.scene) {
        case AutoGradeScene::Portrait:
            grade.shadowsHue = 218.0;
            grade.shadowsSat = 0.050;
            grade.midtonesHue = 28.0;
            grade.midtonesSat = 0.045;
            grade.midtonesLum = 1.5;
            grade.highlightsHue = 40.0;
            grade.highlightsSat = 0.055;
            grade.highlightsLum = -0.5;
            grade.blending = 74.0;
            grade.balance = 8.0;
            break;
        case AutoGradeScene::GoldenHour:
            grade.shadowsHue = 210.0;
            grade.shadowsSat = 0.090;
            grade.midtonesHue = 32.0;
            grade.midtonesSat = 0.055;
            grade.highlightsHue = 45.0;
            grade.highlightsSat = 0.110;
            grade.highlightsLum = -1.5;
            grade.blending = 68.0;
            grade.balance = 5.0;
            break;
        case AutoGradeScene::Landscape:
            grade.shadowsHue = 216.0;
            grade.shadowsSat = 0.080;
            grade.midtonesHue = 150.0;
            grade.midtonesSat = 0.025;
            grade.highlightsHue = 43.0;
            grade.highlightsSat = 0.075;
            grade.blending = 70.0;
            grade.balance = -4.0;
            break;
        case AutoGradeScene::Night:
            grade.shadowsHue = 220.0;
            grade.shadowsSat = 0.120;
            grade.shadowsLum = -1.0;
            grade.midtonesHue = 198.0;
            grade.midtonesSat = 0.040;
            grade.midtonesLum = 0.5;
            grade.highlightsHue = 35.0;
            grade.highlightsSat = 0.080;
            grade.highlightsLum = -1.0;
            grade.blending = 64.0;
            grade.balance = -12.0;
            break;
        case AutoGradeScene::Urban:
            grade.shadowsHue = 212.0;
            grade.shadowsSat = 0.090;
            grade.midtonesHue = 30.0;
            grade.midtonesSat = 0.020;
            grade.highlightsHue = 38.0;
            grade.highlightsSat = 0.065;
            grade.blending = 66.0;
            grade.balance = -3.0;
            break;
        case AutoGradeScene::Neutral:
            break;
    }

    // A grade should be visibly distinct from the technical correction while
    // preserving skin and already-saturated colors. The RGB curves create a
    // restrained cool-shadow/warm-highlight separation; Vibrance supplies
    // scene-aware chroma without turning the grade into a film simulation.
    auto& vibrance = params.vibrance;
    vibrance = rtengine::procparams::VibranceParams();
    vibrance.enabled = true;
    vibrance.pastels = 14;
    vibrance.saturated = 2;
    vibrance.protectskins = true;
    vibrance.avoidcolorshift = true;
    vibrance.pastsattog = true;

    auto& curves = params.rgbCurves;
    curves.rcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.168,
        0.50, 0.508,
        0.82, 0.842,
        1.0, 1.0
    };
    curves.gcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.182,
        0.50, 0.50,
        0.82, 0.818,
        1.0, 1.0
    };
    curves.bcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.198,
        0.50, 0.498,
        0.82, 0.798,
        1.0, 1.0
    };

    int gradeContrast = 3;
    switch (features.scene) {
        case AutoGradeScene::Portrait:
            vibrance.pastels = 10;
            vibrance.saturated = -2;
            gradeContrast = 1;
            break;
        case AutoGradeScene::GoldenHour:
            vibrance.pastels = 12;
            vibrance.saturated = 0;
            gradeContrast = 2;
            break;
        case AutoGradeScene::Landscape:
            vibrance.pastels = 18;
            vibrance.saturated = 3;
            gradeContrast = 4;
            break;
        case AutoGradeScene::Night:
            vibrance.pastels = 9;
            vibrance.saturated = -3;
            gradeContrast = 3;
            break;
        case AutoGradeScene::Urban:
            vibrance.pastels = 11;
            vibrance.saturated = 0;
            gradeContrast = 5;
            break;
        case AutoGradeScene::Neutral:
            break;
    }

    params.toneCurve.contrast = std::min(45, params.toneCurve.contrast + gradeContrast);
    params.sh.highlights = std::min(45, params.sh.highlights + 3);
    if (features.valid && features.shadowFraction > 0.34 && features.highlightFraction < 0.16) {
        params.sh.shadows = std::min(30, params.sh.shadows + 1);
    }

    params.filmPresets.enabled = false;
}

void applySteepAutoFilm(
    const AutoGradeFeatures& features,
    rtengine::procparams::ProcParams& params)
{
    auto& film = params.filmPresets;
    film = rtengine::procparams::FilmPresetsParams();
    film.enabled = true;
    film.modelVersion = 2;
    film.preset = "sovereign";
    film.process = "c41";
    film.output = "ra4";
    film.format = "35mm";
    film.strength = 36;
    // Place the digital negative near the stock's intended middle gray. The
    // film shoulder already protects bright values; routinely underexposing
    // the film stage only makes the print muddy and buries useful midtones.
    film.exposure = features.highlightFraction > 0.24 && features.medianLuma >= 0.30
        ? -0.05
        : features.highlightFraction > 0.18 && features.medianLuma >= 0.30
          ? -0.02
          : features.medianLuma < 0.24
            ? 0.07
            : 0.0;
    film.fade = -8;
    film.rolloff = features.highlightFraction > 0.22 ? 4 : 0;
    film.saturation = features.saturation > 0.48 ? -5 : features.saturation < 0.20 ? 2 : 0;
    film.contrast = features.dynamicRange < 0.38 ? 8 : features.dynamicRange > 0.72 ? 4 : 6;
    film.shadowHue = 218;
    film.highlightHue = 40;
    film.shadowTint = 2;
    film.highlightTint = 3;
    const double isoStops = std::max(0.0, std::log2(std::max(features.iso, 100u) / 100.0));
    film.grain = std::max(
        7,
        std::min(26, static_cast<int>(std::round(7.0 + isoStops * 3.5))));
    film.halation = std::max(
        5,
        std::min(14, static_cast<int>(std::round(5.0 + features.highlightFraction * 40.0))));
    film.skinProtection = 68;
    film.layerCoupling = 16;
    film.grainSize = 4;
    film.grainClumping = 8;
    film.grainColor = -8;
    film.halationSize = 8;
    film.halationThreshold = 12;
    film.halationColor = 10;
    film.bloom = 1;
    film.outputSoftness = 2;

    switch (features.scene) {
        case AutoGradeScene::Portrait:
            film.preset = "porcelain_400";
            film.process = "c41";
            film.output = "ra4";
            film.strength = 35;
            film.contrast = 7;
            film.fade = -6;
            film.rolloff = 2;
            film.saturation -= 2;
            film.grain = std::min(film.grain, 17);
            film.halation = std::min(film.halation, 11);
            film.skinProtection = 90;
            film.outputSoftness = 3;
            break;
        case AutoGradeScene::GoldenHour:
            film.preset = "golden_hour";
            film.process = "c41";
            film.output = "ra4";
            film.strength = 39;
            film.contrast = 5;
            film.warmth = -1;
            film.halation = std::min(14, film.halation + 2);
            film.rolloff = std::max(film.rolloff, 2);
            film.skinProtection = 78;
            break;
        case AutoGradeScene::Landscape:
            film.preset = features.saturation > 0.35 && features.highlightFraction < 0.13
                ? "vivid_chrome"
                : "sovereign";
            if (film.preset == "vivid_chrome") {
                film.process = "e6";
                film.output = "projection";
                film.strength = 30;
                film.contrast = -8;
                film.fade = -3;
                film.rolloff = -4;
                film.saturation -= 3;
            } else {
                film.process = "c41";
                film.output = "ra4";
                film.strength = 36;
                film.contrast = 6;
            }
            film.grain = std::min(film.grain, 16);
            film.halation = std::min(film.halation, 10);
            film.vibrance = features.saturation < 0.24 ? 3 : 0;
            break;
        case AutoGradeScene::Night:
            film.preset = "cinematic_500t";
            film.process = "ecn2";
            film.output = "cinema";
            film.strength = 39;
            film.exposure = 0.08;
            film.contrast = 10;
            film.fade = 6;
            film.rolloff = 4;
            film.warmth = -2;
            film.grain = std::min(32, film.grain + 6);
            film.halation = std::min(18, film.halation + 4);
            film.skinProtection = 68;
            break;
        case AutoGradeScene::Urban:
            film.preset = "street_800";
            film.process = "c41";
            film.output = "ra4";
            film.strength = 37;
            film.contrast = 2;
            film.fade = -10;
            film.rolloff = 0;
            film.grain = std::min(30, film.grain + 4);
            film.halation = std::min(film.halation, 11);
            break;
        case AutoGradeScene::Neutral:
            break;
    }

    const bool darkPrint = features.valid && features.medianLuma < 0.30;
    if (darkPrint) {
        // A dark negative needs a longer print exposure and a softer toe, not
        // another contrast pass. Preserve local color and texture while
        // keeping the scene recognizably low key.
        film.exposure = std::max(film.exposure, 0.10);
        film.fade = std::max(film.fade, 5);
        film.rolloff = std::max(film.rolloff, 4);
        const int darkFilmContrast = features.scene == AutoGradeScene::Night
            ? (features.dynamicRange < 0.18 ? 8 : 10)
            : (features.dynamicRange < 0.18 ? 3 : 5);
        film.contrast = std::min(film.contrast, darkFilmContrast);
    }

    // Keep the technical exposure, stock response, and print curve in their
    // own lanes. Protect highlight-rich frames with headroom, while allowing
    // enough negative density for faces and middle values to stay luminous.
    double exposureCeiling = 0.38;
    double exposureFloor = -1.5;
    if (features.medianLuma < 0.14) {
        exposureCeiling = features.scene == AutoGradeScene::Night ? 1.35 : 1.65;
        exposureFloor = features.scene == AutoGradeScene::Night ? 0.24 : 0.36;
    } else if (features.medianLuma < 0.23) {
        exposureCeiling = features.scene == AutoGradeScene::Night ? 1.15 : 1.45;
        exposureFloor = features.scene == AutoGradeScene::Night ? 0.16 : 0.28;
    } else if (features.medianLuma < 0.30) {
        exposureCeiling = features.scene == AutoGradeScene::Night ? 0.95 : 1.20;
        exposureFloor = features.scene == AutoGradeScene::Night ? 0.08 : 0.16;
    } else if (features.highlightFraction > 0.22 || features.medianLuma > 0.62) {
        exposureCeiling = 0.12;
    } else if (features.highlightFraction > 0.14 || features.medianLuma > 0.55) {
        exposureCeiling = 0.22;
    }
    params.toneCurve.expcomp = std::max(
        exposureFloor,
        std::min(params.toneCurve.expcomp, exposureCeiling));
    params.toneCurve.brightness = std::min(params.toneCurve.brightness, 3);
    params.sh.shadows = std::min(params.sh.shadows, features.scene == AutoGradeScene::Night ? 2 : 1);
    if (darkPrint) {
        const int inputContrastCeiling = features.dynamicRange < 0.18
            ? 10
            : features.dynamicRange < 0.35 ? 12 : 14;
        params.toneCurve.contrast = std::min(params.toneCurve.contrast, inputContrastCeiling);
    }

    // Paper-grade compensation. Printing a flat negative through a soft film
    // recipe washes the image out — the darkroom answer is a harder paper
    // grade, not the same soft one. Estimate the wash-out risk from the
    // source's measured flatness plus how soft this recipe ended up (lifted
    // fade, low print contrast), then harden the print stage in proportion:
    // firmer film contrast, less black-lift, and a print curve with a real
    // toe and a steeper midsection.
    double washRisk = 0.0;
    if (features.valid) {
        const double sourceFlatness = std::max(0.0, (0.55 - features.dynamicRange) / 0.55);
        const double fadeLift = std::max(0, film.fade) / 10.0;
        const double softContrast = std::max(0, 8 - film.contrast) / 16.0;
        washRisk = std::min(1.0, 0.65 * sourceFlatness + 0.20 * fadeLift + 0.15 * softContrast);

        if (washRisk > 0.25) {
            film.contrast = std::min(12, film.contrast + static_cast<int>(std::round(6.0 * washRisk)));

            // Don't lift blacks the source never had. Scaled by flatness, so
            // deliberate lifted looks (Night's cinematic fade) survive on
            // sources with genuine contrast.
            if (film.fade > 0 && sourceFlatness > 0.30) {
                film.fade = std::max(-2, film.fade - static_cast<int>(std::round(8.0 * sourceFlatness)));
            }

            // A washed print also reads desaturated; give the dyes a nudge.
            if (washRisk > 0.5) {
                film.saturation = std::min(6, film.saturation + 2);
            }
        }
    }

    const double toeOut = (darkPrint ? 0.084 : 0.073) * (1.0 - 0.45 * washRisk);
    const double lowerOut = (darkPrint ? 0.232 : 0.215) * (1.0 - 0.07 * washRisk);
    const double upperOut = std::min(0.70, (darkPrint ? 0.642 : 0.625) * (1.0 + 0.07 * washRisk));
    const double shoulderOut = std::min(0.90, (darkPrint ? 0.865 : 0.855) * (1.0 + 0.035 * washRisk));
    params.rgbCurves.mastercurve = {
        DCT_Spline,
        0.0, 0.0,
        0.08, toeOut,
        0.20, lowerOut,
        0.52, upperOut,
        0.80, shoulderOut,
        1.0, 0.995
    };
}

void prepareAutoLevelImageSource(
    rtengine::ImageSource* source,
    const rtengine::procparams::ProcParams& params)
{
    if (!source || !source->isRAW()) {
        return;
    }

    auto raw = params.raw;
    raw.bayersensor.method = rtengine::procparams::RAWParams::BayerSensor::getMethodString(
        rtengine::procparams::RAWParams::BayerSensor::Method::FAST);
    raw.xtranssensor.method = rtengine::procparams::RAWParams::XTransSensor::getMethodString(
        rtengine::procparams::RAWParams::XTransSensor::Method::FAST);

    source->setCurrentFrame(raw.bayersensor.imageNum);
    float redDehaze = 0.f;
    float greenDehaze = 0.f;
    float blueDehaze = 0.f;
    source->preprocess(
        raw,
        params.lensProf,
        params.coarse,
        redDehaze,
        greenDehaze,
        blueDehaze,
        false);
    double contrastThreshold = 0.0;
    source->demosaic(raw, false, contrastThreshold);
}

struct AutoEditBatchState {
    std::vector<Thumbnail*> thumbnails;
    AutoEditMode mode = AutoEditMode::Neutral;
    std::atomic<bool> analysisFinished{false};
    size_t applied = 0;
    bool finalized = false;

    ~AutoEditBatchState()
    {
        for (auto* thumbnail : thumbnails) {
            thumbnail->decreaseRef();
        }
    }
};

struct AutoLevelBatchItem {
    Thumbnail* thumbnail = nullptr;
    Glib::ustring filename;
    bool isRaw = false;
    rtengine::procparams::ProcParams params;
};

struct AutoLevelBatchState {
    std::vector<AutoLevelBatchItem> items;
    std::vector<std::pair<Thumbnail*, rtengine::procparams::ProcParams>> readyResults;
    std::mutex resultsMutex;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::atomic<bool> analysisFinished{false};
    std::atomic<size_t> analyzed{0};
    std::atomic<size_t> applied{0};
    std::atomic<size_t> alreadyLevel{0};
    std::atomic<size_t> loadFailures{0};
    std::atomic<size_t> detectionFailures{0};
    std::atomic<double> lastAppliedAngle{0.0};
    size_t prepareIndex = 0;

    ~AutoLevelBatchState()
    {
        for (auto& item : items) {
            item.thumbnail->decreaseRef();
        }
    }
};

bool fileBrowserPerfLogEnabled()
{
    static const bool enabled = std::getenv("STEEP_FILESEL_LOG") != nullptr;
    return enabled;
}

long long quickWarmDurationMs(
    const QuickWarmClock::time_point& start,
    const QuickWarmClock::time_point& end)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

void fileBrowserPerfLog(const char* fmt, ...)
{
    if (!fileBrowserPerfLogEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(fileBrowserPerfLogMutex);
    const char* const home = std::getenv("USERPROFILE");
    const std::string path = home ? std::string(home) + "\\steep-fileSel.log" : "steep-fileSel.log";

    FILE* const f = std::fopen(path.c_str(), "ab");
    if (!f) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fclose(f);
}

void lowerQuickPreviewWarmThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

struct QuickPreviewCacheWarmItem {
    Thumbnail* thumbnail;
};

class QuickPreviewCacheWarmTask final
{
public:
    QuickPreviewCacheWarmTask(
        std::vector<QuickPreviewCacheWarmItem>&& items,
        unsigned generation,
        int height)
        : items_(std::move(items)),
          generation_(generation),
          height_(height)
    {
    }

    ~QuickPreviewCacheWarmTask()
    {
        for (auto& item : items_) {
            item.thumbnail->decreaseRef();
        }
    }

    void run()
    {
        const auto start = QuickWarmClock::now();
        size_t loaded = 0;
        size_t missed = 0;
        size_t remaining = 0;

        for (size_t i = 0; i < items_.size(); ++i) {
            if (quickPreviewCacheWarmGeneration.load(std::memory_order_acquire) != generation_) {
                remaining = items_.size() - i;
                break;
            }

            double scale = 1.0;
            auto pixbuf = items_[i].thumbnail->tryLoadCachedPreviewPixbuf(height_, scale);
            if (pixbuf) {
                ++loaded;
            } else {
                ++missed;
            }
        }

        fileBrowserPerfLog(
            "[quickWarm] %s duration=%lldms loaded=%zu missed=%zu remaining=%zu total=%zu\n",
            remaining > 0 ? "canceled" : "done",
            quickWarmDurationMs(start, QuickWarmClock::now()),
            loaded,
            missed,
            remaining,
            items_.size());
    }

private:
    std::vector<QuickPreviewCacheWarmItem> items_;
    unsigned generation_;
    int height_;
};

class QuickPreviewCacheWarmExecutor final
{
public:
    void enqueue(std::unique_ptr<QuickPreviewCacheWarmTask> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A navigation burst only needs the newest neighborhood. Replacing
            // pending work also releases its thumbnail references immediately.
            pending_ = std::move(task);
            if (!workerStarted_) {
                workerStarted_ = true;
                std::thread([this]() { run(); }).detach();
            }
        }
        cv_.notify_one();
    }

    void cancelPending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }

private:
    void run()
    {
        lowerQuickPreviewWarmThreadPriority();
#ifdef _OPENMP
        // Cached-preview warming is latency-insensitive and persistent. A
        // single OpenMP lane avoids retaining a fresh team per navigation.
        omp_set_num_threads(1);
#endif
        for (;;) {
            std::unique_ptr<QuickPreviewCacheWarmTask> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return pending_ != nullptr; });
                task = std::move(pending_);
            }
            task->run();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<QuickPreviewCacheWarmTask> pending_;
    bool workerStarted_ = false;
};

QuickPreviewCacheWarmExecutor& quickPreviewCacheWarmExecutor()
{
    static auto* executor = new QuickPreviewCacheWarmExecutor();
    return *executor;
}

void scheduleCachedQuickPreviewWarm(std::vector<QuickPreviewCacheWarmItem>&& items, int previewHeight)
{
    if (items.empty()) {
        return;
    }

    const unsigned generation = quickPreviewCacheWarmGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int height = std::max(1, previewHeight);
    quickPreviewCacheWarmExecutor().enqueue(
        std::make_unique<QuickPreviewCacheWarmTask>(std::move(items), generation, height));
}

void cancelCachedQuickPreviewWarmJobs()
{
    quickPreviewCacheWarmGeneration.fetch_add(1, std::memory_order_acq_rel);
    quickPreviewCacheWarmExecutor().cancelPending();
}

std::string foldedBrowserPathKey(const Glib::ustring& path)
{
    std::string key = path.casefold().raw();
    std::replace(key.begin(), key.end(), '\\', '/');

    return key;
}

std::string browserPathKey(const Glib::ustring& path)
{
    if (Glib::path_is_absolute(path)) {
        return foldedBrowserPathKey(path);
    }

    const std::string rawPath = path.raw();
    Glib::ustring normalized = path;

    try {
        Glib::RefPtr<Gio::File> file;

        if (rawPath.rfind("file:", 0) == 0) {
            file = Gio::File::create_for_uri(path);
        } else {
            file = Gio::File::create_for_path(path);
        }

        const Glib::ustring nativePath = file->get_path();
        if (!nativePath.empty()) {
            normalized = nativePath;
        } else {
            const Glib::ustring parseName = file->get_parse_name();
            if (!parseName.empty()) {
                normalized = parseName;
            }
        }
    } catch (const Glib::Exception&) {
    }

    return foldedBrowserPathKey(normalized);
}

Glib::ustring getLowercaseExtension (const Glib::ustring& filename)
{
    const Glib::ustring basename = Glib::path_get_basename(filename.lowercase());

    const Glib::ustring::size_type pos = basename.find_last_of ('.');
    if (pos >= basename.length () - 1) {
        return {};
    }

    return basename.substr(pos + 1);
}

bool isRawOriginalExtension(const Glib::ustring& extension)
{
    static const std::set<Glib::ustring> rawExtensions = {
        "3fr", "arw", "arq", "cr2", "cr3", "crf", "crw", "dcr", "dng",
        "fff", "iiq", "kdc", "mef", "mos", "mrw", "nef", "nrw", "orf",
        "ori", "pef", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf",
        "srw", "x3f"
    };

    return rawExtensions.find(extension) != rawExtensions.end();
}

bool isLikely8BitJpegExtension(const Glib::ustring& extension)
{
    return extension == "jpg" || extension == "jpeg" || extension == "jpe";
}

size_t nonRawBytesPerPixel(rtengine::IIOSampleFormat sampleFormat)
{
    if (sampleFormat & rtengine::IIOSF_UNSIGNED_CHAR) {
        return 4;
    }
    if (sampleFormat & rtengine::IIOSF_UNSIGNED_SHORT) {
        return 8;
    }

    return 16;
}

size_t estimateInitialImageBytes(
    int width,
    int height,
    bool isRaw,
    rtengine::eSensorType sensorType,
    rtengine::IIOSampleFormat sampleFormat,
    unsigned int frameCount)
{
    if (width <= 0 || height <= 0) {
        return 0;
    }

    const size_t bytesPerPixel = isRaw
        ? ((sensorType == rtengine::ST_BAYER || sensorType == rtengine::ST_FUJI_XTRANS) ? 6 : 12)
        : nonRawBytesPerPixel(sampleFormat);

    const size_t frames = isRaw ? std::max(1u, frameCount) : 1u;
    return static_cast<size_t>(width) * static_cast<size_t>(height) * bytesPerPixel * frames;
}

int getExtensionRank(const Glib::ustring& extension)
{
    const auto& originalExtensions = App::get().options().parsedExtensions;

    for (size_t i = 0; i < originalExtensions.size(); ++i) {
        if (originalExtensions[i] == extension) {
            return static_cast<int>(i);
        }
    }

    return std::numeric_limits<int>::max() / 4;
}

int getOriginalPriority(const ThumbBrowserEntryBase* entry)
{
    const Glib::ustring extension = getLowercaseExtension(entry->filename);

    if (extension.empty()) {
        return std::numeric_limits<int>::max() / 2;
    }

    // Prefer true RAW files over companion rendered files with the same stem.
    // In particular, RAF must win over an in-camera JPEG sibling even if the
    // user's extension list happens to place jpg before raf.
    const int family = isRawOriginalExtension(extension) ? 0 : 1;

    return family * 10000 + getExtensionRank(extension);
}

std::string originalFamilyKey(const Glib::ustring& filename)
{
    const auto basename = Glib::path_get_basename(filename.lowercase());

    const auto pos = basename.find_last_of('.');
    if (pos >= basename.length() - 1) {
        return browserPathKey(filename);
    }

    const auto withoutExtension = basename.substr(0, pos);
    const auto dirname = Glib::path_get_dirname(filename);

    return browserPathKey(Glib::build_filename(dirname, withoutExtension));
}

ThumbBrowserEntryBase* selectOriginalEntry (ThumbBrowserEntryBase* original, ThumbBrowserEntryBase* candidate)
{
    if (original == nullptr) {
        return candidate;
    }

    if (getOriginalPriority(candidate) < getOriginalPriority(original)) {
        return candidate;
    }

    return original;
}

std::string browserPathKeyForEntry(const ThumbBrowserEntryBase* entry)
{
    const auto* fileEntry = static_cast<const FileBrowserEntry*>(entry);
    const std::string& entryKey = fileEntry->getBrowserPathKey();

    return entryKey.empty() ? browserPathKey(entry->filename) : entryKey;
}

bool browserPathKeyMatchesEntry(const ThumbBrowserEntryBase* entry, const std::string& key)
{
    const auto* fileEntry = static_cast<const FileBrowserEntry*>(entry);
    const std::string& entryKey = fileEntry->getBrowserPathKey();

    return entryKey.empty() ? browserPathKey(entry->filename) == key : entryKey == key;
}

const std::string& originalFamilyKeyForEntry(FileBrowserEntry* entry)
{
    const std::string& cachedKey = entry->getBrowserOriginalFamilyKey();
    if (!cachedKey.empty()) {
        return cachedKey;
    }

    entry->setBrowserOriginalFamilyKey(originalFamilyKey(entry->filename));
    return entry->getBrowserOriginalFamilyKey();
}

// When the color grade and the film stage stack, keep each in its lane: the
// grade owns color direction (wheels, split-tone channel curves, vibrance),
// the film stock owns tonal response and texture (curve, grain, halation).
// Left untamed, both tone shadows cool and highlights warm, doubling the
// cast into mud — so the film's own split tinting steps back, the grade
// blends a little lighter, and chroma is not added twice.
void harmonizeSteepGradeWithFilm(rtengine::procparams::ProcParams& params)
{
    auto& film = params.filmPresets;
    film.shadowTint = (film.shadowTint + 1) / 2;
    film.highlightTint = (film.highlightTint + 1) / 2;
    film.saturation = std::max(-100, film.saturation - 2);

    auto& grade = params.colorGrading;
    grade.blending = std::max(50.0, grade.blending - 8.0);

    auto& vibrance = params.vibrance;
    vibrance.pastels = std::max(8, vibrance.pastels - 3);
}

AutoGradeFeatures buildSteepAutoEditParamsInternal(
    Thumbnail& thumbnail,
    AutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result)
{
    const AutoGradeFeatures features = analyzeSteepAutoGrade(thumbnail);
    applySteepAutoEdit(thumbnail, features, result);
    restoreSteepAutoEditGeometry(source, result);

    if (mode == AutoEditMode::Grade) {
        applySteepAutoGrade(features, result);
    } else if (mode == AutoEditMode::GradeFilm) {
        applySteepAutoFilm(features, result);
    } else if (mode == AutoEditMode::GradedFilm) {
        // Order matters: the grade disables filmPresets (it is a look of its
        // own), so the film stage runs after and re-enables its pipeline.
        // Film's exposure/contrast lanes and print curve legitimately win.
        applySteepAutoGrade(features, result);
        applySteepAutoFilm(features, result);
        harmonizeSteepGradeWithFilm(result);
    }

    return features;
}

}

void buildSteepAutoEditParams(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result)
{
    buildSteepAutoEditParamsInternal(thumbnail, mode, source, result);
}

FileBrowser::FileBrowser () :
    editExternal(nullptr),
    menuRank(nullptr),
    menuLabel(nullptr),
    miOpenDefaultViewer(nullptr),
    selectDF(nullptr),
    thisIsDF(nullptr),
    autoDF(nullptr),
    selectFF(nullptr),
    thisIsFF(nullptr),
    autoFF(nullptr),
    clearFromCache(nullptr),
    clearFromCacheFull(nullptr),
    colorLabel_actionData(nullptr),
    bppcl(nullptr),
    tbl(nullptr),
    filterPassThrough_(true),
    numFiltered(0),
    exportPanel(nullptr),
    autoEditHoverPool_(new Glib::ThreadPool(1, true)),
    autoEditHoverGeneration_(std::make_shared<std::atomic<unsigned>>(0))
{
    session_id_ = 0;
    selectionNotifyIdlePending_ = false;

    ProfileStore::getInstance()->addListener(this);
    const auto& options = App::get().options();

    int p = 0;
    pmenu = new Gtk::Menu ();

    /***********************
     * Inline quick actions at the very top: one-click flag / rank /
     * color-label for the whole selection, no submenu digging. The
     * buttons route through menuItemActivated() using the hidden
     * identity items created further down.
     ***********************/
    {
        // GtkMenu grabs all input while open, so real buttons inside menu
        // items never receive clicks. The icons are plain images; presses
        // are hit-tested at the menu level against each icon's allocation
        // (same pattern as the Auto Edit submenu hit test below).
        auto inlineZones = std::make_shared<std::vector<std::pair<Gtk::Widget*, std::function<void()>>>>();

        auto makeInlineRow = [](const Glib::ustring& caption) {
            auto* item = Gtk::manage(new Gtk::MenuItem());
            item->set_name("InlineActionRow");
            auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
            row->set_margin_top(3);
            row->set_margin_bottom(3);
            auto* label = Gtk::manage(new Gtk::Label(caption));
            label->set_width_chars(7);
            label->set_xalign(0.0);
            label->get_style_context()->add_class("dim-label");
            row->pack_start(*label, Gtk::PACK_SHRINK);
            item->add(*row);
            return std::make_pair(item, row);
        };

        auto addInlineIcon = [this, inlineZones](Gtk::Box* row, const char* icon,
                                                 const Glib::ustring& tooltip,
                                                 std::function<Gtk::MenuItem*()> target) {
            auto* img = Gtk::manage(new RTImage(icon, Gtk::ICON_SIZE_LARGE_TOOLBAR));
            img->set_margin_start(6);
            img->set_margin_end(6);
            img->set_margin_top(4);
            img->set_margin_bottom(4);
            img->set_tooltip_text(tooltip);
            // Dimmed at rest; the hover tracker below raises the icon under
            // the pointer to full opacity so the target is unmistakable.
            img->set_opacity(0.65);
            row->pack_start(*img, Gtk::PACK_SHRINK);
            inlineZones->emplace_back(img, [this, target]() {
                menuItemActivated(target());
            });
        };

        // Flags: pick / unflag / reject
        auto flagRow = makeInlineRow(M("FILEBROWSER_POPUPFLAG"));
        addInlineIcon(flagRow.second, "menu-flag-pick", M("FILEBROWSER_POPUPPICK"),
                      [this]() -> Gtk::MenuItem* { return pickFlag; });
        addInlineIcon(flagRow.second, "menu-flag-unflagged", M("FILEBROWSER_POPUPUNFLAG"),
                      [this]() -> Gtk::MenuItem* { return unflagFlag; });
        addInlineIcon(flagRow.second, "menu-flag-reject", M("FILEBROWSER_POPUPREJECT"),
                      [this]() -> Gtk::MenuItem* { return rejectFlag; });
        pmenu->attach(*flagRow.first, 0, 1, p, p + 1);
        p++;

        // Rank: none + 1..5 stars
        auto rankRow = makeInlineRow(M("FILEBROWSER_POPUPRANK"));
        addInlineIcon(rankRow.second, "menu-star-empty", M("FILEBROWSER_POPUPUNRANK"),
                      [this]() -> Gtk::MenuItem* { return rank[0]; });
        for (int i = 1; i <= 5; i++) {
            addInlineIcon(rankRow.second,
                          Glib::ustring::compose("menu-star-%1", i).c_str(),
                          M(Glib::ustring::compose("FILEBROWSER_POPUPRANK%1", i)),
                          [this, i]() -> Gtk::MenuItem* { return rank[i]; });
        }
        pmenu->attach(*rankRow.first, 0, 1, p, p + 1);
        p++;

        // Color labels: none + 5 colors
        static const std::array<const char*, 6> inlineClabelIcons = {
            "circle-empty-gray-small", "circle-red-small", "circle-yellow-small",
            "circle-green-small", "circle-blue-small", "circle-purple-small"
        };
        auto colorRow = makeInlineRow(M("FILEBROWSER_POPUPCOLORLABEL"));
        for (int i = 0; i <= 5; i++) {
            addInlineIcon(colorRow.second, inlineClabelIcons[i],
                          M(Glib::ustring::compose("FILEBROWSER_POPUPCOLORLABEL%1", i)),
                          [this, i]() -> Gtk::MenuItem* { return colorlabel[i]; });
        }
        pmenu->attach(*colorRow.first, 0, 1, p, p + 1);
        p++;

        pmenu->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, p, p + 1);
        p++;

        // Padded horizontally by half the inter-icon gap so a row has no
        // dead zones: any click along the row lands on the nearest icon.
        auto hitZoneAt = [inlineZones](double xRoot, double yRoot) -> Gtk::Widget* {
            for (auto& zone : *inlineZones) {
                Gtk::Widget* widget = zone.first;
                if (!widget->get_visible()) {
                    continue;
                }
                auto window = widget->get_window();
                if (!window) {
                    continue;
                }

                int windowX = 0;
                int windowY = 0;
                window->get_origin(windowX, windowY);
                const auto allocation = widget->get_allocation();
                const double ix = xRoot - windowX - allocation.get_x();
                const double iy = yRoot - windowY - allocation.get_y();

                if (ix >= -8.0 && iy >= -6.0
                        && ix < allocation.get_width() + 8.0
                        && iy < allocation.get_height() + 6.0) {
                    return widget;
                }
            }
            return nullptr;
        };

        pmenu->signal_button_press_event().connect(
            [this, inlineZones, hitZoneAt](GdkEventButton* event) -> bool {
                if (!event || event->button != 1) {
                    return false;
                }

                Gtk::Widget* hit = hitZoneAt(event->x_root, event->y_root);
                if (!hit) {
                    return false;
                }

                for (auto& zone : *inlineZones) {
                    if (zone.first == hit) {
                        const auto action = zone.second;
                        pmenu->popdown();
                        action();
                        return true;
                    }
                }
                return false;
            },
            false);

        // Hover feedback: the icon under the pointer goes full-opacity
        auto hoverState = std::make_shared<Gtk::Widget*>(nullptr);
        auto setHover = [inlineZones, hoverState](Gtk::Widget* hovered) {
            if (*hoverState == hovered) {
                return;
            }
            if (*hoverState) {
                (*hoverState)->set_opacity(0.65);
            }
            *hoverState = hovered;
            if (hovered) {
                hovered->set_opacity(1.0);
            }
        };

        pmenu->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
        pmenu->signal_motion_notify_event().connect(
            [hitZoneAt, setHover](GdkEventMotion* event) -> bool {
                setHover(event ? hitZoneAt(event->x_root, event->y_root) : nullptr);
                return false;
            },
            false);
        pmenu->signal_leave_notify_event().connect(
            [setHover](GdkEventCrossing*) -> bool {
                setHover(nullptr);
                return false;
            },
            false);
        pmenu->signal_hide().connect([setHover]() {
            setHover(nullptr);
        });
    }

    pmenu->attach (*Gtk::manage(open = new MyImageMenuItem (M("FILEBROWSER_POPUPOPEN"), "menu-open")), 0, 1, p, p + 1);
    p++;
    if (options.inspectorWindow) {
        pmenu->attach (*Gtk::manage(inspect = new MyImageMenuItem (M("FILEBROWSER_POPUPINSPECT"), "menu-inspect")), 0, 1, p, p + 1);
        p++;
    }
    pmenu->attach (*Gtk::manage(develop = new MyImageMenuItem (M("FILEBROWSER_POPUPPROCESS"), "gears")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(developfast = new MyImageMenuItem (M("FILEBROWSER_POPUPPROCESSFAST"), "menu-develop-fast")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(saveImage = new MyImageMenuItem (M("FILEBROWSER_POPUPSAVEIMAGE"), "menu-save")), 0, 1, p, p + 1);
    p++;

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;

    /***********************
     * AI & Quick Actions
     ***********************/
    pmenu->attach (*Gtk::manage(aiDenoise = new MyImageMenuItem (M("FILEBROWSER_POPUPAIDENOISE"), "ai-denoise")), 0, 1, p, p + 1);
    p++;
    {
        int position = 0;
        Gtk::Menu* submenu = Gtk::manage(new Gtk::Menu());
        submenu->attach(
            *Gtk::manage(autoGrade = new MyImageMenuItem(M("FILEBROWSER_POPUPAUTOGRADE"), "color-circles")),
            0, 1, position, position + 1);
        ++position;
        submenu->attach(
            *Gtk::manage(autoGradeFilm = new MyImageMenuItem(M("FILEBROWSER_POPUPFILMLAB"), "filmstrip-show")),
            0, 1, position, position + 1);
        ++position;
        submenu->attach(
            *Gtk::manage(autoGradedFilm = new MyImageMenuItem(M("FILEBROWSER_POPUPAUTOGRADEFILM"), "auto-grade-film")),
            0, 1, position, position + 1);
        submenu->show_all();

        autoEditMenu = Gtk::manage(new MyImageMenuItem(M("FILEBROWSER_POPUPAUTOEDIT"), "palette-brush"));
        autoEditMenu->set_submenu(*submenu);
        auto autoEditHitTest = [this](double rootX, double rootY, int rightInset) {
            auto window = autoEditMenu->get_window();
            if (!window) {
                return false;
            }

            int windowX = 0;
            int windowY = 0;
            window->get_origin(windowX, windowY);
            const auto allocation = autoEditMenu->get_allocation();
            const double itemX = rootX - windowX - allocation.get_x();
            const double itemY = rootY - windowY - allocation.get_y();
            return itemX >= 0.0
                && itemY >= 0.0
                && itemX < allocation.get_width() - rightInset
                && itemY < allocation.get_height();
        };
        pmenu->signal_button_press_event().connect(
            [this, autoEditHitTest](GdkEventButton* event) {
                constexpr int SUBMENU_HIT_WIDTH = 34;
                if (event->button != 1) {
                    return false;
                }

                if (autoEditHitTest(event->x_root, event->y_root, SUBMENU_HIT_WIDTH)) {
                    menuItemActivated(autoEditMenu);
                    pmenu->popdown();
                    return true;
                }
                return false;
            },
            false);
        pmenu->add_events(Gdk::POINTER_MOTION_MASK);
        pmenu->signal_motion_notify_event().connect(
            [this, autoEditHitTest](GdkEventMotion* event) {
                constexpr int SUBMENU_HIT_WIDTH = 34;
                if (autoEditHitTest(event->x_root, event->y_root, SUBMENU_HIT_WIDTH)) {
                    startAutoEditHoverPreview(autoEditMenu);
                }
                return false;
            },
            false);
        pmenu->attach(*autoEditMenu, 0, 1, p, p + 1);
    }
    p++;
    pmenu->attach (*Gtk::manage(autoLevel = new MyImageMenuItem (M("TP_ROTATE_AUTO_LEVEL"), "rotate-straighten-small")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(duplicate = new MyImageMenuItem (M("FILEBROWSER_POPUPDUPLICATE"), "menu-duplicate")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(addToAlbum = new MyImageMenuItem (M("EDITOR_ADD_TO_ALBUM_TOOLTIP"), "add-to-album")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(setAlbumCover = new MyImageMenuItem (M("FILEBROWSER_POPUPSETALBUMCOVER"), "menu-album-cover")), 0, 1, p, p + 1);
    p++;

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(selall = new MyImageMenuItem (M("FILEBROWSER_POPUPSELECTALL"), "menu-select-all")), 0, 1, p, p + 1);
    p++;

    /***********************
     * sort
     ***********************/
    const std::array<std::string, 2> cnameSortOrders = {
        M("SORT_ASCENDING"),
        M("SORT_DESCENDING"),
    };

    const std::array<std::string, Options::SORT_METHOD_COUNT> cnameSortMethods = {
        M("SORT_BY_NAME"),
        M("SORT_BY_DATE"),
        M("SORT_BY_EXIF"),
        M("SORT_BY_RANK"),
        M("SORT_BY_LABEL"),
        M("SORT_BY_FILETYPE"),
    };

    pmenu->attach (*Gtk::manage(menuSort = new MyImageMenuItem (M("FILEBROWSER_POPUPSORTBY"), "menu-sort")), 0, 1, p, p + 1);
    p++;
    Gtk::Menu* submenuSort = Gtk::manage (new Gtk::Menu ());
    Gtk::RadioButtonGroup sortOrderGroup, sortMethodGroup;
    for (size_t i = 0; i < cnameSortOrders.size(); i++) {
        submenuSort->attach (*Gtk::manage(sortOrder[i] = new Gtk::RadioMenuItem (sortOrderGroup, cnameSortOrders[i])), 0, 1, p, p + 1);
        p++;
        sortOrder[i]->set_active (i == options.sortDescending);
    }
    submenuSort->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;
    for (size_t i = 0; i < cnameSortMethods.size(); i++) {
        submenuSort->attach (*Gtk::manage(sortMethod[i] = new Gtk::RadioMenuItem (sortMethodGroup, cnameSortMethods[i])), 0, 1, p, p + 1);
        p++;
        sortMethod[i]->set_active (i == options.sortMethod);
    }
    submenuSort->show_all ();
    menuSort->set_submenu (*submenuSort);

    /***********************
     * rank / color labels / pick flags — identity items only.
     * The visible UI is the inline quick-action rows at the top of the
     * menu; these MyImageMenuItems remain as hidden targets so
     * menuItemActivated(), keyboard shortcuts, and the inline buttons
     * share one activation path. They live in a hidden submenu so the
     * menu keeps ownership of their lifetime.
     ***********************/
    std::array<std::string, 6> rankIcons = {"menu-star-empty", "menu-star-1", "menu-star-2", "menu-star-3", "menu-star-4", "menu-star-5"};
    std::array<std::string, 6> clabelActiveIcons = {"circle-empty-gray-small", "circle-red-small", "circle-yellow-small", "circle-green-small", "circle-blue-small", "circle-purple-small"};

    {
        auto* hiddenActions = Gtk::manage(new MyImageMenuItem(M("FILEBROWSER_POPUPRANK"), "menu-sort-rank"));
        Gtk::Menu* hiddenSub = Gtk::manage(new Gtk::Menu());
        int hp = 0;

        hiddenSub->attach(*Gtk::manage(rank[0] = new MyImageMenuItem(M("FILEBROWSER_POPUPUNRANK"), rankIcons[0])), 0, 1, hp, hp + 1);
        hp++;
        for (int i = 1; i <= 5; i++) {
            hiddenSub->attach(*Gtk::manage(rank[i] = new MyImageMenuItem(M(Glib::ustring::compose("%1%2", "FILEBROWSER_POPUPRANK", i)), rankIcons[i])), 0, 1, hp, hp + 1);
            hp++;
        }
        for (int i = 0; i <= 5; i++) {
            hiddenSub->attach(*Gtk::manage(colorlabel[i] = new MyImageMenuItem(M(Glib::ustring::compose("%1%2", "FILEBROWSER_POPUPCOLORLABEL", i)), clabelActiveIcons[i])), 0, 1, hp, hp + 1);
            hp++;
        }
        hiddenSub->attach(*Gtk::manage(pickFlag = new MyImageMenuItem(M("FILEBROWSER_POPUPPICK"), "menu-flag-pick")), 0, 1, hp, hp + 1);
        hp++;
        hiddenSub->attach(*Gtk::manage(unflagFlag = new MyImageMenuItem(M("FILEBROWSER_POPUPUNFLAG"), "menu-flag-unflagged")), 0, 1, hp, hp + 1);
        hp++;
        hiddenSub->attach(*Gtk::manage(rejectFlag = new MyImageMenuItem(M("FILEBROWSER_POPUPREJECT"), "menu-flag-reject")), 0, 1, hp, hp + 1);
        hp++;

        hiddenActions->set_submenu(*hiddenSub);
        pmenu->attach(*hiddenActions, 0, 1, p, p + 1);
        p++;
        hiddenActions->set_no_show_all(true);
        hiddenActions->hide();
    }

    /***********************
     * external programs
     * *********************/
    if (!App::get().isGimpPlugin()) {
        pmenu->attach(*Gtk::manage(
            editExternal = new MyImageMenuItem(M("MAIN_BUTTON_SENDTOEDITOR"), "external-editor")),
            0, 1, p, p + 1);
        p++;
    }

#if defined(_WIN32)
    Gtk::manage(miOpenDefaultViewer = new MyImageMenuItem (M("FILEBROWSER_OPENDEFAULTVIEWER"), "external-editor"));
#endif

    // Build a list of menu items
    mMenuExtProgs.clear();
    amiExtProg = nullptr;

    for (const auto& action : extProgStore->getActions ()) {
        if (action.target == 1 || action.target == 2) {
            mMenuExtProgs[action.getFullName ()] = &action;
        }
    }

    // Attach them to menu
    if (!mMenuExtProgs.empty() || miOpenDefaultViewer) {
        amiExtProg = new Gtk::MenuItem*[mMenuExtProgs.size()];
        int itemNo = 0;

        if (options.menuGroupExtProg) {
            pmenu->attach (*Gtk::manage(menuExtProg = new Gtk::MenuItem (M("FILEBROWSER_EXTPROGMENU"))), 0, 1, p, p + 1);
            p++;
            Gtk::Menu* submenuExtProg = Gtk::manage (new Gtk::Menu());

#ifdef _WIN32
            if (miOpenDefaultViewer) {
                submenuExtProg->attach (*miOpenDefaultViewer, 0, 1, p, p + 1);
                p++;
            }
#endif
            for (auto it = mMenuExtProgs.begin(); it != mMenuExtProgs.end(); it++, itemNo++) {
                submenuExtProg->attach (*Gtk::manage(amiExtProg[itemNo] = new Gtk::MenuItem ((*it).first)), 0, 1, p, p + 1);
                p++;
            }

            submenuExtProg->show_all ();
            menuExtProg->set_submenu (*submenuExtProg);
        } else {
#ifdef _WIN32
            if (miOpenDefaultViewer) {
                pmenu->attach (*miOpenDefaultViewer, 0, 1, p, p + 1);
                p++;
            }
#endif
            for (auto it = mMenuExtProgs.begin(); it != mMenuExtProgs.end(); it++, itemNo++) {
                pmenu->attach (*Gtk::manage(amiExtProg[itemNo] = new Gtk::MenuItem ((*it).first)), 0, 1, p, p + 1);
                p++;
            }
        }

        pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
    } else if (editExternal) {
        pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
    }

    /***********************
     * File Operations
     * *********************/
    if (options.menuGroupFileOperations) {
        pmenu->attach (*Gtk::manage(menuFileOperations = new MyImageMenuItem (M("FILEBROWSER_POPUPFILEOPERATIONS"), "menu-trash")), 0, 1, p, p + 1);
        p++;
        Gtk::Menu* submenuFileOperations = Gtk::manage (new Gtk::Menu ());

        submenuFileOperations->attach (*Gtk::manage(trash = new MyImageMenuItem (M("FILEBROWSER_POPUPTRASH"), "menu-trash")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(untrash = new MyImageMenuItem (M("FILEBROWSER_POPUPUNTRASH"), "menu-trash-restore")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(rename = new MyImageMenuItem (M("FILEBROWSER_POPUPRENAME"), "menu-rename")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(remove = new MyImageMenuItem (M("FILEBROWSER_POPUPREMOVE"), "menu-delete")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(removeInclProc = new MyImageMenuItem (M("FILEBROWSER_POPUPREMOVEINCLPROC"), "menu-delete-all")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(copyTo = new MyImageMenuItem (M("FILEBROWSER_POPUPCOPYTO"), "menu-copy-to")), 0, 1, p, p + 1);
        p++;
        submenuFileOperations->attach (*Gtk::manage(moveTo = new MyImageMenuItem (M("FILEBROWSER_POPUPMOVETO"), "menu-move-to")), 0, 1, p, p + 1);
        p++;

        submenuFileOperations->show_all ();
        menuFileOperations->set_submenu (*submenuFileOperations);
    } else {
        pmenu->attach (*Gtk::manage(trash = new MyImageMenuItem (M("FILEBROWSER_POPUPTRASH"), "menu-trash")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(untrash = new MyImageMenuItem (M("FILEBROWSER_POPUPUNTRASH"), "menu-trash-restore")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(rename = new MyImageMenuItem (M("FILEBROWSER_POPUPRENAME"), "menu-rename")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(remove = new MyImageMenuItem (M("FILEBROWSER_POPUPREMOVE"), "menu-delete")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(removeInclProc = new MyImageMenuItem (M("FILEBROWSER_POPUPREMOVEINCLPROC"), "menu-delete-all")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(copyTo = new MyImageMenuItem (M("FILEBROWSER_POPUPCOPYTO"), "menu-copy-to")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(moveTo = new MyImageMenuItem (M("FILEBROWSER_POPUPMOVETO"), "menu-move-to")), 0, 1, p, p + 1);
        p++;
    }

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;

    /***********************
     * Profile Operations
     * *********************/

    // Build "Copy Settings..." submenu with group drill-downs
    auto buildCopyFilterSubmenu = [this]() {
        Gtk::Menu* topMenu = Gtk::manage(new Gtk::Menu());
        copyFilters_.clear();
        int p = 0;

        // Prevent menu close on CheckMenuItem click
        auto preventClose = [](Gtk::CheckMenuItem* item) {
            item->signal_button_release_event().connect(
                [item](GdkEventButton*) {
                    item->set_active(!item->get_active());
                    return true;
                }, false);
        };

        // Prevent menu close on plain MenuItem click
        auto preventCloseAction = [](Gtk::MenuItem* item, std::function<void()> action) {
            item->signal_button_release_event().connect(
                [action](GdkEventButton*) {
                    action();
                    return true;
                }, false);
        };

        // Global "Copy All" / "Copy None"
        auto* copyAll = Gtk::manage(new MyImageMenuItem(M("GENERAL_ALL"), "menu-select-all"));
        preventCloseAction(copyAll, [this]() {
            for (auto& kv : copyFilters_) kv.second->set_active(true);
        });
        topMenu->attach(*copyAll, 0, 1, p, p + 1); p++;

        auto* copyNone = Gtk::manage(new MyImageMenuItem(M("GENERAL_NONE"), "menu-select-none"));
        preventCloseAction(copyNone, [this]() {
            for (auto& kv : copyFilters_) kv.second->set_active(false);
        });
        topMenu->attach(*copyNone, 0, 1, p, p + 1); p++;

        topMenu->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, p, p + 1); p++;

        // Add a group MenuItem with submenu of individual CheckMenuItems
        // Each submenu gets All/None toggles at the top
        auto addGroup = [&](const Glib::ustring& groupLabel,
                            const std::vector<std::pair<std::string, Glib::ustring>>& items) {
            Gtk::MenuItem* groupItem = Gtk::manage(new Gtk::MenuItem(groupLabel));
            topMenu->attach(*groupItem, 0, 1, p, p + 1);
            p++;

            Gtk::Menu* sub = Gtk::manage(new Gtk::Menu());
            int s = 0;

            // Collect children for group-level All/None
            std::vector<Gtk::CheckMenuItem*> children;
            for (const auto& kv : items) {
                auto* item = Gtk::manage(new Gtk::CheckMenuItem(kv.second));
                item->set_active(true);
                copyFilters_[kv.first] = item;
                preventClose(item);
                children.push_back(item);
            }

            // Group All / None
            auto* grpAll = Gtk::manage(new MyImageMenuItem(M("GENERAL_ALL"), "menu-select-all"));
            preventCloseAction(grpAll, [children]() {
                for (auto* c : children) c->set_active(true);
            });
            sub->attach(*grpAll, 0, 1, s, s + 1); s++;

            auto* grpNone = Gtk::manage(new MyImageMenuItem(M("GENERAL_NONE"), "menu-select-none"));
            preventCloseAction(grpNone, [children]() {
                for (auto* c : children) c->set_active(false);
            });
            sub->attach(*grpNone, 0, 1, s, s + 1); s++;

            sub->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, s, s + 1); s++;

            // Individual items
            for (auto* item : children) {
                sub->attach(*item, 0, 1, s, s + 1);
                s++;
            }

            sub->show_all();
            groupItem->set_submenu(*sub);
        };

        // Basic (no Local Contrast, no Tone Mapping/EPD/Fattal)
        addGroup(M("PARTIALPASTE_BASICGROUP"), {
            {"wb",            M("PARTIALPASTE_WHITEBALANCE")},
            {"toneCurve",     M("PARTIALPASTE_EXPOSURE")},
            {"sh",            M("PARTIALPASTE_SHADOWSHIGHLIGHTS")},
            {"toneEqualizer", M("PARTIALPASTE_TONE_EQUALIZER")},
        });

        // Detail
        addGroup(M("PARTIALPASTE_DETAILGROUP"), {
            {"sharpening",      M("PARTIALPASTE_SHARPENING")},
            {"sharpenEdge",     M("PARTIALPASTE_SHARPENEDGE")},
            {"sharpenMicro",    M("PARTIALPASTE_SHARPENMICRO")},
            {"impulseDenoise",  M("PARTIALPASTE_IMPULSEDENOISE")},
            {"dirpyrDenoise",   M("PARTIALPASTE_DIRPYRDENOISE")},
            {"defringe",        M("PARTIALPASTE_DEFRINGE")},
            {"dehaze",          M("PARTIALPASTE_DEHAZE")},
            {"dirpyrequalizer", M("PARTIALPASTE_DIRPYREQUALIZER")},
        });

        // Color (no Color Appearance, no ICM)
        addGroup(M("PARTIALPASTE_COLORGROUP"), {
            {"labCurve",       M("PARTIALPASTE_LABCURVE")},
            {"rgbCurves",      M("PARTIALPASTE_RGBCURVES")},
            {"colorToning",    M("PARTIALPASTE_COLORTONING")},
            {"chmixer",        M("PARTIALPASTE_CHANNELMIXER")},
            {"blackwhite",     M("PARTIALPASTE_CHANNELMIXERBW")},
            {"hsvequalizer",   M("PARTIALPASTE_HSVEQUALIZER")},
            {"filmSimulation", M("PARTIALPASTE_FILMSIMULATION")},
            {"softlight",      M("PARTIALPASTE_SOFTLIGHT")},
            {"vibrance",       M("PARTIALPASTE_VIBRANCE")},
        });

        // Lens
        addGroup(M("PARTIALPASTE_LENSGROUP"), {
            {"distortion",   M("PARTIALPASTE_DISTORTION")},
            {"cacorrection", M("PARTIALPASTE_CACORRECTION")},
            {"vignetting",   M("PARTIALPASTE_VIGNETTING")},
            {"lensProf",     M("PARTIALPASTE_LENSPROFILE")},
        });

        // Composition (no Post-Crop Vignetting)
        addGroup(M("PARTIALPASTE_COMPOSITIONGROUP"), {
            {"coarse",       M("PARTIALPASTE_COARSETRANS")},
            {"rotate",       M("PARTIALPASTE_ROTATION")},
            {"crop",         M("PARTIALPASTE_CROP")},
            {"resize",       M("PARTIALPASTE_RESIZE")},
            {"prsharpening", M("PARTIALPASTE_PRSHARPENING")},
            {"perspective",  M("PARTIALPASTE_PERSPECTIVE")},
            {"commonTrans",  M("PARTIALPASTE_COMMONTRANSFORMPARAMS")},
            {"gradient",     M("PARTIALPASTE_GRADIENT")},
            {"framing",      M("PARTIALPASTE_FRAMING")},
        });

        // Advanced
        addGroup(M("PARTIALPASTE_ADVANCEDGROUP"), {
            {"retinex", M("PARTIALPASTE_RETINEX")},
            {"wavelet", M("PARTIALPASTE_EQUALIZER")},
            {"spot",    M("PARTIALPASTE_SPOT")},
            {"cg",      M("PARTIALPASTE_COMPRESSGAMUT")},
        });

        // Selective Editing — single item directly in top menu
        auto* locallabItem = Gtk::manage(new Gtk::CheckMenuItem(M("PARTIALPASTE_LOCALLABGROUP")));
        locallabItem->set_active(true);
        copyFilters_["locallab"] = locallabItem;
        preventClose(locallabItem);
        topMenu->attach(*locallabItem, 0, 1, p, p + 1);
        p++;

        topMenu->show_all();
        return topMenu;
    };

    if (options.menuGroupProfileOperations) {
        pmenu->attach (*Gtk::manage(menuProfileOperations = new MyImageMenuItem (M("FILEBROWSER_POPUPPROFILEOPERATIONS"), "menu-profile-apply")), 0, 1, p, p + 1);
        p++;

        Gtk::Menu* submenuProfileOperations = Gtk::manage (new Gtk::Menu ());

        submenuProfileOperations->attach (*Gtk::manage(copyprof = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE"), "menu-profile-copy")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(copyprofSettings = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE_SETTINGS"), "gears")), 0, 1, p, p + 1);
        copyprofSettings->set_submenu (*buildCopyFilterSubmenu());
        p++;
        submenuProfileOperations->attach (*Gtk::manage(pasteprof = new MyImageMenuItem (M("FILEBROWSER_PASTEPROFILE"), "menu-profile-paste")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(partpasteprof = new MyImageMenuItem (M("FILEBROWSER_PARTIALPASTEPROFILE"), "menu-profile-partial")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(applyprof = new MyImageMenuItem (M("FILEBROWSER_APPLYPROFILE"), "menu-profile-apply")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(applypartprof = new MyImageMenuItem (M("FILEBROWSER_APPLYPROFILE_PARTIAL"), "menu-profile-apply-partial")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(resetdefaultprof = new MyImageMenuItem (M("FILEBROWSER_RESETDEFAULTPROFILE"), "menu-profile-reset")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(clearprof = new MyImageMenuItem (M("FILEBROWSER_CLEARPROFILE"), "menu-profile-clear")), 0, 1, p, p + 1);
        p++;

        submenuProfileOperations->show_all ();
        menuProfileOperations->set_submenu (*submenuProfileOperations);
    } else {
        pmenu->attach (*Gtk::manage(copyprof = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE"), "menu-profile-copy")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(copyprofSettings = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE_SETTINGS"), "gears")), 0, 1, p, p + 1);
        copyprofSettings->set_submenu (*buildCopyFilterSubmenu());
        p++;
        pmenu->attach (*Gtk::manage(pasteprof = new MyImageMenuItem (M("FILEBROWSER_PASTEPROFILE"), "menu-profile-paste")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(partpasteprof = new MyImageMenuItem (M("FILEBROWSER_PARTIALPASTEPROFILE"), "menu-profile-partial")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(applyprof = new MyImageMenuItem (M("FILEBROWSER_APPLYPROFILE"), "menu-profile-apply")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(applypartprof = new MyImageMenuItem (M("FILEBROWSER_APPLYPROFILE_PARTIAL"), "menu-profile-apply-partial")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(resetdefaultprof = new MyImageMenuItem (M("FILEBROWSER_RESETDEFAULTPROFILE"), "menu-profile-reset")), 0, 1, p, p + 1);
        p++;
        pmenu->attach (*Gtk::manage(clearprof = new MyImageMenuItem (M("FILEBROWSER_CLEARPROFILE"), "menu-profile-clear")), 0, 1, p, p + 1);
        p++;
    }


    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(menuDF = new MyImageMenuItem (M("FILEBROWSER_DARKFRAME"), "menu-darkframe")), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(menuFF = new MyImageMenuItem (M("FILEBROWSER_FLATFIELD"), "menu-flatfield")), 0, 1, p, p + 1);
    p++;

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(cachemenu = new MyImageMenuItem (M("FILEBROWSER_CACHE"), "menu-cache-clear")), 0, 1, p, p + 1);

    pmenu->show_all ();

    /***********************
     * Accelerators
     * *********************/
    pmaccelgroup = Gtk::AccelGroup::create ();
    pmenu->set_accel_group (pmaccelgroup);
    selall->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_a, Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
    trash->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_Delete, (Gdk::ModifierType)0, Gtk::ACCEL_VISIBLE);
    untrash->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_Delete, Gdk::SHIFT_MASK, Gtk::ACCEL_VISIBLE);
    open->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_Return, (Gdk::ModifierType)0, Gtk::ACCEL_VISIBLE);
    if (options.inspectorWindow)
        inspect->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_f, (Gdk::ModifierType)0, Gtk::ACCEL_VISIBLE);
    develop->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_B, Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
    developfast->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_B, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, Gtk::ACCEL_VISIBLE);
    copyprof->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_C, Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
    pasteprof->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_V, Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
    partpasteprof->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_V, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, Gtk::ACCEL_VISIBLE);
    copyTo->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_C, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, Gtk::ACCEL_VISIBLE);
    moveTo->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_M, Gdk::CONTROL_MASK | Gdk::SHIFT_MASK, Gtk::ACCEL_VISIBLE);

    // Bind to event handlers
    open->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), open));

    if (options.inspectorWindow) {
        inspect->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), inspect));
    }

    for (int i = 0; i < 2; i++) {
        sortOrder[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), sortOrder[i]));
    }

    for (int i = 0; i < Options::SORT_METHOD_COUNT; i++) {
        sortMethod[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), sortMethod[i]));
    }

    for (int i = 0; i < 6; i++) {
        rank[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), rank[i]));
    }

    for (int i = 0; i < 6; i++) {
        colorlabel[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), colorlabel[i]));
    }

    pickFlag->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), pickFlag));
    rejectFlag->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), rejectFlag));
    unflagFlag->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), unflagFlag));

    for (size_t i = 0; i < mMenuExtProgs.size(); i++) {
        amiExtProg[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), amiExtProg[i]));
    }

#ifdef _WIN32
    if (miOpenDefaultViewer) {
        miOpenDefaultViewer->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), miOpenDefaultViewer));
    }
#endif

    trash->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), trash));
    untrash->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), untrash));
    develop->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), develop));
    developfast->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), developfast));
    saveImage->signal_activate().connect ([this]() { m_save_image_requested.emit(); });
    if (editExternal) {
        editExternal->signal_activate().connect([this]() { m_external_editor_requested.emit(); });
    }
    rename->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), rename));
    remove->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), remove));
    removeInclProc->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), removeInclProc));
    selall->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), selall));
    copyTo->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), copyTo));
    moveTo->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), moveTo));
    copyprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), copyprof));
    pasteprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), pasteprof));
    partpasteprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), partpasteprof));
    applyprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), applyprof));
    applypartprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), applypartprof));
    resetdefaultprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), resetdefaultprof));
    clearprof->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), clearprof));
    cachemenu->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), cachemenu));
    aiDenoise->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), aiDenoise));
    autoGrade->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoGrade));
    autoGradeFilm->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoGradeFilm));
    autoGradedFilm->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoGradedFilm));
    auto* autoEditSubmenu = dynamic_cast<Gtk::Menu*>(autoEditMenu->get_submenu());
    autoEditMenu->signal_select().connect([this, autoEditSubmenu]() {
        if (!autoEditSubmenu) {
            return;
        }
        // Gtk occasionally misses submenu crossing/prelight events on native
        // Windows popups. Track the actual GDK pointer while this submenu is
        // open so Film Lab highlights and previews on the first hover.
        autoEditHoverTrackingConnection_.disconnect();
        autoEditHoverTrackingConnection_ = Glib::signal_timeout().connect(
            [this, autoEditSubmenu]() -> bool {
                if (!pmenu->get_visible()) {
                    return false;
                }
                if (!autoEditSubmenu->get_visible()) {
                    return true;
                }

                auto display = autoEditSubmenu->get_display();
                if (!display) {
                    return true;
                }
                auto seat = display->get_default_seat();
                auto device = seat ? seat->get_pointer() : Glib::RefPtr<Gdk::Device>();
                if (!device) {
                    return true;
                }

                const auto pointerIsOver = [&device](Gtk::MenuItem* menuItem) {
                    auto window = menuItem->get_window();
                    if (!window) {
                        return false;
                    }

                    int pointerX = 0;
                    int pointerY = 0;
                    Gdk::ModifierType modifiers;
                    window->get_device_position(device, pointerX, pointerY, modifiers);
                    const auto allocation = menuItem->get_allocation();
                    return pointerX >= allocation.get_x()
                        && pointerY >= allocation.get_y()
                        && pointerX < allocation.get_x() + allocation.get_width()
                        && pointerY < allocation.get_y() + allocation.get_height();
                };

                Gtk::MenuItem* hoveredItem = pointerIsOver(autoGradedFilm)
                    ? static_cast<Gtk::MenuItem*>(autoGradedFilm)
                    : pointerIsOver(autoGradeFilm)
                      ? static_cast<Gtk::MenuItem*>(autoGradeFilm)
                      : pointerIsOver(autoGrade)
                        ? static_cast<Gtk::MenuItem*>(autoGrade)
                        : nullptr;
                if (hoveredItem) {
                    gtk_menu_shell_select_item(
                        GTK_MENU_SHELL(autoEditSubmenu->gobj()),
                        GTK_WIDGET(hoveredItem->gobj()));
                    startAutoEditHoverPreview(hoveredItem);
                }
                return true;
            },
            35,
            G_PRIORITY_DEFAULT_IDLE);
    });
    autoGrade->signal_select().connect(sigc::bind(sigc::mem_fun(*this, &FileBrowser::startAutoEditHoverPreview), autoGrade));
    autoGradeFilm->signal_select().connect(sigc::bind(sigc::mem_fun(*this, &FileBrowser::startAutoEditHoverPreview), autoGradeFilm));
    autoGradedFilm->signal_select().connect(sigc::bind(sigc::mem_fun(*this, &FileBrowser::startAutoEditHoverPreview), autoGradedFilm));
    const auto connectAutoEditHoverEvents = [this](Gtk::MenuItem* item) {
        item->add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::POINTER_MOTION_MASK);
        item->signal_enter_notify_event().connect([this, item](GdkEventCrossing*) -> bool {
            startAutoEditHoverPreview(item);
            return false;
        });
        item->signal_motion_notify_event().connect([this, item](GdkEventMotion*) -> bool {
            startAutoEditHoverPreview(item);
            return false;
        });
    };
    connectAutoEditHoverEvents(autoGrade);
    connectAutoEditHoverEvents(autoGradeFilm);
    connectAutoEditHoverEvents(autoGradedFilm);
    pmenu->signal_deactivate().connect([this]() {
        autoEditHoverTrackingConnection_.disconnect();
        cancelAutoEditHoverPreview(nullptr, true);
    });
    autoLevel->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoLevel));
    duplicate->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), duplicate));

    addToAlbum->signal_activate().connect([this]() {
        if (!addToAlbumSetter_) return;

        Glib::ustring selectedFile;
        {
            MYREADERLOCK(l, entryRW);
            if (selected.size() == 1) {
                selectedFile = static_cast<FileBrowserEntry*>(selected[0])->filename;
            }
        }

        if (!selectedFile.empty()) {
            addToAlbumSetter_(selectedFile);
        }
    });

    setAlbumCover->signal_activate().connect([this]() {
        if (!albumCoverSetter_) return;
        MYREADERLOCK(l, entryRW);
        if (selected.size() == 1) {
            Glib::ustring fname = (static_cast<FileBrowserEntry*>(selected[0]))->filename;
            albumCoverSetter_(fname);
        }
    });

    // A separate pop-up menu for Color Labels
    int c = 0;
    pmenuColorLabels = new Gtk::Menu();

    for (int i = 0; i <= 5; i++) {
        pmenuColorLabels->attach(*Gtk::manage(colorlabel_pop[i] = new MyImageMenuItem(M(Glib::ustring::compose("%1%2", "FILEBROWSER_POPUPCOLORLABEL", i)), clabelActiveIcons[i])), 0, 1, c, c + 1);
        c++;
    }

    pmenuColorLabels->show_all();

    // Has to be located after creation of applyprof and applypartprof
    updateProfileList ();

    // Bind to event handlers
    for (int i = 0; i <= 5; i++) {
        colorlabel_pop[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuColorlabelActivated), colorlabel_pop[i]));
    }
}

FileBrowser::~FileBrowser ()
{
    autoEditHoverTrackingConnection_.disconnect();
    cancelAutoEditHoverPreview(nullptr, false);
    autoEditHoverPool_.reset();
    if (autoLevelCancel_) {
        autoLevelCancel_->store(true, std::memory_order_release);
    }
    autoLevelPollConnection_.disconnect();
    if (autoLevelThread_.joinable()) {
        autoLevelThread_.join();
    }
    if (autoCullCancel_) {
        autoCullCancel_->store(true, std::memory_order_release);
    }
    autoCullPollConnection_.disconnect();
    if (autoCullThread_.joinable()) {
        autoCullThread_.join();
    }
    for (auto& undo : autoCullUndo_) {
        undo.first->decreaseRef();
    }
    autoCullUndo_.clear();

    // Flush any deferred rating persistence synchronously so no rank/label/
    // pick change is lost on teardown.
    persistConn_.disconnect();
    while (!persistQueue_.empty()) {
        Thumbnail* thm = persistQueue_.front();
        persistQueue_.pop_front();
        thm->updateCache();
        thm->decreaseRef();
    }
    idle_register.destroy();

    // Cancel deferred deletion callback and flush remaining entries
    deletionConnection_.disconnect();
    for (auto* entry : pendingDeletion_) {
        delete entry;
    }
    pendingDeletion_.clear();

    ProfileStore::getInstance()->removeListener(this);
    delete pmenu;
    delete pmenuColorLabels;
    delete[] amiExtProg;
}

void FileBrowser::rightClicked ()
{

    {
        MYREADERLOCK(l, entryRW);

        trash->set_sensitive (false);
        untrash->set_sensitive (false);

        for (size_t i = 0; i < selected.size(); i++)
            if ((static_cast<FileBrowserEntry*>(selected[i]))->thumbnail->getTrashed()) {
                untrash->set_sensitive (true);
                break;
            }

        for (size_t i = 0; i < selected.size(); i++)
            if (!(static_cast<FileBrowserEntry*>(selected[i]))->thumbnail->getTrashed()) {
                trash->set_sensitive (true);
                break;
            }

        pasteprof->set_sensitive (clipboard.hasProcParams());
        partpasteprof->set_sensitive (clipboard.hasProcParams());
        copyprof->set_sensitive (selected.size() == 1);
        clearprof->set_sensitive (!selected.empty());
        copyTo->set_sensitive (!selected.empty());
        moveTo->set_sensitive (!selected.empty());
        autoEditMenu->set_sensitive(!quickActionRunning_ && !selected.empty());
        autoLevel->set_sensitive(!quickActionRunning_ && !selected.empty());
    }

    // submenuDF
    int p = 0;
    Gtk::Menu* submenuDF = Gtk::manage (new Gtk::Menu ());
    submenuDF->attach (*Gtk::manage(selectDF = new MyImageMenuItem (M("FILEBROWSER_SELECTDARKFRAME"), "menu-darkframe")), 0, 1, p, p + 1);
    p++;
    submenuDF->attach (*Gtk::manage(autoDF = new MyImageMenuItem (M("FILEBROWSER_AUTODARKFRAME"), "menu-darkframe-auto")), 0, 1, p, p + 1);
    p++;
    submenuDF->attach (*Gtk::manage(thisIsDF = new MyImageMenuItem (M("FILEBROWSER_MOVETODARKFDIR"), "menu-darkframe-move")), 0, 1, p, p + 1);
    selectDF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), selectDF));
    autoDF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoDF));
    thisIsDF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), thisIsDF ));
    submenuDF->show_all ();
    menuDF->set_submenu (*submenuDF);

    // submenuFF
    p = 0;
    Gtk::Menu* submenuFF = Gtk::manage (new Gtk::Menu ());
    submenuFF->attach (*Gtk::manage(selectFF = new MyImageMenuItem (M("FILEBROWSER_SELECTFLATFIELD"), "menu-flatfield")), 0, 1, p, p + 1);
    p++;
    submenuFF->attach (*Gtk::manage(autoFF = new MyImageMenuItem (M("FILEBROWSER_AUTOFLATFIELD"), "menu-flatfield-auto")), 0, 1, p, p + 1);
    p++;
    submenuFF->attach (*Gtk::manage(thisIsFF = new MyImageMenuItem (M("FILEBROWSER_MOVETOFLATFIELDDIR"), "menu-flatfield-move")), 0, 1, p, p + 1);
    selectFF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), selectFF));
    autoFF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), autoFF));
    thisIsFF->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), thisIsFF ));
    submenuFF->show_all ();
    menuFF->set_submenu (*submenuFF);

    // build cache sub menu
    p = 0;
    Gtk::Menu* cachesubmenu = Gtk::manage (new Gtk::Menu ());
    cachesubmenu->attach (*Gtk::manage(clearFromCache = new MyImageMenuItem (M("FILEBROWSER_CACHECLEARFROMPARTIAL"), "menu-cache-partial")), 0, 1, p, p + 1);
    p++;
    cachesubmenu->attach (*Gtk::manage(clearFromCacheFull = new MyImageMenuItem (M("FILEBROWSER_CACHECLEARFROMFULL"), "menu-cache-clear")), 0, 1, p, p + 1);
    p++;
    clearFromCache->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), clearFromCache));
    clearFromCacheFull->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuItemActivated), clearFromCacheFull));
    cachesubmenu->show_all ();
    cachemenu->set_submenu (*cachesubmenu);

    // These actions operate on the current editor and only belong in its filmstrip.
    if (isInTabMode()) {
        saveImage->show();
        if (editExternal) {
            editExternal->show();
        }
    } else {
        saveImage->hide();
        if (editExternal) {
            editExternal->hide();
        }
    }

    // "Set as album cover" — only when viewing an album and exactly one image is selected
    {
        MYREADERLOCK(l2, entryRW);
        if (addToAlbumSetter_ && selected.size() == 1) {
            addToAlbum->show();
        } else {
            addToAlbum->hide();
        }

        if (isInAlbumMode_ && isInAlbumMode_() && selected.size() == 1) {
            setAlbumCover->show();
        } else {
            setAlbumCover->hide();
        }
    }

    pmenu->popup (3, this->eventTime);
}

void FileBrowser::doubleClicked (ThumbBrowserEntryBase* entry)
{

    if (tbl && entry) {
        openRequested({static_cast<FileBrowserEntry*>(entry)});
    }
}

void FileBrowser::addEntry (FileBrowserEntry* entry)
{
    const unsigned int sid = session_id();

    idle_register.add(
        [this, entry, sid]() -> bool
        {
            if (sid != session_id()) {
                delete entry;
            } else {
                addEntry_(entry);
            }

            return false;
        }
    );
}

void FileBrowser::reserveEntries (std::size_t additionalEntries)
{
    if (additionalEntries == 0) {
        return;
    }

    MYWRITERLOCK(l, entryRW);
    const std::size_t target = fd.size() + additionalEntries;
    fd.reserve(target);
    entryKeys_.reserve(target);
    entriesByKey_.reserve(target);
    if (filter.showOriginal) {
        originalFamilies_.reserve(target);
    }
}

void FileBrowser::addOriginalFamilyEntry_(ThumbBrowserEntryBase* entry)
{
    originalFamilies_[originalFamilyKeyForEntry(static_cast<FileBrowserEntry*>(entry))].push_back(entry);
}

void FileBrowser::ensureOriginalFamiliesCurrent_()
{
    if (originalFamiliesCurrent_) {
        return;
    }

    originalFamilies_.clear();
    originalFamilies_.reserve(fd.size());

    for (auto* entry : fd) {
        addOriginalFamilyEntry_(entry);
    }

    originalFamiliesCurrent_ = true;
}

void FileBrowser::clearOriginalMarks_()
{
    for (auto* entry : fd) {
        entry->setOriginal(nullptr);
    }

    originalFamilies_.clear();
    originalFamilies_.rehash(0);
    originalFamiliesCurrent_ = false;
}

void FileBrowser::markEntryIndexDirty_()
{
    entryIndex_.clear();
}

void FileBrowser::startAutoEditHoverPreview(Gtk::MenuItem* item)
{
    if (!item || quickActionRunning_) {
        return;
    }
    // Native popup themes may clear GTK's implicit prelight between motion
    // events. Reassert it before the same-item early return as well.
    item->set_state_flags(Gtk::STATE_FLAG_PRELIGHT, false);
    if (autoEditHoverItem_ == item
            && (autoEditHoverDelayConnection_.connected()
                || autoEditHoverInFlight_
                || !autoEditHoverPreviewFile_.empty())) {
        return;
    }

    cancelAutoEditHoverPreview(nullptr, true);
    autoEditHoverItem_ = item;

    autoEditHoverDelayConnection_ = Glib::signal_timeout().connect(
        [this, item]() -> bool {
            if (autoEditHoverItem_ != item || quickActionRunning_) {
                return false;
            }

            FileBrowserEntry* entry = nullptr;
            {
                MYREADERLOCK(l, entryRW);
                if (!selected.empty()) {
                    entry = static_cast<FileBrowserEntry*>(selected.back());
                }
            }
            if (!entry || !entry->thumbnail || !autoEditHoverPool_) {
                autoEditHoverItem_ = nullptr;
                autoEditHoverInFlight_ = false;
                return false;
            }

            const AutoEditMode mode = item == autoGradedFilm
                ? AutoEditMode::GradedFilm
                : item == autoGradeFilm
                 ? AutoEditMode::GradeFilm
                 : item == autoGrade
                  ? AutoEditMode::Grade
                  : AutoEditMode::Neutral;
            fileBrowserPerfLog(
                "[autoEditHover] start mode=%s file=%s\n",
                autoEditModeName(mode),
                entry->filename.c_str());
            const auto desired = entry->getDesiredPreviewSize();
            const Glib::ustring filename = entry->filename;
            Thumbnail* const thumbnail = entry->thumbnail;
            thumbnail->increaseRef();
            const unsigned generation = autoEditHoverGeneration_->fetch_add(1, std::memory_order_acq_rel) + 1;
            const auto generationState = autoEditHoverGeneration_;
            autoEditHoverInFlight_ = true;

            autoEditHoverPool_->push([this, generationState, generation, mode, desired, filename, thumbnail]() {
                if (generationState->load(std::memory_order_acquire) != generation) {
                    thumbnail->decreaseRef();
                    return;
                }

                const rtengine::procparams::ProcParams sourceParams = thumbnail->getProcParams();
                rtengine::procparams::ProcParams params;
                buildSteepAutoEditParamsInternal(*thumbnail, mode, sourceParams, params);
                if (generationState->load(std::memory_order_acquire) != generation) {
                    thumbnail->decreaseRef();
                    return;
                }

                // Start the full editor preview as soon as the parameters are
                // ready. Thumbnail rendering continues independently below.
                auto editorParams = params;
                idle_register.add(
                    [this, generationState, generation, filename, editorParams = std::move(editorParams)]() -> bool {
                        if (generationState->load(std::memory_order_acquire) != generation) {
                            return false;
                        }

                        const bool editorApplied = tbl
                            && tbl->transientEditPreviewRequested(filename, &editorParams, false);
                        fileBrowserPerfLog(
                            "[autoEditHover] editor-preview %s file=%s\n",
                            editorApplied ? "applied" : "no-matching-editor",
                            filename.c_str());
                        autoEditHoverInFlight_ = false;
                        autoEditHoverPreviewFile_ = filename;
                        return false;
                    });

                double scale = 1.0;
                const int previewHeight = desired.first.scaleToDevice(desired.second).height;
                rtengine::IImage8* image = thumbnail->processFullThumbImage(params, previewHeight, scale);
                thumbnail->decreaseRef();

                if (!image || generationState->load(std::memory_order_acquire) != generation) {
                    delete image;
                    return;
                }

                const auto crop = params.crop;
                idle_register.add([this, generationState, generation, mode, desired, filename, image, scale, crop]() -> bool {
                    if (generationState->load(std::memory_order_acquire) != generation) {
                        delete image;
                        return false;
                    }

                    auto* current = findEntry(filename);
                    if (!current) {
                        delete image;
                        return false;
                    }

                    ThumbImageUpdateListener::ImageUpdate update(
                        image,
                        desired.first,
                        desired.second,
                        scale,
                        crop);
                    current->updateImage(update);
                    autoEditHoverPreviewFile_ = filename;
                    fileBrowserPerfLog(
                        "[autoEditHover] applied mode=%s file=%s\n",
                        autoEditModeName(mode),
                        filename.c_str());
                    return false;
                });
            });

            return false;
        },
        110);
}

void FileBrowser::cancelAutoEditHoverPreview(Gtk::MenuItem* item, bool restore)
{
    if (item && autoEditHoverItem_ != item) {
        return;
    }

    autoEditHoverDelayConnection_.disconnect();
    if (autoEditHoverItem_) {
        autoEditHoverItem_->unset_state_flags(Gtk::STATE_FLAG_PRELIGHT);
    }
    autoEditHoverItem_ = nullptr;
    autoEditHoverInFlight_ = false;
    autoEditHoverGeneration_->fetch_add(1, std::memory_order_acq_rel);

    if (!autoEditHoverPreviewFile_.empty()) {
        const Glib::ustring previewFile = autoEditHoverPreviewFile_;
        if (tbl) {
            tbl->transientEditPreviewRequested(previewFile, nullptr, restore);
        }
        if (auto* entry = findEntry(previewFile)) {
            entry->invalidateTransientPreview(restore);
        }
        autoEditHoverPreviewFile_.clear();
    }
}

ThumbBrowserEntryBase* FileBrowser::findEntryLocked_(const Glib::ustring& fname)
{
    const std::string fnameKey = browserPathKey(fname);
    const auto entry = entriesByKey_.find(fnameKey);
    if (entry != entriesByKey_.end()) {
        return entry->second;
    }

    for (auto* candidate : fd) {
        if (candidate->filename == fname || browserPathKeyMatchesEntry(candidate, fnameKey)) {
            entriesByKey_[fnameKey] = candidate;
            return candidate;
        }
    }

    return nullptr;
}

std::ptrdiff_t FileBrowser::findEntryIndexLocked_(const Glib::ustring& fname)
{
    const std::string fnameKey = browserPathKey(fname);
    ThumbBrowserEntryBase* mappedEntry = nullptr;
    const auto entry = entriesByKey_.find(fnameKey);
    if (entry != entriesByKey_.end()) {
        mappedEntry = entry->second;
    }

    auto idxIt = entryIndex_.find(fnameKey);

    if (idxIt != entryIndex_.end() && idxIt->second < fd.size()) {
        const auto* indexedEntry = fd[idxIt->second];
        if ((mappedEntry && indexedEntry == mappedEntry)
            || indexedEntry->filename == fname
            || browserPathKeyMatchesEntry(indexedEntry, fnameKey)) {
            return static_cast<std::ptrdiff_t>(idxIt->second);
        }
    }

    if (mappedEntry) {
        const auto found = std::find(fd.begin(), fd.end(), mappedEntry);
        if (found != fd.end()) {
            const auto index = static_cast<size_t>(std::distance(fd.begin(), found));
            entryIndex_[fnameKey] = index;
            return static_cast<std::ptrdiff_t>(index);
        }

        entriesByKey_.erase(fnameKey);
    }

    // Defensive fallback: if a caller observes the list between an unusual
    // mutation and map update, recover once instead of dropping adjacent
    // preload/refresh for this navigation step.
    for (size_t i = 0; i < fd.size(); ++i) {
        if (fd[i]->filename == fname || browserPathKeyMatchesEntry(fd[i], fnameKey)) {
            entriesByKey_[fnameKey] = fd[i];
            entryIndex_[fnameKey] = i;
            return static_cast<std::ptrdiff_t>(i);
        }
    }

    return -1;
}

std::ptrdiff_t FileBrowser::findEntryIndexReadLocked_(const Glib::ustring& fname)
{
    const std::string fnameKey = browserPathKey(fname);
    ThumbBrowserEntryBase* mappedEntry = nullptr;
    const auto entry = entriesByKey_.find(fnameKey);
    if (entry != entriesByKey_.end()) {
        mappedEntry = entry->second;
    }

    const auto idxIt = entryIndex_.find(fnameKey);

    if (idxIt != entryIndex_.end() && idxIt->second < fd.size()) {
        const auto* indexedEntry = fd[idxIt->second];
        if ((mappedEntry && indexedEntry == mappedEntry)
            || indexedEntry->filename == fname
            || browserPathKeyMatchesEntry(indexedEntry, fnameKey)) {
            return static_cast<std::ptrdiff_t>(idxIt->second);
        }
    }

    if (mappedEntry) {
        const auto found = std::find(fd.begin(), fd.end(), mappedEntry);
        if (found != fd.end()) {
            return static_cast<std::ptrdiff_t>(std::distance(fd.begin(), found));
        }
    }

    for (size_t i = 0; i < fd.size(); ++i) {
        if (fd[i]->filename == fname || browserPathKeyMatchesEntry(fd[i], fnameKey)) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }

    return -1;
}

std::ptrdiff_t FileBrowser::findEntryIndexLocked_(ThumbBrowserEntryBase* entry)
{
    if (!entry) {
        return -1;
    }

    const std::string entryKey = browserPathKeyForEntry(entry);
    auto idxIt = entryIndex_.find(entryKey);

    if (idxIt != entryIndex_.end() && idxIt->second < fd.size() && fd[idxIt->second] == entry) {
        return static_cast<std::ptrdiff_t>(idxIt->second);
    }

    const auto found = std::find(fd.begin(), fd.end(), entry);
    if (found == fd.end()) {
        entryIndex_.erase(entryKey);
        entriesByKey_.erase(entryKey);
        return -1;
    }

    const auto index = static_cast<size_t>(std::distance(fd.begin(), found));
    entryIndex_[entryKey] = index;
    entriesByKey_[entryKey] = entry;

    return static_cast<std::ptrdiff_t>(index);
}

void FileBrowser::flushPendingInsertsForSelection_()
{
    if (layoutPaused_) {
        return;
    }

    bool hasPending = false;
    {
        MyMutex::MyLock lock(pendingMutex_);
        hasPending = !pendingInserts_.empty();
    }

    if (hasPending) {
        redraw();
    }
}

void FileBrowser::entriesOrderChanged_()
{
    markEntryIndexDirty_();
}

void FileBrowser::entriesInserted_(const std::vector<ThumbBrowserEntryBase*>& entries)
{
    entriesByKey_.reserve(entriesByKey_.size() + entries.size());

    for (auto* entry : entries) {
        const auto* fileEntry = static_cast<FileBrowserEntry*>(entry);
        const std::string& entryKey = fileEntry->getBrowserPathKey();
        entriesByKey_[entryKey.empty() ? browserPathKey(entry->filename) : entryKey] = entry;
    }
}

void FileBrowser::removeOriginalFamilyEntry_(ThumbBrowserEntryBase* entry)
{
    if (!originalFamiliesCurrent_) {
        return;
    }

    const std::string familyKey = originalFamilyKeyForEntry(static_cast<FileBrowserEntry*>(entry));
    auto family = originalFamilies_.find(familyKey);

    if (family == originalFamilies_.end()) {
        return;
    }

    auto& entries = family->second;
    entries.erase(std::remove(entries.begin(), entries.end(), entry), entries.end());

    if (entries.empty()) {
        originalFamilies_.erase(family);
    }
}

void FileBrowser::refreshAllOriginalFamilies_()
{
    for (const auto& family : originalFamilies_) {
        ThumbBrowserEntryBase* original = nullptr;

        for (const auto entry : family.second) {
            original = selectOriginalEntry(original, entry);
        }

        for (const auto entry : family.second) {
            entry->setOriginal(entry != original ? original : nullptr);
        }
    }
}

void FileBrowser::refreshOriginalFamily_(const std::string& familyKey)
{
    auto family = originalFamilies_.find(familyKey);

    if (family == originalFamilies_.end()) {
        return;
    }

    ThumbBrowserEntryBase* original = nullptr;

    for (const auto entry : family->second) {
        original = selectOriginalEntry(original, entry);
    }

    for (const auto entry : family->second) {
        entry->setOriginal(entry != original ? original : nullptr);
    }

    if (filter.showOriginal) {
        for (auto* familyEntry : family->second) {
            familyEntry->filtered = !checkFilter(familyEntry);
        }
    }
}

bool FileBrowser::addEntry_ (FileBrowserEntry* entry)
{
    std::vector<FileBrowserEntry*> entries;
    entries.push_back(entry);
    return addEntries_(entries) == 1;
}

std::size_t FileBrowser::addEntries_ (std::vector<FileBrowserEntry*>& entries)
{
    if (entries.empty()) {
        return 0;
    }

    std::vector<ThumbBrowserEntryBase*> acceptedEntries;
    acceptedEntries.reserve(entries.size());
    std::size_t accepted = 0;
    const int thumbnailHeight = getThumbnailHeight();
    const bool maintainOriginalFamilies = filter.showOriginal;
    const bool hasEditedFiles = !editedFiles.empty();

    for (auto* entry : entries) {
        std::string computedEntryKey;
        const std::string& existingEntryKey = entry->getBrowserPathKey();
        const std::string& entryKey = !existingEntryKey.empty()
            ? existingEntryKey
            : (computedEntryKey = browserPathKey(entry->filename));
        if (!entryKeys_.insert(entryKey).second) {
            delete entry;
            continue;
        }
        if (existingEntryKey.empty()) {
            entry->setBrowserPathKey(std::move(computedEntryKey));
        }

        entry->setParent(this);

        entry->selected = false;
        entry->drawable = false;
        entry->framed = hasEditedFiles && editedFiles.find(entry->filename) != editedFiles.end();

        const int rank = entry->thumbnail->getRank();
        const int colorLabel = entry->thumbnail->getColorLabel();
        if (rank > 0 || colorLabel > 0) {
            auto* thumbButtonSet = entry->ensureThumbButtonSet(this);
            thumbButtonSet->setRank(rank);
            thumbButtonSet->setColorLabel(colorLabel);
        }
        if (maintainOriginalFamilies) {
            const std::string familyKey = originalFamilyKeyForEntry(entry);
            addOriginalFamilyEntry_(entry);
            refreshOriginalFamily_(familyKey);
        } else if (filterPassThrough_) {
            entry->filtered = false;
        } else {
            entry->filtered = !checkFilter(entry);
        }
        if (!entry->filtered) {
            ++numFiltered;
        }

        // Establish layout dimensions now, but let the draw/filter paths
        // request rendered thumbnail pixels only for entries that become
        // visible. Large folders should not spend CPU on off-screen thumbs.
        entry->resizeWithoutThumbnailJob(thumbnailHeight);

        entries[accepted++] = entry;
        acceptedEntries.push_back(entry);
    }

    entries.resize(accepted);
    if (accepted > 0 && !maintainOriginalFamilies) {
        originalFamiliesCurrent_ = false;
    }
    insertEntries(acceptedEntries);

    return accepted;
}

FileBrowserEntry* FileBrowser::delEntry (const Glib::ustring& fname)
{
    MYWRITERLOCK(l, entryRW);

    for (std::vector<ThumbBrowserEntryBase*>::iterator i = fd.begin(); i != fd.end(); ++i)
        if ((*i)->filename == fname) {
            ThumbBrowserEntryBase* entry = *i;
            clearVisibleEntries_();
            clearDrawableEntries_();
            if (filter.showOriginal) {
                ensureOriginalFamiliesCurrent_();
            }
            entry->selected = false;
            const std::string entryKey = browserPathKeyForEntry(entry);
            entryKeys_.erase(entryKey);
            entriesByKey_.erase(entryKey);
            const std::string familyKey = originalFamiliesCurrent_
                ? originalFamilyKeyForEntry(static_cast<FileBrowserEntry*>(entry))
                : std::string();
            removeOriginalFamilyEntry_(entry);
            if (filter.showOriginal) {
                refreshOriginalFamily_(familyKey);
            }
            fd.erase (i);
            markEntryIndexDirty_();
            std::vector<ThumbBrowserEntryBase*>::iterator j = std::find (selected.begin(), selected.end(), entry);

            MYWRITERLOCK_RELEASE(l);

            if (j != selected.end()) {
                if (checkFilter (*j)) {
                    numFiltered--;
                }

                selected.erase (j);
                notifySelectionListener ();
            }

            if (lastClicked == entry) {
                lastClicked = nullptr;
            }

            redraw ();

            return (static_cast<FileBrowserEntry*>(entry));
        }

    return nullptr;
}

std::vector<FileBrowserEntry*> FileBrowser::delEntries (const std::set<Glib::ustring>& fnames)
{
    std::vector<FileBrowserEntry*> removed;
    removed.reserve(fnames.size());

    bool selectionChanged = false;

    {
        MYWRITERLOCK(l, entryRW);
        std::unordered_set<std::string> affectedFamilies;
        clearVisibleEntries_();
        clearDrawableEntries_();
        if (filter.showOriginal) {
            ensureOriginalFamiliesCurrent_();
        }

        // Remove matching entries from fd in a single pass
        auto newEnd = std::stable_partition(fd.begin(), fd.end(),
            [&fnames](ThumbBrowserEntryBase* e) {
                return fnames.find(e->filename) == fnames.end();
            });

        for (auto it = newEnd; it != fd.end(); ++it) {
            ThumbBrowserEntryBase* entry = *it;
            entry->selected = false;

            auto j = std::find(selected.begin(), selected.end(), entry);
            if (j != selected.end()) {
                if (checkFilter(*j)) {
                    numFiltered--;
                }
                selected.erase(j);
                selectionChanged = true;
            }

            if (lastClicked == entry) {
                lastClicked = nullptr;
            }

            const std::string entryKey = browserPathKeyForEntry(entry);
            entryKeys_.erase(entryKey);
            entriesByKey_.erase(entryKey);
            if (originalFamiliesCurrent_) {
                const std::string familyKey = originalFamilyKeyForEntry(static_cast<FileBrowserEntry*>(entry));
                affectedFamilies.insert(familyKey);
            }
            removeOriginalFamilyEntry_(entry);
            removed.push_back(static_cast<FileBrowserEntry*>(entry));
        }

        fd.erase(newEnd, fd.end());
        markEntryIndexDirty_();

        if (filter.showOriginal) {
            for (const auto& familyKey : affectedFamilies) {
                refreshOriginalFamily_(familyKey);
            }
        }
    }

    if (selectionChanged) {
        notifySelectionListener();
    }

    redraw();

    return removed;
}

void FileBrowser::close ()
{
    ++session_id_;
    clearVisibleEntries_();
    clearDrawableEntries_();

    // Drain any pending deletions from PREVIOUS folder switches immediately.
    // The idle callback runs at G_PRIORITY_LOW which is starved during active
    // loading, causing thousands of old Thumbnails (with image buffers) to
    // accumulate across folder switches.  Free them now before loading more.
    if (deletionConnection_.connected()) {
        deletionConnection_.disconnect();
    }
    for (auto* entry : pendingDeletion_) {
        delete entry;
    }
    pendingDeletion_.clear();

    // Clear any pending batch inserts — move to deferred deletion
    redrawTimeout_.disconnect();
    {
        MyMutex::MyLock lock(pendingMutex_);
        pendingDeletion_.insert(pendingDeletion_.end(),
                                pendingInserts_.begin(),
                                pendingInserts_.end());
        pendingInserts_.clear();
        redrawPending_ = false;
    }

    {
        MYWRITERLOCK(l, entryRW);

        selected.clear ();
        anchor = nullptr;

        MYWRITERLOCK_RELEASE(l); // notifySelectionListener will need read access!

        notifySelectionListener ();

        MYWRITERLOCK_ACQUIRE(l);

        // Move entries to deferred deletion instead of blocking the main thread
        pendingDeletion_.insert(pendingDeletion_.end(), fd.begin(), fd.end());
        fd.clear ();
        entryKeys_.clear();
        entriesByKey_.clear();
        originalFamilies_.clear();
        originalFamilies_.rehash(0);
        originalFamiliesCurrent_ = true;
        entryIndex_.clear();
        lastOpenRequestedFname_.clear();
    }

    lastClicked = nullptr;

    // Schedule batched deletion via idle callback
    if (!pendingDeletion_.empty() && !deletionConnection_.connected()) {
        deletionConnection_ = Glib::signal_idle().connect(
            sigc::mem_fun(*this, &FileBrowser::onDeletionIdle_),
            G_PRIORITY_LOW
        );
    }
}

bool FileBrowser::onDeletionIdle_()
{
    const int BATCH_SIZE = 64;
    int count = 0;

    while (!pendingDeletion_.empty() && count < BATCH_SIZE) {
        delete pendingDeletion_.back();
        pendingDeletion_.pop_back();
        ++count;
    }

    if (pendingDeletion_.empty()) {
        return false; // all done, unregister
    }
    return true; // more to delete, call again
}

void FileBrowser::menuColorlabelActivated (Gtk::MenuItem* m)
{

    std::vector<FileBrowserEntry*> tbe;
    tbe.push_back (static_cast<FileBrowserEntry*>(colorLabel_actionData));

    for (int i = 0; i < 6; i++)
        if (m == colorlabel_pop[i]) {
            colorlabelRequested (tbe, i);
            return;
        }
}

void FileBrowser::menuItemActivated (Gtk::MenuItem* m)
{
    std::vector<FileBrowserEntry*> mselected;

    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }
    }


    if (!tbl || (m != selall && mselected.empty()) ) {
        return;
    }

    for (int i = 0; i < 2; i++)
        if (m == sortOrder[i]) {
            sortOrderRequested (i);
            return;
        }

    for (int i = 0; i < Options::SORT_METHOD_COUNT; i++)
        if (m == sortMethod[i]) {
            sortMethodRequested (i);
            return;
        }

    for (int i = 0; i < 6; i++)
        if (m == rank[i]) {
            rankingRequested (mselected, i);
            return;
        }

    for (int i = 0; i < 6; i++)
        if (m == colorlabel[i]) {
            colorlabelRequested (mselected, i);
            return;
        }

    if (m == pickFlag) {
        pickRequested (mselected, 1);
        return;
    } else if (m == rejectFlag) {
        pickRequested (mselected, -1);
        return;
    } else if (m == unflagFlag) {
        pickRequested (mselected, 0);
        return;
    }

    const auto& options = App::get().options();

    for (size_t j = 0; j < mMenuExtProgs.size(); j++) {
        if (m == amiExtProg[j]) {
            const auto pAct = mMenuExtProgs[m->get_label()];

            // Build vector of all file names
            std::vector<Glib::ustring> selFileNames;

            for (size_t i = 0; i < mselected.size(); i++) {
                Glib::ustring fn = mselected[i]->thumbnail->getFileName();

                // Maybe batch processed version
                if (pAct->target == 2) {
                    fn = Glib::ustring::compose ("%1.%2", BatchQueue::calcAutoFileNameBase(fn), options.saveFormatBatch.format);
                }

                selFileNames.push_back(fn);
            }

            pAct->execute (selFileNames);
            return;
        }
    }

    if (m == open) {
        openRequested(mselected);
    } else if (options.inspectorWindow && m == inspect) {
        inspectRequested(mselected);
    } else if (m == remove) {
        tbl->deleteRequested (mselected, false, true);
    } else if (m == removeInclProc) {
        tbl->deleteRequested (mselected, true, true);
    } else if (m == trash) {
        toTrashRequested (mselected);
    } else if (m == untrash) {
        fromTrashRequested (mselected);
    }

    else if (m == develop) {
        tbl->developRequested (mselected, false);
    } else if (m == developfast) {
        if (exportPanel) {
            // force saving export panel settings
            exportPanel->setExportPanelListener(nullptr);
            exportPanel->FastExportPressed();
            exportPanel->setExportPanelListener(this);
        }
        tbl->developRequested (mselected, true);
    }

    else if (m == rename) {
        tbl->renameRequested (mselected);
    } else if (m == selall) {
        lastClicked = nullptr;
        {
            MYWRITERLOCK(l, entryRW);

            selected.clear();

            for (size_t i = 0; i < fd.size(); ++i) {
                if (checkFilter(fd[i])) {
                    fd[i]->selected = true;
                    selected.push_back(fd[i]);
                }
            }
            if (!anchor && !selected.empty()) {
                anchor = selected[0];
            }
        }
        queue_draw ();
        notifySelectionListener();
    } else if( m == copyTo) {
        tbl->copyMoveRequested (mselected, false);
    }

    else if( m == moveTo) {
        tbl->copyMoveRequested (mselected, true);
    }

    else if (m == autoDF) {
        if (!mselected.empty() && bppcl) {
            bppcl->beginBatchPParamsChange(mselected.size());
        }

        for (size_t i = 0; i < mselected.size(); i++) {
            rtengine::procparams::ProcParams pp = mselected[i]->thumbnail->getProcParams();
            pp.raw.df_autoselect = true;
            pp.raw.dark_frame.clear();
            mselected[i]->thumbnail->setProcParams(pp, nullptr, FILEBROWSER, false);
        }

        if (!mselected.empty() && bppcl) {
            bppcl->endBatchPParamsChange();
        }

    } else if (m == selectDF) {
        if( !mselected.empty() ) {
            rtengine::procparams::ProcParams pp = mselected[0]->thumbnail->getProcParams();
            Gtk::FileChooserDialog fc (getToplevelWindow (this), "Dark Frame", Gtk::FILE_CHOOSER_ACTION_OPEN );
            bindCurrentFolder (fc, App::get().mut_options().lastDarkframeDir);
            fc.add_button( M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
            fc.add_button( M("GENERAL_APPLY"), Gtk::RESPONSE_APPLY);

            if(!pp.raw.dark_frame.empty()) {
                fc.set_filename( pp.raw.dark_frame );
            }

            if( fc.run() == Gtk::RESPONSE_APPLY ) {
                if (bppcl) {
                    bppcl->beginBatchPParamsChange(mselected.size());
                }

                for (size_t i = 0; i < mselected.size(); i++) {
                    rtengine::procparams::ProcParams lpp = mselected[i]->thumbnail->getProcParams();
                    lpp.raw.dark_frame = fc.get_filename();
                    lpp.raw.df_autoselect = false;
                    mselected[i]->thumbnail->setProcParams(lpp, nullptr, FILEBROWSER, false);
                }

                if (bppcl) {
                    bppcl->endBatchPParamsChange();
                }
            }
        }
    } else if( m == thisIsDF) {
        if( !options.rtSettings.darkFramesPath.empty()) {
            if (Gio::File::create_for_path(options.rtSettings.darkFramesPath)->query_exists() ) {
                for (size_t i = 0; i < mselected.size(); i++) {
                    Glib::RefPtr<Gio::File> file = Gio::File::create_for_path ( mselected[i]->filename );

                    if( !file ) {
                        continue;
                    }

                    Glib::ustring destName = options.rtSettings.darkFramesPath + "/" + file->get_basename();
                    Glib::RefPtr<Gio::File> dest = Gio::File::create_for_path ( destName );
                    file->move(  dest );
                }

                // Reinit cache
                rtengine::DFManager::getInstance().init( options.rtSettings.darkFramesPath );
            } else {
                // Target directory creation failed, we clear the darkFramesPath setting
                App::get().mut_options().rtSettings.darkFramesPath.clear();
                Glib::ustring msg_ = Glib::ustring::compose (M("MAIN_MSG_PATHDOESNTEXIST"), escapeHtmlChars(options.rtSettings.darkFramesPath))
                                     + "\n\n" + M("MAIN_MSG_OPERATIONCANCELLED");
                Gtk::MessageDialog msgd (msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
                msgd.set_title(M("TP_DARKFRAME_LABEL"));
                msgd.run ();
            }
        } else {
            Glib::ustring msg_ = M("MAIN_MSG_SETPATHFIRST") + "\n\n" + M("MAIN_MSG_OPERATIONCANCELLED");
            Gtk::MessageDialog msgd (msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.set_title(M("TP_DARKFRAME_LABEL"));
            msgd.run ();
        }
    } else if (m == autoFF) {
        if (!mselected.empty() && bppcl) {
            bppcl->beginBatchPParamsChange(mselected.size());
        }

        for (size_t i = 0; i < mselected.size(); i++) {
            rtengine::procparams::ProcParams pp = mselected[i]->thumbnail->getProcParams();
            pp.raw.ff_AutoSelect = true;
            pp.raw.ff_file.clear();
            mselected[i]->thumbnail->setProcParams(pp, nullptr, FILEBROWSER, false);
        }

        if (!mselected.empty() && bppcl) {
            bppcl->endBatchPParamsChange();
        }
    } else if (m == selectFF) {
        if( !mselected.empty() ) {
            rtengine::procparams::ProcParams pp = mselected[0]->thumbnail->getProcParams();
            Gtk::FileChooserDialog fc (getToplevelWindow (this), "Flat Field", Gtk::FILE_CHOOSER_ACTION_OPEN );
            bindCurrentFolder (fc, App::get().mut_options().lastFlatfieldDir);
            fc.add_button( M("GENERAL_CANCEL"), Gtk::RESPONSE_CANCEL);
            fc.add_button( M("GENERAL_APPLY"), Gtk::RESPONSE_APPLY);

            if(!pp.raw.ff_file.empty()) {
                fc.set_filename( pp.raw.ff_file );
            }

            if( fc.run() == Gtk::RESPONSE_APPLY ) {
                if (bppcl) {
                    bppcl->beginBatchPParamsChange(mselected.size());
                }

                for (size_t i = 0; i < mselected.size(); i++) {
                    rtengine::procparams::ProcParams lpp = mselected[i]->thumbnail->getProcParams();
                    lpp.raw.ff_file = fc.get_filename();
                    lpp.raw.ff_AutoSelect = false;
                    mselected[i]->thumbnail->setProcParams(lpp, nullptr, FILEBROWSER, false);
                }

                if (bppcl) {
                    bppcl->endBatchPParamsChange();
                }
            }
        }
    } else if( m == thisIsFF) {
        if( !options.rtSettings.flatFieldsPath.empty()) {
            if (Gio::File::create_for_path(options.rtSettings.flatFieldsPath)->query_exists() ) {
                for (size_t i = 0; i < mselected.size(); i++) {
                    Glib::RefPtr<Gio::File> file = Gio::File::create_for_path ( mselected[i]->filename );

                    if( !file ) {
                        continue;
                    }

                    Glib::ustring destName = options.rtSettings.flatFieldsPath + "/" + file->get_basename();
                    Glib::RefPtr<Gio::File> dest = Gio::File::create_for_path ( destName );
                    file->move(  dest );
                }

                // Reinit cache
                rtengine::ffm.init( options.rtSettings.flatFieldsPath );
            } else {
                // Target directory creation failed, we clear the flatFieldsPath setting
                App::get().mut_options().rtSettings.flatFieldsPath.clear();
                Glib::ustring msg_ = Glib::ustring::compose (M("MAIN_MSG_PATHDOESNTEXIST"), escapeHtmlChars(options.rtSettings.flatFieldsPath))
                                     + "\n\n" + M("MAIN_MSG_OPERATIONCANCELLED");
                Gtk::MessageDialog msgd (msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
                msgd.set_title(M("TP_FLATFIELD_LABEL"));
                msgd.run ();
            }
        } else {
            Glib::ustring msg_ = M("MAIN_MSG_SETPATHFIRST") + "\n\n" + M("MAIN_MSG_OPERATIONCANCELLED");
            Gtk::MessageDialog msgd (msg_, true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
            msgd.set_title(M("TP_FLATFIELD_LABEL"));
            msgd.run ();
        }
    } else if (m == copyprof) {
        copyProfile ();
    } else if (m == pasteprof) {
        pasteProfile ();
    } else if (m == partpasteprof) {
        partPasteProfile ();
    } else if (m == clearprof) {
        for (size_t i = 0; i < mselected.size(); i++) {
            mselected[i]->thumbnail->clearProcParams (FILEBROWSER);
        }

        queue_draw ();
    } else if (m == resetdefaultprof) {
        if (!mselected.empty() && bppcl) {
            bppcl->beginBatchPParamsChange(mselected.size());
        }

        for (size_t i = 0; i < mselected.size(); i++)  {
            const auto thumbnail = mselected[i]->thumbnail;
            const auto rank = thumbnail->getRank();
            const auto colorLabel = thumbnail->getColorLabel();
            const auto stage = thumbnail->getTrashed();

            thumbnail->createProcParamsForUpdate (false, true);
            thumbnail->setRank(rank);
            thumbnail->setColorLabel(colorLabel);
            thumbnail->setTrashed(stage);

            // Empty run to update the thumb
            rtengine::procparams::ProcParams params = thumbnail->getProcParams ();
            thumbnail->setProcParams (params, nullptr, FILEBROWSER, true, true);
        }

        if (!mselected.empty() && bppcl) {
            bppcl->endBatchPParamsChange();
        }
    } else if (m == clearFromCache) {
        tbl->clearFromCacheRequested (mselected, false);

        //queue_draw ();
    } else if (m == clearFromCacheFull) {
        tbl->clearFromCacheRequested (mselected, true);

        //queue_draw ();
    } else if (m == aiDenoise) {
        // Enable AI Denoise on all selected photos
        if (!mselected.empty() && bppcl) {
            bppcl->beginBatchPParamsChange(mselected.size());
        }

        for (size_t i = 0; i < mselected.size(); i++) {
            rtengine::procparams::ProcParams pp = mselected[i]->thumbnail->getProcParams();
            pp.aiDenoise.enabled = true;
            mselected[i]->thumbnail->setProcParams(pp, nullptr, FILEBROWSER, false);
        }

        if (!mselected.empty() && bppcl) {
            bppcl->endBatchPParamsChange();
        }
    } else if (m == autoEditMenu || m == autoGrade || m == autoGradeFilm || m == autoGradedFilm) {
        cancelAutoEditHoverPreview(nullptr, false);
        if (quickActionRunning_) {
            return;
        }

        auto state = std::make_shared<AutoEditBatchState>();
        state->mode = m == autoGradedFilm
            ? AutoEditMode::GradedFilm
            : m == autoGradeFilm
             ? AutoEditMode::GradeFilm
             : m == autoGrade
              ? AutoEditMode::Grade
              : AutoEditMode::Neutral;
        state->thumbnails.reserve(mselected.size());
        for (auto* entry : mselected) {
            entry->thumbnail->increaseRef();
            state->thumbnails.push_back(entry->thumbnail);
        }

        if (state->thumbnails.empty()) {
            return;
        }

        fileBrowserPerfLog(
            "[autoEditBatch] start count=%zu mode=%s\n",
            state->thumbnails.size(),
            autoEditModeName(state->mode));

        quickActionRunning_ = true;
        autoEditMenu->set_sensitive(false);
        autoLevel->set_sensitive(false);
        tbl->quickActionProgress(M(autoEditModeLabel(state->mode)), 0.01);
        if (bppcl) {
            bppcl->beginBatchPParamsChange(state->thumbnails.size());
        }

        auto finishBatch = std::make_shared<std::function<void()>>();
        *finishBatch = [this, state]() {
            if (state->finalized
                    || !state->analysisFinished.load(std::memory_order_acquire)
                    || state->applied < state->thumbnails.size()) {
                return;
            }

            state->finalized = true;
            if (bppcl) {
                bppcl->endBatchPParamsChange();
            }
            quickActionRunning_ = false;
            autoEditMenu->set_sensitive(true);
            autoLevel->set_sensitive(true);
            tbl->quickActionProgress(
                Glib::ustring::compose(
                    "%1: %2/%3",
                    M(autoEditModeLabel(state->mode)),
                    state->thumbnails.size(),
                    state->thumbnails.size()),
                0.0);
            queue_draw();
        };

        // RAW analysis can require upgrading an embedded preview. Keep that
        // work off GTK's thread, then apply each completed recipe on GTK's
        // thread so listeners and thumbnail drawing remain correctly owned.
        autoEditHoverPool_->push([this, state, finishBatch]() {
            for (size_t index = 0; index < state->thumbnails.size(); ++index) {
                auto* thm = state->thumbnails[index];
                AutoGradeFeatures features;
                rtengine::procparams::ProcParams pp;
                bool succeeded = true;
                std::string error;

                try {
                    const rtengine::procparams::ProcParams sourceParams = thm->getProcParams();
                    features = buildSteepAutoEditParamsInternal(*thm, state->mode, sourceParams, pp);
                } catch (const std::exception& exception) {
                    succeeded = false;
                    error = exception.what();
                    pp = thm->getProcParams();
                } catch (...) {
                    succeeded = false;
                    error = "unknown error";
                    pp = thm->getProcParams();
                }

                idle_register.add([this, state, finishBatch, thm, features, pp = std::move(pp), succeeded, error = std::move(error)]() mutable -> bool {
                    if (!succeeded) {
                        fileBrowserPerfLog(
                            "[autoEditBatch] failed mode=%s error=%s file=%s\n",
                            autoEditModeName(state->mode),
                            error.c_str(),
                            thm->getFileName().c_str());
                    } else {
                        fileBrowserPerfLog(
                            "[autoEditBatch] mode=%s scene=%s median=%.3f range=%.3f sat=%.3f skin=%.3f skinSat=%.3f sky=%.3f foliage=%.3f edge=%.3f iso=%u exposure=%.3f brightness=%d contrast=%d highlight=%d film=%s strength=%d file=%s\n",
                            autoEditModeName(state->mode),
                            autoGradeSceneName(features.scene),
                            features.medianLuma,
                            features.dynamicRange,
                            features.saturation,
                            features.skinFraction,
                            features.skinSaturation,
                            features.skyFraction,
                            features.foliageFraction,
                            features.edgeDensity,
                            features.iso,
                            pp.toneCurve.expcomp,
                            pp.toneCurve.brightness,
                            pp.toneCurve.contrast,
                            pp.toneCurve.hlcompr,
                            pp.filmPresets.enabled ? pp.filmPresets.preset.c_str() : "off",
                            pp.filmPresets.strength,
                            thm->getFileName().c_str());
                        thm->setProcParams(pp, nullptr, FILEBROWSER, true);
                    }

                    ++state->applied;
                    tbl->quickActionProgress(
                        Glib::ustring::compose(
                            "%1 %2/%3",
                            M(autoEditModeLabel(state->mode)),
                            state->applied,
                            state->thumbnails.size()),
                        std::max(0.01, static_cast<double>(state->applied) / state->thumbnails.size()));
                    (*finishBatch)();
                    return false;
                });
            }

            state->analysisFinished.store(true, std::memory_order_release);
            idle_register.add([finishBatch]() -> bool {
                (*finishBatch)();
                return false;
            });
        });
    } else if (m == autoLevel) {
        if (quickActionRunning_) {
            return;
        }

        auto state = std::make_shared<AutoLevelBatchState>();
        state->items.reserve(mselected.size());
        state->cancel = std::make_shared<std::atomic<bool>>(false);
        for (auto* entry : mselected) {
            auto* thumbnail = entry->thumbnail;
            thumbnail->increaseRef();
            state->items.push_back({
                thumbnail,
                thumbnail->getFileName(),
                thumbnail->getType() == FT_Raw,
                {}
            });
        }

        if (state->items.empty()) {
            return;
        }

        fileBrowserPerfLog("[autoLevelBatch] start count=%zu\n", state->items.size());

        quickActionRunning_ = true;
        autoEditMenu->set_sensitive(false);
        autoLevel->set_sensitive(false);
        autoLevelCancel_ = state->cancel;
        if (autoLevelThread_.joinable()) {
            autoLevelThread_.join();
        }

        // Copy parameters on GTK's thread in small slices before handing the
        // image-only analysis to a serial background worker.
        idle_register.add([this, state]() -> bool {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
            size_t prepared = 0;
            while (state->prepareIndex < state->items.size()
                    && (prepared == 0 || std::chrono::steady_clock::now() < deadline)
                    && prepared < 8) {
                auto& item = state->items[state->prepareIndex++];
                item.params = item.thumbnail->getProcParams();
                ++prepared;
            }

            if (state->prepareIndex < state->items.size()) {
                return true;
            }

            tbl->quickActionProgress(M("TP_ROTATE_AUTO_LEVEL"), 0.01);
            state->readyResults.reserve(state->items.size());
            autoLevelThread_ = std::thread([state]() {
                lowerQuickPreviewWarmThreadPriority();

                for (const auto& item : state->items) {
                    if (state->cancel->load(std::memory_order_acquire)) {
                        break;
                    }

                    try {
                        int errorCode = 0;
                        auto releaseInitialImage = [](rtengine::InitialImage* image) {
                            if (image) {
                                image->decreaseRef();
                            }
                        };
                        std::unique_ptr<rtengine::InitialImage, decltype(releaseInitialImage)> initialImage(
                            FilePanel::loadAuxiliaryInitialImage(
                                item.filename,
                                item.isRaw,
                                &errorCode,
                                state->cancel),
                            releaseInitialImage);
                        if (initialImage && errorCode == 0
                                && !state->cancel->load(std::memory_order_acquire)) {
                            auto* source = initialImage->getImageSource();
                            const auto prepareStart = QuickWarmClock::now();
                            prepareAutoLevelImageSource(source, item.params);
                            const auto detectStart = QuickWarmClock::now();
                            const auto detection = rtengine::PerspectiveCorrection::autoLevel(source, &item.params);
                            const auto detectEnd = QuickWarmClock::now();
                            fileBrowserPerfLog(
                                "[autoLevelBatch] raw=%d prepare=%lldms detect=%lldms success=%d angle=%.4f confidence=%.3f horizontal=%d vertical=%d file=%s\n",
                                source && source->isRAW() ? 1 : 0,
                                quickWarmDurationMs(prepareStart, detectStart),
                                quickWarmDurationMs(detectStart, detectEnd),
                                detection.success ? 1 : 0,
                                detection.angle,
                                detection.confidence,
                                detection.horizontal_lines,
                                detection.vertical_lines,
                                item.filename.c_str());
                            if (detection.success) {
                                if (std::abs(detection.angle) < 0.015) {
                                    state->alreadyLevel.fetch_add(1, std::memory_order_release);
                                } else {
                                    rtengine::procparams::ProcParams leveled = item.params;
                                    const double newRotation = leveled.rotate.degree + detection.angle;
                                    if (std::abs(newRotation) <= 45.0) {
                                        leveled.rotate.degree = newRotation;
                                        state->lastAppliedAngle.store(detection.angle, std::memory_order_release);
                                        std::lock_guard<std::mutex> lock(state->resultsMutex);
                                        state->readyResults.emplace_back(item.thumbnail, std::move(leveled));
                                    }
                                }
                            } else {
                                state->detectionFailures.fetch_add(1, std::memory_order_release);
                            }
                        } else if (!state->cancel->load(std::memory_order_acquire)) {
                            state->loadFailures.fetch_add(1, std::memory_order_release);
                        }
                    } catch (const std::exception& error) {
                        state->loadFailures.fetch_add(1, std::memory_order_release);
                        fileBrowserPerfLog("[autoLevelBatch] analysis failed: %s\n", error.what());
                    } catch (...) {
                        state->loadFailures.fetch_add(1, std::memory_order_release);
                        fileBrowserPerfLog("[autoLevelBatch] analysis failed with an unknown error\n");
                    }

                    state->analyzed.fetch_add(1, std::memory_order_release);
                }

                state->analysisFinished.store(true, std::memory_order_release);
            });

            autoLevelPollConnection_.disconnect();
            autoLevelPollConnection_ = Glib::signal_timeout().connect(
                [this, state]() -> bool {
                    std::vector<std::pair<Thumbnail*, rtengine::procparams::ProcParams>> completed;
                    {
                        std::lock_guard<std::mutex> lock(state->resultsMutex);
                        completed.swap(state->readyResults);
                    }

                    if (!completed.empty()) {
                        if (bppcl) {
                            bppcl->beginBatchPParamsChange(completed.size());
                        }
                        for (auto& result : completed) {
                            result.first->setProcParams(result.second, nullptr, FILEBROWSER, true);
                            fileBrowserPerfLog(
                                "[autoLevelBatch] applied degree=%.4f file=%s\n",
                                result.second.rotate.degree,
                                result.first->getFileName().c_str());
                        }
                        if (bppcl) {
                            bppcl->endBatchPParamsChange();
                        }
                        state->applied.fetch_add(completed.size(), std::memory_order_release);
                        queue_draw();
                    }

                    const size_t analyzed = state->analyzed.load(std::memory_order_acquire);
                    tbl->quickActionProgress(
                        Glib::ustring::compose(
                            "%1 %2/%3",
                            M("TP_ROTATE_AUTO_LEVEL"),
                            analyzed,
                            state->items.size()),
                        std::max(0.01, static_cast<double>(analyzed) / state->items.size()));

                    if (!state->analysisFinished.load(std::memory_order_acquire)) {
                        return true;
                    }

                    if (autoLevelThread_.joinable()) {
                        autoLevelThread_.join();
                    }

                    const size_t applied = state->applied.load(std::memory_order_acquire);
                    const size_t alreadyLevel = state->alreadyLevel.load(std::memory_order_acquire);
                    const size_t loadFailures = state->loadFailures.load(std::memory_order_acquire);
                    const Glib::ustring resultText = applied > 0
                        ? state->items.size() == 1
                          ? Glib::ustring::compose(
                              "%1: %2",
                              M("TP_ROTATE_AUTO_LEVEL"),
                              Glib::ustring::compose(
                                  M("TP_ROTATE_AUTO_LEVEL_APPLIED"),
                                  std::round(std::abs(state->lastAppliedAngle.load(std::memory_order_acquire)) * 1000.0) / 1000.0))
                          : Glib::ustring::compose(
                              "%1: %2/%3",
                              M("TP_ROTATE_AUTO_LEVEL"),
                              applied,
                              state->items.size())
                        : Glib::ustring::compose(
                            "%1: %2",
                            M("TP_ROTATE_AUTO_LEVEL"),
                            M(loadFailures == state->items.size()
                                ? "TP_ROTATE_AUTO_LEVEL_LOAD_FAILED"
                                : alreadyLevel > 0
                                 ? "TP_ROTATE_AUTO_LEVEL_ALREADY"
                                 : "TP_ROTATE_AUTO_LEVEL_FAILED"));
                    tbl->quickActionProgress(resultText, 0.0);
                    quickActionRunning_ = false;
                    autoEditMenu->set_sensitive(true);
                    autoLevel->set_sensitive(true);
                    autoLevelCancel_.reset();
                    return false;
                },
                40,
                G_PRIORITY_DEFAULT_IDLE);
            return false;
        });
    } else if (m == duplicate) {
        // Duplicate: copy each selected file with a _copy suffix
        for (size_t i = 0; i < mselected.size(); i++) {
            Glib::ustring srcPath = mselected[i]->filename;
            auto srcFile = Gio::File::create_for_path(srcPath);
            if (!srcFile || !srcFile->query_exists()) continue;

            // Build destination name: insert _copy before extension
            Glib::ustring dir = Glib::path_get_dirname(srcPath);
            Glib::ustring base = Glib::path_get_basename(srcPath);
            auto dotPos = base.rfind('.');
            Glib::ustring destName;
            if (dotPos != Glib::ustring::npos) {
                destName = base.substr(0, dotPos) + "_copy" + base.substr(dotPos);
            } else {
                destName = base + "_copy";
            }
            Glib::ustring destPath = Glib::build_filename(dir, destName);

            // Avoid overwriting: append _2, _3, etc.
            int suffix = 2;
            while (Gio::File::create_for_path(destPath)->query_exists()) {
                if (dotPos != Glib::ustring::npos) {
                    destName = base.substr(0, dotPos) + "_copy" + std::to_string(suffix) + base.substr(dotPos);
                } else {
                    destName = base + "_copy" + std::to_string(suffix);
                }
                destPath = Glib::build_filename(dir, destName);
                suffix++;
            }

            try {
                srcFile->copy(Gio::File::create_for_path(destPath));
                // Also copy the PP3 sidecar if it exists
                Glib::ustring pp3Src = srcPath + ".pp3";
                auto pp3File = Gio::File::create_for_path(pp3Src);
                if (pp3File->query_exists()) {
                    pp3File->copy(Gio::File::create_for_path(destPath + ".pp3"));
                }
            } catch (const Glib::Error&) {
                // Silently skip failed copies
            }
        }
        queue_draw ();
#ifdef _WIN32
    } else if (miOpenDefaultViewer && m == miOpenDefaultViewer) {
        openDefaultViewer(1);
#endif
    }
}

void FileBrowser::copyProfile ()
{
    MYREADERLOCK(l, entryRW);

    if (selected.size() == 1) {
        const auto& srcPP = (static_cast<FileBrowserEntry*>(selected[0]))->thumbnail->getProcParams();

        // Check if all visible filters are active (fast path: full copy)
        bool allActive = true;
        for (const auto& kv : copyFilters_) {
            if (!kv.second->get_active()) {
                allActive = false;
                break;
            }
        }

        if (allActive) {
            clipboard.setProcParams(srcPP);
        } else {
            // Build a ParamsEdited filter: start all-true, disable unchecked items
            ParamsEdited filterPE(true);
            filterPE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
            ParamsEdited falsePE;
            falsePE.locallab.spots.resize(srcPP.locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(false));

            // Always exclude general (rank/color labels)
            filterPE.general = falsePE.general;

            // Helper: check if a filter key is unchecked
            auto isOff = [this](const std::string& key) -> bool {
                auto it = copyFilters_.find(key);
                return it != copyFilters_.end() && !it->second->get_active();
            };

            // Basic
            if (isOff("wb"))            filterPE.wb = falsePE.wb;
            if (isOff("toneCurve"))     filterPE.toneCurve = falsePE.toneCurve;
            if (isOff("sh"))            filterPE.sh = falsePE.sh;
            if (isOff("toneEqualizer")) filterPE.toneEqualizer = falsePE.toneEqualizer;

            // Detail
            if (isOff("sharpening"))      filterPE.sharpening = falsePE.sharpening;
            if (isOff("sharpenEdge"))     filterPE.sharpenEdge = falsePE.sharpenEdge;
            if (isOff("sharpenMicro"))    filterPE.sharpenMicro = falsePE.sharpenMicro;
            if (isOff("impulseDenoise"))  filterPE.impulseDenoise = falsePE.impulseDenoise;
            if (isOff("dirpyrDenoise"))   filterPE.dirpyrDenoise = falsePE.dirpyrDenoise;
            if (isOff("defringe"))        filterPE.defringe = falsePE.defringe;
            if (isOff("dehaze"))          filterPE.dehaze = falsePE.dehaze;
            if (isOff("dirpyrequalizer")) filterPE.dirpyrequalizer = falsePE.dirpyrequalizer;

            // Color
            if (isOff("labCurve"))       filterPE.labCurve = falsePE.labCurve;
            if (isOff("rgbCurves"))      filterPE.rgbCurves = falsePE.rgbCurves;
            if (isOff("colorToning"))    filterPE.colorToning = falsePE.colorToning;
            if (isOff("chmixer"))        filterPE.chmixer = falsePE.chmixer;
            if (isOff("blackwhite"))     filterPE.blackwhite = falsePE.blackwhite;
            if (isOff("hsvequalizer"))   filterPE.hsvequalizer = falsePE.hsvequalizer;
            if (isOff("filmSimulation")) filterPE.filmSimulation = falsePE.filmSimulation;
            if (isOff("softlight"))      filterPE.softlight = falsePE.softlight;
            if (isOff("vibrance"))       filterPE.vibrance = falsePE.vibrance;

            // Lens
            if (isOff("distortion"))   filterPE.distortion = falsePE.distortion;
            if (isOff("cacorrection")) filterPE.cacorrection = falsePE.cacorrection;
            if (isOff("vignetting"))   filterPE.vignetting = falsePE.vignetting;
            if (isOff("lensProf"))     filterPE.lensProf = falsePE.lensProf;

            // Composition
            if (isOff("coarse"))       filterPE.coarse = falsePE.coarse;
            if (isOff("rotate"))       filterPE.rotate = falsePE.rotate;
            if (isOff("crop"))         filterPE.crop = falsePE.crop;
            if (isOff("resize"))       filterPE.resize = falsePE.resize;
            if (isOff("prsharpening")) filterPE.prsharpening = falsePE.prsharpening;
            if (isOff("perspective"))  filterPE.perspective = falsePE.perspective;
            if (isOff("commonTrans"))  filterPE.commonTrans = falsePE.commonTrans;
            if (isOff("gradient"))     filterPE.gradient = falsePE.gradient;
            if (isOff("framing"))      filterPE.framing = falsePE.framing;

            // Advanced
            if (isOff("retinex")) filterPE.retinex = falsePE.retinex;
            if (isOff("wavelet")) filterPE.wavelet = falsePE.wavelet;
            if (isOff("spot"))    filterPE.spot = falsePE.spot;
            if (isOff("cg"))      filterPE.cg = falsePE.cg;

            // Selective Editing
            if (isOff("locallab")) filterPE.locallab = falsePE.locallab;

            // Apply filter: combine only selected settings from source
            rtengine::procparams::ProcParams filteredPP;
            filterPE.combine(filteredPP, srcPP, true);

            rtengine::procparams::PartialProfile pp(&filteredPP, &filterPE);
            clipboard.setPartialProfile(pp);
        }
    }
}

void FileBrowser::pasteProfile ()
{

    if (clipboard.hasProcParams()) {
        std::vector<FileBrowserEntry*> mselected;
        {
            MYREADERLOCK(l, entryRW);

            for (unsigned int i = 0; i < selected.size(); i++) {
                mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
            }
        }

        if (!tbl || mselected.empty()) {
            return;
        }

        if (!mselected.empty() && bppcl) {
            bppcl->beginBatchPParamsChange(mselected.size());
        }

        for (unsigned int i = 0; i < mselected.size(); i++) {
            // copying read only clipboard PartialProfile to a temporary one
            const rtengine::procparams::PartialProfile& cbPartProf = clipboard.getPartialProfile();
            rtengine::procparams::PartialProfile pastedPartProf(cbPartProf.pparams, cbPartProf.pedited, true);

            // applying the PartialProfile to the thumb's ProcParams
            mselected[i]->thumbnail->setProcParams (*pastedPartProf.pparams, pastedPartProf.pedited, FILEBROWSER);
            pastedPartProf.deleteInstance();
        }

        if (!mselected.empty() && bppcl) {
            bppcl->endBatchPParamsChange();
        }

        queue_draw ();
    }
}

void FileBrowser::partPasteProfile ()
{

    if (clipboard.hasProcParams()) {

        std::vector<FileBrowserEntry*> mselected;
        {
            MYREADERLOCK(l, entryRW);

            for (unsigned int i = 0; i < selected.size(); i++) {
                mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
            }
        }

        if (!tbl || mselected.empty()) {
            return;
        }

        auto toplevel = static_cast<Gtk::Window*> (get_toplevel ());
        PartialPasteDlg partialPasteDlg (M("PARTIALPASTE_DIALOGLABEL"), toplevel);

        partialPasteDlg.updateSpotWidget(clipboard.getPartialProfile().pparams);

        int i = partialPasteDlg.run ();

        if (i == Gtk::RESPONSE_OK) {

            if (!mselected.empty() && bppcl) {
                bppcl->beginBatchPParamsChange(mselected.size());
            }

            for (auto entry : mselected) {
                // copying read only clipboard PartialProfile to a temporary one, initialized to the thumb's ProcParams
                entry->thumbnail->createProcParamsForUpdate(false, false); // this can execute customprofilebuilder to generate param file
                const rtengine::procparams::PartialProfile& cbPartProf = clipboard.getPartialProfile();
                rtengine::procparams::PartialProfile pastedPartProf(&entry->thumbnail->getProcParams (), nullptr);

                // pushing the selected values of the clipboard PartialProfile to the temporary PartialProfile
                partialPasteDlg.applyPaste (pastedPartProf.pparams, pastedPartProf.pedited, cbPartProf.pparams, cbPartProf.pedited);

                // applying the temporary PartialProfile to the thumb's ProcParams
                entry->thumbnail->setProcParams (*pastedPartProf.pparams, pastedPartProf.pedited, FILEBROWSER);
                pastedPartProf.deleteInstance();
            }

            if (!mselected.empty() && bppcl) {
                bppcl->endBatchPParamsChange();
            }

            queue_draw ();
        }

        partialPasteDlg.hide ();
    }
}

#ifdef _WIN32
void FileBrowser::openDefaultViewer (int destination)
{
    bool success = true;

    {
        MYREADERLOCK(l, entryRW);

        if (selected.size() == 1) {
            success = (static_cast<FileBrowserEntry*>(selected[0]))->thumbnail->openDefaultViewer(destination);
        }
    }

    if (!success) {
        Gtk::MessageDialog msgd(getToplevelWindow(this), M("MAIN_MSG_IMAGEUNPROCESSED"), true, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msgd.run ();
    }
}
#endif

bool FileBrowser::keyPressed (GdkEventKey* event)
{
    bool ctrl  = event->state & GDK_CONTROL_MASK;
    bool shift = event->state & GDK_SHIFT_MASK;
    bool alt   = event->state & GDK_MOD1_MASK;
#ifdef __WIN32__
    bool altgr = event->state & GDK_MOD2_MASK;
#endif

    if ((event->keyval == GDK_KEY_C || event->keyval == GDK_KEY_c) && ctrl && shift) {
        menuItemActivated (copyTo);
        return true;
    } else if ((event->keyval == GDK_KEY_M || event->keyval == GDK_KEY_m) && ctrl && shift) {
        menuItemActivated (moveTo);
        return true;
    } else if ((event->keyval == GDK_KEY_C || event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_Insert) && ctrl) {
        copyProfile ();
        return true;
    } else if ((event->keyval == GDK_KEY_V || event->keyval == GDK_KEY_v) && ctrl && !shift) {
        pasteProfile ();
        return true;
    } else if (event->keyval == GDK_KEY_Insert && shift) {
        pasteProfile ();
        return true;
    } else if ((event->keyval == GDK_KEY_V || event->keyval == GDK_KEY_v) && ctrl && shift) {
        partPasteProfile ();
        return true;
    } else if (event->keyval == GDK_KEY_Delete && !shift) {
        menuItemActivated (trash);
        return true;
    } else if (event->keyval == GDK_KEY_Delete && shift) {
        menuItemActivated (untrash);
        return true;
    } else if ((event->keyval == GDK_KEY_B || event->keyval == GDK_KEY_b) && ctrl && !shift) {
        menuItemActivated (develop);
        return true;
    } else if ((event->keyval == GDK_KEY_B || event->keyval == GDK_KEY_b) && ctrl && shift) {
        menuItemActivated (developfast);
        return true;
    } else if ((event->keyval == GDK_KEY_A || event->keyval == GDK_KEY_a) && ctrl) {
        menuItemActivated (selall);
        return true;
    } else if (event->keyval == GDK_KEY_F2 && !ctrl) {
        menuItemActivated (rename);
        return true;
    } else if (event->keyval == GDK_KEY_F3 && !(ctrl || shift || alt)) { // open Previous image from FileBrowser perspective
        FileBrowser::openPrevImage ();
        return true;
    } else if (event->keyval == GDK_KEY_F4 && !(ctrl || shift || alt)) { // open Next image from FileBrowser perspective
        FileBrowser::openNextImage ();
        return true;
    } else if (event->keyval == GDK_KEY_Left) {
        selectPrev (1, shift);
        return true;
    } else if (event->keyval == GDK_KEY_Right) {
        selectNext (1, shift);
        return true;
    } else if (event->keyval == GDK_KEY_Up) {
        selectPrev (numOfCols, shift);
        return true;
    } else if (event->keyval == GDK_KEY_Down) {
        selectNext (numOfCols, shift);
        return true;
    } else if (event->keyval == GDK_KEY_Home) {
        selectFirst (shift);
        return true;
    } else if (event->keyval == GDK_KEY_End) {
        selectLast (shift);
        return true;
    } else if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        std::vector<FileBrowserEntry*> mselected;

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }

        openRequested(mselected);
#ifdef _WIN32
    } else if (event->keyval == GDK_KEY_F5) {
        int dest = 1;

        if (event->state & GDK_SHIFT_MASK) {
            dest = 2;
        } else if (event->state & GDK_CONTROL_MASK) {
            dest = 3;
        }

        openDefaultViewer (dest);
        return true;
#endif
    } else if (event->keyval == GDK_KEY_Page_Up) {
        scrollPage(GDK_SCROLL_UP);
        return true;
    } else if (event->keyval == GDK_KEY_Page_Down) {
        scrollPage(GDK_SCROLL_DOWN);
        return true;
    }

#ifdef __WIN32__
    else if (!shift && !ctrl && !alt && !altgr) { // pick/reject flags
        switch(event->keyval) {
        case GDK_KEY_p:
        case GDK_KEY_P:
            requestPick (1);
            return true;
        case GDK_KEY_u:
        case GDK_KEY_U:
            requestPick (0);
            return true;
        case GDK_KEY_x:
        case GDK_KEY_X:
            requestPick (-1);
            return true;
        }
    }

    if (!shift && !ctrl && !alt && !altgr) { // rank
        switch(event->hardware_keycode) {
        case 0x30:  // 0-key
            requestRanking (0);
            return true;

        case 0x31:  // 1-key
            requestRanking (1);
            return true;

        case 0x32:  // 2-key
            requestRanking (2);
            return true;

        case 0x33:  // 3-key
            requestRanking (3);
            return true;

        case 0x34:  // 4-key
            requestRanking (4);
            return true;

        case 0x35:  // 5-key
            requestRanking (5);
            return true;
        }
    } else if (shift && ctrl && !alt && !altgr) { // color labels
        switch(event->hardware_keycode) {
        case 0x30:  // 0-key
            requestColorLabel (0);
            return true;

        case 0x31:  // 1-key
            requestColorLabel (1);
            return true;

        case 0x32:  // 2-key
            requestColorLabel (2);
            return true;

        case 0x33:  // 3-key
            requestColorLabel (3);
            return true;

        case 0x34:  // 4-key
            requestColorLabel (4);
            return true;

        case 0x35:  // 5-key
            requestColorLabel (5);
            return true;
        }
    }

#else
    else if (!shift && !ctrl && !alt) { // pick/reject flags
        switch(event->keyval) {
        case GDK_KEY_p:
        case GDK_KEY_P:
            requestPick (1);
            return true;
        case GDK_KEY_u:
        case GDK_KEY_U:
            requestPick (0);
            return true;
        case GDK_KEY_x:
        case GDK_KEY_X:
            requestPick (-1);
            return true;
        }
    }

    if (!shift && !ctrl && !alt) { // rank
        switch(event->hardware_keycode) {
        case 0x13:
            requestRanking (0);
            return true;

        case 0x0a:
            requestRanking (1);
            return true;

        case 0x0b:
            requestRanking (2);
            return true;

        case 0x0c:
            requestRanking (3);
            return true;

        case 0x0d:
            requestRanking (4);
            return true;

        case 0x0e:
            requestRanking (5);
            return true;
        }
    } else if (shift && ctrl && !alt) { // color labels
        switch(event->hardware_keycode) {
        case 0x13:
            requestColorLabel (0);
            return true;

        case 0x0a:
            requestColorLabel (1);
            return true;

        case 0x0b:
            requestColorLabel (2);
            return true;

        case 0x0c:
            requestColorLabel (3);
            return true;

        case 0x0d:
            requestColorLabel (4);
            return true;

        case 0x0e:
            requestColorLabel (5);
            return true;
        }
    }

#endif

    return false;
}

void FileBrowser::saveThumbnailHeight (int height)
{
    auto& options = App::get().mut_options();
    if (!options.sameThumbSize && getLocation() == THLOC_EDITOR) {
        options.thumbSizeTab = height;
    } else {
        options.thumbSize = height;
    }
}

int FileBrowser::getThumbnailHeight ()
{
    const auto& options = App::get().options();
    // The user could have manually forced the option to a too big value
    if (!options.sameThumbSize && getLocation() == THLOC_EDITOR) {
        // Upper bound matches the filmstrip size slider's maximum
        return std::max(std::min(options.thumbSizeTab, 220), 10);
    } else {
        return std::max(std::min(options.thumbSize, 800), 10);
    }
}

void FileBrowser::enableTabMode(bool enable)
{
    ThumbBrowserBase::enableTabMode(enable);
    const auto& options = App::get().options();
    if (options.inspectorWindow) {
        if (enable) {
            inspect->remove_accelerator(pmenu->get_accel_group(), GDK_KEY_f, (Gdk::ModifierType)0);
        }
        else {
            inspect->add_accelerator ("activate", pmenu->get_accel_group(), GDK_KEY_f, (Gdk::ModifierType)0, Gtk::ACCEL_VISIBLE);
        }
    }
}

void FileBrowser::applyMenuItemActivated (ProfileStoreLabel *label)
{
    MYREADERLOCK(l, entryRW);

    const rtengine::procparams::PartialProfile* partProfile = ProfileStore::getInstance()->getProfile (label->entry);

    if (partProfile->pparams && !selected.empty()) {
        if (bppcl) {
            bppcl->beginBatchPParamsChange(selected.size());
        }

        for (size_t i = 0; i < selected.size(); i++) {
            (static_cast<FileBrowserEntry*>(selected[i]))->thumbnail->setProcParams (*partProfile->pparams, partProfile->pedited, FILEBROWSER);
        }

        if (bppcl) {
            bppcl->endBatchPParamsChange();
        }

        queue_draw ();
    }
}

void FileBrowser::applyPartialMenuItemActivated (ProfileStoreLabel *label)
{

    {
        MYREADERLOCK(l, entryRW);

        if (!tbl || selected.empty()) {
            return;
        }
    }

    const rtengine::procparams::PartialProfile* srcProfiles = ProfileStore::getInstance()->getProfile (label->entry);

    if (srcProfiles->pparams) {

        auto toplevel = static_cast<Gtk::Window*> (get_toplevel ());
        PartialPasteDlg partialPasteDlg (M("PARTIALPASTE_DIALOGLABEL"), toplevel);

        partialPasteDlg.updateSpotWidget(srcProfiles->pparams);

        if (partialPasteDlg.run() == Gtk::RESPONSE_OK) {

            MYREADERLOCK(l, entryRW);

            if (bppcl) {
                bppcl->beginBatchPParamsChange(selected.size());
            }

            for (size_t i = 0; i < selected.size(); i++) {
                selected[i]->thumbnail->createProcParamsForUpdate(false, false);  // this can execute customprofilebuilder to generate param file

                rtengine::procparams::PartialProfile dstProfile(true);
                *dstProfile.pparams = (static_cast<FileBrowserEntry*>(selected[i]))->thumbnail->getProcParams ();
                dstProfile.set(true);
                dstProfile.pedited->locallab.spots.resize(dstProfile.pparams->locallab.spots.size(), LocallabParamsEdited::LocallabSpotEdited(true));
                partialPasteDlg.applyPaste (dstProfile.pparams, dstProfile.pedited, srcProfiles->pparams, srcProfiles->pedited);
                (static_cast<FileBrowserEntry*>(selected[i]))->thumbnail->setProcParams (*dstProfile.pparams, dstProfile.pedited, FILEBROWSER);
                dstProfile.deleteInstance();
            }

            if (bppcl) {
                bppcl->endBatchPParamsChange();
            }

            queue_draw ();
        }

        partialPasteDlg.hide ();
    }
}

bool FileBrowser::applyPassThroughFilterFast (const BrowserFilter& filter)
{
    if (!filter.isPassThrough() || !filterPassThrough_) {
        return false;
    }

    this->filter = filter;
    filterPassThrough_ = true;
    if (!layoutPaused_) {
        flushPendingInserts_();
    }

    {
        MYWRITERLOCK(l, entryRW);
        numFiltered = static_cast<int>(fd.size());
    }

    tbl->filterApplied();
    redraw(nullptr, true);
    return true;
}

void FileBrowser::applyFilter (const BrowserFilter& filter)
{

    const bool wasShowingOriginal = this->filter.showOriginal;
    this->filter = filter;
    filterPassThrough_ = this->filter.isPassThrough();
    if (!layoutPaused_) {
        flushPendingInserts_();
    }

    // remove items not complying the filter from the selection
    bool selchanged = false;
    numFiltered = 0;
    std::vector<ThumbBrowserEntryBase*> newlyVisibleThumbnailEntries;
    {
        MYWRITERLOCK(l, entryRW);

        if (filter.showOriginal) {
            ensureOriginalFamiliesCurrent_();
            refreshAllOriginalFamilies_();
        } else if (wasShowingOriginal) {
            clearOriginalMarks_();
        }

        for (size_t i = 0; i < fd.size(); i++) {
            const bool wasFiltered = fd[i]->filtered;
            const bool passesFilter = filterPassThrough_ || checkFilter(fd[i]);
            fd[i]->filtered = !passesFilter;

            if (passesFilter) {
                numFiltered++;
                if (wasFiltered) {
                    newlyVisibleThumbnailEntries.push_back(fd[i]);
                }
            } else if (fd[i]->selected) {
                fd[i]->selected = false;
                std::vector<ThumbBrowserEntryBase*>::iterator j = std::find(selected.begin(), selected.end(), fd[i]);
                selected.erase(j);

                if (lastClicked == fd[i]) {
                    lastClicked = nullptr;
                }

                selchanged = true;
            }
        }
        if (selected.empty() || (anchor && std::find(selected.begin(), selected.end(), anchor) == selected.end())) {
            anchor = nullptr;
        }
    }

    if (selchanged) {
        notifySelectionListener ();
    }

    std::vector<ThumbImageUpdater::Request> newlyVisibleThumbnailRequests;
    newlyVisibleThumbnailRequests.reserve(newlyVisibleThumbnailEntries.size());
    for (auto* entry : newlyVisibleThumbnailEntries) {
        entry->appendQuickThumbnailJob(newlyVisibleThumbnailRequests);
    }

    if (!newlyVisibleThumbnailRequests.empty()) {
        thumbImageUpdater->addBatch(newlyVisibleThumbnailRequests);
    }

    tbl->filterApplied();
    redraw(nullptr, true);
}

bool FileBrowser::checkFilter (ThumbBrowserEntryBase* entryb) const   // true -> entry complies filter
{
    if (filterPassThrough_) {
        return true;
    }

    FileBrowserEntry* entry = static_cast<FileBrowserEntry*>(entryb);

    // Album whitelist filter: if active, only show files in the album
    if (!filter.albumWhitelist.empty()) {
        const std::string& entryKey = entry->getBrowserPathKey();
        const bool inAlbum = entryKey.empty()
            ? filter.albumWhitelist.find(browserPathKey(entry->filename)) != filter.albumWhitelist.end()
            : filter.albumWhitelist.find(entryKey) != filter.albumWhitelist.end();

        if (!inAlbum) {
            return false;
        }
    }

    if (filter.showOriginal && entry->getOriginal()) {
        return false;
    }

    // return false if pick filter settings are not satisfied
    {
        int pick = entry->thumbnail->getPick();
        if (filter.hideRejects && pick == -1) {
            return false;
        }
        if ((pick == 1 && !filter.showPicked) ||
            (pick == -1 && !filter.showRejected) ||
            (pick == 0 && !filter.showUnflagged)) {
            return false;
        }
    }

    // return false if basic filter settings are not satisfied
    if ((!filter.showRanked[entry->thumbnail->getRank()] ) ||
            (!filter.showCLabeled[entry->thumbnail->getColorLabel()] ) ||

            ((entry->thumbnail->hasProcParams() && filter.showEdited[0]) && !filter.showEdited[1]) ||
            ((!entry->thumbnail->hasProcParams() && filter.showEdited[1]) && !filter.showEdited[0]) ||

            ((entry->thumbnail->isRecentlySaved() && filter.showRecentlySaved[0]) && !filter.showRecentlySaved[1]) ||
            ((!entry->thumbnail->isRecentlySaved() && filter.showRecentlySaved[1]) && !filter.showRecentlySaved[0]) ||

            (entry->thumbnail->getTrashed() && !filter.showTrash) ||
            (!entry->thumbnail->getTrashed() && !filter.showNotTrash)) {
        return false;
    }

    // Filetype filter from filter bar dropdown
    if (!filter.filetypeFilter.empty()) {
        const CacheImageData* cfs = entry->thumbnail->getCacheImageData();
        if (filter.filetypeFilter.find(cfs->getFiletypeUpper()) == filter.filetypeFilter.end()) {
            return false;
        }
    }

    // return false if query is not satisfied
    if (!filter.vFilterStrings.empty()) {
        // check if image's FileName contains queryFileName (case insensitive)
        // TODO should we provide case-sensitive search option via preferences?
        const std::string& FileName = entry->getBrowserFileNameUpper();
        int iFilenameMatch = 0;

        for (const auto& filterString : filter.vFilterStrings) {
            if (FileName.find(filterString) != std::string::npos) {
                ++iFilenameMatch;
                break;
            }
        }

        if (filter.matchEqual) {
            if (iFilenameMatch == 0) { //none of the vFilterStrings found in FileName
                return false;
            }
        } else {
            if (iFilenameMatch > 0) { // match is found for at least one of vFilterStrings in FileName
                return false;
            }
        }
    }

    if (!filter.exifFilterEnabled) {
        return true;
    }

    // check exif filter
    const CacheImageData* cfs = entry->thumbnail->getCacheImageData();
    double tol = 0.01;
    double tol2 = 1e-8;

    if (!cfs->exifValid) {
        return (!filter.exifFilter.filterCamera || filter.exifFilter.cameras.count(cfs->getCameraName()) > 0)
               && (!filter.exifFilter.filterLens || filter.exifFilter.lenses.count(cfs->getLensRaw()) > 0)
               && (!filter.exifFilter.filterFiletype || filter.exifFilter.filetypes.count(cfs->getFiletypeRaw()) > 0)
               && (!filter.exifFilter.filterExpComp || filter.exifFilter.expcomp.count(cfs->getExpCompRaw()) > 0);
    }

    if (filter.exifFilter.filterShutter) {
        const double shutter = rtengine::FramesMetaData::shutterFromString(
            rtengine::FramesMetaData::shutterToString(cfs->shutter));
        if (shutter < filter.exifFilter.shutterFrom - tol2 || shutter > filter.exifFilter.shutterTo + tol2) {
            return false;
        }
    }

    if (filter.exifFilter.filterFNumber) {
        const double fnumber = rtengine::FramesMetaData::apertureFromString(
            rtengine::FramesMetaData::apertureToString(cfs->fnumber));
        if (fnumber < filter.exifFilter.fnumberFrom - tol2 || fnumber > filter.exifFilter.fnumberTo + tol2) {
            return false;
        }
    }

    return
        (!filter.exifFilter.filterFocalLen || (cfs->focalLen >= filter.exifFilter.focalFrom - tol && cfs->focalLen <= filter.exifFilter.focalTo + tol))
        && (!filter.exifFilter.filterISO     || (cfs->iso >= filter.exifFilter.isoFrom && cfs->iso <= filter.exifFilter.isoTo))
        && (!filter.exifFilter.filterExpComp || filter.exifFilter.expcomp.count(cfs->getExpCompRaw()) > 0)
        && (!filter.exifFilter.filterCamera  || filter.exifFilter.cameras.count(cfs->getCameraName()) > 0)
        && (!filter.exifFilter.filterLens    || filter.exifFilter.lenses.count(cfs->getLensRaw()) > 0)
        && (!filter.exifFilter.filterFiletype  || filter.exifFilter.filetypes.count(cfs->getFiletypeRaw()) > 0);
}

void FileBrowser::toTrashRequested (std::vector<FileBrowserEntry*> tbe)
{

    for (size_t i = 0; i < tbe.size(); i++) {
        // try to load the last saved parameters from the cache or from the paramfile file
        tbe[i]->thumbnail->createProcParamsForUpdate(false, false, true);  // this can execute customprofilebuilder to generate param file in "flagging" mode

        // no need to notify listeners as item goes to trash, likely to be deleted

        if (tbe[i]->thumbnail->getTrashed()) {
            continue;
        }

        tbe[i]->thumbnail->setTrashed (true);

        if (tbe[i]->getThumbButtonSet()) {
            tbe[i]->getThumbButtonSet()->setRank (tbe[i]->thumbnail->getRank());
            tbe[i]->getThumbButtonSet()->setColorLabel (tbe[i]->thumbnail->getColorLabel());
            tbe[i]->thumbnail->updateCache (); // needed to save the colorlabel to disk in the procparam file(s) and the cache image data file
        }
    }

    trash_changed().emit();
    applyFilter (filter);
}

void FileBrowser::fromTrashRequested (std::vector<FileBrowserEntry*> tbe)
{

    for (size_t i = 0; i < tbe.size(); i++) {
        // if thumbnail was marked inTrash=true then param file must be there, no need to run customprofilebuilder

        if (!tbe[i]->thumbnail->getTrashed()) {
            continue;
        }

        tbe[i]->thumbnail->setTrashed (false);

        if (tbe[i]->getThumbButtonSet()) {
            tbe[i]->getThumbButtonSet()->setRank (tbe[i]->thumbnail->getRank());
            tbe[i]->getThumbButtonSet()->setColorLabel (tbe[i]->thumbnail->getColorLabel());
            tbe[i]->thumbnail->updateCache (); // needed to save the colorlabel to disk in the procparam file(s) and the cache image data file
        }
    }

    trash_changed().emit();
    applyFilter (filter);
}

void FileBrowser::sortMethodRequested (int method)
{
    auto& options = App::get().mut_options();
    options.sortMethod = Options::SortMethod(method);
    resort ();
}

void FileBrowser::sortOrderRequested (int order)
{
    auto& options = App::get().mut_options();
    options.sortDescending = !!order;
    resort ();
}

void FileBrowser::queueThumbnailPersist (Thumbnail* thm)
{
    thm->increaseRef();
    persistQueue_.push_back(thm);

    if (persistConn_.connected()) {
        return;
    }

    persistConn_ = Glib::signal_idle().connect(
        [this]() -> bool {
            int budget = 4; // disk writes per idle tick
            while (budget-- > 0 && !persistQueue_.empty()) {
                Thumbnail* thm = persistQueue_.front();
                persistQueue_.pop_front();
                thm->updateCache();
                thm->decreaseRef();
            }
            return !persistQueue_.empty();
        },
        G_PRIORITY_LOW);
}

void FileBrowser::rankingRequested (std::vector<FileBrowserEntry*> tbe, int rank)
{

    if (!tbe.empty() && bppcl) {
        bppcl->beginBatchPParamsChange(tbe.size());
    }

    for (size_t i = 0; i < tbe.size(); i++) {

        // try to load the last saved parameters from the cache or from the paramfile file
        tbe[i]->thumbnail->createProcParamsForUpdate(false, false, true);  // this can execute customprofilebuilder to generate param file in "flagging" mode

        // notify listeners TODO: should do this ONLY when params changed by customprofilebuilder?
        tbe[i]->thumbnail->notifylisterners_procParamsChanged(FILEBROWSER);

        tbe[i]->thumbnail->setRank (rank);
        // Persistence is deferred to idle chunks: the rank applies (and
        // renders) instantly even for large selections.
        queueThumbnailPersist (tbe[i]->thumbnail);

        auto* thumbButtonSet = rank > 0 ? tbe[i]->ensureThumbButtonSet(this) : tbe[i]->getThumbButtonSet();
        if (thumbButtonSet) {
            thumbButtonSet->setRank (tbe[i]->thumbnail->getRank());
        }

        // Trigger overlay animation (all entries animate in parallel)
        tbe[i]->startRatingAnimation();
    }

    applyFilter (filter);

    if (!tbe.empty() && bppcl) {
        bppcl->endBatchPParamsChange();
    }
}

void FileBrowser::colorlabelRequested (std::vector<FileBrowserEntry*> tbe, int colorlabel)
{

    if (!tbe.empty() && bppcl) {
        bppcl->beginBatchPParamsChange(tbe.size());
    }

    for (size_t i = 0; i < tbe.size(); i++) {
        // try to load the last saved parameters from the cache or from the paramfile file
        tbe[i]->thumbnail->createProcParamsForUpdate(false, false, true);  // this can execute customprofilebuilder to generate param file in "flagging" mode

        // notify listeners TODO: should do this ONLY when params changed by customprofilebuilder?
        tbe[i]->thumbnail->notifylisterners_procParamsChanged(FILEBROWSER);

        tbe[i]->thumbnail->setColorLabel (colorlabel);
        queueThumbnailPersist (tbe[i]->thumbnail);
        auto* thumbButtonSet = colorlabel > 0 ? tbe[i]->ensureThumbButtonSet(this) : tbe[i]->getThumbButtonSet();
        if (thumbButtonSet) {
            thumbButtonSet->setColorLabel (tbe[i]->thumbnail->getColorLabel());
        }

        // Trigger overlay animation (all entries animate in parallel)
        tbe[i]->startColorLabelAnimation();
    }

    applyFilter (filter);

    if (!tbe.empty() && bppcl) {
        bppcl->endBatchPParamsChange();
    }
}

void FileBrowser::requestRanking(int rank)
{
    std::vector<FileBrowserEntry*> mselected;
    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }
    }

    rankingRequested (mselected, rank);
}

void FileBrowser::requestColorLabel(int colorlabel)
{
    std::vector<FileBrowserEntry*> mselected;
    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }
    }

    colorlabelRequested (mselected, colorlabel);
}

std::vector<FileBrowserEntry*> FileBrowser::getRejectedEntries ()
{
    std::vector<FileBrowserEntry*> rejects;
    MYREADERLOCK(l, entryRW);

    for (auto* entry : fd) {
        auto* fileEntry = static_cast<FileBrowserEntry*>(entry);
        if (fileEntry->thumbnail && fileEntry->thumbnail->getPick() == -1) {
            rejects.push_back(fileEntry);
        }
    }

    return rejects;
}

namespace {

// Auto-cull verdict from the same scene statistics the auto-edit uses.
// Tolerances are 0-100; higher culls more aggressively. The verdicts are
// deliberately conservative: only blatant garbage should ever be flagged.
bool shouldAutoCull (const AutoGradeFeatures& f, int focusTolerance, int exposureTolerance)
{
    if (!f.valid) {
        return false;
    }

    const double focusT = focusTolerance / 100.0;
    const double exposureT = exposureTolerance / 100.0;

    // Blatant failures regardless of tolerance: essentially uniform black or
    // white frames (lens cap, misfire, blown test shot).
    if (f.dynamicRange < 0.045 && (f.medianLuma < 0.04 || f.medianLuma > 0.96)) {
        return true;
    }

    // Out of focus. Mean gradient conflates smooth composition with blur —
    // a tack-sharp portrait against bokeh has a LOW mean gradient — so the
    // primary signal is the strong-edge fraction: any in-focus image (even
    // a soft portrait) produces some hard local edges, while a defocused
    // frame has essentially none. Mean gradient stays only as a co-signal.
    const double strongEdgeThreshold = 0.00002 + 0.00034 * focusT;
    if (f.strongEdgeFraction < strongEdgeThreshold
            && f.edgeDensity < 0.008 + 0.010 * focusT) {
        return true;
    }

    // Overexposed: TRUE clipped whites dominating the frame. Bright high-key
    // and snow scenes have lots of bright pixels but few actual clips. At
    // default strictness well over half the frame must be genuinely blown.
    const double clipThreshold = 0.80 - 0.40 * exposureT;
    if (f.clippedFraction > clipThreshold) {
        return true;
    }

    // Severely underexposed with nothing bright anywhere and almost no
    // tonal range. Real night scenes keep specular lights (highlights) and
    // far more range than this.
    const double underThreshold = 0.005 + 0.018 * exposureT;
    if (f.medianLuma < underThreshold
            && f.highlightFraction < 0.002
            && f.dynamicRange < 0.10) {
        return true;
    }

    return false;
}

}

void FileBrowser::startAutoCull (int focusTolerance, int exposureTolerance)
{
    if (quickActionRunning_ || !tbl) {
        return;
    }

    struct AutoCullState {
        std::vector<Thumbnail*> thumbnails;      // refs held for the run
        std::shared_ptr<std::atomic<bool>> cancel;
        std::mutex resultsMutex;
        std::vector<Thumbnail*> readyRejects;
        std::atomic<size_t> analyzed{0};
        std::atomic<bool> analysisFinished{false};
    };

    auto state = std::make_shared<AutoCullState>();
    state->cancel = std::make_shared<std::atomic<bool>>(false);
    autoCullCancel_ = state->cancel;

    {
        MYREADERLOCK(l, entryRW);
        state->thumbnails.reserve(fd.size());
        for (auto* entry : fd) {
            auto* fileEntry = static_cast<FileBrowserEntry*>(entry);
            // Already-rejected images need no analysis
            if (fileEntry->thumbnail && fileEntry->thumbnail->getPick() != -1) {
                fileEntry->thumbnail->increaseRef();
                state->thumbnails.push_back(fileEntry->thumbnail);
            }
        }
    }

    if (state->thumbnails.empty()) {
        tbl->quickActionProgress(M("FILEBROWSER_AUTOCULL"), 0.0);
        return;
    }

    // A fresh run replaces the previous undo set
    for (auto& undo : autoCullUndo_) {
        undo.first->decreaseRef();
    }
    autoCullUndo_.clear();

    quickActionRunning_ = true;
    tbl->quickActionProgress(M("FILEBROWSER_AUTOCULL"), 0.01);

    if (autoCullThread_.joinable()) {
        autoCullThread_.join();
    }

    autoCullThread_ = std::thread([state, focusTolerance, exposureTolerance]() {
        for (auto* thm : state->thumbnails) {
            if (state->cancel->load(std::memory_order_acquire)) {
                break;
            }

            try {
                const AutoGradeFeatures features = analyzeSteepAutoGrade(*thm);
                if (shouldAutoCull(features, focusTolerance, exposureTolerance)) {
                    std::lock_guard<std::mutex> lock(state->resultsMutex);
                    state->readyRejects.push_back(thm);
                }
            } catch (...) {
                fileBrowserPerfLog("[autoCull] analysis failed for %s\n", thm->getFileName().c_str());
            }

            state->analyzed.fetch_add(1, std::memory_order_release);
        }

        state->analysisFinished.store(true, std::memory_order_release);
    });

    autoCullPollConnection_.disconnect();
    autoCullPollConnection_ = Glib::signal_timeout().connect(
        [this, state]() -> bool {
            std::vector<Thumbnail*> culled;
            {
                std::lock_guard<std::mutex> lock(state->resultsMutex);
                culled.swap(state->readyRejects);
            }

            if (!culled.empty()) {
                if (bppcl) {
                    bppcl->beginBatchPParamsChange(culled.size());
                }
                for (auto* thm : culled) {
                    // Record previous state for undo; the undo list owns a ref
                    thm->increaseRef();
                    autoCullUndo_.emplace_back(thm, thm->getPick());
                    thm->setPick(-1);
                    thm->updateCache();
                }
                if (bppcl) {
                    bppcl->endBatchPParamsChange();
                }
                queue_draw();
            }

            const size_t analyzed = state->analyzed.load(std::memory_order_acquire);
            tbl->quickActionProgress(
                Glib::ustring::compose(
                    "%1 %2/%3",
                    M("FILEBROWSER_AUTOCULL"),
                    analyzed,
                    state->thumbnails.size()),
                std::max(0.01, static_cast<double>(analyzed) / state->thumbnails.size()));

            if (!state->analysisFinished.load(std::memory_order_acquire)) {
                return true;
            }

            if (autoCullThread_.joinable()) {
                autoCullThread_.join();
            }

            for (auto* thm : state->thumbnails) {
                thm->decreaseRef();
            }
            state->thumbnails.clear();

            tbl->quickActionProgress(
                Glib::ustring::compose(
                    "%1: %2",
                    M("FILEBROWSER_AUTOCULL"),
                    Glib::ustring::compose(M("FILEBROWSER_AUTOCULL_RESULT"), autoCullUndo_.size())),
                0.0);
            quickActionRunning_ = false;

            // Re-apply so a standing hide-rejects preference takes effect
            applyFilter(filter);
            queue_draw();
            return false;
        },
        60,
        G_PRIORITY_DEFAULT_IDLE);
}

void FileBrowser::undoAutoCull ()
{
    if (autoCullUndo_.empty()) {
        return;
    }

    if (bppcl) {
        bppcl->beginBatchPParamsChange(autoCullUndo_.size());
    }
    for (auto& undo : autoCullUndo_) {
        undo.first->setPick(undo.second);
        undo.first->updateCache();
        undo.first->decreaseRef();
    }
    if (bppcl) {
        bppcl->endBatchPParamsChange();
    }

    const size_t restored = autoCullUndo_.size();
    autoCullUndo_.clear();

    if (tbl) {
        tbl->quickActionProgress(
            Glib::ustring::compose(
                "%1: %2",
                M("FILEBROWSER_AUTOCULL_UNDO"),
                Glib::ustring::compose(M("FILEBROWSER_AUTOCULL_UNDONE"), restored)),
            0.0);
    }

    applyFilter(filter);
    queue_draw();
}

void FileBrowser::pickRequested (std::vector<FileBrowserEntry*> tbe, int pick)
{
    if (!tbe.empty() && bppcl) {
        bppcl->beginBatchPParamsChange(tbe.size());
    }

    for (size_t i = 0; i < tbe.size(); i++) {
        tbe[i]->thumbnail->createProcParamsForUpdate(false, false, true);
        tbe[i]->thumbnail->notifylisterners_procParamsChanged(FILEBROWSER);
        tbe[i]->thumbnail->setPick (pick);
        queueThumbnailPersist (tbe[i]->thumbnail);
        tbe[i]->startPickAnimation();
    }

    applyFilter (filter);

    if (!tbe.empty() && bppcl) {
        bppcl->endBatchPParamsChange();
    }
}

void FileBrowser::requestPick(int pick)
{
    std::vector<FileBrowserEntry*> mselected;
    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }
    }

    pickRequested (mselected, pick);
}

void FileBrowser::requestDevelop()
{
    std::vector<FileBrowserEntry*> mselected;
    {
        MYREADERLOCK(l, entryRW);

        for (size_t i = 0; i < selected.size(); i++) {
            mselected.push_back (static_cast<FileBrowserEntry*>(selected[i]));
        }
    }

    if (tbl) {
        tbl->developRequested (mselected, false);
    }
}

void FileBrowser::buttonPressed (LWButton* button, int actionCode, void* actionData)
{

    if (actionCode >= 0 && actionCode <= 5) { // rank
        std::vector<FileBrowserEntry*> tbe;
        tbe.push_back (static_cast<FileBrowserEntry*>(actionData));
        rankingRequested (tbe, actionCode);
    } else if (actionCode == 6 && tbl) { // to processing queue
        std::vector<FileBrowserEntry*> tbe;
        tbe.push_back (static_cast<FileBrowserEntry*>(actionData));
        tbl->developRequested (tbe, false); // not a fast, but a FULL mode
    } else if (actionCode == 7) { // to trash / undelete
        std::vector<FileBrowserEntry*> tbe;
        FileBrowserEntry* entry = static_cast<FileBrowserEntry*>(actionData);
        tbe.push_back (entry);

        if (!entry->thumbnail->getTrashed()) {
            toTrashRequested (tbe);
        } else {
            fromTrashRequested (tbe);
        }
    } else if (actionCode == 8 && tbl) { // color label
        // show popup menu
        colorLabel_actionData = actionData;// this will be reused when pmenuColorLabels is clicked
        pmenuColorLabels->popup (3, this->eventTime);
    }
}

void FileBrowser::openNextImage()
{
    MYWRITERLOCK(l, entryRW);

    if (!fd.empty() && !selected.empty() && !App::get().options().tabbedUI && tbl) {
        const std::ptrdiff_t current = findEntryIndexLocked_(selected.front());

        if (current < 0) {
            return;
        }

        for (size_t k = static_cast<size_t>(current) + 1; k < fd.size(); k++) {
            if (!fd[k]->filtered/*checkFilter (fd[k])*/) {

                // clear current selection
                for (size_t j = 0; j < selected.size(); j++) {
                    selected[j]->selected = false;
                }

                selected.clear();

                // set new selection
                fd[k]->selected = true;
                selected.push_back(fd[k]);
                //queue_draw();

                // scroll to the selected position, centered horizontally in the container
                double x1, y1;
                getScrollPosition(x1, y1);

                double x2 = fd[k]->getStartX();
                double y2 = fd[k]->getStartY();

                auto* openEntry = static_cast<FileBrowserEntry*>(fd[k]);
                Thumbnail* thumb = openEntry->thumbnail;
                const Glib::ustring openFname = fd[k]->filename;
                int tw = fd[k]->getMinimalWidth(); // thumb width

                int ww = get_width(); // window width

                MYWRITERLOCK_RELEASE(l);

                // open the selected image
                openEntry->cacheCurrentPreviewForQuickOpen();
                lastOpenRequestedFname_ = openFname;
                tbl->openRequested({thumb}, NAV_NEXT);

                // this will require a read access
                scheduleSelectionNotify();

                // scroll only when selected[0] is outside of the displayed bounds
                // or less than a thumbnail's width from either edge.
                if ((x2 > x1 + ww - 1.5 * tw) || (x2 - tw / 2 < x1)) {
                    setScrollPosition(x2 - (ww - tw) / 2, y2);
                }

                return;
            }
        }
    }
}

void FileBrowser::openPrevImage()
{
    MYWRITERLOCK(l, entryRW);

    if (!fd.empty() && !selected.empty() && !App::get().options().tabbedUI && tbl) {
        const std::ptrdiff_t current = findEntryIndexLocked_(selected.front());

        if (current <= 0) {
            return;
        }

        // find the first not-filtered-out (previous) image
        for (std::ptrdiff_t k = current - 1; k >= 0; k--) {
            if (!fd[k]->filtered/*checkFilter (fd[k])*/) {

                // clear current selection
                for (size_t j = 0; j < selected.size(); j++) {
                    selected[j]->selected = false;
                }

                selected.clear();

                // set new selection
                fd[k]->selected = true;
                selected.push_back(fd[k]);
                //queue_draw();

                // scroll to the selected position, centered horizontally in the container
                double x1, y1;
                getScrollPosition(x1, y1);

                double x2 = fd[k]->getStartX();
                double y2 = fd[k]->getStartY();

                auto* openEntry = static_cast<FileBrowserEntry*>(fd[k]);
                Thumbnail* thumb = openEntry->thumbnail;
                const Glib::ustring openFname = fd[k]->filename;
                int tw = fd[k]->getMinimalWidth(); // thumb width

                int ww = get_width(); // window width

                MYWRITERLOCK_RELEASE(l);

                // open the selected image
                openEntry->cacheCurrentPreviewForQuickOpen();
                lastOpenRequestedFname_ = openFname;
                tbl->openRequested({thumb}, NAV_PREVIOUS);

                // this will require a read access
                scheduleSelectionNotify();

                // scroll only when selected[0] is outside of the displayed bounds
                // or less than a thumbnail's width from either edge.
                if ((x2 > x1 + ww - 1.5 * tw) || (x2 - tw / 2 < x1)) {
                    setScrollPosition(x2 - (ww - tw) / 2, y2);
                }

                return;
            }
        }
    }
}

void FileBrowser::selectImage(const Glib::ustring& fname, bool doScroll)
{
    flushPendingInsertsForSelection_();

    MYWRITERLOCK(l, entryRW);

    if (!fd.empty() && !App::get().options().tabbedUI) {
        ThumbBrowserEntryBase* entry = findEntryLocked_(fname);
        if (entry) {
            if (!entry->filtered) {
                const bool alreadyOnlySelected = selected.size() == 1 && selected.front() == entry && entry->selected;
                if (alreadyOnlySelected) {
                    const double x = entry->getStartX();
                    const double y = entry->getStartY();
                    const int tw = entry->getMinimalWidth();
                    const int th = entry->getMinimalHeight();
                    const int ww = get_width();
                    const int wh = get_height();

                    MYWRITERLOCK_RELEASE(l);

                    if (doScroll) {
                        // Center both axes: horizontal matters in the
                        // filmstrip, vertical in the browser grid.
                        setScrollPosition(x - (ww - tw) / 2, y - (wh - th) / 2);
                    }

                    return;
                }

                // matching file found for sync

                // clear current selection
                for (size_t j = 0; j < selected.size(); j++) {
                    selected[j]->selected = false;
                }

                selected.clear();

                // set new selection
                entry->selected = true;
                selected.push_back(entry);
                queue_draw();

                // scroll to the selected position, centered in the container
                double x = entry->getStartX();
                double y = entry->getStartY();

                int tw = entry->getMinimalWidth();  // thumb width
                int th = entry->getMinimalHeight(); // thumb height

                int ww = get_width();  // window width
                int wh = get_height(); // window height

                MYWRITERLOCK_RELEASE(l);

                // Programmatic sync does not need to block the open path on
                // secondary tool-panel selection updates.
                scheduleSelectionNotify();

                if (doScroll) {
                    // Center thumb on both axes (filmstrip scrolls
                    // horizontally, browser grid vertically)
                    setScrollPosition(x - (ww - tw) / 2, y - (wh - th) / 2);
                }

                return;
            }
        }
    }
}

Thumbnail* FileBrowser::getSelectedThumbnail()
{
    MYREADERLOCK(l, entryRW);
    if (lastClicked) {
        return lastClicked->thumbnail;
    }
    if (!selected.empty()) {
        return selected.front()->thumbnail;
    }
    return nullptr;
}

void FileBrowser::openNextPreviousEditorImage (const Glib::ustring& fname, eRTNav nextPrevious)
{
    flushPendingInsertsForSelection_();

    MYWRITERLOCK(l, entryRW);

    if (fd.empty() || App::get().options().tabbedUI || !tbl) {
        return;
    }

    const std::ptrdiff_t current = findEntryIndexLocked_(fname);
    if (current < 0) {
        return;
    }

    const std::ptrdiff_t step = nextPrevious == NAV_PREVIOUS ? -1 : 1;
    if (nextPrevious != NAV_NEXT && nextPrevious != NAV_PREVIOUS) {
        return;
    }

    for (std::ptrdiff_t k = current + step; k >= 0 && k < static_cast<std::ptrdiff_t>(fd.size()); k += step) {
        if (fd[k]->filtered) {
            continue;
        }

        for (size_t j = 0; j < selected.size(); j++) {
            selected[j]->selected = false;
        }

        selected.clear();

        fd[k]->selected = true;
        selected.push_back(fd[k]);

        double x1, y1;
        getScrollPosition(x1, y1);

        const double x2 = fd[k]->getStartX();
        const double y2 = fd[k]->getStartY();
        auto* openEntry = static_cast<FileBrowserEntry*>(fd[k]);
        Thumbnail* thumb = openEntry->thumbnail;
        const Glib::ustring openFname = fd[k]->filename;
        const int tw = fd[k]->getMinimalWidth();
        const int ww = get_width();

        MYWRITERLOCK_RELEASE(l);

        openEntry->cacheCurrentPreviewForQuickOpen();
        lastOpenRequestedFname_ = openFname;
        tbl->openRequested({thumb}, nextPrevious);

        scheduleSelectionNotify();

        if ((x2 > x1 + ww - 1.5 * tw) || (x2 - tw / 2 < x1)) {
            setScrollPosition(x2 - (ww - tw) / 2, y2);
        }

        return;
    }
}

void FileBrowser::openEditorImage(const Glib::ustring& fname, eRTNav preloadDirectionHint)
{
    flushPendingInsertsForSelection_();

    MYWRITERLOCK(l, entryRW);

    if (fd.empty() || App::get().options().tabbedUI || !tbl) {
        return;
    }

    const std::ptrdiff_t target = findEntryIndexLocked_(fname);
    if (target < 0 || fd[target]->filtered) {
        return;
    }

    for (size_t j = 0; j < selected.size(); j++) {
        selected[j]->selected = false;
    }

    selected.clear();

    fd[target]->selected = true;
    selected.push_back(fd[target]);

    double x1, y1;
    getScrollPosition(x1, y1);

    const double x2 = fd[target]->getStartX();
    const double y2 = fd[target]->getStartY();
    auto* openEntry = static_cast<FileBrowserEntry*>(fd[target]);
    Thumbnail* thumb = openEntry->thumbnail;
    const Glib::ustring openFname = fd[target]->filename;
    const int tw = fd[target]->getMinimalWidth();
    const int ww = get_width();

    MYWRITERLOCK_RELEASE(l);

    openEntry->cacheCurrentPreviewForQuickOpen();
    lastOpenRequestedFname_ = openFname;
    tbl->openRequested({thumb}, preloadDirectionHint);

    scheduleSelectionNotify();

    if ((x2 > x1 + ww - 1.5 * tw) || (x2 - tw / 2 < x1)) {
        setScrollPosition(x2 - (ww - tw) / 2, y2);
    }
}

std::vector<FileBrowser::AdjacentEntry> FileBrowser::getAdjacentEntries(const Glib::ustring& fname, int count)
{
    return getAdjacentEntriesAndRefresh(fname, count, 0, 0);
}

FileBrowserEntry* FileBrowser::findEntry(const Glib::ustring& fname)
{
    MYWRITERLOCK(l, entryRW);
    return static_cast<FileBrowserEntry*>(findEntryLocked_(fname));
}

void FileBrowser::cancelCachedQuickPreviewWarm()
{
    cancelCachedQuickPreviewWarmJobs();
}

std::vector<FileBrowser::AdjacentEntry> FileBrowser::getAdjacentEntriesAndRefresh(const Glib::ustring& fname, int preloadCount, int refreshCount, int quickPreviewWarmCount, eRTNav preferredDirection)
{
    std::vector<AdjacentEntry> result;
    result.reserve(static_cast<size_t>(std::max(preloadCount, 0)) * 2);
    std::vector<FileBrowserEntry*> refreshEntries;
    refreshEntries.reserve(static_cast<size_t>(std::max(refreshCount, 0)) * 2);
    std::vector<FileBrowserEntry*> quickPreviewWarmEntries;
    quickPreviewWarmEntries.reserve(static_cast<size_t>(std::max(quickPreviewWarmCount, 0)) * 2);

    {
        // Use the writer-side lookup so a cache miss can populate entryIndex_.
        // The adjacent walk itself is tiny (N +/- kRadius), and avoiding repeated
        // fallback scans matters more for large folders during rapid navigation.
        MYWRITERLOCK(l, entryRW);

        const std::ptrdiff_t idx = findEntryIndexLocked_(fname);

        if (idx < 0) return result;

        const int maxCount = std::max({preloadCount, refreshCount, quickPreviewWarmCount});
        auto collectSide = [&](std::ptrdiff_t start, std::ptrdiff_t step) {
            std::vector<FileBrowserEntry*> sideEntries;
            sideEntries.reserve(static_cast<size_t>(std::max(maxCount, 0)));

            for (std::ptrdiff_t k = start; k >= 0 && k < static_cast<std::ptrdiff_t>(fd.size()) && static_cast<int>(sideEntries.size()) < maxCount; k += step) {
                if (fd[k]->filtered) {
                    continue;
                }

                sideEntries.push_back(static_cast<FileBrowserEntry*>(fd[k]));
            }

            return sideEntries;
        };

        const bool directionalQuickPreviewWarm =
            preferredDirection == NAV_NEXT || preferredDirection == NAV_PREVIOUS;

        auto appendEntry = [&](FileBrowserEntry* entry, size_t sideIndex, bool preferredSide) {
            if (!entry) {
                return;
            }

            if (static_cast<int>(sideIndex) < refreshCount) {
                refreshEntries.push_back(entry);
            }
            if (static_cast<int>(sideIndex) < quickPreviewWarmCount
                && (!directionalQuickPreviewWarm || preferredSide)) {
                quickPreviewWarmEntries.push_back(entry);
            }
            if (static_cast<int>(sideIndex) < preloadCount) {
                const bool isRaw = entry->thumbnail->getType() == FT_Raw;
                const CacheImageData* const cfs = entry->thumbnail->getCacheImageData();
                const int fullW = cfs ? cfs->width : 0;
                const int fullH = cfs ? cfs->height : 0;
                const auto sensorType = cfs ? static_cast<rtengine::eSensorType>(cfs->sensortype) : rtengine::ST_NONE;
                auto sampleFormat = cfs ? cfs->sampleFormat : rtengine::IIOSF_UNKNOWN;
                if (!isRaw
                    && sampleFormat == rtengine::IIOSF_UNKNOWN
                    && isLikely8BitJpegExtension(getLowercaseExtension(entry->filename))) {
                    sampleFormat = rtengine::IIOSF_UNSIGNED_CHAR;
                }
                const unsigned int frameCount = cfs ? std::max(1u, static_cast<unsigned int>(cfs->frameCount)) : 1u;
                const size_t estimatedBytes = estimateInitialImageBytes(fullW, fullH, isRaw, sensorType, sampleFormat, frameCount);

                result.push_back({
                    entry->filename,
                    std::string(entry->filename),
                    estimatedBytes,
                    sensorType,
                    sampleFormat,
                    frameCount,
                    isRaw,
                    preferredSide
                });
            }
        };

        const auto forwardEntries = collectSide(idx + 1, 1);
        const auto backwardEntries = collectSide(idx - 1, -1);
        const auto* preferredEntries = &forwardEntries;
        const auto* oppositeEntries = &backwardEntries;
        if (preferredDirection == NAV_PREVIOUS) {
            preferredEntries = &backwardEntries;
            oppositeEntries = &forwardEntries;
        }

        size_t preferredStart = 0;
        size_t oppositeStart = 0;

        if (preferredDirection == NAV_NEXT || preferredDirection == NAV_PREVIOUS) {
            const size_t preferredLeadCountForDirectionalNav = static_cast<size_t>(std::max(preloadCount, 0));
            const size_t preferredLeadCount = std::min(preferredLeadCountForDirectionalNav, preferredEntries->size());
            for (; preferredStart < preferredLeadCount; ++preferredStart) {
                appendEntry((*preferredEntries)[preferredStart], preferredStart, true);
            }
        }

        const size_t interleaveCount = std::max(
            preferredEntries->size() > preferredStart ? preferredEntries->size() - preferredStart : 0,
            oppositeEntries->size() > oppositeStart ? oppositeEntries->size() - oppositeStart : 0);
        for (size_t i = 0; i < interleaveCount; ++i) {
            const size_t preferredIndex = preferredStart + i;
            if (preferredIndex < preferredEntries->size()) {
                appendEntry((*preferredEntries)[preferredIndex], preferredIndex, true);
            }

            const size_t oppositeIndex = oppositeStart + i;
            if (oppositeIndex < oppositeEntries->size()) {
                appendEntry((*oppositeEntries)[oppositeIndex], oppositeIndex, false);
            }
        }
    }

    std::vector<QuickPreviewCacheWarmItem> cacheWarmItems;
    cacheWarmItems.reserve(quickPreviewWarmEntries.size());
    size_t quickWarmAlreadyCached = 0;
    size_t quickWarmFromMemory = 0;
    size_t quickWarmBusy = 0;

    for (auto* entry : quickPreviewWarmEntries) {
        if (!entry || !entry->thumbnail) {
            continue;
        }

        double cachedScale = 1.0;
        bool cachedBusy = false;
        if (entry->thumbnail->tryGetCachedPixbuf(cachedScale, &cachedBusy)) {
            ++quickWarmAlreadyCached;
            continue;
        }
        if (cachedBusy) {
            ++quickWarmBusy;
            continue;
        }

        if (entry->cacheCurrentPreviewForQuickOpen()) {
            ++quickWarmFromMemory;
            continue;
        }

        entry->thumbnail->increaseRef();
        cacheWarmItems.push_back({entry->thumbnail});
    }

    if (quickPreviewWarmCount > 0) {
        fileBrowserPerfLog(
            "[quickWarm] scheduled ready=%zu memory=%zu disk=%zu busy=%zu radius=%d anchor=%s\n",
            quickWarmAlreadyCached,
            quickWarmFromMemory,
            cacheWarmItems.size(),
            quickWarmBusy,
            quickPreviewWarmCount,
            fname.c_str());
    }
    scheduleCachedQuickPreviewWarm(
        std::move(cacheWarmItems),
        App::get().options().maxThumbnailHeight);

    std::vector<ThumbImageUpdater::Request> refreshRequests;
    refreshRequests.reserve(refreshEntries.size());
    const bool cacheAdjacentPixbufs = preloadCount > 0;
    for (auto* entry : refreshEntries) {
        entry->appendQuickThumbnailJob(refreshRequests, cacheAdjacentPixbufs);
    }
    thumbImageUpdater->addBatch(refreshRequests);

    return result;
}

void FileBrowser::refreshAdjacentThumbnails(const Glib::ustring& fname, int count)
{
    getAdjacentEntriesAndRefresh(fname, 0, count, 0);
}

void FileBrowser::thumbRearrangementNeeded ()
{
    idle_register.add(
        [this]() -> bool
        {
            // The entry already adopted the delivered pixel geometry. Reflow
            // without regenerating every thumbnail or restoring stale metadata.
            redraw();
            return false;
        }
    );
}

void FileBrowser::selectionChanged ()
{

    notifySelectionListener ();
}

void FileBrowser::notifySelectionListener ()
{
    if (!tbl) {
        return;
    }

    std::vector<Thumbnail*> thm;

    {
        MYREADERLOCK(l, entryRW);

        thm.reserve(selected.size());

        for (size_t i = 0; i < selected.size(); i++) {
            thm.push_back ((static_cast<FileBrowserEntry*>(selected[i]))->thumbnail);
        }
    }

    tbl->selectionChanged (thm);
}

void FileBrowser::scheduleSelectionNotify()
{
    if (selectionNotifyIdlePending_) {
        return;
    }

    selectionNotifyIdlePending_ = true;
    idle_register.add(
        [this]() -> bool {
            selectionNotifyIdlePending_ = false;
            notifySelectionListener();
            return false;
        },
        G_PRIORITY_DEFAULT_IDLE);
}

void FileBrowser::redrawNeeded (LWButton* button)
{
    GThreadLock lock;
    queue_draw ();
}
FileBrowser::type_trash_changed FileBrowser::trash_changed ()
{
    return m_trash_changed;
}

FileBrowser::type_save_image_requested& FileBrowser::save_image_requested ()
{
    return m_save_image_requested;
}

FileBrowser::type_external_editor_requested& FileBrowser::external_editor_requested ()
{
    return m_external_editor_requested;
}


// ExportPanel interface
void FileBrowser::exportRequested ()
{
    FileBrowser::menuItemActivated(developfast);
}

void FileBrowser::setExportPanel (ExportPanel* expanel)
{

    exportPanel = expanel;
    exportPanel->set_sensitive (false);
    exportPanel->setExportPanelListener (this);
}

void FileBrowser::storeCurrentValue()
{
}

void FileBrowser::updateProfileList()
{
    // submenu applmenu
    int p = 0;

    const std::vector<const ProfileStoreEntry*> *profEntries = ProfileStore::getInstance()->getFileList();  // lock and get a pointer to the profiles' list

    std::map<unsigned short /* folderId */, Gtk::Menu*> subMenuList;  // store the Gtk::Menu that Gtk::MenuItem will have to be attached to

    subMenuList[0] = Gtk::manage (new Gtk::Menu ()); // adding the root submenu

    const auto& options = App::get().options();

    // iterate the profile store's profile list
    for (size_t i = 0; i < profEntries->size(); i++) {
        // create a new label for the current entry (be it a folder or file)
        ProfileStoreLabel *currLabel = Gtk::manage(new ProfileStoreLabel( profEntries->at(i) ));

        // create the MenuItem object
        Gtk::MenuItem* mi = Gtk::manage (new Gtk::MenuItem (*currLabel));

        // create a new Menu object if the entry is a folder and not the root one
        if (currLabel->entry->type == PSET_FOLDER) {
            // creating the new sub-menu
            Gtk::Menu* subMenu = Gtk::manage (new Gtk::Menu ());

            // add it to the menu list
            subMenuList[currLabel->entry->folderId] = subMenu;

            // add it to the parent MenuItem
            mi->set_submenu(*subMenu);
        }

        // Hombre: ... does parentMenuId sounds like a hack?         ... Yes.
        int parentMenuId = !options.useBundledProfiles && currLabel->entry->parentFolderId == 1 ? 0 : currLabel->entry->parentFolderId;
        subMenuList[parentMenuId]->attach (*mi, 0, 1, p, p + 1);
        p++;

        if (currLabel->entry->type == PSET_FILE) {
            mi->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::applyMenuItemActivated), currLabel));
        }

        mi->show ();
    }

    if (subMenuList.size() && applyprof)
        // TODO: Check that the previous one has been deleted, including all childrens
    {
        applyprof->set_submenu (*(subMenuList.at(0)));
    }

    subMenuList.clear();
    subMenuList[0] = Gtk::manage (new Gtk::Menu ()); // adding the root submenu
    // keep profEntries list

    // submenu applpartmenu
    p = 0;

    for (size_t i = 0; i < profEntries->size(); i++) {
        ProfileStoreLabel *currLabel = Gtk::manage(new ProfileStoreLabel( profEntries->at(i) ));

        Gtk::MenuItem* mi = Gtk::manage (new Gtk::MenuItem (*currLabel));

        if (currLabel->entry->type == PSET_FOLDER) {
            // creating the new sub-menu
            Gtk::Menu* subMenu = Gtk::manage (new Gtk::Menu ());

            // add it to the menu list
            subMenuList[currLabel->entry->folderId] = subMenu;

            // add it to the parent MenuItem
            mi->set_submenu(*subMenu);
        }

        // Hombre: ... does parentMenuId sounds like a hack?         ... yes.
        int parentMenuId = !options.useBundledProfiles && currLabel->entry->parentFolderId == 1 ? 0 : currLabel->entry->parentFolderId;
        subMenuList[parentMenuId]->attach (*mi, 0, 1, p, p + 1);
        p++;

        if (currLabel->entry->type == PSET_FILE) {
            mi->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::applyPartialMenuItemActivated), currLabel));
        }

        mi->show ();
    }

    if (subMenuList.size() && applypartprof)
        // TODO: Check that the previous one has been deleted, including all childrens
    {
        applypartprof->set_submenu (*(subMenuList.at(0)));
    }

    ProfileStore::getInstance()->releaseFileList();
    subMenuList.clear();
}

void FileBrowser::restoreValue()
{
}

void FileBrowser::openRequested( std::vector<FileBrowserEntry*> mselected)
{
    std::vector<Thumbnail*> entries;
    // in Single Editor Mode open only last selected image
    const bool tabbedUI = App::get().options().tabbedUI;
    size_t openStart = tabbedUI ? 0 : ( mselected.size() > 0 ? mselected.size() - 1 : 0);
    const bool warmQuickPreview = !tabbedUI || mselected.size() == 1;
    Glib::ustring openFname;

    for (size_t i = openStart; i < mselected.size(); i++) {
        if (warmQuickPreview) {
            mselected[i]->cacheCurrentPreviewForQuickOpen();
        }
        entries.push_back (mselected[i]->thumbnail);
        openFname = mselected[i]->filename;
    }

    eRTNav preloadDirectionHint = NAV_NONE;

    if (entries.size() == 1 && !openFname.empty()) {
        // Direction is only a preload hint. Never block the GTK thread behind
        // thumbnail/list refresh work just to compute it; opening the selected
        // image is more important than a directional cache preference.
        MyTryReaderLock lock(entryRW);
        if (lock.owns_lock()) {
            const std::ptrdiff_t current = findEntryIndexLocked_(openFname);
            const std::ptrdiff_t previous = lastOpenRequestedFname_.empty()
                ? -1
                : findEntryIndexLocked_(lastOpenRequestedFname_);

            if (current >= 0 && previous >= 0) {
                if (current > previous) {
                    preloadDirectionHint = NAV_NEXT;
                } else if (current < previous) {
                    preloadDirectionHint = NAV_PREVIOUS;
                }
            }
        }

        lastOpenRequestedFname_ = openFname;
    } else if (!openFname.empty()) {
        lastOpenRequestedFname_ = openFname;
    }

    tbl->openRequested (entries, preloadDirectionHint);
}

void FileBrowser::inspectRequested(std::vector<FileBrowserEntry*> mselected)
{
    getInspector()->showWindow(true);
}
