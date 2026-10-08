// Scores rtengine/stuckpixels.h on synthetic Bayer and X-Trans mosaics.
//   c++ -std=c++14 -O2 tools/stuck_pixel_tests.cc -o stuck-pixel-tests
//
// Every scene is noisy background plus the things the detector must leave
// alone -- lens-blurred stars of every colour, thin lines, hard edges -- and
// planted single-photosite spikes it must find. A spike only counts as
// found if its exact photosite is marked; any mark within 3 photosites of a
// real feature counts against the detector.
#include "../rtengine/stuckpixels.h"

#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rtengine;

namespace {

constexpr double kPi = 3.14159265358979323846;

const unsigned char kXtrans[6][6] = {
    {1, 1, 0, 1, 1, 2},
    {1, 1, 2, 1, 1, 0},
    {2, 0, 1, 0, 2, 1},
    {1, 1, 2, 1, 1, 0},
    {1, 1, 0, 1, 1, 2},
    {0, 2, 1, 2, 0, 1},
};

stuckpixels::CfaLayout bayer()
{
    stuckpixels::CfaLayout l;
    l.period = 2;
    l.color[0][0] = 0;
    l.color[0][1] = 1;
    l.color[1][0] = 1;
    l.color[1][1] = 2;
    return l;
}

stuckpixels::CfaLayout xtrans()
{
    stuckpixels::CfaLayout l;
    l.period = 6;
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) {
            l.color[r][c] = kXtrans[r][c];
        }
    }
    return l;
}

struct Plane {
    int w, h;
    std::vector<float> data;
    std::vector<float*> rows;

    Plane(int w, int h) : w(w), h(h), data(size_t(w) * h), rows(h)
    {
        for (int r = 0; r < h; ++r) {
            rows[r] = &data[size_t(r) * w];
        }
    }
    float* operator[](int r) const { return rows[r]; }
};

// Fraction of a unit-flux Gaussian (sigma s, centred at mu) that lands on
// the photosite spanning [x - 0.5, x + 0.5].
double cover(double x, double mu, double s)
{
    const double k = 1.0 / (s * std::sqrt(2.0));
    return 0.5 * (std::erf((x + 0.5 - mu) * k) - std::erf((x - 0.5 - mu) * k));
}

struct Scene {
    std::string name;
    stuckpixels::CfaLayout cfa;
    float background;   // rawData units
    float gain;         // units per electron: sets shot noise
    float readNoise;    // units
    float psf;          // lens blur sigma, photosites
};

struct Result {
    int planted = 0, found = 0;
    int stars = 0, starsHit = 0;
    int neutralLineMarks = 0, colourLineMarks = 0, edgeMarks = 0;
    int backgroundMarks = 0;
    int plantedByBin[4] = {}, foundByBin[4] = {};
    int starsHitByColour[5] = {};
    std::vector<float> defectLeaks, colouredLightLeaks; // leak score of each mark
};

const char* kBinNames[4] = {"<12 sigma", "12-25 sigma", "25-60 sigma", ">60 sigma"};

