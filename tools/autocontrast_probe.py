#!/usr/bin/env python3
"""How much contrast is Auto Edit actually handing a frame, from both stages?

Two things steepen the same midtones and neither knows about the other: the
metered Contrast slider, and the depth of the master S-curve. Reading either on
its own says nothing -- a frame with a modest slider and a steep curve is a
contrasty frame. This adds them:

    combined = contrast / 45 + (midtone chord slope - 1)

and reports the distribution, so an over-contrasted frame can be recognised as
an outlier rather than argued about. DSCF0266 was reported by eye and measured
first of sixty on exactly this number.

    python tools/autocontrast_probe.py <log> [<log2>]
    python tools/autocontrast_probe.py --gate <log>

Traces come from the headless self-test -- see tools/autoedit_probe.py for the
invocation. Note the frame paths contain spaces, so anything parsing this log
has to anchor on the following key rather than on whitespace.
"""
import argparse
import re
import statistics
import sys

FINAL = re.compile(r"\[autoSelfTest\] (.+?) scene=(\w+) expcomp=([-\d.]+) bright=([-\d]+) contrast=(\d+)")
SLOPE = re.compile(r"chord slopes: black->toe=[\d.]+ toe->pivot=([\d.]+)")
RANGE = re.compile(r"range=([\d.]+)\n")


def read(path):
    text = open(path, errors="replace").read()
    final = {}

    for m in FINAL.finditer(text):
        final[m.group(1).split("\\")[-1]] = (m.group(2), int(m.group(5)))

    curves = {}

    for block in text.split("[autoCurve] ==== ")[1:]:
        name = block.split("\n")[0].replace(" ====", "").split("\\")[-1]
        slope = SLOPE.search(block)
        rng = RANGE.search(block)

        if slope:
            curves[name] = (float(slope.group(1)),
                            float(rng.group(1)) if rng else 0.0)

    rows = []

    for name, (scene, contrast) in final.items():
        if name in curves:
            slope, rng = curves[name]
            rows.append({"name": name, "scene": scene, "contrast": contrast,
                         "slope": slope, "range": rng,
                         "combined": contrast / 45.0 + (slope - 1.0)})

    return sorted(rows, key=lambda r: -r["combined"])


def report(path):
    rows = read(path)

    if not rows:
        sys.exit("no frames parsed from " + path)

    combined = [r["combined"] for r in rows]
    med = statistics.median(combined)

    print("  %s -- %d frames" % (path, len(rows)))
    print("  %-16s %-10s %8s %7s %7s %9s" %
          ("frame", "scene", "contrast", "slope", "range", "combined"))

    for r in rows[:6]:
        print("  %-16s %-10s %8d %7.3f %7.3f %9.3f"
              % (r["name"], r["scene"], r["contrast"], r["slope"], r["range"], r["combined"]))

    print("  median %.3f, top %.3f, top/median %.2f"
          % (med, combined[0], combined[0] / max(med, 1e-6)))
    return rows, med


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--gate", action="store_true")
    args = ap.parse_args()

    rows, med = report(args.logs[0])

    for extra in args.logs[1:]:
        print()
        report(extra)

    if args.gate:
        ok = True
        top = rows[0]["combined"]

        # An absolute distance from the median, not a ratio. This was never a
        # population effect -- the flattest quarter of the set measures the
        # same as the whole of it -- so there is no trend to catch. What goes
        # wrong is that one frame collects the same flatness answer from both
        # stages and lands outside the pack, which is what DSCF0266 did at
        # 1.203 against a median of 0.668.
        if top > med + 0.5:
            print("\nFAIL  %s at %.3f sits %.3f above the median of %.3f"
                  % (rows[0]["name"], top, top - med, med))
            ok = False

        print("\nALL PASS" if ok else "\nFAILURES PRESENT")
        sys.exit(0 if ok else 1)


main()
