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
GEO = 256  # square geometry fixtures
SCENE_W, SCENE_H = 512, 384  # segmentation fixture
SCENE_HORIZON = 0.55         # fraction of SCENE_H above which it is sky

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

    # Geometry fixtures. Square, so a cover fit onto the 256x64 base is
    # exactly 1:1 horizontally and the probe row reads the partner's middle.
    # One ramps across, one ramps down: a quarter turn swaps which of them
    # varies along the probe row, which is what makes rotation checkable
    # from a single row of pixels.
    hramp = Image.new("RGB", (GEO, GEO))
    hramp_px = hramp.load()
    vramp = Image.new("RGB", (GEO, GEO))
    vramp_px = vramp.load()
    for x in range(GEO):
        for y in range(GEO):
            hramp_px[x, y] = (x, x, x)
            vramp_px[x, y] = (y, y, y)
    hramp.save(os.path.join(HERE, "partner_hramp.png"))
    vramp.save(os.path.join(HERE, "partner_vramp.png"))

    # A scene the segmentation model can find something in: sky over ground,
    # with the horizon at 55% down the frame. Subject selection is judged on
    # real photographs, but the plumbing - decode, segment, feather, bounding
    # box, weight - is checkable against a boundary we placed ourselves.
    scene = Image.new("RGB", (SCENE_W, SCENE_H))
    scene_px = scene.load()
    for y in range(SCENE_H):
        for x in range(SCENE_W):
            if y < SCENE_H * SCENE_HORIZON:
                t = y / (SCENE_H * SCENE_HORIZON)
                scene_px[x, y] = (int(90 + 90 * t), int(140 + 70 * t), int(225 - 25 * t))
            else:
                t = (y - SCENE_H * SCENE_HORIZON) / (SCENE_H * (1 - SCENE_HORIZON))
                scene_px[x, y] = (int(70 - 20 * t), int(110 - 40 * t), int(50 - 20 * t))
    scene.save(os.path.join(HERE, "partner_scene.png"))


def sample_ramp(u):
    """Linear value the engine reads from a 256-wide ramp at continuous
    coordinate u — samplePartner's bilinear fetch of the LINEARIZED pixels,
    edge-clamped, at skip 1."""
    tu = u - 0.5
    x0 = math.floor(tu)
    dx = tu - x0
    if x0 < 0:
        x0, dx = 0, 0.0
    elif x0 > GEO - 1:
        x0, dx = GEO - 1, 0.0
    x1 = min(x0 + 1, GEO - 1)
    return srgb_to_lin(x0) + dx * (srgb_to_lin(x1) - srgb_to_lin(x0))


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


