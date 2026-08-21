#!/usr/bin/env python3
"""Measure the Film Lab tone response outside the renderer.

Mirrors the maths in rtengine/ipfilmlab.cc so a recipe change can be checked
against hard numbers before it costs a build. Every model here must stay in
step with the C++; when one changes, change both.

    python tools/filmsim_probe.py              # acceptance table for the shipped model
    python tools/filmsim_probe.py --all        # every model, for comparison
    python tools/filmsim_probe.py --strength 50

Acceptance targets (see --gate output):
  * scene black (eight stops under middle grey, 2.3/255 going in) must not be
    lifted past 4/255 - 5.5 for ECN-2 stocks, 7 for the Creative ones, which
    are meant to sit on base fog
  * diffuse white (1.0 linear) must render at or above 253/255 sRGB
  * midtone gamma >= 1.00 for negative stocks, >= 1.25 for reversal: a film
    stage may shape contrast but must never hand back less than it got
  * stocks must not all measure the same gamma. The spread across the set is
    the whole point of having sixteen of them, and the shipped V3 measured
    0.06 - every stock the same picture.
  * V4 (scene-referred): the same tone maths fed the unclipped rgbProc tap.
    The HDR ladder gate (report_v4_hdr) holds the contract above diffuse
    white: monotonic through 8x white, a negative separates the first stop
    over white, 8x white stays under 1.35 linear. The C++-only parts of V4
    (stops-gridded density LUT, the scene tap / Lab-gain reconstruction, and
    halationHighlightSourceV4) are sampling and spatial details with no probe
    counterpart - verify those against steep-cli renders.
"""

import argparse
import math
import sys

# --------------------------------------------------------------------------
# Stock table - mirrors STOCKS[] in rtengine/ipfilmlab.cc
# --------------------------------------------------------------------------

# id, class, speed, contrast, toe, shoulder, saturation, warmth, tint,
# redBias, greenBias, blueBias, grain, halation, acutance
STOCKS = [
    ("custom",           "Custom",         100, 1.00, 0.00, 0.00, 1.00,  0.00,  0.00,  0.00,  0.00,  0.00, 0.00, 0.00, 0.00),
    ("heritage_gold",    "ColorNegative",  200, 1.04, 0.23, 0.38, 1.06,  0.11, -0.01,  0.05,  0.01, -0.05, 0.24, 0.06, 0.18),
    ("porcelain_400",    "ColorNegative",  400, 0.96, 0.28, 0.52, 0.93,  0.03,  0.02,  0.02,  0.00, -0.02, 0.22, 0.04, 0.12),
    ("vivid_chrome",     "Reversal",        50, 1.22, 0.08, 0.14, 1.27, -0.01,  0.01,  0.00,  0.02, -0.01, 0.10, 0.01, 0.27),
    ("arctic",           "Reversal",       100, 1.12, 0.10, 0.18, 1.10, -0.08, -0.01, -0.04,  0.01,  0.05, 0.14, 0.01, 0.22),
    ("sovereign",        "ColorNegative",  160, 1.03, 0.20, 0.35, 1.02,  0.04,  0.01,  0.02,  0.01, -0.02, 0.18, 0.04, 0.17),
    ("golden_hour",      "ColorNegative",  200, 1.00, 0.25, 0.45, 1.03,  0.16,  0.01,  0.07,  0.02, -0.07, 0.22, 0.08, 0.11),
    ("twilight_160",     "MotionNegative", 160, 0.94, 0.24, 0.58, 0.91, -0.04,  0.00, -0.02,  0.01,  0.04, 0.20, 0.07, 0.10),
    ("nostalgia_200",    "ColorNegative",  200, 0.92, 0.34, 0.49, 0.84,  0.09,  0.03,  0.04,  0.00, -0.04, 0.28, 0.05, 0.08),
    ("desert_chrome",    "Reversal",        64, 1.15, 0.12, 0.17, 1.17,  0.10, -0.02,  0.05,  0.01, -0.05, 0.12, 0.02, 0.24),
    ("street_800",       "ColorNegative",  800, 1.07, 0.30, 0.44, 0.91, -0.01,  0.01, -0.01,  0.01,  0.01, 0.48, 0.05, 0.20),
    ("cinematic_500t",   "MotionNegative", 500, 0.92, 0.26, 0.63, 0.88, -0.07,  0.00, -0.04,  0.00,  0.05, 0.30, 0.18, 0.09),
    ("fade_bloom",       "Creative",       200, 0.84, 0.47, 0.58, 0.76,  0.08,  0.04,  0.04,  0.00, -0.03, 0.35, 0.17, 0.03),
    ("ember",            "Creative",       400, 1.08, 0.22, 0.39, 1.05,  0.17,  0.03,  0.08,  0.01, -0.07, 0.30, 0.13, 0.16),
    ("silver_gelatin",   "Monochrome",     400, 1.14, 0.25, 0.31, 0.00,  0.00,  0.00,  0.02,  0.04, -0.06, 0.42, 0.00, 0.28),
    ("analog_dream",     "Creative",       200, 0.89, 0.40, 0.62, 0.82,  0.07,  0.05,  0.04, -0.01, -0.02, 0.39, 0.14, 0.04),
    ("cinema_reveal_35", "MotionNegative", 250, 0.96, 0.24, 0.57, 0.93, -0.02,  0.01, -0.01,  0.01,  0.02, 0.24, 0.11, 0.13),
]
S = {s[0]: s for s in STOCKS}

