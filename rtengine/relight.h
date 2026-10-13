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
#pragma once

#include <string>

#include <glibmm/ustring.h>

namespace rtengine
{

class Imagefloat;

namespace procparams
{
struct RelightParams;
}

// Where a buffer handed to ImProcFunctions::relight sits in the photograph:
// the full frame after transforms (the frame the AI masks and the scene depth
// map cover), and the buffer's top-left corner and pixel pitch in it.
struct RelightFrame {
    std::string imageId;            // imgsrc->getFileName(): keys mask and depth
    Glib::ustring workingProfile;
    int fullWidth = 0;
    int fullHeight = 0;
    double originX = 0.0;           // full-frame pixels
    double originY = 0.0;
    double step = 1.0;              // full-frame pixels per buffer pixel
};

// Makes sure the subject mask and the depth map of the photograph exist,
// estimating them from `source` (its working-space RGB after transforms) the
// first time. Runs the models on the calling thread, so the pass that turns
// relight on pays for them once; later passes find them cached. With
// `reuseCached`, a mask made from another rendering of the same photograph
// is kept rather than segmented again (exports after a preview).
void prepareRelightInputs(const procparams::RelightParams& params, const std::string& imageId,
                          const Imagefloat& source, int fullWidth, int fullHeight,
                          const Glib::ustring& workingProfile, bool reuseCached);

} // namespace rtengine
