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
#include <cmath>
#include <cstddef>
#include <vector>

#include <glibmm/ustring.h>

#include "color.h"
#include "rt_math.h"

namespace rtengine
{

/**
 * The perceptual black & white conversion: a hue mixer (eight colour bands,
 * a look preset and a lens filter, each as exposure by hue, weighted by how
 * saturated the pixel is), a tone shaper (shadows, midtones, highlights and
 * contrast about mid-grey) and a print tone.
 *
 * One implementation for the photo-wide tool (rgbProc) and for a mask's own
 * conversion (iplocallab.cc), so the two cannot drift apart. Works on linear
 * working-space RGB in the 0..65535 range.
 */
class PerceptualBW
{
public:
    struct Settings {
        // red, orange, yellow, green, cyan, blue, purple, magenta; 33 = neutral
        std::array<float, 8> mixer{{33.f, 33.f, 33.f, 33.f, 33.f, 33.f, 33.f, 33.f}};
        Glib::ustring setting;
        Glib::ustring filter;
        float contrast = 0.f;    // -100..100
        float midtones = 0.f;    // -100..100
        float highlights = 0.f;  // -100..100
        float shadows = 0.f;     // -100..100
        float printTone = 0.f;   // -100..100, cool..warm
    };

    void build(const Settings& s, float yRed, float yGreen, float yBlue)
    {
        yRed_ = yRed;
        yGreen_ = yGreen;
        yBlue_ = yBlue;

        constexpr std::array<float, 8> centers = {{
            0.f, 1.f / 12.f, 2.f / 12.f, 4.f / 12.f,
            6.f / 12.f, 8.f / 12.f, 9.f / 12.f, 10.f / 12.f
        }};
        std::array<float, 8> presetEv = {};
        if (s.setting == "Landscape") {
            presetEv = {{0.25f, 0.35f, 0.18f, 0.05f, -0.12f, -0.35f, -0.2f, 0.1f}};
        } else if (s.setting == "Portrait") {
            presetEv = {{0.18f, 0.3f, 0.14f, -0.05f, -0.08f, -0.12f, 0.f, 0.1f}};
        } else if (s.setting == "Orthochromatic") {
            presetEv = {{-1.1f, -0.5f, 0.05f, 0.3f, 0.35f, 0.35f, 0.1f, -0.45f}};
        } else if (s.setting == "InfraRed") {
            presetEv = {{1.4f, 1.1f, 0.55f, 0.8f, -0.35f, -1.2f, -0.8f, 0.45f}};
        } else if (s.setting == "HighContrast") {
            presetEv = {{0.35f, 0.25f, 0.1f, -0.08f, -0.18f, -0.3f, -0.15f, 0.15f}};
        }

        float filterCenter = -1.f;
        if (s.filter == "Red") filterCenter = 0.f;
        else if (s.filter == "Orange") filterCenter = 1.f / 12.f;
        else if (s.filter == "Yellow") filterCenter = 2.f / 12.f;
        else if (s.filter == "YellowGreen") filterCenter = 3.f / 12.f;
        else if (s.filter == "Green") filterCenter = 4.f / 12.f;
        else if (s.filter == "Cyan") filterCenter = 6.f / 12.f;
        else if (s.filter == "Blue") filterCenter = 8.f / 12.f;
        else if (s.filter == "Purple") filterCenter = 9.f / 12.f;

        for (size_t i = 0; i < hueGain_.size(); ++i) {
            const float hue = static_cast<float>(i) / static_cast<float>(hueGain_.size() - 1);
            float weightedEv = 0.f;
            float weightSum = 0.f;
            for (size_t band = 0; band < centers.size(); ++band) {
                float distance = std::abs(hue - centers[band]);
                distance = std::min(distance, 1.f - distance);
                const float normalized = std::max(0.f, 1.f - distance * 6.f);
                const float weight = normalized * normalized * (3.f - 2.f * normalized);
                const float userEv = rtengine::LIM((s.mixer[band] - 33.f) / 67.f, -1.5f, 1.5f) * 1.15f;
                weightedEv += weight * (userEv + presetEv[band]);
                weightSum += weight;
            }
            float ev = weightSum > 0.f ? weightedEv / weightSum : 0.f;
            if (filterCenter >= 0.f) {
                ev += 0.9f * std::cos(2.f * rtengine::RT_PI_F * (hue - filterCenter));
            }
            hueGain_[i] = std::exp2(rtengine::LIM(ev, -2.5f, 2.5f));
        }

        const float contrastPower = std::exp2(s.contrast / 125.f);
        const float midtoneEv = s.midtones / 100.f;
        const float highlightEv = s.highlights / 100.f;
        const float shadowEv = s.shadows / 100.f;
        const auto smoothstep = [](float edge0, float edge1, float value) {
            const float t = rtengine::LIM((value - edge0) / (edge1 - edge0), 0.f, 1.f);
            return t * t * (3.f - 2.f * t);
        };
        tone_.resize(65536);
        for (size_t i = 0; i < tone_.size(); ++i) {
            float x = static_cast<float>(i) / 65535.f;
            const float shadowWeight = 1.f - smoothstep(0.04f, 0.42f, x);
            const float highlightWeight = smoothstep(0.38f, 0.92f, x);
            const float midtoneWeight = rtengine::LIM(4.f * x * (1.f - x), 0.f, 1.f);
            x *= std::exp2(shadowEv * shadowWeight + highlightEv * highlightWeight + midtoneEv * midtoneWeight);
            x = 0.18f * std::pow(std::max(x / 0.18f, 0.f), contrastPower);
            tone_[i] = std::max(0.f, x * 65535.f);
        }

        redFactor_ = greenFactor_ = blueFactor_ = 1.f;
        const float printTone = s.printTone / 100.f;
        if (printTone > 0.f) {
            redFactor_ += 0.1f * printTone;
            greenFactor_ += 0.025f * printTone;
            blueFactor_ -= 0.14f * printTone;
        } else if (printTone < 0.f) {
            const float cool = -printTone;
            redFactor_ -= 0.1f * cool;
            greenFactor_ -= 0.015f * cool;
            blueFactor_ += 0.13f * cool;
        }
        const float neutralY = yRed_ + yGreen_ + yBlue_;
        const float tonedY = yRed_ * redFactor_ + yGreen_ * greenFactor_ + yBlue_ * blueFactor_;
        const float toneNormalization = tonedY > 0.f ? neutralY / tonedY : 1.f;
        redFactor_ *= toneNormalization;
        greenFactor_ *= toneNormalization;
        blueFactor_ *= toneNormalization;
    }