DEFAULT_PROCESS = {
    "Reversal": "e6", "MotionNegative": "ecn2", "Monochrome": "bw",
    "ColorNegative": "c41", "Custom": "c41", "Creative": "c41",
}

MIDDLE_GREY = 0.18
WHITE_STOPS = math.log2(1.0 / MIDDLE_GREY)   # 2.4739


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def srgb(v):
    v = max(v, 0.0)
    return 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def softplus(v, softness):
    softness = max(softness, 0.02)
    x = v / softness
    if x > 12.0:
        return v
    if x < -12.0:
        return 0.0
    return softness * math.log1p(math.exp(x))


# --------------------------------------------------------------------------
# V2 - the legacy Film Lab model (rtengine/ipfilmlab.cc, filmPresets tail)
# --------------------------------------------------------------------------

def v2_process(p):
    if p == "e6":
        return dict(contrast=1.10, toe=-0.05, shoulder=-0.08, sat=1.10)
    if p == "ecn2":
        return dict(contrast=0.92, toe=0.00, shoulder=0.16, sat=0.92)
    if p == "bw":
        return dict(contrast=1.05, toe=0.08, shoulder=0.00, sat=0.00)
    return dict(contrast=1.0, toe=0.0, shoulder=0.0, sat=1.0)


def v2_output(o):
    if o == "ra4":
        return dict(contrast=1.06, sat=1.04, shoulder=0.0)
    if o == "projection":
        return dict(contrast=1.10, sat=1.08, shoulder=-0.04)
    if o == "cinema":
        return dict(contrast=1.04, sat=0.96, shoulder=0.08)
    return dict(contrast=1.0, sat=1.0, shoulder=0.0)


def v2_film_response(v, expo, contrast, toe, shoulder):
    v = max(v * (2.0 ** expo), 1e-7)
    stops = math.log2(v / MIDDLE_GREY)
    if stops < 0:
        stops /= 1.0 + toe * (-stops) * 0.18
    else:
        stops /= 1.0 + shoulder * stops * 0.20
    return clamp(MIDDLE_GREY * (2.0 ** (stops * contrast)), 0.0, 4.0)


