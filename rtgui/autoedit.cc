/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

// Auto Edit: the file browser's one-click grading pipeline.
//
// Three stages, in order:
//   1. analyzeSteepAutoGrade  - read the picture and describe it
//   2. applySteepAutoEdit     - exposure, shadow/highlight and the tone curve
//   3. applySteepAutoGrade / applySteepAutoFilm - the look laid on top
//
// Set STEEP_FILESEL_LOG=1 to trace every decision into
// %USERPROFILE%\steep-fileSel.log.

#include "autoedit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "steepperflog.h"
#include "thumbnail.h"

#include "rtengine/diagonalcurvetypes.h"
#include "rtengine/rt_math.h"
#include "rtengine/iimage.h"
#include "rtengine/procparams.h"

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

const char* autoEditModeLabel(SteepAutoEditMode mode)
{
    switch (mode) {
        case SteepAutoEditMode::Grade: return "FILEBROWSER_POPUPAUTOGRADE";
        case SteepAutoEditMode::GradeFilm: return "FILEBROWSER_POPUPFILMLAB";
        case SteepAutoEditMode::GradedFilm: return "FILEBROWSER_POPUPAUTOGRADEFILM";
        case SteepAutoEditMode::Neutral: return "FILEBROWSER_POPUPAUTOEDITNEUTRAL";
    }
    return "FILEBROWSER_POPUPAUTOEDITNEUTRAL";
}

const char* autoEditModeName(SteepAutoEditMode mode)
{
    switch (mode) {
        case SteepAutoEditMode::Grade: return "grade";
        case SteepAutoEditMode::GradeFilm: return "film-lab";
        case SteepAutoEditMode::GradedFilm: return "graded-film-lab";
        case SteepAutoEditMode::Neutral: return "neutral";
    }
    return "neutral";
}

namespace
{

using AutoEditMode = SteepAutoEditMode;

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

// A threshold turned into a slope. Below `low` the evidence says no, above
// `high` it says yes, and in between it says how much -- which is what a
// photograph usually has to offer.
double ramp(double value, double low, double high)
{
    return std::max(0.0, std::min(1.0, (value - low) / std::max(1e-6, high - low)));
}

// The same, inverted: full weight below `low`, none above `high`.
double falloff(double value, double low, double high)
{
    return 1.0 - ramp(value, low, high);
}

// How dark the frame reads. Split out because applySteepAutoEdit has to ask it
// again about its own render, and the two answers must agree.
double sceneDarkScore(double medianLuma, double shadowFraction)
{
    return falloff(medianLuma, 0.16, 0.34) * ramp(shadowFraction, 0.28, 0.46);
}

// What the picture is, as five independent readings rather than one verdict.
//
// The old bounds survive as the midpoints of these ramps, so a frame that
// clearly satisfied a test still scores near 1 and a frame that clearly failed
// still scores 0. What changes is everything in between, and the fact that a
// frame can now be two things at once.
//
// Content evidence (skin, sky, foliage, edges) comes off the source and is
// stable; the tonal evidence is passed in, because it has to be asked twice --
// once about the neutral render, and again about the frame Auto Edit actually
// produced. Answering it once, on the neutral render, is what made every
// third frame in a real library come back labelled "night".
AutoSceneScores scoreSteepAutoScene(
    const AutoGradeFeatures& features,
    double medianLuma,
    double shadowFraction,
    double dynamicRange,
    double brightWarmFraction)
{
    AutoSceneScores scores;

    // A face has to be both present and roughly where a subject sits; a heavy
    // overall cast that merely resembles skin should not read as a portrait,
    // which is what the saturation guard is for.
    const double skinPresent = std::min(
        ramp(features.skinFraction, 0.030, 0.070),
        ramp(features.centerSkinFraction, 0.050, 0.110));
    const double skinPlausible = falloff(features.skinSaturation, 0.44, 0.56);
    const double notGraphic = std::max(
        falloff(features.saturation, 0.33, 0.45),
        ramp(dynamicRange, 0.24, 0.32));
    scores.portrait = skinPresent * skinPlausible * notGraphic;

    // Warm light rather than warm objects: a warm cast that is also BRIGHT,
    // and decisively warmer than the frame is cool.
    const double warmDominance = ramp(
        features.warmFraction / std::max(1e-4, features.coolFraction), 1.0, 1.6);
    scores.lowSun = ramp(brightWarmFraction, 0.050, 0.140) * warmDominance;

    scores.dark = sceneDarkScore(medianLuma, shadowFraction);

    scores.open = std::max(
        ramp(features.skyFraction, 0.040, 0.110),
        ramp(features.foliageFraction, 0.080, 0.180))
        * falloff(features.skinFraction, 0.020, 0.050);

    // edgeDensity sums every neighbour gradient, so a noisy or finely
    // textured frame scores as highly as an architectural one. The share of
    // gradients steep enough to be actual edges tells them apart. It is a
    // modulation rather than a gate, because the threshold for "a real edge"
    // is not yet measured over a library the way the others are.
    scores.urban = ramp(features.edgeDensity, 0.055, 0.095)
        * falloff(features.foliageFraction, 0.060, 0.140)
        * (0.55 + 0.45 * ramp(features.strongEdgeFraction, 0.015, 0.055));

    return scores;
}

// One word for the picture, for the log and for anything that wants a label.
// This is now argmax rather than a priority chain: a golden-hour portrait used
// to be a portrait purely because Portrait was tested first.
AutoGradeScene dominantScene(const AutoSceneScores& scores)
{
    // Below this nothing is really being claimed, and a neutral grade is the
    // honest answer.
    constexpr double kFloor = 0.22;
    const double best = scores.strongest();

    if (best < kFloor) {
        return AutoGradeScene::Neutral;
    }

    if (scores.portrait >= best) return AutoGradeScene::Portrait;
    if (scores.lowSun >= best) return AutoGradeScene::GoldenHour;
    if (scores.dark >= best) return AutoGradeScene::Night;
    if (scores.open >= best) return AutoGradeScene::Landscape;
    return AutoGradeScene::Urban;
}

}

double brightWarmFractionAtGain(const AutoGradeFeatures& features, double gain)
{
    if (!features.valid) {
        return 0.0;
    }

    if (gain <= 1.0001) {
        return features.brightWarmFraction;
    }

    // Which neutral-render luminance lands on 0.55 once the frame is exposed
    // the way Auto Edit exposes it. Bins are counted from the first one whose
    // centre clears that, so the answer is the same question the neutral pass
    // asked, re-asked at the brightness the viewer will see.
    const double threshold = 0.55 / gain;
    const size_t bins = features.warmLumaHistogram.size();
    double fraction = 0.0;

    for (size_t i = 0; i < bins; ++i) {
        if ((i + 0.5) / bins > threshold) {
            fraction += features.warmLumaHistogram[i];
        }
    }

    return fraction;
}

// Also used by auto-cull, which judges a frame on the same statistics.
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
    std::array<size_t, 32> warmLumaBins{};

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
            const bool isWarm = saturation > 0.12 && (hue <= 70.0 || hue >= 340.0);
            warm += isWarm;
            brightWarm += isWarm && luminance > 0.55;

            if (isWarm) {
                warmLumaBins[std::min<size_t>(
                    warmLumaBins.size() - 1, luminance * warmLumaBins.size())]++;
            }
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
    features.p10 = percentile(0.1);
    features.p90 = percentile(0.9);
    features.p98 = percentile(0.98);
    features.dynamicRange = features.p90 - features.p10;
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

    size_t warmBelowMedian = 0;
    size_t warmTotal = 0;

    for (size_t i = 0; i < warmLumaBins.size(); ++i) {
        features.warmLumaHistogram[i] = static_cast<float>(warmLumaBins[i] / count);
        warmTotal += warmLumaBins[i];

        if ((i + 0.5) / warmLumaBins.size() < features.medianLuma) {
            warmBelowMedian += warmLumaBins[i];
        }
    }

    features.warmShadowShare = warmTotal
        ? static_cast<double>(warmBelowMedian) / warmTotal
        : 0.0;

    // Provisional only. "Is this a night scene?" and "is this golden hour?"
    // are questions about how the picture READS, and this render is not the
    // picture anyone sees -- classifySteepAutoGrade is called again once the
    // tonal stage has said what the frame actually becomes. The provisional
    // label still has to be reasonable, because auto-cull uses it and because
    // the restraint terms inside applySteepAutoEdit run before the render is
    // measured.
    features.scores = scoreSteepAutoScene(
        features,
        features.medianLuma,
        features.shadowFraction,
        features.dynamicRange,
        features.brightWarmFraction);
    features.scene = dominantScene(features.scores);

    return features;
}

namespace
{

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
    // The double exposure stack is the photographer's content too -- Auto
    // Edit re-grades the plate, it must not dismantle the composite.
    target.doubleExposure = source.doubleExposure;
}

// Percentiles of the image AS THE TONE CURVE WILL RECEIVE IT, i.e. rendered
// with the exposure and tone decisions Auto Edit has just made, with the curve
// itself disabled. Measuring on a neutral render instead reports a far darker
// image than the curve ever sees, which drags every control point down into
// the shadows.
//
// TWO distributions come back, because two different questions are being
// asked. "How bright is this picture?" is a luminance question, and drives the
// exposure decision. "Where does the curve need its control points?" is not:
// the master curve is applied to R, G and B INDEPENDENTLY, so the values that
// actually index it are the channel samples, whose spread is typically around
// twice that of their luma. Sizing the curve's steep region from luma puts a
// third of the real data out in the flattened toe and shoulder runs.
// Where Auto Edit wants the two ends of a finished frame to sit: close enough
// to the limits that the range is used, far enough off them that nothing is
// welded to the floor or the ceiling. Both ends were previously a side effect
// of where the data happened to land, which is why frames came back either
// clipped at both ends or visibly short of both.
constexpr double kBlackTarget = 0.008;   // ~2/255
constexpr double kWhiteTarget = 0.985;   // ~251/255

// Specular highlights are allowed to blow; a sky is not. This is the share of
// channel samples that may sit on the top code before exposure gives ground.
constexpr double kClipTolerance = 0.002;
constexpr double kClipRecoveryGain = 12.0;

// How much of the gap to each target a single pass may close. Endpoint
// placement is a correction to a frame the rest of Auto Edit has already
// judged, not a second opinion about it, so it never takes the whole gap.
constexpr double kEndpointAuthority = 0.75;

// ... and a cap on the absolute move, so a frame that is soft on purpose —
// fog, high key, a low-key studio black — is given some depth rather than
// dragged all the way into being something it was never meant to be.
constexpr double kMaxEndpointMove = 0.10;

