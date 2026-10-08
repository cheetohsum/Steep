// Checks rtengine::filmhalation::convolve against the tables it serves.
// CMake target steep-halation-convolve-tests (STEEP_BUILD_PROFILE_TESTS).
//
// The halo goes in as "kernel convolved with the source, minus the
// uniform-field share", evaluated through a cosine transform and the
// kernel's closed-form Hankel transform. So: a flat field must come back
// untouched, the redistribution must add no light overall, a point source
// must reproduce the tabulated radial profile, and the size slider must scale
// the radius and nothing else.
#include "../rtengine/filmhalation.h"
#include "rtgui/threadutils.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace rtengine {
MyMutex* fftwMutex = nullptr;
}

namespace fh = rtengine::filmhalation;

namespace {

int failures = 0;

void check(bool ok, const char* what, double got = 0, double want = 0)
{
    std::printf("%s  %s", ok ? "PASS" : "FAIL", what);
    if (got != 0 || want != 0) {
        std::printf("  (got %.5g, expected %.5g)", got, want);
    }
    std::printf("\n");
    failures += !ok;
}

struct Plane {
    int w, h;
    std::vector<float> data;
    std::vector<float*> rows;

    Plane(int w, int h, float v = 0.f) : w(w), h(h), data(size_t(w) * h, v), rows(h)
    {
        for (int y = 0; y < h; ++y) {
            rows[y] = &data[size_t(y) * w];
        }
    }
    float** ptr() { return rows.data(); }
};

// Tabulated density at radius r (mm), piecewise constant per annulus.
double density(const fh::HaloTable& t, int b, int k, double r)
{
    for (int i = 0; i < t.bins; ++i) {
        if (r < t.edgesMM[i + 1]) {
            return t.kernel[b][k][i];
        }
    }
    return 0.0;
}

// Runs convolve on red-only source planes; returns the red-layer output.
Plane halo(fh::Construction c, const Plane& redSource, float pxPerMM, float size)
{
    Plane r = redSource, g(redSource.w, redSource.h), b(redSource.w, redSource.h);
    Plane outR(redSource.w, redSource.h), outG(redSource.w, redSource.h), outB(redSource.w, redSource.h);
    float** src[3] = {r.ptr(), g.ptr(), b.ptr()};
    float** dst[3] = {outR.ptr(), outG.ptr(), outB.ptr()};
    const float gain[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    fh::convolve(c, src, dst, redSource.w, redSource.h, pxPerMM, size, gain, true);
    return outR;
}

// Mean of p over pixels whose centre lies within half a pixel of radius d.
double ringMean(const Plane& p, int cx, int cy, double d)
{
    double sum = 0;
    int n = 0;
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            const double r = std::hypot(x - cx, y - cy);
            if (std::fabs(r - d) <= 0.5) {
                sum += p.rows[y][x];
                ++n;
            }
        }
    }
    return n ? sum / n : 0.0;
}

} // namespace

