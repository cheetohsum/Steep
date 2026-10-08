// Offline Monte Carlo tracer for film halation kernels.
//
// Follows light through each film construction's layer stack -- emulsion
// layers that scatter and absorb, filter and anti-halation layers that only
// absorb, the base, the backing, the pressure plate behind it -- with
// Fresnel reflection and total internal reflection at every interface. Every
// packet of light absorbed by a sensitive layer is filed as "direct" or, if
// it ever crossed into the base, as "halation", by radius from where it
// entered. The halation tallies, divided by each layer's direct efficiency,
// are the kernels rtengine/filmhalation.h serves.
//
//   c++ -std=c++14 -O2 -fopenmp tools/halation_tracer.cc -o halation-tracer
//   halation-tracer --selftest            physics checks against closed forms
//   halation-tracer --report              per-construction summary
//   halation-tracer --generate rtengine/filmhalationtables.cc
//   halation-tracer --csv <dir>           radial profiles for plotting
//
// Built with rtengine/filmhalationtables.cc and -DHALATION_TABLES_LINKED it
// also offers --check-tables, which retraces everything and fails if the
// committed tables are stale. Generation is deterministic for a given photon
// count: the same inputs produce the same file byte for byte, whatever the
// thread count.
//
// The layer parameters below are physically plausible estimates, not
// manufacturer data (which is not published at this level). Thicknesses and
// refractive indices are the well-known ones; absorption and scattering are
// set so each layer takes a believable share of its band. Calibrate against
// scans of point lights on real stock and regenerate.

#include "../rtengine/filmhalation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace fh = rtengine::filmhalation;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int PANCHROMATIC = 3;
const char* const kBandNames = "RGB";

// ---------------------------------------------------------------------------
// Layer stacks

struct Layer {
    std::string name;
    double thicknessMM;
    double n;
    double mua[3]; // absorption per mm, per band (R, G, B)
    double mus[3]; // scattering per mm, per band
    double g;      // Henyey-Greenstein anisotropy
    int exposes;   // -1 inert, 0..2 that layer, PANCHROMATIC all three
    bool base;
};

double muFraction(double fractionPerPass, double thicknessMM)
{
    return -std::log(1.0 - fractionPerPass) / thicknessMM;
}

double muDensity(double opticalDensityPerPass, double thicknessMM)
{
    return opticalDensityPerPass * std::log(10.0) / thicknessMM;
}

Layer clearLayer(const char* name, double um, double n = 1.52)
{
    return {name, um * 1e-3, n, {0, 0, 0}, {0, 0, 0}, 0.0, -1, false};
}

// Absorption given as the fraction of each band one straight pass takes.
Layer sensitiveLayer(const char* name, double um, int exposes, const double take[3], const double scatter[3], double g)
{
    const double t = um * 1e-3;
    return {name, t, 1.52, {muFraction(take[0], t), muFraction(take[1], t), muFraction(take[2], t)},
            {scatter[0], scatter[1], scatter[2]}, g, exposes, false};
}

Layer absorberLayer(const char* name, double um, double n, double odR, double odG, double odB)
{
    const double t = um * 1e-3;
    return {name, t, n, {muDensity(odR, t), muDensity(odG, t), muDensity(odB, t)}, {0, 0, 0}, 0.0, -1, false};
}

Layer baseLayer(const char* name, double mm, double n, double odPerPass)
{
    const double mu = muDensity(odPerPass, mm);
    return {name, mm, n, {mu, mu, mu}, {0, 0, 0}, 0.0, -1, true};
}

struct Construction {
    std::string id;
    std::string summary;
    std::vector<Layer> layers; // exposure side first
    double plateAlbedo;        // Lambertian reflector behind the film (0 = nothing comes back)
    double plateGapMM;
    double fNumber;
};

struct Format {
    const char* suffix;
    double baseMM;
    double baseN;
    const char* baseName;
    double plateAlbedo;
    const char* behind;
};

const Format kFormats[3] = {
    {"35", 0.127, 1.48, "triacetate", 0.04, "black pressure plate"},
    {"120", 0.100, 1.48, "triacetate", 0.06, "black side of the backing paper"},
    {"sheet", 0.178, 1.65, "polyester", 0.04, "film holder"},
};

// Three-colour stack, exposure side first: blue layer, yellow filter, green,
// red. Scattering is the silver halide's, strongly forward (g 0.8) and
// stronger at shorter wavelengths; reversal emulsions are finer grained and
// scatter less. Halation comes out red because of this stack, not because we
// say so: blue dies in the yellow filter, green is mostly taken (and
// scattered back up) by the green layer, while the dye-sensitised red layer
// is a weak absorber, so most red light reaches the base -- and the red
// layer, lying on the base, is the first thing it meets on the way back.
void addColourStack(std::vector<Layer>& layers, bool reversal)
{
    const double scatter[3] = {reversal ? 95.0 : 130.0, reversal ? 150.0 : 200.0, reversal ? 190.0 : 260.0};
    const double blue[3] = {0.01, 0.04, 0.70};
    const double green[3] = {0.03, 0.70, 0.05};
    const double red[3] = {0.30, 0.10, 0.02};
    layers.push_back(clearLayer("supercoat", 1.5));
    layers.push_back(sensitiveLayer("blue-sensitive", 5.0, 2, blue, scatter, 0.8));
    layers.push_back(absorberLayer("yellow filter", 1.0, 1.52, 0.0, 0.01, 1.3));
    layers.push_back(sensitiveLayer("green-sensitive", 6.0, 1, green, scatter, 0.8));
    layers.push_back(clearLayer("interlayer", 1.0));
    layers.push_back(sensitiveLayer("red-sensitive", 6.0, 0, red, scatter, 0.8));
}