struct CurveAnchors {
    // Rec.709 luma — the picture's brightness.
    double lumaP02 = 0.0;
    double lumaP10 = 0.0;
    double lumaMid = 0.0;
    double lumaP90 = 0.0;
    double lumaP98 = 1.0;
    // R/G/B samples pooled — what the per-channel curve is indexed by.
    double chanP10 = 0.0;
    double chanMid = 0.0;
    double chanP90 = 0.0;
    // The ends of the frame, read off a much finer histogram than the
    // percentiles above. The 128-bin one resolves to 1/128, which is two
    // display codes and far too coarse to tell "sitting just off the floor"
    // from "welded to it" — which is the whole question at the endpoints.
    double chanLo = 0.0;      // p0.1: the darkest tone that is really there
    double chanHi = 1.0;      // p99.9: the brightest
    double clipLow = 0.0;     // share of channel samples pinned at the floor
    double clipHigh = 0.0;    // ... and at the ceiling
    // The same two shares the source analysis reports, measured here on the
    // frame as it actually renders. Thresholds identical to the neutral pass
    // (0.16 / 0.84) so the two are directly comparable.
    double lumaShadowFraction = 0.0;
    double lumaHighFraction = 0.0;
};

bool measureCurveAnchors(
    Thumbnail& thumbnail,
    const rtengine::procparams::ProcParams& params,
    CurveAnchors& anchors)
{
    rtengine::procparams::ProcParams probe = params;
    probe.rgbCurves = rtengine::procparams::RGBCurvesParams();

    double scale = 1.0;
    std::unique_ptr<rtengine::IImage8> image(thumbnail.processFullThumbImage(probe, 160, scale));

    if (!image || !image->getData() || image->getWidth() < 8 || image->getHeight() < 8) {
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(image->getWidth()) * image->getHeight();

    if (pixelCount == 0) {
        return false;
    }

    const auto* pixels = image->getData();
    std::array<size_t, 128> lumaHist{};
    std::array<size_t, 128> chanHist{};
    // A second, per-display-code histogram, used only for the endpoints. The
    // coarse one above stays exactly as it was so the tuned mid/p10/p90
    // behaviour does not shift underneath this.
    std::array<size_t, 256> chanFine{};

    for (size_t i = 0; i < pixelCount; ++i) {
        const double luminance = (0.2126 * pixels[i * 3]
                                  + 0.7152 * pixels[i * 3 + 1]
                                  + 0.0722 * pixels[i * 3 + 2]) / 255.0;
        lumaHist[std::min<size_t>(lumaHist.size() - 1, luminance * lumaHist.size())]++;

        for (int c = 0; c < 3; ++c) {
            const unsigned char code = pixels[i * 3 + c];
            const double value = code / 255.0;
            chanHist[std::min<size_t>(chanHist.size() - 1, value * chanHist.size())]++;
            chanFine[code]++;
        }
    }

    const auto percentileOf = [](const std::array<size_t, 128>& histogram,
                                 size_t sampleCount,
                                 double fraction) {
        const size_t target = static_cast<size_t>(std::round(fraction * (sampleCount - 1)));
        size_t accumulated = 0;

        for (size_t i = 0; i < histogram.size(); ++i) {
            accumulated += histogram[i];

            if (accumulated > target) {
                return (i + 0.5) / histogram.size();
            }
        }

        return 1.0;
    };

    const auto luma = [&](double fraction) {
        return percentileOf(lumaHist, pixelCount, fraction);
    };
    const auto chan = [&](double fraction) {
        return percentileOf(chanHist, pixelCount * 3, fraction);
    };

    anchors.lumaP02 = luma(0.02);
    anchors.lumaP10 = luma(0.10);
    anchors.lumaMid = luma(0.50);
    anchors.lumaP90 = luma(0.90);
    anchors.lumaP98 = luma(0.98);
    anchors.chanP10 = chan(0.10);
    anchors.chanMid = chan(0.50);
    anchors.chanP90 = chan(0.90);

    const size_t channelSamples = pixelCount * 3;
    const auto finePercentile = [&](double fraction) {
        const size_t target = static_cast<size_t>(std::round(fraction * (channelSamples - 1)));
        size_t accumulated = 0;
        for (size_t i = 0; i < chanFine.size(); ++i) {
            accumulated += chanFine[i];
            if (accumulated > target) {
                return i / 255.0;
            }
        }
        return 1.0;
    };
    size_t renderShadows = 0;
    size_t renderHighlights = 0;

    for (size_t i = 0; i < lumaHist.size(); ++i) {
        const double centre = (i + 0.5) / lumaHist.size();

        if (centre < 0.16) {
            renderShadows += lumaHist[i];
        } else if (centre > 0.84) {
            renderHighlights += lumaHist[i];
        }
    }

    anchors.lumaShadowFraction = static_cast<double>(renderShadows) / pixelCount;
    anchors.lumaHighFraction = static_cast<double>(renderHighlights) / pixelCount;

    anchors.chanLo = finePercentile(0.001);
    anchors.chanHi = finePercentile(0.999);
    // "Pinned" means the bottom or top display code: those samples have lost
    // their value and no curve downstream can give it back.
    anchors.clipLow = static_cast<double>(chanFine.front()) / channelSamples;
    anchors.clipHigh = static_cast<double>(chanFine.back()) / channelSamples;

    fileBrowserPerfLog(
        "[autoCurve]   luma    p02=%.4f p10=%.4f mid=%.4f p90=%.4f p98=%.4f (span=%.4f)\n"
        "[autoCurve]   channel p10=%.4f mid=%.4f p90=%.4f (span=%.4f)\n",
        anchors.lumaP02, anchors.lumaP10, anchors.lumaMid, anchors.lumaP90,
        anchors.lumaP98, anchors.lumaP90 - anchors.lumaP10,
        anchors.chanP10, anchors.chanMid, anchors.chanP90,
        anchors.chanP90 - anchors.chanP10);
    return true;
}

// Start Auto Edit from a known neutral profile so repeated runs and previously
// edited images produce the same result.
void applySteepAutoEdit(
    Thumbnail& thumbnail,
    const AutoGradeFeatures& features,
    rtengine::procparams::ProcParams& params,
    AutoEditRender& render)
{
    params.setDefaults();
    render = AutoEditRender();

    const auto unit = [](double value) {
        return std::max(0.0, std::min(1.0, value));
    };

    // How much this frame needs protecting from an assertive edit, judged
    // before anything is applied. Blown highlights are the one thing that
    // cannot be walked back, so they — not a blanket timidity — are what
    // holds the edit back. Auto Edit used to take a flat 40% haircut off
    // every decision, which is why frames came back looking untouched.
    const double protection = features.valid
        ? std::max(unit((features.clippedFraction - 0.002) / 0.020),
                   unit((features.highlightFraction - 0.10) / 0.18))
        : 0.35;
    const double neutralFlatness = features.valid
        ? unit((0.62 - features.dynamicRange) / 0.62)
        : 0.35;

    // --- White balance ----------------------------------------------------
    //
    // Auto Edit never touched this. Every photograph came out on camera WB
    // whatever the light was doing, which is a strange gap in something whose
    // job is to make a picture look right -- colour is most of what a look is.
    // Both readings it needs were already sitting in the thumbnail cache:
    // getCamWB and getAutoWB read stored numbers, neither renders anything, so
    // the only reason this was never asked is that nobody asked it.
    //
    // The disagreement between the two is NOT an error signal by itself. A
    // camera reporting 3200K on a frame whose grey point says 4600K is either
    // wrong, or looking at a genuinely warm scene and rendering it faithfully,
    // and nothing in the two numbers tells you which. What does tell you is
    // where the warmth sits. Low sun lights the bright end; a cast tints the
    // shadows too. So a correction that would REMOVE warmth has to earn it
    // from warmShadowShare, while one that adds warmth -- the camera left a
    // tungsten interior blue, say -- does not need the same permission.
    //
    // Mireds throughout, because equal steps there are equal visual steps:
    // 500K at the tungsten end is an enormous move and at the shade end is
    // barely visible.
    double camTemp = 0.0;
    double camGreen = 1.0;
    double autoTemp = -1.0;
    double autoGreen = -1.0;
    thumbnail.getCamWB(camTemp, camGreen, params.wb.observer);
    thumbnail.getAutoWB(autoTemp, autoGreen, params.wb.equal, params.wb.observer,
                        params.wb.tempBias);

    double wbMiredMove = 0.0;
    double wbGreenMove = 0.0;
    bool wbApplied = false;

    if (camTemp > 1000.0 && autoTemp > 1000.0) {
        const double camMired = 1e6 / camTemp;
        const double autoMired = 1e6 / autoTemp;
        const double disagreement = autoMired - camMired;

        // Below this the camera and the grey point effectively agree, and
        // moving anything would be noise dressed as a decision.
        constexpr double kDeadband = 12.0;
        // A bounded correction, never a replacement. At 5000K this is a move
        // of roughly 400K, which is visible without rewriting the picture.
        constexpr double kMaxMired = 18.0;

        if (std::abs(disagreement) > kDeadband) {
            // Positive mired move = cooling the render (removing warmth).
            const double cooling = disagreement > 0.0 ? 1.0 : 0.0;
            const double castLike = unit((features.warmShadowShare - 0.35) / 0.25);
            const double intentionalLight =
                cooling * (1.0 - castLike) * features.scores.lowSun;
            const double authority = 0.60 * (1.0 - 0.85 * intentionalLight)
                * (cooling > 0.0 ? (0.45 + 0.55 * castLike) : 1.0);

            const double wanted = (disagreement - (disagreement > 0.0 ? kDeadband : -kDeadband))
                * authority;
            wbMiredMove = std::max(-kMaxMired, std::min(kMaxMired, wanted));

            // Green is the tint axis, and a green or magenta cast is almost
            // never something a photographer chose, so it needs no equivalent
            // of the low-sun guard -- only the same restraint about size.
            if (autoGreen > 0.0) {
                wbGreenMove = std::max(-0.030, std::min(0.030,
                    (autoGreen - camGreen) * 0.60));
            }

            if (std::abs(wbMiredMove) > 0.5 || std::abs(wbGreenMove) > 0.002) {
                params.wb.method = "Custom";
                params.wb.temperature = 1e6 / std::max(1.0, camMired + wbMiredMove);
                params.wb.green = camGreen + wbGreenMove;
                wbApplied = true;
            }
        }
    }

    // --- Noise ------------------------------------------------------------
    //
    // ISO was already being measured and then thrown away: its only consumer
    // was film.grain, which the engine does not read. Meanwhile the pipeline
    // routinely applies two stops of exposure and a contrast curve to a high
    // ISO frame and never once denoises it. Chroma noise is both the more
    // objectionable artefact and the cheaper one to remove without costing
    // detail, so it carries most of this; luma denoising stays small and
    // starts later, because it is what eats texture.
    const double isoStops = std::log2(std::max(features.iso, 100u) / 100.0);
    const double chromaNoise = std::min(35.0, std::max(0.0, 5.0 * (isoStops - 1.0)));
    const double lumaNoise = std::min(18.0, std::max(0.0, 2.5 * (isoStops - 2.5)));
    // How much this frame's noise should hold the rest of Auto Edit back.
    // Nothing at ISO 400, everything by ISO 3200.
    const double noiseRisk = unit((isoStops - 2.0) / 3.0);

    if (chromaNoise > 0.5 || lumaNoise > 0.5) {
        auto& denoise = params.dirpyrDenoise;
        denoise.enabled = true;
        denoise.luma = lumaNoise;
        denoise.chroma = chromaNoise;
        denoise.Ldetail = 50.0;
    }

    auto& tone = params.toneCurve;
    tone.autoexp = true;
    // Expose right up to the clipping point but not into it. NOTE: clip is a
    // PERCENTAGE (the GUI labels it "Clip %"), so 0.02 already commits only
    // 1 pixel in 5000 to pure white — it is not the 2% it reads like. An
    // earlier attempt to tighten this to 0.001 silently disabled metering
    // altogether, because the only path that can honour a non-default clip
    // needs the raw AE histogram and that is never resident for a
    // cache-loaded thumbnail.
    tone.clip = 0.02;
    tone.hrenabled = false;
    tone.method = "Coloropp";
    tone.expcomp = std::numeric_limits<double>::quiet_NaN();
    const bool measuredExposure =
        thumbnail.applyAutoExp(params) && std::isfinite(tone.expcomp);

    if (!measuredExposure) {
        // No metering available for this frame. These constants are a last
        // resort, not a recipe: say so, because a silent substitution here
        // looks exactly like a real measurement downstream.
        std::fprintf(
            stderr,
            "steep: Auto Edit could not meter %s — no auto-exposure data in the "
            "thumbnail cache; falling back to fixed exposure constants.\n",
            thumbnail.getFileName().c_str());
        fileBrowserPerfLog(
            "[autoCurve] WARNING no metering for %s — using fallback constants\n",
            thumbnail.getFileName().c_str());
        tone.expcomp = 0.20;
        tone.brightness = 2;
        tone.contrast = 0;
        tone.black = 0;
        tone.hlcompr = 35;
        tone.hlcomprthresh = 0;
    }

    // Everything from here down used to ADD to the exposure, and every one of
    // those additions was calibrated when metering was dead and the starting
    // point was always the same 0.20 constant. With metering restored they
    // stack on a number that is already correct: a blanket +0.05, a scene
    // nudge of up to +0.12, and the mid-target chimp below, which together
    // moved frames by a third of a stop or more for no measured reason. When
    // the meter spoke, it owns the exposure; only the chimp survives, as a
    // bounded trim for frames that land genuinely far from a print mid tone.
    if (!measuredExposure) {
        // Give normally exposed and dark frames a modest lift while respecting
        // the highlight compression selected by histogram analysis.
        const double exposureLift = tone.expcomp > 0.0
            ? (tone.hlcompr < 55 ? 0.05 : 0.0)
            : 0.0;
        tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp + exposureLift));
    }

    // Scene luminance supplies a restrained correction so night remains night
    // and bright frames retain highlight headroom — but only where no
    // histogram metering was available to see that for itself.
    if (features.valid && !measuredExposure) {
        double intent = 0.0;
        if (features.medianLuma < 0.19 && features.highlightFraction < 0.09) {
            intent = features.scene == AutoGradeScene::Night ? 0.02 : 0.12;
        } else if (features.medianLuma < 0.32 && features.highlightFraction < 0.14) {
            intent = features.scene == AutoGradeScene::Night ? 0.0 : 0.05;
        } else if (features.medianLuma > 0.61 || features.highlightFraction > 0.24) {
            intent = -0.24;
        }
        tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp + intent));
    }

    // Highlight compression answers what the source actually holds, so it is
    // not part of the metering question and runs either way.
    if (features.valid) {
        tone.hlcompr = std::max(tone.hlcompr, static_cast<int>(std::round(
            std::min(45.0, features.highlightFraction * 120.0))));
    }
    if (tone.expcomp > 0.0) {
        tone.expcomp *= 1.0 - 0.45 * protection;
    }
    // A floor, not an addition — same reasoning as contrast below. Adding to
    // a metered value was only ever safe while metering was dead and that
    // value was always 0.
    tone.brightness = std::max(0, std::min(35, static_cast<int>(std::round(
        std::max(std::max(0, tone.brightness), 3) * (1.0 - 0.40 * protection)))));
    // Contrast answers the frame: a flat scan wants a lot, a scene that
    // already carries its own contrast wants little. The old recipe asked for
    // a fixed +5 and then cut it by 40%, which landed on 3 — invisible.
    //
    // This is a FLOOR, not an addition. Adding it was safe only while metering
    // was broken and the metered contrast was always 0; with metering restored
    // a frame the meter already read as contrasty (28) got another 17 piled on
    // and pinned to the 45 ceiling, which — on top of the curve's own S — is
    // half of why wide-range frames came back too dark.
    const double contrastWanted = 7.0 + 13.0 * neutralFlatness;
    tone.contrast = std::max(0, std::min(45, static_cast<int>(std::round(
        std::max(static_cast<double>(std::max(0, tone.contrast)), contrastWanted)
        * (1.0 - 0.50 * protection)))));
    tone.autoexp = false;
    tone.curve = {DCT_Linear};
    tone.curve2 = {DCT_Linear};
    tone.curveR = {DCT_Linear};
    tone.curveG = {DCT_Linear};
    tone.curveB = {DCT_Linear};
    tone.curveMode = rtengine::procparams::ToneCurveMode::STD;
    tone.curveMode2 = rtengine::procparams::ToneCurveMode::STD;
    // Shadow compression lifts the bottom of the frame and highlight
    // compression pulls the top down: together they walk both ends toward the
    // middle, which is the flat, uninteresting look. At 50/55 they were
    // washing the picture out before the curve ever saw it. Spend them only
    // where the frame genuinely needs rescuing.
    // Shadow compression lifts the darks, and the darks are where the noise
    // lives. On a high ISO frame this is the control that makes grain visible.
    tone.shcompr = static_cast<int>(std::round(18.0 * (1.0 - 0.55 * noiseRisk)));
    tone.hlcompr = std::min(50, std::max(tone.hlcompr, tone.expcomp > 0.60 ? 30 : 12));

    // A meter that asks for heavy highlight compression has exposed past what
    // the frame can hold and is buying the top back by squashing it. That
    // pairing — a big positive exposure plus hlcompr pinned at 50 — is what
    // reads as "bright and flat on top", and inheriting both halves of it
    // means accepting the over-exposure to get the rescue. We have our own
    // shoulder (the curve) and our own highlight recovery, so take the
    // exposure back down instead and let those hold the highlights.
    if (measuredExposure && tone.hlcompr > 30 && tone.expcomp > 0.0) {
        const double excess = unit((tone.hlcompr - 30) / 20.0);
        tone.expcomp = std::max(0.0, tone.expcomp - 0.35 * excess);
        tone.hlcompr = static_cast<int>(std::round(30.0 + 10.0 * excess));
    }

    tone.hlbl = 0;
    tone.hlth = 1.0;
    tone.histmatching = false;
    tone.fromHistMatching = false;
    tone.clampOOG = true;

    // Highlight recovery has to be decided BEFORE the frame is measured. It
    // pulls the top of the range down over a very wide tonal band (70), so a
    // measurement taken without it describes a histogram the finished image
    // never has — and every curve station is then placed against that fiction.
    // It depends only on the source analysis and the exposure, so nothing here
    // needs the measurement.
    int highlightRecovery = 18;

    if (features.valid) {
        // Recovery darkens the highlights — it buys back detail that is
        // genuinely at risk and dulls everything else. Only blown frames pay.
        highlightRecovery = std::max(0, std::min(28, static_cast<int>(std::round(
            features.highlightFraction * 90.0
            + std::max(0.0, tone.expcomp - 0.40) * 12.0))));
    }

    auto& shadowsHighlights = params.sh;
    shadowsHighlights.enabled = highlightRecovery != 0;
    shadowsHighlights.highlights = highlightRecovery;
    shadowsHighlights.htonalwidth = 70;
    shadowsHighlights.shadows = 0;
    shadowsHighlights.stonalwidth = 24;
    shadowsHighlights.radius = 40;
    shadowsHighlights.lab = false;

    // One bound on the exposure, applied whatever path set it.
    //
    // Every other clamp here lives inside a branch: the fallback constants
    // clamp, and the chimp clamps only when it actually moves the value. A
    // metered exposure that landed inside the chimp's deadband therefore
    // reached the curve unbounded, and a real frame in the trace came through
    // at +4.15EV. It has to land before the measurement, not after, or every
    // percentile below would describe a frame a stop and a half brighter than
    // the one being built.
    tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp));

    // Chimp the histogram. Render the frame with what has been chosen so far
    // and, if the mid tone has not landed where a print would carry it,
    // correct the EXPOSURE rather than leaving the curve to do the
    // brightening: lifting mids with a curve compresses everything above
    // them, while lifting exposure moves the whole frame and lets highlight
    // recovery hold the top. Headroom keeps the veto.
    CurveAnchors shot;
    const bool shotValid = measureCurveAnchors(thumbnail, params, shot);

    const double shotMid = shot.lumaMid;
    const double shotP90 = shot.lumaP90;

    // Where the mid tone belongs depends on what the picture is. A night
    // frame carried up to a print's mid tone stops being a night frame — it
    // should sit low and keep its weight. Everything else wants the print.
    //
    // This used to be a switch on a label plus a hard shotMid < 0.24 gate, so
    // a frame at 0.239 aimed 0.14 lower than one at 0.241. It is a slope now,
    // anchored on the same two values: full print target when nothing about
    // the frame is dark, 0.31 when it thoroughly is.
    const double darkFrameScore = shotValid
        ? sceneDarkScore(shotMid, shot.lumaShadowFraction)
        : (features.scene == AutoGradeScene::Night ? 1.0 : 0.0);
    const double midTarget = 0.45 - 0.14 * darkFrameScore;

    double predictedP10 = shot.chanP10;
    double predictedMid = shot.chanMid;
    double predictedP90 = shot.chanP90;
    double predictedLumaMid = shot.lumaMid;
    double predictedLumaP10 = shot.lumaP10;
    double predictedLumaP90 = shot.lumaP90;
    double predictedLo = shot.chanLo;
    double predictedHi = shot.chanHi;
    double evAdd = 0.0;
    double hiRoomEv = 0.0;

    if (shotValid) {
        // The median is not the subject. On any frame with a large uniform
        // area that is not meant to read as mid grey — sky, snow, a dark wall,
        // a black backdrop — driving it to a print mid tone is simply wrong,
        // and on this fork's sky-dominated frames it used to spend the whole
        // +0.75 allowance doing it. So the chimp is a CORRECTION to metering,
        // not a replacement for it: when the frame was actually metered, the
        // meter owns the exposure and this may only trim it. The wide old
        // range survives solely for frames with no metering at all, where
        // there is nothing better to go on.
        const double chimpCeiling = measuredExposure ? 0.20 : 0.75;
        const double chimpFloor = measuredExposure ? -0.20 : -0.50;

        // A metered frame that already lands near a print mid tone does not
        // need nudging at all. Firing on every small gap is what made this a
        // second exposure decision rather than a correction to the first.
        const double deadband = measuredExposure ? 0.08 : 0.0;

        // How much room the TOP of the frame has left, in stops. p90 is far
        // too deep in the distribution to answer that: it stays under 0.90 —
        // and so leaves `headroom` at a full 1.0 — on frames whose brightest
        // tones are already against the ceiling. Ask the brightest tones.
        hiRoomEv = 2.2 * std::log2(kWhiteTarget / std::max(shot.chanHi, 0.05));

        if (shotMid < midTarget - deadband) {
            // Display ratio into stops: display ≈ linear^(1/2.2).
            const double evNeed = 2.2 * std::log2(midTarget / std::max(0.06, shotMid));
            const double headroom = unit((0.90 - shotP90) / 0.20);
            // Wanting a brighter mid tone is not permission to put the
            // highlights into the clip. Whichever runs out first wins.
            evAdd = std::min({chimpCeiling, evNeed * 0.45, std::max(0.0, hiRoomEv)})
                * headroom * (1.0 - 0.75 * protection);
        } else if (shotMid > midTarget + 0.10) {
            evAdd = std::max(chimpFloor,
                             -2.2 * std::log2(shotMid / (midTarget + 0.10)) * 0.40);
        }

        // A frame that is already welded to the ceiling has to come down
        // regardless of where its mid tone sits — this is the case the mid
        // tone test cannot see, because burning out the top barely moves the
        // median. Metering exposes to the right and the chimp only ever
        // pushed further; nothing before this could give exposure back.
        const double clipExcess = shot.clipHigh - kClipTolerance;
        if (clipExcess > 0.0) {
            const double recover = std::min(0.60, 2.2 * std::log2(
                1.0 + clipExcess * kClipRecoveryGain));
            evAdd = std::min(evAdd, -recover);
        } else if (hiRoomEv < 0.0) {
            evAdd = std::min(evAdd, std::max(-0.60, hiRoomEv));
        }

        if (std::abs(evAdd) > 0.01) {
            tone.expcomp = std::max(-1.5, std::min(2.75, tone.expcomp + evAdd));
            const double gain = std::pow(2.0, evAdd / 2.2);
            predictedP10 = std::min(0.99, shot.chanP10 * gain);
            predictedMid = std::min(0.97, shot.chanMid * gain);
            predictedP90 = std::min(0.995, shot.chanP90 * gain);
            predictedLumaMid = std::min(0.97, shot.lumaMid * gain);
            predictedLumaP10 = std::min(0.99, shot.lumaP10 * gain);
            predictedLumaP90 = std::min(0.995, shot.lumaP90 * gain);
            predictedLo = std::min(0.99, shot.chanLo * gain);
            predictedHi = std::min(1.0, shot.chanHi * gain);
        }
    }

    // Hand the finished tonal state to the look stages. Everything downstream
    // used to re-read the neutral render's numbers, which describe a picture
    // that is roughly a stop and a half darker than the one being graded.
    if (shotValid) {
        const double gain = std::pow(2.0, evAdd / 2.2);
        render.valid = true;
        render.mid = predictedLumaMid;
        render.p10 = predictedLumaP10;
        render.p90 = predictedLumaP90;
        render.p98 = std::min(0.999, shot.lumaP98 * gain);
        render.range = std::max(0.0, predictedLumaP90 - predictedLumaP10);
        // The shadow and highlight shares are counted on the rendered
        // histogram, so they need no gain correction beyond the chimp's own
        // trim; that trim is bounded to +/-0.20EV on a metered frame, which
        // moves these by less than the bin width they were counted in.
        render.shadowFraction = shot.lumaShadowFraction;
        render.highlightFraction = shot.lumaHighFraction;
        render.clippedFraction = shot.clipHigh;
        render.exposure = tone.expcomp;
        render.gain = std::pow(2.0, tone.expcomp / 2.2);
    }

    // The shadow lift genuinely needs the measurement, so it lands after it.
    // It is capped at 12 on a 0-100 control and only fires on frames that are
    // both blocked and wide, so leaving it out of the measured histogram costs
    // far less than leaving highlight recovery out did.
    int shadowLift = 0;

    if (shotValid) {
        const double blocked = unit((0.12 - predictedLumaP10) / 0.12);
        const double depth = unit(((predictedLumaP90 - predictedLumaP10) - 0.35) / 0.35);
        // Opening shadows is how a picture goes flat. Reserve it for frames
        // that are genuinely blocked AND carry a wide range — the deep end of
        // an ordinary photograph should stay deep, and the curve's toe, not
        // this control, decides how the darks read.
        double lift = 14.0 * blocked * depth;

        // Opening the shadows of a noisy frame opens its noise with them.
        lift *= 1.0 - 0.60 * noiseRisk;

        // A night scene keeps its own weight, in proportion to how much of a
        // night scene it actually is.
        lift *= 1.0 - 0.55 * darkFrameScore;

        shadowLift = std::max(0, std::min(12, static_cast<int>(std::round(lift))));
    }

    shadowsHighlights.enabled = highlightRecovery != 0 || shadowLift != 0;
    shadowsHighlights.shadows = shadowLift;
    shadowsHighlights.stonalwidth = 24 + shadowLift;
    shadowsHighlights.lab = false;

    // --- Tone curve -------------------------------------------------------
    //
    // Built the way a colorist builds one: the control points sit at fixed
    // stations spread across the range — a toe below the mids, a pivot on the
    // image's own mid tone, a shoulder up among the lights — and the image
    // decides how far each station MOVES. Using the image's p10/median/p90 as
    // the station POSITIONS (the previous design) put all three in the bottom
    // third of a normally exposed frame: the "highlight" station landed near
    // 0.25 and everything above it was one long unguided spline segment, which
    // is what sagged the right-hand side of the curve. Highlights get shaped
    // where highlights actually live, and white stays at white — holding the
    // highlights is the exposure metering's job (it targets no clipping), not
    // a squashed curve top.
    // Reuse the histogram read taken for the exposure decision, advanced by
    // whatever correction it prompted; a second full render per image would
    // buy almost nothing.
    //
    // Two distributions, two jobs. The STATION positions come from the channel
    // percentiles, because R, G and B are what index the curve. The risk and
    // flatness judgements below are questions about the picture ("is it
    // bright?", "is it already contrasty?") and stay on luma.
    const bool measured = shotValid;
    const double stationP10 = predictedP10;
    const double stationMid = predictedMid;
    const double stationP90 = predictedP90;

    const auto unitInterval = [](double value) {
        return std::max(0.0, std::min(1.0, value));
    };

    // Fallback path only: approximate the exposure gain in display space.
    const double expShift = std::pow(2.0, tone.expcomp / 2.2);
    const double displayMid = measured
        ? predictedLumaMid
        : unitInterval((features.valid ? features.medianLuma : 0.30) * expShift);
    const double displayP90 = measured
        ? predictedLumaP90
        : unitInterval((features.valid ? features.p90 : 0.60) * expShift);
    const double displayRange = measured
        ? std::max(0.0, predictedLumaP90 - predictedLumaP10)
        : (features.valid ? features.dynamicRange : 0.50);

    const double flatness = unitInterval((0.62 - displayRange) / 0.62);

    // Several stages already act on the upper half — auto exposure, highlight
    // recovery, global contrast, and this curve. Treat broad or near-clipped
    // highlights as the strongest warning, then blend in source brightness,
    // existing contrast, and positive exposure.
    //
    // "Are the highlights broad?" is a question about the frame this curve is
    // about to shape, so it is asked of the render. Asked of the source it
    // was unanswerable: across 98 frames the neutral render's highlight
    // fraction never once reached the 0.05 where this term starts to ramp, so
    // it contributed exactly zero to every decision below.
    const double renderHighlightFraction = measured
        ? shot.lumaHighFraction
        : (features.valid ? features.highlightFraction : 0.0);
    const double renderShadowFraction = measured
        ? shot.lumaShadowFraction
        : (features.valid ? features.shadowFraction : 0.30);
    const double broadHighlightRisk =
        unitInterval((renderHighlightFraction - 0.05) / 0.20);
    const double clippingRisk = features.valid
        ? unitInterval((features.clippedFraction - 0.002) / 0.018)
        : 0.0;
    const double upperTailRisk = unitInterval((displayP90 - 0.86) / 0.12);
    const double brightFrameRisk = unitInterval((displayMid - 0.52) / 0.20);
    const double existingContrastRisk = unitInterval((displayRange - 0.58) / 0.24);
    const double positiveExposureRisk = unitInterval(tone.expcomp / 1.0);

    double overtuneRisk = std::max(
        clippingRisk,
        unitInterval(
            0.27 * broadHighlightRisk
            + 0.27 * upperTailRisk
            + 0.18 * brightFrameRisk
            + 0.16 * existingContrastRisk
            + 0.12 * positiveExposureRisk));
    // How much of a portrait, and how much of a night scene, this frame is.
    // Darkness is re-scored against the render for the same reason the
    // classifier is: the source reading says almost every frame is dark.
    const double portraitness = features.scores.portrait;
    const double darkness = darkFrameScore;

    // A scene reading is a reason to be more careful, not a reason to stop
    // looking. These were floors — max(risk, 0.38) and max(risk, 0.48) — which
    // meant a frame that measured 0.12 risk had that measurement thrown away
    // and replaced by a constant because it happened to contain skin. The two
    // constants are the second and third most common values of overtuneRisk in
    // a real trace. Nudge in proportion instead, so the measurement still
    // decides the ordering between frames.
    overtuneRisk = std::min(1.0,
        overtuneRisk + 0.15 * portraitness + 0.22 * darkness);

    // Stations sit either side of the frame's tonal centre — the anchor the
    // curve turns around. Everything left of it is pulled down, everything
    // right of it is pushed up, and how far each goes is the image's own
    // decision. Exposure has already placed the centre, so the curve's whole
    // job here is contrast about that point.
    const double pivot = measured
        ? std::max(0.18, std::min(0.68, stationMid))
        : std::max(0.18, std::min(0.68, displayMid));

    // The stations sit INSIDE the frame's own tonal mass — between the anchor
    // and each end of where the pixels actually are. Placing them at fixed
    // fractions of the 0..1 range put them out in empty territory on a
    // low-contrast frame: the toe landed below the darkest pixel and the
    // shoulder above the brightest, so pulling them shaped nothing the
    // picture contained. That, not the amount of pull, is why the results
    // kept coming back flat.
    double lowSpan = measured
        ? std::max(0.04, pivot - stationP10)
        : pivot * 0.55;
    double highSpan = measured
        ? std::max(0.04, stationP90 - pivot)
        : (1.0 - pivot) * 0.45;

    // Keep the S balanced about its pivot. The skew of a frame's histogram
    // belongs in where the PIVOT sits, not in how lopsided the curve's shape
    // is — but with each station placed at 0.85 x its own span, a skewed
    // frame gets a wildly asymmetric curve. A rock face lit against deep
    // shade measures mid 0.23 with p90 0.84: lowSpan 0.11, highSpan 0.61.
    // That parked the shoulder at 0.746, out in a sparse highlight tail, and
    // since the lift is a share of the station's distance to the pivot it
    // then pushed that station up by 0.13 — dragging everything from the mid
    // to the highlights with it. The frame came back too bright, too
    // contrasty through the middle, and with the shoulder-to-white run
    // pinned against its 0.50 slope floor, so the brightest tones lost
    // separation as well. Neither side may now exceed twice the other, which
    // leaves genuinely balanced frames untouched.
    constexpr double kMaxSpanRatio = 2.0;
    const double spanRef = std::min(lowSpan, highSpan);
    lowSpan = std::min(lowSpan, spanRef * kMaxSpanRatio);
    highSpan = std::min(highSpan, spanRef * kMaxSpanRatio);

    // How far out the stations sit, as a share of each span.
    //
    // This was 0.85, and that width is what kept the curve feeling
    // constrained no matter how much strength it was given. A station way out
    // near the end of the data has a long lever to the pivot, so the same
    // slope demands a large absolute drop — which the 0→toe run cannot
    // absorb, so the shadow floor clips it. On three of five reference frames
    // the requested strength was simply being thrown away by that floor, and
    // the delivered inner slope came out BELOW what was asked for.
    //
    // Pulling the stations in is how the manual edit does it: a tight, steep
    // bump either side of the tonal centre, with the ends left close to
    // linear. Narrower stations mean the same slope needs a much smaller
    // move, so the floor stops binding, the ends keep their separation, and
    // the contrast lands where the pixels actually are.
    constexpr double kStationReach = 0.60;
    const double toeIn = std::max(0.02, pivot - kStationReach * lowSpan);
    const double shoulderIn = std::min(0.98, pivot + kStationReach * highSpan);

    // Both of these ask how much room the finished frame has left at one end,
    // so both read the render. On the neutral one they were stuck at opposite
    // rails — shadowFraction sat above 0.38 on 73% of frames, pinning
    // shadowRoom at zero, while highlightFraction never reached 0.17, pinning
    // highlightRoom at one. Two constants wearing the clothes of measurements,
    // and between them they are most of why the lift side ended up welded to
    // the toe on 92% of frames.
    const double shadowRoom = unitInterval((0.40 - renderShadowFraction) / 0.40);
    const double highlightRoom = unitInterval((0.17 - renderHighlightFraction) / 0.15);
    const double displayHeadroom = unitInterval((0.94 - displayP90) / 0.30);
    double liftPermission = highlightRoom * displayHeadroom
        * (1.0 - 0.65 * overtuneRisk)
        * (1.0 - 0.22 * portraitness)
        * (1.0 - 0.38 * darkness);

    // ONE contrast decision, applied to both sides of the anchor.
    //
    // The two sides used to come from unrelated polynomials with different
    // constants — density from (0.50 + 0.30 x flatness)(0.70 + 0.30 x
    // shadowRoom)(risk), lift from (0.45 + 0.30 x flatness)(0.45 + 0.55 x
    // liftPermission). Nothing tied them together, so on a typical frame,
    // which has plenty of highlight headroom but real shadow content, the
    // lift systematically came out larger than the density: a rock face
    // measured toe 0.479 against lift 0.550 and got pushed brighter than it
    // was pulled darker. That is a brightening move in a contrast curve's
    // clothing, and it is why frames kept reading too bright even after the
    // exposure was right.
    //
    // Now: a single contrast intent from the frame's flatness, damped once by
    // overall risk, and then reduced INDEPENDENTLY on each side by that
    // side's own protection — shadow room on the left, highlight headroom on
    // the right. Asymmetry can now only come from protection, never from the
    // shape of two different formulas.
    // The dampers are multiplicative, so widening any one of them quietly
    // weakens every frame. Making protection a (0.60 + 0.40 x room) term cost
    // a real portrait — shadowRoom 0.115, because it HAS shadows, like most
    // photographs — nearly a third of its curve: toe 0.343 down to 0.302, with
    // the symmetry clamp taking the lift down with it. Protection now has a
    // narrower range and the base carries more, so a typical frame lands near
    // 0.45-0.55 (inner slopes 1.45-1.55) instead of 0.30.
    // Raised alongside the narrower station reach above: with a shorter lever
    // to the pivot, a larger strength is both safe and necessary to reach the
    // same slope. Strength IS the inner slope minus one, so a typical frame
    // now runs at about 1.55 either side of the tonal centre.
    const double contrastIntent = (0.62 + 0.28 * flatness) * (1.0 - 0.28 * overtuneRisk);

    // Night keeps its shadow detail and mood; faces want a gentle falloff.
    const double sceneRestraint =
        (1.0 - 0.30 * darkness) * (1.0 - 0.12 * portraitness);

    double toeStrength = contrastIntent * (0.72 + 0.22 * shadowRoom) * sceneRestraint;
    toeStrength = std::max(0.0, std::min(0.75, toeStrength));

    double liftStrength = contrastIntent * (0.78 + 0.22 * liftPermission) * sceneRestraint;
    // An S-curve is contrast about the tonal centre, not a brightening
    // device. Protection may hold the lift back below the density, but it may
    // never push it above: brightening the highlights harder than the shadows
    // are deepened is exactly what the eye reads as "too bright".
    // Left as a hard ceiling on purpose. It bound on 92% of frames, but that
    // was the two room terms above being stuck at their rails, not this rule
    // being wrong: an S-curve that brightens the highlights harder than it
    // deepens the shadows reads as "too bright" whatever the frame. Now that
    // both sides are measured, the trace below reports whether it still bites.
    const double liftWanted = liftStrength;
    liftStrength = std::max(0.0, std::min({0.70, liftStrength, toeStrength}));
    const bool liftClamped = liftWanted > liftStrength + 1e-6;

    // Strength IS the slope here: pulling a station away from the anchor by
    // k x its distance to the anchor makes that segment run at 1+k. These
    // therefore read directly as the contrast either side of the tonal
    // centre, applied across the tones the frame actually holds.
    const double pivotOut = pivot;   // the anchor holds
    double toeOut = toeIn - toeStrength * (pivot - toeIn);
    double shoulderOut = shoulderIn + liftStrength * (shoulderIn - pivot);
    constexpr double whiteOut = 1.0;

    // Highlights keep their separation: the run from the shoulder to white
    // never falls below this slope.
    constexpr double minHighlightSlope = 0.50;
    shoulderOut = std::min(shoulderOut, whiteOut - minHighlightSlope * (1.0 - shoulderIn));

    // Shadows keep theirs too, and this guard matters MORE than the highlight
    // one because the run it protects is so much shorter. The pull above is
    // proportional to the toe's distance from the pivot, but the room it has
    // to fall into is only toeIn — and on a wide-range frame those are wildly
    // mismatched. A landscape measuring p10=0.07 against a pivot at 0.51 puts
    // the toe station at 0.139 and then drops it by 0.325 x 0.369 = 0.120,
    // i.e. to 0.019: the black-to-toe run collapses to slope 0.14 and the
    // spline clips everything below x=0.067 to pure black. The strength that
    // did that reads as a mild 0.325, because "strength" describes the
    // toe->pivot slope and says nothing about the run underneath it. Without
    // this floor, deep shadows guarantee a crushed toe no matter how gentle
    // the requested strength.
    constexpr double minShadowSlope = 0.55;
    toeOut = std::max(toeOut, minShadowSlope * toeIn);

    // Monotonic, with room between the stations.
    toeOut = std::max(0.0, std::min(toeOut, pivotOut - 0.03));
    shoulderOut = std::max(shoulderOut, pivotOut + 0.05);

    // --- Endpoints ---------------------------------------------------------
    //
    // Everything above shapes CONTRAST about the pivot and says nothing about
    // where the two ends of the frame finish. The curve ran 0->0 and 1->1, so
    // the finished black and white points were whatever exposure happened to
    // leave: a frame whose data stopped at 0.06 and 0.88 came back visibly
    // short of both limits and read as flat, while a hot one arrived with its
    // ends already pinned. Place them deliberately instead.
    //
    // These are stations like any other — the spline passes through them — so
    // the run outside each one is compressed or stretched, not clipped. Only
    // the ~0.1% of samples beyond them are affected in bulk.
    std::vector<double> master = {
        DCT_Spline,
        0.0, 0.0,
        toeIn, toeOut,
        pivot, pivotOut,
        shoulderIn, shoulderOut,
        1.0, whiteOut
    };

    bool blackPlaced = false;
    bool whitePlaced = false;

    if (measured) {
        // Pull each end a bounded share of the way to its target. Highlights
        // additionally defer to overtuneRisk, which already knows when a frame
        // is bright, contrasty or near-clipped and should be left alone.
        const double blackAuthority = kEndpointAuthority;
        const double whiteAuthority = kEndpointAuthority * (1.0 - 0.45 * overtuneRisk);

        // Cap the absolute move as well as the share.
        const auto boundedMove = [](double from, double target, double authority) {
            const double move = (target - from) * authority;
            return from + std::max(-kMaxEndpointMove, std::min(kMaxEndpointMove, move));
        };

        const double blackIn = predictedLo;
        const double whiteIn = predictedHi;
        const double blackOut = boundedMove(blackIn, kBlackTarget, blackAuthority);
        const double whiteOutPoint = boundedMove(whiteIn, kWhiteTarget, whiteAuthority);

        // A station only earns its place if it sits clear of the toe/shoulder
        // and keeps the curve monotonic. Frames whose data already reaches the
        // ends leave both out and keep the plain 0->0, 1->1 run.
        // The x-separation floors matter twice over: they keep the spline from
        // ringing between a new station and the fixed 0,0 / 1,1 corner, and
        // they skip the station entirely when the end is already where we
        // wanted it, which is the common case on a well-exposed frame.
        const bool blackUsable =
            blackIn > 0.012 && blackIn < toeIn - 0.02
            && blackOut > 0.0005 && blackOut < toeOut - 0.004;
        const bool whiteUsable =
            whiteIn < 0.988 && whiteIn > shoulderIn + 0.02
            && whiteOutPoint < 0.9995 && whiteOutPoint > shoulderOut + 0.004;

        whitePlaced = whiteUsable;
        blackPlaced = blackUsable;
        if (whiteUsable) {
            master.insert(master.end() - 2, {whiteIn, whiteOutPoint});
        }
        if (blackUsable) {
            master.insert(master.begin() + 3, {blackIn, blackOut});
        }
    }

    auto& curves = params.rgbCurves;
    curves.enabled = true;
    curves.lumamode = false;
    curves.mastercurve = std::move(master);
    curves.rcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
    curves.gcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};
    curves.bcurve = {DCT_Spline, 0.0, 0.0, 1.0, 1.0};

    // Diagnostics: set STEEP_FILESEL_LOG=1 to trace every Auto Edit decision
    // into %USERPROFILE%\steep-fileSel.log.
    fileBrowserPerfLog(
        "[autoWB] %s cam=%.0fK/%.3f auto=%.0fK/%.3f warmShadowShare=%.3f "
        "lowSun=%.3f -> applied=%d %.0fK/%.3f (mired move %+.1f)\n"
        "[autoNoise] iso=%u stops=%.2f luma=%.1f chroma=%.1f noiseRisk=%.2f\n",
        thumbnail.getFileName().c_str(),
        camTemp, camGreen, autoTemp, autoGreen,
        features.warmShadowShare, features.scores.lowSun,
        wbApplied ? 1 : 0,
        wbApplied ? params.wb.temperature : camTemp,
        wbApplied ? params.wb.green : camGreen,
        wbMiredMove,
        features.iso, isoStops, lumaNoise, chromaNoise, noiseRisk);

    fileBrowserPerfLog(
        "[autoCurve] ==== %s ====\n"
        "[autoCurve]  features: valid=%d scene=%s medLuma=%.3f p10=%.3f p90=%.3f p98=%.3f range=%.3f\n"
        "[autoCurve]            shadowFrac=%.3f hlFrac=%.3f clipFrac=%.4f sat=%.3f iso=%u\n"
        "[autoCurve]            skinFrac=%.3f centerSkin=%.3f skinSat=%.3f sky=%.3f foliage=%.3f\n"
        "[autoCurve]  protection=%.3f neutralFlatness=%.3f measuredExposure=%d\n"
        "[autoCurve]  exposure: expcomp=%.3f bright=%d contrast=%d hlcompr=%d shcompr=%d evAdd=%.3f\n"
        "[autoCurve]  midTarget=%.2f darkFrame=%.3f shotValid=%d sh: hl=%d shadows=%d\n"
        "[autoCurve]  stations from channel: p10=%.3f mid=%.3f p90=%.3f\n"
        "[autoCurve]  picture from luma:     p10=%.3f mid=%.3f p90=%.3f range=%.3f\n"
        "[autoCurve]  flatness=%.3f overtuneRisk=%.3f darkness=%.3f liftPermission=%.3f\n"
        "[autoCurve]  scores: portrait=%.3f lowSun=%.3f open=%.3f dark=%.3f urban=%.3f\n"
        "[autoCurve]  spans: lowSpan=%.4f highSpan=%.4f  (floor 0.04 hit: low=%d high=%d)\n"
        "[autoCurve]  strengths: toe=%.3f lift=%.3f (wanted %.3f clamped=%d)\n"
        "[autoCurve]  rooms: shadowRoom=%.3f highlightRoom=%.3f displayHeadroom=%.3f\n"
        "[autoCurve]  render fractions: shadow=%.4f high=%.4f  (source shadow=%.4f high=%.4f)\n"
        "[autoCurve]  endpoints: measured lo=%.4f hi=%.4f  clipLow=%.4f clipHigh=%.4f\n"
        "[autoCurve]             after exposure lo=%.4f hi=%.4f  hiRoomEv=%.3f\n"
        "[autoCurve]             placed black=%d white=%d  (targets %.3f / %.3f)\n"
        "[autoCurve]  CURVE: 0,0  %.5f,%.5f  %.5f,%.5f  %.5f,%.5f  1,1\n"
        "[autoCurve]  chord slopes: black->toe=%.3f toe->pivot=%.3f pivot->shldr=%.3f shldr->white=%.3f\n",
        thumbnail.getFileName().c_str(),
        features.valid ? 1 : 0, autoGradeSceneName(features.scene),
        features.medianLuma, features.p10, features.p90, features.p98, features.dynamicRange,
        features.shadowFraction, features.highlightFraction, features.clippedFraction,
        features.saturation, features.iso,
        features.skinFraction, features.centerSkinFraction, features.skinSaturation,
        features.skyFraction, features.foliageFraction,
        protection, neutralFlatness, measuredExposure ? 1 : 0,
        tone.expcomp, tone.brightness, tone.contrast, tone.hlcompr, tone.shcompr, evAdd,
        midTarget, darkFrameScore, shotValid ? 1 : 0, highlightRecovery, shadowLift,
        stationP10, stationMid, stationP90,
        predictedLumaP10, displayMid, displayP90, displayRange,
        flatness, overtuneRisk, darkness, liftPermission,
        features.scores.portrait, features.scores.lowSun, features.scores.open,
        features.scores.dark, features.scores.urban,
        lowSpan, highSpan,
        (measured && (pivot - stationP10) < 0.04) ? 1 : 0,
        (measured && (stationP90 - pivot) < 0.04) ? 1 : 0,
        toeStrength, liftStrength, liftWanted, liftClamped ? 1 : 0,
        shadowRoom, highlightRoom, displayHeadroom,
        renderShadowFraction, renderHighlightFraction,
        features.shadowFraction, features.highlightFraction,
        shot.chanLo, shot.chanHi, shot.clipLow, shot.clipHigh,
        predictedLo, predictedHi, hiRoomEv,
        blackPlaced ? 1 : 0, whitePlaced ? 1 : 0, kBlackTarget, kWhiteTarget,
        toeIn, toeOut, pivot, pivotOut, shoulderIn, shoulderOut,
        toeIn > 0 ? toeOut / toeIn : 0.0,
        (pivotOut - toeOut) / std::max(1e-6, pivot - toeIn),
        (shoulderOut - pivotOut) / std::max(1e-6, shoulderIn - pivot),
        (1.0 - shoulderOut) / std::max(1e-6, 1.0 - shoulderIn));
}

