#!/usr/bin/env python3
"""Validate and render the bundled film recipes through the native CLI.

This is a rendering/recipe regression probe, not physical film calibration.
"""
import argparse
import configparser
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image, ImageDraw
import tifffile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--profiles", type=Path,
                        default=Path(__file__).resolve().parents[1] / "rtdata/profiles/Film Looks")
    parser.add_argument("--output", type=Path, default=Path("test-output/film-looks/render"))
    parser.add_argument("--photo", type=Path, help="Optional real RAW for an additional contact sheet")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        # Report loader failures instead of leaving an unattended modal dialog.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)

    profiles = sorted(args.profiles.glob("*.pp3"))
    ids = set()
    recipes = {}
    for path in profiles:
        recipe = configparser.ConfigParser(interpolation=None)
        recipe.optionxform = str
        recipe.read(path, encoding="utf-8")
        assert set(recipe.sections()) == {"Steep Look", "Film Presets"}, path
        meta, film = recipe["Steep Look"], recipe["Film Presets"]
        assert meta["Version"] == "1" and meta["Kind"] == "film", path
        assert meta["Id"] not in ids, path
        ids.add(meta["Id"])
        assert film["ModelVersion"] == "5", path
        assert float(film["Exposure"]) == 0 and float(film["PrintExposure"]) == 0, path
        assert 0 < float(film["Strength"]) <= 100, path
        recipes[path.stem] = recipe

    # Neutral ramps, a soft warm highlight, skin-like patches and saturated colors.
    width, height = 640, 384
    pixels = np.full((height, width, 3), 0.06, dtype=np.float64)
    pixels[256:, :, :] = np.linspace(0, 1, width)[None, :, None]
    yy, xx = np.mgrid[:256, :width]
    glow = np.exp(-((xx - 510) ** 2 + (yy - 112) ** 2) / (2 * 32 ** 2))
    pixels[:256] += glow[:, :, None] * np.array([0.94, 0.88, 0.72])
    colors = [(0.30, 0.19, 0.14), (0.55, 0.35, 0.25), (0.78, 0.59, 0.46),
              (0.12, 0.42, 0.17), (0.16, 0.29, 0.72), (0.88, 0.12, 0.05)]
    for index, color in enumerate(colors):
        row, col = divmod(index, 3)
        pixels[30 + row * 110:110 + row * 110, 20 + col * 130:120 + col * 130] = color
    source = args.output / "chart.png"
    Image.fromarray(np.rint(np.clip(pixels, 0, 1) * 255).astype(np.uint8)).save(source)
    baseline = args.output / "baseline.pp3"
    baseline.write_text("[Film Presets]\nEnabled=false\n[Sharpening]\nEnabled=false\n"
                        "[White Balance]\nEnabled=false\n", encoding="ascii")
    env = dict(os.environ, OMP_NUM_THREADS="4",
               RT_SETTINGS=str((args.output / "settings").resolve()),
               RT_CACHE=str((args.output / "cache").resolve()))
    times = {}

    def render(name, looks, source_path=source, baseline_profile=baseline):
        output = args.output / (name + ".tif")
        command = [str(args.cli.resolve()), "-Y", "-o", str(output.resolve()),
                   "-p", str(baseline_profile.resolve())]
        for look in looks:
            command += ["-p", str(look.resolve())]
        command += ["-a", "-t", "-b16", "-c", str(source_path.resolve())]
        start = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=120, env=env)
        times[name] = round(time.monotonic() - start, 3)
        (args.output / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode or not output.exists():
            raise RuntimeError(f"{name}: native CLI failed ({result.returncode}); see {name}.log")
        data = tifffile.imread(output)
        assert data.dtype == np.uint16 and data.ndim == 3 and data.shape[2] == 3, name
        if source_path == source:
            assert data.shape == (height, width, 3), name
        return data.astype(np.float64) / 65535

    rendered = {"Neutral": render("neutral", [])}
    results = {}
    for path in profiles:
        name = path.stem
        data = render(name, [path])
        rendered[name] = data
        ramp = data[300:360].mean(axis=(0, 2))
        results[name] = {
            "finite_nonblank": bool(np.isfinite(data).all() and data.std() > 0.04),
            "monotone_neutral_ramp": bool(np.diff(ramp).min() >= -4 / 65535),
            "highlight_range": float(ramp[-10:].mean() - ramp[510:530].mean()),
            "clipped_fraction": float(np.mean(data >= 1.0)),
            "midtone": float(ramp[310:330].mean()),
        }
        assert results[name]["finite_nonblank"], name
        assert results[name]["monotone_neutral_ramp"], f"non-monotone ramp: {name}"
        assert results[name]["highlight_range"] > 0.03, f"collapsed highlights: {name}"
        assert results[name]["clipped_fraction"] < 0.001, f"clipped test chart: {name}"
        if recipes[name]["Steep Look"]["Family"] == "monochrome":
            assert recipes[name]["Film Presets"]["SkinProtection"] == "0", name
            # The existing silver model deliberately includes warmtone paper.
            # Allow that subtle tint, but not the source's restored skin color.
            results[name]["max_paper_toning_channel_spread"] = float(np.ptp(data, axis=2).max())
            assert results[name]["max_paper_toning_channel_spread"] < 0.045, name
        # Tiny spatial/ringing deviations are recorded separately from gross reversals.
        assert np.diff(ramp).min() > -0.01, f"tone reversal: {name}"
    a, b = profiles[0], profiles[-1]
    switched = render("switch_A_to_B", [a, b])
    repeated = render("repeat_B", [b, b])
    results["switch_matches_direct"] = bool(np.array_equal(switched, rendered[b.stem]))
    results["repeat_matches_direct"] = bool(np.array_equal(repeated, rendered[b.stem]))
    assert results["switch_matches_direct"] and results["repeat_matches_direct"]

    columns, tile_width, tile_height = 3, 320, 216
    rows = (len(rendered) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * tile_width, rows * tile_height), (28, 28, 28))
    draw = ImageDraw.Draw(sheet)
    for index, (name, data) in enumerate(rendered.items()):
        x, y = index % columns * tile_width, index // columns * tile_height
        tile = Image.fromarray(np.rint(np.clip(data, 0, 1) * 255).astype(np.uint8))
        sheet.paste(tile.resize((320, 192), Image.Resampling.LANCZOS), (x, y + 24))
        draw.text((x + 8, y + 6), name, fill=(235, 235, 235))
    sheet.save(args.output / "contact-sheet.png")
    if args.photo:
        photo_baseline = args.output / "photo-baseline.pp3"
        photo_baseline.write_text(
            "[Film Presets]\nEnabled=false\n[White Balance]\nEnabled=true\nSetting=Camera\n"
            "[Resize]\nEnabled=true\nDataSpecified=3\nWidth=1000\nHeight=1000\n"
            "Method=Lanczos\nAppliesTo=Full image\nAllowUpscaling=false\n", encoding="ascii")
        photo_sheet = Image.new("RGB", (1200, 648), (28, 28, 28))
        photo_draw = ImageDraw.Draw(photo_sheet)
        examples = [None, "Sovereign Everyday", "Porcelain Portrait", "Neon Tungsten Night",
                    "Street Silver 400", "Vivid Chrome"]
        for index, name in enumerate(examples):
            looks = [] if name is None else [args.profiles / (name + ".pp3")]
            label = name or "Neutral"
            data = render("photo-" + label, looks, args.photo, photo_baseline)
            assert np.isfinite(data).all() and data.std() > 0.01, label
            x, y = index % 3 * 400, index // 3 * 324
            tile = Image.fromarray(np.rint(np.clip(data, 0, 1) * 255).astype(np.uint8))
            tile.thumbnail((400, 300), Image.Resampling.LANCZOS)
            photo_sheet.paste(tile, (x + (400 - tile.width) // 2, y + 24))
            photo_draw.text((x + 8, y + 6), label, fill=(235, 235, 235))
        photo_sheet.save(args.output / "photo-contact-sheet.png")
    results["elapsed_seconds_including_cli_startup"] = times
    (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="ascii")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