    /** Converts one pixel in place. */
    void apply(float& r, float& g, float& b) const
    {
        const float scale = std::max(65535.f, std::max(r, std::max(g, b)));
        float hue = 0.f;
        float saturation = 0.f;
        float value = 0.f;
        Color::rgb2hsv01(
            rtengine::LIM(r / scale, 0.f, 1.f),
            rtengine::LIM(g / scale, 0.f, 1.f),
            rtengine::LIM(b / scale, 0.f, 1.f),
            hue,
            saturation,
            value);
        const size_t hueIndex = static_cast<size_t>(rtengine::LIM(
            hue * static_cast<float>(hueGain_.size() - 1),
            0.f,
            static_cast<float>(hueGain_.size() - 1)));
        const float chromaWeight = saturation * saturation * (3.f - 2.f * saturation);
        const float hueGain = 1.f + (hueGain_[hueIndex] - 1.f) * chromaWeight;
        float gray = (yRed_ * r + yGreen_ * g + yBlue_ * b) * hueGain;
        const size_t toneIndex = static_cast<size_t>(rtengine::LIM(
            gray, 0.f, static_cast<float>(tone_.size() - 1)));
        gray = gray <= 65535.f ? tone_[toneIndex] : gray;
        r = gray * redFactor_;
        g = gray * greenFactor_;
        b = gray * blueFactor_;
    }

private:
    std::array<float, 4096> hueGain_;
    std::vector<float> tone_;
    float yRed_ = 0.f;
    float yGreen_ = 0.f;
    float yBlue_ = 0.f;
    float redFactor_ = 1.f;
    float greenFactor_ = 1.f;
    float blueFactor_ = 1.f;
};

}
