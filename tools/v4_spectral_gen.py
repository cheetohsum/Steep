#!/usr/bin/env python3
"""Generate the V4 spectral crosstalk matrices for rtengine/ipfilmlab.cc.

Each film class gets a 3x3 layer-exposure matrix derived by integrating
class-plausible spectral sensitivity curves against a smooth reflectance
basis and a daylight illuminant:

    M[i][j] = integral  S_i(lambda) * B_j(lambda) * I(lambda) dlambda

The basis is a partition of unity (B_R + B_G + B_B = 1 at every
wavelength): a long-pass "red", a short-pass "blue", and the band between
them as "green". That mirrors how broad real-world reflectances are - a
narrow display-primary basis would understate crosstalk - and it makes a
neutral input integrate to the layer's full response, so after row
normalisation grey maps to equal layer exposures by construction and the
tone pipeline is untouched.

Sensitivities are class-plausible shapes (peak/width/leaks in the range
published C-41/E-6/ECN-2 stocks span), NOT any specific branded product -
the stocks in this project are intentionally fictional.

Run and paste the printed block into makeV4SpectralMatrix() when shapes
change. Keep this script and the C++ constants in step.
"""

import math

STEP = 2.0
LAMBDAS = [380.0 + i * STEP for i in range(int((730 - 380) / STEP) + 1)]


def gauss(l, mu, sigma):
    return math.exp(-0.5 * ((l - mu) / sigma) ** 2)


def basis(l):
    b = 1.0 / (1.0 + math.exp((l - 505.0) / 18.0))   # short-pass
    r = 1.0 / (1.0 + math.exp(-(l - 585.0) / 20.0))  # long-pass
    g = max(1.0 - b - r, 0.0)
    return r, g, b


def yellow_filter(l):
    """The Carey Lea silver / yellow filter layer above the green- and
    red-sensitive layers: silver halide is natively blue-sensitive, so
    without this hard cut below ~500nm every layer would respond to blue.
    Omitting it was what twisted V4 blues 20-27 degrees toward cyan."""
    return 1.0 / (1.0 + math.exp(-(l - 497.0) / 8.0))


def illuminant(l):
    """Planck 5500K, normalised shape only (row normalisation eats scale)."""
    h, c, k = 6.626e-34, 2.998e8, 1.381e-23
    lm = l * 1e-9
    return (1.0 / lm ** 5) / (math.exp(h * c / (lm * k * 5500.0)) - 1.0)


# Per class: (blue layer, green layer, red layer) sensitivity shapes.
# Each shape: list of (weight, peak nm, sigma nm) lobes.
SENSITIVITIES = {
    # Colour negative: forgiving, broad, red sensitiser shoulder into
    # green-orange; the yellow filter layer leaks a little blue into the
    # green- and red-sensitive layers.
    # Layers marked filtered=True sit under the yellow filter layer; their
    # residual blue response is the small unfiltered leak lobe.
    "ColorNegative": {
        "blue": [(1.0, 462, 34)],
        "green": [(1.0, 546, 40)],
        "green_leak": [(0.035, 468, 25)],
        "red": [(1.0, 640, 46), (0.22, 602, 30)],
        "red_leak": [(0.02, 468, 25)],
    },
    # Reversal: narrower, cleaner separation - that IS the slide look.
    "Reversal": {
        "blue": [(1.0, 452, 28)],
        "green": [(1.0, 540, 33)],
        "green_leak": [(0.02, 466, 24)],
        "red": [(1.0, 645, 36), (0.10, 600, 25)],
        "red_leak": [(0.012, 466, 24)],
    },
    # Motion negative: the softest palette, widest overlap.
    "MotionNegative": {
        "blue": [(1.0, 464, 38)],
        "green": [(1.0, 552, 46)],
        "green_leak": [(0.05, 470, 26)],
        "red": [(1.0, 632, 50), (0.30, 596, 34)],
        "red_leak": [(0.025, 470, 26)],
    },
}


def matrix_for(shapes):
    rows = []
    for layer in ("red", "green", "blue"):
        lobes = shapes[layer]
        leak = shapes.get(layer + "_leak", [])
        filtered = layer != "blue"
        row = [0.0, 0.0, 0.0]
        for l in LAMBDAS:
            s = sum(w * gauss(l, mu, sig) for w, mu, sig in lobes)
            if filtered:
                s = s * yellow_filter(l) + sum(w * gauss(l, mu, sig) for w, mu, sig in leak)
            e = illuminant(l)
            br, bg, bb = basis(l)
            row[0] += s * br * e
            row[1] += s * bg * e
            row[2] += s * bb * e
        total = sum(row)
        rows.append([v / total for v in row])
    return rows


def main():
    for cls, shapes in SENSITIVITIES.items():
        m = matrix_for(shapes)
        print(f"        case StockClass::{cls}:")
        print("            return {{")
        for i, row in enumerate(m):
            comma = "," if i < 2 else ""
            print(f"                {row[0]:.4f}f, {row[1]:.4f}f, {row[2]:.4f}f{comma}")
        print("            }};")


if __name__ == "__main__":
    main()