def v2_curve(stock_id, process=None, output="scan", fp=None):
    fp = fp or {}
    st = S[stock_id]
    process = process or DEFAULT_PROCESS[st[1]]
    pr, ou = v2_process(process), v2_output(output)
    push = clamp(fp.get("pushPull", 0), -2, 3)
    contrast = clamp(st[3] * pr["contrast"] * ou["contrast"] * (1 + push * 0.075)
                     * (1 + fp.get("contrast", 0) / 250.0), 0.55, 1.65)
    toe = clamp(st[4] + pr["toe"] - push * 0.025 + fp.get("fade", 0) / 260.0, 0.0, 0.85)
    shoulder = clamp(st[5] + pr["shoulder"] + ou["shoulder"] + fp.get("rolloff", 0) / 220.0, 0.0, 1.1)
    sat = clamp(st[6] * pr["sat"] * ou["sat"] * (1 + fp.get("saturation", 0) / 150.0), 0.0, 1.75)
    expo = clamp(fp.get("exposure", 0.0), -4, 4) - push * 0.33

    base = st[10]                      # green layer bias
    chan_expo = expo + base
    black = v2_film_response(0.0, chan_expo, contrast, toe, shoulder)
    middle = v2_film_response(MIDDLE_GREY, base, contrast, toe, shoulder)
    scale = MIDDLE_GREY / max(middle - black, 1e-6)

    def transfer(v):
        response = v2_film_response(v, chan_expo, contrast, toe, shoulder)
        return clamp((response - black) * scale, 0.0, 4.0)

    return transfer, dict(sat=sat)


# --------------------------------------------------------------------------
# V3 - density-domain model
# --------------------------------------------------------------------------

# The print/scanner is calibrated against one reference negative: a fixed
# density-per-stop, which is what the printer's own contrast is set for. A
# stock that is contrastier than this reference prints contrastier. Calibrating
# against each stock's *own* gamma (what the first V3 did) cancelled the stock
# out exactly, which is why every stock measured the same contrast.
#
# 0.18 density/stop is C-41's ~0.60 gamma per log10 exposure. The absolute
# value only sets where the emulsion sits inside [fog, dmax]; what reaches the
# picture is the ratio between a stock's gamma and this reference.
REFERENCE_GAMMA = 0.18

# Where middle grey sits on Custom's straight line, in stops above base fog.
# Big enough that diffuse white and three stops of specular clear D-max,
# small enough that scene black reaches fog instead of floating above it.
CUSTOM_STRAIGHT_OFFSET = 9.5


def v3_profile(stock_id):
    st = S[stock_id]
    p = dict(baseFog=0.055, maxDensity=2.55, gamma=st[3])
    cls = st[1]
    if cls == "Reversal":
        p.update(baseFog=0.025, maxDensity=2.85)
    elif cls == "MotionNegative":
        p.update(baseFog=0.065, maxDensity=2.48)
    elif cls == "Monochrome":
        p.update(baseFog=0.075, maxDensity=2.72)
    elif cls == "Creative":
        p.update(maxDensity=2.35)
    elif cls == "Custom":
        p.update(baseFog=0.05, maxDensity=2.55)
    return p


def v3_process(p):
    if p == "e6":
        return dict(gamma=1.10, toe=-0.05, shoulder=-0.08, sat=1.09, fog=-0.012)
    if p == "ecn2":
        return dict(gamma=0.91, toe=0.00, shoulder=0.16, sat=0.93, fog=0.006)
    if p == "bw":
        return dict(gamma=1.05, toe=0.08, shoulder=0.00, sat=0.00, fog=0.012)
    return dict(gamma=1.0, toe=0.0, shoulder=0.0, sat=1.0, fog=0.0)


# Legacy output profiles (the shipped V3). systemGamma here is `contrast`, and
# at 1.00-1.12 it barely undoes the negative, which is the wash.
def v3_output_legacy(o):
    if o == "ra4":
        return dict(contrast=1.07, toe=0.16, shoulder=0.18, sat=1.035)
    if o == "projection":
        return dict(contrast=1.12, toe=0.10, shoulder=0.10, sat=1.075)
    if o == "cinema":
        return dict(contrast=1.025, toe=0.15, shoulder=0.25, sat=0.965)
    return dict(contrast=1.0, toe=0.08, shoulder=0.12, sat=1.0)


