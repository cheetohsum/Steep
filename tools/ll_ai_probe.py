"""Locallab AI mask verification probe.

The masking tab's AI masks go through AIMaskCache::getPreparedMask, which the
double exposure probe does not touch — that path uses PartnerMaskStore. This
renders a scene whose sky/ground boundary we placed ourselves and checks the
spot's effect lands on the class it was told to find, and nowhere else.

Run after any change to aimaskcache.cc, aisubject.h, or the AI mask block in
iplocallab.cc.
"""
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = r"C:\msys64\home\alexr\build-hw\Release\steep-cli.exe"

SCENE_W, SCENE_H = 512, 384
HORIZON = int(SCENE_H * 0.55)   # 211: sky above, ground below


def make_scene(path):
    im = Image.new("RGB", (SCENE_W, SCENE_H))
    px = im.load()
    for y in range(SCENE_H):
        for x in range(SCENE_W):
            if y < HORIZON:
                t = y / HORIZON
                px[x, y] = (int(90 + 90 * t), int(140 + 70 * t), int(225 - 25 * t))
            else:
                t = (y - HORIZON) / (SCENE_H - HORIZON)
                px[x, y] = (int(70 - 20 * t), int(110 - 40 * t), int(50 - 20 * t))
    im.save(path)


def render(pp3, base, out):
    outp = os.path.join(HERE, out)
    for f in (outp, outp + ".tif"):
        if os.path.exists(f):
            os.remove(f)
    r = subprocess.run([CLI, "-Y", "-o", outp, "-d", "-p", pp3, "-t", "-b8", "-c", base],
                       capture_output=True, text=True)
    got = outp + ".tif" if os.path.exists(outp + ".tif") else outp
    if not os.path.exists(got):
        print(r.stderr[-2000:])
        sys.exit("render failed: " + out)
    return got


SPOT = """[Locallab]
Enabled=true
Selspot=0
Name_0=probe
Isvisible_0=true
Shape_0=ELI
SpotMethod_0=norm
ShapeMethod_0=IND
Loc_0=3000;3000;3000;3000
Centerx_0=0
Centery_0=0
Circrad_0=18
Transit_0=35
UseAIMask_0=true
Visiaimask_0=true
Expaimask_0=true
AIMaskThreshold_0=0.3
AIMaskFeather_0=35
AIMaskBlur_0=0
AIMaskOpacity_0=1
AIMaskRefineRadius_0=8
AIMaskRefineEps_0=0.01
AIMaskShapeOp_0=0
Expexpose_0=true
Visiexpose_0=true
Expcomp_0=3
"""


def main():
    scene = os.path.join(HERE, "ll_scene.png")
    make_scene(scene)
    base = Image.open(scene).convert("RGB").load()
    ok = True

    for name, cls, invert, wanted_above, wanted_below in [
        ("sky mask lifts the sky only", 2, "false", True, False),
        ("inverted lifts the ground only", 2, "true", False, True),
    ]:
        path = os.path.join(HERE, f"ll_{name.split()[0]}_{invert}.pp3")
        with open(path, "w", newline="\n") as f:
            f.write(SPOT + f"AIMaskClass_0={cls}\nAIMaskInvert_0={invert}\n")

        out = Image.open(render(path, scene, f"ll_out_{cls}_{invert}.tif")).convert("RGB").load()

        # Well clear of the boundary, so the feather is not under test here.
        above = max(out[256, y][1] - base[256, y][1] for y in (20, 80, 150))
        below = max(out[256, y][1] - base[256, y][1] for y in (260, 310, 360))
        good = (above > 20) == wanted_above and (below > 20) == wanted_below
        print(f"{'PASS' if good else 'FAIL'}  {name:34s} above = +{above}, below = +{below}")
        ok &= good

    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    sys.exit(0 if ok else 1)


main()
