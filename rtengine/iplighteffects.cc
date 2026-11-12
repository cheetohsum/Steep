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

#include <algorithm>
#include <cmath>

#include "array2D.h"
#include "boxblur.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "rt_math.h"

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

// L in a LabImage runs 0..32768.
constexpr float L_SCALE = 32768.f;
// a/b are stored scaled by this: one CIELab unit is 327.68 here. Writing
// plain Lab-sized numbers into them shifts the colour by hundredths of a
// unit, which is why the halation fringe first came out colourless.
constexpr float AB_PER_LAB_UNIT = 327.68f;

float smoothStep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) {
        return x < edge0 ? 0.f : 1.f;
    }

    const float t = rtengine::LIM((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Three successive box blurs approximate a Gaussian of the same sigma. One box
// leaves square halos, and a halo around a point highlight is the single place
// that shows most plainly. (Same approach the film halation stage uses.)
void tripleBoxBlur(array2D<float>& source, array2D<float>& destination,
                   array2D<float>& scratch, int radius, int width, int height,
                   bool multiThread)
{
    const int pass = std::max(1, static_cast<int>(radius * 0.577f + 0.5f));
    boxblur(static_cast<float**>(source), static_cast<float**>(destination), pass, width, height, multiThread);
    boxblur(static_cast<float**>(destination), static_cast<float**>(scratch), pass, width, height, multiThread);
    boxblur(static_cast<float**>(scratch), static_cast<float**>(destination), pass, width, height, multiThread);
}

// How much light a pixel throws into its surroundings. Keyed to the top of the
// range the pipeline actually delivers: by this stage the tone curve has
// already folded the scene into a bounded signal, so the scene's real specular
// magnitude is gone. Intensity is read from how completely a pixel is blown,
// which is why a light source blooms and a white shirt barely does.
float highlightSource(float luminance01, float onset)
{
    const float blown = smoothStep(onset, 1.f, luminance01);

    // Weight the most blown pixels harder so a light source throws more than
    // a bright wall — but gently. Squaring this and keying it near 1.0, as a
    // first cut did, left the mask at hundredths even for the brightest pixel
    // the pipeline delivers, and the effect was invisible at every setting.
    return blown * (0.5f + 0.5f * smoothStep(0.80f, 1.f, luminance01));
}

// Streak kernel: walk out along +/- the flare axis, accumulating the mask with
// a linear falloff. Separable in the sense that it is a single 1-D pass, so
// cost stays close to a blur rather than a full 2-D convolution.
void directionalStreak(const array2D<float>& source, array2D<float>& destination,
                       int width, int height, float angleDeg, int reach,
                       bool multiThread)
{
    const float rad = angleDeg * static_cast<float>(rtengine::RT_PI) / 180.f;
    const float dx = std::cos(rad);
    const float dy = std::sin(rad);
    const int steps = std::max(1, reach);

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0.f;
            float weight = 0.f;

            for (int s = -steps; s <= steps; ++s) {
                const float w = 1.f - std::fabs(static_cast<float>(s)) / (steps + 1.f);
                const int sx = static_cast<int>(x + dx * s + 0.5f);
                const int sy = static_cast<int>(y + dy * s + 0.5f);

                if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
                    sum += source[sy][sx] * w;
                }

                weight += w;
            }

            destination[y][x] = weight > 0.f ? sum / weight : 0.f;
        }
    }
}

// CIE L* (0..100) <-> relative luminance Y (0..1).
inline float lstarToY(float l)
{
    if (l > 8.f) {
        const float f = (l + 16.f) / 116.f;
        return f * f * f;
    }

    return l / 903.3f;
}

inline float yToLstar(float y)
{
    return y > 0.008856f ? 116.f * std::cbrt(y) - 16.f : 903.3f * y;
}