# New output profiles. `gamma` is the system gamma of negative plus print,
# which is what decides whether the result reads as a photograph or a flat
# scan. `toe` deepens print blacks. The highlight shoulder is not a free
# parameter: it is solved so diffuse white lands on paper white.
def v3_output(o):
    if o == "labscan":
        return dict(gamma=1.30, toe=0.14, sat=1.05)
    if o == "ra4":
        return dict(gamma=1.36, toe=0.12, sat=1.035)
    if o == "projection":
        return dict(gamma=1.46, toe=0.10, sat=1.075)
    if o == "cinema":
        return dict(gamma=1.28, toe=0.14, sat=0.965)
    return dict(gamma=1.24, toe=0.16, sat=1.0)


# The print's shadow expansion exists to undo the emulsion's toe, so it has to
# be proportional to how much toe there was. Applying it flat crushed the
# short-toe reversal stocks (2.6 gamma three stops down) while still leaving
# the long-toe stocks lifted.
REFERENCE_TOE = 0.26


# A negative's own gamma spans 0.84-1.22 here, but the medium it is printed
# onto compensates: Portra on RA-4 and Velvia projected end up at similar
# system gammas, and what separates them is toe/shoulder shape and colour, not
# global contrast. Passing the stock's gamma through at full weight would make
# the soft stocks flatter than the source, which is the wash we are removing.
STOCK_CONTRAST_WEIGHT = 0.70


def v3_density(v, expo, gamma, toe, shoulder, fog, dmax, legacy=False, straight=False):
    exposed = max(v * (2.0 ** expo), 1e-8)
    stops = math.log2(exposed / MIDDLE_GREY)
    if straight:
        # Custom is not a film stock, so it gets no toe and no shoulder at all:
        # a pure straight line, which the print inverts exactly. Anything the
        # user has not asked for stays off.
        return clamp(fog + gamma * (stops + CUSTOM_STRAIGHT_OFFSET), fog, dmax)
    if legacy:
        toe_start = -2.75 + toe * 1.12
        shoulder_start = 3.35 - shoulder * 0.92
        toe_soft = 0.44 + toe * 0.52
        shoulder_soft = 0.58 + shoulder * 0.58
    else:
        # A colour negative carries roughly six stops below middle grey before
        # the toe takes over. The first V3 put the toe at -2.75, which threw
        # away the shadow range and then had to invent it back, lifting blacks
        # by two stops. Placing it where real film puts it is most of the fix.
        toe_start = -6.20 + toe * 1.60
        shoulder_start = 3.60 - shoulder * 0.90
        toe_soft = 0.55 + toe * 1.10
        shoulder_soft = 0.55 + shoulder * 0.75
    density = fog + gamma * (softplus(stops - toe_start, toe_soft)
                             - softplus(stops - shoulder_start, shoulder_soft))
    return clamp(density, fog, dmax)


def _solve_shoulder_k(slope):
    """k for shape(t) = (1-exp(-k t))/(1-exp(-k)) with shape'(0) == slope.

    shape(0)=0 and shape(1)=1 for every k, so diffuse white lands on paper
    white by construction and `slope` is free to be the system gamma.
    k > 0 compresses highlights (a shoulder), k < 0 expands them.
    """
    if abs(slope - 1.0) < 1e-4:
        return 0.0
    lo, hi = (0.0, 40.0) if slope > 1.0 else (-40.0, 0.0)
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if abs(mid) < 1e-9:
            value = 1.0
        else:
            value = mid / (1.0 - math.exp(-mid))
        if value < slope:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def _shape(t, k):
    if abs(k) < 1e-9:
        return t
    return (1.0 - math.exp(-k * t)) / (1.0 - math.exp(-k))


