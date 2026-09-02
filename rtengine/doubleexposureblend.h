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

} // namespace rtengine
