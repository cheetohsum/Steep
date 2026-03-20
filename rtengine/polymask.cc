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
#include "polymask.h"
#include "rt_math.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace rtengine {

bool pointInPolygon(float px, float py,
                    const std::vector<std::pair<float, float>>& vertices)
{
    const int n = static_cast<int>(vertices.size());
    if (n < 3) {
        return false;
    }

    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float xi = vertices[i].first, yi = vertices[i].second;
        const float xj = vertices[j].first, yj = vertices[j].second;

        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// Distance from point (px,py) to the line segment (ax,ay)-(bx,by)
static inline float distToSegment(float px, float py,
                                  float ax, float ay, float bx, float by)
{
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lenSq = dx * dx + dy * dy;

    if (lenSq < 1e-12f) {
        const float ex = px - ax;
        const float ey = py - ay;
        return std::sqrt(ex * ex + ey * ey);
    }

    float t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = LIM(t, 0.f, 1.f);

    const float closestX = ax + t * dx;
    const float closestY = ay + t * dy;
    const float ex = px - closestX;
    const float ey = py - closestY;
    return std::sqrt(ex * ex + ey * ey);
}

float distToPolygonBoundary(float px, float py,
                            const std::vector<std::pair<float, float>>& vertices)
{
    const int n = static_cast<int>(vertices.size());
    if (n < 2) {
        return FLT_MAX;
    }

    float minDist = FLT_MAX;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float d = distToSegment(
            px, py,
            vertices[j].first, vertices[j].second,
            vertices[i].first, vertices[i].second
        );
        if (d < minDist) {
            minDist = d;
        }
    }
    return minDist;
}

// Compute scanline intersections for a single row.
// Returns sorted x-intercepts where the polygon boundary crosses scanline y=py.
static void scanlineIntersections(float py,
                                  const std::vector<std::pair<float, float>>& vertices,
                                  std::vector<float>& xIntersections)
{
    xIntersections.clear();
    const int n = static_cast<int>(vertices.size());

    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float yi = vertices[i].second;
        const float yj = vertices[j].second;

        // Edge must cross the scanline (not touch at endpoints to avoid double-counting)
        if ((yi > py) != (yj > py)) {
            const float xi = vertices[i].first;
            const float xj = vertices[j].first;
            const float xHit = xi + (py - yi) / (yj - yi) * (xj - xi);
            xIntersections.push_back(xHit);
        }
    }
    std::sort(xIntersections.begin(), xIntersections.end());
}

void rasterizePolygonMask(
    const std::vector<std::pair<float, float>>& vertices,
    int width, int height,
    float feather,
    array2D<float>& mask)
{
    mask(width, height, ARRAY2D_CLEAR_DATA);

    const int n = static_cast<int>(vertices.size());
    if (n < 3) {
        return;
    }

    // Compute bounding box with feather margin
    float minX = FLT_MAX, minY = FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (const auto& v : vertices) {
        minX = std::min(minX, v.first);
        minY = std::min(minY, v.second);
        maxX = std::max(maxX, v.first);
        maxY = std::max(maxY, v.second);
    }

    const int y0 = std::max(0, static_cast<int>(std::floor(minY - feather)));
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxY + feather)));
    const int x0 = std::max(0, static_cast<int>(std::floor(minX - feather)));
    const int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxX + feather)));

    if (feather <= 0.f) {
        // --- Hard edge: scanline fill, no distance computation ---
#ifdef _OPENMP
        #pragma omp parallel
#endif
        {
            std::vector<float> xHits;
#ifdef _OPENMP
            #pragma omp for schedule(dynamic, 16)
#endif
            for (int y = y0; y <= y1; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                scanlineIntersections(py, vertices, xHits);

                // Fill between pairs of intersections (even-odd rule)
                for (size_t k = 0; k + 1 < xHits.size(); k += 2) {
                    const int xa = std::max(x0, static_cast<int>(std::ceil(xHits[k] - 0.5f)));
                    const int xb = std::min(x1, static_cast<int>(std::floor(xHits[k + 1] - 0.5f)));
                    for (int x = xa; x <= xb; ++x) {
                        mask[y][x] = 1.f;
                    }
                }
            }
        }
        return;
    }

    // --- Feathered edge: two-pass approach ---
    // Pass 1: Scanline fill to determine inside/outside efficiently
    // Pass 2: Apply feathering only near polygon edges

    const float featherInv = 1.f / feather;

    // Tight bounding box of the polygon itself (no feather)
    const int iy0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int iy1 = std::min(height - 1, static_cast<int>(std::ceil(maxY)));

    // Pass 1: Fill interior with 1.0 using scanline
