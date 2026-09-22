#!/usr/bin/env python3
"""Render smooth neutral/colored highlights through the actual Film Lab engine."""
import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image
import tifffile


def encode(linear):
    return np.where(linear <= 0.0031308, linear * 12.92,
                    1.055 * np.maximum(linear, 0) ** (1 / 2.4) - 0.055)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("test-output/filmlab-highlight-colors"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    ramp = np.linspace(0.002, 1, 1536)
    colors = np.array(((1, 1, 1), (1, .83, .58), (1, .62, .27), (1, .07, .02), (.02, .12, 1)))
    pixels = np.repeat(ramp[None, :, None] * colors[:, None, :], 64, axis=0)
    source = args.output / "ramp.tif"
    tifffile.imwrite(source, np.round(encode(pixels) * 65535).astype(np.uint16), photometric="rgb")
    yy, xx = np.mgrid[-1:1:768j, -1:1:768j]
    light = .002 + .998 * np.exp(-(xx * xx + yy * yy) / .12)
    bokeh = args.output / "bokeh.tif"
    tifffile.imwrite(bokeh, np.round(encode(light[:, :, None] * colors[1]) * 65535).astype(np.uint16), photometric="rgb")

    def render(name, input_path=source, working_profile="ProPhoto", clamp_oog="true", **settings):
        profile = dict(Enabled="true", Preset="golden_hour", ModelVersion=5,
                       Process="ecn2", Output="cinema", Strength=100,
                       Halation=-100, Bloom=-100, OutputSoftness=0)
        profile.update(settings)
        pp3 = args.output / (name + ".pp3")
        pp3.write_text(f"[Exposure]\nCompensation=3\nClampOOG={clamp_oog}\n\n"
                       f"[Color Management]\nWorkingProfile={working_profile}\n\n[Film Presets]\n" +
                       "".join(f"{k}={v}\n" for k, v in profile.items()), encoding="ascii")
        output = args.output / (name + ".tif")
        env = dict(os.environ, OMP_NUM_THREADS="4", RT_SETTINGS=str((args.output / "settings").resolve()),
                   RT_CACHE=str((args.output / "cache").resolve()))
        result = subprocess.run([str(args.cli.resolve()), "-Y", "-o", str(output.resolve()),
                                 "-d", "-p", str(pp3.resolve()), "-t", "-b16", "-c", str(input_path.resolve())],
                                capture_output=True, text=True, timeout=180, env=env)
        if result.returncode or not output.exists():
            raise RuntimeError(result.stdout[-1000:] + result.stderr[-1000:])
        values = tifffile.imread(output).astype(np.float64) / 65535
        Image.fromarray(np.round(values * 255).astype(np.uint8)).save(args.output / (name + ".png"))
        return values

    results = {}
    checks = {}
    for preset, process, output in (("golden_hour", "ecn2", "cinema"), ("porcelain_400", "c41", "ra4")):
        values = render(preset, Preset=preset, Process=process, Output=output)
        for i, color in enumerate(("neutral", "warm", "tungsten", "red", "blue")):
            samples = values[i * 64 + 32]
            results[preset + "_" + color] = dict(
                samples=[samples[x].tolist() for x in (100, 200, 400, 800, 1200, 1500)],
                max_step=float(np.abs(np.diff(samples[200:], axis=0)).max()),
                min_green_minus_blue=float((samples[200:, 1] - samples[200:, 2]).min()))
            if i < 3:
                green = samples[180:, 1]
                # Increasing light must not cause a visible green dip/pink ring.
                dip = float((np.maximum.accumulate(green) - green).max())
                results[preset + "_" + color]["green_dip"] = dip
                checks[preset + "_" + color + "_no_ring"] = dip <= 1 / 255
                checks[preset + "_" + color + "_approaches_white"] = float(np.ptp(samples[-1])) < .08
            elif i == 3:
                checks[preset + "_red_keeps_color"] = bool(samples[200, 0] - samples[200, 2] > .25)
            else:
                checks[preset + "_blue_keeps_color"] = bool(samples[200, 2] - samples[200, 0] > .25)
    for working in ("sRGB", "ACESp1"):
        values = render("working_" + working, working_profile=working)
        green = values[96, 180:, 1]
        checks[working + "_no_ring"] = float((np.maximum.accumulate(green) - green).max()) <= 1 / 255
    unclamped = render("unclamped", clamp_oog="false")
    checks["unclamped_finite_nonblank"] = bool(np.isfinite(unclamped).all() and unclamped.std() > .04)
    for name, halo in (("bokeh_no_halo", -100), ("bokeh_halo", 30)):
        values = render(name, bokeh, Halation=halo, Bloom=-100 if halo < 0 else 0)
        green = values[384, :384, 1]
        checks[name + "_no_ring"] = float((np.maximum.accumulate(green) - green).max()) <= 1 / 255
    render("bokeh_no_film", bokeh, Enabled="false")
    results["checks"] = checks
    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="ascii")
    print(json.dumps(checks, indent=2))
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise SystemExit("FAIL: " + ", ".join(failed))
    print("PASS: Film Lab highlight color continuity")


if __name__ == "__main__":
    main()
