#!/usr/bin/env python3
"""End-to-end check of the traced halation in Film Lab V5, through steep-cli.

    python tools/halation_probe.py --cli <steep-cli.exe> [--baseline <older steep-cli.exe>]

Renders a synthetic scene -- a point light, a bright window and a large flat
bright patch on dark ground, pushed three stops over -- with halation at full
strength and switched off, and measures the difference. Gates:

  * a ring: around the point light the halo is brighter just outside the
    base's total-internal-reflection edge than inside it (a blob is not);
  * flat areas are untouched: deep inside the bright patch, halation on and
    off agree (the uniform-field share is already in the stock curve);
  * edges still halate: a red fringe outside the window;
  * off-axis lean: a light near a corner halates more on its outward side
    than its inward side, a light on the axis evenly; a rangefinder lens
    leans harder than an SLR, a telecentric one not at all;
  * with --baseline: V4 renders are identical to the older build (only V5
    changed).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

W, H = 2400, 1600               # short side 1600 px = 24 mm of 35mm film
PX_PER_MM = H / 24.0
RING_EDGE_MM = 0.2328           # 127 um acetate, n 1.48: 2 t tan(asin(1/n))
POINT = (620, 800)
CENTRE_POINT = (1200, 800)       # on the optical axis
CORNER_POINT = (150, 1480)       # ~17 degrees off axis with a 60 mm exit pupil
WINDOW = (1500, 500, 1640, 1100)
PATCH = (1900, 250, 2350, 1350)


def scene(path):
    img = Image.new("RGB", (W, H), (12, 12, 12))
    d = ImageDraw.Draw(img)
    for p in (POINT, CENTRE_POINT, CORNER_POINT):
        d.ellipse((p[0] - 3, p[1] - 3, p[0] + 3, p[1] + 3), fill=(255, 255, 255))
    d.rectangle(WINDOW, fill=(255, 255, 255))
    d.rectangle(PATCH, fill=(235, 235, 235))
    img.save(path)


def render(cli, out_dir, name, source, **film):
    settings = {"Enabled": "true", "Preset": "cinema_reveal_35", "ModelVersion": 5, "Strength": 100,
                "Halation": 100, "HalationSize": 0, "HalationThreshold": 0, "Bloom": -100,
                "OutputSoftness": 0, "Format": "35"}
    settings.update(film)
    pp3 = out_dir / (name + ".pp3")
    pp3.write_text("[Exposure]\nCompensation=3\n\n[Film Presets]\n" +
                   "".join(f"{k}={v}\n" for k, v in settings.items()), encoding="ascii")
    out = out_dir / (name + ".tif")
    env = dict(os.environ, OMP_NUM_THREADS="4")
    env["RT_SETTINGS"] = str((out_dir / "settings").resolve())
    env["RT_CACHE"] = str((out_dir / "cache").resolve())
    r = subprocess.run([str(Path(cli).resolve()), "-q", "-Y", "-o", str(out.resolve()), "-d", "-p", str(pp3.resolve()),
                        "-t", "-b16", "-c", str(source.resolve())], capture_output=True, text=True, timeout=300, env=env)
    if r.returncode != 0 or not out.exists():
        raise RuntimeError(f"{name}: {r.stdout[-800:]}\n{r.stderr[-800:]}")
    return read_tiff16(out)


def read_tiff16(path):
    """steep-cli's uncompressed 16-bit RGB TIFF, without needing tifffile (PIL can't hold 48-bit RGB)."""
    try:
        import tifffile
        return tifffile.imread(path).astype(np.float64) / 65535.0
    except ImportError:
        pass
    import struct
    data = Path(path).read_bytes()
    end = "<" if data[:2] == b"II" else ">"
    (ifd,) = struct.unpack(end + "I", data[4:8])
    (count,) = struct.unpack(end + "H", data[ifd:ifd + 2])
    tags = {}
    for i in range(count):
        tag, typ, n, value = struct.unpack(end + "HHII", data[ifd + 2 + 12 * i: ifd + 14 + 12 * i])
        if tag not in (256, 257, 259, 273, 277, 279) or typ not in (3, 4):
            continue  # metadata (ICC profile, XMP, ...) is not needed
        size = 2 if typ == 3 else 4
        if typ == 3 and n == 1:
            value = struct.unpack(end + "H", data[ifd + 10 + 12 * i: ifd + 12 + 12 * i])[0]
        if n * size > 4:  # value is an offset to an array
            fmt = end + ("H" if typ == 3 else "I") * n
            value = list(struct.unpack(fmt, data[value:value + n * size]))
        tags[tag] = value
    w, h = tags[256], tags[257]
    spp = tags.get(277, 3)
    if tags.get(259, 1) != 1:
        raise RuntimeError("compressed TIFF; run steep-cli with -t, not -tz")
    offsets = tags[273] if isinstance(tags[273], list) else [tags[273]]
    counts = tags[279] if isinstance(tags[279], list) else [tags[279]]
    raw = b"".join(data[o:o + c] for o, c in zip(offsets, counts))
    img = np.frombuffer(raw, dtype=end + "u2")[: w * h * spp].reshape(h, w, spp)[..., :3]
    return img.astype(np.float64) / 65535.0


def radial_mean(diff, centre, r_px, half=0.75):
    yy, xx = np.mgrid[0:diff.shape[0], 0:diff.shape[1]]
    rr = np.hypot(xx - centre[0], yy - centre[1])
    m = np.abs(rr - r_px) <= half
    return float(diff[m].mean())


def lean(diff, light, rc):
    # outward : inward mean halo around a light, halves split by the direction
    # away from the frame centre (+x for a light on the axis)
    cx, cy = W / 2.0, H / 2.0
    ox, oy = light[0] - cx, light[1] - cy
    if abs(ox) < 1 and abs(oy) < 1:
        ox, oy = 1.0, 0.0
    r_out = int(3 * rc) + 2
    ys, xs = np.mgrid[light[1] - r_out:light[1] + r_out + 1, light[0] - r_out:light[0] + r_out + 1]
    ok = (ys >= 0) & (ys < diff.shape[0]) & (xs >= 0) & (xs < diff.shape[1])
    ys, xs = ys[ok], xs[ok]
    dx, dy = xs - light[0], ys - light[1]
    r = np.hypot(dx, dy)
    ring = (r >= 0.6 * rc) & (r <= 3 * rc)
    side = dx * ox + dy * oy
    vals = diff[ys, xs]
    return float(vals[ring & (side > 0)].mean() / max(vals[ring & (side < 0)].mean(), 1e-9))


def measure(on, off):
    red = on[..., 0] - off[..., 0]
    rc = RING_EDGE_MM * PX_PER_MM
    inside = np.mean([radial_mean(red, POINT, f * rc) for f in (0.45, 0.6, 0.75)])
    band = np.mean([radial_mean(red, POINT, f * rc) for f in (1.05, 1.15, 1.25)])
    x0, y0, x1, y1 = PATCH
    margin = int(1.5 * PX_PER_MM)  # well past the kernel's main body
    interior = np.abs(on[y0 + margin:y1 - margin, x0 + margin:x1 - margin] - off[y0 + margin:y1 - margin, x0 + margin:x1 - margin])
    fringe = red[WINDOW[1] + 50:WINDOW[3] - 50, WINDOW[2] + int(0.25 * PX_PER_MM):WINDOW[2] + int(0.6 * PX_PER_MM)]
    return {"ring": band / max(inside, 1e-9), "inside": inside, "band": band,
            "flatMax": float(interior.max()), "flatMean": float(interior.mean()), "fringe": float(fringe.mean()),
            "leanCorner": lean(red, CORNER_POINT, rc), "leanCentre": lean(red, CENTRE_POINT, rc)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--output", type=Path, default=Path("test-output/halation-probe"))
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    src = args.output / "scene.png"
    scene(src)

    failures = 0

    def gate(ok, what, detail=""):
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + what + (f"  ({detail})" if detail else ""))
        failures += not ok

    on = render(args.cli, args.output, "v5_on", src)
    off = render(args.cli, args.output, "v5_off", src, Halation=-100)
    m = measure(on, off)
    print(f"V5 new: ring {m['ring']:.2f} (band {m['band'] * 65535:.0f} vs inside {m['inside'] * 65535:.0f} /65535), "
          f"flat interior max {m['flatMax'] * 65535:.1f} mean {m['flatMean'] * 65535:.2f}, window fringe {m['fringe'] * 65535:.0f}")
    gate(m["ring"] > 1.15, "V5: the point light's halo is a ring, brighter just outside the TIR edge than inside",
         f"ratio {m['ring']:.2f}")
    gate(m["flatMax"] * 65535 < 64, "V5: deep inside a flat bright area, halation on and off agree",
         f"max diff {m['flatMax'] * 65535:.1f}/65535")
    gate(m["fringe"] * 65535 > 200, "V5: a red fringe still hugs the window's edge", f"{m['fringe'] * 65535:.0f}/65535")
    gate(m["leanCorner"] > 1.15, "V5: near a corner the halo leans outwards", f"outward/inward {m['leanCorner']:.2f}")
    gate(abs(m["leanCentre"] - 1.0) < 0.07, "V5: on the axis the halo is even", f"right/left {m['leanCentre']:.2f}")
    rf = measure(render(args.cli, args.output, "v5_rangefinder", src, LensDesign="rangefinder"), off)
    tc = measure(render(args.cli, args.output, "v5_telecentric", src, LensDesign="telecentric"), off)
    gate(rf["leanCorner"] > m["leanCorner"] + 0.1, "V5: a rangefinder lens leans the corner halo harder than an SLR",
         f"{rf['leanCorner']:.2f} vs {m['leanCorner']:.2f}")
    gate(abs(tc["leanCorner"] - 1.0) < 0.07, "V5: a telecentric lens keeps the corner halo round", f"outward/inward {tc['leanCorner']:.2f}")

    if args.baseline:
        bon = render(args.baseline, args.output, "base_v5_on", src)
        boff = render(args.baseline, args.output, "base_v5_off", src, Halation=-100)
        b = measure(bon, boff)
        print(f"V5 old: ring {b['ring']:.2f}, flat interior max {b['flatMax'] * 65535:.1f} mean {b['flatMean'] * 65535:.2f}, "
              f"window fringe {b['fringe'] * 65535:.0f}")
        # Builds before the traced kernels made a blob here (ratio ~0.2), which
        # is what showed the ring measure can tell the two apart; later
        # baselines already have the ring, so this is information, not a gate.
        print(f"INFO  baseline V5 ring ratio {b['ring']:.2f} (a blob scores below 1)")
        for preset in ("cinema_reveal_35", "sovereign"):
            n4 = render(args.cli, args.output, f"v4_{preset}", src, ModelVersion=4, Preset=preset)
            o4 = render(args.baseline, args.output, f"base_v4_{preset}", src, ModelVersion=4, Preset=preset)
            gate(np.array_equal(n4, o4), f"V4 ({preset}) renders bit-identically to the older build",
                 f"max diff {np.abs(n4 - o4).max() * 65535:.0f}/65535")

    print(("\n%d FAILED" % failures) if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