def col(path, x=W // 2):
    im = Image.open(path).convert("RGB")
    assert im.size == (W, H), im.size
    px = im.load()
    return [px[x, y][1] for y in range(H)]


def check(name, got, expected, tol=3.0, skip_clipped=False, skipx=()):
    worst = -1.0
    worst_x = -1
    for x in range(2, W - 2):  # borders can catch resampling edge effects
        if x in skipx:
            continue
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

    # ------------------------------------------------------------------
    # Placement: the partner is cover-fitted, scaled about its centre and
    # shifted; outside its placed frame it contributes nothing.
    # ------------------------------------------------------------------

    # T13: same-aspect partner at 50% size -> covers the central half of the
    # frame (x in [64,192) on the probe row); ADD, auto gain off.
    pp3 = write_pp3("t13_scale.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\nLayer1OffsetX=0\nLayer1OffsetY=0\nLayer1Scale=50\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t13_scale.tif"))

    def t13_expected(x):
        inside = 64 <= x < 192
        return lin_to_srgb(srgb_to_lin(x) + (P_GRAY if inside else 0.0))
    ok &= check("placement: 50% size, centred", got, t13_expected, skipx=(63, 64, 191, 192))

    # T13b: shifted right by 25% of the frame at 50% size -> x in [128,256).
    pp3 = write_pp3("t13b_offset.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\nLayer1OffsetX=25\nLayer1OffsetY=0\nLayer1Scale=50\n" + GATE_OFF)
    got = row(render(pp3, "base_grad.png", "t13b_offset.tif"))

    def t13b_expected(x):
        inside = 128 <= x
        return lin_to_srgb(srgb_to_lin(x) + (P_GRAY if inside else 0.0))
    ok &= check("placement: shifted +25%", got, t13b_expected, skip_clipped=True, skipx=(127, 128))

    # T13c: placement keys at their defaults must be bitwise the legacy
    # cover fit (no frame-edge coverage on an untouched layer).
    pp3 = write_pp3("t13c_default.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=4\nLayer1OffsetX=0\nLayer1OffsetY=0\nLayer1Scale=100\n" + GATE_OFF)
    b = row(render(pp3, "base_grad.png", "t13c_default.tif"))
    ok &= identical("placement defaults == cover fit", t2, b)

    # ------------------------------------------------------------------
    # Rotation. A square partner cover-fits the 256x64 base 1:1 across, so
    # the probe row reads the partner's middle. Turning it a quarter swaps
    # the axes: the across-ramp goes flat and the down-ramp starts varying.
    # ------------------------------------------------------------------
    hramp_path = os.path.join(HERE, "partner_hramp.png").replace("\\", "/")
    vramp_path = os.path.join(HERE, "partner_vramp.png").replace("\\", "/")

    def geo_pp3(name, partner, extra_keys):
        return write_pp3(name,
            "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
            f"LayerCount=1\nLayer1Path={partner}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
            "Layer1BlendMode=0\n" + extra_keys + GATE_OFF)

    # T14: the down-ramp unturned reads its own middle row everywhere, so
    # the layer contributes one constant value across the frame.
    got = row(render(geo_pp3("t14_vramp.pp3", vramp_path, ""), "base_grad.png", "t14_vramp.tif"))
    ok &= check("rotate 0: down-ramp is flat", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(GEO / 2 + 0.5)),
                skip_clipped=True)

    # T14a: turned a quarter clockwise, the same partner ramps along the row
    # - and backwards, because the inverse map reads v = 256 - fx.
    got = row(render(geo_pp3("t14a_rot90.pp3", vramp_path, "Layer1Rotate=90\n"),
                     "base_grad.png", "t14a_rot90.tif"))
    ok &= check("rotate 90: down-ramp reads across", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(GEO - (x + 0.5))),
                skip_clipped=True)

    # T14b: the across-ramp turned a quarter goes flat, at its own middle.
    got = row(render(geo_pp3("t14b_rot90h.pp3", hramp_path, "Layer1Rotate=90\n"),
                     "base_grad.png", "t14b_rot90h.tif"))
    ok &= check("rotate 90: across-ramp goes flat", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(GEO / 2 + 0.5)),
                skip_clipped=True)

    # T14c: mirroring the across-ramp reverses it.
    got = row(render(geo_pp3("t14c_flip.pp3", hramp_path, "Layer1FlipH=true\n"),
                     "base_grad.png", "t14c_flip.tif"))
    ok &= check("mirror: across-ramp reverses", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(GEO - (x + 0.5))),
                skip_clipped=True)

    # T14d: an unturned, unmirrored layer must still be bitwise the legacy
    # cover fit - the new keys may not disturb an untouched exposure.
    a = row(render(geo_pp3("t14d_plain.pp3", hramp_path, ""), "base_grad.png", "t14d_plain.tif"))
    b = row(render(geo_pp3("t14d_zero.pp3", hramp_path,
                           "Layer1Rotate=0\nLayer1Pattern=0\nLayer1FlipH=false\n"),
                   "base_grad.png", "t14d_zero.tif"))
    ok &= identical("rotate/pattern zero == untouched", a, b)

    # ------------------------------------------------------------------
    # Patterning. At quarter size the tile is 64 base px wide, so the frame
    # holds four of them; the across-ramp then reads as a sawtooth.
    # ------------------------------------------------------------------
    def tile_u(x, mirror=False, spacing=0.0, stagger=0.0):
        """deplace::map's wrap, for a square partner at 25% over the base."""
        scale = 0.25
        su = (x + 0.5 - W / 2) / scale       # invCover is 1 for this pair
        sv = (PROBE_ROW + 0.5 - H / 2) / scale
        cw = GEO * (1.0 + spacing)
        tx = su / cw
        ty = sv / cw
        row_i = math.floor(ty + 0.5)
        if stagger and (int(row_i) & 1):
            tx += stagger
        col = math.floor(tx + 0.5)
        rx = tx - col
        if mirror and (int(col) & 1):
            rx = -rx
        return rx * cw + GEO / 2

    # The seam between tiles is anti-aliased over aa = 1/scale source px, so
    # the columns that land on a tile edge are not hand-computable.
    def seam(x, **kw):
        u = tile_u(x, **kw)
        return u < 6.0 or u > GEO - 6.0

    got = row(render(geo_pp3("t15_repeat.pp3", hramp_path, "Layer1Scale=25\nLayer1Pattern=1\n"),
                     "base_grad.png", "t15_repeat.tif"))
    ok &= check("repeat: four tiles of the ramp", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(tile_u(x))),
                skip_clipped=True, skipx={x for x in range(W) if seam(x)})

    # T15b: mirrored tiles reflect their neighbours, so the sawtooth becomes
    # a triangle wave and the seams stop jumping.
    got = row(render(geo_pp3("t15b_mirror.pp3", hramp_path, "Layer1Scale=25\nLayer1Pattern=2\n"),
                     "base_grad.png", "t15b_mirror.tif"))
    ok &= check("mirrored tiles reflect", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(tile_u(x, mirror=True))),
                skip_clipped=True, skipx={x for x in range(W) if seam(x, mirror=True)})

    # T15c: a full tile of spacing leaves the base untouched in the gutters.
    got = row(render(geo_pp3("t15c_gutter.pp3", hramp_path,
                             "Layer1Scale=25\nLayer1Pattern=1\nLayer1PatternSpacing=100\n"),
                     "base_grad.png", "t15c_gutter.tif"))
    plain = row(render(geo_pp3("t15c_base.pp3", hramp_path, "Layer1Opacity=0\n"),
                       "base_grad.png", "t15c_base.tif"))
    gutters = [x for x in range(2, W - 2)
               if not (-8.0 < tile_u(x, spacing=1.0) < GEO + 8.0)]
    worst = max(abs(got[x] - plain[x]) for x in gutters) if gutters else 99.0
    status = "PASS" if worst <= 1 and len(gutters) > 20 else "FAIL"
    print(f"{status}  {'spacing 100: gutters are base':34s} worst |err| = {worst:5.2f}"
          f"  over {len(gutters)} px")
    ok &= worst <= 1 and len(gutters) > 20


    # ------------------------------------------------------------------
    # Radial repeat: N copies stood around a ring, each turned to face
    # outward. With two copies the ring lies along the probe row, so both
    # are readable from it - and the far one arrives turned a half turn,
    # which is what makes the check worth writing.
    # ------------------------------------------------------------------
    def radial_u(x, count=2, diameter=50.0):
        ring = 0.5 * (diameter / 100.0) * W        # invCover and scale are 1
        su = x + 0.5 - W / 2
        sv = PROBE_ROW + 0.5 - H / 2
        step = 2 * math.pi / count
        spoke = math.floor(math.atan2(sv, su) / step + 0.5) * step
        rx = math.cos(spoke) * su + math.sin(spoke) * sv
        return rx - ring + GEO / 2

    got = row(render(geo_pp3("t18_radial.pp3", hramp_path,
                             "Layer1Pattern=3\nLayer1PatternCount=2\nLayer1PatternDiameter=50\n"),
                     "base_grad.png", "t18_radial.tif"))
    ok &= check("radial: two copies, one turned", got,
                lambda x: lin_to_srgb(srgb_to_lin(x) + sample_ramp(radial_u(x))),
                skip_clipped=True, skipx={126, 127, 128, 129})

    # T18b: the count actually counts. One copy stands alone on the +x spoke,
    # so the far side of the frame is left to the base; two copies reach it.
    # (A column would have done for six-versus-two, but only weakly - along
    # the centre line both resolve to the same pair of spokes.)
    one = row(render(geo_pp3("t18b_one.pp3", hramp_path,
                             "Layer1Pattern=3\nLayer1PatternCount=1\nLayer1PatternDiameter=50\n"),
                     "base_grad.png", "t18b_one.tif"))
    bare = row(render(geo_pp3("t18b_bare.pp3", hramp_path, "Layer1Opacity=0\n"),
                      "base_grad.png", "t18b_bare.tif"))
    far = range(2, 56)
    alone = max(abs(one[x] - bare[x]) for x in far)
    paired = max(abs(got[x] - bare[x]) for x in far)
    good = alone <= 1 and paired > 20
    print(f"{'PASS' if good else 'FAIL'}  {'radial: one copy leaves the far side':34s} "
          f"one = {alone}, two = {paired}")
    ok &= good

    # T18c: widening the ring moves the copies outward, so the frame changes.
    wide = row(render(geo_pp3("t18c_wide.pp3", hramp_path,
                              "Layer1Pattern=3\nLayer1PatternCount=2\nLayer1PatternDiameter=150\n"),
                      "base_grad.png", "t18c_wide.tif"))
    moved = max(abs(wide[x] - got[x]) for x in range(2, W - 2))
    print(f"{'PASS' if moved > 20 else 'FAIL'}  {'radial: diameter moves copies':34s} max |diff| = {moved}")
    ok &= moved > 20

    # ------------------------------------------------------------------
    # Subject selection. The model is not the thing under test here - the
    # plumbing is: that a class map reaches the weight, that inverting it
    # swaps which half of the frame the layer lands in, and above all that
    # asking for no mask changes nothing at all.
    # ------------------------------------------------------------------
    scene_path = os.path.join(HERE, "partner_scene.png").replace("\\", "/")

    def scene_pp3(name, extra_keys):
        return write_pp3(name,
            "Enabled=true\nAutoGain=false\nBaseEV=0\nHighlightLatitude=0\n"
            f"LayerCount=1\nLayer1Path={scene_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
            "Layer1BlendMode=0\n" + extra_keys + GATE_OFF)

    # T16: an explicit "whole frame" must be bitwise the same as no key.
    a = col(render(scene_pp3("t16_none.pp3", ""), "base_grad.png", "t16_none.tif"))
    b = col(render(scene_pp3("t16_off.pp3", "Layer1MaskClass=0\nLayer1MaskFeather=25\n"),
                   "base_grad.png", "t16_off.tif"))
    ident = max(abs(a[y] - b[y]) for y in range(H))
    print(f"{'PASS' if ident == 0 else 'FAIL'}  {'mask off == no mask':34s} max |diff| = {ident}")
    ok &= ident == 0

    # The base is 256x64 and the partner 512x384, so the cover fit shows the
    # partner's middle 128 rows: base row y reads partner row 2y + 129. The
    # horizon at 0.55 * 384 = 211 therefore lands on base row 41.
    horizon_row = int((SCENE_H * SCENE_HORIZON - (SCENE_H / 2 - H)) / 2)

    sky = col(render(scene_pp3("t16b_sky.pp3", "Layer1MaskClass=3\nLayer1MaskFeather=25\n"),
                     "base_grad.png", "t16b_sky.tif"))
    plain = col(render(scene_pp3("t16b_plain.pp3", "Layer1Opacity=0\n"),
                       "base_grad.png", "t16b_plain.tif"))

    above = max(abs(sky[y] - plain[y]) for y in range(2, horizon_row - 6))
    below = max(abs(sky[y] - plain[y]) for y in range(horizon_row + 6, H - 2))
    good = above > 20 and below <= 2
    print(f"{'PASS' if good else 'FAIL'}  {'sky mask lands above horizon':34s} "
          f"above = {above}, below = {below}")
    ok &= good

    # T16c: inverting it swaps the two halves.
    inv = col(render(scene_pp3("t16c_inv.pp3",
                               "Layer1MaskClass=3\nLayer1MaskFeather=25\nLayer1MaskInvert=true\n"),
                     "base_grad.png", "t16c_inv.tif"))
    iabove = max(abs(inv[y] - plain[y]) for y in range(2, horizon_row - 6))
    ibelow = max(abs(inv[y] - plain[y]) for y in range(horizon_row + 6, H - 2))
    good = ibelow > 20 and iabove <= 2
    print(f"{'PASS' if good else 'FAIL'}  {'inverted mask lands below':34s} "
          f"above = {iabove}, below = {ibelow}")
    ok &= good

    # T17: cropping to the subject makes the selection's bounding box the
    # exposure's frame. The sky box is the top 220 rows of the partner, so
    # the cover fit now shows sky everywhere instead of sky over a horizon -
    # which is what lets a pattern repeat a cut-out rather than a picture.
    crop = col(render(scene_pp3("t17_crop.pp3",
                                "Layer1MaskClass=3\nLayer1MaskFeather=25\nLayer1CropToSubject=true\n"),
                      "base_grad.png", "t17_crop.tif"))
    cabove = max(abs(crop[y] - plain[y]) for y in range(2, horizon_row - 6))
    cbelow = max(abs(crop[y] - plain[y]) for y in range(horizon_row + 6, H - 2))
    good = cabove > 20 and cbelow > 20
    print(f"{'PASS' if good else 'FAIL'}  {'crop to subject fills the frame':34s} "
          f"above = {cabove}, below = {cbelow}")
    ok &= good

    # T17b: the same keys one flag apart must not render the same picture.
    moved = max(abs(sky[y] - crop[y]) for y in range(H))
    print(f"{'PASS' if moved > 20 else 'FAIL'}  {'crop changes the framing':34s} max |diff| = {moved}")
    ok &= moved > 20

    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    sys.exit(0 if ok else 1)


main()
