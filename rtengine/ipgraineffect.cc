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
#include "imagefloat.h"
#include "labimage.h"
#include "procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

void ImProcFunctions::grainEffect(LabImage *lab, const GrainParams &params, int fw, int fh,
                                  int originX, int originY, int oscale)
{
    if (!params.enabled || params.strength == 0) {
        return;
    }

    const int w = lab->W;
    const int h = lab->H;

    Imagefloat *tmp = new Imagefloat(w, h);

    // Copy L channel to g (grain operates on g channel as luminance)
    // Copy a→r, b→b to preserve the pattern used by filmGrain
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            tmp->g(y, x) = lab->L[y][x];
            tmp->r(y, x) = lab->a[y][x];
            tmp->b(y, x) = lab->b[y][x];
        }
    }

    filmGrainV2(tmp, params.iso, params.strength, params.scale, params.color, originX, originY, oscale, fw, fh);

    // Copy luminance - and, when chroma grain ran, a/b - back
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            lab->L[y][x] = tmp->g(y, x);

            if (params.color > 0) {
                lab->a[y][x] = tmp->r(y, x);
                lab->b[y][x] = tmp->b(y, x);
            }
        }
    }

    delete tmp;
}
