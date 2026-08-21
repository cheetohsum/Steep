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
#include <memory>
#include <vector>

#include "improcfun.h"

#include "doubleexposureblend.h"
#include "imagefloat.h"
#include "partnerimagestore.h"
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
    float invCover; // base full-res px -> partner full-res px (cover fit)
    procparams::DoubleExposureParams::BlendMode mode;
    bool gateOnLayer;   // gate luminance source: layer sample vs accumulated base
    float gateLow;      // window in 0..1 linear luminance
    float gateHigh;
    float gateFeather;
    float gateStrength; // 0..1; 0 = gate off
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
    const bool fullRes = fullResPartners;

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
            // Cover fit: scale the partner up until it fills the base frame.
            const float cover = std::max(static_cast<float>(fullW) / partner->fullWidth,
                                         static_cast<float>(fullH) / partner->fullHeight);
            rl.invCover = 1.f / cover;
            rl.mode = layer.blendMode;
            rl.gateOnLayer = layer.gateSource == procparams::DoubleExposureParams::GateSource::LAYER;
            rl.gateLow = LIM01(static_cast<float>(layer.gateLow) / 100.f);
            rl.gateHigh = LIM01(static_cast<float>(layer.gateHigh) / 100.f);
            rl.gateFeather = LIM01(static_cast<float>(layer.gateFeather) / 100.f);
            rl.gateStrength = LIM01(static_cast<float>(layer.gateStrength) / 100.f);
            resolved.push_back(std::move(rl));
        }
    }

    if (settings->verbose) {
        std::fprintf(stderr, "[doubleExposure] %dx%d full=%dx%d off=%d,%d skip=%.2f layers=%u resolved=%u fullRes=%d\n",
                     W, H, fullW, fullH, offX, offY, skip,
                     static_cast<unsigned>(deParams.layers.size()), static_cast<unsigned>(resolved.size()),
                     fullResPartners ? 1 : 0);
    }

    if (resolved.empty()) {
        return;
    }

    // In-camera practice: meter every frame of an N-frame multiple exposure
    // down by log2(N) EV so the summed exposure lands correctly. Only ADD
    // layers stack light, so only they (and the base) are metered down.
    int addLayers = 0;

    for (const auto& rl : resolved) {
        if (rl.mode == procparams::DoubleExposureParams::BlendMode::ADD) {
            ++addLayers;
        }
    }

    const float autoGainFactor = (deParams.autoGain && addLayers > 0)
                                 ? 1.f / static_cast<float>(addLayers + 1)
                                 : 1.f;
    const float baseGain = static_cast<float>(std::pow(2.0, deParams.baseEv)) * autoGainFactor;

    for (auto& rl : resolved) {
        if (rl.mode == procparams::DoubleExposureParams::BlendMode::ADD) {
            rl.gain *= autoGainFactor;
        }
    }

    constexpr float white = 65535.f;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = 0; y < H; ++y) {
        const float fy = offY + (y + 0.5f) * skip;

        for (int x = 0; x < W; ++x) {
            const float fx = offX + (x + 0.5f) * skip;

            float r = rgb->r(y, x) * baseGain;
            float g = rgb->g(y, x) * baseGain;
            float b = rgb->b(y, x) * baseGain;

            for (const auto& rl : resolved) {
                const float u = (fx - fullW * 0.5f) * rl.invCover + rl.partner->fullWidth * 0.5f;
                const float v = (fy - fullH * 0.5f) * rl.invCover + rl.partner->fullHeight * 0.5f;

                float pr, pg, pb;
                samplePartner(*rl.partner, u, v, pr, pg, pb);

                pr = std::max(pr, 0.f) * rl.gain;
                pg = std::max(pg, 0.f) * rl.gain;
                pb = std::max(pb, 0.f) * rl.gain;

                float cr, cg, cb;
                deblend::blend(rl.mode, white, r, g, b, pr, pg, pb, cr, cg, cb);

                float w = rl.opacity;

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

            rgb->r(y, x) = r;
            rgb->g(y, x) = g;
            rgb->b(y, x) = b;
        }
    }
}

} // namespace rtengine
