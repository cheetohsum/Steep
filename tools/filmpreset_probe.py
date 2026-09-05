#!/usr/bin/env python3
"""What does each film preset actually do to a picture?

Renders one synthetic chart through every preset at full strength and reports,
per preset, the direction and size of the colour move it makes. The point is
not that any single number is right -- it is that each preset's signature shows
up in the column it is supposed to, and that no two presets end up measuring
the same picture.

    python tools/filmpreset_probe.py                 # the table
    python tools/filmpreset_probe.py --gate          # table plus pass/fail
    python tools/filmpreset_probe.py --save before.json
    python tools/filmpreset_probe.py --compare before.json

Columns, all measured against the untouched render of the same chart:
  warm      mean (R-B) shift over the grey ramp, in 8-bit counts. Positive is
            a warmer picture; this is the axis "yellower" lives on.
  yellow    mean (R+G)/2 - B shift on the warm patches -- yellow separated
            from plain red, since a stock can warm without going yellow.
  chroma    mean change in max(RGB)-min(RGB) over the colour patches, which is
            how much more (or less) saturated it renders.
  contrast  (white patch - black patch) shift: the slope of the thing.
  split     shadow warmth minus highlight warmth. Negative means cool shadows
            under warm highlights, which is what a split-toned stock does.
"""
import argparse
import json
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"

# Preset ids, in the order rtengine/ipfilmpresets.cc declares them. "custom" is
# the identity recipe and is left out: it has no character to measure.
PRESETS = [
    "heritage_gold", "porcelain_400", "vivid_chrome", "arctic", "sovereign",
    "golden_hour", "twilight_160", "nostalgia_200", "desert_chrome",
    "street_800", "cinematic_500t", "fade_bloom", "ember", "silver_gelatin",
    "analog_dream", "cinema_reveal_35",
]

PATCH = 32
# A grey ramp along the top, then colour patches. Each is a flat block so a
# single sample per patch is the patch.
GREYS = [8, 24, 48, 72, 104, 136, 168, 200, 232, 250]
COLOURS = {
    "red": (200, 40, 40), "orange": (210, 130, 45), "yellow": (215, 200, 60),
    "green": (60, 170, 80), "cyan": (60, 180, 190), "blue": (60, 90, 190),
    "magenta": (180, 70, 160), "skin": (215, 165, 140), "sky": (110, 150, 205),
    "foliage": (95, 130, 70),
}
WARM_PATCHES = ("red", "orange", "yellow", "skin")


def make_chart(path):
    w = PATCH * max(len(GREYS), len(COLOURS))
    im = Image.new("RGB", (w, PATCH * 2), (0, 0, 0))
    px = im.load()

    for i, v in enumerate(GREYS):
        for y in range(PATCH):
            for x in range(PATCH):
                px[i * PATCH + x, y] = (v, v, v)

    for i, rgb in enumerate(COLOURS.values()):
        for y in range(PATCH):
            for x in range(PATCH):
                px[i * PATCH + x, PATCH + y] = rgb

    im.save(path)


PP3 = """[Film Presets]
Enabled={on}
Preset={preset}
Strength=100
Contrast=0
Saturation=0
Warmth=0
Tint=0
Fade=0
Rolloff=0
Halation=0
RedShift=0
GreenShift=0
BlueShift=0
Grain=-100
Vibrance=0
"""


def render(tag, text, chart):
    pp3 = os.path.join(HERE, "fp_%s.pp3" % tag)

    with open(pp3, "w", newline="\n") as f:
        f.write(text)

    out = os.path.join(HERE, "fp_%s" % tag)

    for f in (out, out + ".tif"):
        if os.path.exists(f):
            os.remove(f)

    r = subprocess.run([CLI, "-Y", "-o", out, "-d", "-p", pp3, "-t", "-b8", "-c", chart],
                       capture_output=True, text=True)
    got = out + ".tif" if os.path.exists(out + ".tif") else out

    if not os.path.exists(got):
        print(r.stderr[-1500:])
        sys.exit("render failed: " + tag)

    return Image.open(got).convert("RGB").load()


def sample(px):
    """Mean RGB of the middle of every patch, keyed by name."""
    out = {}
    half = PATCH // 2

    for i, v in enumerate(GREYS):
        out["grey%d" % v] = px[i * PATCH + half, half]

    for i, name in enumerate(COLOURS):
        out[name] = px[i * PATCH + half, PATCH + half]

    return out


