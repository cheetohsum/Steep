#pragma once

// Stuck-photosite detection on raw CFA data, before demosaicing.
//
// A stuck (hot) photosite is one sample that reads far too high while
// everything around it is normal. Demosaicing turns it into a pure red,
// green or blue dot, because only one colour channel carries the error.
// Real point detail -- a star, a catchlight, a glint, the tip of a hair --
// arrives through a lens, so it always spreads over several photosites and
// lifts the other colours around it too. Even a saturated red or blue light
// leaks a few percent into the neighbouring green photosites through the
// colour filters. The test leans on exactly that difference:
//
//   1. the sample must beat ALL of its nearest same-colour neighbours, by
//      many times their spread and many times the sensor noise at that
//      brightness;
//   2. the differently-coloured photosites touching it must sit at their
//      own background, within noise: the light went nowhere else.
//
// The sensor noise is measured from the frame itself (per colour, as a
// function of brightness) rather than from the eight neighbours, whose
// spread is far too unreliable to divide by.
//
// Values are expected in rawData units after scaleColors (black subtracted,
// white-balance pre-multipliers applied, full scale 65535), so the colours
// are roughly comparable with each other.
//
// Header-only so tools/stuck_pixel_tests.cc can score it on synthetic
// mosaics without linking rtengine.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace rtengine { namespace stuckpixels {

constexpr int kMaxSame = 12;

// A candidate whose leak score (see find) is above this looks more like a
// small coloured light than a defect. Real sensors put it high: a stuck
// photosite's neighbours run a little warm, so on an X-T4, 3% of its
// confirmed defects and 6% of fresh candidates score above 0.8 (18% and 28%
// above 0.5). On the synthetic set 24% of the coloured lights that fool the
// single-frame test score above 0.8, against 1% of defects. Used once a
// camera's own defects are known; a held-back defect is learned at its next
// sighting anyway.
constexpr float kAmbiguousLeak = 0.8f;
constexpr int kMaxOuter = 24;
constexpr int kMargin = 6; // the outer same-colour ring reaches 6 out

struct CfaLayout {
    int period = 0;                  // 2 = Bayer, 6 = X-Trans, 0 = unsupported
    unsigned char color[6][6] = {};  // 0 red, 1 green, 2 blue
};

struct Tuning {
    float isolation;  // deviation must exceed this x (brightest same-colour neighbour - their median)
    float noise;      // ... and this x the sensor noise at that brightness
    float floor;      // ... and this many rawData units
    float riseMax;    // a touching other-colour photosite may rise this fraction of the deviation ...
    float riseMean;   // ... and on average this fraction ...
    float riseNoise;  // ... plus this many of its own noise sigmas
    float riseMeanNoise; // ... and the average this many sigmas of the average
    float bumpNoise;  // the near neighbours may sit this many sigmas above the wider ring ...
    float bumpFrac;   // ... plus this fraction of the deviation
};

// sensitivity 0..100, 50 being the shipped default. Higher finds fainter
// stuck photosites; every setting still demands a single-photosite spike.
inline Tuning tuningFor(int sensitivity)
{
    const float s = std::max(0, std::min(100, sensitivity)) / 100.f;
    const float q = std::pow(0.25f, s);
    Tuning t;
    t.isolation = 11.f * q;                               // 11 .. 2.75  (5.5 at 50)
    t.noise = 16.f * q;                                   // 16 .. 4  (8 at 50)
    t.floor = 65535.f * 0.016f * std::pow(0.0625f, s);   // 1.6% .. 0.1% of full scale (0.4% at 50)
    t.riseMax = 0.015f + 0.03f * s;
    t.riseMean = 0.003f + 0.006f * s;
    t.riseNoise = 3.f;
    t.riseMeanNoise = 2.f;
    t.bumpNoise = 1.5f;
    t.bumpFrac = 0.04f + 0.08f * s;
    return t;
}

