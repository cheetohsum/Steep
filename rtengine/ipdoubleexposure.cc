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
#include "effectperf.h"
#include "iccstore.h"
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
    int w = 0;
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

// A motion plate integrates the shutter once per plate pixel, and each pixel
// takes two reads per plate pixel of travel, so a long shutter costs the cube
// of the plate's width. The plate is as sharp as the view needs (`wantLong`,
// long edge), then shrinks until it fits `budget` reads -- never below
// `floorLong` unless the view asks for less. A real 0.94 s pan measured ~1900
// reads per pixel when the path was read at partner width instead.
void plateSize(const PartnerImage& partner, const procparams::DEMotionParams& params, int wantLong,
               double budget, int floorLong, int capLong, int& w, int& h)
{
    const int srcW = partner.image->getWidth();
    const int srcH = partner.image->getHeight();
    const int srcLong = std::max(srcW, srcH);
    const int want = std::max(64, std::min({wantLong, capLong, srcLong}));
    // Reads per pixel ~ 2 per pixel of travel plus the half-filled segment
    // ends (sample() reads at least once per interval).
    const double readsPerWidth = 2.6 * std::max(1e-3f, demotion::pathLength(params));
    const double budgetW = std::cbrt(budget / (readsPerWidth * double(srcH) / srcW));
    const double scale = std::min(double(want), std::max(budgetW * srcLong / srcW, double(std::min(want, floorLong)))) / srcLong;
    w = std::max(1, int(std::lround(srcW * scale)));
    h = std::max(1, int(std::lround(srcH * scale)));
}

std::shared_ptr<MotionPlate> renderMotion(const ResolvedLayer& layer, const procparams::DEMotionParams& params, int plateW, int plateH)
{
    EffectPerfScope timing("de-motion-plate", plateW, plateH);
    auto result = std::make_shared<MotionPlate>();
    result->source = layer.partner;
    result->params = params;
#ifdef RT_AI_MASKING
    result->mask = layer.mask;
    result->depth = layer.depth;
    result->depthSelect = layer.depthSelect;
#endif
    result->w = plateW;
    result->h = plateH;
    result->pixels.resize(size_t(plateW) * plateH);
    const float pw = layer.partner->fullWidth, ph = layer.partner->fullHeight;

    // The path is stepped at plate resolution, so the source is first
    // box-filtered down to it: a one-pixel highlight in the partner then
    // still lands on every step instead of between them.
    demotion::PremultipliedBuffer src;
    src.w = plateW;
    src.h = plateH;
    src.pixels.resize(size_t(plateW) * plateH);
    const int nx = std::max(1, int(std::ceil(double(layer.partner->image->getWidth()) / plateW)));
    const int ny = std::max(1, int(std::ceil(double(layer.partner->image->getHeight()) / plateH)));
    const float norm = 1.f / (nx * ny);
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 8)
#endif
    for (int y = 0; y < plateH; ++y) for (int x = 0; x < plateW; ++x) {
        demotion::Pixel sum;
        for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
            const float u = (x + (i + .5f) / nx) / plateW * pw, v = (y + (j + .5f) / ny) / plateH * ph;
            float r, g, b, a = 1;
            samplePartner(*layer.partner, u, v, r, g, b);
#ifdef RT_AI_MASKING
            if (layer.mask) a = layer.mask->sample(u, v);
            if (layer.depth) a *= layer.depthSelect.weight(layer.depth->sample(u, v));
#endif
            sum.r += r * a;
            sum.g += g * a;
            sum.b += b * a;
            sum.a += a;
        }
        src.pixels[size_t(y) * plateW + x] = {sum.r * norm, sum.g * norm, sum.b * norm, sum.a * norm};
    }

    const auto path = demotion::trajectory(params, plateW, 384);
    const auto read = [&](float u, float v) { return src.at(u, v); };
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int y = 0; y < plateH; ++y) for (int x = 0; x < plateW; ++x)
        result->pixels[size_t(y) * plateW + x] = demotion::sample<true>(path,
            (x + .5f) / plateW, (y + .5f) / plateH, pw / ph, read);
    return result;
}

