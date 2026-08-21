#!/usr/bin/env python3
"""Film Lab V4 verification probe (Tracks A, D, E).

Renders synthetic scenes through steep-cli and gates the V4 contracts:

  1. SDR equality: below diffuse white V4 matches V3 (same tone maths).
  2. Radiance ranking: sources the display pipeline clips to identical
     white must halate in proportion to their true scene magnitude, which
     only survives through the rgbProc scene tap.
  3. Format scaling: the halo is sized in film micrometres, so a large
     format frame shows a proportionally tighter halo than 35mm.
  4. Remjet: a motion-picture stock in its native ECN-2 bath barely
     halates; cross-processed in C-41 (remjet stripped) it glows.
  5. Grain decoupling: the film stage adds NO grain in any model (that is
     the standalone Grain tool's job now), the tool's luminance grain
     lands in a sane band with no mean shift, and its Color parameter
     adds chroma grain.

A passing run proves the tap plumbing end to end through the simpleprocess
(export) path. The Grain tool's Selwyn preview attenuation and its
crop-anchored pattern have no CLI path - check them in the editor by
comparing fit-screen, 100% zoom, and a detail window over the same area.
"""

import os
import statistics
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"

W = H = 800
PATCH = 80
PATCH_Y = 200
# sRGB values -> linear 1.0, 0.585, 0.35, 0.216; +3EV lifts them to
# 8.0, 4.7, 2.8, 1.7 in the tap while the display path clips all to white.
PATCHES = [(100, 255), (300, 201), (500, 160), (700, 130)]
RAMP_Y = 700
BACKGROUND = 30

FORMAT_W = FORMAT_H = 3000
FORMAT_PATCH = 200


