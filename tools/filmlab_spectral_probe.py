#!/usr/bin/env python3
"""Film Lab V6 spectral response and black-and-white filters, through steep-cli.

    python tools/filmlab_spectral_probe.py --cli <steep-cli.exe>

Renders a chart of grey, pale and saturated patches.

Colour stock, spectral response on against off (STEEP_FILMLAB_NO_SPECTRAL=1):
  * greys do not move, pale colours barely: the spectral response only adds
    what the matrix cannot say, and that grows with saturation;
  * saturated colours do move, within bounds (the correction is limited to
    about half a stop per layer).

Black-and-white stock, each lens filter against none:
  * grey stays where it was (the exposure is compensated);
  * yellow, orange, red darken blue progressively and lighten red;
  * green lightens green and darkens red and blue;
  * blue lightens blue and darkens red;
  * film that cannot see red ("ortho") darkens red.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halation_probe as hp  # noqa: E402

PATCHES = [
    ("grey dark", (60, 60, 60)), ("grey mid", (120, 120, 120)), ("grey light", (200, 200, 200)),
    ("skin", (200, 150, 125)), ("pale sky", (150, 180, 210)), ("pale leaf", (140, 160, 120)),
    ("red", (200, 30, 30)), ("green", (40, 170, 60)), ("blue", (30, 50, 200)), ("yellow", (230, 210, 30)),
    ("cyan", (20, 190, 200)), ("magenta", (200, 40, 180)), ("orange", (230, 120, 20)), ("deep sky", (40, 100, 200)),
]
SIZE = 160


def chart(path):
    img = Image.new("RGB", (SIZE * len(PATCHES), SIZE * 2), (90, 90, 90))
    for i, (_, rgb) in enumerate(PATCHES):
        img.paste(rgb, (i * SIZE, 0, (i + 1) * SIZE, SIZE * 2))
    img.save(path)


def render(cli, out, name, src, spectral=True, **film):
    s = dict(Enabled="true", ModelVersion=6, Preset="sovereign", Strength=100, Halation=-100, Bloom=-100,
             OutputSoftness=0, Format=35, LightSources=-100, FilmSharpness=100)
    s.update(film)
    pp3 = out / (name + ".pp3")
    pp3.write_text("[Film Presets]\n" + "".join(f"{k}={v}\n" for k, v in s.items()), encoding="ascii")
    tif = out / (name + ".tif")
    env = dict(os.environ, OMP_NUM_THREADS="4")
    env["RT_SETTINGS"] = str((out / "settings").resolve())
    env["RT_CACHE"] = str((out / "cache").resolve())
    env.pop("STEEP_FILMLAB_NO_SPECTRAL", None)
    if not spectral:
        env["STEEP_FILMLAB_NO_SPECTRAL"] = "1"
    r = subprocess.run([str(Path(cli).resolve()), "-q", "-Y", "-o", str(tif.resolve()), "-d", "-p", str(pp3.resolve()),
                        "-t", "-b16", "-c", str(Path(src).resolve())], capture_output=True, text=True, timeout=600, env=env)
    if r.returncode != 0 or not tif.exists():
        raise RuntimeError(f"{name}: {r.stdout[-600:]}\n{r.stderr[-600:]}")
    img = hp.read_tiff16(tif) * 255.0
    m = SIZE // 4
    return np.array([img[m:2 * SIZE - m, i * SIZE + m:(i + 1) * SIZE - m].mean(axis=(0, 1)) for i in range(len(PATCHES))])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--output", type=Path, default=Path("test-output/filmlab-spectral-probe"))
    args = ap.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    src = out / "chart.png"
    chart(src)
    names = [n for n, _ in PATCHES]
    at = names.index

    failures = 0

    def gate(ok, what, detail=""):
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + what + (f"  ({detail})" if detail else ""))
        failures += not ok

    for preset in ("sovereign", "vivid_chrome", "cinema_reveal_35"):
        on = render(args.cli, out, f"{preset}_spectral", src, Preset=preset)
        off = render(args.cli, out, f"{preset}_matrix", src, spectral=False, Preset=preset)
        move = np.abs(on - off).max(axis=1)
        print(f"{preset}: output levels (8-bit), matrix only -> spectral")
        for i, n in enumerate(names):
            print(f"    {n:10s} {off[i][0]:6.1f} {off[i][1]:6.1f} {off[i][2]:6.1f}  ->  {on[i][0]:6.1f} {on[i][1]:6.1f} {on[i][2]:6.1f}   moved {move[i]:5.2f}")
        gate(move[:3].max() < 0.3, f"{preset}: greys do not move", f"max {move[:3].max():.2f} levels")
        gate(move[3:6].max() < 3.0, f"{preset}: pale colours barely move", f"max {move[3:6].max():.2f} levels")
        gate(move[6:].max() > 1.0, f"{preset}: saturated colours do move", f"max {move[6:].max():.2f} levels")
        # a reversal stock's steep curve turns the same half stop into a larger step
        bound = 90.0 if preset == "vivid_chrome" else 40.0
        gate(move[6:].max() < bound, f"{preset}: and stay within bounds", f"max {move[6:].max():.2f} of {bound:.0f} levels")

    mono = {f: render(args.cli, out, f"mono_{f}", src, Preset="silver_gelatin", Process="bw", BWFilter=f)[:, 1]
            for f in ("none", "yellow", "orange", "red", "green", "blue", "ortho")}
    print("black and white, output level per patch:")
    print("    " + " ".join(f"{n[:8]:>8s}" for n in names))
    for f, v in mono.items():
        print(f"{f:7s}" + " ".join(f"{x:8.1f}" for x in v))
    none = mono["none"]
    for f, v in mono.items():
        if f != "none":
            gate(np.abs(v[:3] - none[:3]).max() < 2.0, f"{f} filter: greys stay where they were",
                 f"max {np.abs(v[:3] - none[:3]).max():.2f} levels")
    b, r, g, sky = at("blue"), at("red"), at("green"), at("deep sky")
    gate(none[b] > mono["yellow"][b] > mono["orange"][b] > mono["red"][b], "yellow, orange, red darken blue progressively",
         f"{none[b]:.0f} > {mono['yellow'][b]:.0f} > {mono['orange'][b]:.0f} > {mono['red'][b]:.0f}")
    gate(none[sky] - mono["red"][sky] > 15, "a red filter darkens a deep blue sky clearly", f"{none[sky]:.0f} -> {mono['red'][sky]:.0f}")
    gate(mono["red"][r] > none[r] + 10, "a red filter lightens red", f"{none[r]:.0f} -> {mono['red'][r]:.0f}")
    gate(mono["green"][g] > none[g] + 3 and mono["green"][r] < none[r] - 5, "a green filter lightens green and darkens red",
         f"green {none[g]:.0f} -> {mono['green'][g]:.0f}, red {none[r]:.0f} -> {mono['green'][r]:.0f}")
    gate(mono["blue"][b] > none[b] + 10 and mono["blue"][r] < none[r] - 5, "a blue filter lightens blue and darkens red",
         f"blue {none[b]:.0f} -> {mono['blue'][b]:.0f}, red {none[r]:.0f} -> {mono['blue'][r]:.0f}")
    gate(mono["ortho"][r] < none[r] - 10, "film that cannot see red darkens red", f"{none[r]:.0f} -> {mono['ortho'][r]:.0f}")

    print(("\n%d FAILED" % failures) if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
