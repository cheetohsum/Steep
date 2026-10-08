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
#include <mutex>
#include <list>

#include "improcfun.h"

#include "doubleexposureblend.h"
#include "demotion.h"
#include "imagefloat.h"
#include "partnerimagestore.h"
#include "partnermaskstore.h"
#include "partnerdepthstore.h"
#include "procparams.h"
#include "rt_math.h"
#include "settings.h"

namespace rtengine
{

namespace
{

struct MotionPlate : demotion::Buffer {
    std::shared_ptr<PartnerImage> source;
    procparams::DEMotionParams params;
#ifdef RT_AI_MASKING
    std::shared_ptr<const PartnerMask> mask;
    std::shared_ptr<const PartnerDepth> depth;
    DepthSelection depthSelect;
#endif
};
struct MotionGate {
    const PartnerImage* source = nullptr;
    procparams::DEMotionParams params;
#ifdef RT_AI_MASKING
    const PartnerMask* mask = nullptr;
    const PartnerDepth* depth = nullptr;
    DepthSelection depthSelect;
#endif
    std::weak_ptr<std::mutex> mutex;
};

struct ResolvedLayer {
    std::shared_ptr<PartnerImage> partner;
    demotion::Trajectory motion;
    std::shared_ptr<const MotionPlate> motionPlate;
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
    // Depth confinement, on the same terms: null when the layer does not
    // ask for it or no estimate could be made.
    std::shared_ptr<const PartnerDepth> depth;
    DepthSelection depthSelect;
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

// Only interactive plates are retained. Export samples the full source directly.
std::shared_ptr<const MotionPlate> previewMotion(const ResolvedLayer& layer, const procparams::DEMotionParams& params)
{
    static std::mutex mutex;
    static std::list<std::shared_ptr<const MotionPlate>> cache;
    static std::list<MotionGate> gates;
    std::shared_ptr<std::mutex> gate;
    constexpr size_t budget = 128U * 1024U * 1024U;
    const auto matches = [&](const std::shared_ptr<const MotionPlate>& p) {
        return p->source == layer.partner && p->params == params
#ifdef RT_AI_MASKING
            && p->mask == layer.mask && p->depth == layer.depth && p->depthSelect == layer.depthSelect
#endif
            ;
    };
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto i = cache.begin(); i != cache.end(); ++i) if (matches(*i)) {
            const auto result = *i;
            cache.splice(cache.end(), cache, i);
            return result;
        }
        for (auto i = gates.begin(); i != gates.end();) {
            if (i->mutex.expired()) { i = gates.erase(i); continue; }
            if (i->source == layer.partner.get() && i->params == params
#ifdef RT_AI_MASKING
                && i->mask == layer.mask.get() && i->depth == layer.depth.get() && i->depthSelect == layer.depthSelect
#endif
            ) gate = i->mutex.lock();
            ++i;
        }
        if (!gate) {
            gate = std::make_shared<std::mutex>();
            MotionGate key;
            key.source = layer.partner.get(); key.params = params; key.mutex = gate;
#ifdef RT_AI_MASKING
            key.mask = layer.mask.get();
            key.depth = layer.depth.get();
            key.depthSelect = layer.depthSelect;
#endif
            gates.push_back(key);
        }
    }
    std::lock_guard<std::mutex> singleRender(*gate);
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& existing : cache) if (matches(existing)) return existing;
    }
    auto result = std::make_shared<MotionPlate>();
    result->source = layer.partner;
    result->params = params;
#ifdef RT_AI_MASKING
    result->mask = layer.mask;
    result->depth = layer.depth;
    result->depthSelect = layer.depthSelect;