def build_scene():
    im = Image.new("RGB", (W, H), (BACKGROUND,) * 3)
    px = im.load()
    for cx, value in PATCHES:
        for x in range(cx - PATCH // 2, cx + PATCH // 2):
            for y in range(PATCH_Y - PATCH // 2, PATCH_Y + PATCH // 2):
                px[x, y] = (value,) * 3
    for x in range(W):
        v = round(x * 255 / (W - 1))
        for y in range(RAMP_Y, RAMP_Y + 40):
            px[x, y] = (v,) * 3
    path = os.path.join(HERE, "v4_scene.png")
    im.save(path)
    return path


def build_format_scene():
    im = Image.new("RGB", (FORMAT_W, FORMAT_H), (BACKGROUND,) * 3)
    px = im.load()
    c = FORMAT_W // 2
    for x in range(c - FORMAT_PATCH // 2, c + FORMAT_PATCH // 2):
        for y in range(c - FORMAT_PATCH // 2, c + FORMAT_PATCH // 2):
            px[x, y] = (255,) * 3
    path = os.path.join(HERE, "v4_format_scene.png")
    im.save(path)
    return path


def build_flat_scene():
    im = Image.new("RGB", (400, 400), (128,) * 3)
    path = os.path.join(HERE, "v4_flat_scene.png")
    im.save(path)
    return path


def write_pp3(name, compensation=3, contrast=0, **kv):
    settings = {
        "Enabled": "true",
        "Preset": "sovereign",
        "ModelVersion": 4,
        "Strength": 100,
        "Halation": 60,
        "Grain": -100,
    }
    settings.update(kv)
    path = os.path.join(HERE, name)
    with open(path, "w", newline="\n") as f:
        f.write(f"[Exposure]\nCompensation={compensation}\nContrast={contrast}\n\n[Film Presets]\n")
        for k, v in settings.items():
            f.write(f"{k}={v}\n")
    return path


def render(pp3, scene, out):
    outp = os.path.join(HERE, out)
    for candidate in (outp, outp + ".tif"):
        if os.path.exists(candidate):
            os.remove(candidate)
    r = subprocess.run(
        [CLI, "-Y", "-o", outp, "-d", "-p", pp3, "-t", "-b8", "-c", scene],
        capture_output=True, text=True)
    result = outp + ".tif" if os.path.exists(outp + ".tif") else outp
    if not os.path.exists(result):
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        sys.exit("render failed: " + out)
    return result


def load(path):
    return Image.open(path).convert("RGB").load()


def ring_warmth(px, cx, cy, half, inner, outer, far_y):
    """Mean warm shift (R - B) in a ring [inner, outer] px beyond the patch
    edge, minus the far background's own R - B."""
    ring = []
    for d in range(half + inner, half + outer):
        for x, y in ((cx + d, cy), (cx - d, cy), (cx, cy + d), (cx, cy - d)):
            r, g, b = px[x, y]
            ring.append(r - b)
    far = []
    for x in range(40, 761, 60):
        r, g, b = px[min(x, cx * 2 - 40), far_y]
        far.append(r - b)
    return sum(ring) / len(ring) - sum(far) / len(far)


def main():
    failures = []
    scene = build_scene()

    out3 = render(write_pp3("v4_m3.pp3", ModelVersion=3), scene, "v4_out_m3")
    out4 = render(write_pp3("v4_m4.pp3"), scene, "v4_out_m4")
    px3, px4 = load(out3), load(out4)

    # --- 1. SDR equality below the clip point. +3EV puts encoded ~99
    #     (x ~ 310) at 1.0 linear, where V4's paper rolloff legitimately
    #     parts ways with V3's hard clip and the ramp's tail halates.
    worst = 0
    for x in range(8, 270, 4):
        for c in range(3):
            worst = max(worst, abs(px3[x, RAMP_Y + 20][c] - px4[x, RAMP_Y + 20][c]))
    print(f"1. SDR ramp: worst V3-vs-V4 difference {worst} codes")
    if worst > 2:
        failures.append(f"SDR ramp diverges by {worst} codes")

    # --- 1b. Tone controls act on the print. V4 excludes the tone curve
    #     from the film EXPOSURE (that is what un-clips highlights), but
    #     re-applies the user's display grading to the film's output. The
    #     contract: wherever V3 responds to a tone control, V4 must move
    #     the same way with comparable magnitude. (RT's Contrast pivots on
    #     the image mean, so absolute expectations per point are wrong -
    #     this regressed once as "every tone control silently dead in V4",
    #     which read as the film washing out dark areas.)
    t30 = load(render(write_pp3("v4_t3_c0.pp3", compensation=0, ModelVersion=3, Halation=-100),
                      scene, "v4_out_t3_c0"))
    t3c = load(render(write_pp3("v4_t3_c40.pp3", compensation=0, contrast=40, ModelVersion=3, Halation=-100),
                      scene, "v4_out_t3_c40"))
    t40 = load(render(write_pp3("v4_t4_c0.pp3", compensation=0, Halation=-100),
                      scene, "v4_out_t4_c0"))
    t4c = load(render(write_pp3("v4_t4_c40.pp3", compensation=0, contrast=40, Halation=-100),
                      scene, "v4_out_t4_c40"))
    report = []
    for x in (90, 240, 640):
        d3 = t3c[x, RAMP_Y + 20][1] - t30[x, RAMP_Y + 20][1]
        d4 = t4c[x, RAMP_Y + 20][1] - t40[x, RAMP_Y + 20][1]
        report.append(f"in{round(x * 255 / (W - 1))}: v3 {d3:+d} v4 {d4:+d}")
        if abs(d3) >= 3:
            if d3 * d4 < 0:
                failures.append(f"V4 tone response opposes V3 at x={x} ({d3:+d} vs {d4:+d})")
            elif abs(d4) < 0.4 * abs(d3) - 1:
                failures.append(f"V4 barely responds to tone controls at x={x} ({d3:+d} vs {d4:+d})")
    print("1b. tone response deltas with Contrast=40: " + " | ".join(report))

    # --- 2. Halation ranks with true magnitude.
    h3 = [ring_warmth(px3, cx, PATCH_Y, PATCH // 2, 6, 22, 520) for cx, _ in PATCHES]
    h4 = [ring_warmth(px4, cx, PATCH_Y, PATCH // 2, 6, 22, 520) for cx, _ in PATCHES]
    print("2. patch stops over white:   +3.00    +2.23    +1.50    +0.77")
    print("   V3 halo (R-B codes):  " + "".join(f"{h:>9.2f}" for h in h3))
    print("   V4 halo (R-B codes):  " + "".join(f"{h:>9.2f}" for h in h4))
    if not all(h4[i] > h4[i + 1] + 0.25 for i in range(3)):
        failures.append("V4 halo does not rank with magnitude")
    if h4[0] < 2.0:
        failures.append(f"V4 brightest halo too weak ({h4[0]:.2f})")

    # --- 3. Format scaling: 220um of tail is ~27px on a 3000px 35mm frame
    #     but only ~7px on 4x5. Sample where the 35mm blur still has
    #     support (its three-pass box is ~28px sigma) and 4x5 does not.
    fmt_scene = build_format_scene()
    c, half = FORMAT_W // 2, FORMAT_PATCH // 2
    p35 = load(render(write_pp3("v4_fmt35.pp3", Format="35mm"), fmt_scene, "v4_out_fmt35"))
    plf = load(render(write_pp3("v4_fmtlarge.pp3", Format="large"), fmt_scene, "v4_out_fmtlarge"))
    far35 = ring_warmth(p35, c, c, half, 16, 36, 400)
    farlf = ring_warmth(plf, c, c, half, 16, 36, 400)
    near35 = ring_warmth(p35, c, c, half, 4, 12, 400)
    nearlf = ring_warmth(plf, c, c, half, 4, 12, 400)
    print(f"3. format halo: 35mm near {near35:.2f} far {far35:.2f} | large near {nearlf:.2f} far {farlf:.2f}")
    if nearlf < 0.5:
        failures.append("large-format halo missing entirely")
    if far35 < farlf * 3.0 or far35 < 1.0:
        failures.append(f"35mm tail not wider than large format ({far35:.2f} vs {farlf:.2f})")

    # --- 4. Remjet: native ECN-2 vs C-41 cross-process on a motion stock.
    ecn = load(render(write_pp3("v4_ecn2.pp3", Preset="cinematic_500t", Process="ecn2"),
                      scene, "v4_out_ecn2"))
    c41 = load(render(write_pp3("v4_c41.pp3", Preset="cinematic_500t", Process="c41"),
                      scene, "v4_out_c41"))
    hecn = ring_warmth(ecn, PATCHES[0][0], PATCH_Y, PATCH // 2, 6, 22, 520)
    hc41 = ring_warmth(c41, PATCHES[0][0], PATCH_Y, PATCH // 2, 6, 22, 520)
    print(f"4. remjet: ecn2 halo {hecn:.2f}, c41 cross-process halo {hc41:.2f}")
    if not hc41 > max(hecn * 3.0, hecn + 2.0):
        failures.append(f"cross-process halo not dominant ({hc41:.2f} vs {hecn:.2f})")

    # --- 5. Grain decoupling and the standalone Grain tool. The film
    #     stage must add NO grain of its own in any model (the Film
    #     Presets Grain key is dead), and Effects > Grain carries the
    #     texture instead: luminance grain in a sane band, no mean shift,
    #     and the new Color parameter adds chroma noise.
    flat = build_flat_scene()
    goff = load(render(write_pp3("v4_grain_off.pp3", compensation=0, Halation=-100),
                       flat, "v4_out_goff"))
    gfilm = load(render(write_pp3("v4_grain_film.pp3", compensation=0, Halation=-100, Grain=60),
                        flat, "v4_out_gfilm"))

    def grain_pp3(name, color):
        path = os.path.join(HERE, name)
        with open(path, "w", newline="\n") as f:
            f.write("[Film Presets]\nEnabled=true\nPreset=sovereign\nModelVersion=4\n"
                    "Strength=100\nHalation=-100\n\n"
                    "[Grain]\nEnabled=true\nISO=1600\nStrength=60\nScale=100\n"
                    f"Color={color}\n")
        return path

    gtool = load(render(grain_pp3("v4_grain_tool.pp3", 0), flat, "v4_out_gtool"))
    gcolor = load(render(grain_pp3("v4_grain_color.pp3", 60), flat, "v4_out_gcolor"))

    worst_film = 0
    lum = []
    warm_l = []
    warm_c = []
    mean_on = 0.0
    mean_off = 0.0
    n = 0
    for x in range(100, 300, 2):
        for y in range(100, 300, 2):
            for ch in range(3):
                worst_film = max(worst_film, abs(gfilm[x, y][ch] - goff[x, y][ch]))
            r, g, b = gtool[x, y]
            lum.append(g)
            warm_l.append(r - b)
            r, g, b = gcolor[x, y]
            warm_c.append(r - b)
            mean_on += sum(gtool[x, y][:3]) / 3
            mean_off += sum(goff[x, y][:3]) / 3
            n += 1
    sigma_l = statistics.pstdev(lum)
    sigma_warm0 = statistics.pstdev(warm_l)
    sigma_warm1 = statistics.pstdev(warm_c)
    shift = abs(mean_on - mean_off) / n
    print(f"5. grain: film-stage residual {worst_film} codes | tool sigma_L {sigma_l:.2f}, "
          f"chroma sigma(R-B) {sigma_warm0:.2f} -> {sigma_warm1:.2f} with Color, mean shift {shift:.2f}")
    if worst_film > 0:
        failures.append(f"film presets still add grain ({worst_film} codes)")
    if not 1.0 < sigma_l < 14.0:
        failures.append(f"grain tool amplitude out of band (sigma_L {sigma_l:.2f})")
    if sigma_warm1 < sigma_warm0 * 1.5:
        failures.append(f"Color parameter adds no chroma grain ({sigma_warm1:.2f} vs {sigma_warm0:.2f})")
    if shift > 1.5:
        failures.append(f"grain shifts the mean by {shift:.2f} codes")

    # --- 5b. Halation locality: a frame crowded with big blown bokeh (the
    #     scenario that exposed the "tinted bloom" failure) must render as
    #     red fringes hugging each blob, NOT as a global veil. Fringe is
    #     measured just outside a blob; veil is the background lift in a
    #     far corner, against a halation-free render.
    bokeh_path = os.path.join(HERE, "v4_bokeh_scene.png")
    im = Image.new("RGB", (FORMAT_W, FORMAT_H), (25,) * 3)
    bpx = im.load()
    blobs = [(700, 700, 220), (1700, 900, 260), (1100, 1700, 180),
             (2100, 1800, 240), (800, 2300, 200)]
    for bx, by, br in blobs:
        for x in range(bx - br, bx + br):
            for y in range(by - br, by + br):
                if (x - bx) ** 2 + (y - by) ** 2 <= br * br:
                    bpx[x, y] = (255,) * 3
    im.save(bokeh_path)
    hon = load(render(write_pp3("v4_bokeh_on.pp3"), bokeh_path, "v4_out_bokeh_on"))
    hoff = load(render(write_pp3("v4_bokeh_off.pp3", Halation=-100), bokeh_path, "v4_out_bokeh_off"))

    fringe = []
    for d in range(8, 30):
        r, g, b = hon[700 + 220 + d, 700]
        r0, g0, b0 = hoff[700 + 220 + d, 700]
        fringe.append((sum((r, g, b)) - sum((r0, g0, b0))) / 3)
    veil = []
    for x in range(2700, 2960, 20):
        for y in range(2700, 2960, 20):
            veil.append((sum(hon[x, y][:3]) - sum(hoff[x, y][:3])) / 3)
    fringe_lift = sum(fringe) / len(fringe)
    veil_lift = sum(veil) / len(veil)
    print(f"5b. halation locality: fringe +{fringe_lift:.2f} codes, far-field veil +{veil_lift:.2f} codes")
    if fringe_lift < 6.0:
        failures.append(f"halation fringe too weak ({fringe_lift:.2f} codes)")
    if veil_lift > 5.0:
        failures.append(f"halation veils the whole frame (+{veil_lift:.2f} codes far from sources)")
    if veil_lift > 0.5 and fringe_lift / veil_lift < 4.0:
        failures.append(f"halation not local (fringe/veil {fringe_lift / max(veil_lift, 0.01):.1f}x)")

    # --- 6. Adjacency: the diffused-inhibitor edge effect. On a mid-grey
    #     patch against darker grey (no halation, no exposure push), the
    #     Eberhard signature is contrast amplification ACROSS the edge:
    #     brighter just inside than deep inside, darker just outside than
    #     far outside. outputSoftness=100 drives the adjacency strength
    #     negative, giving the control rendering.
    adj_scene_path = os.path.join(HERE, "v4_adj_scene.png")
    im = Image.new("RGB", (FORMAT_W, FORMAT_H), (80,) * 3)
    apx = im.load()
    c = FORMAT_W // 2
    for x in range(c - FORMAT_PATCH // 2, c + FORMAT_PATCH // 2):
        for y in range(c - FORMAT_PATCH // 2, c + FORMAT_PATCH // 2):
            apx[x, y] = (140,) * 3
    im.save(adj_scene_path)
    aon = load(render(write_pp3("v4_adj_on.pp3", compensation=0, Halation=-100),
                      adj_scene_path, "v4_out_adjon"))
    aoff = load(render(write_pp3("v4_adj_off.pp3", compensation=0, Halation=-100, OutputSoftness=100),
                       adj_scene_path, "v4_out_adjoff"))

    def edge_step(px):
        """Luma step across the patch's right edge, inner minus outer,
        sampled 2px either side, averaged along the edge."""
        e = c + FORMAT_PATCH // 2
        vals = []
        for y in range(c - 60, c + 61, 10):
            inner = sum(px[e - 3, y][:3]) / 3
            outer = sum(px[e + 2, y][:3]) / 3
            vals.append(inner - outer)
        return sum(vals) / len(vals)

    step_on, step_off = edge_step(aon), edge_step(aoff)
    print(f"6. adjacency edge step: with {step_on:.2f}, control {step_off:.2f} codes")
    if step_on < step_off + 1.0:
        failures.append(f"adjacency does not amplify the edge ({step_on:.2f} vs {step_off:.2f})")

    print()
    if failures:
        for f in failures:
            print("FAIL:", f)
        sys.exit(1)
    print("PASS: V4 scene tap, halation physics, and grain contracts all hold")


if __name__ == "__main__":
    main()
