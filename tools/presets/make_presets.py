#!/usr/bin/env python3
"""Generate the bundled look library under rtdata/profiles from catalogue.py.

Two kinds of file are written:

  * looks   - partial pp3s that own a fixed set of "look" sections (Film Lab, colour grading,
    colour toning, the Lab curves, the Black-and-White switch, grain, glow, vignette, tilt-shift,
    graduated filter, soft light, dehaze, clarity, texture). Every look writes every one of those
    sections, switched off where it does not use them, so one look never leaves residue under
    the next.
  * layers  - partial pp3s that carry a single finish section and nothing else, so they stack
    on whatever look is applied.

No file may contain exposure compensation, white balance, crop, lens or raw keys; lint()
enforces that, and that every key is one procparams.cc reads.

    python tools/presets/make_presets.py            # write the files
    python tools/presets/make_presets.py --check    # lint only, write nothing
"""
import argparse
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROFILES = ROOT / "rtdata" / "profiles"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalogue  # noqa: E402

# Film Lab keys a look owns, with the engine default. Exposure and PrintExposure are never
# written: a look must not place exposure.
FILM_DEFAULTS = [
    ("Enabled", "true"), ("Preset", "cinema_reveal_35"), ("ModelVersion", 6), ("PushPull", 0),
    ("Process", "auto"), ("Output", "scan"), ("Format", "35mm"),
    ("Backing", "auto"), ("BWFilter", "none"), ("LensDesign", "slr"),
    ("Strength", 100), ("Contrast", 0), ("Saturation", 0), ("Warmth", 0), ("Tint", 0),
    ("Fade", 0), ("Rolloff", 0),
    ("ShadowHue", 220), ("ShadowTint", 0), ("MidHue", 35), ("MidTint", 0),
    ("HighlightHue", 40), ("HighlightTint", 0),
    ("Halation", 0), ("RedShift", 0), ("GreenShift", 0), ("BlueShift", 0),
    ("Grain", 0), ("Vibrance", 0), ("SkinProtection", 35), ("LayerCoupling", 0),
    ("GrainSize", 0), ("GrainClumping", 0), ("GrainColor", 0),
    ("HalationSize", 0), ("HalationThreshold", 0), ("LightSources", 0), ("FilmSharpness", 0),
    ("HalationColor", 0), ("Bloom", 0), ("OutputSoftness", 0),
]
FILM_KEYS = {k for k, _ in FILM_DEFAULTS}
STOCKS = {"heritage_gold", "porcelain_400", "vivid_chrome", "arctic", "sovereign", "golden_hour",
          "twilight_160", "nostalgia_200", "desert_chrome", "street_800", "cinematic_500t",
          "cinema_reveal_35", "fade_bloom", "ember", "silver_gelatin", "analog_dream"}
ENUMS = {"Process": {"auto", "c41", "e6", "ecn2", "bw"},
         "Output": {"scan", "labscan", "ra4", "projection", "cinema"},
         "Format": {"35mm", "120", "large"},
         "Backing": {"auto", "intact", "removed"},
         "BWFilter": {"none", "yellow", "orange", "red", "green", "blue", "ortho"},
         "LensDesign": {"slr", "rangefinder", "telecentric"}}
LAYER_SECTIONS = {"Grain", "Light Effects", "PCVignette", "Clarity", "Texture", "TiltShift", "Gradient"}
LOOK_SECTIONS = LAYER_SECTIONS | {"Film Presets", "Color Grading", "ColorToning", "Luminance Curve",
                                  "Black & White", "SoftLight", "Dehaze"}
ALLOWED_KEYS = {"Black & White": {"Enabled", "Method"},
                "ColorToning": {"Enabled", "Method", "LabGridALow", "LabGridBLow", "LabGridAHigh", "LabGridBHigh"},
                "Luminance Curve": {"Enabled", "Brightness", "Contrast", "Chromaticity", "LCurve", "aCurve", "bCurve"}}
FORBIDDEN_WORDS = re.compile(
    r"\b(kodak|fuji\w*|portra|velvia|provia|ektar|ektachrome|kodachrome|tri-?x|ilford|hp5|delta|agfa|"
    r"cinestill|polaroid|leica|canon|nikon|sony|hasselblad|technicolor|arri|alexa|aerochrome|lomo\w*)\b", re.I)


def lab_curve(black=0.0, white=1.0, c=0.0, mid=0.0, **_):
    """A spline for the Lab lightness curve: end points, S-curve contrast and mid-tone lift."""
    points = []
    for x in (0, 0.06, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 0.94, 1):
        s = x - c * 0.075 * math.sin(2 * math.pi * x) + mid * 0.08 * math.sin(math.pi * x)
        y = min(max(black + (white - black) * s, 0.0), 1.0)
        points += ["%g" % x, "%.4f" % y]
    return "1;" + ";".join(points) + ";"