// One scene's contribution to the grade. Every field is numeric so recipes can
// be mixed; the wheels are mixed as vectors rather than as angles, below.
struct GradeRecipe {
    double shadowsHue, shadowsSat, shadowsLum;
    double midtonesHue, midtonesSat, midtonesLum;
    double highlightsHue, highlightsSat, highlightsLum;
    double blending, balance;
    double pastels, saturated, contrast;
    double splitDepth;   // multiplier on the channel-curve separation
};

const GradeRecipe kGradeNeutral{
    218.0, 0.085,  1.0,   32.0, 0.030, 1.0,   42.0, 0.075, -1.0,
    70.0,   0.0,   14.0,  2.0,  2.0,   1.00};
const GradeRecipe kGradePortrait{
    218.0, 0.050,  1.0,   28.0, 0.045, 1.5,   40.0, 0.055, -0.5,
    74.0,   8.0,   10.0, -2.0,  1.0,   0.75};
const GradeRecipe kGradeLowSun{
    210.0, 0.090,  1.0,   32.0, 0.055, 1.0,   45.0, 0.110, -1.5,
    68.0,   5.0,   12.0,  0.0,  1.0,   1.15};
const GradeRecipe kGradeOpen{
    216.0, 0.080,  1.0,  150.0, 0.025, 1.0,   43.0, 0.075, -1.0,
    70.0,  -4.0,   18.0,  3.0,  3.0,   1.00};