# Integral of smoothstep(0, PRINT_TOE_STOPS, t), so the print's shadow
# expansion is defined by its *slope* and that slope is bounded and smooth.
# Scaling the value instead (s * (1 + c * min(s^2/16, 1))) put a spike in the
# derivative exactly at the knee, which blocked up shadows on the contrastier
# stocks.
PRINT_TOE_STOPS = 4.0


def _toe_slope_integral(t):
    u = min(t / PRINT_TOE_STOPS, 1.0)
    return PRINT_TOE_STOPS * (u ** 3 - 0.5 * u ** 4) + max(t - PRINT_TOE_STOPS, 0.0)


def _shape_slope(t, k):
    """Derivative of _shape with respect to t."""
    if abs(k) < 1e-6:
        return 1.0
    return k * math.exp(-k * t) / (1.0 - math.exp(-k))


# How far past paper white the V4 print may reach, in output stops. The V3
# print shape was built for input capped at diffuse white; fed real scene
# radiance its solved shoulder k can be small or negative, and the soft stocks
# then EXPAND super-white input without bound (twilight_160 rendered 8x white
# at 2.5x linear). A real print cannot: just past the exposure that prints
# diffuse white, paper density is at Dmin and the sheet has nothing left to
# give. A third of a stop of sparkle is what survives in practice.
PRINT_OVERSHOOT_STOPS = 0.35


