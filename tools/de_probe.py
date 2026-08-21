"""Double Exposure Phase-1 verification probe.

Renders synthetic gray gradients through steep-cli with partial pp3s and
compares against hand-computed scene-linear math. Gray inputs keep every
channel equal, so working-space matrix conversions are identity for the
purposes of channelwise blend math.
"""
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"

W, H = 256, 64
PROBE_ROW = H // 2


def srgb_to_lin(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def lin_to_srgb(v):
    v = min(1.0, max(0.0, v))
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return v * 255.0


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


def make_inputs():
    grad = Image.new("RGB", (W, H))
    grad_px = grad.load()
    rgrad = Image.new("RGB", (W, H))
    rgrad_px = rgrad.load()
    gray = Image.new("RGB", (W, H), (128, 128, 128))
    for x in range(W):
        for y in range(H):
            grad_px[x, y] = (x, x, x)
            rgrad_px[x, y] = (255 - x, 255 - x, 255 - x)
    grad.save(os.path.join(HERE, "base_grad.png"))
    rgrad.save(os.path.join(HERE, "partner_rgrad.png"))
    gray.save(os.path.join(HERE, "partner_gray.png"))


def write_pp3(name, body):
    path = os.path.join(HERE, name)
    with open(path, "w", newline="\n") as f:
        f.write("[Double Exposure]\n" + body)
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


def main():
    make_inputs()
    gray_path = os.path.join(HERE, "partner_gray.png").replace("\\", "/")
    rgrad_path = os.path.join(HERE, "partner_rgrad.png").replace("\\", "/")

    P_GRAY = srgb_to_lin(128)
    ok = True

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
    ident = max(abs(a[x] - b[x]) for x in range(W))
    print(f"{'PASS' if ident == 0 else 'FAIL'}  legacy-vs-new migration          max |diff| = {ident}")
    ok &= ident == 0

    # ...and the legacy render itself must match the hand-computed legacy
    # fill-shadows math (screen gated into base shadows at strength 0.40).
    def t1_expected(x):
        base = srgb_to_lin(x)
        scr = 1 - (1 - base) * (1 - P_GRAY)
        w = 0.60 + 0.40 * smoothwindow(gate_encode(base), 0.0, 0.35, 0.33)
        return lin_to_srgb(base + w * (scr - base))
    ok &= check("migrated fill-gate math", a, t1_expected)

    # T2: DARKEN, gate off -> min(base, partner)
    pp3 = write_pp3("t2_darken.pp3",
        "Enabled=true\nAutoGain=false\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=4\nLayer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=10\n"
        "Layer1GateFeather=35\nLayer1GateStrength=0\n")
    got = row(render(pp3, "base_grad.png", "t2_darken.tif"))
    ok &= check("darken min(a,b)", got,
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
        "Layer1BlendMode=5\nLayer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=10\n"
        "Layer1GateFeather=35\nLayer1GateStrength=0\n")
    got = row(render(pp3, "base_grad.png", "t4_absdiff.tif"))
    ok &= check("absdiff |a-b|", got,
                lambda x: lin_to_srgb(abs(srgb_to_lin(x) - srgb_to_lin(255 - x))))

    # T5: ADD gated on the LAYER's own luminance (shadows of the partner
    # gradient), on a uniform gray base.
    grad_path = os.path.join(HERE, "base_grad.png").replace("\\", "/")
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
    ident = max(abs(a[x] - b[x]) for x in range(W))
    print(f"{'PASS' if ident == 0 else 'FAIL'}  muted layer == no composite      max |diff| = {ident}")
    ok &= ident == 0

    # T7: ADD auto film gain with one ADD layer -> base and layer both 1/2.
    pp3 = write_pp3("t7_autogain.pp3",
        "Enabled=true\nAutoGain=true\nBaseEV=0\n"
        f"LayerCount=1\nLayer1Path={gray_path}\nLayer1Enabled=true\nLayer1EV=0\nLayer1Opacity=100\n"
        "Layer1BlendMode=0\nLayer1GateSource=0\nLayer1GateLow=0\nLayer1GateHigh=10\n"
        "Layer1GateFeather=35\nLayer1GateStrength=0\n")
    got = row(render(pp3, "base_grad.png", "t7_autogain.tif"))
    ok &= check("add auto film gain 1/2", got,
                lambda x: lin_to_srgb(0.5 * (srgb_to_lin(x) + P_GRAY)))

    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    sys.exit(0 if ok else 1)


main()
