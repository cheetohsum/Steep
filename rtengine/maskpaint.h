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
#pragma once

// Hand-painted corrections to an automatic mask. A segmentation is a good
// guess and no more, so the user needs somewhere to say "not that bit" and
// "this bit too". Strokes are kept as geometry in normalised frame
// coordinates rather than as a rasterised mask: they cost a line of text in
// the pp3, replay at any resolution, and survive the mask being recomputed
// with different settings underneath them.

#include <string>
#include <vector>

// array2D lives at global scope, not in rtengine.
template<typename T>
class array2D;

namespace rtengine
{

struct MaskStroke {
    // A point on the stroke, as a fraction of the frame's width and height.
    struct Point {
        double x = 0.0;
        double y = 0.0;

        bool operator ==(const Point& other) const
        {
            return x == other.x && y == other.y;
        }
    };

    bool add = true;         // include, or cut away
    double radius = 0.05;    // fraction of the frame's SHORT side
    double hardness = 0.25;  // 0 = feathered to nothing, 1 = a hard disc
    double strength = 1.0;   // 0..1
    std::vector<Point> points;

    bool operator ==(const MaskStroke& other) const;

    bool operator !=(const MaskStroke& other) const
    {
        return !(*this == other);
    }
};

struct MaskPaint {
    std::vector<MaskStroke> strokes;

    bool empty() const
    {
        return strokes.empty();
    }

    bool operator ==(const MaskPaint& other) const
    {
        return strokes == other.strokes;
    }

    bool operator !=(const MaskPaint& other) const
    {
        return !(*this == other);
    }

    // One pp3 value. Empty when there is nothing painted, so the key can be
    // left out altogether and a file written before painting existed loads
    // as unpainted.
    std::string encode() const;
    static MaskPaint decode(const std::string& text);

    // Stable across runs and platforms: it keys the caches that hold prepared
    // masks, so it has to change whenever a stroke does and not otherwise.
    unsigned int hash() const;

    // Stamps the strokes into `mask`, which holds values in 0..1 over the
    // frame. Adding raises the mask towards the stroke's strength, cutting
    // lowers it away from it, so the two are commutative within their kind
    // and the later kind wins where they cross.
    void apply(array2D<float>& mask, int width, int height, bool multiThread) const;
};

} // namespace rtengine