#ifdef _OPENMP
    #pragma omp parallel
#endif
    {
        std::vector<float> xHits;
#ifdef _OPENMP
        #pragma omp for schedule(dynamic, 16)
#endif
        for (int y = iy0; y <= iy1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            scanlineIntersections(py, vertices, xHits);

            for (size_t k = 0; k + 1 < xHits.size(); k += 2) {
                const int xa = std::max(x0, static_cast<int>(std::ceil(xHits[k] - 0.5f)));
                const int xb = std::min(x1, static_cast<int>(std::floor(xHits[k + 1] - 0.5f)));
                for (int x = xa; x <= xb; ++x) {
                    mask[y][x] = 1.f;
                }
            }
        }
    }

    // Pass 2: Apply feathering near polygon edges.
    // Only pixels within feather distance of the boundary need distance computation.
    // We process a band of feather width around each polygon edge's bounding box.
    const int featherCeil = static_cast<int>(std::ceil(feather));

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;

        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;

            // Quick reject: if pixel is deep inside (far from any edge), skip.
            // Check if all 4 neighbors at feather distance are also inside —
            // if so, this pixel doesn't need feathering.
            const float curVal = mask[y][x];

            // Check if we're near a boundary: look at a small neighborhood
            // If this pixel and its feather-distance neighbors all have the same
            // inside/outside status, no feathering is needed.
            bool nearBoundary = false;

            // Check cardinal directions at feather distance
            const int xm = std::max(x0, x - featherCeil);
            const int xp = std::min(x1, x + featherCeil);
            const int ym = std::max(y0, y - featherCeil);
            const int yp = std::min(y1, y + featherCeil);

            if (curVal > 0.5f) {
                // Inside: check if any neighbor at feather distance is outside
                if (mask[ym][x] < 0.5f || mask[yp][x] < 0.5f ||
                    mask[y][xm] < 0.5f || mask[y][xp] < 0.5f) {
                    nearBoundary = true;
                }
            } else {
                // Outside: check if any neighbor at feather distance is inside
                if (mask[ym][x] > 0.5f || mask[yp][x] > 0.5f ||
                    mask[y][xm] > 0.5f || mask[y][xp] > 0.5f) {
                    nearBoundary = true;
                }
            }

            if (!nearBoundary) {
                continue; // Deep inside or deep outside — no feathering needed
            }

            // Compute exact distance to nearest polygon edge
            const float dist = distToPolygonBoundary(px, py, vertices);

            if (dist >= feather) {
                continue; // Outside feather zone — keep current value
            }

            if (curVal > 0.5f) {
                // Inside, near edge: ramp from 0 at edge to 1 at feather depth
                mask[y][x] = dist * featherInv;
            } else {
                // Outside, near edge: ramp from 1 at edge to 0 at feather distance
                mask[y][x] = 1.f - dist * featherInv;
            }
        }
    }
}

// --- PolyMaskCache implementation ---

PolyMaskCache& PolyMaskCache::getInstance()
{
    static PolyMaskCache instance;
    return instance;
}

bool PolyMaskCache::getCached(int spotIndex,
                               const std::vector<int>& points,
                               float feather,
                               int width, int height,
                               array2D<float>& mask)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(spotIndex);
    if (it == entries_.end()) {
        return false;
    }

    const auto& entry = it->second;
    if (entry.width != width || entry.height != height ||
        std::abs(entry.feather - feather) > 0.01f ||
        entry.points != points) {
        return false;
    }

    // Cache hit: copy the mask
    mask(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            mask[y][x] = entry.mask[y][x];
        }
    }
    return true;
}

void PolyMaskCache::store(int spotIndex,
                           const std::vector<int>& points,
                           float feather,
                           int width, int height,
                           const array2D<float>& mask)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = entries_[spotIndex];
    entry.points = points;
    entry.feather = feather;
    entry.width = width;
    entry.height = height;
    entry.mask(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            entry.mask[y][x] = mask[y][x];
        }
    }
}

void PolyMaskCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

} // namespace rtengine