std::vector<Construction> buildConstructions()
{
    std::vector<Construction> all;
    const double fNumber = 2.8;

    // Order must match rtengine::filmhalation::Construction.
    const char* const kinds[5] = {"colour_negative", "reversal", "monochrome", "motion_remjet", "motion_remjet_removed"};

    for (int kind = 0; kind < 5; ++kind) {
        for (const Format& f : kFormats) {
            Construction c;
            c.id = std::string(kinds[kind]) + "_" + f.suffix;
            c.plateGapMM = 0.02;
            c.plateAlbedo = f.plateAlbedo;
            c.fNumber = fNumber;
            char base[96];
            std::snprintf(base, sizeof base, "%.0f um %s base (n %.2f)", f.baseMM * 1000, f.baseName, f.baseN);

            switch (kind) {
                case 0: // still colour negative: colloidal-silver anti-halation undercoat
                    addColourStack(c.layers, false);
                    c.layers.push_back(absorberLayer("anti-halation undercoat", 1.5, 1.52, 0.6, 0.6, 0.6));
                    c.layers.push_back(baseLayer("base", f.baseMM, f.baseN, 0.004));
                    c.layers.push_back(clearLayer("anti-curl backing", 5.0));
                    c.summary = std::string("colour negative, AHU OD 0.6, ") + base + ", " + f.behind;
                    break;
                case 1: // reversal: denser undercoat, finer emulsion
                    addColourStack(c.layers, true);
                    c.layers.push_back(absorberLayer("anti-halation undercoat", 1.5, 1.52, 0.8, 0.8, 0.8));
                    c.layers.push_back(baseLayer("base", f.baseMM, f.baseN, 0.004));
                    c.layers.push_back(clearLayer("anti-curl backing", 5.0));
                    c.summary = std::string("reversal, AHU OD 0.8, ") + base + ", " + f.behind;
                    break;
                case 2: { // black and white: one panchromatic layer on a grey base
                    const double take[3] = {0.40, 0.50, 0.55};
                    const double scatter[3] = {180, 200, 230};
                    c.layers.push_back(clearLayer("supercoat", 1.5));
                    c.layers.push_back(sensitiveLayer("panchromatic emulsion", 12.0, PANCHROMATIC, take, scatter, 0.8));
                    c.layers.push_back(baseLayer("grey base", f.baseMM, f.baseN, 0.15));
                    c.layers.push_back(clearLayer("anti-curl backing", 5.0));
                    c.summary = std::string("monochrome, grey base OD 0.15, ") + base + ", " + f.behind;
                    break;
                }
                case 3: // motion picture (ECN-2): no undercoat, carbon rem-jet on the back
                    addColourStack(c.layers, false);
                    c.layers.push_back(baseLayer("base", f.baseMM, f.baseN, 0.004));
                    c.layers.push_back(absorberLayer("rem-jet backing", 2.0, 1.50, 3.0, 3.0, 3.0));
                    c.summary = std::string("motion picture with rem-jet OD 3, ") + base + ", " + f.behind;
                    break;
                case 4: // the CineStill case: rem-jet washed off, bare base to air
                    addColourStack(c.layers, false);
                    c.layers.push_back(baseLayer("base", f.baseMM, f.baseN, 0.004));
                    c.summary = std::string("motion picture, rem-jet removed, bare ") + base + ", " + f.behind;
                    break;
            }
            all.push_back(c);
        }
    }
    return all;
}

// ---------------------------------------------------------------------------
// Radial bins: fine through the ring, coarser in the tail.

std::vector<double> buildEdges()
{
    std::vector<double> e;
    for (int i = 0; i <= 120; ++i) {
        e.push_back(0.005 * i); // to 0.6 mm
    }
    for (int i = 1; i <= 50; ++i) {
        e.push_back(0.6 + 0.02 * i); // to 1.6 mm
    }
    for (int i = 1; i <= 28; ++i) {
        e.push_back(1.6 + 0.05 * i); // to 3.0 mm
    }
    return e;
}

// ---------------------------------------------------------------------------
// Random numbers: xoshiro256**, seeded per chunk so results don't depend on
// thread scheduling.

