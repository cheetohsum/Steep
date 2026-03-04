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

#include "improcfun.h"
#include "gauss.h"
#include "labimage.h"
#include "procparams.h"

#include <cmath>
#include <algorithm>

using namespace rtengine;
using namespace rtengine::procparams;

void ImProcFunctions::lensBlur(LabImage *lab, const LensBlurParams &params, double scale)
{
    if (!params.enabled || params.amount <= 0.0) {
        return;
    }

    const int w = lab->W;
    const int h = lab->H;
    const double amt = params.amount / 100.0;
    // Base sigma from amount, scaled for preview
    const double baseSigma = (params.amount * 2.0) / std::max(scale, 1.0);

    if (baseSigma < 0.3) {
        return;
    }

    // Allocate temporary buffer for blurred L channel
    float **blurred = new float*[h];
    for (int y = 0; y < h; y++) {
        blurred[y] = new float[w];
    }

    // Gaussian blur on L channel
    gaussianBlur(lab->L, blurred, w, h, baseSigma);

    const float cx = w / 2.0f;
    const float cy = h / 2.0f;
    const float maxDist = std::sqrt(cx * cx + cy * cy);

    // Focus range parameters
    const float focusLum = params.depth / 100.0f;
    const float transRange = std::max(params.range / 100.0f, 0.01f);
    const double cateyeAmt = params.cateye / 100.0;
    const double bokehAmt = params.bokeh / 100.0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float lum = lab->L[y][x] / 32768.0f;
            lum = std::max(0.0f, std::min(1.0f, lum));

            // Base blur weight from amount
            float weight = static_cast<float>(amt);

            // Bokeh boost: brighter areas get more blur (highlight bloom)
            if (bokehAmt > 0.0) {
                weight *= (1.0f - static_cast<float>(bokehAmt) * 0.5f + static_cast<float>(bokehAmt) * lum);
            }

            // Cat eye: radial falloff (more blur toward edges)
            if (cateyeAmt > 0.0) {
                float dx = x - cx;
                float dy = y - cy;
                float dist = std::sqrt(dx * dx + dy * dy) / maxDist;
                weight *= (1.0f - static_cast<float>(cateyeAmt) * 0.7f + static_cast<float>(cateyeAmt) * 0.7f * dist);
            }

            // Focus range: areas far from focus depth get more blur
            if (params.depth > 0 || params.range < 100) {
                float lumDist = std::abs(lum - focusLum) / transRange;
                lumDist = std::min(1.0f, lumDist);
                weight *= lumDist;
            }

            weight = std::min(1.0f, std::max(0.0f, weight));
            lab->L[y][x] = lab->L[y][x] * (1.0f - weight) + blurred[y][x] * weight;
        }
    }

    // Clean up
    for (int y = 0; y < h; y++) {
        delete[] blurred[y];
    }
    delete[] blurred;
}