def is_mono(look):
    return look.get("bw") or look["stock"] == "silver_gelatin" or look.get("film", {}).get("Process") == "bw"


def header(group, look):
    return ["[Steep Look]", "Version=2", "Kind=look",
            "Id=%s" % re.sub(r"[^a-z0-9]+", "-", (group + " " + look["name"]).lower()).strip("-"),
            "Family=%s" % look["family"]]


def look_text(group, look):
    out = ["# %s" % look["note"],
           "# Look: owns Film Lab, colour grading and toning, the Lab curves, the Black-and-White",
           "# switch and the finish tools below. It never sets exposure, white balance, crop or lens",
           "# corrections, and the rest of the edit is always kept.",
           ] + header(group, look) + ["", "[Film Presets]"]
    if look["stock"] is None:
        out += ["Enabled=false"]
    else:
        values = dict(FILM_DEFAULTS)
        values["Preset"] = look["stock"]
        for key, value in look.get("film", {}).items():
            if key not in FILM_KEYS:
                raise SystemExit("%s: unknown Film Lab key %s" % (look["name"], key))
            values[key] = value
        out += ["%s=%s" % (key, values[key]) for key, _ in FILM_DEFAULTS]

    cg = look.get("cg", {})
    lum = cg.get("lum", (0, 0, 0, 0))
    out += ["", "[Color Grading]", "Enabled=%s" % ("true" if cg else "false")]
    for zone, key, index in (("Shadows", "sh", 0), ("Midtones", "mid", 1), ("Highlights", "hi", 2), ("Global", "gl", 3)):
        hue, chroma = cg.get(key, (0, 0))
        out += ["%sHue=%g" % (zone, hue), "%sSat=%g" % (zone, round(min(chroma, 40) / 40.0, 4)), "%sLum=%g" % (zone, lum[index])]
    out += ["Blending=100", "Balance=%g" % cg.get("bal", 60)]

    # Colour toning Lab grid: (a, b) added at black and at white, in Lab units.
    ct = look.get("ct")
    out += ["", "[ColorToning]", "Enabled=%s" % ("true" if ct else "false"), "Method=LabGrid"]
    al, bl, ah, bh = ct or (0, 0, 0, 0)
    out += ["LabGridALow=%d" % round(al * 1000), "LabGridBLow=%d" % round(bl * 1000),
            "LabGridAHigh=%d" % round(ah * 1000), "LabGridBHigh=%d" % round(bh * 1000)]

    lab = look.get("lab", {})
    out += ["", "[Luminance Curve]", "Enabled=%s" % ("true" if lab else "false"), "Brightness=0", "Contrast=0",
            "Chromaticity=%g" % lab.get("chroma", 0),
            "LCurve=%s" % (lab["L"] if "L" in lab else lab_curve(**lab) if set(lab) & {"black", "white", "c", "mid"} else "0;"),
            "aCurve=%s" % lab.get("a", "0;"), "bCurve=%s" % lab.get("b", "0;")]

    # Film Lab carries the picture's earlier colour edits over to its own output channel by
    # channel, which puts colour back into a monochrome stock; the Black-and-White tool keeps the
    # result neutral, and the stock's lens filters still act.
    out += ["", "[Black & White]", "Enabled=%s" % ("true" if is_mono(look) else "false")]
    if is_mono(look):
        out += ["Method=Desaturation"]

    iso, strength, colour, size, style = (tuple(look.get("grain", (400, 0))) + (0, 100, "film"))[:5] \
        if len(look.get("grain", ())) < 5 else look["grain"]
    out += ["", "[Grain]", "Enabled=%s" % ("true" if strength else "false"), "ISO=%d" % iso,
            "Strength=%d" % strength, "Scale=%d" % size, "Color=%d" % colour, "Style=%s" % style]
    fx = look.get("fx", {})
    out += ["", "[Light Effects]", "Enabled=%s" % ("true" if fx else "false"), "Threshold=%d" % fx.get("threshold", 70),
            "Glow=%d" % fx.get("glow", 0), "GlowRadius=%d" % fx.get("radius", 25),
            "Halation=0", "HalationSize=30", "HalationWarmth=70", "Flare=%d" % fx.get("flare", 0),
            "FlareLength=%d" % fx.get("length", 40), "FlareAngle=%d" % fx.get("angle", 0)]
    strength, feather, roundness = (tuple(look.get("vig", (0, 50))) + (50,))[:3]
    out += ["", "[PCVignette]", "Enabled=%s" % ("true" if strength else "false"), "Strength=%g" % strength,
            "Feather=%d" % feather, "Roundness=%d" % roundness]
    amount, pos, width, angle = (tuple(look.get("tilt", (0,))) + (50, 20, 0))[:4]
    out += ["", "[TiltShift]", "Enabled=%s" % ("true" if amount else "false"), "Amount=%d" % amount,
            "FocusPos=%d" % pos, "FocusWidth=%d" % width, "Feather=50", "Angle=%d" % angle]
    strength, degree, feather, centre = (tuple(look.get("grad", (0,))) + (0, 40, 0))[:4]
    out += ["", "[Gradient]", "Enabled=%s" % ("true" if strength else "false"), "Degree=%g" % degree,
            "Feather=%d" % feather, "Strength=%g" % strength, "CenterX=0", "CenterY=%d" % centre]
    out += ["", "[SoftLight]", "Enabled=%s" % ("true" if look.get("soft") else "false"), "Strength=%d" % look.get("soft", 30)]
    out += ["", "[Dehaze]", "Enabled=%s" % ("true" if look.get("dehaze") else "false"), "Strength=%d" % look.get("dehaze", 50)]
    out += ["", "[Clarity]", "Enabled=%s" % ("true" if look.get("clarity") else "false"), "Radius=100", "Amount=%d" % look.get("clarity", 0)]
    out += ["", "[Texture]", "Enabled=%s" % ("true" if look.get("texture") else "false"), "Radius=20", "Amount=%d" % look.get("texture", 0)]
    return "\n".join(out) + "\n"


