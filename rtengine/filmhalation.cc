// Runtime side of the traced halation kernels (see filmhalation.h).

#include "filmhalation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include <fftw3.h>

#include "rtgui/threadutils.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace rtengine {

extern MyMutex* fftwMutex;

namespace filmhalation {

namespace {

constexpr double kPi = 3.14159265358979323846;

// J1 by the classic rational approximations (Numerical Recipes bessj1),
// absolute error ~1e-8: far below what a halo can show.
double besselJ1(double x)
{
    const double ax = std::fabs(x);
    if (ax < 8.0) {
        const double y = x * x;
        const double a1 = x * (72362614232.0 + y * (-7895059235.0 + y * (242396853.1 + y * (-2972611.439 + y * (15704.48260 + y * (-30.16036606))))));
        const double a2 = 144725228442.0 + y * (2300535178.0 + y * (18583304.74 + y * (99447.43394 + y * (376.9991397 + y))));
        return a1 / a2;
    }
    const double z = 8.0 / ax;
    const double y = z * z;
    const double xx = ax - 2.356194491;
    const double a1 = 1.0 + y * (0.183105e-2 + y * (-0.3516396496e-4 + y * (0.2457520174e-5 + y * (-0.240337019e-6))));
    const double a2 = 0.04687499995 + y * (-0.2002690873e-3 + y * (0.8449199096e-5 + y * (-0.88228987e-6 + y * 0.105787412e-6)));
    const double ans = std::sqrt(0.636619772 / ax) * (std::cos(xx) * a1 - z * std::sin(xx) * a2);
    return x < 0.0 ? -ans : ans;
}

// Hankel transform of a function that is constant on each ring [edges[j],
// edges[j+1]): a ring integrates in closed form, [r J1(2 pi nu r) / nu]
// between its edges, so the transform is exact for the tabulated profile.
struct Transfer {
    static constexpr double kStep = 0.05;  // cycles per mm of film
    static constexpr int kSamples = 4001;  // to 200 cycles/mm; beyond, nothing but the core is left
    std::vector<float> h[kChannels][kChannels]; // the kernel K
    std::vector<float> g[kChannels][kChannels]; // G = -integral_r^inf K, for the off-axis lean (mm)
};

void hankelRings(const std::vector<double>& edges, const std::vector<const std::vector<double>*>& values,
                 std::vector<std::vector<float>*>& out)
{
    const int rings = static_cast<int>(edges.size()) - 1;
#ifdef _OPENMP
    #pragma omp parallel
#endif
    {
        std::vector<double> rj(edges.size());
#ifdef _OPENMP
        #pragma omp for schedule(static)
#endif
        for (int s = 0; s < Transfer::kSamples; ++s) {
            const double nu = s * Transfer::kStep;
            if (s > 0) {
                for (size_t j = 0; j < edges.size(); ++j) {
                    rj[j] = edges[j] * besselJ1(2.0 * kPi * nu * edges[j]);
                }
            }
            for (size_t f = 0; f < values.size(); ++f) {
                const std::vector<double>& v = *values[f];
                double acc = 0.0;
                for (int i = 0; i < rings; ++i) {
                    const double ring = s == 0 ? kPi * (edges[i + 1] * edges[i + 1] - edges[i] * edges[i]) : (rj[i + 1] - rj[i]) / nu;
                    acc += v[i] * ring;
                }
                (*out[f])[s] = static_cast<float>(acc);
            }
        }
    }
}

std::unique_ptr<Transfer> buildTransfer(const HaloTable& t)
{
    std::unique_ptr<Transfer> tr(new Transfer);

    // K itself, on the table's own rings.
    std::vector<double> edges(t.edgesMM, t.edgesMM + t.bins + 1);
    std::vector<std::vector<double>> kernels;
    std::vector<std::vector<float>*> kOut;
    // G, piecewise linear between the table's edges, sampled on 2.5 um rings.
    constexpr double fine = 0.0025;
    const double rMax = edges.back();
    const int nFine = static_cast<int>(std::ceil(rMax / fine));
    std::vector<double> fineEdges(nFine + 1);
    for (int j = 0; j <= nFine; ++j) {
        fineEdges[j] = std::min(j * fine, rMax);
    }
    std::vector<std::vector<double>> gs;
    std::vector<std::vector<float>*> gOut;

    for (int b = 0; b < kChannels; ++b) {
        for (int k = 0; k < kChannels; ++k) {
            if (!t.kernel[b][k]) {
                continue;
            }
            kernels.emplace_back(t.kernel[b][k], t.kernel[b][k] + t.bins);
            tr->h[b][k].assign(Transfer::kSamples, 0.f);
            kOut.push_back(&tr->h[b][k]);

            std::vector<double> tail(t.bins + 1, 0.0);
            for (int i = t.bins - 1; i >= 0; --i) {
                tail[i] = tail[i + 1] + t.kernel[b][k][i] * (edges[i + 1] - edges[i]);
            }
            std::vector<double> g(nFine);
            int i = 0;
            for (int j = 0; j < nFine; ++j) {
                const double rm = 0.5 * (fineEdges[j] + fineEdges[j + 1]);
                while (i + 1 < t.bins && rm >= edges[i + 1]) {
                    ++i;
                }
                g[j] = -(t.kernel[b][k][i] * (edges[i + 1] - rm) + tail[i + 1]);
            }
            gs.push_back(std::move(g));
            tr->g[b][k].assign(Transfer::kSamples, 0.f);
            gOut.push_back(&tr->g[b][k]);
        }
    }

    std::vector<const std::vector<double>*> kIn, gIn;
    for (const auto& v : kernels) {
        kIn.push_back(&v);
    }
    for (const auto& v : gs) {
        gIn.push_back(&v);
    }
    hankelRings(edges, kIn, kOut);
    hankelRings(fineEdges, gIn, gOut);
    return tr;
}

const Transfer& transfer(Construction construction)
{
    static std::mutex mutex;
    static std::unique_ptr<Transfer> cache[static_cast<int>(Construction::Count)];
    std::lock_guard<std::mutex> lock(mutex);
    std::unique_ptr<Transfer>& slot = cache[static_cast<int>(construction)];
    if (!slot) {
        slot = buildTransfer(table(construction));
    }
    return *slot;
}

inline float lookup(const std::vector<float>& h, double index)
{
    if (index >= Transfer::kSamples - 1) {
        return 0.f;
    }
    const int i = static_cast<int>(index);
    const float f = static_cast<float>(index - i);
    return h[i] + (h[i + 1] - h[i]) * f;
}

float* allocPlane(size_t n)
{
    return static_cast<float*>(fftwf_malloc(n * sizeof(float)));
}

} // namespace

void convolve(Construction construction, float** const src[kChannels], float** const dst[kChannels], int width, int height,
              float pixelsPerMM, float sizeMultiplier, const float gain[kChannels][kChannels], bool multiThread,
              bool subtractUniform, const Obliquity* obliquity)
{
    if (width < 2 || height < 2 || !(pixelsPerMM > 0.f) || !(sizeMultiplier > 0.f)) {
        return;
    }

    const HaloTable& table = filmhalation::table(construction);
    const Transfer& tr = transfer(construction);
    bool bandUsed[kChannels] = {};
    bool layerUsed[kChannels] = {};
    bool any = false;
    float strongest = 0.f;
    for (int b = 0; b < kChannels; ++b) {
        for (int k = 0; k < kChannels; ++k) {
            if (!tr.h[b][k].empty() && gain[b][k] != 0.f) {
                bandUsed[b] = layerUsed[k] = any = true;
                strongest = std::max(strongest, std::fabs(gain[b][k]) * table.total[b][k]);
            }
        }
    }
    if (!any) {
        return;
    }

    // The off-axis lean only for the pairs that carry a visible share of the
    // halo; the rest lean too, but by nothing anyone could see.
    const bool lean = obliquity && obliquity->exitPupilMM > 0.f && table.obliqueBeta[3] > 0.f;
    bool pairLean[kChannels][kChannels] = {};
    bool bandLean[kChannels] = {};
    bool layerLean[kChannels] = {};
    if (lean) {
        for (int b = 0; b < kChannels; ++b) {
            for (int k = 0; k < kChannels; ++k) {
                if (!tr.g[b][k].empty() && gain[b][k] != 0.f && std::fabs(gain[b][k]) * table.total[b][k] >= 0.1f * strongest) {
                    pairLean[b][k] = bandLean[b] = layerLean[k] = true;
                }
            }
        }
    }

    const size_t n = static_cast<size_t>(width) * height;

    MyMutex::MyLock lock(*fftwMutex);
#ifdef RT_FFTW3F_OMP
    if (multiThread) {
        fftwf_init_threads();
        fftwf_plan_with_nthreads(omp_get_max_threads());
    }
#endif

    float* work = allocPlane(n);
    float* acc = allocPlane(n);
    float* coef[kChannels] = {};
    float* coefX[kChannels] = {};
    float* coefY[kChannels] = {};
    for (int b = 0; b < kChannels; ++b) {
        if (bandUsed[b]) {
            coef[b] = allocPlane(n);
        }
        if (bandLean[b]) {
            coefX[b] = allocPlane(n);
            coefY[b] = allocPlane(n);
        }
    }

    // DCT-II forward, DCT-III back: convolution under mirror-image edges.
    const fftwf_plan forward = fftwf_plan_r2r_2d(height, width, work, acc, FFTW_REDFT10, FFTW_REDFT10, FFTW_ESTIMATE);
    const fftwf_plan backward = fftwf_plan_r2r_2d(height, width, acc, work, FFTW_REDFT01, FFTW_REDFT01, FFTW_ESTIMATE);

    // beta u: how hard, and which way, each point's halo leans.
    std::vector<float> leanX, leanY;
    if (lean) {
        leanX.resize(n);
        leanY.resize(n);
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float dx = (x - obliquity->centreX) / pixelsPerMM;
                const float dy = (y - obliquity->centreY) / pixelsPerMM;
                const float r = std::sqrt(dx * dx + dy * dy);
                const size_t at = static_cast<size_t>(y) * width + x;
                if (r < 1e-6f) {
                    leanX[at] = leanY[at] = 0.f;
                    continue;
                }
                const float tilt = std::atan(r / obliquity->exitPupilMM) * static_cast<float>(180.0 / kPi);
                const float beta = obliqueBetaAt(table, tilt);
                leanX[at] = beta * dx / r;
                leanY[at] = beta * dy / r;
            }
        }
    }

    for (int b = 0; b < kChannels; ++b) {
        if (!bandUsed[b]) {
            continue;
        }
        const auto fill = [&](const std::vector<float>* modulation) {
#ifdef _OPENMP
            #pragma omp parallel for schedule(static) if (multiThread)
#endif
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t at = static_cast<size_t>(y) * width + x;
                    work[at] = src[b][y][x] * (modulation ? (*modulation)[at] : 1.f);
                }
            }
        };
        fill(nullptr);
        fftwf_execute_r2r(forward, work, coef[b]);
        if (bandLean[b]) {
            fill(&leanX);
            fftwf_execute_r2r(forward, work, coefX[b]);
            fill(&leanY);
            fftwf_execute_r2r(forward, work, coefY[b]);
        }
    }

    // Coefficient (u, v) is spatial frequency (u / 2W, v / 2H) cycles per
    // pixel; a halo scaled up by sizeMultiplier has H(size * nu), and the
    // lean's G -- a length -- size * G(size * nu).
    const double toIndex = static_cast<double>(sizeMultiplier) * pixelsPerMM / Transfer::kStep;
    const float norm = 1.f / (4.f * width * height);

    // Fills acc with sum_b factor[b] * transfer(nu) * coefficients[b].
    const auto multiply = [&](int k, bool useG, float* const* coefficients, bool subtract) {
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int v = 0; v < height; ++v) {
            const double fv = v / (2.0 * height);
            for (int u = 0; u < width; ++u) {
                const double fu = u / (2.0 * width);
                const double index = toIndex * std::sqrt(fu * fu + fv * fv);
                const size_t at = static_cast<size_t>(v) * width + u;
                float sum = 0.f;
                for (int b = 0; b < kChannels; ++b) {
                    if (!coefficients[b] || gain[b][k] == 0.f) {
                        continue;
                    }
                    if (useG) {
                        if (pairLean[b][k]) {
                            sum += gain[b][k] * sizeMultiplier * lookup(tr.g[b][k], index) * coefficients[b][at];
                        }
                    } else if (!tr.h[b][k].empty()) {
                        // minus H(0): the uniform-field share the stock curve already holds
                        sum += gain[b][k] * (lookup(tr.h[b][k], index) - (subtract ? tr.h[b][k][0] : 0.f)) * coefficients[b][at];
                    }
                }
                acc[at] = sum;
            }
        }
    };

    for (int k = 0; k < kChannels; ++k) {
        if (!layerUsed[k]) {
            continue;
        }
        multiply(k, false, coef, subtractUniform);
        fftwf_execute_r2r(backward, acc, work);
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) if (multiThread)
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                dst[k][y][x] += work[static_cast<size_t>(y) * width + x] * norm;
            }
        }

        if (!layerLean[k]) {
            continue;
        }
        // d/dx of G (x) (beta u_x src), then d/dy of G (x) (beta u_y src):
        // central differences, mirrored at the edges like the transform.
        const float dScale = norm * pixelsPerMM * 0.5f;
        for (int axis = 0; axis < 2; ++axis) {
            multiply(k, true, axis == 0 ? coefX : coefY, false);
            fftwf_execute_r2r(backward, acc, work);
#ifdef _OPENMP
            #pragma omp parallel for schedule(static) if (multiThread)
#endif
            for (int y = 0; y < height; ++y) {
                const float* row = work + static_cast<size_t>(y) * width;
                const float* up = work + static_cast<size_t>(std::max(y - 1, 0)) * width;
                const float* down = work + static_cast<size_t>(std::min(y + 1, height - 1)) * width;
                for (int x = 0; x < width; ++x) {
                    const float d = axis == 0 ? row[std::min(x + 1, width - 1)] - row[std::max(x - 1, 0)] : down[x] - up[x];
                    dst[k][y][x] += d * dScale;
                }
            }
        }
    }

    fftwf_destroy_plan(forward);
    fftwf_destroy_plan(backward);
    for (int b = 0; b < kChannels; ++b) {
        for (float* c : {coef[b], coefX[b], coefY[b]}) {
            if (c) {
                fftwf_free(c);
            }
        }
    }
    fftwf_free(acc);
    fftwf_free(work);
}

} // namespace filmhalation
} // namespace rtengine