int main()
{
    rtengine::fftwMutex = new MyMutex;
    const fh::Construction cine = fh::Construction::MotionRemjetRemoved35;
    const fh::HaloTable& t = fh::table(cine);
    const float pxPerMM = 40.f;

    // 1. A flat field is left alone: its halo is exactly the uniform share.
    {
        const Plane flat(200, 150, 0.7f);
        const Plane out = halo(cine, flat, pxPerMM, 1.f);
        double worst = 0;
        for (float v : out.data) {
            worst = std::max(worst, std::fabs(double(v)));
        }
        check(worst < 1e-4, "flat field: no halo anywhere, borders included", worst, 0);
    }

    // 2 + 3. A point source: the light is only moved, and lands where the
    // tracer said it would.
    {
        const int n = 301, c = 150;
        Plane point(n, n);
        point.rows[c][c] = 1.f; // one pixel of unit exposure
        const Plane out = halo(cine, point, pxPerMM, 1.f);

        double sum = 0;
        for (float v : out.data) {
            sum += v;
        }
        check(std::fabs(sum) < 1e-5, "the halo moves light, it adds none (sums to zero)", sum, 0);
        check(out.rows[c][c] < 0.f, "the highlight itself gives up its uniform share", out.rows[c][c], -t.total[0][0]);

        // Output per pixel = density (per mm^2) x one pixel's area in mm^2.
        const double rc = t.ringEdgeMM;
        for (double rmm : {0.5 * rc, 1.6 * rc, 0.6, 1.0, 1.8}) {
            const double want = density(t, 0, 0, rmm) / (pxPerMM * pxPerMM);
            const double got = ringMean(out, c, c, rmm * pxPerMM);
            char label[96];
            std::snprintf(label, sizeof label, "point source reproduces the traced profile at %.2f mm", rmm);
            check(std::fabs(got - want) <= 0.06 * want + 2e-9, label, got, want);
        }
        const double inside = ringMean(out, c, c, 0.6 * rc * pxPerMM);
        const double edge = ringMean(out, c, c, 1.15 * rc * pxPerMM);
        check(edge > 1.5 * inside, "the ring survives the convolution: brighter at its edge than inside", edge / inside, 1.5);
    }

    // 3b. Leaving the uniform share to the caller: the two modes differ by
    // exactly total * src, pixel for pixel.
    {
        const int n = 120, c = 60;
        Plane src(n, n, 0.02f);
        for (int y = c - 4; y <= c + 4; ++y) {
            for (int x = c - 4; x <= c + 4; ++x) {
                src.rows[y][x] = 5.f;
            }
        }
        Plane z(n, n);
        Plane a0(n, n), a1(n, n), a2(n, n), b0(n, n), b1(n, n), b2(n, n);
        float** s[3] = {src.ptr(), z.ptr(), z.ptr()};
        float** da[3] = {a0.ptr(), a1.ptr(), a2.ptr()};
        float** db[3] = {b0.ptr(), b1.ptr(), b2.ptr()};
        const float gain[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        fh::convolve(cine, s, da, n, n, pxPerMM, 1.f, gain, true, true);
        fh::convolve(cine, s, db, n, n, pxPerMM, 1.f, gain, true, false);
        double worst = 0;
        for (int i = 0; i < n * n; ++i) {
            worst = std::max(worst, std::fabs(double(b0.data[i]) - t.total[0][0] * src.data[i] - a0.data[i]));
        }
        check(worst < 2e-5, "subtractUniform=false is the same halo plus total * src", worst, 0);
    }

    // 4. The size slider scales the radius: twice the size, the same density
    // at twice the radius, a quarter as dense.
    {
        const int n = 401, c = 200;
        Plane point(n, n);
        point.rows[c][c] = 1.f;
        const Plane one = halo(cine, point, pxPerMM, 1.f);
        const Plane two = halo(cine, point, pxPerMM, 2.f);
        const double a = ringMean(one, c, c, 0.5 * pxPerMM);
        const double b = ringMean(two, c, c, 1.0 * pxPerMM);
        check(std::fabs(b * 4 - a) <= 0.06 * a, "size x2 puts the same halo at twice the radius", b * 4, a);
    }

    // 5. Off-axis lean: a point 20 degrees off the axis puts (pi + 2 beta) :
    // (pi - 2 beta) of its halo on the outward : inward side, the split a
    // K (1 + beta cos phi) kernel has. On the axis nothing leans.
    {
        const int n = 401, cy = 200;
        const float centre = 200.f;
        const float offset = 150.f; // 3.75 mm at 40 px/mm
        fh::Obliquity ob;
        ob.centreX = centre;
        ob.centreY = centre;
        ob.exitPupilMM = static_cast<float>((offset / pxPerMM) / std::tan(20.0 * 3.14159265358979 / 180.0));
        const double beta = fh::obliqueBetaAt(t, 20.f);
        const double want = (3.14159265358979 + 2 * beta) / (3.14159265358979 - 2 * beta);

        const auto split = [&](int px) {
            Plane point(n, n), z(n, n), o0(n, n), o1(n, n), o2(n, n);
            point.rows[cy][px] = 1.f;
            float** s[3] = {point.ptr(), z.ptr(), z.ptr()};
            float** d[3] = {o0.ptr(), o1.ptr(), o2.ptr()};
            const float gain[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
            fh::convolve(cine, s, d, n, n, pxPerMM, 1.f, gain, true, true, &ob);
            double right = 0, left = 0;
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    const double r = std::hypot(x - px, y - cy);
                    if (r > 2 && r < 1.5 * pxPerMM && x != px) { // the column through the light is neither side
                        (x > px ? right : left) += o0.rows[y][x];
                    }
                }
            }
            return right / left;
        };
        const double rightOfAxis = split(int(centre + offset));
        const double leftOfAxis = split(int(centre - offset));
        const double onAxis = split(int(centre));
        check(std::fabs(rightOfAxis - want) <= 0.06 * want, "off-axis point: halo leans outwards by the traced beta", rightOfAxis, want);
        check(std::fabs(1.0 / leftOfAxis - want) <= 0.06 * want, "mirror image on the other side of the axis", 1.0 / leftOfAxis, want);
        check(std::fabs(onAxis - 1.0) < 0.02, "on the axis the halo is round", onAxis, 1.0);

        // A flat field hardly moves: only the slow change of tilt across the
        // frame shifts a little light (about 0.2% here, and physical).
        Plane flat(200, 150, 0.7f), z(200, 150), o0(200, 150), o1(200, 150), o2(200, 150);
        float** s[3] = {flat.ptr(), z.ptr(), z.ptr()};
        float** d[3] = {o0.ptr(), o1.ptr(), o2.ptr()};
        const float gain[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        fh::Obliquity wide;
        wide.centreX = 100.f;
        wide.centreY = 75.f;
        wide.exitPupilMM = 60.f;
        fh::convolve(cine, s, d, 200, 150, pxPerMM, 1.f, gain, true, true, &wide);
        double worst = 0;
        for (float v : o0.data) {
            worst = std::max(worst, std::fabs(double(v)));
        }
        check(worst < 3e-3 * 0.7, "flat field stays flat with the lean on", worst / 0.7, 3e-3);
    }

    // 6. Cost on a preview-sized plane.
    {
        Plane src(1560, 1040, 0.05f);
        for (int y = 400; y < 640; ++y) {
            for (int x = 700; x < 860; ++x) {
                src.rows[y][x] = 8.f;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        Plane r = src, g = src, b = src;
        Plane o0(src.w, src.h), o1(src.w, src.h), o2(src.w, src.h);
        float** s[3] = {r.ptr(), g.ptr(), b.ptr()};
        float** d[3] = {o0.ptr(), o1.ptr(), o2.ptr()};
        const float gain[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        fh::convolve(cine, s, d, src.w, src.h, 43.f, 1.f, gain, true);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("INFO  1560x1040 plane, three bands to three layers: %.0f ms (first call includes building the transfer table)\n", ms);
        const auto t1 = std::chrono::steady_clock::now();
        fh::convolve(cine, s, d, src.w, src.h, 43.f, 1.f, gain, true);
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        // Informational only: on an oversubscribed machine OpenMP's barriers
        // dominate (measured 75 ms at 4 threads, 8.5 s at 16 with 12 cores
        // busy elsewhere), so wall time says more about the box than the code.
        std::printf("INFO  second call: %.0f ms (try OMP_NUM_THREADS=4 if the machine is busy)\n", ms2);
    }

    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
