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

#include <array>
#include <memory>
#include <string>

#include <glibmm/ustring.h>

#include "array2D.h"

namespace rtengine
{

class Imagefloat;

// Relative scene depth estimated on a double exposure partner, 0 near to 1
// far, at the partner's preview tier. Relative within that photograph only:
// two partners' depths are not the same distance. Invalid samples are
// negative, and a read touching one is invalid as a whole, so an inverted
// range cannot reveal what the estimator could not see.
struct PartnerDepth {
    array2D<float> depth;   // `width` x `height`
    int width = 0;
    int height = 0;
    int fullWidth = 0;      // the partner's own full frame, which `depth` covers
    int fullHeight = 0;

    bool valid() const
    {
        return width > 0 && height > 0;
    }

    // Bilinear read at partner full-frame coordinates, edge-clamped; -1 when
    // any contributing sample is invalid.
    float sample(float u, float v) const;
};

// What a layer keeps of its depth: four ordered handles (near fade start,
// near full, far full, far fade end), the shape of each transition and
// whether the selection is turned inside out. The same maths the
// compositor's layers use, so a range reads the same in both tools.
struct DepthSelection {
    std::array<double, 4> range = {{0.0, 0.0, 1.0, 1.0}};
    double nearShape = 1.0;
    double farShape = 1.0;
    bool invert = false;

    bool operator==(const DepthSelection& other) const;
    bool operator!=(const DepthSelection& other) const
    {
        return !(*this == other);
    }

    // 0..1 weight for a depth sample; an invalid sample selects nothing.
    float weight(float depth) const;
};

// LRU of estimated partner depth maps, one per (file, working profile).
// Estimation runs on the calling thread the first time a map is asked for,
// as PartnerMaskStore does for segmentation; the picker warms it on its own
// worker so a redraw never waits on the model.
class PartnerDepthStore final
{
public:
    static PartnerDepthStore& getInstance();

    PartnerDepthStore(const PartnerDepthStore&) = delete;
    PartnerDepthStore& operator=(const PartnerDepthStore&) = delete;

    // Directory holding composite_depth.onnx; set once by rtengine::init.
    void setModelDirectory(const Glib::ustring& directory);
    bool available() const;

    // nullptr when no model is installed or the partner cannot be decoded;
    // the layer then renders unrestricted, as a missing mask does.
    std::shared_ptr<const PartnerDepth> getDepth(const Glib::ustring& path, const Glib::ustring& workingProfile);

    // Cache-only lookup for redraws.
    std::shared_ptr<const PartnerDepth> peekDepth(const Glib::ustring& path, const Glib::ustring& workingProfile);

    // Depth of the photograph being edited, for tools that light by distance.
    // `source` is its working-space frame after transforms -- the frame the AI
    // masks use -- at any resolution; fullWidth/fullHeight is that frame at
    // full size, which the returned map covers. Estimated on the calling
    // thread the first time; afterwards reused while the frame keeps its
    // shape and content, so a preview's map also serves the detail windows
    // and an export from the same session.
    std::shared_ptr<const PartnerDepth> getSceneDepth(const std::string& imageId, const Imagefloat& source,
                                                      int fullWidth, int fullHeight, const Glib::ustring& workingProfile);

    // Cache-only: the latest map of this image whose frame has the same shape.
    std::shared_ptr<const PartnerDepth> peekSceneDepth(const std::string& imageId, int fullWidth, int fullHeight,
                                                       const Glib::ustring& workingProfile);

    void clearCache();

private:
    PartnerDepthStore();
    ~PartnerDepthStore();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rtengine
