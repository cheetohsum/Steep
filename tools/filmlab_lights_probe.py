#!/usr/bin/env python3
"""Film Lab V6 light recovery against a known answer, through steep-cli.

    python tools/filmlab_lights_probe.py --cli <steep-cli.exe> [--baseline <older steep-cli.exe>]

A lamp 64x brighter than the capture's clip, on dark ground, rendered three ways:

  truth    : the scene stored unclipped (16-bit, everything / 64, +6 stops in
             the pp3), recovery off
  captured : the same scene clipped at the lamp, recovery at a fixed +6 stops
             (STEEP_FILMLAB_LIGHTS_STOPS, which also switches off the size rule)
  recorded : captured, recovery off

Gates:
  * the halo round the lamp (halation on minus off) is the same for truth and
    captured: recovering the right brightness recovers the right halo;
  * recorded is several times weaker: the clip did hide the light;
  * a large over-exposed wall whose surroundings are within a stop of the clip
    is not raised: it is a surface, not a light;
  * with --baseline: V5, and V6 with recovery off, render as the older build.
"""
import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halation_probe as hp  # noqa: E402

W, H = 2400, 1600
LAMP, RADIUS = (700, 800), 14
WALL = (1500, 300, 2300, 1300)          # clipped, large, ringed by a bright surround
BOOST = 64.0


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * x ** (1 / 2.4) - 0.055)


def write_tiff16(path, img):
    h, w, _ = img.shape
    data = img.astype("<u2").tobytes()
    tags = [(256, 4, 1, w), (257, 4, 1, h), (258, 3, 3, None), (259, 3, 1, 1), (262, 3, 1, 2),
            (273, 4, 1, None), (277, 3, 1, 3), (278, 4, 1, h), (279, 4, 1, len(data)), (284, 3, 1, 1)]
    ifd_off = 8
    bits_off = ifd_off + 2 + 12 * len(tags) + 4
    data_off = bits_off + 6
    buf = b"II*\x00" + struct.pack("<I", ifd_off) + struct.pack("<H", len(tags))
    for tag, typ, n, v in tags:
        if tag == 258:
            v = bits_off
        if tag == 273:
            v = data_off
        buf += struct.pack("<HHI", tag, typ, n) + (struct.pack("<HH", v, 0) if typ == 3 and n == 1 else struct.pack("<I", v))
    buf += struct.pack("<I", 0) + struct.pack("<HHH", 16, 16, 16) + data
    Path(path).write_bytes(buf)