namespace detail {

struct Offset {
    int dr, dc;
};

struct Phase {
    Offset same[kMaxSame];
    int nSame = 0;
    Offset outer[kMaxOuter];
    int nOuter = 0;
    Offset cross[8];
    int nCross = 0;
    int colour = 0;
};

inline int wrap(int v, int p)
{
    return ((v % p) + p) % p;
}

// For every position of the CFA repeat: the nearest same-colour photosites
// (all of them at the cut-off distance, so the set stays symmetric), a wider
// ring of same-colour photosites 3.5 to 6 out that stands for the local
// background, and the touching photosites of any other colour.
inline std::vector<Phase> buildPhases(const CfaLayout& cfa)
{
    const int p = cfa.period;
    std::vector<Phase> phases(p * p);

    for (int pr = 0; pr < p; ++pr) {
        for (int pc = 0; pc < p; ++pc) {
            Phase& ph = phases[pr * p + pc];
            const int c = cfa.color[pr][pc];
            ph.colour = c;

            struct Cand {
                int d2;
                Offset o;
            };
            std::vector<Cand> cands;

            std::vector<Cand> ring;

            for (int dr = -6; dr <= 6; ++dr) {
                for (int dc = -6; dc <= 6; ++dc) {
                    if ((dr || dc) && cfa.color[wrap(pr + dr, p)][wrap(pc + dc, p)] == c) {
                        const int d2 = dr * dr + dc * dc;
                        if (std::abs(dr) <= 3 && std::abs(dc) <= 3) {
                            cands.push_back({d2, {dr, dc}});
                        }
                        if (d2 > 12 && d2 <= 36) {
                            ring.push_back({d2, {dr, dc}});
                        }
                    }
                }
            }

            const auto nearer = [](const Cand& a, const Cand& b) { return a.d2 < b.d2; };
            std::stable_sort(cands.begin(), cands.end(), nearer);
            std::stable_sort(ring.begin(), ring.end(), nearer);

            if (cands.size() >= 8) {
                const int cut = cands[7].d2;
                for (const Cand& cand : cands) {
                    if (cand.d2 > cut || ph.nSame == kMaxSame) {
                        break;
                    }
                    ph.same[ph.nSame++] = cand.o;
                }
            }

            for (const Cand& cand : ring) {
                if (ph.nOuter == kMaxOuter) {
                    break;
                }
                ph.outer[ph.nOuter++] = cand.o;
            }

            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    if ((dr || dc) && cfa.color[wrap(pr + dr, p)][wrap(pc + dc, p)] != c) {
                        ph.cross[ph.nCross++] = {dr, dc};
                    }
                }
            }
        }
    }

    return phases;
}

