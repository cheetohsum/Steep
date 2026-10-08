#include "rtengine/demotion.h"
#include "rtengine/doubleexposureblend.h"
#include "rtengine/imagefloat.h"
#include "rtengine/improcfun.h"
#include "rtengine/partnerimagestore.h"
#include "rtengine/rtapp.h"
#include "demotionlayers.h"
#include "options.h"
#include <giomm.h>
#include <glibmm/miscutils.h>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rtengine;
using namespace rtengine::procparams;
namespace
{
int checks = 0;
void check(bool ok, const char *text)
{
    if (!ok)
        throw std::runtime_error(text);
    ++checks;
    std::cout << "PASS " << text << std::endl;
}
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) < tolerance; }
float difference(const Imagefloat &a, const Imagefloat &b)
{
    float error = 0;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x) {
            for (float v : {a.r(y, x) - b.r(y, x), a.g(y, x) - b.g(y, x), a.b(y, x) - b.b(y, x)}) {
                if (!std::isfinite(v))
                    return std::numeric_limits<float>::infinity();
                error = std::max(error, std::abs(v));
            }
        }
    return error;
}
} // namespace
int main(int argc, char **argv)
{
    try {
        Gio::init();
        std::setlocale(LC_NUMERIC, "C");
        DEMotionParams p;
        check(demotion::trajectory(p, 1000, 96).empty(), "legacy layers have no motion samples");
        p.path = demotion::Path::LINE;
        p.noise = 0;
        auto end = demotion::pose(p, 1);
        check(near(end.x, .08f) && near(end.y, 0),
              "quarter-second pan travels eight percent at default speed");
        p.shutter = .5;
        check(near(demotion::pose(p, 1).x, end.x * 2),
              "doubling shutter doubles path length without changing gain");
        p.direction = 90;
        check(near(demotion::pose(p, 1).y, .16f) && near(demotion::pose(p, 1).x, 0),
              "direction rotates the path in physical coordinates");
        p.shutter = 0;
        check(demotion::trajectory(p, 1000, 96).empty(), "zero shutter is an identity");
        p.shutter = .25;
        p.direction = 0;
        const auto constant = [](float, float) { return demotion::Pixel{.2f, .3f, .4f, 1}; };
        bool allFinite = true, stable = true, flat = true;
        for (int shape = 1; shape <= 8; ++shape) {
            p.path = static_cast<demotion::Path>(shape);
            p.noise = 37;
            auto a = demotion::trajectory(p, 1000, 192), b = demotion::trajectory(p, 1000, 192);
            for (size_t i = 0; i < a.knots.size(); ++i) {
                allFinite &= std::isfinite(a.knots[i].x + a.knots[i].y + a.knots[i].invZoom);
                stable &= a.knots[i].x == b.knots[i].x && a.knots[i].y == b.knots[i].y && a.knots[i].c == b.knots[i].c;
            }
            const auto color = demotion::sample(a, .5, .5, 1.5f, constant);
            flat &= near(color.r, .2) && near(color.g, .3) && near(color.b, .4);
        }
        check(allFinite, "all eight paths have finite bounded transforms");
        check(stable, "motion is deterministic across repeated renders");
        check(flat, "all paths preserve a constant field's linear brightness and color");
        p.path = demotion::Path::WANDER;
        auto a = demotion::pose(p, .63f);
        p.seed = demotion::nextSeed(p.seed);
        auto b = demotion::pose(p, .63f);
        check(!near(a.x, b.x) || !near(a.y, b.y), "randomize changes the recorded path");
        p.path = demotion::Path::LINE;
        p.noise = 0;
        a = demotion::pose(p, .63f);
        p.seed = 4242;
        b = demotion::pose(p, .63f);
        check(near(a.x, b.x) && near(a.y, b.y), "zero noise keeps standardized paths independent of seed");
        p.path = demotion::Path::ZOOM_IN;
        check(demotion::pose(p, 1).invZoom < 1, "zoom-in expands the image");
        p.path = demotion::Path::ZOOM_OUT;
        check(demotion::pose(p, 1).invZoom > 1, "zoom-out contracts the image");
        p.path = demotion::Path::LINE;
        const auto poses = demotion::trajectory(p, 1000, 192);
        const auto edge = demotion::sample(poses, .01, .5, 1.f, constant);
        check(edge.a > 0 && edge.a < .5 && near(edge.r, .2),
              "frame edges lose coverage, not color or exposure");
        const auto cutout = [](float u, float) {
            return u > .49 && u < .51 ? demotion::Pixel{1, .2f, .1f, 1} : demotion::Pixel{};
        };
        const auto trail = demotion::sample(poses, .55, .5, 1.f, cutout);
        check(trail.a > .1 && near(trail.r, 1) && near(trail.g, .2),
              "a cut-out mask travels with its premultiplied light trail");
        {
            DEMotionParams pan;
            pan.path = demotion::Path::LINE;
            pan.noise = 0;
            pan.speed = 100;
            pan.shutter = .6;
            constexpr int width = 4096;
            const float light = .2f * width;
            const auto shutter = demotion::trajectory(pan, width, 192);
            const auto pointLight = [&](float u, float) {
                const float energy = std::max(0.f, 1.f - std::abs(u * width - light));
                return demotion::Pixel{energy, energy, energy, 1};
            };
            float minimum = 1, maximum = 0;
            for (int x = int(light) + 4; x < int(light + .6f * width) - 4; x += 3) {
                const auto pixel = demotion::sample(shutter, (x + .5f) / width, .5, 1.f, pointLight);
                const float relative = pixel.r * pixel.a * (.6f * width);
                minimum = std::min(minimum, relative);
                maximum = std::max(maximum, relative);
            }
            check(minimum > .9f && maximum < 1.1f,
                  "long shutter produces an uninterrupted evenly exposed one-pixel light trail");
            pan.direction = 180;
            const auto reverse = demotion::trajectory(pan, width, 192);
            const auto rightLight = [&](float u, float v) { return pointLight(1 - u, v); };
            const auto forwardPixel = demotion::sample(shutter, .61, .5, 1.f, pointLight);
            const auto reversePixel = demotion::sample(reverse, .39, .5, 1.f, rightLight);
            check(near(forwardPixel.r * forwardPixel.a, reversePixel.r * reversePixel.a, 1e-6),
                  "reversing a pan preserves the integrated light energy");
            const auto coverage = demotion::sample(shutter, .15, .5, 1.f, constant);
            check(near(coverage.a, .25f, 1e-6),
                  "frame edge coverage follows the exact fraction of open-shutter time");
            const auto widerIntervals = demotion::trajectory(pan, width, 24);
            const auto samePixel = demotion::sample(widerIntervals, .39, .5, 1.f, rightLight);
            check(near(reversePixel.r * reversePixel.a, samePixel.r * samePixel.a, .00004f),
                  "light trail exposure is independent of the path interval budget");
        }
        {
            bool agrees = true;
            const auto gradient = [](float u, float v) { return demotion::Pixel{u, v, u * v, 1}; };
            for (int shape = 1; shape <= 8; ++shape) {
                DEMotionParams motion;
                motion.path = static_cast<demotion::Path>(shape);
                motion.noise = 40;
                motion.shutter = .7;
                motion.complexity = 7;
                const auto path = demotion::trajectory(motion, 1024, 192);
                const auto integrated = demotion::sample(path, .48, .59, 1.5f, gradient);
                double r = 0, g = 0, b = 0, a = 0;
                constexpr int count = 32768;
                for (int i = 0; i < count; ++i) {
                    const auto q = demotion::pose(motion, (i + .5f) / count);
                    const float x = .48f - .5f - q.x, y = (.59f - .5f) / 1.5f - q.y;
                    const float u = (q.c * x + q.s * y) * q.invZoom + .5f;
                    const float v = (-q.s * x + q.c * y) * q.invZoom * 1.5f + .5f;
                    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                    const auto p = gradient(u, v);
                    r += p.r; g += p.g; b += p.b; a += 1;
                }
                agrees &= near(integrated.r * integrated.a, r / count, .0002f) &&
                          near(integrated.g * integrated.a, g / count, .0002f) &&
                          near(integrated.b * integrated.a, b / count, .0002f) &&
                          near(integrated.a, a / count, .0002f);
            }
            check(agrees, "all motion paths agree with a dense uniform-time shutter reference");
        }
        const auto transparent = demotion::mix({9, 2, 3, 0}, {.3f, .2f, .1f, 1}, .5);
        check(near(transparent.r, .3) && near(transparent.a, .5),
              "transparent sample RGB cannot contaminate motion edges");
        float r, g, bl;
        deblend::blend(DoubleExposureParams::BlendMode::ADAPTIVE, DoubleExposureParams::Compare::LUMINANCE,
                       .5, 1, .01, .01, .01, .1, .1, .1, r, g, bl);
        check(r > .08 && r <= .100001,
              "adaptive comparison keeps light detail over dark scenes without additive clipping");
        deblend::blend(DoubleExposureParams::BlendMode::ADAPTIVE, DoubleExposureParams::Compare::LUMINANCE,
                       .5, 1, .8, .8, .8, .5, .5, .5, r, g, bl);
        check(r >= .5 && r < .6, "adaptive comparison keeps dark detail over bright scenes");
        deblend::blend(DoubleExposureParams::BlendMode::ADAPTIVE, DoubleExposureParams::Compare::LUMINANCE,
                       .5, 1, .4, .2, .1, .4, .2, .1, r, g, bl);
        check(near(r, .4) && near(g, .2) && near(bl, .1),
              "identical exposures do not brighten or desaturate each other");
        deblend::blend(DoubleExposureParams::BlendMode::ADAPTIVE, DoubleExposureParams::Compare::LUMINANCE,
                       .5, 1, .48, .48, .48, .01, .01, .01, r, g, bl);
        check(r > .01f + .19f * (.48f - .01f),
              "adaptive sharp layer preserves the continuous trail beneath dark detail");
        deblend::blend(DoubleExposureParams::BlendMode::ADAPTIVE, DoubleExposureParams::Compare::LUMINANCE,
                       .5, 1, .01, .01, .01, .14, .14, .14, r, g, bl);
        check(r < .14f - .19f * (.14f - .01f),
              "adaptive sharp layer preserves the continuous trail beneath bright detail");
        DoubleExposureParams params;
        check(demotionui::addMotionLayer(params, "fixture.jpg") == 0 && params.layers.size() == 2,
              "empty setup seeds the current photo plus its motion copy");
        check(params.layers[0].path == params.layers[1].path &&
                  params.layers[0].motion.path == demotion::Path::LINE &&
                  params.layers[1].motion.path == demotion::Path::OFF &&
                  params.layers[1].blendMode == DoubleExposureParams::BlendMode::ADAPTIVE,
              "motion sits immediately below the sharp adaptive exposure");
        params.layers[1].offsetX = 15;
        check(params.layers[0].offsetX == 0, "copied layer placement is independently editable");
        check(demotionui::addMotionLayer(params, "other.jpg") == 1 && params.layers[1].offsetX == 15,
              "another motion layer copies the first still exposure, not an existing motion layer");
        while (params.layers.size() < 8)
            demotionui::addMotionLayer(params, "fixture.jpg");
        check(demotionui::addMotionLayer(params, "fixture.jpg") == -1 && params.layers.size() == 8,
              "motion additions obey the layer limit");
        Glib::KeyFile key;
        p.noise = 73;
        p.direction = -62;
        p.shutter = 1.25;
        p.seed = 831;
        p.save(key, "Layer1");
        DEMotionParams restored;
        restored.load(key, "Layer1");
        check(restored == p, "motion settings and random seed round-trip exactly");
        restored.load(key, "Absent");
        check(restored.path == demotion::Path::OFF, "old profiles default to still layers");
        key.set_integer("Double Exposure", "Layer1MotionPath", 999);
        key.set_double("Double Exposure", "Layer1MotionSpeed", 1e9);
        key.set_double("Double Exposure", "Layer1MotionNoise", -200);
        restored.load(key, "Layer1");
        check(restored.path == demotion::Path::OFF && restored.speed == 100 && restored.noise == 0,
              "malformed values are bounded safely");
        if (argc == 3) {
            App::get().setArgv0(argv[1]);
            Options::load(true);
            const std::string scratch = argv[2];
            const auto file = Glib::build_filename(scratch, "de-motion-source.tif");
            Imagefloat fixture(192, 128);
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 192; ++x) {
                    fixture.r(y, x) = x > 90 && x < 100 ? 56000 : 3000;
                    fixture.g(y, x) = y > 40 && y < 75 ? 36000 : 2000;
                    fixture.b(y, x) = 6000;
                }
            check(fixture.saveAsTIFF(file, 16, false) == 0, "write native motion fixture");
            std::promise<void> begin;
            auto ready = begin.get_future().share();
            std::vector<std::future<std::shared_ptr<PartnerImage>>> loads;
            for (int i = 0; i < 4; ++i) loads.push_back(std::async(std::launch::async, [file, ready] {
                ready.wait();
                return PartnerImageStore::getInstance().getPartner(file, "ProPhoto", false);
            }));
            begin.set_value();
            const auto firstPartner = loads.front().get();
            bool sharedDecode = bool(firstPartner);
            for (size_t i = 1; i < loads.size(); ++i) sharedDecode &= loads[i].get() == firstPartner;
            check(sharedDecode, "simultaneous partner loads share one decoded image");
            ProcParams pp;
            pp.doubleExposure.enabled = true;
            pp.doubleExposure.highlightLatitude = 0;
            DoubleExposureParams::Layer layer;
            layer.path = file;
            layer.motion.path = demotion::Path::WAVE;
            layer.motion.noise = 20;
            pp.doubleExposure.layers = {layer};
            const auto saved = Glib::build_filename(scratch, "de-motion.pp3");
            check(pp.save(saved) == 0, "save full motion profile");
            ProcParams opened;
            check(opened.load(saved) == 0 && opened.doubleExposure == pp.doubleExposure,
                  "full PP3 restores layer order, motion and blending");
            ImProcFunctions ip(&pp, true);
            std::unique_ptr<Imagefloat> full(fixture.copy()), again(fixture.copy());
            const auto start = std::chrono::steady_clock::now();
            ip.doubleExposure(full.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, true);
            ip.doubleExposure(again.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, true);
            check(difference(*full, *again) == 0, "native motion output repeats exactly");
            check(difference(*full, fixture) > 100, "native engine actually applies the motion effect");
            Imagefloat tile(64, 48);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    tile.r(y, x) = fixture.r(y + 32, x + 48);
                    tile.g(y, x) = fixture.g(y + 32, x + 48);
                    tile.b(y, x) = fixture.b(y + 32, x + 48);
                }
            ip.doubleExposure(&tile, pp.doubleExposure, "ProPhoto", 192, 128, 48, 32, 1, true);
            float tileError = 0;
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x)
                    tileError =
                        std::max(tileError, std::max({std::abs(tile.r(y, x) - full->r(y + 32, x + 48)),
                                                      std::abs(tile.g(y, x) - full->g(y + 32, x + 48)),
                                                      std::abs(tile.b(y, x) - full->b(y + 32, x + 48))}));
            check(tileError < .02, "native zoom tile matches the corresponding full-frame motion");
            std::unique_ptr<Imagefloat> preview(fixture.copy()), warm(fixture.copy());
            ip.doubleExposure(preview.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, false);
            ip.doubleExposure(warm.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, false);
            check(difference(*preview, *warm) == 0, "cached interactive motion is repeatable");
            check(difference(*preview, *full) < .2,
                  "interactive cached motion agrees with full-source integration on the fixture");
            pp.doubleExposure.layers[0].motion.seed =
                demotion::nextSeed(pp.doubleExposure.layers[0].motion.seed);
            std::unique_ptr<Imagefloat> randomized(fixture.copy());
            ip.doubleExposure(randomized.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, false);
            check(difference(*preview, *randomized) > 1, "randomize invalidates the editor motion cache");
            pp.doubleExposure.layers[0].motion.path = demotion::Path::OFF;
            std::unique_ptr<Imagefloat> still(fixture.copy()), zero(fixture.copy());
            ip.doubleExposure(still.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, true);
            pp.doubleExposure.layers[0].motion.path = demotion::Path::LINE;
            pp.doubleExposure.layers[0].motion.speed = 0;
            ip.doubleExposure(zero.get(), pp.doubleExposure, "ProPhoto", 192, 128, 0, 0, 1, true);
            check(difference(*still, *zero) == 0,
                  "zero-distance motion preserves the legacy engine path exactly");
            full->saveAsTIFF(Glib::build_filename(scratch, "de-motion-render.tif"), 16, false);
            std::cout << "Native fixture suite milliseconds: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()
                      << std::endl;
        }
        std::cout << checks << " checks passed" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