def render(cli, out, name, src, comp, stops=None, **film):
    s = dict(Enabled="true", ModelVersion=6, Preset="cinema_reveal_35", Strength=100, Halation=0, HalationSize=0,
             HalationThreshold=0, Bloom=-100, OutputSoftness=0, Format=35, FilmSharpness=100)
    s.update(film)
    pp3 = out / (name + ".pp3")
    pp3.write_text("[Exposure]\nCompensation=%g\n\n[Film Presets]\n" % comp + "".join(f"{k}={v}\n" for k, v in s.items()),
                   encoding="ascii")
    tif = out / (name + ".tif")
    env = dict(os.environ, OMP_NUM_THREADS="4", STEEP_FILMLAB_LIGHTS_TRACE="1")
    env["RT_SETTINGS"] = str((out / "settings").resolve())
    env["RT_CACHE"] = str((out / "cache").resolve())
    env.pop("STEEP_FILMLAB_LIGHTS_STOPS", None)
    env["STEEP_FILMLAB_NO_SPECTRAL"] = "1"  # compare against builds from before the spectral response
    if stops is not None:
        env["STEEP_FILMLAB_LIGHTS_STOPS"] = str(stops)
    r = subprocess.run([str(Path(cli).resolve()), "-q", "-Y", "-o", str(tif.resolve()), "-d", "-p", str(pp3.resolve()),
                        "-t", "-b16", "-c", str(Path(src).resolve())], capture_output=True, text=True, timeout=600, env=env)
    if r.returncode != 0 or not tif.exists():
        raise RuntimeError(f"{name}: {r.stdout[-600:]}\n{r.stderr[-600:]}")
    return hp.read_tiff16(tif)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--output", type=Path, default=Path("test-output/filmlab-lights-probe"))
    args = ap.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    yy, xx = np.mgrid[0:H, 0:W]
    scene = np.full((H, W), 0.004)                       # linear, 1.0 = the capture's clip
    scene[np.hypot(xx - LAMP[0], yy - LAMP[1]) <= RADIUS] = BOOST
    x0, y0, x1, y1 = WALL
    scene[y0 - 120:y1 + 120, x0 - 120:x1 + 120] = 0.7    # within a stop of the clip
    scene[y0:y1, x0:x1] = 1.3                            # the wall itself, just over
    write_tiff16(out / "truth.tif", np.repeat((srgb(scene / BOOST) * 65535 + 0.5)[..., None], 3, axis=2))
    write_tiff16(out / "captured.tif", np.repeat((srgb(np.minimum(scene, 1.0)) * 65535 + 0.5)[..., None], 3, axis=2))

    rc = hp.RING_EDGE_MM * hp.PX_PER_MM
    rr = np.hypot(xx - LAMP[0], yy - LAMP[1])
    near = (rr >= RADIUS + 1.0 * rc) & (rr <= RADIUS + 1.4 * rc)
    far = (rr >= RADIUS + 1.5 * rc) & (rr <= RADIUS + 3.0 * rc)

    halo, lit = {}, {}
    for name, src, comp, lights, stops in (("truth", "truth.tif", 6, -100, None), ("captured", "captured.tif", 0, 0, 6),
                                           ("recorded", "captured.tif", 0, -100, None)):
        lit[name] = render(args.cli, out, name + "_on", out / src, comp, stops, LightSources=lights)
        off = render(args.cli, out, name + "_off", out / src, comp, stops, LightSources=lights, Halation=-100)
        d = (lit[name] - off)[..., 0] * 255
        halo[name] = (float(d[near].mean()), float(d[far].mean()))
        print(f"{name:9s} lamp halo, red, 8-bit levels: at the ring {halo[name][0]:7.2f}   further out {halo[name][1]:7.2f}")

    failures = 0

    def gate(ok, what, detail=""):
        nonlocal failures
        print(("PASS  " if ok else "FAIL  ") + what + (f"  ({detail})" if detail else ""))
        failures += not ok

    for i, where in enumerate(("at the ring", "further out")):
        t, c, r = halo["truth"][i], halo["captured"][i], halo["recorded"][i]
        gate(abs(c - t) <= 0.03 * max(t, 1.0), f"recovered halo matches the unclipped scene {where}", f"{c:.2f} vs {t:.2f}")
        gate(r < 0.5 * t, f"without recovery the halo is much weaker {where}", f"{r:.2f} vs {t:.2f}")
    wall = np.abs(lit["captured"] - lit["recorded"])[y0 - 110:y1 + 110, x0 - 110:x1 + 110]
    gate(wall.max() * 255 < 0.5, "an over-exposed wall with a bright surround is not raised", f"max {wall.max() * 255:.3f} levels")

    if args.baseline:
        for model in (5, 6):
            n = render(args.cli, out, f"same_new{model}", out / "captured.tif", 0, ModelVersion=model, LightSources=-100)
            o = render(args.baseline, out, f"same_old{model}", out / "captured.tif", 0, ModelVersion=model, LightSources=-100)
            gate(np.array_equal(n, o), f"V{model} with recovery off renders as the older build",
                 f"max diff {np.abs(n - o).max() * 65535:.0f}/65535")
        n = render(args.cli, out, "same_new5_lights", out / "captured.tif", 0, ModelVersion=5, LightSources=100)
        o = render(args.baseline, out, "same_old5", out / "captured.tif", 0, ModelVersion=5, LightSources=-100)
        gate(np.array_equal(n, o), "V5 ignores the Light sources slider", f"max diff {np.abs(n - o).max() * 65535:.0f}/65535")

    print(("\n%d FAILED" % failures) if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
