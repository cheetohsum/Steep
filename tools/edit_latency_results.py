"""Summarize opt-in editor traces or compare two saved preview frames."""

import argparse
import json
from pathlib import Path
import re
import statistics


def summarize(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    painted = re.findall(
        r"cropPainted serial=(\d+).*?newestLag=([\d.]+)ms", text
    )
    samples = [float(ms) for serial, ms in painted if int(serial) > 1]
    return {
        "trace": str(path),
        "edit_frames": len(samples),
        "median_ms": round(statistics.median(samples), 1) if samples else None,
        "range_ms": [min(samples), max(samples)] if samples else None,
        "settled_publications": text.count("settledPublished"),
        "benchmark_finished": "[editBench] finished" in text,
    }


def compare(paths):
    import numpy as np
    from PIL import Image

    def read(path):
        if path.suffix.lower() in (".tif", ".tiff"):
            import tifffile
            return tifffile.imread(path).astype(np.int32)
        return np.asarray(Image.open(path)).astype(np.int32)

    a, b = [read(p) for p in paths]
    if a.shape != b.shape:
        raise ValueError(f"Frame sizes differ: {a.shape} != {b.shape}")
    delta = np.abs(a - b)
    return {
        "shape": a.shape,
        "identical": bool(np.array_equal(a, b)),
        "max_channel_delta": int(delta.max()),
        "mean_channel_delta": float(delta.mean()),
        "changed_pixels": int(np.count_nonzero(np.any(delta, axis=2))),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="*")
    parser.add_argument("--compare", type=Path, nargs=2)
    parser.add_argument("--compare-directories", type=Path, nargs=2)
    args = parser.parse_args()
    result = {"traces": [summarize(path) for path in args.logs]}
    if args.compare:
        result["comparison"] = compare(args.compare)
    if args.compare_directories:
        baseline, candidate = args.compare_directories
        frames = sorted(baseline.glob("*.tif"))
        if not frames:
            raise ValueError("No TIFF baseline frames found")
        result["exports"] = {
            frame.name: compare([frame, candidate / frame.name]) for frame in frames
        }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
