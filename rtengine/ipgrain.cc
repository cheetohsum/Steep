/* -*- C++ -*-
 *
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2018 Alberto Griggio <alberto.griggio@gmail.com>
 *  Small adaptation to Rawtherapee Locallab October 2019
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

/* film grain emulation.
 * Ported from darktable (src/iop/grain.c). Original copyright/license follows
 */
/*
    This file is part of darktable,
    copyright (c) 2010-2012 Henrik Andersson.
    adaptation to Rawtherapee 2021 Jacques Desmis jdesmis@gmail.com

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "imagefloat.h"
#include "improcfun.h"
#include "graincharacter.h"
#include "rt_math.h"
#include <cstdint>
#include <vector>


namespace rtengine {

namespace {

constexpr float GRAIN_LIGHTNESS_STRENGTH_SCALE = 0.15f;
constexpr float GRAIN_SCALE_FACTOR = 213.2f;

constexpr int GRAIN_LUT_SIZE = 128;
constexpr float GRAIN_LUT_DELTA_MAX = 2.0f;
constexpr float GRAIN_LUT_DELTA_MIN = 0.0001f;
constexpr float GRAIN_LUT_PAPER_GAMMA = 1.0f;


const int grad3[12][3] = { { 1, 1, 0 },
                           { -1, 1, 0 },
                           { 1, -1, 0 },
                           { -1, -1, 0 },
                           { 1, 0, 1 },
                           { -1, 0, 1 },
                           { 1, 0, -1 },
                           { -1, 0, -1 },
                           { 0, 1, 1 },
                           { 0, -1, 1 },
                           { 0, 1, -1 },
                           { 0, -1, -1 } };

const int permutation[]
    = { 151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,
        69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,
        94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
        171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
        60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161,
        1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
        164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126,
        255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
        119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
        19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193,
        238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
        181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
        222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180 };


class GrainEvaluator {
public:
    GrainEvaluator(int offset_x, int offset_y, int full_width, int full_height, double scale, float divgr, int call, int fww, int fhh):
        ox(offset_x),
        oy(offset_y),
        fw(full_width),
        fh(full_height),
        scale(scale)
    {
        simplex_noise_init();
        constexpr float mb = 100.f;// * divgr;
        evaluate_grain_lut(mb, divgr);
    }
    
    // Legacy locallab path. The standalone Grain tool (Effects > Grain) uses
    // the dye-cloud model in grainv3 below.
    void operator()(int isogr, int strengr, int scalegr, float divgr, Imagefloat *lab, bool multithread, int call, int fww, int fhh)
    {
        const double strength = (strengr / 100.0);
        const double octaves = 3.;
        const double wd = std::min(fw, fh);
        const double wdf = std::min(fww, fhh);
        
        const double zoom = (1.0 + 8 * (double(isogr) / GRAIN_SCALE_FACTOR) / 100.0) / 800.0;
        const double s = std::max(scale / 3.0, 1.0) / (double(std::max(scalegr, 1)) / 100.0);
        const int W = lab->getWidth();
        const int H = lab->getHeight();
        float **lab_L = lab->g.ptrs;
        double wddf = wd;
        if (call == 1 || call == 3) {
            wddf = wdf;
        }


#ifdef _OPENMP
#       pragma omp parallel for if (multithread)
#endif
        for (int j = 0; j < H; ++j) {
            double wy = oy + j;
            double y = wy / wddf;
            for (int i = 0; i < W; ++i) {
                double wx = ox + i;
                double x = wx / wddf;
                double noise = simplex_2d_noise(x, y, octaves, zoom) / s;
                lab_L[j][i] += lut_lookup(noise * strength * GRAIN_LIGHTNESS_STRENGTH_SCALE, lab_L[j][i] / 32768.f);
            }
        }
    }

private:
    void simplex_noise_init()
    {
        for(int i = 0; i < 512; i++) perm[i] = permutation[i & 255];
    }

    double dot(const int *g, double x, double y, double z)
    {
        return g[0] * x + g[1] * y + g[2] * z;
    }

    float FASTFLOOR(float x)
    {
        return (x > 0 ? (int)(x) : (int)(x)-1);
    }

    double simplex_noise(double xin, double yin, double zin)
    {
        double n0, n1, n2, n3; // Noise contributions from the four corners
        // Skew the input space to determine which simplex cell we're in
        const double F3 = 1.0 / 3.0;
        const double s = (xin + yin + zin) * F3; // Very nice and simple skew factor for 3D
        const int i = FASTFLOOR(xin + s);
        const int j = FASTFLOOR(yin + s);
        const int k = FASTFLOOR(zin + s);
        const double G3 = 1.0 / 6.0; // Very nice and simple unskew factor, too
        const double t = (i + j + k) * G3;
        const double X0 = i - t; // Unskew the cell origin back to (x,y,z) space
        const double Y0 = j - t;
        const double Z0 = k - t;
        const double x0 = xin - X0; // The x,y,z distances from the cell origin
        const double y0 = yin - Y0;
        const double z0 = zin - Z0;
        // For the 3D case, the simplex shape is a slightly irregular tetrahedron.
        // Determine which simplex we are in.
        int i1, j1, k1; // Offsets for second corner of simplex in (i,j,k) coords
        int i2, j2, k2; // Offsets for third corner of simplex in (i,j,k) coords
        if(x0 >= y0)
        {
            if(y0 >= z0)
            {
                i1 = 1; // X Y Z order
                j1 = 0;
                k1 = 0;
                i2 = 1;
                j2 = 1;
                k2 = 0;
            }
            else if(x0 >= z0)
            {
                i1 = 1; // X Z Y order
                j1 = 0;
                k1 = 0;
                i2 = 1;
                j2 = 0;
                k2 = 1;
            }
            else
            {
                i1 = 0; // Z X Y order
                j1 = 0;
                k1 = 1;
                i2 = 1;
                j2 = 0;
                k2 = 1;
            }
        }
        else // x0<y0
        {
            if(y0 < z0)
            {
                i1 = 0; // Z Y X order
                j1 = 0;
                k1 = 1;
                i2 = 0;
                j2 = 1;
                k2 = 1;
            }
            else if(x0 < z0)
            {
                i1 = 0; // Y Z X order
                j1 = 1;
                k1 = 0;
                i2 = 0;
                j2 = 1;
                k2 = 1;
            }
            else
            {
                i1 = 0; // Y X Z order
                j1 = 1;
                k1 = 0;
                i2 = 1;
                j2 = 1;
                k2 = 0;
            }
        }
        //  A step of (1,0,0) in (i,j,k) means a step of (1-c,-c,-c) in (x,y,z),
        //  a step of (0,1,0) in (i,j,k) means a step of (-c,1-c,-c) in (x,y,z), and
        //  a step of (0,0,1) in (i,j,k) means a step of (-c,-c,1-c) in (x,y,z), where
        //  c = 1/6.
        const double x1 = x0 - i1 + G3; // Offsets for second corner in (x,y,z) coords
        const double y1 = y0 - j1 + G3;
        const double z1 = z0 - k1 + G3;
        const double x2 = x0 - i2 + 2.0 * G3; // Offsets for third corner in (x,y,z) coords
        const double y2 = y0 - j2 + 2.0 * G3;
        const double z2 = z0 - k2 + 2.0 * G3;
        const double x3 = x0 - 1.0 + 3.0 * G3; // Offsets for last corner in (x,y,z) coords
        const double y3 = y0 - 1.0 + 3.0 * G3;
        const double z3 = z0 - 1.0 + 3.0 * G3;
        // Work out the hashed gradient indices of the four simplex corners
        const int ii = i & 255;
        const int jj = j & 255;
        const int kk = k & 255;
        const int gi0 = perm[ii + perm[jj + perm[kk]]] % 12;
        const int gi1 = perm[ii + i1 + perm[jj + j1 + perm[kk + k1]]] % 12;
        const int gi2 = perm[ii + i2 + perm[jj + j2 + perm[kk + k2]]] % 12;
        const int gi3 = perm[ii + 1 + perm[jj + 1 + perm[kk + 1]]] % 12;
        // Calculate the contribution from the four corners
        double t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
        if(t0 < 0)
            n0 = 0.0;
        else
        {
            t0 *= t0;
            n0 = t0 * t0 * dot(grad3[gi0], x0, y0, z0);
        }
        double t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
        if(t1 < 0)
            n1 = 0.0;
        else
        {
            t1 *= t1;
            n1 = t1 * t1 * dot(grad3[gi1], x1, y1, z1);
        }
        double t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
        if(t2 < 0)
            n2 = 0.0;
        else
        {
            t2 *= t2;
            n2 = t2 * t2 * dot(grad3[gi2], x2, y2, z2);
        }
        double t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
        if(t3 < 0)
            n3 = 0.0;
        else
        {
            t3 *= t3;
            n3 = t3 * t3 * dot(grad3[gi3], x3, y3, z3);
        }
        // Add contributions from each corner to get the final noise value.
        // The result is scaled to stay just inside [-1,1]
        return 32.0 * (n0 + n1 + n2 + n3);
    }

    double simplex_2d_noise(double x, double y, uint32_t octaves, double z)
    {
        double total = 0;

        // parametrization of octaves to match power spectrum of real grain scans
        static double f[] = {0.4910, 0.9441, 1.7280};
        static double a[] = {0.2340, 0.7850, 1.2150};

        for(uint32_t o = 0; o < octaves; o++)
        {
            total += (simplex_noise(x * f[o] / z, y * f[o] / z, o) * a[o]);
        }
        return total;
    }

    float paper_resp(float exposure, float mb, float gp, float divgr)
    {   
        float dived = 1.f;
        if(divgr > 1.8f) {
            dived = 1.f + (divgr - 1.8f);
        }
        const float delta = dived * GRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(GRAIN_LUT_DELTA_MIN / dived));
        const float density = (1.0f + 2.0f * delta) / (1.0f + expf( (4.0f * gp * (0.5f - exposure)) / (1.0f + 2.0f * delta) )) - delta;
        return density;
    }

    float paper_resp_inverse(float density, float mb, float gp, float divgr)
    {
        float dived = 1.f;
        if(divgr > 1.8f) {
            dived = 1.f + (divgr - 1.8f);
        }
        const float delta =  dived * GRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(GRAIN_LUT_DELTA_MIN / dived));
        const float exposure = -logf((1.0f + 2.0f * delta) / (density + delta) - 1.0f) * (1.0f + 2.0f * delta) / (4.0f * gp) + 0.5f;
        return exposure;
    }

    void evaluate_grain_lut(const float mb, float divgr)
    {
        for(int i = 0; i < GRAIN_LUT_SIZE; i++)
        {
            for(int j = 0; j < GRAIN_LUT_SIZE; j++)
            {
                float gu = (float)i / (GRAIN_LUT_SIZE - 1) - 0.5;
                float l = (float)j / (GRAIN_LUT_SIZE - 1);
                float divg = divgr; //1.f
                grain_lut[j * GRAIN_LUT_SIZE + i] = 32768.f * (paper_resp(gu + paper_resp_inverse(l, mb, divg * GRAIN_LUT_PAPER_GAMMA, divgr), mb, divg * GRAIN_LUT_PAPER_GAMMA, divgr) - l);
            }
        }
    }

    float lut_lookup(const float x, const float y)
    {
        const float _x = LIM((x + 0.5f) * (GRAIN_LUT_SIZE - 1), 0.f, float(GRAIN_LUT_SIZE - 1));
        const float _y = LIM(y * (GRAIN_LUT_SIZE - 1), 0.f, float(GRAIN_LUT_SIZE - 1));

        const int _x0 = _x < GRAIN_LUT_SIZE - 2 ? _x : GRAIN_LUT_SIZE - 2;
        const int _y0 = _y < GRAIN_LUT_SIZE - 2 ? _y : GRAIN_LUT_SIZE - 2;

        const int _x1 = _x0 + 1;
        const int _y1 = _y0 + 1;

        const float x_diff = _x - _x0;
        const float y_diff = _y - _y0;

        const float l00 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x0];
        const float l01 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x1];
        const float l10 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x0];
        const float l11 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x1];

        const float xy0 = (1.0 - y_diff) * l00 + l10 * y_diff;
        const float xy1 = (1.0 - y_diff) * l01 + l11 * y_diff;
        return xy0 * (1.0f - x_diff) + xy1 * x_diff;
    }

    int ox;
    int oy;
    int fw;
    int fh;
    double scale;
    int perm[512];
    float grain_lut[GRAIN_LUT_SIZE * GRAIN_LUT_SIZE];
};

// ---------------------------------------------------------------------------
// Grain V3: silver clumps and dye clouds, not value noise.
//
// A developed emulsion is a random field of opaque clumps (silver) or soft
// clouds (dye), each a few micrometres across, scattered with Poisson-like
// irregularity and gathering into larger clumps. What a scan or print shows
// is that field integrated by the sampling aperture. This renderer builds it
// literally: on a jittered lattice whose pitch is the grain diameter in
// image pixels (from the film format, the ISO and the Size slider), each
// cell holds a couple of soft blobs with random position, size and signed
// density; a second lattice 2.7x coarser adds the clumping. The blobs are
// splatted into the image, so the cost is a few additions per pixel and the
// pattern is anchored to full-image coordinates: the detail window, the main
// preview and the export show the same grains in the same places.
//
// Previews integrate: an output pixel spanning s full-resolution pixels
// widens every blob by the pixel's own footprint and scales it to keep its
// integral, and once a pixel is larger than a grain the lattice itself
// coarsens with the density of blobs reduced accordingly (Selwyn's law), so
// a zoomed-out view shows the fine, quiet noise a real scan would, never a
// blow-up of the texture.
//
// The field is then applied as density: luminance is multiplied by
// exp(noise * sigma), with sigma following where real granularity lives in
// the tone scale (lighter mid-tones for a negative, whose dense areas print
// as highlights; shadows for reversal film), fading at paper white and in
// the empty shadows of a thin negative. Colour grain is the same field
// built once per dye layer, coarser, with the blue layer coarsest.
// ---------------------------------------------------------------------------
namespace grainv3 {

inline uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

inline uint32_t cellHash(int32_t x, int32_t y, uint32_t k, uint32_t seed)
{
    return mix(uint32_t(x) * 0x9E3779B1U ^ mix(uint32_t(y) * 0x85EBCA77U ^ (k * 0xC2B2AE3DU) ^ seed));
}

inline float unitFloat(uint32_t h)
{
    return (h >> 8) * (1.f / 16777216.f);
}

constexpr int GRAINS_PER_CELL = 2;

struct Lattice {
    double cell;     // pitch in full-resolution pixels
    double amp;      // amplitude multiplier
    uint32_t seed;
};

// Accumulate one lattice's blobs over a band of output rows [y0, y0 + rows).
// Output pixel (i, j) sits at full-resolution (ox + (i + .5) * s, oy + (j + .5) * s).
void splat(float* out, int W, int y0, int rows, const Lattice& lattice, double s, double ox, double oy)
{
    const double cell = lattice.cell;
    const double widen = s * s * .25;              // the output pixel's own footprint
    const double reach = std::sqrt(cell * cell + widen);
    const double bandY0 = oy + y0 * s, bandY1 = oy + (y0 + rows) * s;
    const int32_t gy0 = int32_t(std::floor((bandY0 - reach) / cell));
    const int32_t gy1 = int32_t(std::floor((bandY1 + reach) / cell));
    const int32_t gx0 = int32_t(std::floor((ox - reach) / cell));
    const int32_t gx1 = int32_t(std::floor((ox + W * s + reach) / cell));

    for (int32_t gy = gy0; gy <= gy1; ++gy) {
        for (int32_t gx = gx0; gx <= gx1; ++gx) {
            for (uint32_t k = 0; k < GRAINS_PER_CELL; ++k) {
                const uint32_t h0 = cellHash(gx, gy, k, lattice.seed);
                const uint32_t h1 = mix(h0 ^ 0x68E31DA4U);
                const uint32_t h2 = mix(h0 ^ 0xB5297A4DU);
                const uint32_t h3 = mix(h0 ^ 0x1B56C4E9U);
                const float u0 = unitFloat(h0), u1 = unitFloat(h1), u2 = unitFloat(h2), u3 = unitFloat(h3);
                const double px = (gx + u0) * cell, py = (gy + u1) * cell;
                // Diameter spread: most grains near the nominal size, a few
                // large ones. Bounded at one cell so neighbouring cells suffice.
                const double rad = cell * (.5 + .5 * u2 * u2);
                // Signed density: triangular, slightly heavy toward dense
                // clumps, which is what real granularity histograms show.
                const float t = u3 * 2.f - 1.f;
                const float amp = float(lattice.amp) * (t + .35f * t * std::fabs(t));
                const double r2 = rad * rad + widen;
                const double rr = std::sqrt(r2);
                const float keep = float(rad * rad / r2);   // integral preserved when widened
                const int i0 = std::max(0, int(std::ceil((px - rr - ox) / s - .5)));
                const int i1 = std::min(W - 1, int(std::floor((px + rr - ox) / s - .5)));
                const int j0 = std::max(y0, int(std::ceil((py - rr - oy) / s - .5)));
                const int j1 = std::min(y0 + rows - 1, int(std::floor((py + rr - oy) / s - .5)));
                if (i0 > i1 || j0 > j1) continue;
                const float inv = float(1. / r2);
                for (int j = j0; j <= j1; ++j) {
                    const double dy = oy + (j + .5) * s - py;
                    float* row = out + size_t(j - y0) * W;
                    for (int i = i0; i <= i1; ++i) {
                        const double dx = ox + (i + .5) * s - px;
                        const float q = 1.f - float(dx * dx + dy * dy) * inv;
                        if (q > 0.f) row[i] += amp * keep * q * q;
                    }
                }
            }
        }
    }
}

// Standard deviation of a unit-amplitude lattice at full resolution, so the
// field can be expressed in sigma units. The field is self-similar, so one
// measurement serves every cell size.
float unitSigma()
{
    static const float sigma = [] {
        const int n = 192;
        std::vector<float> patch(size_t(n) * n, 0.f);
        splat(patch.data(), n, 0, n, {6., 1., 0x51A7F00DU}, 1., 0., 0.);
        double sum = 0, sq = 0;
        for (float v : patch) { sum += v; sq += double(v) * v; }
        const double mean = sum / patch.size();
        return float(std::sqrt(std::max(1e-12, sq / patch.size() - mean * mean)));
    }();
    return sigma;
}

// Where in the tone scale the granularity lives: a hump centred on the
// stock's density peak, wider toward black than toward white, with a floor
// so the emptiest shadows are not glassily clean.
inline float granularity(float lstar, float peak)
{
    const float d = lstar - peak;
    const float width = d < 0.f ? .40f : .22f;
    const float hump = std::exp(-d * d / (width * width));
    const float paperWhite = 1.f - .85f * LIM((lstar - .88f) / .12f, 0.f, 1.f);
    return (.14f + .86f * hump) * paperWhite;
}

inline float labToY(float L)
{
    const float fy = (L * (100.f / 32768.f) + 16.f) / 116.f;
    return fy > 6.f / 29.f ? fy * fy * fy : 3.f * (6.f / 29.f) * (6.f / 29.f) * (fy - 4.f / 29.f);
}

inline float yToLab(float Y)
{
    const float fy = Y > 216.f / 24389.f ? std::cbrt(Y) : (24389.f / 27.f * Y + 16.f) / 116.f;
    return (116.f * fy - 16.f) * (32768.f / 100.f);
}

} // namespace grainv3

} // namespace


void ImProcFunctions::filmGrain(Imagefloat *rgb, int isogr, int strengr, int scalegr, float divgr, int bfw, int bfh, int call, int fw, int fh)
{
    if (settings->verbose) {
        printf("iso=%i strength=%i scale=%i gamma=%f\n", isogr, strengr, scalegr, divgr);
    }

    GrainEvaluator ge(0, 0, bfw, bfh, scale, divgr, call, fw, fh);
    ge(isogr, strengr, scalegr, divgr, rgb, multiThread, call, fw, fh);
}

void ImProcFunctions::filmGrainV2(Imagefloat *lab, int isogr, int strengr, int scalegr, int colorgr,
                                  int originX, int originY, double oscale, int fullW, int fullH,
                                  const GrainCharacter &character)
{
    using namespace grainv3;
    const int W = lab->getWidth(), H = lab->getHeight();
    if (W <= 0 || H <= 0 || strengr <= 0) return;
    const double s = std::max(oscale, 1.);
    const double wd = std::max(1, std::min(fullW, fullH));

    // Grain diameter: micrometres on a 35 mm frame (24 mm short side), from
    // the ISO slider. Fine-grained slow film near 7 um, fast film past 30.
    const double micrometres = (6. + 2.6 * std::pow(std::max(isogr, 20) / 100., .6))
                               * (std::max(scalegr, 10) / 100.) * std::max(.25f, character.sizeMul);
    const double cell = std::max(.75, micrometres * wd / 24000.);
    const float sigmaUnit = unitSigma();

    // Once an output pixel outspans a grain the lattice follows the pixel
    // and the blob density drops with it: Selwyn's law for the integrated
    // texture, applied to how the field is built rather than as a fudge on
    // the result.
    const auto lattice = [&](double pitch, double amplitude, uint32_t seed) {
        const double effective = std::max(pitch, s * .5);
        return Lattice{effective, amplitude * (pitch / effective) / sigmaUnit, seed};
    };
    const float clumping = LIM(character.clumping, 0.f, 1.f);
    const double norm = 1. / std::sqrt(1. + double(clumping) * clumping);
    const Lattice fine = lattice(cell, norm, 0x12345678U);
    const Lattice clump = lattice(cell * 2.7, norm * clumping, 0x9ABCDEF0U);
    const bool colour = colorgr > 0 && !character.mono && character.chroma > 0.f;
    // Dye clouds are larger than silver grains, the blue-sensitive layer's
    // the largest; the colour lattices stay independent of the density one.
    const Lattice dyeA = lattice(cell * 1.6, 1., 0x56789ABCU);
    const Lattice dyeB = lattice(cell * 2.0, 1., 0x2468ACE0U);

    const float strength = strengr / 100.f;
    const float sigma = strength * .26f * std::max(.1f, character.gain);
    const float chromaAmp = strength * (colorgr / 100.f) * LIM(character.chroma, 0.f, 3.f) * 2400.f;
    const float peak = LIM(character.densityPeak, .1f, .9f);

    float **lab_L = lab->g.ptrs;
    float **lab_a = lab->r.ptrs;
    float **lab_b = lab->b.ptrs;
    constexpr int BAND = 32;
    const int bands = (H + BAND - 1) / BAND;
    const int fields = colour ? 4 : 2;

#ifdef _OPENMP
#   pragma omp parallel if (multiThread)
#endif
    {
        std::vector<float> buffer(size_t(fields) * BAND * W);
#ifdef _OPENMP
#       pragma omp for schedule(dynamic)
#endif
        for (int band = 0; band < bands; ++band) {
            const int y0 = band * BAND, rows = std::min(BAND, H - y0);
            std::fill(buffer.begin(), buffer.end(), 0.f);
            float* density = buffer.data();
            float* dyeFieldA = density + size_t(BAND) * W;
            float* dyeFieldB = dyeFieldA + size_t(BAND) * W;
            splat(density, W, y0, rows, fine, s, originX, originY);
            splat(density, W, y0, rows, clump, s, originX, originY);
            if (colour) {
                splat(dyeFieldA, W, y0, rows, dyeA, s, originX, originY);
                splat(dyeFieldB, W, y0, rows, dyeB, s, originX, originY);
            }
            for (int j = y0; j < y0 + rows; ++j) {
                const float* d = density + size_t(j - y0) * W;
                const float* ca = dyeFieldA + size_t(j - y0) * W;
                const float* cb = dyeFieldB + size_t(j - y0) * W;
                for (int i = 0; i < W; ++i) {
                    const float L = lab_L[j][i];
                    const float lstar = LIM(L / 32768.f, 0.f, 1.2f);
                    const float g = granularity(lstar, peak);
                    const float Y = labToY(L);
                    if (Y > 0.f) {
                        // Density: the grain multiplies light, it does not add to it.
                        const float n = LIM(d[i], -4.f, 4.f);
                        lab_L[j][i] = yToLab(Y * std::exp(n * sigma * g));
                    }
                    if (colour) {
                        // Chroma clouds ride the same granularity but keep
                        // out of the deepest shadows, where a print holds no colour.
                        const float c = chromaAmp * g * (.35f + .65f * std::sqrt(lstar));
                        lab_a[j][i] += LIM(ca[i], -4.f, 4.f) * c;
                        lab_b[j][i] += LIM(cb[i], -4.f, 4.f) * c * .9f;
                    }
                }
            }
        }
    }
}

} // namespace rtengine
