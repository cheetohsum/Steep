"""Double Exposure verification probe.

Renders synthetic inputs through steep-cli with partial pp3s and compares
against hand-computed scene-linear math. Gray inputs keep every channel
equal, so working-space matrix conversions are identity for the purposes of
channelwise blend math. The chromatic cases pin the working profile to sRGB
so their expectations are computable in linear sRGB too.

Run after ANY change to rtengine/doubleexposureblend.h, the composite in
ipdoubleexposure.cc, or the pp3 load/save of [Double Exposure]. Every check
must pass; the identity checks must be bitwise.
"""
import math
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"

W, H = 256, 64
PROBE_ROW = H // 2

# Working profile pinned to sRGB for the chromatic cases (default is
# ProPhoto, under which per-channel and luminance math is no longer
# hand-computable from sRGB inputs).
SRGB_WORKING = "\n[Color Management]\nWorkingProfile=sRGB\n"


def srgb_to_lin(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def lin_to_srgb(v):
    v = min(1.0, max(0.0, v))
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return v * 255.0


def smoothstep01(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def smoothwindow(lum, low, high, feather):
    if low <= lum <= high:
        return 1.0
    if lum < low:
        if feather <= 0:
            return 0.0
        t = (lum - (low - feather)) / feather
    else:
        if feather <= 0:
            return 0.0
        t = ((high + feather) - lum) / feather
    if t <= 0:
        return 0.0
    if t >= 1:
        return 1.0
    return t * t * (3 - 2 * t)


def gate_encode(lin):
    """The engine windows on sRGB-encoded luminance (perceptual gate units).
    For a gray probe pixel decoded from sRGB value v, this is simply v/255."""
    lin = min(1.0, max(0.0, lin))
    return lin * 12.92 if lin <= 0.0031308 else 1.055 * lin ** (1 / 2.4) - 0.055


def lum709(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def compare_weight(lum_base, lum_partner, keep_brighter, softness_stops, white=1.0):
    """deblend::compareWeight."""
    eps = 1e-4 * white
    d = math.log2((max(lum_partner, 0.0) + eps) / (max(lum_base, 0.0) + eps))
    if not keep_brighter:
        d = -d
    if softness_stops <= 0:
        return 1.0 if d > 0 else 0.0
    return smoothstep01(0.5 + d / softness_stops)


def shoulder(x, knee):
    """deblend::shoulder, white = 1."""
    if x <= knee:
        return x
    t = x - knee
    rng = 1.0 - knee
    return knee + rng * t / (t + rng)


def make_inputs():
    grad = Image.new("RGB", (W, H))
    grad_px = grad.load()
    rgrad = Image.new("RGB", (W, H))
    rgrad_px = rgrad.load()
    red = Image.new("RGB", (W, H))
    red_px = red.load()
    gray = Image.new("RGB", (W, H), (128, 128, 128))
    blue = Image.new("RGB", (W, H), (0, 0, 220))
    for x in range(W):
        for y in range(H):
            grad_px[x, y] = (x, x, x)
            rgrad_px[x, y] = (255 - x, 255 - x, 255 - x)
            red_px[x, y] = (x, 0, 0)
    grad.save(os.path.join(HERE, "base_grad.png"))
    rgrad.save(os.path.join(HERE, "partner_rgrad.png"))
    gray.save(os.path.join(HERE, "partner_gray.png"))
    red.save(os.path.join(HERE, "base_red.png"))
    blue.save(os.path.join(HERE, "partner_blue.png"))


def write_pp3(name, body, extra=""):
    path = os.path.join(HERE, name)
    with open(path, "w", newline="\n") as f:
        f.write("[Double Exposure]\n" + body + extra)
    return path


def render(pp3, base, out):
    outp = os.path.join(HERE, out)
    if os.path.exists(outp):
        os.remove(outp)
    r = subprocess.run(
        [CLI, "-Y", "-o", outp, "-d", "-p", pp3, "-t", "-b8",
         "-c", os.path.join(HERE, base)],
        capture_output=True, text=True)
    if not os.path.exists(outp + ".tif") and not os.path.exists(outp):
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        sys.exit("render failed: " + out)
    return outp + ".tif" if os.path.exists(outp + ".tif") else outp


def row(path):
    im = Image.open(path).convert("RGB")
    assert im.size == (W, H), im.size
    px = im.load()
    return [px[x, PROBE_ROW][1] for x in range(W)]


def row_rgb(path):
    im = Image.open(path).convert("RGB")
    assert im.size == (W, H), im.size
    px = im.load()
    return [px[x, PROBE_ROW] for x in range(W)]


def check(name, got, expected, tol=3.0, skip_clipped=False):
    worst = -1.0
    worst_x = -1
    for x in range(2, W - 2):  # borders can catch resampling edge effects
        e = expected(x)
        if skip_clipped and e >= 254.5:
            continue
        d = abs(got[x] - e)
        if d > worst:
            worst, worst_x = d, x
    status = "PASS" if worst <= tol else "FAIL"
    print(f"{status}  {name:34s} worst |err| = {worst:5.2f} @ x={worst_x}")
    return worst <= tol


def check_rgb(name, got, expected, tol=3.0, skip=lambda x: False):
    """expected(x) -> (r, g, b) in 0..255 or None to skip that column."""
    worst = -1.0
    worst_x = -1
    for x in range(2, W - 2):
        if skip(x):
            continue
        e = expected(x)
        if e is None:
            continue
        d = max(abs(got[x][c] - e[c]) for c in range(3))
        if d > worst:
            worst, worst_x = d, x
    status = "PASS" if worst <= tol else "FAIL"
    print(f"{status}  {name:34s} worst |err| = {worst:5.2f} @ x={worst_x}")
    return worst <= tol


def identical(name, a, b):
    ident = max(abs(a[x] - b[x]) for x in range(W))
    print(f"{'PASS' if ident == 0 else 'FAIL'}  {name:34s} max |diff| = {ident}")
    return ident == 0


GATE_OFF = "Layer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=10\nLayer1GateFeather=35\nLayer1GateStrength=0\n"


def main():
    make_inputs()
    gray_path = os.path.join(HERE, "partner_gray.png").replace("\\", "/")
    rgrad_path = os.path.join(HERE, "partner_rgrad.png").replace("\\", "/")
    grad_path = os.path.join(HERE, "base_grad.png").replace("\\", "/")
    blue_path = os.path.join(HERE, "partner_blue.png").replace("\\", "/")

    P_GRAY = srgb_to_lin(128)
    ok = True

    # ------------------------------------------------------------------
    # Legacy identity. These pp3s carry no Compare / Softness /
    # HighlightLatitude keys, so they must render exactly as they did before
    # those parameters existed.
    # ------------------------------------------------------------------

    # T1: legacy pp3 (global BlendMode/FillShadows) vs new per-layer schema
    # must render pixel-identically.
    legacy = write_pp3("t1_legacy.pp3",
        "Enabled=true\nBlendMode=1\nAutoGain=false\nBaseEV=0\nFillShadows=40\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1EV=0\nLayer1Opacity=100\n")
    newfmt = write_pp3("t1_new.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=1\nLayer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=35\n"
        "Layer1GateFeather=33\nLayer1GateStrength=40\n")
    a = row(render(legacy, "base_grad.png", "t1_legacy.tif"))
    b = row(render(newfmt, "base_grad.png", "t1_new.tif"))
    ok &= identical("legacy-vs-new migration", a, b)

    # ...and the legacy render itself must match the hand-computed legacy
    # fill-shadows math (screen gated into base shadows at strength 0.40).
    def t1_expected(x):
        base = srgb_to_lin(x)
        scr = 1 - (1 - base) * (1 - P_GRAY)
        w = 0.60 + 0.40 * smoothwindow(gate_encode(base), 0.0, 0.35, 0.33)
        return lin_to_srgb(base + w * (scr - base))
    ok &= check("migrated fill-gate math", a, t1_expected)

    # T2: DARKEN, gate off -> min(base, partner). No Compare key: legacy
    # per-channel pick (identical to whole-pixel on gray anyway).
    pp3 = write_pp3("t2_darken.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=4\n" + GATE_OFF)
    t2 = row(render(pp3, "base_grad.png", "t2_darken.tif"))
    ok &= check("darken min(a,b)", t2,
                lambda x: lin_to_srgb(min(srgb_to_lin(x), P_GRAY)))

    # T3: SCREEN revealed only in base highlights (window 70..100, feather 20)
    pp3 = write_pp3("t3_higate.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=1\nLayer1GateSource=0\nLayer1GateLow=70\nLayer1GateHigh=100\n"
        "Layer1GateFeather=20\nLayer1GateStrength=100\n")
    got = row(render(pp3, "base_grad.png", "t3_higate.tif"))
    def t3_expected(x):
        base = srgb_to_lin(x)
        scr = 1 - (1 - base) * (1 - P_GRAY)
        w = smoothwindow(gate_encode(base), 0.70, 1.00, 0.20)
        return lin_to_srgb(base + w * (scr - base))
    ok &= check("screen gated to highlights", got, t3_expected)

    # T4: ABSDIFF of opposing gradients, gate off
    pp3 = write_pp3("t4_absdiff.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={rgrad_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=5\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t4_absdiff.tif"))
    ok &= check("absdiff |a-b|", got,
                lambda x: lin_to_srgb(abs(srgb_to_lin(x) - srgb_to_lin(255 - x))))

    # T5: ADD gated on the LAYER's own luminance (shadows of the partner
    # gradient), on a uniform gray base.
    pp3 = write_pp3("t5_layergate.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={grad_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\nLayer1GateSource=1\nLayer1GateLow=0\nLayer1GateHigh=30\n"
        "Layer1GateFeather=10\nLayer1GateStrength=100\n")
    got = row(render(pp3, "partner_gray.png", "t5_layergate.tif"))
    def t5_expected(x):
        layer = srgb_to_lin(x)
        w = smoothwindow(gate_encode(layer), 0.0, 0.30, 0.10)
        return lin_to_srgb(P_GRAY + w * layer)
    ok &= check("add gated on layer shadows", got, t5_expected, skip_clipped=True)

    # T6: muted layer must render exactly like no double exposure at all.
    off = write_pp3("t6_off.pp3", "Enabled=false\n")
    mute = write_pp3("t6_mute.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=false\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\nLayer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=35\n"
        "Layer1GateFeather=33\nLayer1GateStrength=25\n")
    a = row(render(off, "base_grad.png", "t6_off.tif"))
    b = row(render(mute, "base_grad.png", "t6_mute.tif"))
    ok &= identical("muted layer == no composite", a, b)

    # T7: ADD auto film gain with one ADD layer -> base and layer both 1/2.
    pp3 = write_pp3("t7_autogain.pp3",
        "Enabled=true\nAutoGain=true\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t7_autogain.tif"))
    ok &= check("add auto film gain 1/2", got,
                lambda x: lin_to_srgb(0.5 * (srgb_to_lin(x) + P_GRAY)))

    # T7b: a layer with NO gate key and no FillShadows must load the legacy
    # 25% shadow gate, not the constructor's new default. Bitwise against the
    # explicit legacy window.
    implicit = write_pp3("t7b_implicit.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=1\n")
    explicit = write_pp3("t7b_explicit.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=1\nLayer1Compare=1\nLayer1Softness=0\nLayer1GateSource=0\nLayer1GateLow=0\n"
        "Layer1GateHigh=35\nLayer1GateFeather=33\nLayer1GateStrength=25\n")
    a = row(render(implicit, "base_grad.png", "t7b_implicit.tif"))
    b = row(render(explicit, "base_grad.png", "t7b_explicit.tif"))
    ok &= identical("gate-key-absent == legacy 25%", a, b)

    # T7c: HighlightLatitude absent == HighlightLatitude=0 (the T2 body).
    pp3 = write_pp3("t7c_lat0.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=4\n" + GATE_OFF)
    b = row(render(pp3, "base_grad.png", "t7c_lat0.tif"))
    ok &= identical("latitude-key-absent == 0", t2, b)

    # ------------------------------------------------------------------
    # Phase 1: comparative bright / dark by whole-pixel brightness.
    # ------------------------------------------------------------------

    # T8: red gradient base vs constant blue partner, LIGHTEN, whole pixel,
    # softness 0. Every output pixel must be one of the two inputs (no
    # invented colours), the partner winning where its luminance is higher.
    pp3 = write_pp3("t8_bright.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={blue_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=3\nLayer1Compare=0\nLayer1Softness=0\n" + GATE_OFF, SRGB_WORKING)
    got = row_rgb(render(pp3, "base_red.png", "t8_bright.tif"))
    Y_BLUE = lum709(0.0, 0.0, srgb_to_lin(220))

    def t8_expected(x):
        yb = lum709(srgb_to_lin(x), 0.0, 0.0)
        if abs(yb - Y_BLUE) < 0.004:
            return None  # too close to the tie to call
        return (0, 0, 220) if Y_BLUE > yb else (x, 0, 0)
    ok &= check_rgb("comparative bright: whole pixel", got, t8_expected)
    partner_wins = [x for x in range(2, W - 2) if t8_expected(x) == (0, 0, 220)]
    base_wins = [x for x in range(2, W - 2) if t8_expected(x) == (x, 0, 0)]
    both = bool(partner_wins) and bool(base_wins) and max(partner_wins) < min(base_wins)
    print(f"{'PASS' if both else 'FAIL'}  {'bright crossover present':34s} partner<{min(base_wins) if base_wins else -1}")
    ok &= both

    # T8b: same inputs, no Compare key -> legacy per-channel max, which is
    # the magenta (x, 0, 220) that neither frame contains.
    pp3 = write_pp3("t8b_channel.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={blue_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=3\n" + GATE_OFF, SRGB_WORKING)
    got = row_rgb(render(pp3, "base_red.png", "t8b_channel.tif"))
    ok &= check_rgb("legacy per-channel max", got, lambda x: (x, 0, 220))

    # T8c: DARKEN whole pixel keeps the darker frame whole.
    pp3 = write_pp3("t8c_dark.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={blue_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=4\nLayer1Compare=0\nLayer1Softness=0\n" + GATE_OFF, SRGB_WORKING)
    got = row_rgb(render(pp3, "base_red.png", "t8c_dark.tif"))

    def t8c_expected(x):
        yb = lum709(srgb_to_lin(x), 0.0, 0.0)
        if abs(yb - Y_BLUE) < 0.004:
            return None
        return (0, 0, 220) if Y_BLUE < yb else (x, 0, 0)
    ok &= check_rgb("comparative dark: whole pixel", got, t8c_expected)

    # T9: softness 1 stop on gray: hand-over follows smoothstep over log2
    # luminance ratio, centred on the tie.
    pp3 = write_pp3("t9_soft.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=3\nLayer1Compare=0\nLayer1Softness=1.0\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t9_soft.tif"))

    def t9_expected(x):
        yb = srgb_to_lin(x)
        w = compare_weight(yb, P_GRAY, True, 1.0)
        return lin_to_srgb(yb + w * (P_GRAY - yb))
    ok &= check("comparative bright, softness 1", got, t9_expected)

    # ------------------------------------------------------------------
    # Phase 2: highlight latitude (film shoulder on the finished stack).
    # ------------------------------------------------------------------

    # T10: ADD, auto gain off, latitude 50 -> knee 0.75. The sum passes
    # white over the top third of the gradient; it must roll off instead of
    # clipping flat.
    pp3 = write_pp3("t10_latitude.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=50\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t10_latitude.tif"))
    ok &= check("latitude 50 shoulder", got,
                lambda x: lin_to_srgb(shoulder(srgb_to_lin(x) + P_GRAY, 0.75)))
    # ...and it genuinely differs from the hard clip where the sum exceeds white.
    hard = [lin_to_srgb(min(srgb_to_lin(x) + P_GRAY, 1.0)) for x in range(W)]
    diff = max(abs(got[x] - hard[x]) for x in range(200, W - 2))
    print(f"{'PASS' if diff > 6 else 'FAIL'}  {'shoulder differs from clip':34s} max |diff| = {diff:5.2f}")
    ok &= diff > 6

    # T10b: latitude 100 -> knee 0.5, same shape.
    pp3 = write_pp3("t10b_latitude.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=100\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t10b_latitude.tif"))
    ok &= check("latitude 100 shoulder", got,
                lambda x: lin_to_srgb(shoulder(srgb_to_lin(x) + P_GRAY, 0.5)))

    # T10c: with auto film gain ON the shoulder is referenced to one frame's
    # white: shoulder the un-metered sum, then halve. A knee in pipeline units
    # would never engage on an averaged stack (it tops out near 0.5).
    pp3 = write_pp3("t10c_latitude_avg.pp3",
        "Enabled=true\nAutoGain=true\nBaseEV=0\nHighlightLatitude=50\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t10c_latitude_avg.tif"))
    ok &= check("latitude 50 under auto gain", got,
                lambda x: lin_to_srgb(0.5 * shoulder(srgb_to_lin(x) + P_GRAY, 0.75)))
    avg = [lin_to_srgb(0.5 * (srgb_to_lin(x) + P_GRAY)) for x in range(W)]
    diff = max(abs(got[x] - avg[x]) for x in range(200, W - 2))
    print(f"{'PASS' if diff > 6 else 'FAIL'}  {'shoulder engages under avg':34s} max |diff| = {diff:5.2f}")
    ok &= diff > 6

    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    sys.exit(0 if ok else 1)


main()
