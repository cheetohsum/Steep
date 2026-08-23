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

// The one place that decides what a picture IS.
//
// Content evidence (skin, sky, foliage, edges) comes off the source and is
// stable; the tonal evidence is passed in, because it has to be asked twice --
// once about the neutral render, and again about the frame Auto Edit actually
// produced. Answering it once, on the neutral render, is what made every
// third frame in a real library come back labelled "night".
AutoGradeScene classifySteepAutoGrade(
    const AutoGradeFeatures& features,
    double medianLuma,
    double shadowFraction,
    double dynamicRange,
    double brightWarmFraction)
{
    if (features.skinFraction > 0.045
            && features.centerSkinFraction > 0.075
            && features.skinSaturation < 0.44
            && (features.saturation < 0.33 || dynamicRange > 0.28)) {
        return AutoGradeScene::Portrait;
    }

    if (brightWarmFraction > 0.085
            && features.warmFraction > features.coolFraction * 1.22) {
        return AutoGradeScene::GoldenHour;
    }

    if (medianLuma < 0.29 && shadowFraction > 0.38) {
        return AutoGradeScene::Night;
    }

    if ((features.skyFraction > 0.07 || features.foliageFraction > 0.13)
            && features.skinFraction < 0.035) {
        return AutoGradeScene::Landscape;
    }

    if (features.edgeDensity > 0.075 && features.foliageFraction < 0.10) {
        return AutoGradeScene::Urban;
    }

    return AutoGradeScene::Neutral;
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

    for (size_t i = 0; i < warmLumaBins.size(); ++i) {
        features.warmLumaHistogram[i] = static_cast<float>(warmLumaBins[i] / count);
    }

    // Provisional only. "Is this a night scene?" and "is this golden hour?"
    // are questions about how the picture READS, and this render is not the
    // picture anyone sees -- classifySteepAutoGrade is called again once the
    // tonal stage has said what the frame actually becomes. The provisional
    // label still has to be reasonable, because auto-cull uses it and because
    // the restraint terms inside applySteepAutoEdit run before the render is
    // measured.
    features.scene = classifySteepAutoGrade(
        features,
        features.medianLuma,
        features.shadowFraction,
        features.dynamicRange,
        features.brightWarmFraction);

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
    tone.shcompr = 18;
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
    const bool nightFrame =
        features.scene == AutoGradeScene::Night && shotValid && shotMid < 0.24;
    const double midTarget = nightFrame ? 0.31 : 0.45;

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

        if (features.scene == AutoGradeScene::Night && predictedLumaMid < 0.26) {
            lift *= 0.45;   // a night scene keeps its own weight
        }

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
    // Only treat a frame as a night scene if it actually renders dark. The
    // scene classifier reads a neutral render, which is far darker than the
    // finished image, so ordinary photographs get labelled Night and then
    // damped into inertness by every restraint below.
    const bool nightLook =
        features.scene == AutoGradeScene::Night && displayMid < 0.26;

    // A scene label is a reason to be more careful, not a reason to stop
    // looking. These were floors — max(risk, 0.38) and max(risk, 0.48) — which
    // meant a frame that measured 0.12 risk had that measurement thrown away
    // and replaced by a constant because it happened to contain skin. The
    // trace shows the result plainly: 44 of 98 frames sat in the two narrow
    // bands those constants define. Nudge instead, so the measurement still
    // decides the ordering between frames.
    if (features.scene == AutoGradeScene::Portrait) {
        overtuneRisk = std::min(1.0, overtuneRisk + 0.15);
    } else if (nightLook) {
        overtuneRisk = std::min(1.0, overtuneRisk + 0.22);
    }

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
    double liftPermission =
        highlightRoom * displayHeadroom * (1.0 - 0.65 * overtuneRisk);
    if (features.scene == AutoGradeScene::Portrait) {
        liftPermission *= 0.78;
    } else if (nightLook) {
        liftPermission *= 0.62;
    }

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

    double toeStrength = contrastIntent * (0.72 + 0.22 * shadowRoom);
    if (nightLook) {
        toeStrength *= 0.70;   // night keeps its shadow detail and mood
    } else if (features.scene == AutoGradeScene::Portrait) {
        toeStrength *= 0.88;   // gentle falloff on faces
    }
    toeStrength = std::max(0.0, std::min(0.75, toeStrength));

    double liftStrength = contrastIntent * (0.78 + 0.22 * liftPermission);
    if (nightLook) {
        liftStrength *= 0.70;
    } else if (features.scene == AutoGradeScene::Portrait) {
        liftStrength *= 0.88;
    }
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
        "[autoCurve] ==== %s ====\n"
        "[autoCurve]  features: valid=%d scene=%s medLuma=%.3f p10=%.3f p90=%.3f p98=%.3f range=%.3f\n"
        "[autoCurve]            shadowFrac=%.3f hlFrac=%.3f clipFrac=%.4f sat=%.3f iso=%u\n"
        "[autoCurve]            skinFrac=%.3f centerSkin=%.3f skinSat=%.3f sky=%.3f foliage=%.3f\n"
        "[autoCurve]  protection=%.3f neutralFlatness=%.3f measuredExposure=%d\n"
        "[autoCurve]  exposure: expcomp=%.3f bright=%d contrast=%d hlcompr=%d shcompr=%d evAdd=%.3f\n"
        "[autoCurve]  midTarget=%.2f nightFrame=%d shotValid=%d sh: hl=%d shadows=%d\n"
        "[autoCurve]  stations from channel: p10=%.3f mid=%.3f p90=%.3f\n"
        "[autoCurve]  picture from luma:     p10=%.3f mid=%.3f p90=%.3f range=%.3f\n"
        "[autoCurve]  flatness=%.3f overtuneRisk=%.3f nightLook=%d liftPermission=%.3f\n"
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
        midTarget, nightFrame ? 1 : 0, shotValid ? 1 : 0, highlightRecovery, shadowLift,
        stationP10, stationMid, stationP90,
        predictedLumaP10, displayMid, displayP90, displayRange,
        flatness, overtuneRisk, nightLook ? 1 : 0, liftPermission,
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

void applySteepAutoGrade(
    const AutoGradeFeatures& features,
    const AutoEditRender& render,
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

    int gradeContrast = 2;
    switch (features.scene) {
        case AutoGradeScene::Portrait:
            vibrance.pastels = 10;
            vibrance.saturated = -2;
            gradeContrast = 1;
            break;
        case AutoGradeScene::GoldenHour:
            vibrance.pastels = 12;
            vibrance.saturated = 0;
            gradeContrast = 1;
            break;
        case AutoGradeScene::Landscape:
            vibrance.pastels = 18;
            vibrance.saturated = 3;
            gradeContrast = 3;
            break;
        case AutoGradeScene::Night:
            vibrance.pastels = 9;
            vibrance.saturated = -3;
            gradeContrast = 2;
            break;
        case AutoGradeScene::Urban:
            vibrance.pastels = 11;
            vibrance.saturated = 0;
            gradeContrast = 3;
            break;
        case AutoGradeScene::Neutral:
            break;
    }

    params.toneCurve.contrast = std::min(45, params.toneCurve.contrast + gradeContrast);
    params.sh.highlights = std::min(45, params.sh.highlights + 3);
    // Judged on the rendered frame: on the neutral render this test read
    // shadowFraction > 0.34 as true on 80% of frames and highlightFraction
    // < 0.16 as true on all of them, so it was not a condition.
    const double shadowShare = render.valid ? render.shadowFraction : features.shadowFraction;
    const double highlightShare = render.valid ? render.highlightFraction : features.highlightFraction;

    if ((render.valid || features.valid) && shadowShare > 0.34 && highlightShare < 0.16) {
        params.sh.shadows = std::min(30, params.sh.shadows + 1);
    }

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

    // "Dark" gets ONE definition, shared with the tone stage's nightLook.
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

    switch (features.scene) {
        case AutoGradeScene::Portrait:
            film.preset = "porcelain_400";
            film.process = "c41";
            film.output = "ra4";
            film.strength = 60;
            film.contrast = 5;
            film.fade = -3;
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
            film.strength = 66;
            film.contrast = 4;
            film.warmth = -1;
            film.halation = std::min(14, film.halation + 2);
            film.rolloff = std::max(film.rolloff, 2);
            film.skinProtection = 78;
            break;
        case AutoGradeScene::Landscape:
            // Saturation is a colour reading and stays on the source; the
            // highlight share is a tonal one and comes off the render.
            film.preset = features.saturation > 0.35 && frameHighlights < 0.13
                ? "vivid_chrome"
                : "sovereign";
            if (film.preset == "vivid_chrome") {
                film.process = "e6";
                film.output = "projection";
                film.strength = 52;
                film.contrast = -8;
                film.fade = -3;
                film.rolloff = -4;
                film.saturation -= 3;
            } else {
                film.process = "c41";
                film.output = "ra4";
                film.strength = 62;
                film.contrast = 5;
            }
            film.grain = std::min(film.grain, 16);
            film.halation = std::min(film.halation, 10);
            film.vibrance = features.saturation < 0.24 ? 3 : 0;
            break;
        case AutoGradeScene::Night:
            film.preset = "cinematic_500t";
            film.process = "ecn2";
            film.output = "cinema";
            film.strength = 66;
            film.exposure = 0.08;
            film.contrast = 8;
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
            film.strength = 63;
            film.contrast = 2;
            film.fade = -6;
            film.rolloff = 0;
            film.grain = std::min(30, film.grain + 4);
            film.halation = std::min(film.halation, 11);
            break;
        case AutoGradeScene::Neutral:
            break;
    }

    // Grain is deliberately NOT part of the auto film recipe. The film
    // stage is grain-free by design, and texture is a taste decision the
    // user makes themselves through Effects > Grain - an auto edit that
    // ships ~20 strength of grain on every frame took that choice away.
    // (film.grain writes above are inert; they document per-scene intent
    // for anyone who wants to dial grain in by hand.)

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
        features.scene = classifySteepAutoGrade(
            features,
            render.mid,
            render.shadowFraction,
            render.range,
            brightWarmFractionAtGain(features, render.gain));
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
