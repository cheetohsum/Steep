/* -*- C++ -*-
 *
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

// Relight: one virtual point light placed in the photograph's own space.
//
// The scene is a height field. x and y are the frame, measured in long edges
// so the light behaves the same on any aspect ratio and at any preview size;
// z is the estimated depth (0 near .. 1 far) times kSceneDepth. Normals come
// from the gradient of that depth, so a face turns its cheek away from a
// light at its side and a light behind the subject catches only its outline,
// where the depth falls away to the background.
//
// The light multiplies the existing exposure (it reshapes the light that is
// already in the photograph rather than painting over it), by an amount in
// stops: the key raises what faces it and is near it, the shadow term lowers
// what faces away, and the background term moves everything that is not the
// subject. Applied to XYZ as a single factor, which in Lab is an exact scale
// of (L + 16) and of a and b on the cube-root branch.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "improcfun.h"
#include "imagefloat.h"
#include "labimage.h"
#include "partnerdepthstore.h"
#include "procparams.h"
#include "relight.h"
#include "rt_math.h"

#ifdef RT_AI_MASKING
#include "aimaskcache.h"
#include "aisegmentation.h"
#endif

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

constexpr float kSceneDepth = 0.6f;     // depth range of the scene, in long edges
constexpr float kMaxShadowEv = 1.5f;
constexpr float kMaxBackgroundEv = 2.0f;
constexpr float kWrap = 0.35f;          // soft terminator: light wraps a little past 90 degrees
constexpr float kMaxSlope = 4.0f;       // steepest surface the normals may describe
constexpr float kBehindDepth = 0.2f;    // how far behind the subject the light may go, in long edges
constexpr float kDepthTolerance = 0.05f; // of the depth range: how far behind its core the subject may reach
constexpr float kMaskLow = 0.3f;       // subject probability read as "not the subject"
constexpr float kMaskHigh = 0.7f;      // ... and as "certainly the subject"
constexpr float kSilhouetteBand = 0.012f; // width of the rounded edge, in long edges

float sampleMask(const array2D<float>& mask, int mw, int mh, float u, float v)
{
    // u, v: 0..1 across the frame
    const float px = rtengine::LIM(u * mw - 0.5f, 0.f, float(mw - 1));
    const float py = rtengine::LIM(v * mh - 0.5f, 0.f, float(mh - 1));
    const int x0 = int(px);
    const int y0 = int(py);
    const int x1 = std::min(x0 + 1, mw - 1);
    const int y1 = std::min(y0 + 1, mh - 1);
    const float fx = px - x0;
    const float fy = py - y0;
    const float top = mask[y0][x0] + fx * (mask[y0][x1] - mask[y0][x0]);
    const float bottom = mask[y1][x0] + fx * (mask[y1][x1] - mask[y1][x0]);
    // The model's edge ramps over a dozen mask pixels; only its steep
    // middle is the outline, the tails would light a halo.
    const float t = rtengine::LIM01((top + fy * (bottom - top) - kMaskLow) / (kMaskHigh - kMaskLow));
    return t * t * (3.f - 2.f * t);
}

// The scene, resampled onto one coarse grid over the whole frame: depth,
// normal and subject weight. Everything slow happens here, once per call, at
// ~half a megapixel at most; the per-pixel pass only interpolates.
struct SceneGrid {
    int w = 0;
    int h = 0;
    std::vector<float> depth;
    std::vector<float> nx, ny, nz;
    std::vector<float> subject;

    float at(const std::vector<float>& plane, float gx, float gy) const
    {
        gx = rtengine::LIM(gx, 0.f, float(w - 1));
        gy = rtengine::LIM(gy, 0.f, float(h - 1));
        const int x0 = int(gx);
        const int y0 = int(gy);
        const int x1 = std::min(x0 + 1, w - 1);
        const int y1 = std::min(y0 + 1, h - 1);
        const float fx = gx - x0;
        const float fy = gy - y0;
        const float top = plane[y0 * w + x0] + fx * (plane[y0 * w + x1] - plane[y0 * w + x0]);
        const float bottom = plane[y1 * w + x0] + fx * (plane[y1 * w + x1] - plane[y1 * w + x0]);
        return top + fy * (bottom - top);
    }
};

void boxBlur(std::vector<float>& plane, int w, int h, int radius)
{
    if (radius < 1 || w < 2 || h < 2) {
        return;
    }

    std::vector<float> tmp(plane.size());

    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            float sum = 0.f;
            int n = 0;

            for (int x = -radius; x <= radius; ++x) {
                if (x >= 0 && x < w) {
                    sum += plane[y * w + x];
                    ++n;
                }
            }

            for (int x = 0; x < w; ++x) {
                tmp[y * w + x] = sum / n;
                const int out = x - radius;
                const int in = x + radius + 1;

                if (out >= 0) {
                    sum -= plane[y * w + out];
                    --n;
                }

                if (in < w) {
                    sum += plane[y * w + in];
                    ++n;
                }
            }
        }

        for (int x = 0; x < w; ++x) {
            float sum = 0.f;
            int n = 0;

            for (int y = -radius; y <= radius; ++y) {
                if (y >= 0 && y < h) {
                    sum += tmp[y * w + x];
                    ++n;
                }
            }

            for (int y = 0; y < h; ++y) {
                plane[y * w + x] = sum / n;
                const int out = y - radius;
                const int in = y + radius + 1;

                if (out >= 0) {
                    sum -= tmp[out * w + x];
                    --n;
                }

                if (in < h) {
                    sum += tmp[in * w + x];
                    ++n;
                }
            }
        }
    }
}

bool sameAspect(int w1, int h1, int w2, int h2)
{
    if (w1 <= 0 || h1 <= 0 || w2 <= 0 || h2 <= 0) {
        return false;
    }

    return std::fabs((double(w1) / h1) / (double(w2) / h2) - 1.0) < 0.01;
}

float smoothstep(float e0, float e1, float x)
{
    const float t = rtengine::LIM01((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

} // namespace

namespace rtengine
{

void prepareRelightInputs(const RelightParams& params, const std::string& imageId,
                          const Imagefloat& source, int fullWidth, int fullHeight,
                          const Glib::ustring& workingProfile, bool reuseCached)
{
    if (!params.enabled || imageId.empty() || source.getWidth() < 8 || source.getHeight() < 8) {
        return;
    }

#ifdef RT_AI_MASKING
    if (getAISegmentationEngine().isInitialized()
            && !(reuseCached && AIMaskCache::getInstance().hasCachedMasks(imageId))) {
        // A no-op when this rendering of the image is already cached.
        AIMaskCache::getInstance().computeMasks(
            imageId, source.r.ptrs, source.g.ptrs, source.b.ptrs,
            source.getWidth(), source.getHeight(), fullWidth, fullHeight,
            workingProfile.raw(), true);
    }
#endif

    PartnerDepthStore::getInstance().getSceneDepth(imageId, source, fullWidth, fullHeight, workingProfile);
}

void ImProcFunctions::relight(LabImage* lab, const RelightParams& params, const RelightFrame& frame)
{
    if (!params.enabled || !lab || lab->W < 1 || lab->H < 1 || frame.fullWidth < 1 || frame.fullHeight < 1) {
        return;
    }

    const int W = lab->W;
    const int H = lab->H;
    const float fullW = frame.fullWidth;
    const float fullH = frame.fullHeight;
    const float longEdge = std::max(fullW, fullH);

    // ---- inputs, whatever of them exist for this frame
    const auto depthMap = PartnerDepthStore::getInstance().peekSceneDepth(
        frame.imageId, frame.fullWidth, frame.fullHeight, frame.workingProfile);

    std::shared_ptr<const array2D<float>> subjectMask;
    int maskW = 0;
    int maskH = 0;
#ifdef RT_AI_MASKING
    if (AIMaskCache::getInstance().hasCachedMasks(frame.imageId)) {
        // The model's own probabilities, not the guided-refined local
        // adjustment mask: refinement at a fixed threshold pulls in patches
        // of background the raw output only half believes in. sampleMask
        // narrows the soft edge instead.
        const AIMaskSnapshot snap = AIMaskCache::getInstance().getMaskSnapshot(AISegClass::SUBJECT);

        if (snap && sameAspect(snap.fullWidth, snap.fullHeight, frame.fullWidth, frame.fullHeight)) {
            subjectMask = snap.mask;
            maskW = snap.width;
            maskH = snap.height;
        }
    }
#endif

    // ---- the scene grid
    SceneGrid grid;

    if (depthMap && depthMap->valid()) {
        grid.w = depthMap->width;
        grid.h = depthMap->height;
    } else {
        const float s = 256.f / longEdge;
        grid.w = std::max(2, int(std::lround(fullW * s)));
        grid.h = std::max(2, int(std::lround(fullH * s)));
    }

    const int G = grid.w * grid.h;
    grid.depth.assign(G, 0.5f);
    grid.subject.assign(G, 1.f);

    if (depthMap && depthMap->valid()) {
        // Invalid samples take the mean of the valid ones: a hole is better
        // flat than a cliff the normals would light as a rim.
        double sum = 0.0;
        int n = 0;

        for (int y = 0; y < grid.h; ++y) {
            for (int x = 0; x < grid.w; ++x) {
                const float d = depthMap->depth[y][x];

                if (d >= 0.f) {
                    sum += d;
                    ++n;
                }
            }
        }

        const float fill = n ? float(sum / n) : 0.5f;

        for (int y = 0; y < grid.h; ++y) {
            for (int x = 0; x < grid.w; ++x) {
                const float d = depthMap->depth[y][x];
                grid.depth[y * grid.w + x] = d >= 0.f ? d : fill;
            }
        }
    }

    // The depth estimator resolves texture as relief; the light should see
    // forms. Smoothed in proportion to the grid so previews and exports agree.
    boxBlur(grid.depth, grid.w, grid.h, std::max(1, int(std::lround(std::max(grid.w, grid.h) / 170.f))));

    if (subjectMask) {
        for (int y = 0; y < grid.h; ++y) {
            for (int x = 0; x < grid.w; ++x) {
                grid.subject[y * grid.w + x] = sampleMask(*subjectMask, maskW, maskH,
                                                          (x + 0.5f) / grid.w, (y + 0.5f) / grid.h);
            }
        }
    } else if (depthMap && depthMap->valid()) {
        // No subject model: the near part of the scene stands in for it.
        for (int i = 0; i < G; ++i) {
            grid.subject[i] = 1.f - smoothstep(0.35f, 0.65f, grid.depth[i]);
        }
    }

    const bool haveSubject = subjectMask || (depthMap && depthMap->valid());

    // The subject model is unsure of some background -- a patch of wall can
    // come back half believed. The depth map settles it: nothing well behind
    // the subject's confident core belongs to the subject.
    float coreDepth = -1.f;

    if (subjectMask && depthMap && depthMap->valid()) {
        double sum = 0.0;
        double weight = 0.0;

        for (int i = 0; i < G; ++i) {
            if (grid.subject[i] > 0.9f) {
                sum += grid.depth[i];
                weight += 1.0;
            }
        }

        if (weight > G * 0.002) {
            coreDepth = float(sum / weight);

            for (int i = 0; i < G; ++i) {
                grid.subject[i] *= 1.f - smoothstep(coreDepth + kDepthTolerance, coreDepth + 3.f * kDepthTolerance, grid.depth[i]);
            }
        }
    }

    // Normals: n ~ (dz/du, dz/dv, -1), facing the camera (-z).
    const float relief = params.shape / 100.f * 2.5f;
    grid.nx.assign(G, 0.f);
    grid.ny.assign(G, 0.f);
    grid.nz.assign(G, -1.f);

    // The subject's outline as a rounded edge: a blurred copy of its mask,
    // read as height, turns the rim of the subject sideways the way a real
    // silhouette is -- which is where a light from behind catches it. The
    // depth map alone is too soft at the outline to do that.
    std::vector<float> rounded;

    if (subjectMask && relief > 0.f) {
        rounded = grid.subject;
        boxBlur(rounded, grid.w, grid.h, std::max(1, int(std::lround(kSilhouetteBand * std::max(grid.w, grid.h)))));
    }

    if (relief > 0.f && ((depthMap && depthMap->valid()) || !rounded.empty())) {
        // One grid cell in long-edge units.
        const float cell = 1.f / std::max(grid.w, grid.h);
        const bool useDepth = depthMap && depthMap->valid();
        const float roundness = 6.f * kSilhouetteBand * relief;

        for (int y = 0; y < grid.h; ++y) {
            for (int x = 0; x < grid.w; ++x) {
                const int xl = std::max(0, x - 1), xr = std::min(grid.w - 1, x + 1);
                const int yu = std::max(0, y - 1), yd = std::min(grid.h - 1, y + 1);
                float gx = 0.f;
                float gy = 0.f;

                if (useDepth) {
                    gx = (grid.depth[y * grid.w + xr] - grid.depth[y * grid.w + xl]) * kSceneDepth / ((xr - xl) * cell) * relief;
                    gy = (grid.depth[yd * grid.w + x] - grid.depth[yu * grid.w + x]) * kSceneDepth / ((yd - yu) * cell) * relief;
                }

                if (!rounded.empty()) {
                    // Height falls away outwards, inside the subject only.
                    const float inside = grid.subject[y * grid.w + x];
                    gx -= inside * roundness * (rounded[y * grid.w + xr] - rounded[y * grid.w + xl]) / ((xr - xl) * cell);
                    gy -= inside * roundness * (rounded[yd * grid.w + x] - rounded[yu * grid.w + x]) / ((yd - yu) * cell);
                }
                const float slope = std::sqrt(gx * gx + gy * gy);

                if (slope > kMaxSlope) {
                    gx *= kMaxSlope / slope;
                    gy *= kMaxSlope / slope;
                }

                const float inv = 1.f / std::sqrt(gx * gx + gy * gy + 1.f);
                grid.nx[y * grid.w + x] = gx * inv;
                grid.ny[y * grid.w + x] = gy * inv;
                grid.nz[y * grid.w + x] = -inv;
            }
        }
    }

    // ---- the light
    const float lightU = (0.5f + params.lightX / 200.f) * fullW / longEdge;
    const float lightV = (0.5f + params.lightY / 200.f) * fullH / longEdge;

    // Its depth is relative to the subject's, so "in front of" and "behind"
    // mean the same thing on a portrait and on a landscape.
    float subjectDepth = 0.f;
    {
        double sum = 0.0;
        double weight = 0.0;

        for (int i = 0; i < G; ++i) {
            const float s = grid.subject[i];

            if (s > 0.5f) {
                sum += double(s) * grid.depth[i];
                weight += s;
            }
        }

        if (weight > G * 0.002) {
            subjectDepth = float(sum / weight);
        } else {
            subjectDepth = grid.at(grid.depth, lightU * longEdge / fullW * grid.w - 0.5f,
                                   lightV * longEdge / fullH * grid.h - 0.5f);
        }
    }

    // In front it may come all the way to the camera; behind, only as far as
    // still lets it graze the outline -- deep behind, a silhouette is all
    // edge-on to it and nothing would catch the light.
    const float lightZ = subjectDepth * kSceneDepth
                         + (params.distance < 0 ? params.distance / 100.f * kSceneDepth
                                                : params.distance / 100.f * kBehindDepth);
    const float reach = 0.1f + params.reach / 100.f * 1.4f;
    const float invReach2 = 1.f / (reach * reach);
    // Brightness is stored in fiftieths of a stop: 50 = +1 EV, 200 = +4 EV.
    const float keyEv = params.intensity / 50.f;
    const float shadowEv = params.shadows / 100.f * kMaxShadowEv;
    const float backgroundEv = haveSubject ? params.background / 100.f * kMaxBackgroundEv : 0.f;
    const float focus = params.subjectFocus / 100.f;
    const float warmth = params.warmth / 100.f;

    const float gridScaleX = grid.w / fullW;
    const float gridScaleY = grid.h / fullH;
    constexpr float L16 = 16.f * 327.68f;
    constexpr float kKnee = 0.75f;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16) if (multiThread)
#endif
    for (int y = 0; y < H; ++y) {
        const float fyFull = frame.originY + (y + 0.5) * frame.step;
        const float gy = fyFull * gridScaleY - 0.5f;
        const float v = fyFull / longEdge;

        for (int x = 0; x < W; ++x) {
            const float fxFull = frame.originX + (x + 0.5) * frame.step;
            const float gx = fxFull * gridScaleX - 0.5f;
            const float u = fxFull / longEdge;

            const float z = grid.at(grid.depth, gx, gy) * kSceneDepth;
            // The subject at the mask's own resolution: the grid is coarser
            // than the outline needs to be.
            float s = grid.at(grid.subject, gx, gy);

            if (subjectMask) {
                s = sampleMask(*subjectMask, maskW, maskH, fxFull / fullW, fyFull / fullH);

                if (coreDepth >= 0.f) {
                    s *= 1.f - smoothstep(coreDepth + kDepthTolerance, coreDepth + 3.f * kDepthTolerance, z / kSceneDepth);
                }
            }
            float nx = grid.at(grid.nx, gx, gy);
            float ny = grid.at(grid.ny, gx, gy);
            float nz = grid.at(grid.nz, gx, gy);
            const float nInv = 1.f / std::max(1e-6f, std::sqrt(nx * nx + ny * ny + nz * nz));
            nx *= nInv;
            ny *= nInv;
            nz *= nInv;

            const float lx = lightU - u;
            const float ly = lightV - v;
            const float lz = lightZ - z;
            const float dist2 = lx * lx + ly * ly + lz * lz + 1e-4f;
            const float dInv = 1.f / std::sqrt(dist2);
            const float ndotl = (nx * lx + ny * ly + nz * lz) * dInv;
            const float diffuse = LIM01((ndotl + kWrap) / (1.f + kWrap));
            const float atten = 1.f / (1.f + dist2 * invReach2);

            const float lit = 1.f - focus * (1.f - s);
            const float keyPart = keyEv * diffuse * atten * lit;
            const float ev = keyPart
                             - shadowEv * (1.f - diffuse) * lit
                             + backgroundEv * (1.f - s);

            if (std::fabs(ev) < 1e-4f) {
                continue;
            }

            const float L = lab->L[y][x];
            const float fy = (L + L16) / (116.f * 327.68f);

            if (fy <= 0.f) {
                continue;
            }

            const float Y = fy * fy * fy;
            float Yn = Y * std::exp2(ev);

            // Brightening rolls into white instead of clipping.
            if (ev > 0.f && Yn > kKnee) {
                const float soft = kKnee + (1.f - kKnee) * (1.f - std::exp(-(Yn - kKnee) / (1.f - kKnee)));
                Yn = std::max(Y, soft);
            }

            const float r = std::cbrt(Yn / Y);
            const float Lnew = (L + L16) * r - L16;
            float a = lab->a[y][x] * r;
            float b = lab->b[y][x] * r;

            // The light's own colour, in proportion to what it added.
            if (warmth != 0.f && keyPart > 0.f) {
                const float added = std::max(0.f, Lnew - L) * (keyPart / std::max(keyPart, ev > 0.f ? ev : keyPart));
                a += warmth * 0.10f * added;
                b += warmth * 0.30f * added;
            }

            lab->L[y][x] = Lnew;
            lab->a[y][x] = a;
            lab->b[y][x] = b;
        }
    }
}

} // namespace rtengine
