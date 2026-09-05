#!/usr/bin/env python3
"""Does skin protection actually hold skin, now that stocks grade by tone?

A stock that tints its shadows is doing what film does. A stock that tints the
shadow side of a face is doing what a mistake does, and they are the same
operation -- what separates them is skinProtection, which runs immediately
after the zone stage and pulls skin-confident pixels back toward their source
chroma. It defaults to 35.

Asking "does any stock move skin?" is the wrong question: Fade & Bloom is an
expired-film look whose whole point is a magenta cast, and it is supposed to
land on skin like everything else. The question that matters is whether the
control that exists to hold skin still does its job now that there is more for
it to hold. So each stock is rendered three times -- protection off, at its
default, and at full -- and what is measured is whether turning it up brings
skin back.

    python tools/filmskin_probe.py            # the table
    python tools/filmskin_probe.py --gate     # table plus pass/fail

Monochrome stocks are skipped: hue on a near-grey pixel is whatever rounding
says it is, and Silver Gelatin duly reported 164 degrees of it.
"""
import argparse
import colorsys
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import filmpreset_probe as fp  # noqa: E402

PATCH = 32
# One skin tone at five exposures, from a face in shadow to one in sun, in the
# range real skin occupies rather than a swatch book's.
SKINS = [(92, 66, 55), (134, 98, 82), (184, 138, 116), (216, 170, 146), (238, 202, 180)]
# Below this the pixel has no hue worth measuring.
MIN_SAT = 0.10


def make_skin_chart(path):
    im = Image.new("RGB", (PATCH * len(SKINS), PATCH), (0, 0, 0))
    px = im.load()

    for i, rgb in enumerate(SKINS):
        for y in range(PATCH):
            for x in range(PATCH):
                px[i * PATCH + x, y] = rgb

    im.save(path)


def read(px):
    out = []

    for i in range(len(SKINS)):
        r, g, b = px[i * PATCH + PATCH // 2, PATCH // 2]
        h, _, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
        out.append((h * 360.0, s))

    return out


def drift(graded, plain):
    """Largest hue move, over the patches that still have a hue."""
    worst = 0.0

    for (hg, sg), (hp, sp) in zip(graded, plain):
        if min(sg, sp) < MIN_SAT:
            continue

        d = hg - hp

        while d > 180.0:
            d -= 360.0

        while d < -180.0:
            d += 360.0

        if abs(d) > abs(worst):
            worst = d

    return worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", action="store_true")
    args = ap.parse_args()

    chart = os.path.join(HERE, "fs_chart.png")
    make_skin_chart(chart)

    # Measured against the picture with no film on it at all. That is the
    # right baseline because it is where skin protection pulls toward: it
    # blends the graded pixel back toward its SOURCE chroma, so turning the
    # control up has to move skin back toward the original hue. Comparing
    # against a flat film render instead made the control look as though it
    # were making things worse, when it was moving somewhere else entirely.

    def guarded(level):
        return fp.PP3.replace("Halation=0", "Halation=0\nSkinProtection=%d" % level)

    print("  how far the zone grading moves skin, in degrees of hue")
    print("  %-18s %9s %9s %9s   %s" % ("stock", "guard 0", "guard 35", "guard 100", "held?"))

    ok = True
    skipped = []

    plain = read(fp.render("fs_off", fp.PP3.format(on="false", preset="arctic"), chart))

    for preset in fp.PRESETS:
        # A monochrome stock has no hue to hold, and measuring one returns
        # whatever rounding says: Silver Gelatin duly reported 177 degrees.
        probe = read(fp.render("fs_%s_35" % preset,
                               guarded(35).format(on="true", preset=preset), chart))

        if max(s for _, s in probe) < MIN_SAT:
            skipped.append(preset)
            continue

        shifts = []

        for level in (0, 35, 100):
            shot = read(fp.render("fs_%s_%d" % (preset, level),
                                  guarded(level).format(on="true", preset=preset), chart))
            shifts.append(drift(shot, plain))

        # Only a stock that actually pushes skin has anything to hold back, and
        # the test is that the control pulls it back -- not that the look is
        # colourless. A film simulation is allowed to land on a face.
        pushes = abs(shifts[0]) > 3.0
        held = (not pushes) or abs(shifts[2]) < abs(shifts[0]) - 0.5
        verdict = "-" if not pushes else ("yes" if held else "NO")

        if not held:
            ok = False

        print("  %-18s %+9.1f %+9.1f %+9.1f   %s"
              % (preset, shifts[0], shifts[1], shifts[2], verdict))

    if skipped:
        print("\n  skipped, no hue to measure: %s" % ", ".join(skipped))

    if args.gate:
        print("\nALL PASS" if ok else "\nFAILURES PRESENT")
        sys.exit(0 if ok else 1)


main()
