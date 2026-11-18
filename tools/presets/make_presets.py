#!/usr/bin/env python3
"""Generate the bundled look library under rtdata/profiles from catalogue.py.

Three kinds of file are written:

  * film looks  - a [Steep Look] header plus a complete [Film Presets] section.
    The loader treats these as always-partial: they replace the Film Lab stage
    and nothing else (rtengine/procparams.cc, "Steep Look").
  * graded looks - plain partial pp3s that own Film Lab, colour grading and the
    Lab lightness curve, plus any finish sections the look uses.
  * layers      - plain partial pp3s that carry only texture / finish sections
    (Grain, Light Effects, PCVignette, ...) and no Film Lab section, so they
    stack on whatever look is already applied.

No kind may contain exposure compensation, white balance, crop, lens or
raw keys; lint() enforces that, and that every key exists in procparams.cc.

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

# Every Film Lab key a look owns, with the engine default. Exposure and
# PrintExposure are written as 0 because tools/film_look_probe.py requires it
# and a look must not place exposure.
FILM_DEFAULTS = [
    ("Enabled", "true"), ("Preset", "cinema_reveal_35"), ("ModelVersion", 6),
    ("Exposure", 0), ("PrintExposure", 0), ("PushPull", 0),
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
# Sections a layer file may carry. Nothing here places exposure or colour balance.
LAYER_SECTIONS = {"Grain", "Light Effects", "PCVignette", "Clarity", "Texture"}
# Sections a graded look may carry.
LOOK_SECTIONS = LAYER_SECTIONS | {"Film Presets", "Color Grading", "Luminance Curve"}
FORBIDDEN_WORDS = re.compile(
    r"\b(kodak|fuji\w*|portra|velvia|provia|ektar|ektachrome|kodachrome|tri-?x|ilford|hp5|delta|agfa|"
    r"cinestill|polaroid|leica|canon|nikon|sony|hasselblad|technicolor|arri|alexa)\b", re.I)


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def film_look_text(group, look):
    values = dict(FILM_DEFAULTS)
    values["Preset"] = look["stock"]
    for key, value in look.get("film", {}).items():
        if key not in FILM_KEYS:
            raise SystemExit("%s: unknown Film Lab key %s" % (look["name"], key))
        values[key] = value
    out = ["# %s" % look["note"],
           "# Film-only recipe: preserves exposure, white balance, geometry and local edits.",
           "[Steep Look]", "Version=1", "Kind=film", "Id=%s" % slug(group + " " + look["name"]),
           "Family=%s" % look["family"], "", "[Film Presets]"]
    out += ["%s=%s" % (key, values[key]) for key, _ in FILM_DEFAULTS]
    return "\n".join(out) + "\n"


def lab_curve(black=0.0, white=1.0, c=0.0, mid=0.0, **_):
    """A spline for the Lab lightness curve: end points, S-curve contrast and mid-tone lift."""
    points = []
    for x in (0, 0.06, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 0.94, 1):
        s = x - c * 0.075 * math.sin(2 * math.pi * x) + mid * 0.08 * math.sin(math.pi * x)
        y = min(max(black + (white - black) * s, 0.0), 1.0)
        points += ["%g" % x, "%.4f" % y]
    return "1;" + ";".join(points) + ";"


def graded_look_text(look):
    values = dict(FILM_DEFAULTS)
    values["Preset"] = look["stock"]
    for key, value in look.get("film", {}).items():
        if key not in FILM_KEYS:
            raise SystemExit("%s: unknown Film Lab key %s" % (look["name"], key))
        values[key] = value
    out = ["# %s" % look["note"],
           "# Graded look: owns Film Lab, colour grading and the Lab curve, plus the finish sections",
           "# listed below. It never sets exposure, white balance, crop or lens corrections.",
           "# Apply with the fill option off so the rest of the edit is kept.",
           "", "[Film Presets]"]
    out += ["%s=%s" % (key, values[key]) for key, _ in FILM_DEFAULTS if key not in ("Exposure", "PrintExposure")]
    cg = look.get("cg", {})
    lum = cg.get("lum", (0, 0, 0, 0))
    out += ["", "[Color Grading]", "Enabled=%s" % ("true" if cg else "false")]
    for zone, key, index in (("Shadows", "sh", 0), ("Midtones", "mid", 1), ("Highlights", "hi", 2), ("Global", "gl", 3)):
        hue, chroma = cg.get(key, (0, 0))
        if chroma > 40:
            raise SystemExit("%s: chroma out of range" % look["name"])
        out += ["%sHue=%g" % (zone, hue), "%sSat=%g" % (zone, round(chroma / 40.0, 4)), "%sLum=%g" % (zone, lum[index])]
    out += ["Blending=100", "Balance=%g" % cg.get("bal", 60)]
    lab = look.get("lab", {})
    out += ["", "[Luminance Curve]", "Enabled=%s" % ("true" if lab else "false"), "Brightness=0", "Contrast=0",
            "Chromaticity=%g" % lab.get("chroma", 0),
            "LCurve=%s" % (lab_curve(**lab) if set(lab) - {"chroma"} else "0;")]
    if "grain" in look:
        iso, strength, colour, size = (tuple(look["grain"]) + (0, 100))[:4] if len(look["grain"]) < 4 else look["grain"]
        out += ["", "[Grain]", "Enabled=true", "ISO=%d" % iso, "Strength=%d" % strength, "Scale=%d" % size, "Color=%d" % colour]
    if "fx" in look:
        fx = look["fx"]
        out += ["", "[Light Effects]", "Enabled=true", "Threshold=%d" % fx.get("threshold", 70),
                "Glow=%d" % fx.get("glow", 0), "GlowRadius=%d" % fx.get("radius", 25),
                "Halation=%d" % fx.get("halation", 0), "HalationSize=%d" % fx.get("size", 30),
                "HalationWarmth=%d" % fx.get("warmth", 70), "Flare=%d" % fx.get("flare", 0),
                "FlareLength=%d" % fx.get("length", 40), "FlareAngle=%d" % fx.get("angle", 0)]
    if "vig" in look:
        strength, feather, roundness = (tuple(look["vig"]) + (50,))[:3]
        out += ["", "[PCVignette]", "Enabled=true", "Strength=%g" % strength, "Feather=%d" % feather, "Roundness=%d" % roundness]
    if "clarity" in look:
        out += ["", "[Clarity]", "Enabled=true", "Radius=100", "Amount=%d" % look["clarity"]]
    if "texture" in look:
        out += ["", "[Texture]", "Enabled=true", "Radius=20", "Amount=%d" % look["texture"]]
    return "\n".join(out) + "\n"


def layer_text(look):
    out = ["# %s" % look["note"],
           "# Layer: carries only the sections below, so it stacks on the current look.",
           "# Apply with the fill option off; it never sets exposure or white balance."]
    for section, keys in look["layers"].items():
        if section not in LAYER_SECTIONS:
            raise SystemExit("%s: section %s is not a layer section" % (look["name"], section))
        out += ["", "[%s]" % section] + ["%s=%s" % kv for kv in keys.items()]
    return "\n".join(out) + "\n"


def text_for(group, look):
    if "layers" in look:
        return layer_text(look)
    if any(key in look for key in ("cg", "lab", "grain", "fx", "vig", "clarity", "texture")):
        return graded_look_text(look)
    return film_look_text(group, look)


def known_keys():
    """Every (section, key) the engine reads, scraped from procparams.cc."""
    text = (ROOT / "rtengine" / "procparams.cc").read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r'(?:assignFromKeyfile\(keyFile|keyFile\.has_key\(|keyFile\.get_\w+\(), ?"([^"]+)", "([^"]+)"', text)) | \
        set(re.findall(r'keyFile\.(?:has_key|get_\w+)\("([^"]+)", "([^"]+)"', text))


def lint(files):
    known = known_keys()
    problems = []
    ids = set()
    for path, text in files.items():
        name = path.stem
        if FORBIDDEN_WORDS.search(name):
            problems.append("%s: name uses a real brand" % path)
        section = None
        seen = {}
        for line in text.splitlines():
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                section = line.strip("[]")
                continue
            key, value = line.split("=", 1)
            seen.setdefault(section, {})[key] = value
            if section == "Steep Look":
                continue
            if (section, key) not in known:
                problems.append("%s: [%s] %s is not a key the engine reads" % (path, section, key))
        if "Steep Look" in seen:
            if set(seen) != {"Steep Look", "Film Presets"}:
                problems.append("%s: a film look may only carry Film Presets" % path)
            film = seen["Film Presets"]
            if film["Preset"] not in STOCKS:
                problems.append("%s: unknown stock %s" % (path, film["Preset"]))
            for key, allowed in ENUMS.items():
                if film[key] not in allowed:
                    problems.append("%s: %s=%s" % (path, key, film[key]))
            if float(film["Exposure"]) != 0 or float(film["PrintExposure"]) != 0:
                problems.append("%s: a look must not place exposure" % path)
            if seen["Steep Look"]["Id"] in ids:
                problems.append("%s: duplicate Id" % path)
            ids.add(seen["Steep Look"]["Id"])
        else:
            film = seen.get("Film Presets")
            if film:
                if film["Preset"] not in STOCKS:
                    problems.append("%s: unknown stock %s" % (path, film["Preset"]))
                for key, allowed in ENUMS.items():
                    if film[key] not in allowed:
                        problems.append("%s: %s=%s" % (path, key, film[key]))
                if "Exposure" in film or "PrintExposure" in film:
                    problems.append("%s: a look must not place exposure" % path)
            extra = set(seen) - (LOOK_SECTIONS if film else LAYER_SECTIONS)
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
            files[path] = text_for(group, look)
    problems = lint(files)
    for problem in problems:
        print(problem)
    if problems:
        raise SystemExit(1)
    if not args.check:
        for group in catalogue.GROUPS:
            folder = args.out / group
            folder.mkdir(parents=True, exist_ok=True)
            wanted = {p.name for p in files if p.parent == folder}
            for stale in folder.glob("*.pp3"):
                if stale.name not in wanted:
                    stale.unlink()
        for path, text in files.items():
            path.write_text(text, encoding="ascii", newline="\r\n")
    counts = {}
    for path in files:
        counts[path.parent.name] = counts.get(path.parent.name, 0) + 1
    for group, count in counts.items():
        print("%-28s %d" % (group, count))
    print("total", len(files))


if __name__ == "__main__":
    main()