// Film Lab draws its own halation off the negative. Stacking ours on top
// doubles the ring, so ours stands down while Film Lab's is live. Every stock
// carries some base halation, so only a fully trimmed slider counts as off.
bool filmLabHalationActive(const FilmPresetsParams* film)
{
    return film && film->enabled && film->strength > 0 && film->halation > -100;
}

}

void ImProcFunctions::lightEffects(LabImage* lab, const LightEffectsParams& params, int fw, int fh, double scaleFactor,
                                   const FilmPresetsParams* film)
{
    if (!params.enabled || !lab) {
        return;
    }

    const bool wantHalation = params.halation > 0 && !filmLabHalationActive(film);

    if (params.glow <= 0 && !wantHalation && params.flare <= 0) {
        return;
    }

    const int width = lab->W;
    const int height = lab->H;

    if (width < 8 || height < 8) {
        return;
    }

    const float scale = static_cast<float>(std::max(scaleFactor, 1.0));

    // The mask and every blur run downsampled: these are large-radius, low
    // frequency effects, so full-resolution work would be paid for nothing.
    const int reduction = scale <= 1.f ? 4 : (scale <= 2.f ? 2 : 1);
    const int mw = std::max(4, width / reduction);
    const int mh = std::max(4, height / reduction);

    // Onset spans the upper half of the range: by this stage the tone curve has
    // already folded the scene into a bounded signal, so keying it just below
    // clipping would mean nothing qualifies. Quadratic so threshold 0 still
    // only keys bright tones (a linear map from 0.25 lit midtones) while the
    // default of 70 lands where it always did (~0.71).
    const float thresholdUnit = rtengine::LIM(params.threshold / 100.f, 0.f, 1.f);
    const float onset = 0.55f + 0.37f * thresholdUnit * thresholdUnit;

    array2D<float> mask(mw, mh);

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int y = 0; y < mh; ++y) {
        const int y0 = y * height / mh;
        const int y1 = std::min(height, std::max(y0 + 1, (y + 1) * height / mh));

        for (int x = 0; x < mw; ++x) {
            const int x0 = x * width / mw;
            const int x1 = std::min(width, std::max(x0 + 1, (x + 1) * width / mw));

            float acc = 0.f;
            int n = 0;

            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    acc += highlightSource(rtengine::LIM(lab->L[yy][xx] / L_SCALE, 0.f, 1.f), onset);
                    ++n;
                }
            }

            mask[y][x] = n > 0 ? acc / n : 0.f;
        }
    }

    // Radii are a share of the FULL FRAME's short edge, expressed in mask
    // cells (buffer pixels / reduction). Taking the buffer's own edge made a
    // 1:1 tile, which is a small piece of the frame, draw small effects.
    const float fullShort = (fw > 0 && fh > 0)
        ? static_cast<float>(std::min(fw, fh))
        : static_cast<float>(std::min(width, height)) * scale;
    const float shortEdge = fullShort / (scale * reduction);

    array2D<float> blurA(mw, mh);
    array2D<float> scratch(mw, mh);

    array2D<float> glowLayer(mw, mh);
    array2D<float> ringLayer(mw, mh);
    array2D<float> streakLayer(mw, mh);

    const bool wantGlow = params.glow > 0;
    const bool wantFlare = params.flare > 0;

    if (wantGlow) {
        const int radius = std::max(1, static_cast<int>(shortEdge * (0.02f + 0.16f * params.glowRadius / 100.f)));
        tripleBoxBlur(mask, blurA, scratch, radius, mw, mh, multiThread);

        for (int y = 0; y < mh; ++y) {
            for (int x = 0; x < mw; ++x) {
                glowLayer[y][x] = blurA[y][x];
            }
        }
    }

    if (wantHalation) {
        // Two radii, then subtract the source footprint. Without that
        // subtraction this is a milky global veil rather than a fringe
        // hugging the edge of the highlight — the whole character of the
        // effect lives in this step.
        const int inner = std::max(1, static_cast<int>(shortEdge * (0.008f + 0.045f * params.halationSize / 100.f)));
        const int outer = std::max(inner + 1, static_cast<int>(inner * 3.4f));

        tripleBoxBlur(mask, blurA, scratch, inner, mw, mh, multiThread);

        for (int y = 0; y < mh; ++y) {
            for (int x = 0; x < mw; ++x) {
                ringLayer[y][x] = std::max(blurA[y][x] - mask[y][x] * 0.85f, 0.f) * 0.72f;
            }
        }

        tripleBoxBlur(mask, blurA, scratch, outer, mw, mh, multiThread);

        for (int y = 0; y < mh; ++y) {
            for (int x = 0; x < mw; ++x) {
                ringLayer[y][x] += std::max(blurA[y][x] - mask[y][x] * 0.30f, 0.f) * 0.28f;
            }
        }
    }

    if (wantFlare) {
        const int reach = std::max(2, static_cast<int>(shortEdge * (0.03f + 0.35f * params.flareLength / 100.f)));
        directionalStreak(mask, streakLayer, mw, mh, static_cast<float>(params.flareAngle), reach, multiThread);
    }

    // A wide blur spreads the mask's energy over its area, so the peak value
    // arriving here is far below the mask's own peak. These multipliers put a
    // slider at 100 into "obvious but not destroyed" territory.
    const float glowAmount = params.glow / 100.f * 1.30f;
    const float halationAmount = params.halation / 100.f * 2.40f;
    const float flareAmount = params.flare / 100.f * 1.60f;

    // Halation's dye colour: neutral through to the red-orange of light
    // bouncing off the film base. The tint is carried by the light the ring
    // actually adds (its share of the L* increase), saturating at the old
    // full strength once a ring adds ~10 L*: a flat offset keyed off the
    // ring's mere presence turned dark pixels beside a ring orange even
    // where almost no light landed on them.
    const float warmth = params.halationWarmth / 100.f;
    const float ringA = 5.0f * AB_PER_LAB_UNIT * warmth;   // toward red
    const float ringB = 3.0f * AB_PER_LAB_UNIT * warmth;   // toward yellow
    constexpr float RING_TINT_FULL_L = 10.f;

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) if (multiThread)
#endif
    for (int y = 0; y < height; ++y) {
        const int my = std::min(mh - 1, y * mh / height);

        for (int x = 0; x < width; ++x) {
            const int mx = std::min(mw - 1, x * mw / width);

            float lift = 0.f;

            if (wantGlow) {
                lift += glowLayer[my][mx] * glowAmount;
            }

            if (wantFlare) {
                lift += streakLayer[my][mx] * flareAmount;
            }

            float ring = 0.f;

            if (wantHalation) {
                ring = ringLayer[my][mx] * halationAmount;
                lift += ring;
            }

            if (lift <= 0.f) {
                continue;
            }

            // Add the light in linear luminance, screened so highlights at the
            // ceiling do not simply clip harder. The lift keeps its old
            // meaning on black (the L* it raises a black pixel to), but over
            // midtones it no longer lays a milky L*-space veil.
            const float l = lab->L[y][x];
            const float lStar = rtengine::LIM(l / L_SCALE, 0.f, 1.f) * 100.f;
            const float yBase = lstarToY(lStar);
            const float yAdd = lstarToY(rtengine::LIM(lift, 0.f, 1.f) * 100.f);
            const float yOut = yBase + (1.f - yBase) * yAdd;
            const float lOut = yToLstar(yOut) * (L_SCALE / 100.f);
            const float added = std::max(lOut - l, 0.f);
            lab->L[y][x] = std::max(l, lOut);

            if (ring > 0.f) {
                const float ringAddedL = added * (ring / lift) * (100.f / L_SCALE);
                const float tint = rtengine::LIM(ringAddedL / RING_TINT_FULL_L, 0.f, 1.f);
                lab->a[y][x] += ringA * tint;
                lab->b[y][x] += ringB * tint;
            }
        }
    }
}
