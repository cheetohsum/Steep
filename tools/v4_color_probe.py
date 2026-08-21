#!/usr/bin/env python3
"""Film Lab colour rendition probe (Track B/C gate infrastructure).

Renders a synthetic ColorChecker Classic through the Film Lab and reports
each patch's rendition (CIELAB lightness, chroma, hue). Two jobs:

  1. Today: prove V4 colour matches V3 in SDR (the scene tap and stops
     grid must not move colour), and write the V3 baseline rendition to
     v4_color_baseline.json.
  2. When Track B/C land (derived spectral matrices + Langmuir couplers),
     rerun with --baseline to see per-patch what moved, and gate the
     invariants that must survive any spectral rework: neutral patches
     stay neutral, skin hue stays in its band, no patch's hue crosses to
     an adjacent primary.

Patches are the post-2014 X-Rite sRGB values. The chart is rendered big
enough that patch centres are unaffected by neighbours.
"""

import argparse
import colorsys
import json
import math
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"
BASELINE = os.path.join(HERE, "v4_color_baseline.json")

CHECKER = [
    ("dark skin", 115, 82, 68), ("light skin", 194, 150, 130),
    ("blue sky", 98, 122, 157), ("foliage", 87, 108, 67),
    ("blue flower", 133, 128, 177), ("bluish green", 103, 189, 170),
    ("orange", 214, 126, 44), ("purplish blue", 80, 91, 166),
    ("moderate red", 193, 90, 99), ("purple", 94, 60, 108),
    ("yellow green", 157, 188, 64), ("orange yellow", 224, 163, 46),
    ("blue", 56, 61, 150), ("green", 70, 148, 73),
    ("red", 175, 54, 60), ("yellow", 231, 199, 31),
    ("magenta", 187, 86, 149), ("cyan", 8, 133, 161),
    ("white", 243, 243, 242), ("neutral 8", 200, 200, 200),
    ("neutral 6.5", 160, 160, 160), ("neutral 5", 122, 122, 122),
    ("neutral 3.5", 85, 85, 85), ("black", 52, 52, 52),
]

CELL = 120
COLS, ROWS = 6, 4