Result runScene(const Scene& sc, int sensitivity, unsigned seed, bool verbose)
{
    const int W = 768, H = 768;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    // Clean scene per photosite, plus a "near a feature" mask.
    Plane clean(W, H);
    enum : unsigned char { NEUTRAL_STAR = 1, COLOUR_STAR = 2, NEUTRAL_LINE = 4, COLOUR_LINE = 8, EDGE = 16 };
    std::vector<unsigned char> feature(size_t(W) * H, 0);
    const auto colourAt = [&](int r, int c) { return sc.cfa.color[r % sc.cfa.period][c % sc.cfa.period]; };

    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            // gentle gradient and low-frequency texture
            const double g = 1.0 + 0.25 * std::sin(c * 0.013) * std::cos(r * 0.017) + 0.15 * (double(c) / W - 0.5);
            clean[r][c] = float(sc.background * g);
        }
    }

    const auto markFeature = [&](int r0, int c0, int radius, unsigned char kind) {
        for (int r = std::max(0, r0 - radius); r <= std::min(H - 1, r0 + radius); ++r) {
            for (int c = std::max(0, c0 - radius); c <= std::min(W - 1, c0 + radius); ++c) {
                feature[size_t(r) * W + c] |= kind;
            }
        }
    };

    // Hard edges: a few vertical/horizontal/diagonal steps, lens-blurred.
    struct Edge {
        double nx, ny, off, step;
    };
    std::vector<Edge> edges;
    for (int i = 0; i < 3; ++i) {
        const double a = uni(rng) * kPi;
        edges.push_back({std::cos(a), std::sin(a), (0.2 + 0.6 * uni(rng)) * W, sc.background * (0.3 + 0.9 * uni(rng))});
    }
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            for (const Edge& e : edges) {
                const double d = c * e.nx + r * e.ny - e.off;
                clean[r][c] += float(e.step * 0.5 * (1.0 + std::erf(d / (sc.psf * std::sqrt(2.0)))));
                if (std::fabs(d) < 3.0) {
                    markFeature(r, c, 0, EDGE);
                }
            }
        }
    }

    // Thin lines (hair, wires): white or coloured, a fraction of a photosite wide.
    const float lineColours[4][3] = {{1, 1, 1}, {1, 0.12f, 0.04f}, {0.1f, 1, 0.1f}, {0.05f, 0.15f, 1}};
    for (int i = 0; i < 8; ++i) {
        const double a = uni(rng) * kPi;
        const double nx = std::cos(a), ny = std::sin(a);
        const double off = (0.1 + 0.8 * uni(rng)) * W;
        const double amp = sc.background * (2.0 + 20.0 * uni(rng)) + 3000.0 * uni(rng);
        const float* col = lineColours[i % 4];
        for (int r = 0; r < H; ++r) {
            for (int c = 0; c < W; ++c) {
                const double d = c * nx + r * ny - off;
                if (std::fabs(d) < 4.0) {
                    clean[r][c] += float(amp * col[colourAt(r, c)] * cover(d, 0.0, sc.psf));
                    if (std::fabs(d) < 3.0) {
                        markFeature(r, c, 0, i % 4 ? COLOUR_LINE : NEUTRAL_LINE);
                    }
                }
            }
        }
    }

    // Stars / point lights: sub-photosite positions, every colour, wide
    // brightness range, blurred only by the lens.
    const float starColours[5][3] = {{1, 1, 1}, {1, 0.12f, 0.04f}, {0.1f, 1, 0.1f}, {0.05f, 0.15f, 1}, {1, 0.8f, 0.5f}};
    struct Star {
        int r, c, colour;
    };
    std::vector<Star> starCentres;
    for (int i = 0; i < 400; ++i) {
        const double mx = 12 + uni(rng) * (W - 24), my = 12 + uni(rng) * (H - 24);
        const double peak = std::exp(std::log(sc.background * 0.5 + 400.0) + uni(rng) * std::log(60000.0 / (sc.background * 0.5 + 400.0)));
        const float* col = starColours[i % 5];
        for (int r = int(my) - 4; r <= int(my) + 4; ++r) {
            for (int c = int(mx) - 4; c <= int(mx) + 4; ++c) {
                const double f = cover(c, mx, sc.psf) * cover(r, my, sc.psf);
                clean[r][c] += float(peak * col[colourAt(r, c)] * f / (cover(0, 0, sc.psf) * cover(0, 0, sc.psf)));
            }
        }
        starCentres.push_back({int(my + 0.5), int(mx + 0.5), i % 5});
        markFeature(int(my + 0.5), int(mx + 0.5), 3, (i % 5 == 0 || i % 5 == 4) ? NEUTRAL_STAR : COLOUR_STAR);
    }

    // Noise, then clip like a sensor.
    Plane raw(W, H);
    std::normal_distribution<double> gauss(0.0, 1.0);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            const double level = std::max(0.f, clean[r][c]);
            const double sigma = std::sqrt(sc.gain * level + sc.readNoise * sc.readNoise);
            raw[r][c] = float(std::min(65535.0, std::max(0.0, level + sigma * gauss(rng))));
        }
    }

    // Planted stuck photosites, away from features and from each other.
    struct Planted {
        int r, c, bin;
    };
    std::vector<Planted> planted;
    std::vector<unsigned char> taken(size_t(W) * H, 0);
    for (int tries = 0; tries < 20000 && planted.size() < 300; ++tries) {
        const int r = 8 + int(uni(rng) * (H - 16));
        const int c = 8 + int(uni(rng) * (W - 16));
        bool clear = true;
        for (int dr = -6; dr <= 6 && clear; ++dr) {
            for (int dc = -6; dc <= 6 && clear; ++dc) {
                clear = !feature[size_t(r + dr) * W + c + dc] && !taken[size_t(r + dr) * W + c + dc];
            }
        }
        if (!clear) {
            continue;
        }
        const double level = std::max(0.f, clean[r][c]);
        const double sigma = std::sqrt(sc.gain * level + sc.readNoise * sc.readNoise);
        // 6 .. 400 sigma, log-uniform; a tenth of them stuck at full scale
        double amp = sigma * std::exp(std::log(6.0) + uni(rng) * std::log(400.0 / 6.0));
        if (planted.size() % 10 == 0) {
            amp = 65535.0 - raw[r][c];
        }
        const float before = raw[r][c];
        raw[r][c] = float(std::min(65535.0, raw[r][c] + amp));
        const double snr = (raw[r][c] - before) / sigma; // what survived clipping
        const int bin = snr < 12 ? 0 : snr < 25 ? 1 : snr < 60 ? 2 : 3;
        planted.push_back({r, c, bin});
        taken[size_t(r) * W + c] = 1;
    }

    std::vector<unsigned char> marks(size_t(W) * H, 0);
    std::vector<float> leakAt(size_t(W) * H, 0.f);
    stuckpixels::find(raw, W, H, sc.cfa, sensitivity, [&](int r, int c, float, float leak) {
        marks[size_t(r) * W + c] = 1;
        leakAt[size_t(r) * W + c] = leak;
    });

    Result res;
    for (const Planted& p : planted) {
        ++res.planted;
        ++res.plantedByBin[p.bin];
        if (marks[size_t(p.r) * W + p.c]) {
            ++res.found;
            ++res.foundByBin[p.bin];
            res.defectLeaks.push_back(leakAt[size_t(p.r) * W + p.c]);
        }
    }

    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            if (!marks[size_t(r) * W + c] || taken[size_t(r) * W + c]) {
                continue;
            }
            // marks on stars are counted per star below
            const unsigned char f = feature[size_t(r) * W + c];
            if (f & (NEUTRAL_STAR | COLOUR_STAR)) {
            } else if (f & NEUTRAL_LINE) {
                ++res.neutralLineMarks;
            } else if (f & COLOUR_LINE) {
                ++res.colourLineMarks;
            } else if (f & EDGE) {
                ++res.edgeMarks;
            } else {
                ++res.backgroundMarks;
            }
        }
    }

    // A mark within 3 photosites of a star counts against the NEAREST star
    // only, so a hit on a blue star doesn't also blame the white one beside it.
    std::vector<unsigned char> starHit(starCentres.size(), 0);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            if (!marks[size_t(r) * W + c] || taken[size_t(r) * W + c] || !(feature[size_t(r) * W + c] & (NEUTRAL_STAR | COLOUR_STAR))) {
                continue;
            }
            size_t nearest = 0;
            int best = std::numeric_limits<int>::max();
            for (size_t i = 0; i < starCentres.size(); ++i) {
                const int d2 = (starCentres[i].r - r) * (starCentres[i].r - r) + (starCentres[i].c - c) * (starCentres[i].c - c);
                if (d2 < best) {
                    best = d2;
                    nearest = i;
                }
            }
            starHit[nearest] = 1;
            if (starCentres[nearest].colour >= 1 && starCentres[nearest].colour <= 3) {
                res.colouredLightLeaks.push_back(leakAt[size_t(r) * W + c]);
            }
        }
    }
    for (size_t i = 0; i < starCentres.size(); ++i) {
        ++res.stars;
        res.starsHit += starHit[i];
        res.starsHitByColour[starCentres[i].colour] += starHit[i];
    }

    return res;
}

} // namespace