inline float median(float* v, int n)
{
    for (int i = 1; i < n; ++i) {
        const float x = v[i];
        int j = i - 1;
        for (; j >= 0 && v[j] > x; --j) {
            v[j + 1] = v[j];
        }
        v[j + 1] = x;
    }
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

template <typename Rows>
float sameColourMedian(const Rows& raw, int row, int col, const Phase& ph)
{
    float v[kMaxSame];
    for (int k = 0; k < ph.nSame; ++k) {
        v[k] = raw[row + ph.same[k].dr][col + ph.same[k].dc];
    }
    return median(v, ph.nSame);
}

// Sensor noise per colour as a function of brightness: the spread of
// "photosite minus the median of its same-colour neighbours", which is the
// very quantity the detector divides by. Measured on a sparse grid; the
// median of each brightness bin shrugs off texture and edges as long as
// most of the frame at that brightness is smooth, and anything left over
// only makes the detector more careful.
class NoiseModel
{
public:
    static constexpr int kLevelBins = 64;
    static constexpr int kResBins = 128;

    static int levelBin(float level)
    {
        const float x = std::log2(1.f + std::max(level, 0.f) / 16.f) * (kLevelBins / 12.f);
        return std::max(0, std::min(kLevelBins - 1, int(x)));
    }

    float sigma(int colour, float level) const
    {
        return sigma_[colour][levelBin(level)];
    }

    bool valid() const
    {
        return valid_;
    }

    template <typename Rows>
    void measure(const Rows& raw, int width, int height, const CfaLayout& cfa, const std::vector<Phase>& phases)
    {
        using Hist = std::vector<unsigned>;
        const int p = cfa.period;
        Hist total(3 * kLevelBins * kResBins, 0);

#ifdef _OPENMP
        #pragma omp parallel
#endif
        {
            Hist hist(total.size(), 0);

#ifdef _OPENMP
            #pragma omp for schedule(dynamic, 16) nowait
#endif
            for (int row = kMargin; row < height - kMargin; row += 3) {
                for (int col = kMargin + (row / 3) % 3; col < width - kMargin; col += 3) {
                    const Phase& ph = phases[(row % p) * p + col % p];
                    float v[kMaxSame];
                    bool clipped = false;
                    for (int k = 0; k < ph.nSame; ++k) {
                        v[k] = raw[row + ph.same[k].dr][col + ph.same[k].dc];
                        clipped = clipped || v[k] >= 65000.f;
                    }
                    if (clipped || raw[row][col] >= 65000.f) {
                        continue;
                    }
                    const float med = median(v, ph.nSame);
                    const float res = std::fabs(raw[row][col] - med);
                    const int rb = std::max(0, std::min(kResBins - 1, int(std::log2(1.f + res) * (kResBins / 16.f))));
                    ++hist[(ph.colour * kLevelBins + levelBin(med)) * kResBins + rb];
                }
            }

#ifdef _OPENMP
            #pragma omp critical
#endif
            for (size_t i = 0; i < total.size(); ++i) {
                total[i] += hist[i];
            }
        }

        valid_ = true;
        for (int c = 0; c < 3; ++c) {
            // median |residual| per brightness bin -> sigma
            std::array<float, kLevelBins> s;
            s.fill(-1.f);
            for (int lb = 0; lb < kLevelBins; ++lb) {
                const unsigned* h = &total[(c * kLevelBins + lb) * kResBins];
                unsigned n = 0;
                for (int rb = 0; rb < kResBins; ++rb) {
                    n += h[rb];
                }
                if (n < 64) {
                    continue;
                }
                unsigned acc = 0;
                int rb = 0;
                for (; rb < kResBins && 2 * (acc + h[rb]) < n; ++rb) {
                    acc += h[rb];
                }
                const float mad = std::exp2((rb + 0.5f) * (16.f / kResBins)) - 1.f;
                s[lb] = std::max(1.f, 1.4826f * mad);
            }

            // Shot noise only grows with brightness: carry the curve upward
            // through empty or quieter bins, and back-fill the darkest ones.
            float run = -1.f;
            for (int lb = 0; lb < kLevelBins; ++lb) {
                run = std::max(run, s[lb]);
                s[lb] = run;
            }
            if (run < 0.f) {
                valid_ = false;
                continue;
            }
            for (int lb = kLevelBins - 1; lb >= 0; --lb) {
                if (s[lb] < 0.f) {
                    s[lb] = s[lb + 1];
                }
            }
            sigma_[c] = s;
        }
    }

private:
    std::array<std::array<float, kLevelBins>, 3> sigma_ {};
    bool valid_ = false;
};

} // namespace detail

