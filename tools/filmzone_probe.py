#!/usr/bin/env python3
"""Does each film stock put a different colour in the shadows than the highlights?

Real film does, and not as a style: its three emulsions have their own
characteristic curves, so the ratio between the three densities changes with
exposure. The layers cross over. A model that applies one colour balance to the
whole frame cannot do that -- its cast can only scale with signal, which shows
up here as a number that decays from shadows to highlights without ever
changing sign.

    python tools/filmzone_probe.py              # the table
    python tools/filmzone_probe.py --gate       # table plus pass/fail
    python tools/filmzone_probe.py --save before.json
    python tools/filmzone_probe.py --compare before.json

Read at five points up a neutral ramp, per channel, as a percentage of each
patch's own level -- a cast of two counts on a dark patch is a bigger colour
error than twenty on a bright one, and raw counts would credit every stock with
a highlight cast it never chose.

  r-b   warm against cool. Positive is warm.
  g-b   the green/magenta axis, which is where an aged or expired stock lives.

  swing largest r-b minus smallest across the five zones: how much the cast
        changes with tone at all.
  flip  the cast is warm at one end of the ramp and cool at the other. This is
        the thing the model could not do before crossover existed, and the
        reason this probe exists.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import filmpreset_probe as fp  # noqa: E402

# Up the ramp: deep shadow, shadow, midtone, upper midtone, highlight.
ZONES = [("deep", "grey24"), ("shadow", "grey48"), ("mid", "grey104"),
         ("upper", "grey168"), ("high", "grey232")]

# Below this a cast is not a decision, it is rounding on an 8-bit patch.
NOISE = 1.5


def cast(base, shot, key):
    """Red-minus-blue and green-minus-blue, in percent of the patch's level."""
    level = max(8.0, sum(shot[key]) / 3.0)
    rb = 100.0 * ((shot[key][0] - shot[key][2]) - (base[key][0] - base[key][2])) / level
    gb = 100.0 * ((shot[key][1] - shot[key][2]) - (base[key][1] - base[key][2])) / level
    return rb, gb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", action="store_true")
    ap.add_argument("--save")
    ap.add_argument("--compare")
    args = ap.parse_args()

    chart = os.path.join(HERE, "fz_chart.png")
    fp.make_chart(chart)
    base = fp.sample(fp.render("fz_off", fp.PP3.format(on="false", preset="arctic"), chart))

    # The Python tone probe mirrors the scalar table only; the per-layer toe
    # offsets live in C++ and it cannot see them at all. A toe offset lifts
    # black in one channel, so that check has to happen here, on a real render.
    black = {}

    rows = {}
    header = "  %-18s" % "stock" + "".join("%16s" % ("%s r-b/g-b" % z[0]) for z in ZONES)
    print(header + "%8s%7s" % ("swing", "flip"))

    for preset in fp.PRESETS:
        shot = fp.sample(fp.render("fz_" + preset,
                                   fp.PP3.format(on="true", preset=preset), chart))
        zones = {name: cast(base, shot, key) for name, key in ZONES}
        rb = [zones[name][0] for name, _ in ZONES]

        # A flip needs both ends to be saying something, not just to differ in
        # the sign of their own noise.
        flip = (max(rb) > NOISE and min(rb) < -NOISE)
        rows[preset] = {"zones": zones, "swing": max(rb) - min(rb), "flip": flip}

        black[preset] = (sum(shot["grey8"]) - sum(base["grey8"])) / 3.0
        cells = "".join("%+7.1f /%+7.1f" % zones[name] for name, _ in ZONES)
        print("  %-18s%s%8.1f%7s" % (preset, cells, rows[preset]["swing"],
                                     "yes" if flip else "-"))

    flips = sum(1 for r in rows.values() if r["flip"])
    swung = sum(1 for r in rows.values() if r["swing"] > 8.0)
    print("\n  %d of %d stocks change the sign of their cast across the ramp"
          % (flips, len(rows)))
    print("  %d of %d swing more than 8%% between their extreme zones" % (swung, len(rows)))

    if args.save:
        with open(args.save, "w") as f:
            json.dump({k: {"swing": v["swing"], "flip": v["flip"],
                           "zones": {z: list(c) for z, c in v["zones"].items()}}
                       for k, v in rows.items()}, f, indent=1)
        print("\n  saved to " + args.save)

    if args.compare:
        with open(args.compare) as f:
            was = json.load(f)

        print("\n  change against %s" % args.compare)
        print("  %-18s %8s %8s   %s" % ("stock", "swing", "was", "flip"))

        for preset in fp.PRESETS:
            if preset not in was:
                continue

            print("  %-18s %+8.1f %8.1f   %s -> %s"
                  % (preset, rows[preset]["swing"] - was[preset]["swing"],
                     was[preset]["swing"],
                     "yes" if was[preset]["flip"] else "-",
                     "yes" if rows[preset]["flip"] else "-"))

    if args.gate:
        ok = True

        # The point of the whole exercise: enough of the set has to grade by
        # tone rather than applying one colour to the picture.
        if flips < 4:
            print("\nFAIL  only %d stocks change the sign of their cast; want 4" % flips)
            ok = False

        if swung < 8:
            print("FAIL  only %d stocks swing more than 8%%; want 8" % swung)
            ok = False

        # And a stock has to swing smoothly. Crossover is three curves that are
        # not parallel, which reads as a cast changing steadily up the ramp; a
        # number that is large in the deepest patch and ordinary in every other
        # one is a toe offset showing its seam, and looks like a crushed shadow
        # rather than like film.
        for preset, r in rows.items():
            deep = abs(r["zones"]["deep"][0])
            rest = max(abs(r["zones"][z][0]) for z in ("shadow", "mid", "upper", "high"))

            if deep > 8.0 and deep > 2.5 * max(rest, 0.5):
                print("FAIL  %s casts %.1f in the deepest patch against %.1f "
                      "anywhere else: a step, not a gradient" % (preset, deep, rest))
                ok = False

        # Black lift, measured rather than modelled, because the toe offsets
        # that produce shadow crossover are exactly the dial that spends it.
        for preset, lift in sorted(black.items(), key=lambda kv: -kv[1]):
            if lift > 6.0:
                print("FAIL  %s lifts the darkest patch by %.1f counts; want 6"
                      % (preset, lift))
                ok = False

        worst = max(black.items(), key=lambda kv: kv[1])
        print("  worst black lift: %s at %+.1f counts" % (worst[0], worst[1]))

        print("\nALL PASS" if ok else "\nFAILURES PRESENT")
        sys.exit(0 if ok else 1)


main()
