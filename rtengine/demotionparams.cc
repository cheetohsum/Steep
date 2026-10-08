#include "demotionparams.h"
#include <algorithm>
#include <cmath>

namespace rtengine
{
namespace procparams
{
void DEMotionParams::save(Glib::KeyFile &f, const std::string &p) const
{
    const auto g = "Double Exposure";
    f.set_integer(g, p + "MotionPath", static_cast<int>(path));
    f.set_double(g, p + "MotionShutter", shutter);
    f.set_double(g, p + "MotionSpeed", speed);
    f.set_double(g, p + "MotionDirection", direction);
    f.set_double(g, p + "MotionNoise", noise);
    f.set_double(g, p + "MotionComplexity", complexity);
    f.set_integer(g, p + "MotionSeed", seed);
}
void DEMotionParams::load(const Glib::KeyFile &f, const std::string &p)
{
    *this = {};
    const auto g = "Double Exposure";
    if (f.has_key(g, p + "MotionPath")) {
        const int value = f.get_integer(g, p + "MotionPath");
        if (value >= 0 && value <= static_cast<int>(Path::WANDER))
            path = static_cast<Path>(value);
    }
    const auto read = [&](const char *name, double &v, double lo, double hi) {
        if (!f.has_key(g, p + name))
            return;
        const double n = f.get_double(g, p + name);
        if (std::isfinite(n))
            v = std::max(lo, std::min(hi, n));
    };
    read("MotionShutter", shutter, 0, 4);
    read("MotionSpeed", speed, 0, 100);
    read("MotionDirection", direction, -180, 180);
    read("MotionNoise", noise, 0, 100);
    read("MotionComplexity", complexity, 1, 8);
    if (f.has_key(g, p + "MotionSeed"))
        seed = std::max(1, f.get_integer(g, p + "MotionSeed"));
}
} // namespace procparams
} // namespace rtengine
