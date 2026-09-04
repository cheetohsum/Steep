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

// Shared blend and gate math for the Double Exposure tool. The engine
// composite (white = 65535) and the picker dialog's approximate preview
// (white = 1.0) both consume these helpers, so a mode added or changed here
// exists everywhere at once and the two renders cannot drift.

#include <algorithm>
#include <cmath>

#include "procparams.h"

namespace rtengine
{

namespace deblend
{

using BlendMode = procparams::DoubleExposureParams::BlendMode;
using Compare = procparams::DoubleExposureParams::Compare;

inline float clamp01w(float v, float white)
{
    return std::min(std::max(v, 0.f), white);
}

inline float smoothstep01(float t)
{
    t = std::min(std::max(t, 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
}

inline float lum709(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Comparative bright / dark the way cameras do it: the two pixels are
// compared by brightness and the winner is kept whole, so no channel is
// ever taken from the loser and no colour appears that neither frame held.
// `softnessStops` is the width of the hand-over band in stops: 0 is a hard
// per-pixel pick (ties keep the base), around half a stop mixes the two
// colours as their brightnesses approach a tie, which is what Canon's manual
// describes for its Bright / Dark modes. Returns the fraction of the partner
// pixel to take.
inline float compareWeight(float lumBase, float lumPartner, bool keepBrighter, float softnessStops, float white)
{
    const float eps = 1e-4f * white;
    float d = std::log2((std::max(lumPartner, 0.f) + eps) / (std::max(lumBase, 0.f) + eps)); // > 0: partner brighter

    if (!keepBrighter) {
        d = -d;
    }

    if (softnessStops <= 0.f) {
        return d > 0.f ? 1.f : 0.f;
    }

    return smoothstep01(0.5f + d / softnessStops);
}

// Composite one partner sample (pr, pg, pb) onto the accumulated base
// (r, g, b), both scene-linear on a 0..white scale. Returns the un-weighted
// blend result; opacity/gate weighting is applied by the caller.
inline void blend(BlendMode mode, Compare compare, float softnessStops, float white,
                  float r, float g, float b,
                  float pr, float pg, float pb,
                  float& cr, float& cg, float& cb)
{
    switch (mode) {
        case BlendMode::SCREEN: {
            cr = white * (1.f - (1.f - clamp01w(r, white) / white) * (1.f - clamp01w(pr, white) / white));
            cg = white * (1.f - (1.f - clamp01w(g, white) / white) * (1.f - clamp01w(pg, white) / white));
            cb = white * (1.f - (1.f - clamp01w(b, white) / white) * (1.f - clamp01w(pb, white) / white));
            break;
        }

        case BlendMode::MULTIPLY: {
            cr = std::max(r, 0.f) * std::max(pr, 0.f) / white;
            cg = std::max(g, 0.f) * std::max(pg, 0.f) / white;
            cb = std::max(b, 0.f) * std::max(pb, 0.f) / white;
            break;
        }

        case BlendMode::LIGHTEN:
        case BlendMode::DARKEN: {
            const bool keepBrighter = mode == BlendMode::LIGHTEN;

            if (compare == Compare::CHANNEL) {
                // Legacy per-channel pick: fringes where the winner flips
                // between channels, kept for files that were tuned on it.
                if (keepBrighter) {
                    cr = std::max(r, pr);
                    cg = std::max(g, pg);
                    cb = std::max(b, pb);
                } else {
                    cr = std::min(r, pr);
                    cg = std::min(g, pg);
                    cb = std::min(b, pb);
                }
            } else {
                const float w = compareWeight(lum709(r, g, b), lum709(pr, pg, pb), keepBrighter, softnessStops, white);
                cr = r + w * (pr - r);
                cg = g + w * (pg - g);
                cb = b + w * (pb - b);
            }

            break;
        }

        case BlendMode::ABSDIFF: {
            cr = std::fabs(r - pr);
            cg = std::fabs(g - pg);
            cb = std::fabs(b - pb);
            break;
        }

        case BlendMode::ADD:
        default: {
            cr = r + pr;
            cg = g + pg;
            cb = b + pb;
            break;
        }
    }
}

// Gate thresholds are perceptual: a window of 0..0.35 means "the darkest
// ~35% of the tone scale as a viewer judges it". Scene-linear luminance is
// sRGB-encoded before the window is evaluated, so the from/to sliders track
// what is on screen instead of raw linear energy (where 10% is already a
// light midtone and a "shadows" window would swallow most of the image).
inline float gateEncode(float lum01)
{
    lum01 = std::min(std::max(lum01, 0.f), 1.f);
    return lum01 <= 0.0031308f ? lum01 * 12.92f : 1.055f * std::pow(lum01, 1.f / 2.4f) - 0.055f;
}

// The "Reveal in" window: 1 inside [low, high], smoothstep falloff over
// `feather` on each side. All arguments are 0..1 encoded luminance.
// The legacy fill-shadows gate maps to the window (0, 0.35, feather 0.33).
inline float gateWindow(float lum, float low, float high, float feather)
{
    if (lum < low) {
        if (feather <= 0.f) {
            return 0.f;
        }

        const float t = (lum - (low - feather)) / feather;

        if (t <= 0.f) {
            return 0.f;
        }

        return t < 1.f ? t * t * (3.f - 2.f * t) : 1.f;
    }

    if (lum > high) {
        if (feather <= 0.f) {
            return 0.f;
        }

        const float t = ((high + feather) - lum) / feather;

        if (t <= 0.f) {
            return 0.f;
        }

        return t < 1.f ? t * t * (3.f - 2.f * t) : 1.f;
    }

    return 1.f;
}

// Strength dilutes the gate rather than the layer: at 0 the window is
// ignored, at 1 the layer only lands inside it.
inline float gateWeight(float strength, float window)
{
    return (1.f - strength) + strength * window;
}

// Highlight latitude: the emulsion's shoulder, applied to the finished stack.
// Light adds linearly on film and then the dense areas stop registering
// more of it; that compression is what keeps a second frame out of a bright
// sky and is the whole silhouette look. Latitude 0..1 sets the knee at
// 1 - 0.5 * latitude (in white units); below the knee the stack passes
// through, above it a C1-continuous rational roll-off approaches white
// asymptotically, so the composite never leaves [0, white] and never relies
// on the downstream tone curve clipping it. Applied per channel on purpose:
// film's layers saturate independently. Callers reference `white` to ONE
// frame's white (i.e. they undo the auto film gain around the call), so a
// metered-down average still shoulders where a frame was bright.
inline float latitudeKnee(float latitude01)
{
    return 1.f - 0.5f * std::min(std::max(latitude01, 0.f), 1.f);
}

inline float shoulder(float x01, float knee)
{
    if (x01 <= knee) {
        return x01;
    }

    const float t = x01 - knee;
    const float range = 1.f - knee;
    return knee + range * t / (t + range);
}

} // namespace deblend

// Where a base pixel lands in the partner. This is the ONLY copy of that
// map: the engine composite and the picker dialog's preview both call it, so
// a placed, rotated or tiled layer cannot look one way in the picker and
// another in the render — the two bugs this tool has produced most.
//
// The map is unit-agnostic: only the ratios between the base frame and the
// source rect matter, so the engine can work in partner pixels while the
// dialog works with the base width normalised to 1, and neither has to
// convert. Rotation is a real rotation in whichever units the caller uses,
// which is why the caller must pass a *physical* frame size (pixels, or a
// width-normalised size that accounts for the aspect) rather than a unit
// square.
namespace deplace
{

using Pattern = procparams::DoubleExposureParams::Pattern;

struct Frame {
    // Base frame and source rect, in the caller's units.
    float baseW = 1.f;
    float baseH = 1.f;
    float srcX0 = 0.f;   // the source rect is the whole partner frame unless
    float srcY0 = 0.f;   // the layer is cropped to its subject
    float srcW = 1.f;
    float srcH = 1.f;
    float invCover = 1.f; // 1 / max(baseW / srcW, baseH / srcH)
    float scale = 1.f;    // 1 = cover fit
    float offX = 0.f;     // centre shift, fraction of the base frame
    float offY = 0.f;
    float cosA = 1.f;     // rotation of the layer, as its inverse
    float sinA = 0.f;
    float flip = 1.f;     // -1 mirrors the layer horizontally
    Pattern pattern = Pattern::OFF;
    float cell = 1.f;     // tile pitch in source-rect widths; > 1 leaves a gutter
    float stagger = 0.f;  // odd-row shift, fraction of a tile
    int count = 6;        // RADIAL: copies around the ring
    float ring = 0.f;     // RADIAL: ring radius, in source-rect units
    bool upright = false; // RADIAL: leave the copies standing as the picture does
    float twist = 0.f;    // RADIAL: radians added per copy round the ring
    // RADIAL: how much of a half-wedge the hand-over between neighbouring
    // copies is spread over. 0 partitions the plane hard, which shows as a
    // straight line radiating from the centre wherever the two copies differ.
    float handover = 0.f;
    // Soft frame edge, in source-rect units. 0 keeps the old hard edge, where
    // coverage only ever ramps across the single output pixel that straddles
    // the boundary.
    float edge = 0.f;
    // False keeps the legacy path bitwise: no rotation, no wrap, no frame
    // edge — the partner is simply edge-clamped over the whole base, which
    // is how every layer behaved before placement existed.
    bool placed = false;
};

// True when the cell index is odd, for either sign. Mirror tiling reflects
// odd cells so neighbours meet on the same source edge.
inline bool oddCell(float cellIndex)
{
    return (static_cast<long long>(cellIndex) & 1LL) != 0LL;
}

// Fills in everything the layer's own parameters decide. The caller still
// supplies the two frame geometries, which only it knows.
inline void applyLayer(Frame& f, const procparams::DoubleExposureParams::Layer& layer, bool cropped)
{
    constexpr float degToRad = 3.14159265358979323846f / 180.f;

    f.offX = static_cast<float>(layer.offsetX) / 100.f;
    f.offY = static_cast<float>(layer.offsetY) / 100.f;
    f.scale = std::max(0.01f, static_cast<float>(layer.scale) / 100.f);

    const float angle = static_cast<float>(layer.rotate) * degToRad;
    f.cosA = std::cos(angle);
    f.sinA = std::sin(angle);
    f.flip = layer.flipH ? -1.f : 1.f;

    f.pattern = layer.pattern;
    // Negative spacing draws the tiles closer than their own frames, so they
    // overlap; the pitch is floored well above zero so a cell always has room
    // to hold something.
    f.cell = std::max(0.1f, 1.f + static_cast<float>(layer.patternSpacing) / 100.f);
    f.stagger = std::min(std::max(static_cast<float>(layer.patternStagger) / 100.f, 0.f), 1.f);

    // The ring is measured on the base frame, like every other placement
    // control, then carried into source-rect units the same way a position is.
    // The caller must have filled in the two frame geometries by now.
    f.count = std::min(std::max(static_cast<int>(std::lround(layer.patternCount)), 1), 24);
    f.ring = 0.5f * static_cast<float>(layer.patternDiameter) / 100.f * f.baseW * f.invCover / f.scale;

    // A soft edge is what stops a copy ending on a line, but edge-to-edge
    // tiling has no outer edge to soften: every pixel there belongs to some
    // cell, so fading the cell boundary would let the base through as a seam
    // -- exactly the artefact this is meant to remove. So it applies to a
    // placed frame, to tiles with a gutter between them, and to the radial
    // copies, and never to a seamless grid.
    const bool seamlessGrid = (layer.pattern == Pattern::REPEAT || layer.pattern == Pattern::MIRROR)
                              && f.cell <= 1.f;
    const float blend01 = std::min(std::max(static_cast<float>(layer.edgeFeather), 0.f), 100.f) / 100.f;
    f.edge = seamlessGrid ? 0.f : blend01 * 0.25f * std::min(f.srcW, f.srcH);

    // Radial copies do not have an edge against the base where they meet each
    // other -- they have a boundary, and the sample jumps across it. Edge
    // blend widens that into a hand-over: half the wedge at full strength.
    f.handover = 0.5f * blend01;
    f.upright = layer.patternUpright;
    f.twist = static_cast<float>(layer.patternTwist) * degToRad;

    // A plain mirror still covers the base exactly, so it alone does not need
    // the placed path; everything else moves the frame's edges into view.
    f.placed = f.offX != 0.f || f.offY != 0.f || f.scale != 1.f
               || layer.rotate != 0.0 || f.pattern != Pattern::OFF || cropped;
}

// Where one base pixel reads from. Radial copies can need two: the layer is
// sampled once per copy, and near the boundary between two of them the pair
// is mixed rather than one being picked outright.
struct Placed {
    float u = 0.f;
    float v = 0.f;
    float coverage = 0.f;
    float u2 = 0.f;         // the neighbouring radial copy, when mix > 0
    float v2 = 0.f;
    float coverage2 = 0.f;
    float mix = 0.f;        // how much of that neighbour to take, 0..0.5
    bool present = false;
};

// One radial copy: it stands at `ring` along its own spoke, turned by
// `orient` — the spoke itself when the copies face outward, nothing when they
// are kept upright, plus the twist accumulated by its place round the ring.
// Both modes are the same expression, which is why they cannot drift apart.
inline void placeCopy(const Frame& f, float copyIndex, float step, float& su, float& sv)
{
    const float spoke = copyIndex * step;
    const float cs = std::cos(spoke);
    const float sn = std::sin(spoke);

    // Numbered 0..count-1 round the ring, so the twist accumulates in a fixed
    // order instead of jumping where atan2 wraps.
    float ordinal = std::fmod(copyIndex, static_cast<float>(f.count));

    if (ordinal < 0.f) {
        ordinal += static_cast<float>(f.count);
    }

    const float orient = (f.upright ? 0.f : spoke) + ordinal * f.twist;
    const float oc = std::cos(orient);
    const float os = std::sin(orient);

    const float px = su - f.ring * cs;
    const float py = sv - f.ring * sn;
    su = oc * px + os * py;
    sv = oc * py - os * px;
}

// Frame-edge coverage for a point already reduced to source coordinates.
inline float frameCoverage(const Frame& f, float u, float v, float aa)
{
    const float ex = std::min(u - f.srcX0, f.srcX0 + f.srcW - u);
    const float ey = std::min(v - f.srcY0, f.srcY0 + f.srcH - v);
    const float dist = std::min(ex, ey);

    if (f.edge > aa) {
        // Smoothstep over the blend band: a linear ramp this wide reads as a
        // visible gradient wedge, an S-curve as the frame simply thinning out.
        return deblend::smoothstep01(dist / f.edge);
    }

    return std::min(std::max(dist / aa + 0.5f, 0.f), 1.f);
}

// Maps base-frame point (fx, fy) to source coordinates (u, v). `aaStep` is
// the size of one output pixel in base units, which sets the width of the
// anti-aliased frame edge. Returns false where the layer is absent.
inline bool map(const Frame& f, float fx, float fy, float aaStep,
                float& u, float& v, float& coverage)
{
    float dx = fx - f.baseW * (0.5f + f.offX);
    float dy = fy - f.baseH * (0.5f + f.offY);

    if (f.sinA != 0.f) {
        const float qx = f.cosA * dx + f.sinA * dy;
        const float qy = f.cosA * dy - f.sinA * dx;
        dx = qx;
        dy = qy;
    }

    // Written in this order so an unrotated, unplaced layer reproduces the
    // original expression bit for bit.
    float su = f.flip * dx * f.invCover / f.scale;
    float sv = dy * f.invCover / f.scale;

    coverage = 1.f;

    if (!f.placed) {
        u = su + f.srcX0 + f.srcW * 0.5f;
        v = sv + f.srcY0 + f.srcH * 0.5f;
        return true;
    }

    if (f.pattern == Pattern::RADIAL) {
        // N copies stood around a ring, each turned to face outward. A point
        // belongs to the copy whose spoke it is nearest to — the copies are
        // equidistant from the centre, so that is also the nearest copy.
        constexpr float twoPi = 6.28318530717958647692f;
        const float step = twoPi / static_cast<float>(f.count);
        const float k = std::atan2(sv, su) / step;
        const float k0 = std::floor(k + 0.5f);
        placeCopy(f, k0, step, su, sv);
    } else if (f.pattern != Pattern::OFF) {
        const float cw = f.srcW * f.cell;
        const float ch = f.srcH * f.cell;

        float tx = su / cw;
        const float ty = sv / ch;
        const float row = std::floor(ty + 0.5f);

        if (f.stagger != 0.f && oddCell(row)) {
            tx += f.stagger;
        }

        const float col = std::floor(tx + 0.5f);
        float rx = tx - col;
        float ry = ty - row;

        if (f.pattern == Pattern::MIRROR) {
            if (oddCell(col)) {
                rx = -rx;
            }

            if (oddCell(row)) {
                ry = -ry;
            }
        }

        su = rx * cw;
        sv = ry * ch;

        if (f.twist != 0.f) {
            // Counted outward in rings from the middle tile, so the twist
            // grows as the pattern echoes out rather than running along the
            // rows. Turned about the tile's own centre, in source units, or
            // an oblong tile would shear instead of turning.
            const float ring = std::max(std::fabs(col), std::fabs(row));
            const float orient = ring * f.twist;
            const float oc = std::cos(orient);
            const float os = std::sin(orient);
            const float px = su;
            const float py = sv;
            su = oc * px + os * py;
            sv = oc * py - os * px;
        }
    }

    u = su + f.srcX0 + f.srcW * 0.5f;
    v = sv + f.srcY0 + f.srcH * 0.5f;

    const float aa = std::max(aaStep * f.invCover / f.scale, 1e-6f);
    coverage = frameCoverage(f, u, v, aa);

    return coverage > 0.f;
}

// The same map, plus the neighbouring radial copy where one is being handed
// over to. Every caller should use this; the three-output form above is what
// it is built on and stays for the cases that cannot take two samples.
inline Placed place(const Frame& f, float fx, float fy, float aaStep)
{
    Placed out;
    out.present = map(f, fx, fy, aaStep, out.u, out.v, out.coverage);

    if (f.pattern != Pattern::RADIAL || f.handover <= 0.f || f.count < 2) {
        return out;
    }

    // How far into its own wedge this point sits: 0 on the spoke, 0.5 at the
    // boundary with the next copy.
    constexpr float twoPi = 6.28318530717958647692f;
    const float step = twoPi / static_cast<float>(f.count);

    float dx = fx - f.baseW * (0.5f + f.offX);
    float dy = fy - f.baseH * (0.5f + f.offY);

    if (f.sinA != 0.f) {
        const float qx = f.cosA * dx + f.sinA * dy;
        const float qy = f.cosA * dy - f.sinA * dx;
        dx = qx;
        dy = qy;
    }

    const float su = f.flip * dx * f.invCover / f.scale;
    const float sv = dy * f.invCover / f.scale;
    const float k = std::atan2(sv, su) / step;
    const float k0 = std::floor(k + 0.5f);
    const float frac = std::fabs(k - k0);
    const float t = (0.5f - frac) / f.handover;

    if (t >= 1.f) {
        return out;   // well inside this copy's wedge
    }

    // At the boundary the two copies weigh equally; the mix falls to nothing
    // by the time the hand-over band is crossed.
    out.mix = 0.5f * (1.f - deblend::smoothstep01(t));

    float nu = su;
    float nv = sv;
    placeCopy(f, k0 + (k > k0 ? 1.f : -1.f), step, nu, nv);
    out.u2 = nu + f.srcX0 + f.srcW * 0.5f;
    out.v2 = nv + f.srcY0 + f.srcH * 0.5f;

    const float aa = std::max(aaStep * f.invCover / f.scale, 1e-6f);
    out.coverage2 = frameCoverage(f, out.u2, out.v2, aa);

    if (!out.present && out.coverage2 > 0.f) {
        // The neighbour reaches here even though this copy does not.
        out.present = true;
        out.u = out.u2;
        out.v = out.v2;
        out.coverage = 0.f;
    }

    return out;
}

} // namespace deplace

} // namespace rtengine