int main(int argc, char** argv)
{
    const int sensitivity = argc > 1 ? std::atoi(argv[1]) : 50;
    const bool verbose = argc > 2;

    std::vector<Scene> scenes;
    for (int x = 0; x < 2; ++x) {
        const auto cfa = x ? xtrans() : bayer();
        const std::string sensor = x ? "xtrans" : "bayer";
        // base ISO: ~4 units per electron; high ISO: 32x the gain
        for (float psf : {0.55f, 0.75f, 1.1f}) {
            scenes.push_back({sensor + " base-iso dark psf" + std::to_string(psf).substr(0, 4), cfa, 300.f, 4.f, 12.f, psf});
            scenes.push_back({sensor + " base-iso mid  psf" + std::to_string(psf).substr(0, 4), cfa, 6000.f, 4.f, 12.f, psf});
            scenes.push_back({sensor + " iso6400 dark psf" + std::to_string(psf).substr(0, 4), cfa, 300.f, 128.f, 60.f, psf});
            scenes.push_back({sensor + " iso6400 mid  psf" + std::to_string(psf).substr(0, 4), cfa, 6000.f, 128.f, 60.f, psf});
        }
    }

    std::printf("sensitivity %d\n", sensitivity);
    std::printf("%-30s %8s  %-40s %10s %7s %6s %6s\n", "scene", "recall", "recall by SNR (<12 / 12-25 / 25-60 / >60)", "stars hit", "lines", "edges", "bkgnd");
    std::printf("%-30s %8s  %-40s %10s %7s\n", "", "", "", "", "wht+col");

    const float psfs[3] = {0.55f, 0.75f, 1.1f};
    const auto psfIndex = [&](float psf) { return psf < 0.6f ? 0 : psf < 0.9f ? 1 : 2; };

    Result total;
    int starsHit[3][5] = {}; // [psf][colour]
    int starsPer[3] = {};
    unsigned seed = 1234;
    for (const Scene& sc : scenes) {
        const Result r = runScene(sc, sensitivity, seed++, verbose);
        char bins[64];
        std::snprintf(bins, sizeof bins, "%3d/%-3d %3d/%-3d %3d/%-3d %3d/%-3d", r.foundByBin[0], r.plantedByBin[0], r.foundByBin[1], r.plantedByBin[1],
                      r.foundByBin[2], r.plantedByBin[2], r.foundByBin[3], r.plantedByBin[3]);
        std::printf("%-30s %7.1f%%  %-40s %4d/%-5d %3d+%-3d %6d %6d\n", sc.name.c_str(), 100.0 * r.found / std::max(1, r.planted), bins, r.starsHit, r.stars,
                    r.neutralLineMarks, r.colourLineMarks, r.edgeMarks, r.backgroundMarks);
        total.planted += r.planted;
        total.found += r.found;
        total.neutralLineMarks += r.neutralLineMarks;
        total.colourLineMarks += r.colourLineMarks;
        total.edgeMarks += r.edgeMarks;
        total.backgroundMarks += r.backgroundMarks;
        for (int b = 0; b < 4; ++b) {
            total.plantedByBin[b] += r.plantedByBin[b];
            total.foundByBin[b] += r.foundByBin[b];
        }
        total.defectLeaks.insert(total.defectLeaks.end(), r.defectLeaks.begin(), r.defectLeaks.end());
        total.colouredLightLeaks.insert(total.colouredLightLeaks.end(), r.colouredLightLeaks.begin(), r.colouredLightLeaks.end());
        const int pi = psfIndex(sc.psf);
        starsPer[pi] += r.stars / 5;
        for (int k = 0; k < 5; ++k) {
            starsHit[pi][k] += r.starsHitByColour[k];
        }
    }

    std::printf("\nTOTAL recall %.1f%%", 100.0 * total.found / std::max(1, total.planted));
    for (int b = 0; b < 4; ++b) {
        std::printf("  [%s %.1f%%]", kBinNames[b], 100.0 * total.foundByBin[b] / std::max(1, total.plantedByBin[b]));
    }
    std::printf("\nmarks on white hairlines %d, on coloured hairlines %d, on edges %d, on plain background %d\n", total.neutralLineMarks,
                total.colourLineMarks, total.edgeMarks, total.backgroundMarks);
    std::printf("point lights touched   white    warm     red   green    blue\n");
    for (int pi = 0; pi < 3; ++pi) {
        std::printf("  psf %.2f (of %4d)  %6d  %6d  %6d  %6d  %6d\n", psfs[pi], starsPer[pi], starsHit[pi][0], starsHit[pi][4], starsHit[pi][1], starsHit[pi][2],
                    starsHit[pi][3]);
    }

    // How well the leak score separates a defect from a small coloured light
    // that fooled the single-frame test (what a camera memory leans on).
    const auto above = [](const std::vector<float>& v, float cut) {
        size_t n = 0;
        for (float x : v) {
            n += x > cut;
        }
        return v.empty() ? 0.0 : 100.0 * n / v.size();
    };
    std::printf("leak score above   0.5     0.7     0.8     0.9    (defects found %zu, coloured-light marks %zu)\n", total.defectLeaks.size(),
                total.colouredLightLeaks.size());
    for (int which = 0; which < 2; ++which) {
        const std::vector<float>& v = which ? total.colouredLightLeaks : total.defectLeaks;
        std::printf("  %-14s%5.1f%%  %5.1f%%  %5.1f%%  %5.1f%%\n", which ? "coloured light" : "defects", above(v, 0.5f), above(v, 0.7f), above(v, 0.8f),
                    above(v, 0.9f));
    }

    if (argc > 1) {
        return 0; // exploring another sensitivity: report only
    }

    // Gates for the shipped default. What a single frame cannot tell apart:
    // a dim, sharply focused, saturated-colour light leaks less into the
    // other colours than their noise, so it looks exactly like a stuck
    // photosite. Those are reported above, not gated.
    const auto recall = [&](int b0, int b1) {
        int f = 0, n = 0;
        for (int b = b0; b <= b1; ++b) {
            f += total.foundByBin[b];
            n += total.plantedByBin[b];
        }
        return double(f) / std::max(1, n);
    };
    int neutral = 0, softColoured = 0;
    for (int pi = 0; pi < 3; ++pi) {
        neutral += starsHit[pi][0] + starsHit[pi][4];
    }
    for (int k = 1; k <= 3; ++k) {
        softColoured += starsHit[2][k];
    }

    bool ok = true;
    const auto gate = [&](bool pass, const char* what) {
        std::printf("%s  %s\n", pass ? "PASS" : "FAIL", what);
        ok = ok && pass;
    };
    gate(recall(2, 3) >= 0.99, "finds >= 99% of stuck photosites 25+ sigma above the noise");
    gate(recall(1, 1) >= 0.90, "finds >= 90% of stuck photosites 12-25 sigma above the noise");
    gate(neutral == 0, "never touches a white or warm star / point light, however sharp");
    gate(softColoured <= starsPer[2] / 100, "leaves >= 99% of coloured point lights alone once the lens spreads them (psf 1.1)");
    gate(total.neutralLineMarks == 0, "never marks a white hairline");
    gate(total.edgeMarks == 0, "never marks an edge");
    gate(total.backgroundMarks <= int(scenes.size()), "at most 1 noise photosite per scene (harmless: it gets interpolated)");
    gate(above(total.defectLeaks, stuckpixels::kAmbiguousLeak) <= 3.0 && above(total.colouredLightLeaks, stuckpixels::kAmbiguousLeak) >= 15.0,
         "the leak score tells defects (<=3% ambiguous) from fooling coloured lights (>=15% ambiguous)");
    return ok ? 0 : 1;
}
