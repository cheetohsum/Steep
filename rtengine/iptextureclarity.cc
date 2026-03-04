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

#include <cmath>

#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"

using namespace rtengine;
using namespace rtengine::procparams;

void ImProcFunctions::textureContrast(LabImage *lab, const TextureParams &params, double scale)
{
    if (!params.enabled || params.amount == 0.0) {
        return;
    }

    LocalContrastParams lcp;
    lcp.enabled = true;
    lcp.radius = params.radius;
    lcp.amount = std::abs(params.amount) / 100.0;
    lcp.darkness = params.amount > 0 ? 1.0 : 0.5;
    lcp.lightness = params.amount > 0 ? 1.0 : 0.5;

    localContrast(lab, lab->L, lcp, false, scale);
}

void ImProcFunctions::clarityContrast(LabImage *lab, const ClarityParams &params, double scale)
{
    if (!params.enabled || params.amount == 0.0) {
        return;
    }

    LocalContrastParams lcp;
    lcp.enabled = true;
    lcp.radius = params.radius;
    lcp.amount = std::abs(params.amount) / 100.0;
    lcp.darkness = params.amount > 0 ? 1.0 : 0.5;
    lcp.lightness = params.amount > 0 ? 1.0 : 0.5;

    localContrast(lab, lab->L, lcp, false, scale);
}
