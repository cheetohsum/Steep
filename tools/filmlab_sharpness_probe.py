#!/usr/bin/env python3
"""Film Lab V6 emulsion sharpness, measured through steep-cli.

    python tools/filmlab_sharpness_probe.py --cli <steep-cli.exe>

Renders a chart of low-contrast neutral gratings at 10..50 cycles/mm (on 35mm
film) with the film's own sharpness (FilmSharpness=0) and with none lost
(FilmSharpness=100), and takes the ratio of the modulation that survives. Per
output channel that ratio is the layer's transfer: red follows the
red-sensitive layer, and so on.

Gates:
  * every channel loses modulation as frequency rises, and red loses most;
  * on a black-and-white stock, which has one record and no interlayer
    couplers, the measured transfer is the target curve 1 / (1 + (f/f50)^2);
  * on the colour stock green and blue are near their layers' targets, and
    red reads somewhat below its own, as it does on film: the interlayer
    couplers subtract a share of the neighbouring, sharper, layers' image;
  * the same chart on 120 film loses far less: the format is larger.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halation_probe as hp  # noqa: E402
from filmlab_lights_probe import srgb, write_tiff16  # noqa: E402

W, H = 4800, 3200
PX_PER_MM = H / 24.0
FREQS = (10, 20, 30, 40, 50)
BAND = H // len(FREQS)
# cinema_reveal_35: motion negative at ISO 250 -> (50, 68, 75) * 2.5^-0.25
F50 = tuple(v * 2.5 ** -0.25 for v in (50.0, 68.0, 75.0))


def chart(path):
    x = np.arange(W) / PX_PER_MM
    img = np.empty((H, W))
    for i, f in enumerate(FREQS):
        img[i * BAND:(i + 1) * BAND] = 0.18 * (1.0 + 0.12 * np.sin(2 * np.pi * f * x))
    write_tiff16(path, np.repeat((srgb(img) * 65535 + 0.5)[..., None], 3, axis=2))


def render(cli, out, name, src, **film):
    s = dict(Enabled="true", ModelVersion=6, Preset="cinema_reveal_35", Strength=100, Halation=-100, HalationSize=0,
             HalationThreshold=0, Bloom=-100, OutputSoftness=0, Format=35, LightSources=-100)
    s.update(film)
    pp3 = out / (name + ".pp3")
    pp3.write_text("[Film Presets]\n" + "".join(f"{k}={v}\n" for k, v in s.items()), encoding="ascii")
    tif = out / (name + ".tif")
    env = dict(os.environ, OMP_NUM_THREADS="4", STEEP_FILMLAB_LIGHTS_TRACE="1")
    env["RT_SETTINGS"] = str((out / "settings").resolve())
    env["RT_CACHE"] = str((out / "cache").resolve())
    r = subprocess.run([str(Path(cli).resolve()), "-q", "-Y", "-o", str(tif.resolve()), "-d", "-p", str(pp3.resolve()),
                        "-t", "-b16", "-c", str(Path(src).resolve())], capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0 or not tif.exists():
        raise RuntimeError(f"{name}: {r.stdout[-600:]}\n{r.stderr[-600:]}")
    for line in r.stderr.splitlines():
        if "filmlab-sharpness" in line:
            print("   ", name, line.strip())
    return hp.read_tiff16(tif)


def modulation(img, px_per_mm):
    """Amplitude at each band's own frequency, per channel."""
    out = np.zeros((len(FREQS), 3))
    x = np.arange(200, W - 200)
    for i, f in enumerate(FREQS):
        rows = img[i * BAND + 60:(i + 1) * BAND - 60, 200:W - 200].mean(axis=0)
        phase = 2 * np.pi * f * x / px_per_mm
        for c in range(3):
            v = rows[:, c] - rows[:, c].mean()
            out[i, c] = 2 * np.hypot((v * np.sin(phase)).mean(), (v * np.cos(phase)).mean())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--output", type=Path, default=Path("test-output/filmlab-sharpness-probe"))
    args = ap.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    src = out / "chart.tif"
    chart(src)

    failures = 0

    def gate(ok, what, detail=""):
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + what + (f"  ({detail})" if detail else ""))
        failures += not ok

    sharp = modulation(render(args.cli, out, "none_lost", src, FilmSharpness=100), PX_PER_MM)
    film = modulation(render(args.cli, out, "stock", src, FilmSharpness=0), PX_PER_MM)
    ratio = film / np.maximum(sharp, 1e-9)
    target = np.array([[1.0 / (1.0 + (f / f50) ** 2) for f50 in F50] for f in FREQS])
    print("cycles/mm   measured R / G / B        target R / G / B")
    for i, f in enumerate(FREQS):
        print(f"{f:6d}      {ratio[i, 0]:.3f} {ratio[i, 1]:.3f} {ratio[i, 2]:.3f}        "
              f"{target[i, 0]:.3f} {target[i, 1]:.3f} {target[i, 2]:.3f}")
    gate(bool(np.all(np.diff(ratio, axis=0) < 0)), "every channel loses modulation as frequency rises")
    gate(bool(np.all(ratio[1:, 0] < ratio[1:, 1]) and np.all(ratio[1:, 1] < ratio[1:, 2])),
         "red is the softest record, blue the sharpest")
    worst = float(np.abs(ratio[:, 1:] - target[:, 1:]).max())
    gate(worst < 0.03, "green and blue are near their layers' target curves", f"worst difference {worst:.3f}")
    low = target[:, 0] - ratio[:, 0]
    gate(bool(np.all(low > 0) and low.max() < 0.12), "red is a little below its layer's curve (couplers), not above it",
         f"below by {low.min():.3f} to {low.max():.3f}")

    # silver_gelatin: monochrome at ISO 400 -> 95 * 4^-0.25 on every layer
    mono_f50 = 95.0 * 4.0 ** -0.25
    mono_sharp = modulation(render(args.cli, out, "mono_none_lost", src, Preset="silver_gelatin", FilmSharpness=100), PX_PER_MM)
    mono_film = modulation(render(args.cli, out, "mono_stock", src, Preset="silver_gelatin", FilmSharpness=0), PX_PER_MM)
    mono = (mono_film / np.maximum(mono_sharp, 1e-9))[:, 1]
    mono_target = np.array([1.0 / (1.0 + (f / mono_f50) ** 2) for f in FREQS])
    print("black and white: measured " + " ".join(f"{v:.3f}" for v in mono) + "   target " + " ".join(f"{v:.3f}" for v in mono_target))
    worst = float(np.abs(mono - mono_target).max())
    gate(worst < 0.04, "black and white, no couplers: the transfer is the target curve", f"worst difference {worst:.3f}")

    px120 = H / 56.0
    sharp120 = modulation(render(args.cli, out, "none_lost_120", src, FilmSharpness=100, Format=120), PX_PER_MM)
    film120 = modulation(render(args.cli, out, "stock_120", src, FilmSharpness=0, Format=120), PX_PER_MM)
    r120 = film120 / np.maximum(sharp120, 1e-9)
    print(f"the same chart as 120 film ({px120:.0f} px/mm): red keeps " + " ".join(f"{v:.3f}" for v in r120[:, 0]))
    gate(bool(np.all(r120[:, 0] > ratio[:, 0] + 0.02)), "a larger format loses less at the same picture detail")

    print(("\n%d FAILED" % failures) if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
