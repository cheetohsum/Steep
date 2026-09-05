#!/usr/bin/env python3
"""Does Auto Edit's split toning vary with the scene, or only in strength?

Auto Edit has graded shadows, midtones and highlights separately since it was
written, and a crossover in the RGB curves on top of that. What it did not do
was point them anywhere different: measured over 60 frames from 15 folders, the
shadow hue landed between 213 and 220 degrees on every one and the highlight
between 35 and 43. Seven and eight degrees of variation. One look at six
intensities.

Reads a STEEP_FILESEL_LOG trace and reports what the split actually did:

    python tools/autosplit_probe.py ~/steep-fileSel.log
    python tools/autosplit_probe.py before.log after.log   # compare
    python tools/autosplit_probe.py --gate ~/steep-fileSel.log

To produce a trace, see the headless self-test: STEEP_AUTOEDIT_SELFTEST with a
';'-separated frame list, STEEP_AUTOEDIT_SELFTEST_QUIT=1, launched with
Start-Process -PassThru and waited on (steep is a GUI-subsystem binary, so a
plain background call returns before it has written anything).
"""
import argparse
import collections
import re
import sys

WHEELS = re.compile(
    r"\[autoGrade\]\s+wheels sh=([\d.-]+)/([\d.-]+) mid=([\d.-]+)/([\d.-]+) hi=([\d.-]+)/([\d.-]+)")
SPLITS = re.compile(r"splitDepth=([\d.-]+)(?:\s+shSplit=([\d.-]+)\s+hiSplit=([\d.-]+))?")


def read(path):
    text = open(path, errors="replace").read()
    wheels = [tuple(float(v) for v in m) for m in WHEELS.findall(text)]
    splits = [tuple(float(v) if v else None for v in m) for m in SPLITS.findall(text)]
    return wheels, splits


def spread(values):
    return max(values) - min(values) if values else 0.0


def report(path, label):
    wheels, splits = read(path)

    if not wheels:
        sys.exit("no [autoGrade] wheel lines in " + path)

    print("  %s -- %d frames" % (label, len(wheels)))

    stats = {}

    for name, hue_i, sat_i in (("shadow", 0, 1), ("midtone", 2, 3), ("highlight", 4, 5)):
        hues = [w[hue_i] for w in wheels]
        sats = [w[sat_i] for w in wheels]
        stats[name] = spread(hues)
        print("    %-9s hue %6.1f..%-6.1f  spread %6.1f    sat %.3f..%.3f"
              % (name, min(hues), max(hues), spread(hues), min(sats), max(sats)))

    depths = [s[0] for s in splits if s[0] is not None]

    if depths:
        print("    %-9s %6.2f..%-6.2f  spread %6.2f" % ("depth", min(depths), max(depths), spread(depths)))

    ends = [(s[1], s[2]) for s in splits if s[1] is not None]

    if ends:
        sh = [e[0] for e in ends]
        hi = [e[1] for e in ends]
        print("    %-9s %6.2f..%-6.2f  spread %6.2f" % ("shSplit", min(sh), max(sh), spread(sh)))
        print("    %-9s %6.2f..%-6.2f  spread %6.2f" % ("hiSplit", min(hi), max(hi), spread(hi)))

    # How many genuinely different grades came out, rather than how many the
    # recipe table contains.
    sigs = collections.Counter((round(w[0] / 5.0), round(w[4] / 5.0)) for w in wheels)
    stats["distinct"] = len(sigs)
    print("    distinct shadow/highlight directions: %d" % len(sigs))
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--gate", action="store_true")
    args = ap.parse_args()

    stats = report(args.logs[0], args.logs[0])

    for extra in args.logs[1:]:
        print()
        report(extra, extra)

    if args.gate:
        ok = True

        # A split that points the same way at every photograph is a look, not a
        # grader. Twenty degrees is about where two shadow tints stop reading
        # as the same decision.
        for zone in ("shadow", "highlight"):
            if stats[zone] < 20.0:
                print("\nFAIL  %s hue varies by only %.1f degrees across the set; want 20"
                      % (zone, stats[zone]))
                ok = False

        if stats["distinct"] < 4:
            print("FAIL  only %d distinct split directions over the set; want 4"
                  % stats["distinct"])
            ok = False

        print("\nALL PASS" if ok else "\nFAILURES PRESENT")
        sys.exit(0 if ok else 1)


main()
