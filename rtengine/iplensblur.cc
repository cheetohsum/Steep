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
#include "color.h"
#include "labimage.h"
#include "partnerdepthstore.h"
#include "procparams.h"
#include "rt_math.h"

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <vector>

using namespace rtengine;
using namespace rtengine::procparams;

namespace
{

// Largest aperture radius gathered at the working resolution; anything wider
// is blurred on a box-reduced grid so the tap count stays bounded (~120) and
// a preview pass costs the same at any amount.
constexpr float kMaxGatherRadius = 6.f;

struct Tap {
    int dx;
    int dy;
    float w;
    std::ptrdiff_t offset;   // into the interleaved working grid
};

int bladeCount(const Glib::ustring &shape)
{
    if (shape == "5blade") {
        return 5;
    } else if (shape == "6blade") {
        return 6;
    } else if (shape == "7blade") {
        return 7;
    } else if (shape == "8blade") {
        return 8;
    }

    return 0;
}

// The aperture as a flat kernel: a disc, or a regular n-gon of the same area
// (so switching shape does not change the blur's strength), turned a little
// off square the way stopped-down blades sit. Edges are antialiased by
// coverage so small kernels do not alias into crosses.
std::vector<Tap> apertureKernel(float radius, int blades)
{
    std::vector<Tap> taps;
    float circumradius = radius;
    float inradius = radius;
    float sector = 0.f;
    constexpr float rotation = RT_PI_F / 12.f;

    if (blades >= 3) {
        const float n = blades;
        circumradius = radius * std::sqrt(2.f * RT_PI_F / (n * std::sin(2.f * RT_PI_F / n)));
        inradius = circumradius * std::cos(RT_PI_F / n);
        sector = 2.f * RT_PI_F / n;
    }

    const int extent = static_cast<int>(std::ceil(circumradius + 0.5f));

    for (int dy = -extent; dy <= extent; ++dy) {
        for (int dx = -extent; dx <= extent; ++dx) {
            const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            float edge = radius;

            if (blades >= 3) {
                const float theta = std::atan2(static_cast<float>(dy), static_cast<float>(dx)) - rotation;
                const float t = theta - sector * std::floor(theta / sector) - 0.5f * sector;
                edge = inradius / std::cos(t);
            }

            const float coverage = rtengine::LIM01(edge - d + 0.5f);

            if (coverage > 0.f) {
                taps.push_back({dx, dy, coverage, 0});
            }
        }
    }

    if (taps.empty()) {
        taps.push_back({0, 0, 1.f, 0});
    }

    return taps;
}

} // namespace