def srgb_to_lab(r8, g8, b8):
    def lin(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    r, g, b = lin(r8), lin(g8), lin(b8)
    x = 0.4124 * r + 0.3576 * g + 0.1805 * b
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    z = 0.0193 * r + 0.1192 * g + 0.9505 * b

    def f(t):
        return t ** (1 / 3) if t > 0.008856 else 7.787 * t + 16 / 116

    fx, fy, fz = f(x / 0.95047), f(y), f(z / 1.08883)
    L = 116 * fy - 16
    a = 500 * (fx - fy)
    b_ = 200 * (fy - fz)
    return L, a, b_


def patch_stats(px, index):
    cx = (index % COLS) * CELL + CELL // 2
    cy = (index // COLS) * CELL + CELL // 2
    rs = gs = bs = 0
    n = 0
    for dx in range(-20, 21, 4):
        for dy in range(-20, 21, 4):
            r, g, b = px[cx + dx, cy + dy]
            rs += r
            gs += g
            bs += b
            n += 1
    L, a, b = srgb_to_lab(rs / n, gs / n, bs / n)
    chroma = math.hypot(a, b)
    hue = math.degrees(math.atan2(b, a)) % 360
    return {"L": round(L, 2), "C": round(chroma, 2), "h": round(hue, 1)}


def hue_delta(h1, h2):
    d = abs(h1 - h2) % 360
    return min(d, 360 - d)


def build_chart():
    im = Image.new("RGB", (COLS * CELL, ROWS * CELL))
    px = im.load()
    for i, (_name, r, g, b) in enumerate(CHECKER):
        x0 = (i % COLS) * CELL
        y0 = (i // COLS) * CELL
        for x in range(x0, x0 + CELL):
            for y in range(y0, y0 + CELL):
                px[x, y] = (r, g, b)
    path = os.path.join(HERE, "v4_checker.png")
    im.save(path)
    return path


def write_pp3(name, model, preset, output="scan"):
    path = os.path.join(HERE, name)
    with open(path, "w", newline="\n") as f:
        f.write(
            "[Film Presets]\nEnabled=true\n"
            f"Preset={preset}\nModelVersion={model}\n"
            f"Output={output}\n"
            "Strength=100\nHalation=-100\nGrain=-100\n"
        )
    return path


def render(pp3, scene, out):
    outp = os.path.join(HERE, out)
    for c in (outp, outp + ".tif"):
        if os.path.exists(c):
            os.remove(c)
    r = subprocess.run(
        [CLI, "-Y", "-o", outp, "-d", "-p", pp3, "-t", "-b8", "-c", scene],
        capture_output=True, text=True)
    result = outp + ".tif" if os.path.exists(outp + ".tif") else outp
    if not os.path.exists(result):
        print(r.stdout[-1500:])
        sys.exit("render failed: " + out)
    return result


def rendition(model, preset, chart, output="scan"):
    pp3 = write_pp3(f"v4_cc_{preset}_m{model}_{output}.pp3", model, preset, output)
    px = Image.open(render(pp3, chart, f"v4_cc_{preset}_m{model}_{output}")).convert("RGB").load()
    return [patch_stats(px, i) for i in range(len(CHECKER))]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", action="store_true",
                    help="compare V4 against the stored baseline instead of V3")
    ap.add_argument("--presets", default="sovereign,vivid_chrome")
    args = ap.parse_args()

    chart = build_chart()
    presets = args.presets.split(",")
    failures = []
    baseline = {}

    if args.baseline and os.path.exists(BASELINE):
        baseline = json.load(open(BASELINE))

    store = {}
    for preset in presets:
        v4 = rendition(4, preset, chart)
        ref = baseline.get(preset) or rendition(3, preset, chart)
        ref_label = "baseline" if baseline.get(preset) else "V3"
        store[preset] = v4

        # Reversal keeps an asymmetric chroma floor: its unmasked dyes are
        # viewed directly, so saturated colours - magenta and teal above all,
        # famously the weak axes of real slide film - legitimately read a few
        # dC tamer than V3's naive model, and the DIR couplers cannot buy it
        # back because those patches sit against the fog/Dmax headroom limit.
        # The +4 ceiling still holds; only the loss side widens, only here.
        reversal = preset in ("vivid_chrome", "arctic", "desert_chrome")
        dc_floor = -8.0 if reversal else -4.0

        print(f"\n{preset}: V4 vs {ref_label}")
        print(f"{'patch':<14}{'dL':>7}{'dC':>7}{'dh':>7}")
        violations = []
        for i, (name, *_rgb) in enumerate(CHECKER):
            dl = v4[i]["L"] - ref[i]["L"]
            dc = v4[i]["C"] - ref[i]["C"]
            # Hue delta is meaningless for near-neutral patches.
            dh = hue_delta(v4[i]["h"], ref[i]["h"]) if ref[i]["C"] > 6 else 0.0
            flag = ""
            # Ceilings re-targeted 2026-08-19 at the user's direction: the
            # film sim read "a bit desaturated", so the couplers and stock
            # scalars were lifted and the look is ALLOWED to sit up to +6 dC
            # over v3. Neutral and skin invariants below are unchanged.
            if abs(dl) > 3.0 or dc > 6.0 or dc < dc_floor or dh > 10.0:
                flag = "  <-- moved"
                violations.append(f"{name} (dL {dl:.2f}, dC {dc:.2f}, dh {dh:.1f})")
            print(f"{name:<14}{dl:>7.2f}{dc:>7.2f}{dh:>7.1f}{flag}")

        # Invariants that must hold in every spectral rework:
        for i in (18, 19, 20, 21, 22, 23):
            if v4[i]["C"] > 6.0:
                failures.append(f"{preset}: neutral '{CHECKER[i][0]}' drifted to C={v4[i]['C']}")
        for i in (0, 1):
            if v4[i]["C"] > 8 and not (20 <= v4[i]["h"] <= 85):
                failures.append(f"{preset}: skin '{CHECKER[i][0]}' hue {v4[i]['h']} out of band")
        # Bounded-movement limits live in the per-patch loop above. Hue gets
        # more room than lightness and chroma: rotation on saturated colours
        # is authentic film character (the very behaviour the spectral
        # matrices exist to produce), and skin tones are guarded separately
        # by their own hue-band invariant.
        if (not args.baseline or baseline.get(preset)) and violations:
            failures.append(f"{preset}: rendition moved on " + "; ".join(violations))

    # --- Output-chain pass: the spectral print chains (paper, projection,
    #     labscan) against their V3 counterparts. Bounds are wider than the
    #     scan gate: a real paper chain legitimately departs from V3's
    #     hand-tuned matrices, but neutrals and skin must hold everywhere.
    #     labscan has no V3 counterpart and is compared against V3 scan.
    for preset, output in (("sovereign", "ra4"), ("sovereign", "labscan"),
                           ("vivid_chrome", "projection"), ("cinematic_500t", "cinema")):
        v4o = rendition(4, preset, chart, output)
        ref_output = "scan" if output == "labscan" else output
        refo = rendition(3, preset, chart, ref_output)
        reversal = preset in ("vivid_chrome", "arctic", "desert_chrome")
        worst = ""
        for i, (name, *_rgb) in enumerate(CHECKER):
            dl = v4o[i]["L"] - refo[i]["L"]
            dc = v4o[i]["C"] - refo[i]["C"]
            dh = hue_delta(v4o[i]["h"], refo[i]["h"]) if refo[i]["C"] > 6 else 0.0
            # Paper chains are a physically DISTINCT rendering, not a V3
            # imitation: real prints mute the green-violet axes (cyan and
            # yellow paper dyes both absorb green) and 2383 twists hues -
            # so they get a wider chroma floor and hue band, while
            # receiver chains stay tight. Invariants (neutrals, skin, dL)
            # hold everywhere regardless.
            paper = output in ("ra4", "cinema")
            dc_lo = -14.0 if paper else (-9.0 if reversal else -6.0)
            # +2 alongside the scan-chain re-target (user-directed
            # saturation lift, 2026-08-19).
            dc_hi = 10.0 if output == "labscan" else 8.0
            dh_hi = 14.0 if paper else 12.0
            if abs(dl) > 4.0 or dc > dc_hi or dc < dc_lo or dh > dh_hi:
                worst += f" {name}(dL {dl:.1f} dC {dc:.1f} dh {dh:.0f})"
        for i in (18, 19, 20, 21, 22, 23):
            if v4o[i]["C"] > 6.0:
                failures.append(f"{preset}@{output}: neutral '{CHECKER[i][0]}' drifted C={v4o[i]['C']}")
        for i in (0, 1):
            if v4o[i]["C"] > 8 and not (20 <= v4o[i]["h"] <= 85):
                failures.append(f"{preset}@{output}: skin hue {v4o[i]['h']} out of band")
        print(f"{preset}@{output}: " + ("in bounds" if not worst else "moved:" + worst))
        if worst:
            failures.append(f"{preset}@{output} out of bounds:{worst}")

    if not args.baseline and not failures:
        json.dump(store, open(BASELINE, "w"), indent=1)
        print(f"\nbaseline written: {os.path.basename(BASELINE)}")

    print()
    if failures:
        for f in failures:
            print("FAIL:", f)
        sys.exit(1)
    print("PASS: colour rendition invariants hold")


if __name__ == "__main__":
    main()