// Interactive plates are retained; export renders its own from the full tier
// (renderMotion) and lets it go with the image. Preview, detail windows and
// thumbnails ask for different sizes: any cached plate from 85% to twice the
// wanted width serves, so a small zoom change or a thumbnail beside the editor
// does not rebuild, and a request only waits behind a build of its own size.
std::shared_ptr<const MotionPlate> previewMotion(const ResolvedLayer& layer, const procparams::DEMotionParams& params, int wantLong)
{
    int plateW, plateH;
    plateSize(*layer.partner, params, wantLong, 1.0e9, 512, 1 << 20, plateW, plateH);
    static std::mutex mutex;
    static std::list<std::shared_ptr<const MotionPlate>> cache;
    static std::list<MotionGate> gates;
    std::shared_ptr<std::mutex> gate;
    constexpr size_t budget = 256U * 1024U * 1024U;
    const auto usable = [&](const MotionPlate& p) {
        return p.source == layer.partner && p.params == params && p.w * 20 >= plateW * 17 && p.w <= 2 * plateW
#ifdef RT_AI_MASKING
            && p.mask == layer.mask && p.depth == layer.depth && p.depthSelect == layer.depthSelect
#endif
            ;
    };
    const auto find = [&]() {
        std::shared_ptr<const MotionPlate> best;
        for (const auto& p : cache) if (usable(*p) && (!best || std::abs(p->w - plateW) < std::abs(best->w - plateW))) best = p;
        return best;
    };
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto hit = find()) {
            cache.remove(hit);
            cache.push_back(hit);
            return hit;
        }
        for (auto i = gates.begin(); i != gates.end();) {
            if (i->mutex.expired()) { i = gates.erase(i); continue; }
            if (i->source == layer.partner.get() && i->params == params && i->w == plateW
#ifdef RT_AI_MASKING
                && i->mask == layer.mask.get() && i->depth == layer.depth.get() && i->depthSelect == layer.depthSelect
#endif
            ) gate = i->mutex.lock();
            ++i;
        }
        if (!gate) {
            gate = std::make_shared<std::mutex>();
            MotionGate key;
            key.source = layer.partner.get(); key.params = params; key.w = plateW; key.mutex = gate;
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
        if (const auto hit = find()) return hit;
    }
    std::shared_ptr<const MotionPlate> result = renderMotion(layer, params, plateW, plateH);
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto hit = find()) return hit;
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

    EffectPerfScope timing("double-exposure", W, H);
    std::vector<ResolvedLayer> resolved;
    resolved.reserve(deParams.layers.size());

    for (const auto& layer : deParams.layers) {
        if (!layer.enabled) {
            continue;
        }

        std::shared_ptr<PartnerImage> partner;
        {
            EffectPerfScope partnerTiming("de-partner");
            partner = PartnerImageStore::getInstance().getPartner(layer.source(), workingProfile, fullRes);
        }

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

            fr.invCover = 1.f / std::max(fr.baseW / fr.srcW, fr.baseH / fr.srcH);
            deplace::applyLayer(fr, layer, cropped);

            if (!rl.motion.empty()) {
                // One plate pixel per output pixel: the partner's long edge
                // at the size this view shows it (a detail window at 1:1 asks
                // for everything the tier holds, a thumbnail for its own
                // width). Export renders from the full tier and keeps the
                // plate only for this image.
                const float shown = std::max(partner->fullWidth, partner->fullHeight) * fr.scale /
                                    (std::max(skip, 1e-3f) * fr.invCover);
                const int wantLong = static_cast<int>(std::ceil(std::min(1.0e6f, shown)));
                if (fullRes) {
                    int plateW, plateH;
                    plateSize(*partner, layer.motion, wantLong, 6.0e9, 2048, 4096, plateW, plateH);
                    rl.motionPlate = renderMotion(rl, layer.motion, plateW, plateH);
                } else {
                    rl.motionPlate = previewMotion(rl, layer.motion, wantLong);
                }
            }

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
    // Comparisons and gates weigh brightness in the space being composited.
    const deblend::LumWeights lum = deblend::lumWeights(ICCStore::getInstance()->workingSpaceMatrix(workingProfile));

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
                deblend::blend(rl.mode, rl.compare, rl.softness, white, lum, r, g, b, pr, pg, pb, cr, cg, cb);

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
                    const float gateLum = (rl.gateOnLayer ? lum(pr, pg, pb) : lum(r, g, b)) / white;
                    w *= deblend::gateWeight(rl.gateStrength,
                                             deblend::gateWindow(deblend::gateEncode(gateLum), rl.gateLow, rl.gateHigh, rl.gateFeather));
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