def measure(base, shot):
    greys = ["grey%d" % v for v in GREYS]
    warm = (sum((shot[k][0] + shot[k][1]) / 2.0 - shot[k][2]
                - ((base[k][0] + base[k][1]) / 2.0 - base[k][2]) for k in WARM_PATCHES)
            / len(WARM_PATCHES))
    tone = (sum(shot[k][0] - shot[k][2] - (base[k][0] - base[k][2]) for k in greys)
            / len(greys))
    chroma = (sum((max(shot[k]) - min(shot[k])) - (max(base[k]) - min(base[k]))
                  for k in COLOURS) / len(COLOURS))
    contrast = ((shot["grey250"][1] - shot["grey8"][1])
                - (base["grey250"][1] - base["grey8"][1]))
    shadow = shot["grey24"][0] - shot["grey24"][2] - (base["grey24"][0] - base["grey24"][2])
    high = shot["grey232"][0] - shot["grey232"][2] - (base["grey232"][0] - base["grey232"][2])

    return {"warm": tone, "yellow": warm, "chroma": chroma,
            "contrast": contrast, "split": shadow - high}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", action="store_true")
    ap.add_argument("--save")
    ap.add_argument("--compare")
    args = ap.parse_args()

    chart = os.path.join(HERE, "fp_chart.png")
    make_chart(chart)
    base = sample(render("off", PP3.format(on="false", preset="heritage_gold"), chart))

    rows = {}
    print("  %-18s %7s %7s %7s %9s %7s" %
          ("preset", "warm", "yellow", "chroma", "contrast", "split"))

    vectors = {}

    for preset in PRESETS:
        shot = sample(render(preset, PP3.format(on="true", preset=preset), chart))
        rows[preset] = measure(base, shot)
        # Every patch, every channel, as a move away from the untouched
        # picture, so a preset that does little sits near the origin rather
        # than near its neighbours by accident.
        vectors[preset] = [c - d for k in sorted(shot)
                           for c, d in zip(shot[k], base[k])]
        print("  %-18s %+7.1f %+7.1f %+7.1f %+9.1f %+7.1f"
              % (preset, rows[preset]["warm"], rows[preset]["yellow"],
                 rows[preset]["chroma"], rows[preset]["contrast"], rows[preset]["split"]))

    if args.save:
        with open(args.save, "w") as f:
            json.dump(rows, f, indent=1)
        print("\nsaved to " + args.save)

    if args.compare:
        with open(args.compare) as f:
            was = json.load(f)

        print("\n  change against %s" % args.compare)
        print("  %-18s %7s %7s %7s %9s %7s" %
              ("preset", "warm", "yellow", "chroma", "contrast", "split"))

        for preset in PRESETS:
            if preset not in was:
                continue

            print("  %-18s %+7.1f %+7.1f %+7.1f %+9.1f %+7.1f"
                  % (preset,
                     rows[preset]["warm"] - was[preset]["warm"],
                     rows[preset]["yellow"] - was[preset]["yellow"],
                     rows[preset]["chroma"] - was[preset]["chroma"],
                     rows[preset]["contrast"] - was[preset]["contrast"],
                     rows[preset]["split"] - was[preset]["split"]))

    if args.gate:
        ok = True

        # Sixteen presets are only worth having if they render sixteen
        # different pictures. Compared on the whole patch vector rather than on
        # the five summary axes: a pair can differ on an aggregate and still
        # look like each other, which is how five stocks once sat within seven
        # counts while passing a gate that only asked about the summaries.
        for i, a in enumerate(PRESETS):
            for b in PRESETS[i + 1:]:
                apart = max(abs(x - y) for x, y in zip(vectors[a], vectors[b]))

                if apart < 12.0:
                    print("FAIL  %s and %s render nearly the same picture "
                          "(%.1f counts apart at their most different patch)"
                          % (a, b, apart))
                    ok = False

        # And each has to actually do something at full strength.
        for preset in PRESETS:
            moved = max(abs(v) for v in rows[preset].values())

            if moved < 3.0:
                print("FAIL  %s barely moves the picture (%.1f)" % (preset, moved))
                ok = False

        print("\nALL PASS" if ok else "\nFAILURES PRESENT")
        sys.exit(0 if ok else 1)


# Importable: filmzone_probe reuses the chart, the render and the sampling
# rather than keeping a second copy of them that could drift.
if __name__ == "__main__":
    main()
