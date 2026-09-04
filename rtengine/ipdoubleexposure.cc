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

// Double exposure: composite partner images onto the base plate in
// scene-referred linear working space, before any tone processing — the same
// place light stacks on a single frame of film.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "improcfun.h"

#include "doubleexposureblend.h"
#include "imagefloat.h"
#include "partnerimagestore.h"
#include "partnermaskstore.h"
#include "procparams.h"
#include "rt_math.h"
#include "settings.h"

namespace rtengine
{

namespace
{

struct ResolvedLayer {
    std::shared_ptr<PartnerImage> partner;
    float gain;     // 2^ev, with the auto film gain folded in for ADD layers
    float opacity;  // 0..1
    // Cover fit, placement, rotation and tiling, in partner full-res pixels.
    // See deplace::map — the picker's preview builds the same frame.
    deplace::Frame frame;
#ifdef RT_AI_MASKING
    // Subject selection, segmented on the partner. Null when the layer asks
    // for no mask, or when segmentation is unavailable — the layer then
    // renders unmasked rather than disappearing.
    std::shared_ptr<const PartnerMask> mask;
#endif
    procparams::DoubleExposureParams::BlendMode mode;
    procparams::DoubleExposureParams::Compare compare; // comparative modes: whole pixel vs per channel
    float softness;     // comparative hand-over band, stops
    bool gateOnLayer;   // gate luminance source: layer sample vs accumulated base
    float gateLow;      // window in 0..1 linear luminance
    float gateHigh;
    float gateFeather;
    float gateStrength; // 0..1; 0 = gate off
};

// Where one layer landed at one pixel, kept between the two passes: the film
// gain cannot be known until every layer's coverage is.
struct Hit {
    float u;
    float v;
    float coverage;
    bool present;
};

// Bilinear sample of the partner tier at partner full-frame coords (u, v),
// edge-clamped.
inline void samplePartner(const PartnerImage& p, float u, float v, float& r, float& g, float& b)
{
    const Imagefloat& img = *p.image;
    const int w = img.getWidth();
    const int h = img.getHeight();

    float tu = u / p.skip - 0.5f;
    float tv = v / p.skip - 0.5f;

    int x0 = static_cast<int>(std::floor(tu));
    int y0 = static_cast<int>(std::floor(tv));
    float dx = tu - x0;
    float dy = tv - y0;

    if (x0 < 0) {
        x0 = 0;
        dx = 0.f;
    } else if (x0 > w - 1) {
        x0 = w - 1;
        dx = 0.f;
    }

    if (y0 < 0) {
        y0 = 0;
        dy = 0.f;
    } else if (y0 > h - 1) {
        y0 = h - 1;
        dy = 0.f;
    }

    const int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    const int y1 = y0 + 1 < h ? y0 + 1 : h - 1;

    const float w00 = (1.f - dx) * (1.f - dy);
    const float w10 = dx * (1.f - dy);
    const float w01 = (1.f - dx) * dy;
    const float w11 = dx * dy;

    r = w00 * img.r(y0, x0) + w10 * img.r(y0, x1) + w01 * img.r(y1, x0) + w11 * img.r(y1, x1);
    g = w00 * img.g(y0, x0) + w10 * img.g(y0, x1) + w01 * img.g(y1, x0) + w11 * img.g(y1, x1);
    b = w00 * img.b(y0, x0) + w10 * img.b(y0, x1) + w01 * img.b(y1, x0) + w11 * img.b(y1, x1);
}

} // namespace

void ImProcFunctions::doubleExposure(Imagefloat* rgb, const procparams::DoubleExposureParams& deParams,
                                     const Glib::ustring& workingProfile,
                                     int fullW, int fullH, int offX, int offY, float skip, bool fullResPartners)
{
    if (!rgb || !deParams.enabled || deParams.layers.empty()) {
        return;
    }

    const int W = rgb->getWidth();
    const int H = rgb->getHeight();

    if (W <= 0 || H <= 0 || fullW <= 0 || fullH <= 0 || skip <= 0.f) {
        return;
    }

    // Interactive pipelines (preview, detail windows) always sample the
    // preview tier: decoding a full raw partner synchronously inside a crop
    // update stalls 1:1 previews for seconds. Only export pays for full res.
    // STEEP_DE_PREVIEW_TIER=1 forces the preview tier for export too: a
    // diagnostic switch so steep-cli can reproduce the interactive composite.
    static const bool forcePreviewTier = std::getenv("STEEP_DE_PREVIEW_TIER") != nullptr;
    const bool fullRes = fullResPartners && !forcePreviewTier;

    if (settings->verbose) {
        std::fprintf(stderr, "[doubleExposure] enter %dx%d full=%dx%d skip=%.2f layers=%u fullRes=%d\n",
                     W, H, fullW, fullH, skip, static_cast<unsigned>(deParams.layers.size()),
                     fullResPartners ? 1 : 0);
    }

    std::vector<ResolvedLayer> resolved;
    resolved.reserve(deParams.layers.size());

    for (const auto& layer : deParams.layers) {
        if (!layer.enabled) {
            continue;
        }

        auto partner = PartnerImageStore::getInstance().getPartner(layer.path, workingProfile, fullRes);

        if (partner && partner->image && partner->fullWidth > 0 && partner->fullHeight > 0) {
            ResolvedLayer rl;
            rl.partner = partner;
            rl.gain = static_cast<float>(std::pow(2.0, layer.ev));
            rl.opacity = LIM01(static_cast<float>(layer.opacity) / 100.f);

            // Cover fit: scale the source rect up until it fills the base
            // frame. The rect is the whole partner unless the layer is
            // cropped to its subject.
            deplace::Frame& fr = rl.frame;
            fr.baseW = static_cast<float>(fullW);
            fr.baseH = static_cast<float>(fullH);
            fr.srcX0 = 0.f;
            fr.srcY0 = 0.f;
            fr.srcW = static_cast<float>(partner->fullWidth);
            fr.srcH = static_cast<float>(partner->fullHeight);
            bool cropped = false;

#ifdef RT_AI_MASKING
            rl.mask = PartnerMaskStore::getInstance().getMask(layer.path, workingProfile, layer.maskClass,
                                                              layer.maskFeather, layer.maskInvert, multiThread);

            // Cropping to the subject is the whole of "pattern this subject":
            // the source rect becomes the mask's bounding box and every step
            // downstream — cover fit, frame edge, tiling — follows it.
            if (layer.cropToSubject && rl.mask && rl.mask->hasBounds()) {
                fr.srcX0 = static_cast<float>(rl.mask->x0);
                fr.srcY0 = static_cast<float>(rl.mask->y0);
                fr.srcW = static_cast<float>(rl.mask->x1 - rl.mask->x0);
                fr.srcH = static_cast<float>(rl.mask->y1 - rl.mask->y0);
                cropped = true;
            }
#endif

            fr.invCover = 1.f / std::max(fr.baseW / fr.srcW, fr.baseH / fr.srcH);
            deplace::applyLayer(fr, layer, cropped);

            rl.mode = layer.blendMode;
            rl.compare = layer.compare;
            rl.softness = std::max(static_cast<float>(layer.softness), 0.f);
            rl.gateOnLayer = layer.gateSource == procparams::DoubleExposureParams::GateSource::LAYER;
            rl.gateLow = LIM01(static_cast<float>(layer.gateLow) / 100.f);
            rl.gateHigh = LIM01(static_cast<float>(layer.gateHigh) / 100.f);
            rl.gateFeather = LIM01(static_cast<float>(layer.gateFeather) / 100.f);
            rl.gateStrength = LIM01(static_cast<float>(layer.gateStrength) / 100.f);
            resolved.push_back(std::move(rl));
        }
    }

    if (settings->verbose) {
        std::fprintf(stderr, "[doubleExposure] %dx%d full=%dx%d off=%d,%d skip=%.2f layers=%u resolved=%u fullRes=%d latitude=%.0f\n",
                     W, H, fullW, fullH, offX, offY, skip,
                     static_cast<unsigned>(deParams.layers.size()), static_cast<unsigned>(resolved.size()),
                     fullResPartners ? 1 : 0, deParams.highlightLatitude);
    }

    if (resolved.empty()) {
        return;
    }

    // In-camera practice: meter every frame of an N-frame multiple exposure
    // down by log2(N) EV so the summed exposure lands correctly. Only ADD
    // layers stack light, so only they (and the base) are metered down.
    //
    // How many frames land is a per-pixel question once a layer can be placed,
    // tiled or cut out: outside its frame there is only one exposure, and
    // metering the base down for a second one that is not there leaves a hard
    // step at the boundary that no amount of edge blending can remove -- the
    // discontinuity is in the base's own gain. So the count follows coverage.
    // A layer that covers everything contributes exactly 1, which is what the
    // whole-frame case has always done.
    int addLayers = 0;

    for (const auto& rl : resolved) {
        if (rl.mode == procparams::DoubleExposureParams::BlendMode::ADD) {
            ++addLayers;
        }
    }

    const bool meterFrames = deParams.autoGain && addLayers > 0;
    const float baseEvGain = static_cast<float>(std::pow(2.0, deParams.baseEv));

    constexpr float white = 65535.f;

    // Film shoulder on the finished stack (see deblend::shoulder). Off at 0:
    // legacy files then clip downstream exactly as they always did. The knee
    // is measured against ONE frame's white, not the pipeline's: with auto
    // film gain the averaged stack never passes half white, so a knee in
    // pipeline units would have nothing to act on. Undoing the gain, then
    // shouldering, then re-applying it makes the control behave the same
    // whether the frames were metered down or not.
    const float latitude = LIM01(static_cast<float>(deParams.highlightLatitude) / 100.f);
    const bool applyShoulder = latitude > 0.f;
    const float knee = deblend::latitudeKnee(latitude);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = 0; y < H; ++y) {
        const float fy = offY + (y + 0.5f) * skip;

        std::vector<Hit> hits(resolved.size());

        for (int x = 0; x < W; ++x) {
            const float fx = offX + (x + 0.5f) * skip;

            // Where each layer lands, and how much of it is here. Outside a
            // placed frame the layer is simply absent, with a one-output-pixel
            // anti-aliased border (wider, if its edge is set to blend).
            float framesHere = 0.f;

            for (size_t li = 0; li < resolved.size(); ++li) {
                Hit& hit = hits[li];
                hit.present = deplace::map(resolved[li].frame, fx, fy, skip, hit.u, hit.v, hit.coverage);

                if (hit.present && resolved[li].mode == procparams::DoubleExposureParams::BlendMode::ADD) {
                    framesHere += hit.coverage;
                }
            }

            const float gainFactor = meterFrames ? 1.f / (1.f + framesHere) : 1.f;
            const float baseGain = baseEvGain * gainFactor;
            const float shoulderWhite = white * gainFactor;

            float r = rgb->r(y, x) * baseGain;
            float g = rgb->g(y, x) * baseGain;
            float b = rgb->b(y, x) * baseGain;

            for (size_t li = 0; li < resolved.size(); ++li) {
                const ResolvedLayer& rl = resolved[li];
                const Hit& hit = hits[li];

                if (!hit.present) {
                    continue;
                }

                const float u = hit.u;
                const float v = hit.v;
                const float coverage = hit.coverage;
                const float gain = rl.mode == procparams::DoubleExposureParams::BlendMode::ADD
                                   ? rl.gain * gainFactor : rl.gain;

                float pr, pg, pb;
                samplePartner(*rl.partner, u, v, pr, pg, pb);

                pr = std::max(pr, 0.f) * gain;
                pg = std::max(pg, 0.f) * gain;
                pb = std::max(pb, 0.f) * gain;

                float cr, cg, cb;
                deblend::blend(rl.mode, rl.compare, rl.softness, white, r, g, b, pr, pg, pb, cr, cg, cb);

                float w = rl.opacity * coverage;

#ifdef RT_AI_MASKING
                if (rl.mask) {
                    w *= rl.mask->sample(u, v);

                    if (w <= 0.f) {
                        continue;
                    }
                }
#endif

                if (rl.gateStrength > 0.f) {
                    const float lum = (rl.gateOnLayer ? deblend::lum709(pr, pg, pb)
                                                      : deblend::lum709(r, g, b)) / white;
                    w *= deblend::gateWeight(rl.gateStrength,
                                             deblend::gateWindow(deblend::gateEncode(lum), rl.gateLow, rl.gateHigh, rl.gateFeather));
                }

                r += w * (cr - r);
                g += w * (cg - g);
                b += w * (cb - b);
            }

            if (applyShoulder) {
                r = shoulderWhite * deblend::shoulder(std::max(r, 0.f) / shoulderWhite, knee);
                g = shoulderWhite * deblend::shoulder(std::max(g, 0.f) / shoulderWhite, knee);
                b = shoulderWhite * deblend::shoulder(std::max(b, 0.f) / shoulderWhite, knee);
            }

            rgb->r(y, x) = r;
            rgb->g(y, x) = g;
            rgb->b(y, x) = b;
        }
    }
}

} // namespace rtengine
