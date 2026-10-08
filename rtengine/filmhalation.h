#pragma once

// Physically derived halation kernels, one set per film construction.
//
// Halation is light that got through the emulsion, crossed the film base and
// came back off its rear surface to re-expose the emulsion from below. Past
// the base's critical angle that rear surface reflects totally, so the
// returning light lands in a RING whose inner edge sits 2 t tan(theta_c) from
// the highlight (0.23 mm for a 127 um acetate base), with almost nothing
// inside it. How much comes back is set by what the light meets on the way:
// an anti-halation undercoat, a dyed base, a rem-jet backing, a pressure
// plate.
//
// The kernels are traced offline by tools/halation_tracer.cc, which follows
// light through each construction's layer stack (Fresnel and total internal
// reflection at every interface, scattering and absorption in every layer)
// and writes filmhalationtables.cc. Never edit that file by hand: change the
// construction in the tracer and regenerate.
//
// Units: kernel[b][k](r) is the extra exposure layer k receives, per mm^2 of
// film at radius r, for a unit of band-b exposure arriving at a point,
// relative to the exposure layer k takes from its own band directly. So
// sum_b (kernel[b][k] * E_b), convolved over the film plane in mm, is the
// halo in the same units as a scene-linear exposure E_k. Bands and layers are
// both ordered red, green, blue.
//
// Add the halo MINUS its uniform-field share: E_k + sum_b (kernel[b][k] * E_b
// - total[b][k] E_b). A stock's characteristic curve is measured on uniform
// patches, where every point gets total[b][k] of halation from its
// neighbours, so that share is already inside the curve. Adding the raw
// convolution would put it in twice -- for bare motion film, a 28% red lift
// over every flat area. What remains is zero on flat ground and non-zero only
// where exposure changes: the ring round a light, the fringe along a window.

#include <string>

namespace rtengine {
namespace filmhalation {

enum class Construction : int {
    ColourNegative35,
    ColourNegative120,
    ColourNegativeSheet,
    Reversal35,
    Reversal120,
    ReversalSheet,
    Monochrome35,
    Monochrome120,
    MonochromeSheet,
    MotionRemjet35,
    MotionRemjet120,
    MotionRemjetSheet,
    MotionRemjetRemoved35,
    MotionRemjetRemoved120,
    MotionRemjetRemovedSheet,
    Count
};

constexpr int kChannels = 3; // red, green, blue

struct HaloTable {
    const char* id;
    float baseMM;       // base thickness
    float baseIndex;    // base refractive index
    float ringEdgeMM;   // 2 t tan(theta_c): where total internal reflection starts landing
    int bins;
    const float* edgesMM;                     // bins + 1 radial bin edges
    const float* kernel[kChannels][kChannels]; // [band][layer], nullptr where negligible
    float total[kChannels][kChannels];         // integral of each kernel over the film plane
    // Light arriving at an angle (off-axis, towards the corners) halates
    // lopsidedly: K(r, phi) ~ K(r) (1 + beta cos phi), phi measured from the
    // direction the light leans. beta at 0, 10, 20 and 30 degrees of tilt.
    float obliqueBeta[4];
};

constexpr float kObliqueStepDegrees = 10.f;

const HaloTable& table(Construction construction);

// Adds the halo to dst:
//   dst_k += sum_b gain[b][k] * (kernel[b][k] (x) src_b - total[b][k] src_b)
// on a width x height plane whose pixel pitch is pixelsPerMM of film, with the
// kernel's radius scaled by sizeMultiplier. Mirror-image frame edges, so a
// flat area touching the border stays flat. Convolution is exact for the
// tabulated radial profile: it runs as a cosine transform, multiplying by the
// kernel's own Hankel transform. Lives in filmhalation.cc (needs FFTW).
//
// subtractUniform = false leaves out the "- total src" term, for callers that
// convolve on a reduced plane: they must subtract gain * total * src at FULL
// resolution, per pixel, or a small light's share smears over its dark
// neighbours when the plane is scaled back up.
//
// With an Obliquity, each source point's halo leans away from the optical
// axis by beta(tilt), tilt = atan(distance from the axis / exit pupil
// distance). The lean K(r) beta cos(phi) is the gradient of a convolution
// with the radial kernel G(r) = -integral_r^inf K, so it rides the same exact
// cosine-transform path: dst += d/dx (G (x) beta u_x src) + d/dy (G (x) beta u_y src).
struct Obliquity {
    float centreX, centreY; // optical axis on the plane, in plane pixels
    float exitPupilMM;      // film to exit pupil
};

void convolve(Construction construction, float** const src[kChannels], float** const dst[kChannels], int width, int height,
              float pixelsPerMM, float sizeMultiplier, const float gain[kChannels][kChannels], bool multiThread,
              bool subtractUniform = true, const Obliquity* obliquity = nullptr);

// beta for a chief ray tilted by the given angle, interpolated in the table
// and held at its 30 degree value beyond.
inline float obliqueBetaAt(const HaloTable& t, float tiltDegrees)
{
    const float x = tiltDegrees / kObliqueStepDegrees;
    if (!(x > 0.f)) {
        return 0.f;
    }
    if (x >= 3.f) {
        return t.obliqueBeta[3];
    }
    const int i = static_cast<int>(x);
    return t.obliqueBeta[i] + (t.obliqueBeta[i + 1] - t.obliqueBeta[i]) * (x - i);
}

// Film Lab's stock classes and formats, mapped onto the traced constructions.
enum class StockKind { ColourNegative, Reversal, Monochrome, MotionPicture };

// format: "35" (or anything unrecognised), "120", "large". remjetRemoved is
// the CineStill case: motion-picture film with its backing washed off.
inline Construction constructionFor(StockKind kind, const std::string& format, bool remjetRemoved)
{
    const int f = format == "120" ? 1 : format == "large" ? 2 : 0;
    switch (kind) {
        case StockKind::Reversal:
            return static_cast<Construction>(static_cast<int>(Construction::Reversal35) + f);
        case StockKind::Monochrome:
            return static_cast<Construction>(static_cast<int>(Construction::Monochrome35) + f);
        case StockKind::MotionPicture:
            return static_cast<Construction>(static_cast<int>(remjetRemoved ? Construction::MotionRemjetRemoved35 : Construction::MotionRemjet35) + f);
        case StockKind::ColourNegative:
        default:
            return static_cast<Construction>(static_cast<int>(Construction::ColourNegative35) + f);
    }
}

} // namespace filmhalation
} // namespace rtengine