const GradeRecipe kGradeDark{
    220.0, 0.120, -1.0,  198.0, 0.040, 0.5,   35.0, 0.080, -1.0,
    64.0, -12.0,   9.0,  -3.0,  2.0,   1.35};
const GradeRecipe kGradeUrban{
    212.0, 0.090,  1.0,   30.0, 0.020, 1.0,   38.0, 0.065, -1.0,
    66.0,  -3.0,   11.0,  0.0,  3.0,   1.10};

// A colour wheel mixed as an angle is nonsense: a frame that is half open
// landscape (green midtones, 150 degrees) and half low sun (orange, 32) would
// average to 91 and come back yellow-green, a cast neither recipe asked for.
// Mixed as vectors, two tints that disagree cancel toward neutral, which is
// what "the picture is a bit of both" should actually look like.
struct WheelSum {
    double x = 0.0;
    double y = 0.0;

    void add(double hueDeg, double sat, double weight)
    {
        const double radians = hueDeg * rtengine::RT_PI / 180.0;
        x += std::cos(radians) * sat * weight;
        y += std::sin(radians) * sat * weight;
    }

    double hue() const
    {
        if (std::abs(x) < 1e-9 && std::abs(y) < 1e-9) {
            return 0.0;
        }
        const double degrees = std::atan2(y, x) * 180.0 / rtengine::RT_PI;
        return degrees < 0.0 ? degrees + 360.0 : degrees;
    }

