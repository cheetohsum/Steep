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
#include "maskpaint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "array2D.h"

namespace rtengine
{

namespace
{

// Distance from a point to a segment, the shape a brush actually leaves: a
// stroke is a chain of these, not a string of separate discs.
float distanceToSegment(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax;
    const float vy = by - ay;
    const float len2 = vx * vx + vy * vy;
    float t = 0.f;

    if (len2 > 1e-12f) {
        t = std::min(std::max(((px - ax) * vx + (py - ay) * vy) / len2, 0.f), 1.f);
    }

    const float dx = px - (ax + t * vx);
    const float dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

// Brush profile: 1 at the centre, 0 at the rim, with `hardness` deciding how
// much of the radius stays at full strength before the smoothstep falls away.
float brushFalloff(float distance, float radius, float hardness)
{
    if (radius <= 0.f) {
        return 0.f;
    }

    const float r = distance / radius;

    if (r >= 1.f) {
        return 0.f;
    }

    const float core = std::min(std::max(hardness, 0.f), 0.98f);

    if (r <= core) {
        return 1.f;
    }

    const float t = 1.f - (r - core) / (1.f - core);
    return t * t * (3.f - 2.f * t);
}

} // namespace

bool MaskStroke::operator ==(const MaskStroke& other) const
{
    return add == other.add
        && radius == other.radius
        && hardness == other.hardness
        && strength == other.strength
        && points == other.points;
}

std::string MaskPaint::encode() const
{
    std::string out;
    char buffer[64];

    for (const auto& stroke : strokes) {
        if (stroke.points.empty()) {
            continue;
        }

        if (!out.empty()) {
            out += ';';
        }

        std::snprintf(buffer, sizeof(buffer), "%c,%.4f,%.3f,%.3f",
                      stroke.add ? 'a' : 's', stroke.radius, stroke.hardness, stroke.strength);
        out += buffer;

        for (const auto& point : stroke.points) {
            std::snprintf(buffer, sizeof(buffer), ",%.4f,%.4f", point.x, point.y);
            out += buffer;
        }
    }

    return out;
}

MaskPaint MaskPaint::decode(const std::string& text)
{
    MaskPaint paint;
    std::stringstream strokeStream(text);
    std::string strokeText;

    while (std::getline(strokeStream, strokeText, ';')) {
        std::stringstream fieldStream(strokeText);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(fieldStream, field, ',')) {
            fields.push_back(field);
        }

        // kind, radius, hardness, strength, then pairs of coordinates.
        if (fields.size() < 6 || (fields.size() - 4) % 2 != 0) {
            continue;
        }

        MaskStroke stroke;
        stroke.add = fields[0] != "s";

        try {
            stroke.radius = std::stod(fields[1]);
            stroke.hardness = std::stod(fields[2]);
            stroke.strength = std::stod(fields[3]);

            for (size_t i = 4; i + 1 < fields.size(); i += 2) {
                stroke.points.push_back({std::stod(fields[i]), std::stod(fields[i + 1])});
            }
        } catch (...) {
            continue;   // a malformed stroke is dropped, not fatal
        }

        if (!stroke.points.empty()) {
            paint.strokes.push_back(std::move(stroke));
        }
    }

    return paint;
}

unsigned int MaskPaint::hash() const
{
    // FNV-1a over the encoded form: whatever round-trips through the pp3 is
    // exactly what the caches should key on.
    const std::string text = encode();
    unsigned int h = 2166136261u;

    for (char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }

    return h;
}

void MaskPaint::apply(array2D<float>& mask, int width, int height, bool multiThread) const
{
    if (strokes.empty() || width <= 0 || height <= 0) {
        return;
    }

    // The radius is a fraction of the short side, so a brush keeps its size
    // relative to the picture whatever resolution this is being replayed at.
    const float shortSide = static_cast<float>(std::min(width, height));

    for (const auto& stroke : strokes) {
        if (stroke.points.empty()) {
            continue;
        }

        const float radius = std::max(1.f, static_cast<float>(stroke.radius) * shortSide);
        const float hardness = static_cast<float>(stroke.hardness);
        const float strength = std::min(std::max(static_cast<float>(stroke.strength), 0.f), 1.f);

        // Only the box the stroke can reach is touched; a small brush on a
        // large mask should cost what it looks like it costs.
        float minX = static_cast<float>(width);
        float minY = static_cast<float>(height);
        float maxX = 0.f;
        float maxY = 0.f;

        std::vector<float> px(stroke.points.size());
        std::vector<float> py(stroke.points.size());

        for (size_t i = 0; i < stroke.points.size(); ++i) {
            px[i] = static_cast<float>(stroke.points[i].x) * width;
            py[i] = static_cast<float>(stroke.points[i].y) * height;
            minX = std::min(minX, px[i]);
            maxX = std::max(maxX, px[i]);
            minY = std::min(minY, py[i]);
            maxY = std::max(maxY, py[i]);
        }

        const int x0 = std::max(0, static_cast<int>(std::floor(minX - radius)));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxX + radius)));
        const int y0 = std::max(0, static_cast<int>(std::floor(minY - radius)));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxY + radius)));

        if (x1 < x0 || y1 < y0) {
            continue;
        }

        const size_t segments = px.size() > 1 ? px.size() - 1 : 1;

#ifdef _OPENMP
        #pragma omp parallel for if(multiThread)
#endif
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float fx = x + 0.5f;
                const float fy = y + 0.5f;
                float nearest = radius;

                for (size_t seg = 0; seg < segments; ++seg) {
                    const size_t next = px.size() > 1 ? seg + 1 : seg;
                    const float d = distanceToSegment(fx, fy, px[seg], py[seg], px[next], py[next]);

                    if (d < nearest) {
                        nearest = d;

                        if (nearest == 0.f) {
                            break;
                        }
                    }
                }

                const float weight = strength * brushFalloff(nearest, radius, hardness);

                if (weight <= 0.f) {
                    continue;
                }

                float& value = mask[y][x];

                if (stroke.add) {
                    value = std::max(value, weight);
                } else {
                    value = std::min(value, 1.f - weight);
                }
            }
        }
    }
}

} // namespace rtengine
