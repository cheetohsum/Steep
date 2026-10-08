#include "rtengine/masksampling.h"
#include "rtengine/maskpaint.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool value, const char* label)
{
    if (!value) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}
}

int main()
{
    using rtengine::masksampling::atPixel;
    try {
        array2D<float> ramp(128, 96);
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x)
            ramp[y][x] = .6f * (x + .5f) / 128 + .4f * (y + .5f) / 96;
        bool exact = true;
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x)
            exact &= atPixel(ramp, 128, 96, x, y, 128, 96) == ramp[y][x];
        check(exact, "same-resolution sampling leaves mask pixels unchanged");

        for (const auto size : {std::pair<int, int>{6144, 4096}, {4096, 6144}, {6001, 4003}}) {
            double error = 0;
            for (int step : {1, 2, 3, 6, 8}) {
                for (int y = 64; y < size.second / step - 64; y += 31)
                    for (int x = 64; x < size.first / step - 64; x += 37) {
                        const double expected = .6 * (x + .5) * step / size.first +
                                                .4 * (y + .5) * step / size.second;
                        error = std::max(error, std::abs(atPixel(ramp, 128, 96, x * step, y * step,
                                                                size.first, size.second, step) - expected));
                    }
            }
            check(error < 1e-6, "landscape/portrait/odd-size previews sample the same physical mask locations");
        }

        array2D<float> edge(128, 96);
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x)
            edge[y][x] = x < 64 ? 0.f : 1.f;
        check(atPixel(edge, 128, 96, 3071, 2048, 6144, 4096) < .5f &&
                  atPixel(edge, 128, 96, 3072, 2048, 6144, 4096) > .5f &&
                  std::abs(atPixel(edge, 128, 96, 3071, 2048, 6144, 4096) +
                           atPixel(edge, 128, 96, 3072, 2048, 6144, 4096) - 1.f) < 1e-6,
              "full-resolution mask boundary stays centered instead of shifting left");

        bool cropAligned = true;
        for (int step : {1, 2, 3, 6}) for (int y = 0; y < 91; ++y) for (int x = 0; x < 173; ++x) {
            const int cropX = 233, cropY = 127, workX = 17, workY = 9;
            const int globalX = (x + workX + cropX) * step, globalY = (y + workY + cropY) * step;
            const auto a = atPixel(ramp, 128, 96, globalX, globalY, 6144, 4096, step);
            double reference = 0;
            for (int dy = 0; dy < step; ++dy) for (int dx = 0; dx < step; ++dx)
                reference += atPixel(ramp, 128, 96, globalX + dx, globalY + dy, 6144, 4096);
            cropAligned &= std::abs(a - reference / (step * step)) < 1e-6;
        }
        check(cropAligned, "crop and local work-area offsets agree with full-resolution coverage");

        array2D<float> opaque(128, 96);
        opaque.fill(1.f);
        bool borders = true;
        for (int step : {1, 3, 7}) for (int y : {0, (4003 - 1) / step * step})
            for (int x : {0, (6001 - 1) / step * step})
                borders &= atPixel(opaque, 128, 96, x, y, 6001, 4003, step) == 1.f;
        check(borders, "masks cover all image corners including partial preview pixels");
        check(atPixel(opaque, 128, 96, -1, 0, 6001, 4003) == 0 &&
                  atPixel(opaque, 128, 96, 6001, 0, 6001, 4003) == 0 &&
                  atPixel(opaque, 128, 96, 0, 4003, 6001, 4003) == 0 &&
                  atPixel(opaque, 128, 96, 0, 0, 6001, 4003, 0) == 0 &&
                  atPixel(opaque, 128, 96, std::numeric_limits<double>::quiet_NaN(), 0, 6001, 4003) == 0,
              "out-of-image and invalid coordinates remain unselected");

        array2D<float> brush(128, 96);
        brush.fill(0.f);
        rtengine::MaskPaint paint;
        rtengine::MaskStroke stroke;
        stroke.radius = .1;
        stroke.hardness = .3;
        stroke.points = {{52.5 / 128, 30.5 / 96}};
        paint.strokes.push_back(stroke);
        paint.apply(brush, 128, 96, false);
        double sum = 0, cx = 0, cy = 0;
        for (int y = 0; y < 768; ++y) for (int x = 0; x < 1024; ++x) {
            const auto a = atPixel(brush, 128, 96, x, y, 1024, 768);
            sum += a; cx += (x + .5) * a; cy += (y + .5) * a;
        }
        check(sum > 0 && std::abs(cx / sum - 420) < .001 && std::abs(cy / sum - 244) < .001,
              "manual corrections on AI masks stay centered at their saved brush position");
        std::cout << "Mask sampling tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