    double sat() const { return std::sqrt(x * x + y * y); }
};

void applySteepAutoGrade(
    const AutoGradeFeatures& features,
    const AutoEditRender& render,
    rtengine::procparams::ProcParams& params)
{
    // Weights are the scores themselves, with neutral taking up whatever slack
    // is left. A frame that is 0.8 portrait and 0.5 dark gets both, in
    // proportion -- where the switch this replaces would have picked one and
    // discarded the other entirely.
    const AutoSceneScores& scores = features.scores;
    const double claimed = scores.portrait + scores.lowSun + scores.open
                         + scores.dark + scores.urban;
    const double neutralWeight = std::max(0.0, 1.0 - claimed);
    const double total = std::max(1e-6, claimed + neutralWeight);

    const struct { const GradeRecipe* recipe; double weight; } mix[] = {
        {&kGradeNeutral,  neutralWeight / total},
        {&kGradePortrait, scores.portrait / total},
        {&kGradeLowSun,   scores.lowSun / total},
        {&kGradeOpen,     scores.open / total},
        {&kGradeDark,     scores.dark / total},
        {&kGradeUrban,    scores.urban / total},
    };

    WheelSum shadows;
    WheelSum midtones;
    WheelSum highlights;

    double shadowsLum = 0.0;
    double midtonesLum = 0.0;
    double highlightsLum = 0.0;
    double blending = 0.0;
    double balance = 0.0;
    double pastels = 0.0;
    double saturated = 0.0;
    double gradeContrast = 0.0;
    double splitDepth = 0.0;

    for (const auto& entry : mix) {
        const GradeRecipe& r = *entry.recipe;
        const double w = entry.weight;

        shadows.add(r.shadowsHue, r.shadowsSat, w);
        midtones.add(r.midtonesHue, r.midtonesSat, w);
        highlights.add(r.highlightsHue, r.highlightsSat, w);

        shadowsLum += r.shadowsLum * w;
        midtonesLum += r.midtonesLum * w;
        highlightsLum += r.highlightsLum * w;
        blending += r.blending * w;
        balance += r.balance * w;
        pastels += r.pastels * w;
        saturated += r.saturated * w;
        gradeContrast += r.contrast * w;
        splitDepth += r.splitDepth * w;
    }

    auto& grade = params.colorGrading;
    grade = rtengine::procparams::ColorGradingParams();
    grade.enabled = true;
    grade.shadowsHue = shadows.hue();
    grade.shadowsSat = shadows.sat();
    grade.shadowsLum = shadowsLum;
    grade.midtonesHue = midtones.hue();
    grade.midtonesSat = midtones.sat();
    grade.midtonesLum = midtonesLum;
    grade.highlightsHue = highlights.hue();
    grade.highlightsSat = highlights.sat();
    grade.highlightsLum = highlightsLum;
    grade.blending = blending;
    grade.balance = balance;

    // A grade should be visibly distinct from the technical correction while
    // preserving skin and already-saturated colors. The RGB curves create a
    // restrained cool-shadow/warm-highlight separation; Vibrance supplies
    // scene-aware chroma without turning the grade into a film simulation.
    auto& vibrance = params.vibrance;
    vibrance = rtengine::procparams::VibranceParams();
    vibrance.enabled = true;
    vibrance.pastels = static_cast<int>(std::round(pastels));
    vibrance.saturated = static_cast<int>(std::round(saturated));
    vibrance.protectskins = true;
    vibrance.avoidcolorshift = true;
    vibrance.pastsattog = true;

    // The channel curves ARE the grade's colour separation, and they used to be
    // one fixed shape across all six scenes -- the wheels varied, the thing
    // doing the work did not. They carry the blended depth now, so a night
    // frame separates harder than a portrait does.
    auto& curves = params.rgbCurves;
    curves.rcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.18 - 0.012 * splitDepth,
        0.50, 0.50 + 0.008 * splitDepth,
        0.82, 0.82 + 0.022 * splitDepth,
        1.0, 1.0
    };
    curves.gcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.18 + 0.002 * splitDepth,
        0.50, 0.50,
        0.82, 0.82 - 0.002 * splitDepth,
        1.0, 1.0
    };
    curves.bcurve = {
        DCT_Spline,
        0.0, 0.0,
        0.18, 0.18 + 0.018 * splitDepth,
        0.50, 0.50 - 0.002 * splitDepth,
        0.82, 0.82 - 0.022 * splitDepth,
        1.0, 1.0
    };

    params.toneCurve.contrast = std::min(45,
        params.toneCurve.contrast + static_cast<int>(std::round(gradeContrast)));
    params.sh.highlights = std::min(45, params.sh.highlights + 3);

    // Judged on the rendered frame: on the neutral render this test read
    // shadowFraction > 0.34 as true on 80% of frames and highlightFraction
    // < 0.16 as true on all of them, so it was not a condition.
    const double shadowShare = render.valid ? render.shadowFraction : features.shadowFraction;
    const double highlightShare = render.valid ? render.highlightFraction : features.highlightFraction;

    if ((render.valid || features.valid) && shadowShare > 0.34 && highlightShare < 0.16) {
        params.sh.shadows = std::min(30, params.sh.shadows + 1);
    }

    fileBrowserPerfLog(
        "[autoGrade] weights neutral=%.2f portrait=%.2f lowSun=%.2f open=%.2f dark=%.2f urban=%.2f\n"
        "[autoGrade]   wheels sh=%.0f/%.3f mid=%.0f/%.3f hi=%.0f/%.3f blend=%.1f bal=%.1f\n"
        "[autoGrade]   vibrance pastels=%d saturated=%d contrast=%+.1f splitDepth=%.2f\n",
        mix[0].weight, mix[1].weight, mix[2].weight, mix[3].weight,
        mix[4].weight, mix[5].weight,
        grade.shadowsHue, grade.shadowsSat, grade.midtonesHue, grade.midtonesSat,
        grade.highlightsHue, grade.highlightsSat, grade.blending, grade.balance,
        vibrance.pastels, vibrance.saturated, gradeContrast, splitDepth);

    params.filmPresets.enabled = false;
}


