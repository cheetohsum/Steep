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
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <vector>
#include <utility>
#include <mutex>
#include <unordered_map>
#include "array2D.h"

namespace rtengine {

// Rasterize a polygon mask from a list of vertices.
// vertices: polygon vertices as (x,y) pairs in absolute image pixel coordinates
// width, height: output mask dimensions (full image size)
// feather: feather width in pixels (0 = hard edge, up to ~100)
// Returns a float mask: 0.0 = outside, 1.0 = inside, 0..1 = feather zone
void rasterizePolygonMask(
    const std::vector<std::pair<float, float>>& vertices,
    int width, int height,
    float feather,
    array2D<float>& mask
);

// Point-in-polygon test using ray casting algorithm
bool pointInPolygon(
    float px, float py,
    const std::vector<std::pair<float, float>>& vertices
);

// Minimum distance from a point to the polygon boundary (nearest edge segment)
float distToPolygonBoundary(
    float px, float py,
    const std::vector<std::pair<float, float>>& vertices
);

// Cache for rasterized polygon masks — avoids re-rasterization when
// vertices, feather, and output resolution haven't changed.
class PolyMaskCache {
public:
    static PolyMaskCache& getInstance();

    // Returns true if the cached mask is still valid for the given parameters,
    // and copies the cached mask into 'mask'. Returns false if re-rasterization is needed.
    bool getCached(int spotIndex,
                   const std::vector<int>& points,
                   float feather,
                   int width, int height,
                   array2D<float>& mask);

    // Store a newly rasterized mask in the cache.
    void store(int spotIndex,
               const std::vector<int>& points,
               float feather,
               int width, int height,
               const array2D<float>& mask);

    // Clear the cache (e.g., on image change)
    void clear();

private:
    PolyMaskCache() = default;

    struct CacheEntry {
        std::vector<int> points;
        float feather = 0.f;
        int width = 0;
        int height = 0;
        array2D<float> mask;
    };

    std::mutex mutex_;
    std::unordered_map<int, CacheEntry> entries_;  // keyed by spot index
};

} // namespace rtengine
