#!/usr/bin/env python3
"""Isolated native CLI repair fixtures; never modifies a photo or its sidecar."""
import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image
import tifffile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--baseline-cli", type=Path)
    parser.add_argument("--output", type=Path, default=Path("test-output/smart-repair/export"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    y, x = np.mgrid[:800, :1280]
    base = np.stack((.14 + x / 5000, .2 + y / 8000, .29 + x / 11000), axis=-1)
    base += (.015 * np.sin(x / 150) * np.cos(y / 100))[..., None]
    base[(x > 720) & (y > 240) & (y < 250)] *= .7
    source = args.output / "gradient-corner.tif"
    tifffile.imwrite(source, np.round(base * 65535).astype(np.uint16), photometric="rgb")
    env = dict(os.environ, OMP_NUM_THREADS="4", RT_SETTINGS=str((args.output / "settings").resolve()),
               RT_CACHE=str((args.output / "cache").resolve()), STEEP_EDIT_TRACE="1")
    # DLL search needs the matching app-local ONNX runtime, not Windows' built-in version.
    env["PATH"] = str(args.cli.resolve().parent) + os.pathsep + env.get("PATH", "")

    def render(name, radius=40, method=7, version=2, opacity=1, cli=None):
        profile = args.output / (name + ".pp3")
        suffix = "" if version == 0 else f"{version};"
        profile.write_text("[Version]\nVersion=353\n\n[Spot removal]\nEnabled=true\n"
                           f"Spot1=400;400;640;400;{radius};0.4;{opacity};{method};{suffix}\n"
                           "SpotStroke1=640;400;\n", encoding="ascii")
        output = args.output / (name + ".tif")
        result = subprocess.run([str((cli or args.cli).resolve()), "-Y", "-d", "-p", str(profile.resolve()),
                                 "-o", str(output.resolve()), "-t", "-b16", "-c", str(source.resolve())],
                                capture_output=True, text=True, timeout=300, env=env)
        (args.output / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode or not output.exists():
            raise RuntimeError(name + ": " + result.stderr[-1200:])
        image = tifffile.imread(output).astype(float) / 65535
        Image.fromarray(np.round(image * 255).astype(np.uint8)).save(args.output / (name + ".png"))
        return image

    neutral = render("neutral", opacity=0)
    small = render("small")
    repeated = render("small-repeat")
    large = render("tiled", radius=170)
    glare = render("glare", radius=170, method=6)
    legacy = render("legacy", version=0)
    explicit_legacy = render("legacy-explicit", version=1)
    checks = {"repeat_exact": bool(np.array_equal(small, repeated)),
              "legacy_default": bool(np.array_equal(legacy, explicit_legacy))}
    distance = np.hypot(x - 640, y - 400)
    for name, values, radius in (("small", small, 40), ("large", large, 170), ("glare", glare, 170)):
        checks[name + "_outside_unchanged"] = bool(np.max(np.abs(values[distance > radius * 1.4 + 2] - neutral[distance > radius * 1.4 + 2])) <= 2 / 65535)
        checks[name + "_finite"] = bool(np.isfinite(values).all())
        checks[name + "_applied"] = bool(np.max(np.abs(values - neutral)) > 1 / 65535)
    if args.baseline_cli:
        baseline = render("baseline-legacy", version=0, cli=args.baseline_cli)
        checks["legacy_matches_previous_build"] = bool(np.array_equal(legacy, baseline))
    (args.output / "results.json").write_text(json.dumps(checks, indent=2), encoding="ascii")
    print(json.dumps(checks, indent=2), flush=True)
    if not all(checks.values()):
        raise SystemExit("FAIL: " + ", ".join(k for k, v in checks.items() if not v))
    print("PASS: native 16-bit repair exports")


if __name__ == "__main__":
    main()