void applySteepAutoFilm(
    const AutoGradeFeatures& features,
    const AutoEditRender& render,
    rtengine::procparams::ProcParams& params)
{
    // The negative going into the film stage is the frame Auto Edit rendered,
    // not the neutral one. Measured over a real library, the neutral render
    // reports a highlight fraction of 0.000 at the median and never above
    // 0.104 -- so every highlight threshold below used to be unreachable, and
    // every median-luma threshold used to be permanently true. These three
    // read the picture the film actually receives.
    const bool measured = render.valid;
    const double frameMid = measured ? render.mid : features.medianLuma;
    const double frameRange = measured ? render.range : features.dynamicRange;
    const double frameHighlights = measured ? render.highlightFraction : features.highlightFraction;

    // "Dark" gets ONE definition, shared with the tone stage.
    const bool darkFrame = measured
        ? frameMid < 0.26
        : features.valid && features.medianLuma < 0.30;

    auto& film = params.filmPresets;
    film = rtengine::procparams::FilmPresetsParams();
    film.enabled = true;
    film.modelVersion = 4;
    film.preset = "sovereign";
    film.process = "c41";
    film.output = "ra4";
    film.format = "35mm";
    // Film Lab v2 could only subtract contrast, so this had to be kept near a
    // third to stay tolerable, which is also why the stock barely registered.
    // v3 carries a real print gamma, so the stock can be applied as a stock.
    film.strength = 62;
    // Place the digital negative near the stock's intended middle gray. The
    // film shoulder already protects bright values; routinely underexposing
    // the film stage only makes the print muddy and buries useful midtones.
    film.exposure = frameHighlights > 0.24 && !darkFrame
        ? -0.05
        : frameHighlights > 0.18 && !darkFrame
          ? -0.02
          : frameMid < 0.20
            ? 0.07
            : 0.0;
    film.fade = -4;
    film.rolloff = frameHighlights > 0.22 ? 4 : 0;
    film.saturation = features.saturation > 0.48 ? -5 : features.saturation < 0.20 ? 2 : 0;
    film.contrast = frameRange < 0.38 ? 6 : frameRange > 0.72 ? 3 : 4;
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
        std::min(14, static_cast<int>(std::round(5.0 + frameHighlights * 40.0))));
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

    // The stock is chosen, the handling is blended.
    //
    // A film stock is a physical object: you cannot load 60% of one emulsion
    // and 40% of another, and a process is C-41 or it is E-6. So the dominant
    // trait picks the stock, the process and the paper -- while everything
    // that is genuinely a matter of degree (print strength, contrast, fade,
    // shoulder, warmth, skin protection) blends across all five scores. A
    // golden-hour portrait gets porcelain on RA-4 with low sun's warmth and
    // shoulder worked into it, instead of one recipe winning and the other
    // being discarded.
    struct FilmHandling {
        double strength, contrast, fade, rolloff, saturation, warmth,
               skinProtection, softness, halation, vibrance, exposure;
    };

    const double baseHalation = film.halation;
    const double baseRolloff = film.rolloff;
    const double baseSaturation = film.saturation;
    const double baseContrast = film.contrast;
    const double baseExposure = film.exposure;

    const bool chrome = features.saturation > 0.35 && frameHighlights < 0.13;

    const FilmHandling handling[] = {
        // neutral
        {62, baseContrast, -4, baseRolloff, baseSaturation, 0, 68, 2,
         baseHalation, 0, baseExposure},
        // portrait
        {60, 5, -3, 2, baseSaturation - 2, 0, 90, 3,
         std::min(baseHalation, 11.0), 0, baseExposure},
        // low sun
        {66, 4, -4, std::max(baseRolloff, 2.0), baseSaturation, -1, 78, 2,
         std::min(14.0, baseHalation + 2), 0, baseExposure},
        // open
        {chrome ? 52.0 : 62.0, chrome ? -8.0 : 5.0, chrome ? -3.0 : -4.0,
         chrome ? -4.0 : baseRolloff, baseSaturation - (chrome ? 3 : 0), 0, 68, 2,
         std::min(baseHalation, 10.0), features.saturation < 0.24 ? 3.0 : 0.0,
         baseExposure},
        // dark
        {66, 8, 6, 4, baseSaturation, -2, 68, 2,
         std::min(18.0, baseHalation + 4), 0, 0.08},
        // urban
        {63, 2, -6, 0, baseSaturation, 0, 68, 2,
         std::min(baseHalation, 11.0), 0, baseExposure},
    };

    const AutoSceneScores& scores = features.scores;
    const double claimed = scores.portrait + scores.lowSun + scores.open
                         + scores.dark + scores.urban;
    const double neutralWeight = std::max(0.0, 1.0 - claimed);
    const double total = std::max(1e-6, claimed + neutralWeight);
    const double weights[] = {
        neutralWeight / total,
        scores.portrait / total,
        scores.lowSun / total,
        scores.open / total,
        scores.dark / total,
        scores.urban / total,
    };

    FilmHandling mixed{};

    for (size_t i = 0; i < sizeof(weights) / sizeof(weights[0]); ++i) {
        const FilmHandling& h = handling[i];
        const double w = weights[i];
        mixed.strength += h.strength * w;
        mixed.contrast += h.contrast * w;
        mixed.fade += h.fade * w;
        mixed.rolloff += h.rolloff * w;
        mixed.saturation += h.saturation * w;
        mixed.warmth += h.warmth * w;
        mixed.skinProtection += h.skinProtection * w;
        mixed.softness += h.softness * w;
        mixed.halation += h.halation * w;
        mixed.vibrance += h.vibrance * w;
        mixed.exposure += h.exposure * w;
    }

    const auto toInt = [](double value) {
        return static_cast<int>(std::round(value));
    };

    film.strength = toInt(mixed.strength);
    film.contrast = toInt(mixed.contrast);
    film.fade = toInt(mixed.fade);
    film.rolloff = toInt(mixed.rolloff);
    film.saturation = toInt(mixed.saturation);
    film.warmth = toInt(mixed.warmth);
    film.skinProtection = toInt(mixed.skinProtection);
    film.outputSoftness = toInt(mixed.softness);
    film.halation = toInt(mixed.halation);
    film.vibrance = toInt(mixed.vibrance);
    film.exposure = mixed.exposure;

    // The emulsion, the process and the paper come from the dominant trait --
    // and that trait is now argmax over the scores, not whichever branch of an
    // if/else chain happened to be tested first.
    //
    // Within a trait there is then a second question, because "dark" covers
    // both dusk and midnight and "open" covers both a blue hour lake and a
    // desert at noon. Six of the seventeen stocks in the catalogue were
    // reachable; these gates take it to eleven. Every one of them is a swap
    // inside the same family -- a different negative, a different slide -- and
    // never a change of what kind of picture is being made. Automatic
    // monochrome is deliberately NOT among them: converting a colour
    // photograph to black and white off a saturation threshold is a far
    // stronger decision than anything else here, and belongs to the
    // photographer.
    const bool coolDominant =
        features.coolFraction > features.warmFraction * 1.25
        && features.coolFraction > 0.10;
    const bool flatAndDrab = frameRange < 0.34 && features.saturation < 0.22;

    switch (features.scene) {
        case AutoGradeScene::Portrait:
            film.preset = "porcelain_400";
            film.process = "c41";
            film.output = "ra4";
            break;
        case AutoGradeScene::GoldenHour:
            if (scores.open > 0.35 && features.saturation > 0.38) {
                // A saturated warm landscape is what the warm slide is for.
                film.preset = "desert_chrome";
                film.process = "e6";
                film.output = "projection";
            } else if (scores.lowSun < 0.55) {
                // Warm, but not decisively low sun: a gentler warm negative
                // than golden_hour, which is built for the real thing.
                film.preset = "heritage_gold";
                film.process = "c41";
                film.output = "ra4";
            } else {
                film.preset = "golden_hour";
                film.process = "c41";
                film.output = "ra4";
            }
            break;
        case AutoGradeScene::Landscape:
            // Saturation is a colour reading and stays on the source; the
            // highlight share is a tonal one and comes off the render.
            if (coolDominant && features.saturation > 0.24) {
                // Blue hour, snow, open water: a cool slide, not a warm one.
                film.preset = "arctic";
                film.process = "e6";
                film.output = "projection";
            } else if (chrome) {
                film.preset = "vivid_chrome";
                film.process = "e6";
                film.output = "projection";
            } else {
                film.preset = "sovereign";
                film.process = "c41";
                film.output = "ra4";
            }
            break;
        case AutoGradeScene::Night:
            if (scores.dark < 0.65) {
                // Dusk rather than night. The 500T is a stock for actual
                // darkness; twilight_160 is the same family, half the push.
                film.preset = "twilight_160";
                film.process = "ecn2";
                film.output = "cinema";
            } else {
                film.preset = "cinematic_500t";
                film.process = "ecn2";
                film.output = "cinema";
            }
            break;
        case AutoGradeScene::Urban:
            film.preset = "street_800";
            film.process = "c41";
            film.output = "ra4";
            break;
        case AutoGradeScene::Neutral:
            if (flatAndDrab) {
                // Nothing to lose: a soft, faded stock suits material that
                // never had contrast, where sovereign just looks thin.
                film.preset = "nostalgia_200";
            } else {
                film.preset = "sovereign";
            }
            film.process = "c41";
            film.output = "ra4";
            break;
    }

    fileBrowserPerfLog(
        "[autoFilm] stock=%s/%s/%s weights neutral=%.2f portrait=%.2f lowSun=%.2f "
        "open=%.2f dark=%.2f urban=%.2f\n"
        "[autoFilm]   strength=%d contrast=%d fade=%d rolloff=%d sat=%d warmth=%d "
        "halation=%d skinProt=%d exposure=%.3f\n",
        film.preset.c_str(), film.process.c_str(), film.output.c_str(),
        weights[0], weights[1], weights[2], weights[3], weights[4], weights[5],
        film.strength, film.contrast, film.fade, film.rolloff, film.saturation,
        film.warmth, film.halation, film.skinProtection, film.exposure);

    // Grain is deliberately NOT part of the auto film recipe. The film stage
    // is grain-free by design, and texture is a taste decision the user makes
    // themselves through Effects > Grain - an auto edit that ships ~20
    // strength of grain on every frame took that choice away.
    //
    // Note that the film.grain write above reaches nothing either way: the
    // engine reads grainSize, grainClumping and grainColor, but never grain
    // itself. Which means ISO -- the only thing that write consumes -- is
    // measured by this pipeline and then has no effect on any output.

    const bool darkPrint = darkFrame;
    if (darkPrint) {
        // A dark negative needs a longer print exposure and a softer toe, not
        // another contrast pass. Preserve local color and texture while
        // keeping the scene recognizably low key.
        film.exposure = std::max(film.exposure, 0.10);
        film.fade = std::max(film.fade, 5);
        film.rolloff = std::max(film.rolloff, 4);
        const int darkFilmContrast = features.scene == AutoGradeScene::Night
            ? (frameRange < 0.18 ? 8 : 10)
            : (frameRange < 0.18 ? 3 : 5);
        film.contrast = std::min(film.contrast, darkFilmContrast);
    }

    // The exposure, the shadow lift and the tone curve belong to Auto Edit,
    // and Auto Grade only ever nudges them. This used to be the one mode that
    // overrode them instead: exposure clamped to +0.38EV (down to +0.12 on
    // bright frames), brightness to 3, and the measured shadow lift — which
    // Auto Edit derives from a real render and can take to 12 — crushed to 1.
    //
    // Those caps were calibrated against Film Lab v2, which barely changed the
    // tone, plus a fixed print curve that lifted the upper mid tones (0.52 to
    // 0.625) and quietly paid for them. Both are gone: v2's replacement
    // carries a real system gamma that darkens everything below the pivot, and
    // the lifting curve was removed. Keeping the caps on top of that is what
    // made Auto Edit + Film land darker than Auto Edit alone on the same
    // frame. Auto Edit measured the mid tone; let it own the result.
    //
    // Two things legitimately stay. The first is the dark-frame exposure
    // FLOOR: a thin negative needs a longer print exposure, and v3's system
    // gamma darkens everything under the pivot, so this matters more now than
    // it did, not less. Only the ceiling above it was doing harm.
    // The tiers keep their original shape (roughly the lower half and the
    // upper three quarters of the dark band) but are rebased into render
    // space along with the gate above them.
    if (darkFrame) {
        const bool night = features.scene == AutoGradeScene::Night;
        double exposureFloor;
        if (frameMid < 0.12) {
            exposureFloor = night ? 0.24 : 0.36;
        } else if (frameMid < 0.20) {
            exposureFloor = night ? 0.16 : 0.28;
        } else {
            exposureFloor = night ? 0.08 : 0.16;
        }
        params.toneCurve.expcomp = std::max(params.toneCurve.expcomp, exposureFloor);
    }

    // The second is the double-compression guard: film's shoulder and hlcompr
    // both pull highlights down, and on a dark frame the input contrast and
    // the print gamma both steepen the same mid tones.
    if (darkPrint) {
        const int inputContrastCeiling = frameRange < 0.18
            ? 10
            : frameRange < 0.35 ? 12 : 14;
        params.toneCurve.contrast = std::min(params.toneCurve.contrast, inputContrastCeiling);
    }

    // Paper-grade compensation. Printing a flat negative through a soft film
    // recipe washes the image out — the darkroom answer is a harder paper
    // grade, not the same soft one. Estimate the wash-out risk from the
    // source's measured flatness plus how soft this recipe ended up, then
    // harden the film stage in proportion: firmer contrast, less black lift,
    // and a shorter print shoulder.
    //
    // This used to also spend its correction on an RGB master curve, whose
    // upper segments ran at slope 0.82 and 0.70 and lifted 0.52 to 0.625 —
    // measurably flattening and raising exactly the highlights that read as
    // washed, and doing it *harder* the more wash it detected. The film
    // output gamma owns print contrast now, so the curve is gone and the
    // correction goes where the problem is.
    if (measured || features.valid) {
        const double sourceFlatness = std::max(0.0, (0.55 - frameRange) / 0.55);
        const double fadeLift = std::max(0, film.fade) / 10.0;
        const double softContrast = std::max(0, 8 - film.contrast) / 16.0;
        const double washRisk = std::min(1.0, 0.65 * sourceFlatness + 0.20 * fadeLift + 0.15 * softContrast);

        if (washRisk > 0.25) {
            film.contrast = std::min(12, film.contrast + static_cast<int>(std::round(6.0 * washRisk)));

            // Don't lift blacks the source never had. Scaled by flatness, so
            // deliberate lifted looks (Night's cinematic fade) survive on
            // sources with genuine contrast.
            if (film.fade > 0 && sourceFlatness > 0.30) {
                film.fade = std::max(-2, film.fade - static_cast<int>(std::round(8.0 * sourceFlatness)));
            }

            // Shorten the print shoulder so highlights climb to white instead
            // of settling below it, which is where wash actually shows.
            film.rolloff = std::max(-12, film.rolloff - static_cast<int>(std::round(10.0 * washRisk)));

            // A washed print also reads desaturated; give the dyes a nudge.
            if (washRisk > 0.5) {
                film.saturation = std::min(6, film.saturation + 2);
            }
        }
    }

    // The film stock's own shoulder is now the highlight rendering. Leaving
    // the exposure stage's highlight compression at up to 50 on top of it put
    // three compressors in series (hlcompr, film shoulder, print curve), which
    // is the rest of the wash.
    //
    // Shadow compression is NOT the mirror of that and must not be capped
    // alongside it: shcompr lifts the darks while v3's print toe deepens them,
    // so the two oppose rather than stack. Capping it here only made the
    // shadows darker still.
    params.toneCurve.hlcompr = std::min(params.toneCurve.hlcompr, 15);

    // Film Lab v4 re-applies the display grading - the Contrast slider and
    // the Auto Edit master S-curve - onto the film's own render as a print
    // grade. Those numbers were tuned for a chain where they acted once; on
    // top of the emulsion's H&D and the print gamma they read as over-
    // separated and slightly dark, so hand back a share of each under film.
    //
    // The hand-back started at 0.55/0.60 and read FLAT: an S-curve is also
    // a chroma amplifier (steep mids multiply channel differences), so film
    // frames came back both softer and duller than graded ones. With the
    // base and per-scene contrast trims carrying most of the correction
    // now, film keeps 75% of the slider and 80% of the curve.
    params.toneCurve.contrast = static_cast<int>(std::round(params.toneCurve.contrast * 0.75));

    auto& master = params.rgbCurves.mastercurve;
    if (master.size() > 5 && static_cast<int>(master[0]) == DCT_Spline) {
        for (size_t i = 1; i + 1 < master.size(); i += 2) {
            master[i + 1] = master[i] + (master[i + 1] - master[i]) * 0.80;
        }
    }
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
    AutoGradeFeatures features = analyzeSteepAutoGrade(thumbnail);
    AutoEditRender render;
    applySteepAutoEdit(thumbnail, features, result, render);
    restoreSteepAutoEditGeometry(source, result);

    // Now that the frame has been rendered, ask what it is a second time.
    //
    // The first answer came off a neutral render whose median sits around 0.18
    // on a real library, so "medianLuma < 0.29" -- the night test -- was true
    // on 92% of frames and the label stopped carrying information. The tone
    // stage already worked around this with its own nightLook gate; the look
    // stages did not, so a third of the library was getting a night cine stock
    // laid onto an ordinary daylight photograph.
    const AutoGradeScene provisional = features.scene;

    if (render.valid) {
        features.scores = scoreSteepAutoScene(
            features,
            render.mid,
            render.shadowFraction,
            render.range,
            brightWarmFractionAtGain(features, render.gain));
        features.scene = dominantScene(features.scores);
    }

    fileBrowserPerfLog(
        "[autoScene] %s provisional=%s final=%s renderValid=%d "
        "renderMid=%.3f renderRange=%.3f renderShadow=%.3f renderHigh=%.3f "
        "gain=%.3f brightWarm=%.4f->%.4f  (source mid=%.3f range=%.3f shadow=%.3f high=%.3f)\n",
        thumbnail.getFileName().c_str(),
        autoGradeSceneName(provisional),
        autoGradeSceneName(features.scene),
        render.valid ? 1 : 0,
        render.mid, render.range, render.shadowFraction, render.highlightFraction,
        render.gain,
        features.brightWarmFraction,
        brightWarmFractionAtGain(features, render.gain),
        features.medianLuma, features.dynamicRange,
        features.shadowFraction, features.highlightFraction);

    if (mode == AutoEditMode::Grade) {
        applySteepAutoGrade(features, render, result);
    } else if (mode == AutoEditMode::GradeFilm) {
        applySteepAutoFilm(features, render, result);
    } else if (mode == AutoEditMode::GradedFilm) {
        // Order matters: the grade disables filmPresets (it is a look of its
        // own), so the film stage runs after and re-enables its pipeline.
        // Film's exposure/contrast lanes and print curve legitimately win.
        applySteepAutoGrade(features, render, result);
        applySteepAutoFilm(features, render, result);
        harmonizeSteepGradeWithFilm(result);
    }

    return features;
}

}

AutoGradeFeatures buildSteepAutoEditParamsFeatures(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result)
{
    return buildSteepAutoEditParamsInternal(thumbnail, mode, source, result);
}

void buildSteepAutoEditParams(
    Thumbnail& thumbnail,
    SteepAutoEditMode mode,
    const rtengine::procparams::ProcParams& source,
    rtengine::procparams::ProcParams& result)
{
    buildSteepAutoEditParamsInternal(thumbnail, mode, source, result);
}
