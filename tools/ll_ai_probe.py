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


def make_scene(path, w=SCENE_W, h=SCENE_H):
    horizon = int(h * 0.55)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if y < horizon:
                t = y / horizon
                px[x, y] = (int(90 + 90 * t), int(140 + 70 * t), int(225 - 25 * t))
            else:
                t = (y - horizon) / (h - horizon)
                px[x, y] = (int(70 - 20 * t), int(110 - 40 * t), int(50 - 20 * t))
    im.save(path)
    return horizon


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

    # Every check above is on a landscape frame, and so was every double
    # exposure check -- an orientation fault in the mask geometry would have
    # gone unseen by all of them, on a portrait photo of exactly the kind
    # people put subjects in.
    tall = os.path.join(HERE, "ll_scene_tall.png")
    tall_horizon = make_scene(tall, 384, 512)
    tall_base = Image.open(tall).convert("RGB").load()

    path = os.path.join(HERE, "ll_tall.pp3")
    with open(path, "w", newline="\n") as f:
        f.write(SPOT + "AIMaskClass_0=2\nAIMaskInvert_0=false\n")

    tall_out = Image.open(render(path, tall, "ll_out_tall.tif")).convert("RGB").load()
    above = max(tall_out[192, y][1] - tall_base[192, y][1] for y in (30, 120, 220))
    below = max(tall_out[192, y][1] - tall_base[192, y][1] for y in (340, 420, 490))
    good = above > 20 and below * 5 < above
    print(f"{'PASS' if good else 'FAIL'}  {'portrait frame lines up too':34s} above = +{above}, below = +{below}")
    ok &= good

    # Dodge/burn multiplied the AI mask by the spot's transition, and that
    # transition is an ellipse inscribed in the AI mask's own bounding box.
    # calcTransition writes localFactor only inside the transition band, so
    # outside it the caller's 1.f stood: full strength within the ellipse, full
    # strength beyond it, and nothing in the band between -- not a soft edge but
    # a ring of dead effect cutting across the selection. One sample cannot see
    # that; the row has to be scanned.
    db = SPOT.replace("Expexpose_0=true\nVisiexpose_0=true\nExpcomp_0=3\n", "")
    row = None

    for tag, amount in (("dboff", 0), ("dbon", 80)):
        path = os.path.join(HERE, "ll_%s.pp3" % tag)

        with open(path, "w", newline="\n") as f:
            f.write(db + "AIMaskClass_0=2\nAIMaskInvert_0=false\n"
                    "DodgeBurn_0=%d\nDodgeBurnTones_0=0\n" % amount)

        img = Image.open(render(path, scene, "ll_out_%s.tif" % tag)).convert("RGB").load()

        if row is None:
            row = [img[x, 60][1] for x in range(0, SCENE_W, 4)]
        else:
            row = [img[x, 60][1] - was for x, was in zip(range(0, SCENE_W, 4), row)]

    good = min(row) > 20 and max(row) - min(row) <= 6
    print(f"{'PASS' if good else 'FAIL'}  {'dodge/burn is even across it':34s} "
          f"lift = {min(row)}..{max(row)} across the row")
    ok &= good

    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    sys.exit(0 if ok else 1)


main()