def v3_curve(stock_id, process=None, output="scan", fp=None, legacy=False, hdr_print=False):
    fp = fp or {}
    st = S[stock_id]
    process = process or DEFAULT_PROCESS[st[1]]
    prof, pr = v3_profile(stock_id), v3_process(process)
    push = clamp(fp.get("pushPull", 0), -2, 3)
    expo = clamp(fp.get("exposure", 0.0), -4, 4) - push * 0.32
    user_contrast = clamp(1 + fp.get("contrast", 0) / 240.0, 0.58, 1.48)
    user_toe = fp.get("fade", 0) / 250.0
    user_shoulder = fp.get("rolloff", 0) / 210.0
    fog = clamp(prof["baseFog"] + pr["fog"] + max(fp.get("fade", 0), 0) / 2200.0, 0.005, 0.22)
    dmax = clamp(prof["maxDensity"] + push * 0.07 - max(fp.get("fade", 0), 0) / 900.0, 1.65, 3.15)

    toe = clamp(st[4] + pr["toe"] - push * 0.025 + user_toe, 0.0, 1.15)
    shoulder = clamp(st[5] + pr["shoulder"] + user_shoulder, 0.0, 1.45)
    base_expo = st[10]
    eff_expo = base_expo + expo

    if legacy:
        gamma = clamp(0.40 * prof["gamma"] * pr["gamma"] * user_contrast * (1 + push * 0.065), 0.20, 0.78)

        def emulsion(v, exposure):
            return v3_density(v, exposure, gamma, toe, shoulder, fog, dmax, legacy=True)

        ou = v3_output_legacy(output)
        base_gamma = clamp(0.40 * prof["gamma"] * pr["gamma"], 0.20, 0.78)
        ref_d = v3_density(MIDDLE_GREY, base_expo, base_gamma,
                           clamp(st[4] + pr["toe"], 0, 1.15),
                           clamp(st[5] + pr["shoulder"], 0, 1.45), fog, dmax, legacy=True)

        def out_of_density(d):
            stops = (d - ref_d) / max(base_gamma, 0.08)
            if stops < 0:
                stops /= 1.0 + ou["toe"] * (-stops) * 0.15
            else:
                stops /= 1.0 + ou["shoulder"] * stops * 0.15
            return clamp(MIDDLE_GREY * (2.0 ** (stops * ou["contrast"])), 0.0, 8.0)

        out_black = out_of_density(fog)
        out_scale = MIDDLE_GREY / max(MIDDLE_GREY - out_black, 1e-6)

        def transfer(v):
            return clamp((out_of_density(emulsion(v, eff_expo)) - out_black) * out_scale, 0.0, 8.0)

        sat = clamp(st[6] * pr["sat"] * ou["sat"] * (1 + fp.get("saturation", 0) / 160.0), 0.0, 1.72)
        return transfer, dict(sat=sat)

    ou = v3_output(output)
    custom = st[1] == "Custom"
    stock_ratio = (prof["gamma"] ** STOCK_CONTRAST_WEIGHT) * (pr["gamma"] ** STOCK_CONTRAST_WEIGHT)
    gamma = clamp(REFERENCE_GAMMA * stock_ratio * user_contrast * (1 + push * 0.065), 0.06, 0.45)

    def emulsion(v, exposure):
        return v3_density(v, exposure, gamma, toe, shoulder, fog, dmax, straight=custom)

    # The print is calibrated to REFERENCE_GAMMA, so density converts back to
    # stops at a fixed slope. Subtracting the stock's own density at middle
    # grey is the colour head's filtration: a per-channel offset that keeps
    # grey neutral while leaving per-layer gamma free to show as crossover.
    ref_d = emulsion(MIDDLE_GREY, base_expo)

    def to_scene_stops(density):
        return (density - ref_d) / REFERENCE_GAMMA

    # Where diffuse white lands after the emulsion, measured at the stock's
    # baseline exposure so user exposure is not silently undone.
    white_stops = max(to_scene_stops(emulsion(1.0, base_expo)), 0.35)
    if custom:
        # Custom is the neutral baseline a user dials their own recipe onto.
        # No stock means no print character either; only the sliders act.
        system_gamma = 1.0
        print_toe = 0.0
    else:
        system_gamma = ou["gamma"]
        print_toe = ou["toe"] * clamp(toe / REFERENCE_TOE, 0.15, 2.0)
    k = _solve_shoulder_k(system_gamma * white_stops / WHITE_STOPS)

    def print_grade(stops):
        if stops >= 0.0:
            if hdr_print and not custom and stops > white_stops:
                # Paper saturation: value- and slope-continuous at diffuse
                # white, approaching paper white + PRINT_OVERSHOOT_STOPS.
                slope = WHITE_STOPS * _shape_slope(1.0, k) / white_stops
                excess = stops - white_stops
                return WHITE_STOPS + PRINT_OVERSHOOT_STOPS * (
                    1.0 - math.exp(-slope * excess / PRINT_OVERSHOOT_STOPS))
            return WHITE_STOPS * _shape(stops / white_stops, k)
        # Below mid grey the print keeps the emulsion's slope, then ramps in a
        # deep-shadow expansion that reaches full strength four stops down.
        # That is the paper's toe: normal mids, blacks that land on black
        # rather than on base fog.
        return -system_gamma * (-stops + print_toe * _toe_slope_integral(-stops))

    def transfer(v):
        stops = to_scene_stops(emulsion(v, eff_expo))
        return clamp(MIDDLE_GREY * (2.0 ** print_grade(stops)), 0.0, 8.0)

    sat = clamp(st[6] * pr["sat"] * ou["sat"] * (1 + fp.get("saturation", 0) / 160.0), 0.0, 1.72)
    return transfer, dict(sat=sat)


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def local_gamma(transfer, v, delta=0.25):
    """Slope in log-log around linear value v, i.e. output stops per input stop."""
    a, b = v * 2 ** (-delta), v * 2 ** delta
    fa, fb = max(transfer(a), 1e-9), max(transfer(b), 1e-9)
    return (math.log2(fb) - math.log2(fa)) / (2 * delta)


