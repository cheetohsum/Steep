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
#include <giomm/appinfo.h>

#ifdef _WIN32
#include "rtengine/leanwindows.h"
#include <gdk/gdkwin32.h>
#endif

#include "filebrowser.h"

#include "autoedit.h"
#include "batchqueue.h"
#include "clipboard.h"
#include "filepanel.h"
#include "inspector.h"
#include "multilangmgr.h"
#include "options.h"
#include "paramsedited.h"
#include "previewloader.h"
#include "profilestorecombobox.h"
#include "procparamchangers.h"
#include "rtimage.h"
#include "rtscalable.h"
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
#include <shellapi.h>
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
    double p10 = 0.10;          // luma percentiles for curve anchor placement
    double p90 = 0.60;
    double p98 = 0.88;
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

// TEMPORARY DIAGNOSTIC (STEEP_FILESEL_LOG=1) — defined further down.
void fileBrowserPerfLog(const char* fmt, ...);

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
    rtengine::procparams::ProcParams& params)
{
    params.setDefaults();

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
    const double broadHighlightRisk = features.valid
        ? unitInterval((features.highlightFraction - 0.05) / 0.20)
        : 0.0;
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

    if (features.scene == AutoGradeScene::Portrait) {
        overtuneRisk = std::max(overtuneRisk, 0.38);
    } else if (nightLook) {
        overtuneRisk = std::max(overtuneRisk, 0.48);
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

    const double shadowRoom = features.valid
        ? unitInterval((0.40 - features.shadowFraction) / 0.40)
        : 0.50;
    const double sourceHeadroom = features.valid
        ? unitInterval((0.17 - features.highlightFraction) / 0.15)
        : 0.55;
    const double displayHeadroom = unitInterval((0.94 - displayP90) / 0.30);
    double liftPermission =
        sourceHeadroom * displayHeadroom * (1.0 - 0.65 * overtuneRisk);
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
    liftStrength = std::max(0.0, std::min({0.70, liftStrength, toeStrength}));

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
        "[autoCurve]  strengths: toe=%.3f lift=%.3f\n"
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
        toeStrength, liftStrength,
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
    film.exposure = features.highlightFraction > 0.24 && features.medianLuma >= 0.30
        ? -0.05
        : features.highlightFraction > 0.18 && features.medianLuma >= 0.30
          ? -0.02
          : features.medianLuma < 0.24
            ? 0.07
            : 0.0;
    film.fade = -4;
    film.rolloff = features.highlightFraction > 0.22 ? 4 : 0;
    film.saturation = features.saturation > 0.48 ? -5 : features.saturation < 0.20 ? 2 : 0;
    film.contrast = features.dynamicRange < 0.38 ? 6 : features.dynamicRange > 0.72 ? 3 : 4;
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
            film.preset = features.saturation > 0.35 && features.highlightFraction < 0.13
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
    if (features.valid && features.medianLuma < 0.30) {
        const bool night = features.scene == AutoGradeScene::Night;
        double exposureFloor;
        if (features.medianLuma < 0.14) {
            exposureFloor = night ? 0.24 : 0.36;
        } else if (features.medianLuma < 0.23) {
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
        const int inputContrastCeiling = features.dynamicRange < 0.18
            ? 10
            : features.dynamicRange < 0.35 ? 12 : 14;
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
    if (features.valid) {
        const double sourceFlatness = std::max(0.0, (0.55 - features.dynamicRange) / 0.55);
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
    menuFileOperations(nullptr),
    menuExtProg(nullptr),
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

    // GTK freezes a popup window's repaints until a configure event
    // confirms each move/resize request. On Windows, a request the OS
    // treats as a no-op is never confirmed, the freeze leaks, and the
    // reused menu window stops repainting: fully frozen menus pop up
    // BLANK, partially frozen ones mix stale and fresh pixels (the
    // state-aware flag icons losing their pole after flag changes).
    // A fresh native window cannot be frozen, so make every popup a
    // first popup: destroy the menu's window whenever it closes.
    pmenu->signal_hide().connect([this]() {
        // A short timeout, not a default-priority idle: those starve for
        // over a second under load and the guard then silently skipped,
        // letting the next popup reuse the frozen window.
        Glib::signal_timeout().connect([this]() -> bool {
            if (!pmenu || pmenu->get_visible()) {
                return false;  // reopened; the next hide re-arms
            }
            Gtk::Widget* top = pmenu->get_toplevel();
            const bool doIt = top && top->get_realized() && !top->get_mapped();
            if (g_getenv("STEEP_MENU_SELFTEST") || g_getenv("STEEP_MENU_WATCH")) {
                if (FILE* log = fopen((Glib::get_tmp_dir() + "/steep_menu_log.txt").c_str(), "a")) {
                    fprintf(log, "[hide] top=%p realized=%d mapped=%d -> unrealize=%d\n",
                            (void*)top, top ? (int)top->get_realized() : -1,
                            top ? (int)top->get_mapped() : -1, (int)doIt);
                    fclose(log);
                }
            }
            if (doIt) {
                gtk_widget_unrealize(GTK_WIDGET(top->gobj()));
            }
            return false;
        }, 30);
    });

    /***********************
     * Inline quick actions at the very top: one-click flag / rank /
     * color-label for the whole selection, no submenu digging. The
     * buttons route through menuItemActivated() using the hidden
     * identity items created further down.
     ***********************/
    {
        // GtkMenu grabs all input while open, so real buttons inside menu
        // items never receive clicks. The icons are plain images; presses
        // are hit-tested at the menu level against each icon's allocation.
        struct InlineZone {
            Gtk::Widget* widget = nullptr;                      // fixed hit target
            Gtk::Widget* visual = nullptr;                      // icon hover feedback
            std::function<void()> action;                       // click
            std::function<Gtk::MenuItem*()> hoverPreview;       // optional
            std::function<Gtk::Menu*()> hoverMenu;              // hover-opens options
            Glib::ustring tip;                                  // hover-pause tooltip
            bool keepMenuOpen = false;                          // dropdowns
        };
        auto inlineZones = std::make_shared<std::vector<InlineZone>>();

        // Caption sits above its icon row in a small dim font, so the icons
        // form a compact, evenly spaced strip underneath. An empty caption
        // yields a bare icon strip (flag/rank/color rows are self-evident).
        auto makeInlineRow = [](const Glib::ustring& caption) {
            auto* item = Gtk::manage(new Gtk::MenuItem());
            item->set_name("InlineActionRow");
            auto* vbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
            vbox->set_halign(Gtk::ALIGN_START);
            vbox->set_margin_top(2);
            vbox->set_margin_bottom(1);
            if (!caption.empty()) {
                auto* label = Gtk::manage(new Gtk::Label());
                label->set_markup("<span size='9500' alpha='60%'>"
                                  + Glib::Markup::escape_text(caption) + "</span>");
                label->set_xalign(0.0);
                label->set_halign(Gtk::ALIGN_START);
                // Align exactly with the first action slot below.
                label->set_margin_start(0);
                vbox->pack_start(*label, Gtk::PACK_SHRINK);
            }
            auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0));
            row->set_halign(Gtk::ALIGN_START);
            vbox->pack_start(*row, Gtk::PACK_SHRINK);
            item->add(*vbox);

            // Native menu prelight covers the whole row and implies that
            // caption/whitespace are clickable. Only icon slots are targets.
            auto rowCss = Gtk::CssProvider::create();
            rowCss->load_from_data(
                "#InlineActionRow:hover, #InlineActionRow:active {"
                " background-image: none;"
                " background-color: transparent;"
                "}");
            item->get_style_context()->add_provider(
                rowCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 300);

            return std::make_pair(item, row);
        };

        auto makeInlineImage = [](Gtk::Box* row, const char* icon) {
            auto* slot = Gtk::manage(new Gtk::EventBox());
            slot->set_visible_window(false);
            // The slot must fit the DPI-SCALED icon. A hardcoded 28 was
            // smaller than the icon at >100% display scaling, so icons
            // overflowed and drew only their middle strip — with the pick
            // flag's pole living or dying on a ±1px centering rounding
            // that differed between first map and re-show.
            const int slotSize = RTScalable::scalePixelSize(28);
            slot->set_size_request(slotSize, slotSize);
            slot->set_halign(Gtk::ALIGN_START);
            slot->set_valign(Gtk::ALIGN_CENTER);

            auto* img = Gtk::manage(new RTImage(icon, Gtk::ICON_SIZE_LARGE_TOOLBAR));
            // FILL, not CENTER: the image's natural size can measure
            // smaller than the painted icon (8px wide under the theme),
            // and a re-shown widget clips painting to its allocation —
            // rendering only the icon's middle strip (a pick flag drawn
            // without its pole). Filling the 28px slot keeps the
            // allocation larger than anything the icon paints.
            img->set_halign(Gtk::ALIGN_FILL);
            img->set_valign(Gtk::ALIGN_FILL);
            // The theme's `image { padding }` mis-measures these icons
            // (width 8 for a 24px surface) and shifts the drawn surface
            // off-center, which is what clipped the pick flag's pole.
            // Zero the CSS box entirely for the inline icons.
            static auto imgCss = []() {
                auto p = Gtk::CssProvider::create();
                p->load_from_data("image { padding: 0; margin: 0; border-width: 0; }");
                return p;
            }();
            img->get_style_context()->add_provider(
                imgCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 300);
            // Dimmed at rest; the hover tracker below raises the icon under
            // the pointer to full opacity so the target is unmistakable.
            img->set_opacity(0.65);
            slot->add(*img);
            row->pack_start(*slot, Gtk::PACK_SHRINK);
            return std::make_pair(
                static_cast<Gtk::Widget*>(slot),
                static_cast<Gtk::Widget*>(img));
        };

        auto addInlineIcon = [this, inlineZones, makeInlineImage](
                                 Gtk::Box* row, const char* icon,
                                 const Glib::ustring& tooltip,
                                 std::function<Gtk::MenuItem*()> target,
                                 bool hoverPreviews = false) {
            const auto inlineImage = makeInlineImage(row, icon);
            InlineZone zone;
            zone.widget = inlineImage.first;
            zone.visual = inlineImage.second;
            zone.tip = tooltip;
            zone.action = [this, target]() {
                menuItemActivated(target());
            };
            if (hoverPreviews) {
                zone.hoverPreview = target;
            }
            inlineZones->push_back(std::move(zone));
        };

        auto addInlineAction = [inlineZones, makeInlineImage](
                                   Gtk::Box* row, const char* icon,
                                   const Glib::ustring& tooltip,
                                   std::function<void()> action,
                                   bool keepMenuOpen = false) {
            const auto inlineImage = makeInlineImage(row, icon);
            InlineZone zone;
            zone.widget = inlineImage.first;
            zone.visual = inlineImage.second;
            zone.tip = tooltip;
            zone.action = std::move(action);
            zone.keepMenuOpen = keepMenuOpen;
            inlineZones->push_back(std::move(zone));
        };

        // Flags: pick / unflag / reject — each icon shows only when the
        // selection isn't already in that state (rightClicked() toggles
        // them per popup) — plus rotate ccw/cw to the right
        auto flagRow = makeInlineRow("");
        addInlineIcon(flagRow.second, "menu-flag-pick", M("FILEBROWSER_POPUPPICK"),
                      [this]() -> Gtk::MenuItem* { return pickFlag; });
        inlineFlagPickIcon_ = inlineZones->back().widget;
        addInlineIcon(flagRow.second, "menu-flag-unflagged", M("FILEBROWSER_POPUPUNFLAG"),
                      [this]() -> Gtk::MenuItem* { return unflagFlag; });
        inlineFlagUnflagIcon_ = inlineZones->back().widget;
        addInlineIcon(flagRow.second, "menu-flag-reject", M("FILEBROWSER_POPUPREJECT"),
                      [this]() -> Gtk::MenuItem* { return rejectFlag; });
        inlineFlagRejectIcon_ = inlineZones->back().widget;
        addInlineAction(flagRow.second, "rotate-left-90", M("TP_COARSETRAF_TOOLTIP_ROTLEFT"),
                        [this]() { requestRotateSelected(270); });
        inlineZones->back().widget->set_margin_start(10);
        addInlineAction(flagRow.second, "rotate-right-90", M("TP_COARSETRAF_TOOLTIP_ROTRIGHT"),
                        [this]() { requestRotateSelected(90); });
        pmenu->attach(*flagRow.first, 0, 1, p, p + 1);
        p++;

        // Rank: none + 1..5 stars
        auto rankRow = makeInlineRow("");
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
        auto colorRow = makeInlineRow("");
        for (int i = 0; i <= 5; i++) {
            addInlineIcon(colorRow.second, inlineClabelIcons[i],
                          M(Glib::ustring::compose("FILEBROWSER_POPUPCOLORLABEL%1", i)),
                          [this, i]() -> Gtk::MenuItem* { return colorlabel[i]; });
        }
        pmenu->attach(*colorRow.first, 0, 1, p, p + 1);
        p++;

        // Auto looks: edit / grade / film lab / grade+film — hovering an
        // icon live-previews the look on the open image, same as the old
        // submenu entries did
        auto autoRow = makeInlineRow(M("FILEBROWSER_POPUPAUTOEDIT"));
        addInlineIcon(autoRow.second, "palette-brush", M("FILEBROWSER_POPUPAUTOEDITNEUTRAL"),
                      [this]() -> Gtk::MenuItem* { return autoEditMenu; }, true);
        addInlineIcon(autoRow.second, "color-circles", M("FILEBROWSER_POPUPAUTOGRADE"),
                      [this]() -> Gtk::MenuItem* { return autoGrade; }, true);
        addInlineIcon(autoRow.second, "filmstrip-show", M("FILEBROWSER_POPUPFILMLAB"),
                      [this]() -> Gtk::MenuItem* { return autoGradeFilm; }, true);
        addInlineIcon(autoRow.second, "auto-grade-film", M("FILEBROWSER_POPUPAUTOGRADEFILM"),
                      [this]() -> Gtk::MenuItem* { return autoGradedFilm; }, true);
        addInlineIcon(autoRow.second, "ai-denoise", M("FILEBROWSER_POPUPAIDENOISE"),
                      [this]() -> Gtk::MenuItem* { return aiDenoise; });
        addInlineIcon(autoRow.second, "rotate-straighten-small", M("TP_ROTATE_AUTO_LEVEL"),
                      [this]() -> Gtk::MenuItem* { return autoLevel; });
        pmenu->attach(*autoRow.first, 0, 1, p, p + 1);
        p++;

        // Processing profile operations: copy (all) / copy-settings dropdown /
        // paste / paste partial / apply dropdown
        auto profileRow = makeInlineRow(M("FILEBROWSER_POPUPPROFILEOPERATIONS"));
        addInlineIcon(profileRow.second, "menu-profile-copy", M("FILEBROWSER_COPYPROFILE"),
                      [this]() -> Gtk::MenuItem* { return copyprof; });
        addInlineAction(profileRow.second, "arrow-down-small", M("FILEBROWSER_COPYPROFILE_SETTINGS"),
                        [this]() {
                            if (inlineCopySettingsMenu_) {
                                inlineCopySettingsMenu_->popup_at_pointer(nullptr);
                            }
                        });
        addInlineIcon(profileRow.second, "menu-profile-paste", M("FILEBROWSER_PASTEPROFILE"),
                      [this]() -> Gtk::MenuItem* { return pasteprof; });
        addInlineIcon(profileRow.second, "menu-profile-partial", M("FILEBROWSER_PARTIALPASTEPROFILE"),
                      [this]() -> Gtk::MenuItem* { return partpasteprof; });
        addInlineAction(profileRow.second, "menu-profile-apply", M("FILEBROWSER_APPLYPROFILE"),
                        [this]() {
                            if (inlineApplyMenu_) {
                                inlineApplyMenu_->popup_at_pointer(nullptr);
                            }
                        });
        addInlineIcon(profileRow.second, "menu-profile-clear", M("FILEBROWSER_CLEARPROFILE"),
                      [this]() -> Gtk::MenuItem* { return clearprof; });
        pmenu->attach(*profileRow.first, 0, 1, p, p + 1);
        p++;

        // File actions: open / export queue / fast export / duplicate /
        // add to target album (visible only when a target album is set)
        auto actionsRow = makeInlineRow(M("FILEBROWSER_POPUPACTIONS"));
        addInlineIcon(actionsRow.second, "menu-open", M("FILEBROWSER_POPUPOPEN"),
                      [this]() -> Gtk::MenuItem* { return open; });
        addInlineAction(actionsRow.second, "folder-open-small", M("FILEBROWSER_OPENSOURCEFOLDER"),
                        [this]() { openSourceFolder(); });
        addInlineIcon(actionsRow.second, "gears", M("FILEBROWSER_POPUPPROCESS"),
                      [this]() -> Gtk::MenuItem* { return develop; });
        addInlineAction(actionsRow.second, "menu-save", M("FILEBROWSER_POPUPSAVEIMAGE"),
                        [this]() { m_save_image_requested.emit(); });
        addInlineIcon(actionsRow.second, "menu-duplicate", M("FILEBROWSER_POPUPDUPLICATE"),
                      [this]() -> Gtk::MenuItem* { return duplicate; });
        addInlineAction(actionsRow.second, "external-editor", M("MAIN_BUTTON_SENDTOEDITOR"),
                        [this]() { m_external_editor_requested.emit(); });
        addInlineIcon(actionsRow.second, "add-to-album", M("EDITOR_ADD_TO_ALBUM_TOOLTIP"),
                      [this]() -> Gtk::MenuItem* { return addToAlbum; });
        inlineAddToAlbumIcon_ = inlineZones->back().widget;
        pmenu->attach(*actionsRow.first, 0, 1, p, p + 1);
        p++;

        pmenu->attach(*Gtk::manage(new Gtk::SeparatorMenuItem()), 0, 1, p, p + 1);
        p++;

        // Hit-test each fixed slot exactly. The former padded image
        // rectangles overlapped neighboring actions and their captions.
        auto hitZoneAt = [inlineZones](double xRoot, double yRoot) -> InlineZone* {
            for (auto& zone : *inlineZones) {
                Gtk::Widget* widget = zone.widget;
                if (!widget->get_visible()) {
                    continue;
                }
                Gtk::Widget* toplevel = widget->get_toplevel();
                if (!toplevel || !toplevel->get_window()) {
                    continue;
                }

                int inTopX = 0;
                int inTopY = 0;
                if (!widget->translate_coordinates(*toplevel, 0, 0, inTopX, inTopY)) {
                    continue;
                }
                int topX = 0;
                int topY = 0;
                toplevel->get_window()->get_origin(topX, topY);

                const double w = widget->get_allocated_width();
                const double h = widget->get_allocated_height();
                const double ix = xRoot - (topX + inTopX);
                const double iy = yRoot - (topY + inTopY);

                if (ix >= 0.0 && iy >= 0.0 && ix < w && iy < h) {
                    return &zone;
                }
            }
            return nullptr;
        };

        pmenu->signal_button_press_event().connect(
            [this, hitZoneAt](GdkEventButton* event) -> bool {
                if (!event || event->button != 1) {
                    return false;
                }

                InlineZone* hit = hitZoneAt(event->x_root, event->y_root);
                if (!hit) {
                    return false;
                }

                const auto action = hit->action;
                if (!hit->keepMenuOpen) {
                    pmenu->popdown();
                }
                action();
                return true;
            },
            false);

        // Hover feedback: the icon under the pointer goes full-opacity,
        // auto-look icons live-preview their result on the open image, and
        // pausing over any icon shows what it does. GTK's own tooltips
        // cannot fire through the menu grab, so the tip is a small popup
        // window managed by the hover tracker.
        struct HoverState {
            InlineZone* zone = nullptr;
            sigc::connection tipTimer;
            sigc::connection menuTimer;
            sigc::connection menuPoll;
            Gtk::Menu* openSubmenu = nullptr;
            double pointerX = 0.0;
            double pointerY = 0.0;
        };
        auto hoverState = std::make_shared<HoverState>();

        // While a tools submenu is open it owns the input grab, and native
        // Windows popups deliver motion to it sparsely and unreliably - the
        // motion-driven close below sometimes never fired at all. So the
        // authoritative open/close decision is this 35ms GDK pointer poll
        // (same pattern that fixed the auto-edit hover previews): stay open
        // over the submenu or its anchor icon, close the moment the pointer
        // rests anywhere else over the context menu, stay for click-away
        // when it leaves both windows.
        auto startToolsMenuPoll = std::make_shared<std::function<void(Gtk::Menu*, Gtk::Widget*)>>();
        *startToolsMenuPoll = [this, hoverState](Gtk::Menu* sub, Gtk::Widget* anchor) {
            hoverState->menuPoll.disconnect();
            hoverState->menuPoll = Glib::signal_timeout().connect(
                [this, hoverState, sub, anchor]() -> bool {
                    if (hoverState->openSubmenu != sub || !sub->get_visible()) {
                        return false;
                    }

                    auto subWindow = sub->get_window();
                    auto display = sub->get_display();
                    if (!subWindow || !display) {
                        return true;
                    }
                    auto seat = display->get_default_seat();
                    auto device = seat ? seat->get_pointer() : Glib::RefPtr<Gdk::Device>();
                    if (!device) {
                        return true;
                    }

                    int rootX = 0;
                    int rootY = 0;
                    gdk_device_get_position(device->gobj(), nullptr, &rootX, &rootY);

                    int subX = 0;
                    int subY = 0;
                    subWindow->get_origin(subX, subY);
                    const int subW = sub->get_allocated_width();
                    const int subH = sub->get_allocated_height();
                    constexpr int pad = 14;

                    if (rootX >= subX - pad && rootY >= subY - pad
                            && rootX < subX + subW + pad && rootY < subY + subH + pad) {
                        return true;
                    }

                    if (anchor) {
                        if (auto anchorWindow = anchor->get_window()) {
                            int ax = 0;
                            int ay = 0;
                            anchorWindow->get_origin(ax, ay);
                            const auto alloc = anchor->get_allocation();
                            const int ax0 = ax + alloc.get_x() - pad;
                            const int ay0 = ay + alloc.get_y() - pad;
                            const int ax1 = ax + alloc.get_x() + alloc.get_width() + pad;
                            const int ay1 = ay + alloc.get_y() + alloc.get_height() + pad;

                            if (rootX >= ax0 && rootY >= ay0 && rootX < ax1 && rootY < ay1) {
                                return true;
                            }
                        }
                    }

                    if (auto menuWindow = pmenu->get_window()) {
                        int px = 0;
                        int py = 0;
                        menuWindow->get_origin(px, py);
                        const int pw = pmenu->get_allocated_width();
                        const int ph = pmenu->get_allocated_height();

                        if (rootX >= px && rootY >= py && rootX < px + pw && rootY < py + ph) {
                            sub->popdown();
                            if (hoverState->openSubmenu == sub) {
                                hoverState->openSubmenu = nullptr;
                            }
                            return false;
                        }
                    }

                    return true;
                },
                35,
                G_PRIORITY_DEFAULT_IDLE);
        };

        // Remembers the item last chosen from each tools submenu, so clicking
        // that tool's icon again repeats it instead of making the user walk
        // the submenu a second time. Hovering still opens the menu to pick
        // something else.
        auto lastToolChoice = std::make_shared<std::map<Gtk::Menu*, Gtk::MenuItem*>>();

        if (!inlineTipWindow_) {
            inlineTipWindow_ = new Gtk::Window(Gtk::WINDOW_POPUP);
            inlineTipWindow_->set_type_hint(Gdk::WINDOW_TYPE_HINT_TOOLTIP);
            inlineTipWindow_->get_style_context()->add_class("tooltip");
            inlineTipLabel_ = Gtk::manage(new Gtk::Label());
            inlineTipLabel_->set_margin_start(6);
            inlineTipLabel_->set_margin_end(6);
            inlineTipLabel_->set_margin_top(2);
            inlineTipLabel_->set_margin_bottom(2);
            inlineTipWindow_->add(*inlineTipLabel_);
        }

        auto hideTip = [this, hoverState]() {
            hoverState->tipTimer.disconnect();
            hoverState->menuTimer.disconnect();
            if (inlineTipWindow_) {
                inlineTipWindow_->hide();
            }
        };

        auto setHover = [this, hoverState, hideTip, startToolsMenuPoll](InlineZone* hovered, double x, double y) {
            hoverState->pointerX = x;
            hoverState->pointerY = y;

            if (hoverState->zone == hovered) {
                return;
            }

            hideTip();

            // queue_draw the whole slot, not just the image: the theme can
            // shrink the image's own clip below the painted icon, and a
            // partial repaint of that strip mixes fresh and stale pixels
            // on Windows' reused menu backing store.
            InlineZone* previous = hoverState->zone;
            if (previous) {
                previous->visual->set_opacity(0.65);
                previous->widget->queue_draw();
            }
            hoverState->zone = hovered;
            if (hovered) {
                hovered->visual->set_opacity(1.0);
                hovered->widget->queue_draw();

                if (hovered->hoverMenu) {
                    // Hovering a tools icon opens its options menu below the
                    // icon (no tooltip for these — the menu IS the answer)
                    hoverState->menuTimer = Glib::signal_timeout().connect(
                        [this, hoverState, startToolsMenuPoll]() -> bool {
                            InlineZone* zone = hoverState->zone;
                            if (zone && zone->hoverMenu) {
                                if (hoverState->openSubmenu) {
                                    hoverState->openSubmenu->popdown();
                                }
                                Gtk::Menu* sub = zone->hoverMenu();
                                if (sub) {
                                    hoverState->openSubmenu = sub;
                                    sub->popup_at_widget(zone->widget,
                                                         Gdk::GRAVITY_SOUTH_WEST,
                                                         Gdk::GRAVITY_NORTH_WEST,
                                                         nullptr);
                                    (*startToolsMenuPoll)(sub, zone->widget);
                                }
                            }
                            return false;
                        },
                        220);
                } else if (!hovered->tip.empty()) {
                    hoverState->tipTimer = Glib::signal_timeout().connect(
                        [this, hoverState]() -> bool {
                            if (hoverState->zone && inlineTipWindow_ && inlineTipLabel_) {
                                inlineTipLabel_->set_markup(
                                    "<span size='small'>"
                                    + Glib::Markup::escape_text(hoverState->zone->tip)
                                    + "</span>");
                                inlineTipWindow_->move(
                                    static_cast<int>(hoverState->pointerX) + 14,
                                    static_cast<int>(hoverState->pointerY) + 20);
                                inlineTipWindow_->show_all();
                            }
                            return false;
                        },
                        450);
                }
            }

            const bool hadPreview = previous && previous->hoverPreview;
            const bool hasPreview = hovered && hovered->hoverPreview;
            if (hasPreview) {
                startAutoEditHoverPreview(hovered->hoverPreview());
            } else if (hadPreview) {
                cancelAutoEditHoverPreview(nullptr, true);
            }
        };

        pmenu->add_events(Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
        pmenu->signal_motion_notify_event().connect(
            [hitZoneAt, setHover](GdkEventMotion* event) -> bool {
                if (event) {
                    setHover(hitZoneAt(event->x_root, event->y_root), event->x_root, event->y_root);
                }
                return false;
            },
            false);
        pmenu->signal_leave_notify_event().connect(
            [setHover, hoverState](GdkEventCrossing* event) -> bool {
                // Moving onto an open tools submenu leaves pmenu's window.
                // Dropping the hover here would cancel the icon highlight and
                // the hover machinery mid-travel, so keep it while that menu
                // is up — it closes on click-away, Escape, or a choice.
                if (hoverState->openSubmenu) {
                    return false;
                }

                setHover(nullptr,
                         event ? event->x_root : 0.0,
                         event ? event->y_root : 0.0);
                return false;
            },
            false);
        pmenu->signal_hide().connect([setHover, hideTip]() {
            hideTip();
            setHover(nullptr, 0.0, 0.0);
        });

        // NOTE: the surrounding scope stays open until the bottom tools row
        // so the row-building lambdas remain available for it.

    pmenu->attach (*Gtk::manage(open = new MyImageMenuItem (M("FILEBROWSER_POPUPOPEN"), "menu-open")), 0, 1, p, p + 1);
    p++;
    open->set_no_show_all(true);
    open->hide();
    if (options.inspectorWindow) {
        pmenu->attach (*Gtk::manage(inspect = new MyImageMenuItem (M("FILEBROWSER_POPUPINSPECT"), "menu-inspect")), 0, 1, p, p + 1);
        p++;
    }
    pmenu->attach (*Gtk::manage(develop = new MyImageMenuItem (M("FILEBROWSER_POPUPPROCESS"), "gears")), 0, 1, p, p + 1);
    p++;
    develop->set_no_show_all(true);
    develop->hide();
    pmenu->attach (*Gtk::manage(developfast = new MyImageMenuItem (M("FILEBROWSER_POPUPPROCESSFAST"), "menu-develop-fast")), 0, 1, p, p + 1);
    p++;
    developfast->set_no_show_all(true);
    developfast->hide();
    pmenu->attach (*Gtk::manage(saveImage = new MyImageMenuItem (M("FILEBROWSER_POPUPSAVEIMAGE"), "menu-save")), 0, 1, p, p + 1);
    p++;
    saveImage->set_no_show_all(true);
    saveImage->hide();  // surfaced in the Actions quick-action row

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;

    /***********************
     * AI & Quick Actions
     ***********************/
    pmenu->attach (*Gtk::manage(aiDenoise = new MyImageMenuItem (M("FILEBROWSER_POPUPAIDENOISE"), "ai-denoise")), 0, 1, p, p + 1);
    p++;
    aiDenoise->set_no_show_all(true);
    aiDenoise->hide();
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

        // The auto looks live in the inline quick-action row at the top of
        // the menu; this item (and its submenu) remain hidden as identity
        // targets for menuItemActivated() and the hover-preview machinery.
        autoEditMenu = Gtk::manage(new MyImageMenuItem(M("FILEBROWSER_POPUPAUTOEDIT"), "palette-brush"));
        autoEditMenu->set_submenu(*submenu);
        pmenu->attach(*autoEditMenu, 0, 1, p, p + 1);
        autoEditMenu->set_no_show_all(true);
        autoEditMenu->hide();
    }
    p++;
    pmenu->attach (*Gtk::manage(autoLevel = new MyImageMenuItem (M("TP_ROTATE_AUTO_LEVEL"), "rotate-straighten-small")), 0, 1, p, p + 1);
    p++;
    autoLevel->set_no_show_all(true);
    autoLevel->hide();
    pmenu->attach (*Gtk::manage(duplicate = new MyImageMenuItem (M("FILEBROWSER_POPUPDUPLICATE"), "menu-duplicate")), 0, 1, p, p + 1);
    p++;
    duplicate->set_no_show_all(true);
    duplicate->hide();
    pmenu->attach (*Gtk::manage(addToAlbum = new MyImageMenuItem (M("EDITOR_ADD_TO_ALBUM_TOOLTIP"), "add-to-album")), 0, 1, p, p + 1);
    p++;
    addToAlbum->set_no_show_all(true);
    addToAlbum->hide();
    pmenu->attach (*Gtk::manage(setAlbumCover = new MyImageMenuItem (M("FILEBROWSER_POPUPSETALBUMCOVER"), "menu-album-cover")), 0, 1, p, p + 1);
    p++;

    pmenu->attach (*Gtk::manage(new Gtk::SeparatorMenuItem ()), 0, 1, p, p + 1);
    p++;
    pmenu->attach (*Gtk::manage(selall = new MyImageMenuItem (M("FILEBROWSER_POPUPSELECTALL"), "menu-select-all")), 0, 1, p, p + 1);
    p++;
    // Hidden: Ctrl+A accelerator still routes through this identity item
    selall->set_no_show_all(true);
    selall->hide();

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
    menuSort->set_no_show_all(true);
    menuSort->hide();
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
        editExternal->set_no_show_all(true);
        editExternal->hide();  // surfaced in the Actions quick-action row
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
            menuExtProg->set_no_show_all(true);
            menuExtProg->hide();  // surfaced via the bottom tools row
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
        menuFileOperations->set_no_show_all(true);
        menuFileOperations->hide();  // surfaced via the bottom tools row
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

    // Profile operations are surfaced as an inline quick-action row at the
    // top of the menu; the items here stay hidden as identity targets so
    // menuItemActivated(), accelerators (Ctrl+C/V…), and the inline row
    // share one activation path.
    {
        pmenu->attach (*Gtk::manage(menuProfileOperations = new MyImageMenuItem (M("FILEBROWSER_POPUPPROFILEOPERATIONS"), "menu-profile-apply")), 0, 1, p, p + 1);
        p++;

        Gtk::Menu* submenuProfileOperations = Gtk::manage (new Gtk::Menu ());

        submenuProfileOperations->attach (*Gtk::manage(copyprof = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE"), "menu-profile-copy")), 0, 1, p, p + 1);
        p++;
        submenuProfileOperations->attach (*Gtk::manage(copyprofSettings = new MyImageMenuItem (M("FILEBROWSER_COPYPROFILE_SETTINGS"), "gears")), 0, 1, p, p + 1);
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
        menuProfileOperations->set_no_show_all(true);
        menuProfileOperations->hide();
    }

    // Standalone dropdowns for the inline profile row: copy-settings
    // filters and the apply options
    inlineCopySettingsMenu_ = buildCopyFilterSubmenu();

    inlineApplyMenu_ = new Gtk::Menu();
    {
        int ap = 0;
        const auto addApplyForward = [this, &ap](const Glib::ustring& label, const char* icon,
                                                 std::function<Gtk::MenuItem*()> identity) {
            auto* item = Gtk::manage(new MyImageMenuItem(label, icon));
            item->signal_activate().connect([this, identity]() {
                menuItemActivated(identity());
            });
            inlineApplyMenu_->attach(*item, 0, 1, ap, ap + 1);
            ++ap;
        };
        addApplyForward(M("FILEBROWSER_APPLYPROFILE"), "menu-profile-apply",
                        [this]() -> Gtk::MenuItem* { return applyprof; });
        addApplyForward(M("FILEBROWSER_APPLYPROFILE_PARTIAL"), "menu-profile-apply-partial",
                        [this]() -> Gtk::MenuItem* { return applypartprof; });
        addApplyForward(M("FILEBROWSER_RESETDEFAULTPROFILE"), "menu-profile-reset",
                        [this]() -> Gtk::MenuItem* { return resetdefaultprof; });
        addApplyForward(M("FILEBROWSER_CLEARPROFILE"), "menu-profile-clear",
                        [this]() -> Gtk::MenuItem* { return clearprof; });
        inlineApplyMenu_->show_all();
    }


    pmenu->attach (*Gtk::manage(menuDF = new MyImageMenuItem (M("FILEBROWSER_DARKFRAME"), "menu-darkframe")), 0, 1, p, p + 1);
    p++;
    menuDF->set_no_show_all(true);
    menuDF->hide();
    pmenu->attach (*Gtk::manage(menuFF = new MyImageMenuItem (M("FILEBROWSER_FLATFIELD"), "menu-flatfield")), 0, 1, p, p + 1);
    p++;
    menuFF->set_no_show_all(true);
    menuFF->hide();
    pmenu->attach (*Gtk::manage(cachemenu = new MyImageMenuItem (M("FILEBROWSER_CACHE"), "menu-cache-clear")), 0, 1, p, p + 1);
    p++;
    cachemenu->set_no_show_all(true);
    cachemenu->hide();

        // Bottom tools group: hovering (or clicking) an icon opens that
        // tool's options menu directly below it (sort, file operations,
        // open-with, dark frame, flat field, cache)
        {
            auto addSubmenuTool = [this, inlineZones, makeInlineImage,
                                   hitZoneAt, setHover, hoverState, lastToolChoice,
                                   startToolsMenuPoll](
                                      Gtk::Box* row, const char* icon,
                                      const Glib::ustring& tooltip,
                                      std::function<Gtk::MenuItem*()> parent) {
                const auto inlineImage = makeInlineImage(row, icon);
                auto* slot = inlineImage.first;
                const auto getMenu = [parent]() -> Gtk::Menu* {
                    Gtk::MenuItem* item = parent();
                    return item ? item->get_submenu() : nullptr;
                };
                InlineZone zone;
                zone.widget = slot;
                zone.visual = inlineImage.second;
                zone.tip = tooltip;
                zone.keepMenuOpen = true;
                zone.hoverMenu = getMenu;
                zone.action = [this, slot, getMenu, hoverState, lastToolChoice, startToolsMenuPoll]() {
                    Gtk::Menu* sub = getMenu();

                    if (!sub) {
                        return;
                    }

                    // Second click on the icon repeats the last choice made
                    // from this menu — the common case is doing the same file
                    // operation to several images in a row.
                    const auto remembered = lastToolChoice->find(sub);

                    if (remembered != lastToolChoice->end() && remembered->second
                            && remembered->second->get_sensitive()) {
                        Gtk::MenuItem* const repeat = remembered->second;

                        if (hoverState->openSubmenu) {
                            hoverState->openSubmenu->popdown();
                            hoverState->openSubmenu = nullptr;
                        }

                        pmenu->popdown();
                        repeat->activate();
                        return;
                    }

                    hoverState->openSubmenu = sub;
                    sub->popup_at_widget(slot,
                                         Gdk::GRAVITY_SOUTH_WEST,
                                         Gdk::GRAVITY_NORTH_WEST,
                                         nullptr);
                    (*startToolsMenuPoll)(sub, slot);
                };
                inlineZones->push_back(std::move(zone));

                // While this tool's menu is open it owns the input grab, so
                // pmenu no longer sees motion. Track motion here instead:
                // moving over a different tools icon switches menus, moving
                // back over other menu rows closes this one.
                if (Gtk::Menu* sub = getMenu()) {
                    // Remember what gets picked here so the icon can repeat it.
                    for (Gtk::Widget* child : sub->get_children()) {
                        if (auto* item = dynamic_cast<Gtk::MenuItem*>(child)) {
                            item->signal_activate().connect([lastToolChoice, sub, item]() {
                                (*lastToolChoice)[sub] = item;
                            });
                        }
                    }

                    sub->add_events(Gdk::POINTER_MOTION_MASK);
                    sub->signal_motion_notify_event().connect(
                        [this, sub, hitZoneAt, setHover, hoverState](GdkEventMotion* event) -> bool {
                            if (!event) {
                                return false;
                            }

                            if (auto win = sub->get_window()) {
                                int wx = 0;
                                int wy = 0;
                                win->get_origin(wx, wy);
                                const int mw = sub->get_allocated_width();
                                const int mh = sub->get_allocated_height();

                                // Pointer inside the submenu itself: leave it
                                // be so GTK highlights the item under it.
                                if (event->x_root >= wx && event->y_root >= wy
                                        && event->x_root < wx + mw
                                        && event->y_root < wy + mh) {
                                    return false;
                                }

                                // Crossing the gap between the icon and the
                                // menu. This submenu belongs to pmenu's shell,
                                // so forwarding motion here lets GTK select
                                // whatever pmenu row is under the pointer and
                                // pop the menu down mid-travel. Swallow it:
                                // the menu must survive the journey into it.
                                constexpr int travelMargin = 16;

                                if (event->x_root >= wx - travelMargin
                                        && event->y_root >= wy - travelMargin
                                        && event->x_root < wx + mw + travelMargin
                                        && event->y_root < wy + mh + travelMargin) {
                                    return true;
                                }
                            }

                            // Well away from the menu. Another tools icon
                            // switches menus; anywhere else over the context
                            // menu CLOSES this one — while it is up it owns
                            // the input grab, so leaving it open deadened
                            // every other row until a click-away. Outside
                            // both windows it stays for click-away/Escape.
                            InlineZone* over = hitZoneAt(event->x_root, event->y_root);
                            if (over && over->hoverMenu) {
                                setHover(over, event->x_root, event->y_root);
                                return false;
                            }

                            if (auto pwin = pmenu->get_window()) {
                                int px = 0;
                                int py = 0;
                                pwin->get_origin(px, py);
                                const int pw = pmenu->get_allocated_width();
                                const int ph = pmenu->get_allocated_height();

                                if (event->x_root >= px && event->y_root >= py
                                        && event->x_root < px + pw
                                        && event->y_root < py + ph) {
                                    sub->popdown();
                                    if (hoverState->openSubmenu == sub) {
                                        hoverState->openSubmenu = nullptr;
                                    }
                                    // Hand the row under the pointer its
                                    // normal hover behaviour immediately.
                                    setHover(over, event->x_root, event->y_root);
                                }
                            }
                            return false;
                        },
                        false);
                    sub->signal_deactivate().connect([sub, hoverState]() {
                        if (hoverState->openSubmenu == sub) {
                            hoverState->openSubmenu = nullptr;
                            hoverState->menuPoll.disconnect();
                        }
                    });
                }
            };

            auto toolsRow = makeInlineRow(M("FILEBROWSER_POPUPTOOLS"));
            addSubmenuTool(toolsRow.second, "menu-sort", M("FILEBROWSER_POPUPSORTBY"),
                           [this]() -> Gtk::MenuItem* { return menuSort; });
            if (menuFileOperations) {
                addSubmenuTool(toolsRow.second, "menu-trash", M("FILEBROWSER_POPUPFILEOPERATIONS"),
                               [this]() -> Gtk::MenuItem* { return menuFileOperations; });
            }
            if (menuExtProg) {
                addSubmenuTool(toolsRow.second, "open-with", M("FILEBROWSER_EXTPROGMENU"),
                               [this]() -> Gtk::MenuItem* { return menuExtProg; });
            }
            addSubmenuTool(toolsRow.second, "menu-darkframe", M("FILEBROWSER_DARKFRAME"),
                           [this]() -> Gtk::MenuItem* { return menuDF; });
            addSubmenuTool(toolsRow.second, "menu-flatfield", M("FILEBROWSER_FLATFIELD"),
                           [this]() -> Gtk::MenuItem* { return menuFF; });
            addSubmenuTool(toolsRow.second, "menu-cache-clear", M("FILEBROWSER_CACHE"),
                           [this]() -> Gtk::MenuItem* { return cachemenu; });
            pmenu->attach(*toolsRow.first, 0, 1, p, p + 1);
            p++;
        }
    }  // end of inline quick-action scope

    // Compact styling for the remaining regular menu items
    {
        auto compactCss = Gtk::CssProvider::create();
        compactCss->load_from_data(
            "menuitem { padding: 2px 8px; min-height: 0; }"
            "menuitem label { font-size: 0.88em; }"
            "separator { margin: 1px 0; }");
        std::function<void(Gtk::Widget*)> applyDeep = [&applyDeep, &compactCss](Gtk::Widget* w) {
            w->get_style_context()->add_provider(compactCss, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 200);
            if (auto* menuItem = dynamic_cast<Gtk::MenuItem*>(w)) {
                if (auto* sub = menuItem->get_submenu()) {
                    applyDeep(sub);
                }
            }
            if (auto* container = dynamic_cast<Gtk::Container*>(w)) {
                for (auto* child : container->get_children()) {
                    applyDeep(child);
                }
            }
        };
        applyDeep(pmenu);
    }

    pmenu->show_all ();

    // Collapse separator runs: hidden identity sections leave their old
    // separators behind, which would otherwise stack into multiple lines.
    {
        bool lastWasSeparator = true;  // also hides leading separators
        Gtk::Widget* pendingSeparator = nullptr;
        for (auto* child : pmenu->get_children()) {
            const bool isSeparator = dynamic_cast<Gtk::SeparatorMenuItem*>(child) != nullptr;
            if (!isSeparator && !child->get_visible()) {
                continue;  // hidden identity items don't break separator runs
            }
            if (isSeparator) {
                if (lastWasSeparator) {
                    child->set_no_show_all(true);
                    child->hide();
                } else {
                    lastWasSeparator = true;
                    pendingSeparator = child;
                }
            } else {
                lastWasSeparator = false;
                pendingSeparator = nullptr;
            }
        }
        // A separator with nothing visible after it is just a trailing line
        if (pendingSeparator) {
            pendingSeparator->set_no_show_all(true);
            pendingSeparator->hide();
        }
    }

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
    const auto armAutoEditHoverTracking = [this, autoEditSubmenu]() {
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
    };
    // The submenu opens two ways: keyboard/pointer selection of the parent
    // item, and the inline "Auto looks" icon's hover-menu, which calls
    // popup_at_widget and never fires the parent's select signal. The
    // pointer poll is the only reliable hover source on native Windows
    // popups, so arm it from the submenu's own map signal - that fires for
    // every opening path - and keep the select hook for redundancy (arming
    // twice is safe: it replaces the previous poll connection).
    autoEditMenu->signal_select().connect(armAutoEditHoverTracking);
    if (autoEditSubmenu) {
        autoEditSubmenu->signal_map().connect(armAutoEditHoverTracking);
    }
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

    static const std::array<const char*, 6> popClabelIcons = {
        "circle-empty-gray-small", "circle-red-small", "circle-yellow-small",
        "circle-green-small", "circle-blue-small", "circle-purple-small"
    };
    for (int i = 0; i <= 5; i++) {
        pmenuColorLabels->attach(*Gtk::manage(colorlabel_pop[i] = new MyImageMenuItem(M(Glib::ustring::compose("%1%2", "FILEBROWSER_POPUPCOLORLABEL", i)), popClabelIcons[i])), 0, 1, c, c + 1);
        c++;
    }

    pmenuColorLabels->show_all();

    // Has to be located after creation of applyprof and applypartprof
    updateProfileList ();

    // Bind to event handlers
    for (int i = 0; i <= 5; i++) {
        colorlabel_pop[i]->signal_activate().connect (sigc::bind(sigc::mem_fun(*this, &FileBrowser::menuColorlabelActivated), colorlabel_pop[i]));
    }

#ifdef _WIN32
    // STEEP_MENU_SELFTEST=1: autonomously reproduce the flag-row
    // hide/reshow popup cycle and dump true screen pixels (CAPTUREBLT
    // catches layered menu windows) plus widget states to %TEMP%.
    // Diagnostic-only; inert without the env var.
    if (g_getenv("STEEP_MENU_SELFTEST") || g_getenv("STEEP_MENU_WATCH")) {
        auto dump = [this](const char* tag) {
            const std::string logPath = Glib::get_tmp_dir() + "/steep_menu_log.txt";
            FILE* log = fopen(logPath.c_str(), "a");
            auto win = pmenu->get_window();
            if (!win) {
                if (log) { fprintf(log, "[%s] no window\n", tag); fclose(log); }
                return;
            }
            HWND hwnd = (HWND)GDK_WINDOW_HWND(win->gobj());
            // Only synthetic (event-less) popups can open BELOW the main
            // window; raise those so the grab captures the menu. Real
            // popups are already topmost — leave their z-order alone.
            if (g_getenv("STEEP_MENU_SELFTEST")) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                Sleep(60);
            }
            RECT r;
            GetWindowRect(hwnd, &r);
            const int w = r.right - r.left, h = r.bottom - r.top;
            HDC sdc = GetDC(nullptr);
            HDC mdc = CreateCompatibleDC(sdc);
            HBITMAP bmp = CreateCompatibleBitmap(sdc, w, h);
            SelectObject(mdc, bmp);
            BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            auto surf = Cairo::ImageSurface::create(Cairo::FORMAT_RGB24, w, h);
            GetDIBits(mdc, bmp, 0, h, surf->get_data(), &bi, DIB_RGB_COLORS);
            surf->mark_dirty();
            // GTK's own belief of the same instant: offscreen widget draw
            // (must run on the GUI thread; it is small and fast).
            Gtk::Allocation ma = pmenu->get_allocation();
            auto ws = Cairo::ImageSurface::create(
                Cairo::FORMAT_ARGB32,
                std::max(1, ma.get_width()), std::max(1, ma.get_height()));
            {
                auto wcr = Cairo::Context::create(ws);
                wcr->set_source_rgb(0.06, 0.08, 0.16);
                wcr->paint();
                gtk_widget_draw(GTK_WIDGET(pmenu->gobj()), wcr->cobj());
            }
            // PNG encoding is the slow part — keep it off the GUI thread.
            std::thread([surf, ws, tagStr = std::string(tag)]() {
                surf->write_to_png(Glib::get_tmp_dir() + "/steep_menu_" + tagStr + ".png");
                ws->write_to_png(Glib::get_tmp_dir() + "/steep_menu_" + tagStr + "_widget.png");
            }).detach();
            DeleteObject(bmp);
            DeleteDC(mdc);
            ReleaseDC(nullptr, sdc);
            if (log) {
                fprintf(log, "[%s] hwnd=%p hwndVisible=%d rect=(%ld,%ld %dx%d)\n",
                        tag, (void*)hwnd, (int)IsWindowVisible(hwnd), r.left, r.top, w, h);
                int i = 0;
                for (Gtk::Widget* slot : {inlineFlagPickIcon_, inlineFlagUnflagIcon_, inlineFlagRejectIcon_}) {
                    Gtk::Allocation a = slot->get_allocation();
                    Gtk::Widget* img = dynamic_cast<Gtk::Bin*>(slot)
                        ? static_cast<Gtk::Bin*>(slot)->get_child() : nullptr;
                    Gtk::Allocation ia = img ? img->get_allocation() : Gtk::Allocation();
                    int minW = -1, natW = -1, minH = -1, natH = -1;
                    int st = -1, sw = -1, sh = -1;
                    double dsx = 0.0, dsy = 0.0;
                    if (img) {
                        img->get_preferred_width(minW, natW);
                        img->get_preferred_height(minH, natH);
                        GtkImage* gi = GTK_IMAGE(img->gobj());
                        st = (int)gtk_image_get_storage_type(gi);
                        if (st == GTK_IMAGE_SURFACE) {
                            cairo_surface_t* cs = nullptr;
                            g_object_get(gi, "surface", &cs, nullptr);
                            if (cs) {
                                sw = cairo_image_surface_get_width(cs);
                                sh = cairo_image_surface_get_height(cs);
                                cairo_surface_get_device_scale(cs, &dsx, &dsy);
                                cairo_surface_destroy(cs);
                            }
                        }
                    }
                    fprintf(log,
                        "[%s] slot%d vis=%d map=%d a=(%d,%d %dx%d) img map=%d a=(%d,%d %dx%d) op=%.2f pref=(%d/%d x %d/%d) st=%d surf=%dx%d ds=(%.2f,%.2f)\n",
                        tag, i++, (int)slot->get_visible(), (int)slot->get_mapped(),
                        a.get_x(), a.get_y(), a.get_width(), a.get_height(),
                        img ? (int)img->get_mapped() : -1,
                        ia.get_x(), ia.get_y(), ia.get_width(), ia.get_height(),
                        img ? img->get_opacity() : -1.0,
                        minW, natW, minH, natH, st, sw, sh, dsx, dsy);
                }
                fclose(log);
            }
        };
        // STEEP_MENU_WATCH: dump on every REAL popup (map), once, with
        // rolling ids — reproduce the artifact by hand and the dumps
        // capture screen truth vs GTK's render at the same instant.
        if (g_getenv("STEEP_MENU_WATCH")) {
            auto watchCount = std::make_shared<int>(0);
            pmenu->signal_map().connect([this, watchCount, dump]() {
                const int id = ++(*watchCount);
                Glib::signal_timeout().connect_once([this, id, dump]() {
                    if (pmenu->get_mapped()) {
                        dump(("w" + std::to_string(id)).c_str());
                    }
                }, 450);
            });
        }

        if (g_getenv("STEEP_MENU_SELFTEST")) {
        auto stage = std::make_shared<int>(0);
        // Vary the popup position per stage: a same-rect re-popup never
        // gets a configure event, so GTK's freeze-until-configure keeps
        // the window from ever painting — a real pathology, but not the
        // user's (real popups open at the moving pointer).
        auto popupAt = [this](int xOff) {
            Gdk::Rectangle r(40 + xOff, 40, 1, 1);
            pmenu->popup_at_rect(get_window(), r,
                                 Gdk::GRAVITY_SOUTH_WEST,
                                 Gdk::GRAVITY_NORTH_WEST, nullptr);
        };
        auto pickImage = [this]() -> Gtk::Widget* {
            auto* bin = dynamic_cast<Gtk::Bin*>(inlineFlagPickIcon_);
            return bin ? bin->get_child() : nullptr;
        };
        Glib::signal_timeout().connect([this, stage, dump, popupAt, pickImage]() -> bool {
            switch (*stage) {
            case 0:
                if (!get_mapped()) {
                    return true;  // wait for the browser to be on screen
                }
                popupAt(0);
                break;
            case 1:
                dump("s1_all");
                pmenu->popdown();
                inlineFlagPickIcon_->set_visible(false);
                break;
            case 2:
                popupAt(60);
                break;
            case 3:
                dump("s2_pickhidden");
                pmenu->popdown();
                inlineFlagPickIcon_->set_visible(true);
                inlineFlagUnflagIcon_->set_visible(false);
                break;
            case 4:
                popupAt(120);
                break;
            case 5:
                dump("s3_reshown");
                // simulate the pointer poll raising the icon under the
                // pointer, exactly like setHover does
                if (auto* img = pickImage()) {
                    img->set_opacity(1.0);
                    inlineFlagPickIcon_->queue_draw();
                }
                break;
            case 6:
                dump("s4_hover_on");
                if (auto* img = pickImage()) {
                    img->set_opacity(0.65);
                    inlineFlagPickIcon_->queue_draw();
                }
                break;
            case 7:
                dump("s5_hover_off");
                pmenu->popdown();
                inlineFlagUnflagIcon_->set_visible(true);
                return false;
            }
            ++(*stage);
            return true;
        }, 700);
        }
    }
#endif
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
    delete inlineCopySettingsMenu_;
    delete inlineApplyMenu_;
    delete inlineTipWindow_;
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

    // Save-image and external-editor live in the inline Actions row now;
    // their identity items stay hidden in every mode.
    saveImage->hide();
    if (editExternal) {
        editExternal->hide();
    }

    // "Set as album cover" — only when viewing an album and exactly one image is selected
    {
        MYREADERLOCK(l2, entryRW);
        // The inline actions-row icon replaces the old menu item; it only
        // appears when a target album has actually been configured.
        if (inlineAddToAlbumIcon_) {
            const bool available = addToAlbumSetter_
                && (!addToAlbumAvailable_ || addToAlbumAvailable_())
                && selected.size() == 1;
            inlineAddToAlbumIcon_->set_visible(available);
        }

        // Flag icons: offer only the transitions that would change
        // something — an icon hides when every selected image already
        // carries that pick state.
        {
            // Empty selection degrades to the unflagged default.
            bool anyNotPicked = selected.empty(), anyNotUnflagged = false, anyNotRejected = selected.empty();
            for (const auto entry : selected) {
                const int pick = entry->thumbnail ? entry->thumbnail->getPick() : 0;
                anyNotPicked   |= pick != 1;
                anyNotUnflagged |= pick != 0;
                anyNotRejected |= pick != -1;
            }
            if (inlineFlagPickIcon_) {
                inlineFlagPickIcon_->set_visible(anyNotPicked);
            }
            if (inlineFlagUnflagIcon_) {
                inlineFlagUnflagIcon_->set_visible(anyNotUnflagged);
            }
            if (inlineFlagRejectIcon_) {
                inlineFlagRejectIcon_->set_visible(anyNotRejected);
            }
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
    const auto itemName = [this](Gtk::MenuItem* which) -> const char* {
        return which == autoGradedFilm ? "gradedFilm"
            : which == autoGradeFilm ? "gradeFilm"
            : which == autoGrade ? "grade"
            : which == autoEditMenu ? "parent"
            : which ? "other" : "null";
    };

    if (!item || quickActionRunning_) {
        fileBrowserPerfLog("[autoEditHover] enter item=%s REJECT quickAction=%d\n",
                           itemName(item), quickActionRunning_ ? 1 : 0);
        return;
    }
    // Native popup themes may clear GTK's implicit prelight between motion
    // events. Reassert it before the same-item early return as well.
    item->set_state_flags(Gtk::STATE_FLAG_PRELIGHT, false);
    if (autoEditHoverItem_ == item
            && (autoEditHoverDelayConnection_.connected()
                || autoEditHoverInFlight_
                || !autoEditHoverPreviewFile_.empty())) {
        fileBrowserPerfLog(
            "[autoEditHover] enter item=%s LATCHED delay=%d inflight=%d preview='%s'\n",
            itemName(item),
            autoEditHoverDelayConnection_.connected() ? 1 : 0,
            autoEditHoverInFlight_ ? 1 : 0,
            autoEditHoverPreviewFile_.c_str());
        return;
    }

    fileBrowserPerfLog("[autoEditHover] enter item=%s ARM (was item=%s)\n",
                       itemName(item), itemName(autoEditHoverItem_));
    cancelAutoEditHoverPreview(nullptr, true);
    autoEditHoverItem_ = item;

    autoEditHoverDelayConnection_ = Glib::signal_timeout().connect(
        [this, item]() -> bool {
            if (autoEditHoverItem_ != item || quickActionRunning_) {
                fileBrowserPerfLog("[autoEditHover] delay DROPPED itemChanged=%d quickAction=%d\n",
                                   autoEditHoverItem_ != item ? 1 : 0,
                                   quickActionRunning_ ? 1 : 0);
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

                const rtengine::procparams::ProcParams sourceParams = thumbnail->getProcParamsCopy();
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
                        // Only an editor that actually took the preview may
                        // claim the "already showing" latch. Setting it here
                        // unconditionally - before the thumbnail render even
                        // finished - wedged the hover: a first-attempt render
                        // failure left nothing visible while the same-item
                        // guard blocked every re-hover until the pointer
                        // visited a different item.
                        if (editorApplied) {
                            autoEditHoverPreviewFile_ = filename;
                        }
                        return false;
                    });

                double scale = 1.0;
                const int previewHeight = desired.first.scaleToDevice(desired.second).height;
                rtengine::IImage8* image = thumbnail->processFullThumbImage(params, previewHeight, scale);
                thumbnail->decreaseRef();

                if (!image || generationState->load(std::memory_order_acquire) != generation) {
                    delete image;

                    // A failed render must unlatch, or the same-item guard
                    // turns one transient failure (thumb source not resident
                    // yet, typically) into a dead menu item. Clearing the
                    // hover item lets the 35ms pointer poll restart the
                    // preview by itself while the pointer stays put.
                    if (generationState->load(std::memory_order_acquire) == generation) {
                        idle_register.add([this, generationState, generation, filename]() -> bool {
                            if (generationState->load(std::memory_order_acquire) == generation
                                    && autoEditHoverPreviewFile_.empty()) {
                                fileBrowserPerfLog(
                                    "[autoEditHover] render failed, unlatching file=%s\n",
                                    filename.c_str());
                                autoEditHoverInFlight_ = false;
                                autoEditHoverItem_ = nullptr;
                            }
                            return false;
                        });
                    }
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
                    autoEditHoverInFlight_ = false;
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

    if (autoEditHoverDelayConnection_.connected() || autoEditHoverItem_) {
        fileBrowserPerfLog("[autoEditHover] cancel restore=%d hadDelay=%d\n",
                           restore ? 1 : 0,
                           autoEditHoverDelayConnection_.connected() ? 1 : 0);
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
    if (layoutPaused_()) {
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
                    const rtengine::procparams::ProcParams sourceParams = thm->getProcParamsCopy();
                    features = buildSteepAutoEditParamsInternal(*thm, state->mode, sourceParams, pp);
                } catch (const std::exception& exception) {
                    succeeded = false;
                    error = exception.what();
                    pp = thm->getProcParamsCopy();
                } catch (...) {
                    succeeded = false;
                    error = "unknown error";
                    pp = thm->getProcParamsCopy();
                }

                idle_register.add([this, state, finishBatch, thm, features, pp = std::move(pp), succeeded, error = std::move(error)]() mutable -> bool {
                    if (!succeeded) {
                        fileBrowserPerfLog(
                            "[autoEditBatch] failed mode=%s error=%s file=%s\n",
                            autoEditModeName(state->mode),
                            error.c_str(),
                            thm->getFileName().c_str());
                    } else {
                        const auto& autoCurve = pp.rgbCurves.mastercurve;
                        const double autoCurveUpperSlope =
                            autoCurve.size() >= 9 && autoCurve[7] > autoCurve[5]
                            ? (autoCurve[8] - autoCurve[6]) / (autoCurve[7] - autoCurve[5])
                            : 1.0;
                        fileBrowserPerfLog(
                            "[autoEditBatch] mode=%s scene=%s median=%.3f p90=%.3f p98=%.3f range=%.3f highlights=%.3f clipped=%.4f curveUpperSlope=%.3f sat=%.3f skin=%.3f skinSat=%.3f sky=%.3f foliage=%.3f edge=%.3f iso=%u exposure=%.3f brightness=%d contrast=%d highlight=%d film=%s strength=%d file=%s\n",
                            autoEditModeName(state->mode),
                            autoGradeSceneName(features.scene),
                            features.medianLuma,
                            features.p90,
                            features.p98,
                            features.dynamicRange,
                            features.highlightFraction,
                            features.clippedFraction,
                            autoCurveUpperSlope,
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

                // Carry the applied settings to the duplicate: prefer the
                // in-memory params (covers cache-only edits with no sidecar
                // written yet), fall back to copying the sidecar file.
                bool paramsWritten = false;

                if (mselected[i]->thumbnail && mselected[i]->thumbnail->hasProcParams()) {
                    rtengine::procparams::ProcParams pp = mselected[i]->thumbnail->getProcParams();
                    paramsWritten = pp.save(destPath + ".pp3") == 0;
                }

                if (!paramsWritten) {
                    Glib::ustring pp3Src = srcPath + ".pp3";
                    auto pp3File = Gio::File::create_for_path(pp3Src);
                    if (pp3File->query_exists()) {
                        pp3File->copy(Gio::File::create_for_path(destPath + ".pp3"));
                    }
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
    if (!layoutPaused_()) {
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
    if (!layoutPaused_()) {
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

bool FileBrowser::pinEntryAfter (const Glib::ustring& path, const Glib::ustring& anchorPath)
{
    bool found = false;

    {
        MYWRITERLOCK(l, entryRW);

        for (auto* entry : fd) {
            if (entry->filename == path) {
                entry->pinAfter = anchorPath;
                found = true;
                break;
            }
        }

        if (found) {
            applyPinnedOrder_();
            entriesOrderChanged_();
        }
    }

    if (found) {
        redraw();
    }

    return found;
}

bool FileBrowser::checkFilter (ThumbBrowserEntryBase* entryb) const   // true -> entry complies filter
{
    // Pinned partners (double-exposure "edit this layer") stay visible next
    // to their anchor no matter what the browser filter says.
    if (!entryb->pinAfter.empty()) {
        return true;
    }

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

void FileBrowser::requestRotateSelected (int degrees)
{
    std::vector<FileBrowserEntry*> mselected;
    {
        MYREADERLOCK(l, entryRW);
        mselected.reserve(selected.size());
        for (auto* sel : selected) {
            mselected.push_back(static_cast<FileBrowserEntry*>(sel));
        }
    }

    if (mselected.empty()) {
        return;
    }

    if (bppcl) {
        bppcl->beginBatchPParamsChange(mselected.size());
    }

    for (auto* entry : mselected) {
        entry->thumbnail->createProcParamsForUpdate(false, false, true);
        rtengine::procparams::ProcParams pp = entry->thumbnail->getProcParams();
        pp.coarse.rotate = ((pp.coarse.rotate + degrees) % 360 + 360) % 360;
        entry->thumbnail->setProcParams(pp, nullptr, FILEBROWSER, false);
        queueThumbnailPersist(entry->thumbnail);
    }

    if (bppcl) {
        bppcl->endBatchPParamsChange();
    }

    queue_draw();
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
                openEntry->retryThumbnailNow();
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
                openEntry->retryThumbnailNow();
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

        openEntry->retryThumbnailNow();
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

    openEntry->retryThumbnailNow();
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

void FileBrowser::visibleRangeChanged ()
{
    // Only worth doing while entries are still being produced. The loader
    // otherwise works through the folder alphabetically around a hint that was
    // set once when the directory was opened, so scrolling during a cold load
    // used to leave the viewport waiting behind unrelated files.
    if (!previewLoader->hasPendingWork()) {
        if (!lastViewportHint_.empty()) {
            lastViewportHint_.clear();
        }
        return;
    }

    Glib::ustring firstVisible;
    {
        MYREADERLOCK(l, entryRW);

        if (!visibleEntries_.empty()) {
            firstVisible = visibleEntries_.front()->filename;
        }
    }

    if (firstVisible.empty() || firstVisible == lastViewportHint_) {
        return;
    }

    lastViewportHint_ = firstVisible;
    previewLoader->setPriorityHint(firstVisible);
}

void FileBrowser::thumbRearrangementNeeded ()
{
    // The entry already adopted the delivered pixel geometry. Reflow without
    // regenerating every thumbnail or restoring stale metadata.
    //
    // This fires for practically every RAW thumbnail during a cold folder load
    // (the cache records sensor dimensions while the rendered thumb comes from
    // the embedded JPEG, so the delivered aspect almost always differs from the
    // predicted one). Registering an idle per delivery made that O(N) reflow
    // run N times; coalesce the burst into a single pass instead.
    scheduleRelayout();
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
    FileBrowserEntry* recoveryEntry = nullptr;

    {
        MYREADERLOCK(l, entryRW);

        thm.reserve(selected.size());

        for (size_t i = 0; i < selected.size(); i++) {
            thm.push_back ((static_cast<FileBrowserEntry*>(selected[i]))->thumbnail);
        }

        if (lastClicked) {
            recoveryEntry = static_cast<FileBrowserEntry*>(lastClicked);
        } else if (selected.size() == 1) {
            recoveryEntry = static_cast<FileBrowserEntry*>(selected.front());
        }
    }

    if (recoveryEntry) {
        recoveryEntry->retryThumbnailNow();
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
        mselected[i]->retryThumbnailNow();
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

void FileBrowser::openSourceFolder()
{
    Glib::ustring sourcePath;

    {
        MYREADERLOCK(l, entryRW);

        if (!selected.empty()) {
            sourcePath = static_cast<FileBrowserEntry*>(selected.front())->filename;
        }
    }

    if (sourcePath.empty()) {
        return;
    }

    bool launchScheduled = false;

#ifdef _WIN32
    // Windows has no GIO handler registered for a file:// folder URI, so
    // launch_default_for_uri quietly does nothing. Ask the shell directly,
    // and use /select so the folder opens with this photo highlighted.
    {
        std::string windowsPath = sourcePath.raw();

        for (auto& character : windowsPath) {
            if (character == '/') {
                character = '\\';
            }
        }

        const std::string argument = "/select,\"" + windowsPath + "\"";

        if (wchar_t* wideArgument = reinterpret_cast<wchar_t*>(
                g_utf8_to_utf16(argument.c_str(), -1, nullptr, nullptr, nullptr))) {
            const HINSTANCE result = ShellExecuteW(
                nullptr, L"open", L"explorer.exe", wideArgument, nullptr, SW_SHOWNORMAL);
            launchScheduled = reinterpret_cast<INT_PTR>(result) > 32;
            g_free(wideArgument);
        }
    }

    if (launchScheduled) {
        return;
    }
#endif

    try {
        const auto sourceFile = Gio::File::create_for_path(sourcePath);
        const auto sourceFolder = sourceFile ? sourceFile->get_parent() : Glib::RefPtr<Gio::File>();

        if (sourceFolder) {
            // Shell activation may wait on Explorer, D-Bus, or MIME handler
            // discovery. Never hold GTK's main loop while the OS responds.
            Gio::AppInfo::launch_default_for_uri_async(sourceFolder->get_uri());
            launchScheduled = true;
        }
    } catch (const Glib::Error& error) {
        if (App::get().options().rtSettings.verbose) {
            std::fprintf(stderr, "Could not open source folder for %s: %s\n",
                         sourcePath.c_str(), error.what().c_str());
        }
    }

    if (!launchScheduled) {
        Gtk::MessageDialog dialog(
            getToplevelWindow(this),
            M("FILEBROWSER_OPENSOURCEFOLDERERROR"),
            false,
            Gtk::MESSAGE_ERROR,
            Gtk::BUTTONS_OK,
            true);
        dialog.run();
    }
}

void FileBrowser::inspectRequested(std::vector<FileBrowserEntry*> mselected)
{
    getInspector()->showWindow(true);
}
