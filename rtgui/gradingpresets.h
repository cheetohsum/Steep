#pragma once

#include <array>
#include <string>
#include <vector>
#include "rtengine/procparams.h"

namespace rtengine { class Imagefloat; }

namespace gradingpresets {
using Grade = rtengine::procparams::ColorGradingParams;
struct Preset {
    std::string id;
    Glib::ustring name;
    Grade grade;
    bool personal = false;
};
struct Features {
    bool valid = false;
    double shadows = 0, highlights = 0, neutralHighlights = 0;
    double warm = 0, cool = 0, foliage = 0, skin = 0, chroma = 0;
    double p10 = 0, median = 0, p90 = 0;
    std::array<double, 3> a{}, b{}, weight{};
    // Uniform spatial samples retain opposing hues that an average cancels.
    std::vector<std::array<double, 3>> samples;
};

std::vector<Preset> bundled();
std::vector<Preset> load(const Glib::ustring& directory, unsigned& rejected);
void save(const Glib::ustring& directory, Preset& preset);
void erase(const Glib::ustring& directory, const Preset& preset);
bool valid(const Grade& grade);
Features analyze(const rtengine::Imagefloat& image);
std::vector<Preset> rank(std::vector<Preset> presets, const Features& features);
std::array<double, 3> swatch(double hue, double saturation);
std::array<double, 3> swatch(const Grade& grade, int zone);
std::array<double, 3> tonalShift(const Grade& grade, double lightness);
} // namespace gradingpresets
