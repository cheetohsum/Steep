"""Render grading regression fixtures through the native Steep CLI."""
import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image, ImageDraw
import tifffile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--previous-profiles", type=Path)
    parser.add_argument("--chart", type=Path, required=True)
    parser.add_argument("--photo", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, OMP_NUM_THREADS="4",
               RT_SETTINGS=str((args.output / "settings").resolve()),
               RT_CACHE=str((args.output / "cache").resolve()))
    baseline = args.output / "baseline.pp3"
    baseline.write_text("[Film Presets]\nEnabled=false\n[Sharpening]\nEnabled=false\n"
                        "[White Balance]\nEnabled=false\n[Color Grading]\nEnabled=false\n", encoding="ascii")
    profiles = sorted(args.profiles.glob("*.pp3"))
    assert len(profiles) == 24

    def render(name, source, profile=None):
        output = args.output / (name + ".tif")
        command = [str(args.cli.resolve()), "-Y", "-o", str(output.resolve()), "-p", str(baseline.resolve())]
        if profile:
            command += ["-p", str(profile.resolve())]
        command += ["-a", "-t", "-b16", "-c", str(source.resolve())]
        run = subprocess.run(command, env=env, capture_output=True, timeout=120)
        (args.output / (name + ".log")).write_bytes(run.stdout + run.stderr)
        assert run.returncode == 0, (name, run.returncode)
        data = tifffile.imread(output)
        assert data.dtype == np.uint16 and data.ndim == 3 and data.shape[2] == 3
        return data.astype(np.float64) / 65535

    def sheet(images, filename, width=256, height=180, columns=4):
        canvas = Image.new("RGB", (columns * width, ((len(images)+columns-1)//columns) * height), (28,28,28))
        draw = ImageDraw.Draw(canvas)
        for index, (name, data) in enumerate(images.items()):
            x, y = index % columns * width, index // columns * height
            tile = Image.fromarray(np.rint(np.clip(data,0,1)*255).astype(np.uint8))
            tile.thumbnail((width, height-24), Image.Resampling.LANCZOS)
            canvas.paste(tile, (x+(width-tile.width)//2, y+24))
            draw.text((x+6,y+5), name, fill=(240,240,240))
        canvas.save(args.output / filename)

    neutral = render("neutral", args.chart)
    linear = np.where(neutral <= .04045, neutral/12.92, ((neutral+.055)/1.055)**2.4)
    light = linear @ np.array([.2126, .7152, .0722])
    mids = (light > .12) & (light < .4)
    assert mids.sum() > 512, "chart needs enough midtone pixels"
    images = {"Neutral": neutral}
    comparisons = {}
    results = {}
    for profile in profiles:
        data = render(profile.stem, args.chart, profile)
        assert data.shape == neutral.shape
        delta = np.abs(data-neutral).mean()
        assert delta > .004 and data.std() > .04, (profile.stem, "invisible or blank")
        results[profile.stem] = {"mean_rgb_difference": round(float(delta),5),
                                 "clipped_channel_fraction": round(float(np.mean(data >= 1)),5)}
        if args.previous_profiles:
            previous = render("previous-"+profile.stem, args.chart, args.previous_profiles/profile.name)
            old_delta = float(np.abs(previous-neutral)[mids].mean())
            new_delta = float(np.abs(data-neutral)[mids].mean())
            results[profile.stem]["midtone_change_ratio"] = round(new_delta/max(old_delta,1e-8),3)
            if profile.stem in ("amber-slate", "glacier", "neon-orchid", "forest-brass", "teal-amber", "soft-portrait"):
                comparisons[profile.stem+" - neutral"] = neutral
                comparisons[profile.stem+" - previous"] = previous
                comparisons[profile.stem+" - stronger mids"] = data
        images[profile.stem] = data
    sheet(images, "grades-chart.png")
    if comparisons:
        sheet(comparisons, "midtone-comparison.png", columns=3)
        median = float(np.median([r["midtone_change_ratio"] for r in results.values()]))
        assert median > 1.6, ("midtone impact must increase in actual rendered pixels", median)
        print("Median rendered midtone change ratio:", median)
    if args.photo:
        photos = {"Neutral": render("photo-neutral", args.photo)}
        for name in ("amber-slate", "forest-brass", "nocturne", "neon-orchid", "rosewater", "glacier", "teal-amber"):
            photos[name] = render("photo-"+name, args.photo, args.profiles/(name+".pp3"))
        sheet(photos, "grades-photo.png", 300, 260)
    (args.output/"results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps({"rendered_grades":len(profiles), "minimum_rgb_difference":min(x["mean_rgb_difference"] for x in results.values()),
                      "max_clipped_channels":max(x["clipped_channel_fraction"] for x in results.values())}))


if __name__ == "__main__":
    main()