def measure(stock_id, curve_fn, strength, output, process, fp, **kwargs):
    transfer, info = curve_fn(stock_id, process=process, output=output, fp=fp, **kwargs)
    s = strength / 100.0

    def blended(v):
        return v + (transfer(v) - v) * s

    return dict(
        gamma_mid=local_gamma(blended, MIDDLE_GREY),
        gamma_low=local_gamma(blended, MIDDLE_GREY / 8),
        gamma_high=local_gamma(blended, MIDDLE_GREY * 4),
        # Scene black is eight stops under middle grey, which is itself only
        # 2.3/255 going in. Judging the black point against an absolute code
        # would flag a perfect identity transform, so the interesting number
        # is what the stage does to it, not where it lands.
        black=blended(MIDDLE_GREY / 256),
        shadow=blended(MIDDLE_GREY / 64),
        white=blended(1.0),
        sat=info["sat"],
    )


# Scene black going in is 2.3/255, so these are ceilings on how far the stage
# is allowed to lift it. A tungsten cinema negative really does sit on more
# base fog than a colour negative, and "Faded Instant Print" is faded on
# purpose - so the ceiling is per stock class, not one number for all of them.
BLACK_LIMIT = {"MotionNegative": 5.5, "Creative": 7.0}
SHADOW_LIMIT = {"MotionNegative": 1.8, "Creative": 2.3}

# The other half of the same guard. Contrast has to darken what is below the
# pivot, and slide film genuinely buries its shadows - but a stage that leaves
# nothing down there has swapped one broken look for another, so each class
# also has a floor on how much of the shadow it must hand back.
CRUSH_FLOOR = {"Reversal": 0.04, "Monochrome": 0.08, "MotionNegative": 0.30}


def report(title, curve_fn, strength=100, output="scan", process=None, fp=None, gate=False, **kwargs):
    print(f"\n{'=' * 96}")
    print(f"{title}   strength={strength} output={output} fp={fp or {}}")
    print("=" * 96)
    identity_black = srgb(MIDDLE_GREY / 256) * 255.0
    identity_shadow = srgb(MIDDLE_GREY / 64) * 255.0
    print(f"identity reference: black-in {identity_black:.1f}/255, shadow-in {identity_shadow:.1f}/255, "
          f"white-in 255.0/255")
    print(f"{'stock':<18}{'g mid':>7}{'g -3EV':>8}{'g +2EV':>8}"
          f"{'blk/255':>9}{'shdw/255':>10}{'wht/255':>9}{'sat':>7}   status")
    print("-" * 96)

    gammas = []
    failures = []
    for st in STOCKS:
        m = measure(st[0], curve_fn, strength, output, process, fp, **kwargs)
        blk = srgb(m["black"]) * 255.0
        shdw = srgb(m["shadow"]) * 255.0
        wht = srgb(m["white"]) * 255.0
        gammas.append(m["gamma_mid"])

        notes = []
        if st[0] != "custom":
            if blk > BLACK_LIMIT.get(st[1], 4.0):
                notes.append(f"black {blk:.1f}")
            if shdw > identity_shadow * SHADOW_LIMIT.get(st[1], 1.35):
                notes.append(f"shadow {shdw:.1f}")
            if shdw < identity_shadow * CRUSH_FLOOR.get(st[1], 0.12):
                notes.append(f"crushed {shdw:.1f}")
            if wht < 253.0:
                notes.append(f"white {wht:.1f}")
            # Strength blends toward identity, so the full-strength target has
            # to blend with it too - otherwise a perfectly well behaved stock
            # reads as flat purely because it was asked for at half strength.
            target = 1.25 if st[1] == "Reversal" else 1.00
            floor = 1.0 + (target - 1.0) * (strength / 100.0) - 0.005
            if m["gamma_mid"] < floor:
                notes.append(f"flat {m['gamma_mid']:.2f}<{floor:.2f}")
        status = "ok" if not notes else "FAIL " + ", ".join(notes)
        if notes:
            failures.append(st[0])

        print(f"{st[0]:<18}{m['gamma_mid']:>7.3f}{m['gamma_low']:>8.3f}{m['gamma_high']:>8.3f}"
              f"{blk:>9.1f}{shdw:>10.1f}{wht:>9.1f}{m['sat']:>7.3f}   {status}")

    spread = max(gammas) - min(gammas)
    print("-" * 96)
    print(f"midtone gamma spread across stocks: {spread:.3f}"
          f"{'   FAIL - stocks are indistinguishable' if spread < 0.10 else ''}")
    if gate:
        if failures or spread < 0.10:
            print(f"GATE: FAIL ({len(failures)} stock(s): {', '.join(failures) or 'none'})")
            return False
        print("GATE: PASS")
    return not failures and spread >= 0.10