#endif
    result->w = layer.partner->image->getWidth();
    result->h = layer.partner->image->getHeight();
    result->pixels.resize(size_t(result->w) * result->h);
    const float pw = layer.partner->fullWidth, ph = layer.partner->fullHeight;
    const auto read = [&](float u, float v) {
        demotion::Pixel p;
        samplePartner(*layer.partner, u * pw, v * ph, p.r, p.g, p.b);
        p.a = 1;
#ifdef RT_AI_MASKING
        if (layer.mask) p.a = layer.mask->sample(u * pw, v * ph);
        if (layer.depth) p.a *= layer.depthSelect.weight(layer.depth->sample(u * pw, v * ph));
#endif
        return p;
    };
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int y = 0; y < result->h; ++y) for (int x = 0; x < result->w; ++x)
        result->pixels[size_t(y) * result->w + x] = demotion::sample(layer.motion,
            (x + .5f) / result->w, (y + .5f) / result->h, pw / ph, read);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& existing : cache) if (matches(existing)) return existing;
    size_t bytes = result->pixels.size() * sizeof(demotion::Pixel);
    for (const auto& p : cache) bytes += p->pixels.size() * sizeof(demotion::Pixel);
    while (!cache.empty() && bytes > budget) {
        bytes -= cache.front()->pixels.size() * sizeof(demotion::Pixel);
        cache.pop_front();
    }
    if (result->pixels.size() * sizeof(demotion::Pixel) <= budget) cache.push_back(result);
    return result;
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

        auto partner = PartnerImageStore::getInstance().getPartner(layer.source(), workingProfile, fullRes);

        if (partner && partner->image && partner->fullWidth > 0 && partner->fullHeight > 0) {
            ResolvedLayer rl;
            rl.partner = partner;
            rl.motion = demotion::trajectory(layer.motion, partner->image->getWidth(), fullRes ? 512 : 384);
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
            rl.mask = PartnerMaskStore::getInstance().getMask(layer.source(), workingProfile, layer.maskClass,
                                                              layer.maskFeather, layer.maskInvert,
                                                              layer.maskPaint, multiThread);

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

            if (layer.depthEnabled) {
                rl.depth = PartnerDepthStore::getInstance().getDepth(layer.source(), workingProfile);
                rl.depthSelect.range = layer.depthRange;
                rl.depthSelect.nearShape = layer.depthNearShape;
                rl.depthSelect.farShape = layer.depthFarShape;
                rl.depthSelect.invert = layer.depthInvert;
            }
#endif

            if (!fullRes && !rl.motion.empty()) rl.motionPlate = previewMotion(rl, layer.motion);
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
    const bool adaptiveStack = std::any_of(resolved.begin(), resolved.end(), [](const ResolvedLayer& l) {
        return l.mode == procparams::DoubleExposureParams::BlendMode::ADAPTIVE;
    });
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

        // Where each layer landed at this pixel, kept between the two passes:
        // the film gain cannot be known until every layer's coverage is.
        std::vector<deplace::Placed> hits(resolved.size());
        std::vector<demotion::Pixel> motionPixels(resolved.size());

        for (int x = 0; x < W; ++x) {
            const float fx = offX + (x + 0.5f) * skip;

            // Where each layer lands, and how much of it is here. Outside a
            // placed frame the layer is simply absent, with a one-output-pixel
            // anti-aliased border (wider, if its edge is set to blend).
            float framesHere = 0.f;

            for (size_t li = 0; li < resolved.size(); ++li) {
                deplace::Placed& hit = hits[li];
                hit = deplace::place(resolved[li].frame, fx, fy, skip);

                const auto& rl = resolved[li];
                if (hit.present && !rl.motion.empty()) {
                    const float pw = rl.partner->fullWidth, ph = rl.partner->fullHeight;
                    const auto read = [&](float u, float v) {
                        demotion::Pixel p;
                        samplePartner(*rl.partner, u * pw, v * ph, p.r, p.g, p.b);
                        p.a = 1;
#ifdef RT_AI_MASKING
                        if (rl.mask) p.a = rl.mask->sample(u * pw, v * ph);
                        if (rl.depth) p.a *= rl.depthSelect.weight(rl.depth->sample(u * pw, v * ph));
#endif
                        return p;
                    };
                    const auto motionAt = [&](float u, float v) {
                        return rl.motionPlate ? rl.motionPlate->at(u / pw, v / ph)
                            : demotion::sample(rl.motion, u / pw, v / ph, pw / ph, read);
                    };
                    auto p = motionAt(hit.u, hit.v);
                    if (hit.mix > 0) {
                        const auto q = motionAt(hit.u2, hit.v2);
                        p.a *= hit.coverage;
                        auto covered = q; covered.a *= hit.coverage2;
                        p = demotion::mix(p, covered, hit.mix);
                    } else {
                        p.a *= hit.coverage;
                    }
                    motionPixels[li] = p;
                }

                if (hit.present && resolved[li].mode == procparams::DoubleExposureParams::BlendMode::ADD) {
                    framesHere += rl.motion.empty() ? hit.coverage + hit.mix * (hit.coverage2 - hit.coverage)
                                                   : motionPixels[li].a * rl.opacity;
                }
            }

            const float gainFactor = meterFrames ? 1.f / (1.f + framesHere) : 1.f;
            const float baseGain = baseEvGain * gainFactor;
            const float shoulderWhite = white * (adaptiveStack ? 1.f : gainFactor);

            float r = rgb->r(y, x) * baseGain;
            float g = rgb->g(y, x) * baseGain;
            float b = rgb->b(y, x) * baseGain;

            for (size_t li = 0; li < resolved.size(); ++li) {
                const ResolvedLayer& rl = resolved[li];
                const deplace::Placed& hit = hits[li];

                if (!hit.present) {
                    continue;
                }

                const float u = hit.u;
                const float v = hit.v;
                float coverage = hit.coverage;
                const float gain = rl.mode == procparams::DoubleExposureParams::BlendMode::ADD
                                   ? rl.gain * gainFactor : rl.gain;

                float pr, pg, pb;
                if (!rl.motion.empty()) {
                    const auto& p = motionPixels[li];
                    pr = p.r; pg = p.g; pb = p.b; coverage = p.a;
                } else {
                    samplePartner(*rl.partner, u, v, pr, pg, pb);
                }

                if (hit.mix > 0.f && rl.motion.empty()) {
                    // Hand over to the neighbouring radial copy rather than
                    // picking one and leaving a line where they meet.
                    float nr, ng, nb;
                    samplePartner(*rl.partner, hit.u2, hit.v2, nr, ng, nb);
                    pr += hit.mix * (nr - pr);
                    pg += hit.mix * (ng - pg);
                    pb += hit.mix * (nb - pb);
                    coverage += hit.mix * (hit.coverage2 - coverage);
                }

                pr = std::max(pr, 0.f) * gain;
                pg = std::max(pg, 0.f) * gain;
                pb = std::max(pb, 0.f) * gain;

                float cr, cg, cb;
                deblend::blend(rl.mode, rl.compare, rl.softness, white, r, g, b, pr, pg, pb, cr, cg, cb);

                float w = rl.opacity * coverage;

#ifdef RT_AI_MASKING
                if (rl.mask && rl.motion.empty()) {
                    float m = rl.mask->sample(u, v);

                    if (hit.mix > 0.f) {
                        m += hit.mix * (rl.mask->sample(hit.u2, hit.v2) - m);
                    }

                    w *= m;

                    if (w <= 0.f) {
                        continue;
                    }
                }

                if (rl.depth && rl.motion.empty()) {
                    float d = rl.depthSelect.weight(rl.depth->sample(u, v));

                    if (hit.mix > 0.f) {
                        d += hit.mix * (rl.depthSelect.weight(rl.depth->sample(hit.u2, hit.v2)) - d);
                    }

                    w *= d;

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
