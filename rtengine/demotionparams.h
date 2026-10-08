#pragma once
#include <tuple>
#include <glibmm/keyfile.h>

namespace rtengine
{
namespace procparams
{

struct DEMotionParams {
    enum class Path { OFF, LINE, WAVE, ZIGZAG, SWIRL, ZOOM_IN, ZOOM_OUT, HANDHELD, WANDER };
    Path path = Path::OFF;
    double shutter = .25; // seconds; exposure compensated
    double speed = 32;    // percent of source width per second
    double direction = 0;
    double noise = 12;
    double complexity = 3;
    int seed = 1;

    bool operator==(const DEMotionParams &b) const
    {
        return std::tie(path, shutter, speed, direction, noise, complexity, seed) ==
               std::tie(b.path, b.shutter, b.speed, b.direction, b.noise, b.complexity, b.seed);
    }
    bool operator!=(const DEMotionParams &b) const { return !(*this == b); }
    void save(Glib::KeyFile &file, const std::string &prefix) const;
    void load(const Glib::KeyFile &file, const std::string &prefix);
};

} // namespace procparams
} // namespace rtengine
