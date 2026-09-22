#!/usr/bin/env python3
"""Engine-backed Film Lab regression checks; no duplicated rendering formulas.

Run with --cli /path/to/steep-cli. Generated images/profiles stay in --output.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image, ImageDraw
import tifffile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=os.environ.get("STEEP_CLI"), required=not os.environ.get("STEEP_CLI"))
    parser.add_argument("--output", type=Path, default=Path("test-output/filmlab-v5"))
    parser.add_argument("--baseline", type=Path, help="Optional renders from the previous build")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (960, 640), (35, 35, 35))
    draw = ImageDraw.Draw(image)
    for i, color in enumerate(((255, 255, 255), (255, 40, 20), (15, 80, 255), (180, 140, 115))):
        draw.rectangle((90 + i * 210, 110, 150 + i * 210, 220), fill=color)
    for x in range(960):
        v = round(255 * x / 959)
        draw.line((x, 410, x, 639), fill=(v, v, v))
    for i, value in enumerate((160, 185, 210, 235, 255)):
        draw.rectangle((45 + i * 180, 285, 155 + i * 180, 360), fill=(value, value, value))
    source = args.output / "scene.png"
    image.save(source)
    times = {}

    def render(name, bit_depth=8, **settings):
        profile = {"Enabled": "true", "Preset": "sovereign", "ModelVersion": 5,
                   "Strength": 100, "Halation": -100, "Bloom": -100,
                   "OutputSoftness": 0}
        profile.update(settings)
        pp3 = args.output / (name + ".pp3")
        pp3.write_text("[Exposure]\nCompensation=2\n\n[Film Presets]\n" +
                       "".join(f"{k}={v}\n" for k, v in profile.items()), encoding="ascii")
        output = args.output / (name + ".tif")
        start = time.perf_counter()
        env = dict(os.environ, OMP_NUM_THREADS="4")
        env["RT_SETTINGS"] = str((args.output / "settings").resolve())
        env["RT_CACHE"] = str((args.output / "cache").resolve())
        result = subprocess.run([str(Path(args.cli).resolve()), "-Y", "-o", str(output.resolve()),
                                 "-d", "-p", str(pp3.resolve()), "-t", f"-b{bit_depth}", "-c", str(source.resolve())],
                                capture_output=True, text=True, timeout=180, env=env)
        times[name] = round(time.perf_counter() - start, 3)
        if result.returncode != 0 or not output.exists():
            raise RuntimeError(f"{name}: {result.stdout[-1500:]}\n{result.stderr[-1500:]}")
        return tifffile.imread(output).astype(np.float64) / (2 ** bit_depth - 1)

    results = {}
    disabled = render("disabled", Enabled="false")
    zero = render("zero", Strength=0)
    results["zero_is_bypass"] = bool(np.array_equal(disabled, zero))
    legacy = render("v4", ModelVersion=4)
    legacy_print = render("v4_print", ModelVersion=4, PrintExposure=1)
    results["legacy_ignores_new_print_control"] = bool(np.array_equal(legacy, legacy_print))
    full = render("v5")
    repeat = render("repeat")
    results["deterministic"] = bool(np.array_equal(full, repeat))
    half = render("half", Strength=50)
    # Export's sRGB transfer is decoded solely to compare the blend contract.
    def linear(v):
        return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)
    blend_error = np.abs(linear(half) - (linear(disabled) + linear(full)) * 0.5)
    # Exclude clipped source/highlights and edge effects from this color check.
    samples = (disabled.max(axis=2) < 0.94) & (full.max(axis=2) < 0.94)
    results["mix_mean_linear_error"] = float(blend_error[samples].mean())
    results["predictable_mix"] = results["mix_mean_linear_error"] < 0.015
    brighter = render("print_brighter", PrintExposure=0.4)
    results["print_exposure_works"] = bool(brighter[450:620, 180:400].mean() > full[450:620, 180:400].mean() + 0.015)
    ladder = [float(full[300:345, 60 + i * 180:140 + i * 180].mean()) for i in range(5)]
    results["highlight_ladder"] = ladder
    precise = render("v5_precise", bit_depth=16)
    precise_ladder = [float(precise[300:345, 60 + i * 180:140 + i * 180].mean()) for i in range(5)]
    results["highlight_ladder_16bit"] = precise_ladder
    # Diagnose tonal collapse at export precision, not an 8-bit rounding boundary.
    results["highlight_separation"] = all(b - a > 2 / 65535 for a, b in zip(precise_ladder, precise_ladder[1:]))
    results["display_highlight_range"] = ladder[-1] - ladder[0] > 4 / 255
    results["highlight_channels_not_clipped"] = bool(full[300:345, 780:860].max() < 1.0)
    results["print_lift_highlights_not_clipped"] = bool(brighter[300:345, 780:860].max() < 1.0)
    results["half_mix_highlights_not_clipped"] = bool(half[300:345, 780:860].max() < 1.0)
    # The warm patch clips its red channel before luminance reaches white.
    results["warm_highlight_not_clipped"] = bool(full[125:205, 725:765].max() < 1.0)
    for stock, output in (("twilight_160", "scan"), ("cinema_reveal_35", "cinema"),
                          ("sovereign", "ra4"), ("silver_gelatin", "scan")):
        name = stock + "_" + output
        candidate = render(name, Preset=stock, Output=output, PrintExposure=0.4)
        results[name + "_highlights_not_clipped"] = bool(candidate[300:345, 60:860].max() < 1.0)
    custom_v4 = render("custom_v4", Preset="custom", ModelVersion=4)
    custom_v5 = render("custom_v5", Preset="custom")
    # Custom keeps its straight neutral response; V5's existing gamut mapping
    # deliberately differs on saturated colors, independently of this shoulder.
    results["custom_tone_response_preserved"] = bool(
        np.max(np.abs(custom_v5[450:620] - custom_v4[450:620])) <= 1 / 255)
    golden = render("golden_hour", Preset="golden_hour", Process="ecn2", Output="cinema",
                    Strength=82, Exposure=0.1, Contrast=7, Saturation=-3, Fade=5, Rolloff=4,
                    SkinProtection=77, LayerCoupling=16, Halation=8, HalationSize=8,
                    HalationThreshold=12, Bloom=1, OutputSoftness=2)
    results["golden_hour_highlights_not_clipped"] = bool(golden[300:345, 780:860].max() < 1.0)
    if args.baseline:
        previous = np.asarray(Image.open(args.baseline / "v5.tif").convert("RGB"), dtype=np.float64) / 255
        midtones = previous.max(axis=2) < 210 / 255
        results["shadow_mid_max_delta_codes"] = float(np.abs(full - previous)[midtones].max() * 255)
        results["shadows_midtones_preserved"] = results["shadow_mid_max_delta_codes"] <= 1
        previous_v4 = np.asarray(Image.open(args.baseline / "v4.tif").convert("RGB"), dtype=np.float64) / 255
        results["legacy_render_preserved"] = bool(np.array_equal(legacy, previous_v4))
    halo = render("halation", Halation=60)
    halo_delta = halo - full
    results["halation_near_lift"] = float(halo_delta[125:205, 151:171, 0].mean())
    results["halation_far_lift"] = float(halo_delta[10:50, 10:50].mean())
    results["local_halation"] = (results["halation_near_lift"] > 0.001 and
                                  results["halation_far_lift"] < 0.01)
    results["nonblank_finite"] = bool(np.isfinite(full).all() and full.std() > 0.04)
    results["elapsed_seconds_including_cli_startup"] = times
    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="ascii")
    print(json.dumps(results, indent=2))
    failed = [k for k, v in results.items() if isinstance(v, bool) and not v]
    if failed:
        raise SystemExit("FAIL: " + ", ".join(failed))
    print("PASS: Film Lab versioning, mix, optics, and print placement")


if __name__ == "__main__":
    main()
