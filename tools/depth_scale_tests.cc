// g++ -std=c++14 -I. tools/depth_scale_tests.cc -o depth_scale_tests && ./depth_scale_tests
#include "rtengine/depthscale.h"

#include <iostream>
#include <stdexcept>

namespace
{
void check(bool value, const char* label)
{
    if (!value) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}

bool close(double a, double b, double tolerance = 1e-4)
{
    return std::abs(a - b) <= tolerance;
}
}

int main()
{
    using namespace rtengine::depthscale;
    try {
        // --- the band -----------------------------------------------------
        {
            const Range band = makeRange(0.2, 0.6, 0.1);
            check(weight(0.2f, band) == 1.f && weight(0.6f, band) == 1.f && weight(0.4f, band) == 1.f,
                  "the band is at full weight from its near limit to its far limit");
            check(weight(0.09f, band) == 0.f && weight(0.71f, band) == 0.f,
                  "nothing is selected beyond the feather");
            check(close(weight(0.15f, band), 0.5) && close(weight(0.65f, band), 0.5),
                  "the feather is half way down at half its width");
            check(weight(0.0f, makeRange(0.0, 0.5, 0.2)) == 1.f && weight(1.0f, makeRange(0.5, 1.0, 0.2)) == 1.f,
                  "a limit at the end of the scale keeps the end itself at full weight");
            const Range hard = makeRange(0.3, 0.5, 0.0);
            check(weight(0.299f, hard) == 0.f && weight(0.3f, hard) == 1.f && weight(0.501f, hard) == 0.f,
                  "no feather is a hard cut");
        }
        {
            double n = 0, f = 0, w = 0;
            splitRange(makeRange(0.25, 0.75, 0.2), n, f, w);
            check(close(n, 0.25) && close(f, 0.75) && close(w, 0.2), "limits and feather come back out of a band");
            splitRange(makeRange(0.8, 0.3, 0.9), n, f, w);
            check(close(n, 0.8) && close(f, 0.8) && close(w, kMaxFeather),
                  "crossed limits collapse and the feather is capped");
        }
        {
            // near and far of one split are each other's complement
            bool complement = true;
            for (double split : {0.2, 0.41, 0.77}) {
                const Range nearBand = preset(false, split), farBand = preset(true, split);
                for (int i = 0; i <= 200; ++i) {
                    const float d = i / 200.f;
                    complement &= close(weight(d, nearBand) + weight(d, farBand), 1.0, 1e-3);
                }
                complement &= weight(0.f, nearBand) == 1.f && weight(1.f, farBand) == 1.f;
            }
            check(complement, "automatic near and far add up to the whole photo");
        }

        // --- the pp3 text -------------------------------------------------
        {
            check(isAutomatic(parse("")) && isAutomatic(parse("0.1;0.2")) && isAutomatic(parse("a;b;c;d")),
                  "empty or malformed text is the automatic band");
            check(format(parse("")).empty(), "the automatic band is written as nothing");
            const Range band = makeRange(0.0, 0.37, 0.1);
            const Range back = parse(format(band));
            bool same = true;
            for (int k = 0; k < 4; ++k) same &= close(band[k], back[k]);
            check(same && !isAutomatic(back), "a band survives the round trip, feather below zero included");
            const Range wild = parse("-9;1.4;0.2;30");
            check(wild[1] == 1.0 && wild[2] == 1.0 && close(wild[1] - wild[0], kMaxFeather)
                      && close(wild[3] - wild[2], kMaxFeather),
                  "out-of-range text is brought inside the scale");
        }

        // --- the scale ----------------------------------------------------
        {
            // A scene from 1 m to the sky: inverse depth 1 .. 0, plus two wild samples.
            std::vector<float> samples;
            for (int i = 0; i < 5000; ++i) samples.push_back(1.f - i / 5000.f);
            samples.push_back(9.f);
            samples.push_back(7.f);
            const Scale scale = fitScale(samples);
            check(scale.nearest < 1.01f, "a stray near sample does not stretch the scale");
            check(close(scale.reach(), kFarReach, 0.5), "an open scene reaches as far as the far cap");
            check(scale.at(1.f) < 0.01f && scale.at(0.f) == 1.f && scale.at(1e-4f) == 1.f,
                  "the nearest thing is 0, everything past the reach is 1");
            // twice the distance is the same step anywhere on the scale
            const float step = scale.at(0.5f) - scale.at(1.f);
            check(close(scale.at(0.25f) - scale.at(0.5f), step, 2e-3) && close(scale.at(0.125f) - scale.at(0.25f), step, 2e-3),
                  "equal distance ratios are equal steps");
            // a person at 3 m with the ground starting at 1 m is in the near third,
            // not at the far end as on a scale linear in inverse depth (0.67)
            check(scale.at(1.f / 3.f) < 0.34f, "three times the nearest distance is still near");
            bool monotonic = true;
            for (int i = 1; i <= 1000; ++i) monotonic &= scale.at(i / 1000.f) <= scale.at((i - 1) / 1000.f);
            check(monotonic, "farther is never nearer on the scale");
        }
        {
            // A wall a little behind a subject: the far end is the wall, not infinity.
            std::vector<float> samples;
            for (int i = 0; i < 3000; ++i) samples.push_back(i < 1000 ? 1.f : 0.4f);
            const Scale scale = fitScale(samples);
            check(close(scale.reach(), 2.5, 0.01) && scale.at(0.4f) == 1.f && scale.at(1.f) == 0.f,
                  "a closed scene spans its own nearest and farthest");
            std::vector<float> flat(1000, 0.8f);
            const Scale level = fitScale(flat);
            check(level.span > 0.04f && level.at(0.8f) == 0.f, "a flat subject does not divide by zero");
        }

        // --- the split ----------------------------------------------------
        {
            std::vector<double> histogram(256, 0.0);
            for (int i = 20; i < 40; ++i) histogram[i] = 100;      // a subject
            for (int i = 120; i < 150; ++i) histogram[i] = 100;    // what is behind it
            const float split = splitOf(histogram);
            check(split > 70 / 256.f && split < 90 / 256.f, "the split falls midway between two groups of depths");
            for (int i = 250; i < 256; ++i) histogram[i] = 5000;   // six times as much sky
            check(close(splitOf(histogram), split), "sky at the far end does not move the split");
            std::vector<double> sky(256, 0.0);
            sky[255] = 1000;
            sky[10] = 1;
            check(splitOf(sky) > 10 / 256.f, "a frame that is almost all far end still splits");
            check(splitOf(std::vector<double>(256, 0.0)) == 0.5f, "an empty histogram splits in the middle");
        }
        std::cout << "all depth scale tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