uint64_t splitmix(uint64_t& x)
{
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Rng {
    uint64_t s[4];

    explicit Rng(uint64_t seed)
    {
        for (auto& v : s) {
            v = splitmix(seed);
        }
    }

    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    // uniform in (0, 1]
    double u()
    {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return ((result >> 11) + 1) * (1.0 / 9007199254740992.0);
    }
};

// ---------------------------------------------------------------------------
// Optics

// Unpolarised Fresnel reflectance from index n1 into n2 at incidence cosine
// cosi; cost receives the transmitted cosine (0 under total reflection).
double fresnel(double n1, double n2, double cosi, double& cost)
{
    if (n1 == n2) {
        cost = cosi;
        return 0.0;
    }
    const double ratio = n1 / n2;
    const double sint2 = ratio * ratio * (1.0 - cosi * cosi);
    if (sint2 >= 1.0) {
        cost = 0.0;
        return 1.0;
    }
    cost = std::sqrt(1.0 - sint2);
    const double rs = (n1 * cosi - n2 * cost) / (n1 * cosi + n2 * cost);
    const double rp = (n1 * cost - n2 * cosi) / (n1 * cost + n2 * cosi);
    return 0.5 * (rs * rs + rp * rp);
}

void scatterHG(double g, double& ux, double& uy, double& uz, Rng& rng)
{
    double cost;
    if (std::fabs(g) < 1e-6) {
        cost = 2.0 * rng.u() - 1.0;
    } else {
        const double s = (1.0 - g * g) / (1.0 - g + 2.0 * g * rng.u());
        cost = (1.0 + g * g - s * s) / (2.0 * g);
    }
    cost = std::max(-1.0, std::min(1.0, cost));
    const double sint = std::sqrt(1.0 - cost * cost);
    const double phi = 2.0 * kPi * rng.u();
    const double cp = std::cos(phi), sp = std::sin(phi);

    if (std::fabs(uz) > 0.99999) {
        ux = sint * cp;
        uy = sint * sp;
        uz = uz > 0 ? cost : -cost;
    } else {
        const double tmp = std::sqrt(1.0 - uz * uz);
        const double nx = sint * (ux * uz * cp - uy * sp) / tmp + ux * cost;
        const double ny = sint * (uy * uz * cp + ux * sp) / tmp + uy * cost;
        const double nz = -sint * cp * tmp + uz * cost;
        ux = nx;
        uy = ny;
        uz = nz;
    }
}

// ---------------------------------------------------------------------------
// Tracing

struct Stack {
    const Construction* c;
    std::vector<double> zTop, zBot;
    int bottomBase = -1;

    explicit Stack(const Construction& con) : c(&con)
    {
        double z = 0;
        for (const Layer& l : con.layers) {
            zTop.push_back(z);
            z += l.thicknessMM;
            zBot.push_back(z);
        }
    }
};

struct Tally {
    static constexpr int kMap = 300;          // 2D halo map: kMap x kMap cells ...
    static constexpr double kMapCell = 0.02;  // ... of this many mm, centred on the entry point
    std::vector<double> halo[3], direct[3]; // per exposed channel, per radial bin (last = beyond the grid)
    std::vector<double> map;                // red-layer halo on the 2D map, only when asked for
    double haloTotal[3] = {}, directTotal[3] = {};
    double sensitive = 0, entryReflected = 0, escapedTop = 0, inert = 0, lostBack = 0;

    explicit Tally(size_t bins = 0)
    {
        for (int c = 0; c < 3; ++c) {
            halo[c].assign(bins + 1, 0.0);
            direct[c].assign(bins + 1, 0.0);
        }
    }

    void add(const Tally& o)
    {
        for (int c = 0; c < 3; ++c) {
            for (size_t i = 0; i < halo[c].size(); ++i) {
                halo[c][i] += o.halo[c][i];
                direct[c][i] += o.direct[c][i];
            }
            haloTotal[c] += o.haloTotal[c];
            directTotal[c] += o.directTotal[c];
        }
        if (!o.map.empty()) {
            if (map.empty()) {
                map.assign(o.map.size(), 0.0);
            }
            for (size_t i = 0; i < map.size(); ++i) {
                map[i] += o.map[i];
            }
        }
        sensitive += o.sensitive;
        entryReflected += o.entryReflected;
        escapedTop += o.escapedTop;
        inert += o.inert;
        lostBack += o.lostBack;
    }
};

struct Launch {
    enum Kind { Lens, Lambertian } kind = Lens;
    int layer = 0;       // Lambertian: starts at the top of this layer, heading down
    bool viaBase = false;
    double tilt = 0.0;   // Lens: chief ray's angle off the film normal (radians), leaning towards +x
};

class Tracer
{
public:
    Tracer(const Construction& c, const std::vector<double>& edges) : st_(c), edges_(edges) {}

    void photon(int band, Rng& rng, Tally& t, const Launch& launch) const
    {
        const Construction& c = *st_.c;
        const int nL = static_cast<int>(c.layers.size());
        double x = 0, y = 0, z, ux, uy, uz, w = 1.0;
        int li;
        bool viaBase = launch.viaBase;

        if (launch.kind == Launch::Lens) {
            // Uniform over the exit pupil around the chief ray, refracted into
            // the supercoat.
            const double tanTheta = std::sqrt(rng.u()) / (2.0 * c.fNumber);
            const double cosCone = 1.0 / std::sqrt(1.0 + tanTheta * tanTheta);
            const double sinCone = std::sqrt(1.0 - cosCone * cosCone);
            const double phi = 2.0 * kPi * rng.u();
            const double ax = sinCone * std::cos(phi), ay = sinCone * std::sin(phi), az = cosCone;
            const double ct = std::cos(launch.tilt), st = std::sin(launch.tilt);
            const double dx = ax * ct + az * st, dy = ay, dz = -ax * st + az * ct; // tilt about y
            double cost;
            const double R = fresnel(1.0, c.layers[0].n, dz, cost);
            t.entryReflected += R;
            w = 1.0 - R;
            ux = dx / c.layers[0].n;
            uy = dy / c.layers[0].n;
            uz = cost;
            li = 0;
            z = 0.0;
        } else {
            const double cz = std::sqrt(rng.u()); // cosine-weighted
            const double sz = std::sqrt(1.0 - cz * cz);
            const double phi = 2.0 * kPi * rng.u();
            ux = sz * std::cos(phi);
            uy = sz * std::sin(phi);
            uz = cz;
            li = launch.layer;
            z = st_.zTop[li];
        }

        for (int guard = 0; guard < 1000000; ++guard) {
            const Layer& ly = c.layers[li];
            const double mua = ly.mua[band], mus = ly.mus[band], mut = mua + mus;
            const double dist = uz > 0 ? (st_.zBot[li] - z) / uz : uz < 0 ? (st_.zTop[li] - z) / uz : kInf;

            if (mus > 0) {
                const double s = -std::log(rng.u()) / mut;
                if (s < dist) {
                    x += ux * s;
                    y += uy * s;
                    z += uz * s;
                    const double a = w * mua / mut;
                    deposit(ly, a, x, y, viaBase, t);
                    w -= a;
                    scatterHG(ly.g, ux, uy, uz, rng);
                    if (!roulette(w, rng)) {
                        return;
                    }
                    continue;
                }
            } else if (dist == kInf) {
                t.inert += w; // skimming a clear layer forever
                return;
            } else if (mua > 0) {
                const double a = w * (1.0 - std::exp(-mua * dist));
                deposit(ly, a, x, y, viaBase, t);
                w -= a;
            }

            x += ux * dist;
            y += uy * dist;
            z = uz > 0 ? st_.zBot[li] : st_.zTop[li];
            if (!roulette(w, rng)) {
                return;
            }

            const bool down = uz > 0;
            const int next = down ? li + 1 : li - 1;
            const double n1 = ly.n;
            const double n2 = (next < 0 || next >= nL) ? 1.0 : c.layers[next].n;
            double cost;
            if (rng.u() < fresnel(n1, n2, std::fabs(uz), cost)) {
                uz = -uz;
                continue;
            }
            const double k = n1 / n2;
            ux *= k;
            uy *= k;
            uz = down ? cost : -cost;

            if (next < 0) {
                t.escapedTop += w;
                return;
            }

            if (next >= nL) {
                if (!pressurePlate(rng, t, x, y, ux, uy, uz, w)) {
                    return;
                }
                li = nL - 1;
                z = st_.zBot[li];
                viaBase = true; // it left through the base to get out there
                continue;
            }

            li = next;
            if (c.layers[li].base) {
                viaBase = true;
            }
        }
        t.inert += w; // should never get here
    }

    Tally run(int band, uint64_t photons, uint64_t seed, const Launch& launch = Launch(), bool withMap = false) const
    {
        constexpr uint64_t chunk = 1 << 16;
        const uint64_t chunks = (photons + chunk - 1) / chunk;
        std::vector<Tally> part(chunks, Tally(edges_.size() - 1));
        if (withMap) {
            for (Tally& p : part) {
                p.map.assign(size_t(Tally::kMap) * Tally::kMap, 0.0);
            }
        }

#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic, 1)
#endif
        for (long long i = 0; i < static_cast<long long>(chunks); ++i) {
            uint64_t s = seed ^ (uint64_t(band + 1) << 48) ^ (uint64_t(i) * 0x100000001B3ull);
            Rng rng(splitmix(s));
            const uint64_t n = std::min(chunk, photons - uint64_t(i) * chunk);
            for (uint64_t p = 0; p < n; ++p) {
                photon(band, rng, part[i], launch);
            }
        }

        Tally total(edges_.size() - 1);
        for (const Tally& p : part) {
            total.add(p); // fixed order: deterministic sums
        }
        return total;
    }

    const std::vector<double>& edges() const { return edges_; }

private:
    Stack st_;
    std::vector<double> edges_;

    static bool roulette(double& w, Rng& rng)
    {
        if (w >= 1e-4) {
            return true;
        }
        if (rng.u() < 0.1) {
            w *= 10.0;
            return true;
        }
        return false;
    }

    size_t bin(double r) const
    {
        const auto it = std::upper_bound(edges_.begin(), edges_.end(), r);
        if (it == edges_.end()) {
            return edges_.size() - 1; // beyond the grid
        }
        return static_cast<size_t>(std::max<std::ptrdiff_t>(0, it - edges_.begin() - 1));
    }

    void deposit(const Layer& ly, double a, double x, double y, bool viaBase, Tally& t) const
    {
        if (a <= 0) {
            return;
        }
        if (ly.exposes < 0) {
            t.inert += a;
            return;
        }
        t.sensitive += a;
        const size_t b = bin(std::sqrt(x * x + y * y));
        const int c0 = ly.exposes == PANCHROMATIC ? 0 : ly.exposes;
        const int c1 = ly.exposes == PANCHROMATIC ? 2 : ly.exposes;
        for (int c = c0; c <= c1; ++c) {
            if (viaBase) {
                t.halo[c][b] += a;
                t.haloTotal[c] += a;
                if (c == 0 && !t.map.empty()) {
                    const int ix = static_cast<int>(std::floor(x / Tally::kMapCell + Tally::kMap / 2));
                    const int iy = static_cast<int>(std::floor(y / Tally::kMapCell + Tally::kMap / 2));
                    if (ix >= 0 && iy >= 0 && ix < Tally::kMap && iy < Tally::kMap) {
                        t.map[static_cast<size_t>(iy) * Tally::kMap + ix] += a;
                    }
                }
            } else {
                t.direct[c][b] += a;
                t.directTotal[c] += a;
            }
        }
    }

    // Out through the rear into the gap: part is absorbed by what sits behind
    // the film, the rest comes back diffusely and may bounce off the film's
    // rear surface again before it gets in. Returns false if nothing is left.
    bool pressurePlate(Rng& rng, Tally& t, double& x, double& y, double& ux, double& uy, double& uz, double& w) const
    {
        const Construction& c = *st_.c;
        const double gap = c.plateGapMM;
        const double nBack = c.layers.back().n;

        for (int bounce = 0; bounce < 1000; ++bounce) {
            x += ux / uz * gap;
            y += uy / uz * gap;
            t.lostBack += w * (1.0 - c.plateAlbedo);
            w *= c.plateAlbedo;
            if (w <= 0 || !roulette(w, rng)) {
                return false;
            }
            const double cz = std::sqrt(rng.u());
            const double sz = std::sqrt(1.0 - cz * cz);
            const double phi = 2.0 * kPi * rng.u();
            ux = sz * std::cos(phi);
            uy = sz * std::sin(phi);
            x += ux / cz * gap;
            y += uy / cz * gap;
            double cost;
            if (rng.u() < fresnel(1.0, nBack, cz, cost)) {
                uz = cz; // off the film's rear surface, back towards the plate
                continue;
            }
            ux /= nBack;
            uy /= nBack;
            uz = -cost;
            return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Kernels

struct Kernels {
    const Construction* c;
    std::vector<double> edges;
    std::vector<double> k[3][3]; // [band][layer] per mm^2, per unit of the layer's own direct exposure
    double total[3][3] = {};
    double beyond[3][3] = {};    // share of the halo past the grid
    double direct[3][3] = {};    // fraction of band b absorbed directly by layer k
    double haloFraction[3][3] = {};
    double directMedianMM[3] = {}; // emulsion's own spread, for the report
    double obliqueBeta[4] = {};    // lopsidedness of the red halo at 0/10/20/30 degrees of chief-ray tilt
};

constexpr double kObliqueDegrees[4] = {0.0, 10.0, 20.0, 30.0};

// Light reaching the film at an angle (towards the frame's corners) halates
// lopsidedly: the ring keeps its radius but grows brighter on the side the
// light leans towards. K(r, phi) ~ K0(r) (1 + beta cos phi) with one beta per
// angle fits the traced halo to within the Monte Carlo noise (see --oblique),
// so beta is all the tables need to carry.
void measureObliquity(Kernels& K, const std::vector<double>& edges, uint64_t photons, uint64_t seed)
{
    const Tracer tracer(*K.c, edges);
    const int M = Tally::kMap;
    const double cell = Tally::kMapCell;
    for (int a = 1; a < 4; ++a) {
        Launch launch;
        launch.tilt = kObliqueDegrees[a] * kPi / 180.0;
        const std::vector<double> map = tracer.run(0, photons, seed + a, launch, true).map;
        double sum = 0, lean = 0;
        for (int iy = 0; iy < M; ++iy) {
            for (int ix = 0; ix < M; ++ix) {
                const double x = (ix + 0.5 - M / 2.0) * cell, y = (iy + 0.5 - M / 2.0) * cell;
                const double r = std::hypot(x, y);
                const double v = map[size_t(iy) * M + ix];
                sum += v;
                lean += v * x / r;
            }
        }
        K.obliqueBeta[a] = sum > 0 ? 2.0 * lean / sum : 0.0;
    }
}

double ringEdge(double baseMM, double n)
{
    return 2.0 * baseMM * std::tan(std::asin(1.0 / n));
}

Kernels trace(const Construction& c, const std::vector<double>& edges, uint64_t photons, uint64_t seed)
{
    Kernels K;
    K.c = &c;
    K.edges = edges;
    const Tracer tracer(c, edges);
    const size_t bins = edges.size() - 1;
    std::vector<double> haloDensity[3][3];

    for (int b = 0; b < 3; ++b) {
        const Tally t = tracer.run(b, photons, seed);
        for (int k = 0; k < 3; ++k) {
            K.direct[b][k] = t.directTotal[k] / photons;
            K.haloFraction[b][k] = t.haloTotal[k] / photons;
            haloDensity[b][k].assign(bins, 0.0);
            for (size_t i = 0; i < bins; ++i) {
                const double area = kPi * (edges[i + 1] * edges[i + 1] - edges[i] * edges[i]);
                haloDensity[b][k][i] = t.halo[k][i] / (photons * area);
            }
            if (t.haloTotal[k] > 0) {
                K.beyond[b][k] = t.halo[k][bins] / t.haloTotal[k];
            }
            if (b == k && t.directTotal[k] > 0) {
                double acc = 0;
                for (size_t i = 0; i <= bins; ++i) {
                    acc += t.direct[k][i];
                    if (acc >= 0.5 * t.directTotal[k]) {
                        K.directMedianMM[k] = i < bins ? edges[i + 1] : edges.back();
                        break;
                    }
                }
            }
        }
    }

    for (int b = 0; b < 3; ++b) {
        for (int k = 0; k < 3; ++k) {
            K.k[b][k].assign(bins, 0.0);
            const double own = K.direct[k][k];
            if (own <= 0) {
                continue;
            }
            for (size_t i = 0; i < bins; ++i) {
                K.k[b][k][i] = haloDensity[b][k][i] / own;
                K.total[b][k] += K.k[b][k][i] * kPi * (edges[i + 1] * edges[i + 1] - edges[i] * edges[i]);
            }
        }
    }
    return K;
}

// Radius of the kernel's density peak, ignoring the very centre where the
// near-normal Fresnel return piles up.
double peakRadius(const Kernels& K, int b, int k, double fromMM)
{
    double best = -1, at = 0;
    for (size_t i = 0; i < K.k[b][k].size(); ++i) {
        const double r = 0.5 * (K.edges[i] + K.edges[i + 1]);
        if (r >= fromMM && K.k[b][k][i] > best) {
            best = K.k[b][k][i];
            at = r;
        }
    }
    return at;
}

double energyWithin(const Kernels& K, int b, int k, double mm)
{
    double acc = 0;
    for (size_t i = 0; i < K.k[b][k].size() && K.edges[i + 1] <= mm + 1e-12; ++i) {
        acc += K.k[b][k][i] * kPi * (K.edges[i + 1] * K.edges[i + 1] - K.edges[i] * K.edges[i]);
    }
    return K.total[b][k] > 0 ? acc / K.total[b][k] : 0.0;
}

double baseOf(const Construction& c, double& n)
{
    for (const Layer& l : c.layers) {
        if (l.base) {
            n = l.n;
            return l.thicknessMM;
        }
    }
    n = 1.5;
    return 0;
}

constexpr double kNegligible = 1e-4; // below this a kernel cannot show even around extreme lights

// Mean kernel density between two radii (area-weighted).
double meanDensity(const Kernels& K, int b, int k, double fromMM, double toMM)
{
    double e = 0, a = 0;
    for (size_t i = 0; i < K.k[b][k].size(); ++i) {
        if (K.edges[i] >= fromMM - 1e-12 && K.edges[i + 1] <= toMM + 1e-12) {
            const double area = kPi * (K.edges[i + 1] * K.edges[i + 1] - K.edges[i] * K.edges[i]);
            e += K.k[b][k][i] * area;
            a += area;
        }
    }
    return a > 0 ? e / a : 0.0;
}

// ---------------------------------------------------------------------------
// Output

// A C++ float literal: %.5g alone can print "0" or "12", which need a point.
std::string lit(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.5g", v);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    return s + "f";
}

void report(const std::vector<Kernels>& all)
{
    std::printf("%-30s %9s %9s %6s %9s %8s  %-24s %s\n", "construction", "ring edge", "RR peak", "ring", "RR total", "GG/RR", "RR energy <0.25/0.5/1mm",
                "direct R/G/B");
    for (const Kernels& K : all) {
        double n;
        const double t = baseOf(*K.c, n);
        const double rc = ringEdge(t, n);
        const double contrast = meanDensity(K, 0, 0, rc, 1.3 * rc) / std::max(meanDensity(K, 0, 0, 0.4 * rc, 0.85 * rc), 1e-12);
        std::printf("%-30s %7.3fmm %7.3fmm %5.1fx %9.5f %8.3f  %5.0f%% %5.0f%% %5.0f%%       %.2f/%.2f/%.2f\n", K.c->id.c_str(), rc,
                    peakRadius(K, 0, 0, 0.5 * rc), contrast, K.total[0][0], K.total[1][1] / std::max(K.total[0][0], 1e-12),
                    100 * energyWithin(K, 0, 0, 0.25), 100 * energyWithin(K, 0, 0, 0.5), 100 * energyWithin(K, 0, 0, 1.0), K.direct[0][0],
                    K.direct[1][1], K.direct[2][2]);
    }
    std::printf("(ring = mean density just outside the edge over the gap inside it; a centre-peaked blob scores below 1)\n");
    std::printf("\noff-axis lopsidedness beta (halo K(r)(1 + beta cos phi)) at 10/20/30 degrees of chief-ray tilt:\n");
    for (const Kernels& K : all) {
        std::printf("%-30s %.2f %.2f %.2f\n", K.c->id.c_str(), K.obliqueBeta[1], K.obliqueBeta[2], K.obliqueBeta[3]);
    }
    std::printf("\nkernel totals [band -> layer], the halo each layer gets relative to its own direct exposure:\n");
    for (const Kernels& K : all) {
        std::printf("%-30s", K.c->id.c_str());
        for (int b = 0; b < 3; ++b) {
            for (int k = 0; k < 3; ++k) {
                std::printf(" %c%c %.2e", kBandNames[b], kBandNames[k], K.total[b][k]);
            }
        }
        std::printf("\n");
    }
}

bool writeCsv(const std::vector<Kernels>& all, const std::string& dir)
{
    for (const Kernels& K : all) {
        const std::string path = dir + "/" + K.c->id + ".csv";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            return false;
        }
        std::fprintf(f, "r_mm");
        for (int b = 0; b < 3; ++b) {
            for (int k = 0; k < 3; ++k) {
                std::fprintf(f, ",%c%c", kBandNames[b], kBandNames[k]);
            }
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i + 1 < K.edges.size(); ++i) {
            std::fprintf(f, "%.4f", 0.5 * (K.edges[i] + K.edges[i + 1]));
            for (int b = 0; b < 3; ++b) {
                for (int k = 0; k < 3; ++k) {
                    std::fprintf(f, ",%.6g", K.k[b][k][i]);
                }
            }
            std::fprintf(f, "\n");
        }
        std::fclose(f);
    }
    return true;
}

bool generate(const std::vector<Kernels>& all, const std::string& path, uint64_t photons, uint64_t seed)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    const std::vector<double>& edges = all.front().edges;

    std::fprintf(f, "// GENERATED by tools/halation_tracer.cc --generate. Do not edit: change the\n");
    std::fprintf(f, "// construction in the tracer and regenerate. %llu photons per band, seed %llu.\n",
                 static_cast<unsigned long long>(photons), static_cast<unsigned long long>(seed));
    std::fprintf(f, "//\n");
    for (const Kernels& K : all) {
        std::fprintf(f, "// %-28s %s\n", K.c->id.c_str(), K.c->summary.c_str());
    }
    std::fprintf(f, "\n#include \"filmhalation.h\"\n\nnamespace rtengine {\nnamespace filmhalation {\n\nnamespace {\n\n");

    std::fprintf(f, "const float kEdges[] = {");
    for (size_t i = 0; i < edges.size(); ++i) {
        std::fprintf(f, "%s%s%.3ff", i ? "," : "", i % 12 ? " " : "\n    ", edges[i]);
    }
    std::fprintf(f, "\n};\n\n");

    for (const Kernels& K : all) {
        for (int b = 0; b < 3; ++b) {
            for (int k = 0; k < 3; ++k) {
                if (K.total[b][k] < kNegligible) {
                    continue;
                }
                std::fprintf(f, "const float k_%s_%c%c[] = {", K.c->id.c_str(), kBandNames[b], kBandNames[k]);
                for (size_t i = 0; i < K.k[b][k].size(); ++i) {
                    std::fprintf(f, "%s%s%s", i ? "," : "", i % 8 ? " " : "\n    ", lit(K.k[b][k][i]).c_str());
                }
                std::fprintf(f, "\n};\n");
            }
        }
        std::fprintf(f, "\n");
    }

    std::fprintf(f, "const HaloTable kTables[] = {\n");
    for (const Kernels& K : all) {
        double n;
        const double t = baseOf(*K.c, n);
        std::fprintf(f, "    {\"%s\", %.3ff, %.2ff, %.4ff, %d, kEdges,\n        {", K.c->id.c_str(), t, n, ringEdge(t, n),
                     static_cast<int>(edges.size() - 1));
        for (int b = 0; b < 3; ++b) {
            std::fprintf(f, "%s{", b ? ", " : "");
            for (int k = 0; k < 3; ++k) {
                if (K.total[b][k] < kNegligible) {
                    std::fprintf(f, "%snullptr", k ? ", " : "");
                } else {
                    std::fprintf(f, "%sk_%s_%c%c", k ? ", " : "", K.c->id.c_str(), kBandNames[b], kBandNames[k]);
                }
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "},\n        {");
        for (int b = 0; b < 3; ++b) {
            std::fprintf(f, "%s{", b ? ", " : "");
            for (int k = 0; k < 3; ++k) {
                std::fprintf(f, "%s%s", k ? ", " : "", lit(K.total[b][k] < kNegligible ? 0.0 : K.total[b][k]).c_str());
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "},\n        {%s, %s, %s, %s}},\n", lit(K.obliqueBeta[0]).c_str(), lit(K.obliqueBeta[1]).c_str(),
                     lit(K.obliqueBeta[2]).c_str(), lit(K.obliqueBeta[3]).c_str());
    }
    std::fprintf(f, "};\n\n");
    std::fprintf(f, "static_assert(sizeof(kTables) / sizeof(kTables[0]) == static_cast<size_t>(Construction::Count),\n");
    std::fprintf(f, "              \"one table per construction, in enum order\");\n\n");
    std::fprintf(f, "} // namespace\n\n");
    std::fprintf(f, "const HaloTable& table(Construction construction)\n{\n");
    std::fprintf(f, "    return kTables[static_cast<int>(construction)];\n}\n\n");
    std::fprintf(f, "} // namespace filmhalation\n} // namespace rtengine\n");
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Oblique incidence: how the halo changes where light reaches the film at an
// angle (towards the corners of the frame), and whether the upright ring,
// simply shifted outwards, is a fair stand-in for it.

void oblique(const std::vector<Construction>& cons, const std::vector<double>& edges, uint64_t photons)
{
    const int M = Tally::kMap;
    const double cell = Tally::kMapCell;
    const double half = M / 2.0;
    const double angles[] = {0.0, 10.0, 20.0, 30.0};

    for (fh::Construction which : {fh::Construction::MotionRemjetRemoved35, fh::Construction::ColourNegative35}) {
        const Construction& c = cons[static_cast<int>(which)];
        const Tracer tracer(c, edges);
        std::vector<std::vector<double>> maps;
        for (double deg : angles) {
            Launch launch;
            launch.tilt = deg * kPi / 180.0;
            maps.push_back(tracer.run(0, photons, 77, launch, true).map);
        }

        // Upright radial profile, per map cell, 0.01 mm steps.
        const int bins = 420;
        std::vector<double> prof(bins, 0.0), count(bins, 0.0);
        for (int iy = 0; iy < M; ++iy) {
            for (int ix = 0; ix < M; ++ix) {
                const double r = std::hypot((ix + 0.5 - half) * cell, (iy + 0.5 - half) * cell);
                const int b = static_cast<int>(r / 0.01);
                if (b < bins) {
                    prof[b] += maps[0][size_t(iy) * M + ix];
                    count[b] += 1;
                }
            }
        }
        for (int b = 0; b < bins; ++b) {
            prof[b] = count[b] > 0 ? prof[b] / count[b] : 0.0;
        }
        const auto upright = [&](double r) {
            const double f = r / 0.01 - 0.5;
            if (f <= 0) {
                return prof[0];
            }
            const int i = static_cast<int>(f);
            return i + 1 < bins ? prof[i] + (prof[i + 1] - prof[i]) * (f - i) : 0.0;
        };
        const auto residual = [&](const std::vector<double>& map, double shift) {
            double total = 0, predicted = 0;
            for (double v : map) {
                total += v;
            }
            std::vector<double> p(map.size());
            for (int iy = 0; iy < M; ++iy) {
                for (int ix = 0; ix < M; ++ix) {
                    const double x = (ix + 0.5 - half) * cell, y = (iy + 0.5 - half) * cell;
                    p[size_t(iy) * M + ix] = upright(std::hypot(x - shift, y));
                    predicted += p[size_t(iy) * M + ix];
                }
            }
            double err = 0;
            for (size_t i = 0; i < map.size(); ++i) {
                err += std::fabs(map[i] - p[i] * total / std::max(predicted, 1e-30));
            }
            return err / std::max(total, 1e-30);
        };

        double totalUpright = 0;
        for (double v : maps[0]) {
            totalUpright += v;
        }
        std::printf("\n%s (red layer halo)\n", c.id.c_str());
        std::printf("%7s %8s %10s %8s %10s %10s %11s %20s\n", "tilt", "energy", "centroid", "out/in", "peak out", "peak in", "fit as-is",
                    "fit shifted");
        for (size_t a = 0; a < maps.size(); ++a) {
            const std::vector<double>& map = maps[a];
            double total = 0, mx = 0, plus = 0, minus = 0;
            double peakOut = 0, peakIn = 0, bestOut = -1, bestIn = -1;
            for (int iy = 0; iy < M; ++iy) {
                for (int ix = 0; ix < M; ++ix) {
                    const double v = map[size_t(iy) * M + ix];
                    const double x = (ix + 0.5 - half) * cell, y = (iy + 0.5 - half) * cell;
                    total += v;
                    mx += v * x;
                    (x > 0 ? plus : minus) += v;
                    if (std::fabs(y) < 0.03 && std::fabs(x) > 0.1) {
                        if (x > 0 && v > bestOut) {
                            bestOut = v;
                            peakOut = x;
                        }
                        if (x < 0 && v > bestIn) {
                            bestIn = v;
                            peakIn = -x;
                        }
                    }
                }
            }
            double bestShift = 0, bestErr = residual(map, 0.0);
            const double asIs = bestErr;
            for (double sft = 0.0025; sft <= 0.25; sft += 0.0025) {
                const double e = residual(map, sft);
                if (e < bestErr) {
                    bestErr = e;
                    bestShift = sft;
                }
            }
            std::printf("%6.0f%c %7.2fx %8.3fmm %8.2f %8.3fmm %8.3fmm %10.1f%% %9.1f%% @ %.3fmm\n", angles[a], 'd', total / totalUpright,
                        mx / total, plus / std::max(minus, 1e-30), peakOut, peakIn, 100 * asIs, 100 * bestErr, bestShift);
        }

        // First angular harmonic: per 0.02 mm ring, A0 = mean, A1 = 2 mean(cos phi).
        std::printf("  first-harmonic model K0_a(r) (1 + beta cos phi):\n");
        const int rb = 150;
        for (size_t a = 1; a < maps.size(); ++a) {
            const std::vector<double>& map = maps[a];
            std::vector<double> s0(rb, 0.0), s1(rb, 0.0), n(rb, 0.0);
            for (int iy = 0; iy < M; ++iy) {
                for (int ix = 0; ix < M; ++ix) {
                    const double x = (ix + 0.5 - half) * cell, y = (iy + 0.5 - half) * cell;
                    const double r = std::hypot(x, y);
                    const int b = static_cast<int>(r / 0.02);
                    if (b < rb && r > 0) {
                        const double v = map[size_t(iy) * M + ix];
                        s0[b] += v;
                        s1[b] += v * x / r;
                        n[b] += 1;
                    }
                }
            }
            // energy-weighted mean beta, and residual of (a) beta(r) per ring, (b) one constant beta
            double wsum = 0, bsum = 0;
            for (int b = 0; b < rb; ++b) {
                if (s0[b] > 0) {
                    bsum += 2 * s1[b];
                    wsum += s0[b];
                }
            }
            const double betaConst = bsum / std::max(wsum, 1e-30);
            double errRing = 0, errConst = 0, total = 0;
            for (int iy = 0; iy < M; ++iy) {
                for (int ix = 0; ix < M; ++ix) {
                    const double x = (ix + 0.5 - half) * cell, y = (iy + 0.5 - half) * cell;
                    const double r = std::hypot(x, y);
                    const int b = static_cast<int>(r / 0.02);
                    const double v = map[size_t(iy) * M + ix];
                    total += v;
                    if (b >= rb || n[b] == 0 || r == 0) {
                        errRing += std::fabs(v);
                        errConst += std::fabs(v);
                        continue;
                    }
                    const double k0 = s0[b] / n[b];
                    const double beta = s0[b] > 0 ? 2 * s1[b] / s0[b] : 0.0;
                    errRing += std::fabs(v - k0 * (1 + beta * x / r));
                    errConst += std::fabs(v - k0 * (1 + betaConst * x / r));
                }
            }
            std::printf("   %4.0fd  beta(r) at 0.1/0.2/0.3/0.5/1.0 mm:", angles[a]);
            for (double rr : {0.1, 0.2, 0.3, 0.5, 1.0}) {
                const int b = static_cast<int>(rr / 0.02);
                std::printf(" %5.2f", s0[b] > 0 ? 2 * s1[b] / s0[b] : 0.0);
            }
            std::printf("   const beta %.2f   residual: beta(r) %.1f%%, const %.1f%%\n", betaConst, 100 * errRing / total, 100 * errConst / total);
        }
    }
}

// ---------------------------------------------------------------------------
// Self-tests

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

// Closed form for a clear slab lit diffusely from above: the share of light
// that comes back within radius rho after one reflection off the rear face.
double slabReturn(double t, double n, double rho, bool withinOnly)
{
    const int steps = 400000;
    double acc = 0;
    for (int i = 0; i < steps; ++i) {
        const double th = (i + 0.5) * (0.5 * kPi / steps);
        const double r = 2 * t * std::tan(th);
        if (withinOnly && r > rho) {
            break;
        }
        double cost;
        const double R = fresnel(n, 1.0, std::cos(th), cost);
        acc += R * 2 * std::sin(th) * std::cos(th) * (0.5 * kPi / steps);
    }
    return acc;
}

void selftest()
{
    const std::vector<double> edges = buildEdges();

    // 1. Diffuse light into a bare slab, a perfect absorber index-matched on
    // top: the returned radial distribution is a closed form.
    {
        const double t = 0.127, n = 1.48;
        Construction slab;
        slab.id = "slab";
        slab.plateAlbedo = 0;
        slab.plateGapMM = 0;
        slab.fNumber = 2.8;
        Layer detector = absorberLayer("detector", 0.1, n, 30, 30, 30);
        detector.exposes = 0;
        slab.layers.push_back(detector);
        slab.layers.push_back(baseLayer("base", t, n, 0.0));
        const Tracer tracer(slab, edges);
        Launch launch;
        launch.kind = Launch::Lambertian;
        launch.layer = 1;
        launch.viaBase = true;
        const uint64_t N = 4000000;
        const Tally tally = tracer.run(0, N, 7, launch);
        const double returned = tally.haloTotal[0] / N;
        const double want = slabReturn(t, n, 0, false);
        check(std::fabs(returned - want) < 0.004, "bare slab: diffuse light returned matches Fresnel + TIR closed form", returned, want);

        const double rc = ringEdge(t, n);
        for (double f : {0.5, 1.0, 1.5, 2.5, 5.0}) {
            double acc = 0;
            for (size_t i = 0; i + 1 < edges.size() && edges[i + 1] <= f * rc + 1e-9; ++i) {
                acc += tally.halo[0][i];
            }
            // compare at the nearest bin edge
            size_t e = 0;
            while (e + 1 < edges.size() && edges[e + 1] <= f * rc + 1e-9) {
                ++e;
            }
            const double wantWithin = slabReturn(t, n, edges[e], true);
            char label[128];
            std::snprintf(label, sizeof label, "bare slab: share returned within %.2f mm (%.1f x ring edge)", edges[e], f);
            check(std::fabs(acc / N - wantWithin) < 0.004, label, acc / N, wantWithin);
        }
    }

    // 2. Energy bookkeeping on the brightest construction.
    const std::vector<Construction> cons = buildConstructions();
    {
        const Construction& cine = cons[static_cast<int>(fh::Construction::MotionRemjetRemoved35)];
        const Tracer tracer(cine, edges);
        const uint64_t N = 1000000;
        const Tally t = tracer.run(0, N, 11);
        const double sum = t.sensitive + t.entryReflected + t.escapedTop + t.inert + t.lostBack;
        check(std::fabs(sum / N - 1.0) < 0.003, "energy is conserved (absorbed + escaped + lost = launched)", sum / N, 1.0);

        const Tally again = tracer.run(0, N, 11);
        check(again.haloTotal[0] == t.haloTotal[0] && again.halo[0][60] == t.halo[0][60], "tracing is deterministic for a given seed");
    }

    // 3. Physics that must come out of the stacks.
    {
        const uint64_t N = 1000000;
        const Kernels cine = trace(cons[static_cast<int>(fh::Construction::MotionRemjetRemoved35)], edges, N, 5);
        const Kernels remjet = trace(cons[static_cast<int>(fh::Construction::MotionRemjet35)], edges, N, 5);
        const Kernels neg = trace(cons[static_cast<int>(fh::Construction::ColourNegative35)], edges, N, 5);
        const Kernels sheet = trace(cons[static_cast<int>(fh::Construction::MotionRemjetRemovedSheet)], edges, N, 5);

        double n;
        const double t35 = baseOf(*cine.c, n);
        const double rc = ringEdge(t35, n);
        // The very centre always spikes (per-area density of the few percent
        // that reflect straight back onto the highlight itself), so judge the
        // ring by its shape: dark inside the edge, bright just outside it.
        const double peak = peakRadius(cine, 0, 0, 0.5 * rc);
        check(peak > 0.85 * rc && peak < 1.2 * rc, "bare base: red halo peaks at the total-internal-reflection edge", peak, rc);
        const double inside = meanDensity(cine, 0, 0, 0.4 * rc, 0.85 * rc);
        const double band = meanDensity(cine, 0, 0, rc, 1.3 * rc);
        // A centre-peaked blob (any sum of Gaussians) scores below 1. The
        // emulsion's forward scattering keeps most light shallower than the
        // critical angle, where partial Fresnel reflection fills the gap in
        // somewhat, so a real ring is a clear step rather than a black moat.
        check(band > 1.5 * inside, "bare base: a ring, not a blob: brighter just outside the edge than inside", band / std::max(inside, 1e-12), 1.5);
        check(energyWithin(cine, 0, 0, 0.6 * rc) < 0.15, "bare base: little halo inside the ring", energyWithin(cine, 0, 0, 0.6 * rc), 0.15);
        check(remjet.total[0][0] < 0.02 * cine.total[0][0], "rem-jet removes >98% of the halo", remjet.total[0][0] / cine.total[0][0], 0.02);
        check(neg.total[0][0] < 0.2 * cine.total[0][0] && neg.total[0][0] > 0.002 * cine.total[0][0],
              "still colour negative halates, far less than bare motion film", neg.total[0][0] / cine.total[0][0], 0.05);
        check(cine.total[0][0] > 2.5 * cine.total[1][1], "halation is red-orange: red layer gets well over twice the green", cine.total[0][0] / cine.total[1][1], 2.5);
        check(cine.total[2][2] < 1e-3, "blue light never reaches the base (yellow filter)", cine.total[2][2], 0);
        const double sheetN = 1.65;
        double nn;
        const double rcSheet = ringEdge(baseOf(*sheet.c, nn), sheetN);
        const double sheetPeak = peakRadius(sheet, 0, 0, 0.5 * rcSheet);
        check(sheetPeak > 0.85 * rcSheet && sheetPeak < 1.2 * rcSheet, "polyester sheet base: ring follows its own critical angle", sheetPeak, rcSheet);
    }
}

} // namespace

int main(int argc, char** argv)
{
    uint64_t photons = 8000000;
    const uint64_t seed = 20260926;
    std::string mode, arg;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--photons" && i + 1 < argc) {
            photons = std::strtoull(argv[++i], nullptr, 10);
        } else if ((a == "--generate" || a == "--csv") && i + 1 < argc) {
            mode = a;
            arg = argv[++i];
        } else {
            mode = a;
        }
    }

    if (mode == "--oblique") {
        oblique(buildConstructions(), buildEdges(), photons);
        return 0;
    }

    if (mode == "--selftest") {
        selftest();
        std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
        return failures ? 1 : 0;
    }

    if (mode != "--report" && mode != "--generate" && mode != "--csv" && mode != "--check-tables") {
        std::fprintf(stderr, "usage: %s --selftest | --report | --oblique | --generate <file.cc> | --csv <dir> | --check-tables [--photons N]\n", argv[0]);
        return 2;
    }

    const std::vector<double> edges = buildEdges();
    const std::vector<Construction> cons = buildConstructions();
    std::vector<Kernels> all;
    for (const Construction& c : cons) {
        all.push_back(trace(c, edges, photons, seed));
        measureObliquity(all.back(), edges, photons / 4, seed);
    }
    // Kernels hold pointers into cons: keep it alive until here.

    if (mode == "--report") {
        report(all);
        return 0;
    }
    if (mode == "--csv") {
        return writeCsv(all, arg) ? 0 : 1;
    }
    if (mode == "--generate") {
        return generate(all, arg, photons, seed) ? 0 : 1;
    }

#ifdef HALATION_TABLES_LINKED
    // --check-tables: the committed tables must be what the tracer produces now.
    for (size_t i = 0; i < all.size(); ++i) {
        const fh::HaloTable& t = fh::table(static_cast<fh::Construction>(i));
        bool same = all[i].c->id == t.id && t.bins == static_cast<int>(edges.size() - 1);
        for (int a = 0; a < 4 && same; ++a) {
            same = std::fabs(t.obliqueBeta[a] - all[i].obliqueBeta[a]) <= 1e-4 * std::max(1e-6, std::fabs(all[i].obliqueBeta[a]));
        }
        for (int b = 0; b < 3 && same; ++b) {
            for (int k = 0; k < 3 && same; ++k) {
                const double want = all[i].total[b][k] < kNegligible ? 0.0 : all[i].total[b][k];
                same = std::fabs(t.total[b][k] - want) <= 1e-4 * std::max(1e-6, want);
                if (same && t.kernel[b][k]) {
                    for (int j = 0; j < t.bins && same; ++j) {
                        same = std::fabs(t.kernel[b][k][j] - all[i].k[b][k][j]) <= 1e-4 * std::max(1e-9, std::fabs(all[i].k[b][k][j]));
                    }
                }
            }
        }
        check(same, (std::string("committed table matches the tracer: ") + all[i].c->id).c_str());
    }
    check(fh::constructionFor(fh::StockKind::MotionPicture, "120", true) == fh::Construction::MotionRemjetRemoved120,
          "stock mapping: remjet-stripped motion film in 120");
    check(fh::constructionFor(fh::StockKind::ColourNegative, "35", false) == fh::Construction::ColourNegative35,
          "stock mapping: colour negative in 35mm");
    check(fh::constructionFor(fh::StockKind::Monochrome, "large", false) == fh::Construction::MonochromeSheet,
          "stock mapping: monochrome sheet film");
    check(fh::constructionFor(fh::StockKind::Reversal, "weird", false) == fh::Construction::Reversal35,
          "stock mapping: unknown format falls back to 35mm");
    std::printf(failures ? "\n%d STALE\n" : "\ntables are current\n", failures);
    return failures ? 1 : 0;
#else
    std::fprintf(stderr, "--check-tables needs rtengine/filmhalationtables.cc linked in with -DHALATION_TABLES_LINKED\n");
    return 2;
#endif
}
