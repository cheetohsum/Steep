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

#include <algorithm>
#include <cmath>

#include <glibmm/ustring.h>

#include "rtengine/color.h"
#include "rtengine/iccstore.h"
#include "rtengine/rt_math.h"

// The HSV Equalizer and Point Color run on working-space RGB (Rec2020 by
// default, ProPhoto and others possible), so "hue 0.333" there is the working
// space's green primary, not sRGB green. Swatches painted with plain sRGB HSV
// showed a different colour from the one the engine actually targets. This
// turns a working-space HSV triple into the sRGB colour it really is.
class WorkingHSVDisplay
{
public:
    // Returns true when the profile changed and swatches need repainting.
    bool setProfile(const Glib::ustring& name)
    {
        if (valid_ && name == profile_) {
            return false;
        }

        profile_ = name;
        valid_ = false;
        return true;
    }

    // h, s in 0..1 as the engine uses them. v is display brightness 0..1:
    // the engine's HSV sees linear data, so v is decoded from sRGB and applied
    // as a common scale, which leaves the engine's hue and saturation intact.
    void toDisplay(float h, float s, float v, float& r, float& g, float& b) const
    {
        ensureMatrix();

        float wr, wg, wb;
        rtengine::Color::hsv2rgb01(h, s, 1.f, wr, wg, wb);

        const double k = decode(rtengine::LIM01(static_cast<double>(v)));
        double lin[3];

        for (int i = 0; i < 3; ++i) {
            lin[i] = k * (m_[i][0] * wr + m_[i][1] * wg + m_[i][2] * wb);
        }

        // Wide-gamut primaries fall outside sRGB. Pull toward grey of the same
        // luminance until nothing is negative, then scale down any overshoot,
        // so the hue shown is the hue applied rather than a clipped neighbour.
        const double y = 0.2225045 * lin[0] + 0.7168786 * lin[1] + 0.0606169 * lin[2];
        const double lo = std::min({lin[0], lin[1], lin[2]});

        if (lo < 0.0 && y > 0.0) {
            const double t = y / (y - lo);

            for (double& c : lin) {
                c = y + t * (c - y);
            }
        }

        const double hi = std::max({lin[0], lin[1], lin[2]});

        if (hi > 1.0) {
            for (double& c : lin) {
                c /= hi;
            }
        }

        r = encode(lin[0]);
        g = encode(lin[1]);
        b = encode(lin[2]);
    }

private:
    void ensureMatrix() const
    {
        if (valid_) {
            return;
        }

        const rtengine::ICCStore* store = rtengine::ICCStore::getInstance();
        const rtengine::TMatrix wp = store->workingSpaceMatrix(profile_);
        const rtengine::TMatrix isrgb = store->workingSpaceInverseMatrix("sRGB");

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double sum = 0.0;

                for (int k = 0; k < 3; ++k) {
                    sum += isrgb[i][k] * wp[k][j];
                }

                m_[i][j] = sum;
            }
        }

        valid_ = true;
    }

    static double decode(double c)
    {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    static float encode(double c)
    {
        c = rtengine::LIM01(c);
        return static_cast<float>(c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
    }

    // procparams' default working profile, used until read() says otherwise.
    Glib::ustring profile_ = "Rec2020";
    mutable double m_[3][3] = {};
    mutable bool valid_ = false;
};