void ImProcFunctions::lensBlur(LabImage *lab, const LensBlurParams &params, int fw, int fh,
                               int originX, int originY, double scale, const PartnerDepth *depth)
{
    if (!params.enabled || params.amount <= 0.0) {
        return;
    }

    const int W = lab->W;
    const int H = lab->H;
    const double s = std::max(scale, 1e-3);

    // Defined on the full frame and mapped into this buffer, so the fit
    // preview, a 1:1 tile, a thumbnail and the export agree. The aperture
    // radius is 4 x amount full-resolution pixels: a disc whose spread matches
    // the old Gaussian (sigma 2 x amount), so an export keeps its strength.
    const double fullW = fw > 0 ? fw : W * s;
    const double fullH = fh > 0 ? fh : H * s;
    const float radius = static_cast<float>(params.amount * 4.0 / s);

    if (radius < 0.6f) {
        return;
    }

    // Full defocus from half way up; below that the sharp image shows through.
    const float mix = std::min(1.f, static_cast<float>(params.amount / 50.0));
    const float focus = params.depth / 100.f;
    const float transRange = std::max(params.range / 100.f, 0.01f);
    const bool focusRange = params.depth > 0 || params.range < 100;
    const float cateye = params.cateye / 100.f;
    const bool useDepth = depth && depth->valid();

    // Cat eye ramps toward the corners of the frame, not of this buffer.
    const double cx = fullW * 0.5;
    const double cy = fullH * 0.5;
    const double maxDist = std::max(1.0, std::sqrt(cx * cx + cy * cy));

    // Per pixel: how defocused it is (blend), and how much it may lend to
    // its neighbours' discs. With a real depth map an in-focus subject must
    // not bleed into the background behind it; a luminance proxy would only
    // bias the colour, so it lends evenly.
    std::vector<float> blend(static_cast<size_t>(W) * H);
    std::vector<float> lend(useDepth ? static_cast<size_t>(W) * H : 0);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = 0; y < H; ++y) {
        const double fy = originY + y * s;

        for (int x = 0; x < W; ++x) {
            const double fx = originX + x * s;
            float focusTerm = 1.f;

            if (focusRange) {
                float z = useDepth ? depth->sample(static_cast<float>(fx), static_cast<float>(fy)) : -1.f;

                if (z < 0.f) {
                    z = rtengine::LIM01(lab->L[y][x] / 32768.f);
                }

                focusTerm = std::min(1.f, std::fabs(z - focus) / transRange);
            }

            float catTerm = 1.f;

            if (cateye > 0.f) {
                const float dist = static_cast<float>(std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) / maxDist);
                catTerm = 1.f - 0.7f * cateye + 0.7f * cateye * dist;
            }

            const size_t i = static_cast<size_t>(y) * W + x;
            blend[i] = rtengine::LIM01(mix * catTerm * focusTerm);

            if (useDepth) {
                lend[i] = 0.05f + focusTerm;
            }
        }
    }

    // Working grid: box-reduce by `factor` so the aperture fits the tap
    // budget. Each cell holds weighted sums of linear XYZ and the weight, so
    // a lone highlight keeps its share of energy at any reduction.
    const int factor = std::max(1, static_cast<int>(std::ceil(radius / kMaxGatherRadius)));
    const int sw = (W + factor - 1) / factor;
    const int sh = (H + factor - 1) / factor;
    const float smallRadius = radius / factor;

    // Bokeh boost: highlights weigh more in every disc they fall into, so a
    // bright point opens into a bright aperture shape instead of averaging
    // away. Keyed to linear Y, not L*.
    const float boost = static_cast<float>(params.bokeh / 100.0) * 24.f;

    std::vector<float> grid(static_cast<size_t>(sw) * sh * 4, 0.f);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int gy = 0; gy < sh; ++gy) {
        const int y0 = gy * factor;
        const int y1 = std::min(H, y0 + factor);

        for (int gx = 0; gx < sw; ++gx) {
            const int x0 = gx * factor;
            const int x1 = std::min(W, x0 + factor);
            float sx = 0.f, sy = 0.f, sz = 0.f, wsum = 0.f;

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float X, Y, Z;
                    Color::Lab2XYZ(lab->L[y][x], lab->a[y][x], lab->b[y][x], X, Y, Z);
                    const float h = std::min(8.f, std::max(0.f, (Y / 65535.f - 0.45f) / 0.55f));
                    float w = 1.f + boost * h * h;

                    if (useDepth) {
                        w *= lend[static_cast<size_t>(y) * W + x];
                    }

                    sx += w * X;
                    sy += w * Y;
                    sz += w * Z;
                    wsum += w;
                }
            }

            float *cell = &grid[(static_cast<size_t>(gy) * sw + gx) * 4];
            cell[0] = sx;
            cell[1] = sy;
            cell[2] = sz;
            cell[3] = wsum;
        }
    }

    std::vector<Tap> taps = apertureKernel(smallRadius, bladeCount(params.shape));
    int extent = 0;

    for (auto &t : taps) {
        t.offset = (static_cast<std::ptrdiff_t>(t.dy) * sw + t.dx) * 4;
        extent = std::max(extent, std::max(std::abs(t.dx), std::abs(t.dy)));
    }

    // Gather: each cell is the weighted mean of the aperture around it.
    std::vector<float> blurred(static_cast<size_t>(sw) * sh * 3);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int gy = 0; gy < sh; ++gy) {
        const bool rowInside = gy >= extent && gy < sh - extent;

        for (int gx = 0; gx < sw; ++gx) {
            const size_t ci = static_cast<size_t>(gy) * sw + gx;
            const float *centre = &grid[ci * 4];
            float ax = 0.f, ay = 0.f, az = 0.f, aw = 0.f;

            if (rowInside && gx >= extent && gx < sw - extent) {
                for (const auto &t : taps) {
                    const float *p = centre + t.offset;
                    ax += t.w * p[0];
                    ay += t.w * p[1];
                    az += t.w * p[2];
                    aw += t.w * p[3];
                }
            } else {
                // Outside the buffer contributes nothing; the mean
                // renormalises over what is there.
                for (const auto &t : taps) {
                    const int xx = gx + t.dx;
                    const int yy = gy + t.dy;

                    if (xx < 0 || yy < 0 || xx >= sw || yy >= sh) {
                        continue;
                    }

                    const float *p = &grid[(static_cast<size_t>(yy) * sw + xx) * 4];
                    ax += t.w * p[0];
                    ay += t.w * p[1];
                    az += t.w * p[2];
                    aw += t.w * p[3];
                }
            }

            float *out = &blurred[ci * 3];

            if (aw > 0.f) {
                out[0] = ax / aw;
                out[1] = ay / aw;
                out[2] = az / aw;
            } else {
                const float cw = std::max(centre[3], 1e-6f);
                out[0] = centre[0] / cw;
                out[1] = centre[1] / cw;
                out[2] = centre[2] / cw;
            }
        }
    }

    // Back to full resolution (bilinear), back to Lab, blended by how far
    // each pixel sits from the focus plane. L, a and b move together, so
    // colour edges defocus with the tones.
    const float invFactor = 1.f / factor;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int y = 0; y < H; ++y) {
        const float v = std::max(0.f, std::min(static_cast<float>(sh - 1), (y + 0.5f) * invFactor - 0.5f));
        const int r0 = static_cast<int>(v);
        const int r1 = std::min(r0 + 1, sh - 1);
        const float fv = v - r0;

        for (int x = 0; x < W; ++x) {
            const float m = blend[static_cast<size_t>(y) * W + x];

            if (m <= 1e-4f) {
                continue;
            }

            const float u = std::max(0.f, std::min(static_cast<float>(sw - 1), (x + 0.5f) * invFactor - 0.5f));
            const int c0 = static_cast<int>(u);
            const int c1 = std::min(c0 + 1, sw - 1);
            const float fu = u - c0;

            const float *p00 = &blurred[(static_cast<size_t>(r0) * sw + c0) * 3];
            const float *p01 = &blurred[(static_cast<size_t>(r0) * sw + c1) * 3];
            const float *p10 = &blurred[(static_cast<size_t>(r1) * sw + c0) * 3];
            const float *p11 = &blurred[(static_cast<size_t>(r1) * sw + c1) * 3];
            float xyz[3];

            for (int c = 0; c < 3; ++c) {
                const float top = p00[c] + fu * (p01[c] - p00[c]);
                const float bottom = p10[c] + fu * (p11[c] - p10[c]);
                xyz[c] = top + fv * (bottom - top);
            }

            float L, a, b;
            Color::XYZ2Lab(xyz[0], xyz[1], xyz[2], L, a, b);

            lab->L[y][x] += m * (L - lab->L[y][x]);
            lab->a[y][x] += m * (a - lab->a[y][x]);
            lab->b[y][x] += m * (b - lab->b[y][x]);
        }
    }
}