def layer_text(group, look):
    out = ["# %s" % look["note"],
           "# Layer: carries only the sections below, so it stacks on the current look.",
           "# It never sets exposure or white balance."] + header(group, look)
    for section, keys in look["layers"].items():
        if section not in LAYER_SECTIONS:
            raise SystemExit("%s: section %s is not a layer section" % (look["name"], section))
        out += ["", "[%s]" % section] + ["%s=%s" % kv for kv in keys.items()]
    return "\n".join(out) + "\n"


def known_keys():
    """Every (section, key) the engine reads, scraped from procparams.cc."""
    text = (ROOT / "rtengine" / "procparams.cc").read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r'assignFromKeyfile\(keyFile, ?"([^"]+)", "([^"]+)"', text)) | \
        set(re.findall(r'keyFile\.(?:has_key|get_\w+)\("([^"]+)", "([^"]+)"', text))


def lint(files):
    known = known_keys()
    problems = []
    for path, text in files.items():
        if FORBIDDEN_WORDS.search(path.stem):
            problems.append("%s: name uses a real brand" % path)
        section = None
        seen = {}
        for line in text.splitlines():
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                section = line.strip("[]")
                seen[section] = {}
                continue
            key, value = line.split("=", 1)
            seen[section][key] = value
            if section == "Steep Look":
                continue
            if (section, key) not in known:
                problems.append("%s: [%s] %s is not a key the engine reads" % (path, section, key))
        film = seen.get("Film Presets")
        if film and film.get("Enabled") == "true":
            if film["Preset"] not in STOCKS:
                problems.append("%s: unknown stock %s" % (path, film["Preset"]))
            for key, allowed in ENUMS.items():
                if film[key] not in allowed:
                    problems.append("%s: %s=%s" % (path, key, film[key]))
        if film and ("Exposure" in film or "PrintExposure" in film):
            problems.append("%s: a look must not place exposure" % path)
        for name, allowed in ALLOWED_KEYS.items():
            if not set(seen.get(name, {})) <= allowed:
                problems.append("%s: [%s] carries keys a look may not set" % (path, name))
        if seen.get("Steep Look", {}).get("Version") != "2" or seen["Steep Look"].get("Kind") != "look":
            problems.append("%s: missing the look header" % path)
        extra = set(seen) - {"Steep Look"} - (LOOK_SECTIONS if film else LAYER_SECTIONS)
        if extra:
            problems.append("%s: forbidden sections %s" % (path, sorted(extra)))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--out", type=Path, default=PROFILES)
    args = parser.parse_args()
    files = {}
    for group, looks in catalogue.GROUPS.items():
        for look in looks:
            path = args.out / group / (look["name"] + ".pp3")
            if path in files:
                raise SystemExit("duplicate name %s" % path)
            files[path] = layer_text(group, look) if "layers" in look else look_text(group, look)
    problems = lint(files)
    for problem in problems:
        print(problem)
    if problems:
        raise SystemExit(1)
    if not args.check:
        for group in list(catalogue.GROUPS) + list(catalogue.RETIRED_GROUPS):
            folder = args.out / group
            wanted = {p.name for p in files if p.parent == folder}
            for stale in folder.glob("*.pp3"):
                if stale.name not in wanted:
                    stale.unlink()
            if folder.is_dir() and not wanted and not any(folder.iterdir()):
                folder.rmdir()
        for path, text in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="ascii", newline="\r\n")
    counts = {}
    for path in files:
        counts[path.parent.name] = counts.get(path.parent.name, 0) + 1
    for group, count in counts.items():
        print("%-28s %d" % (group, count))
    print("total", len(files))


if __name__ == "__main__":
    main()