def report_v4_hdr(strength=100, output="scan", process=None, fp=None, gate=False):
    """V4 scene-referred contract, above diffuse white.

    The C++ V4 model shares V3's tone maths (its density LUT is merely
    re-gridded over stops), but it is fed the unclipped rgbProc tap, so it is
    the first model whose input actually exceeds 1.0. In Python the two are
    the same functions; what this report gates is the HDR contract those
    functions must keep as Track B rebuilds them:

      * radiance must stay ranked: the transfer is monotonic through
        1x..8x diffuse white
      * a negative's shoulder must still discriminate the first stop over
        diffuse white (>= 0.5 percent of linear separation) - reversal is
        exempt, slide film really does slam into clear film
      * paper white may only be modestly overshot: 8x diffuse white stays
        below 1.35 linear, so specular sources read as sparkle, not as an
        unbounded bloom feeding the display clip

    Custom is excluded: it is a straight line by construction and passes
    HDR input through untouched, which is exactly what it promises.
    """
    print(f"\n{'=' * 96}")
    print(f"V4 scene-referred (HDR ladder)   strength={strength} output={output}")
    print("=" * 96)
    print(f"{'stock':<18}{'1x wht':>9}{'2x wht':>9}{'4x wht':>9}{'8x wht':>9}   status")
    print("-" * 96)

    s = strength / 100.0
    failures = []
    for st in STOCKS:
        if st[0] == "custom":
            continue
        transfer, _info = v3_curve(st[0], process=process, output=output, fp=fp, hdr_print=True)

        def blended(v):
            return v + (transfer(v) - v) * s

        ladder = [blended(1.0), blended(2.0), blended(4.0), blended(8.0)]

        notes = []
        if not (ladder[0] <= ladder[1] + 1e-6 and ladder[1] <= ladder[2] + 1e-6
                and ladder[2] <= ladder[3] + 1e-6):
            notes.append("not monotonic")
        if st[1] != "Reversal" and ladder[1] < ladder[0] * 1.005:
            notes.append(f"no separation over white ({ladder[1]:.4f} vs {ladder[0]:.4f})")
        if ladder[3] > 1.35:
            notes.append(f"unbounded {ladder[3]:.3f}")

        status = "ok" if not notes else "FAIL " + ", ".join(notes)
        if notes:
            failures.append(st[0])
        print(f"{st[0]:<18}{ladder[0]:>9.4f}{ladder[1]:>9.4f}{ladder[2]:>9.4f}{ladder[3]:>9.4f}   {status}")

    print("-" * 96)
    if gate:
        if failures:
            print(f"GATE: FAIL ({len(failures)} stock(s): {', '.join(failures)})")
            return False
        print("GATE: PASS")
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--strength", type=int, default=100)
    parser.add_argument("--output", default="scan",
                        choices=["scan", "labscan", "ra4", "projection", "cinema"])
    parser.add_argument("--all", action="store_true", help="include the legacy models")
    parser.add_argument("--gate", action="store_true", help="exit non-zero on failure")
    args = parser.parse_args()

    ok = True
    if args.all:
        report("V2 legacy", v2_curve, args.strength, args.output)
        report("V3 legacy (shipped)", v3_curve, args.strength, args.output, legacy=True)

    ok = report("V3", v3_curve, args.strength, args.output, gate=args.gate)
    ok = report_v4_hdr(args.strength, args.output, gate=args.gate) and ok

    if args.gate and not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