// Calls mark(row, col, strength, leak) for every stuck photosite and returns
// how many there were. strength is its deviation in multiples of the sensor
// noise there; leak (at most 1) is how close the touching other-colour
// photosites came to the rise that would have rejected it -- around 0 for a
// defect, whose neighbours only carry noise, and nearer 1 for a small
// coloured light that barely leaked into them. A camera memory uses it to
// tell a new defect from scene detail. Rows are handed out whole to one thread each, so mark() only has to
// be safe for concurrent calls on different rows. raw[row][col] must be
// readable for 0 <= row < height, 0 <= col < width; the outer kMargin
// photosites are never judged.
template <typename Rows, typename Mark>
int find(const Rows& raw, int width, int height, const CfaLayout& cfa, int sensitivity, Mark&& mark)
{
    if ((cfa.period != 2 && cfa.period != 6) || width <= 2 * kMargin || height <= 2 * kMargin) {
        return 0;
    }

    const std::vector<detail::Phase> phases = detail::buildPhases(cfa);

    for (const detail::Phase& ph : phases) {
        if (ph.nSame < 8 || ph.nOuter < 8) {
            return 0; // not a layout this test understands
        }
    }

    detail::NoiseModel noise;
    noise.measure(raw, width, height, cfa, phases);

    if (!noise.valid()) {
        return 0; // too small a frame to know its noise
    }

    const Tuning t = tuningFor(sensitivity);
    const int p = cfa.period;
    int count = 0;

#ifdef _OPENMP
    #pragma omp parallel for reduction(+:count) schedule(dynamic, 16)
#endif
    for (int row = kMargin; row < height - kMargin; ++row) {
        const detail::Phase* phaseRow = &phases[(row % p) * p];

        for (int col = kMargin; col < width - kMargin; ++col) {
            const detail::Phase& ph = phaseRow[col % p];
            const float v = raw[row][col];

            // Cheap gate first: most photosites are not the brightest of their
            // colour nearby, and the rest mostly don't clear the floor.
            float vals[kMaxSame];
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            for (int k = 0; k < ph.nSame; ++k) {
                vals[k] = raw[row + ph.same[k].dr][col + ph.same[k].dc];
                lo = std::min(lo, vals[k]);
                hi = std::max(hi, vals[k]);
            }

            if (v <= hi || v - lo <= t.floor) {
                continue;
            }

            const float med = detail::median(vals, ph.nSame);
            const float dev = v - med;

            const float sigma = noise.sigma(ph.colour, med);
            if (dev <= t.floor || dev <= t.isolation * (hi - med) || dev <= t.noise * sigma) {
                continue;
            }

            // Its neighbours must be the local background, not a bump: inside
            // the halo of a soft star they all sit well above the wider ring,
            // which hides the star from every test that looks only one step
            // out.
            float ringVals[kMaxOuter];
            for (int k = 0; k < ph.nOuter; ++k) {
                ringVals[k] = raw[row + ph.outer[k].dr][col + ph.outer[k].dc];
            }
            const float bg = detail::median(ringVals, ph.nOuter);

            // ... and it must stand taller above that background than any of
            // them does, by the same margin as above their median.
            if (med - bg > t.bumpNoise * noise.sigma(ph.colour, bg) + t.bumpFrac * dev || v - bg <= t.isolation * (hi - bg)) {
                continue;
            }

            // The other colours touching it must be sitting at their own
            // background. Anything the lens delivered would lift them too.
            bool lifted = false;
            float riseSum = 0.f;
            float varSum = 0.f;
            float leak = std::numeric_limits<float>::lowest();
            for (int k = 0; k < ph.nCross && !lifted; ++k) {
                const int r = row + ph.cross[k].dr;
                const int c = col + ph.cross[k].dc;
                const detail::Phase& nph = phases[(r % p) * p + c % p];
                const float nmed = detail::sameColourMedian(raw, r, c, nph);
                const float rise = raw[r][c] - nmed;
                const float sig = noise.sigma(nph.colour, nmed);
                const float limit = t.riseMax * dev + t.riseNoise * sig;
                lifted = rise > limit;
                leak = std::max(leak, rise / limit);
                riseSum += rise;
                varSum += sig * sig;
            }

            const float meanLimit = t.riseMean * dev * ph.nCross + t.riseMeanNoise * std::sqrt(varSum);
            if (lifted || (ph.nCross && riseSum > meanLimit)) {
                continue;
            }
            leak = ph.nCross ? std::max(leak, riseSum / meanLimit) : 0.f;

            mark(row, col, dev / std::max(sigma, 1.f), leak);
            ++count;
        }
    }

    return count;
}

}} // namespace rtengine::stuckpixels
