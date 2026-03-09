/** -*- C++ -*-
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
#include "rt_math.h"

#include <cmath>
#include <cstring>

namespace rtengine {

void ImProcFunctions::tiltShiftEffect(LabImage *lab, const procparams::TiltShiftParams &params, int fw, int fh)
{
    if (!params.enabled || params.amount == 0) {
        return;
    }

    const int W = lab->W;
    const int H = lab->H;

    // Compute sigma from amount (0-100 -> 0-50 pixel sigma)
    const double sigma = params.amount * 0.5;

    // Focus band parameters in pixels
    const double focusCenterY = (params.focusPos / 100.0) * H;
    const double halfWidth = (params.focusWidth / 100.0) * H * 0.5;
    const double featherPx = std::max(1.0, (params.feather / 100.0) * H * 0.3);

    // Angle: offset per column
    const double angleRad = params.angle * (RT_PI / 180.0);
    const double tanAngle = std::tan(angleRad);
    const double centerX = W * 0.5;

    // Allocate blurred L channel
    float **blurL = new float*[H];
    for (int y = 0; y < H; y++) {
        blurL[y] = new float[W];
        memcpy(blurL[y], lab->L[y], W * sizeof(float));
    }

    gaussianBlur(lab->L, blurL, W, H, sigma);

    // Also blur a and b channels
    float **blurA = new float*[H];
    float **blurB = new float*[H];
    for (int y = 0; y < H; y++) {
        blurA[y] = new float[W];
        blurB[y] = new float[W];
        memcpy(blurA[y], lab->a[y], W * sizeof(float));
        memcpy(blurB[y], lab->b[y], W * sizeof(float));
    }

    gaussianBlur(lab->a, blurA, W, H, sigma);
    gaussianBlur(lab->b, blurB, W, H, sigma);

    // Blend original and blurred based on distance from focus band
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            // Compute focus center for this column (angle offset)
            double centerY = focusCenterY + (x - centerX) * tanAngle;

            // Distance from focus band edge
            double dist = std::abs(y - centerY) - halfWidth;
            if (dist < 0.0) {
                dist = 0.0;
            }

            // Alpha: 0 = sharp (in focus), 1 = fully blurred
            double alpha = dist / featherPx;
            if (alpha > 1.0) {
                alpha = 1.0;
            }

            // Hermite smoothstep: 3t^2 - 2t^3
            alpha = alpha * alpha * (3.0 - 2.0 * alpha);

            // Blend
            lab->L[y][x] = lab->L[y][x] * (1.0f - alpha) + blurL[y][x] * alpha;
            lab->a[y][x] = lab->a[y][x] * (1.0f - alpha) + blurA[y][x] * alpha;
            lab->b[y][x] = lab->b[y][x] * (1.0f - alpha) + blurB[y][x] * alpha;
        }
    }

    // Free blurred buffers
    for (int y = 0; y < H; y++) {
        delete[] blurL[y];
        delete[] blurA[y];
        delete[] blurB[y];
    }
    delete[] blurL;
    delete[] blurA;
    delete[] blurB;
}

} // namespace rtengine
