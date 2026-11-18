"""The bundled look library, as data. make_presets.py turns it into pp3 files.

A look is:  look(group, name, family, note, stock, film..., cg=, ct=, lab=, grain=, fx=, vig=, ...)

  stock       a Film Lab stock (engine v6), or None to switch Film Lab off for looks that the
              film stage would undo (duotones, false colour)
  film keys   Film Lab controls as keyword arguments (Contrast=10, Output="cinema", ...)
  cg          colour grading: dict(sh=(hue, chroma), mid=, hi=, gl=, lum=(sh, mid, hi, gl), bal=)
              hue is the CIE Lab hue angle in degrees, chroma is in Lab units before GAIN
  ct          colour toning: (a, b at black, a, b at white) in thousands of grid units; with
              bw=True this is a duotone
  lab         Lab curves: dict(black=, white=, c=, mid=, chroma=, L=, a=, b=)
              black / white  where pure black and white land; c  S-curve contrast; mid  mid-tone
              lift; chroma  -100..100; L / a / b  a raw curve string (solarising, false colour)
  bw          True switches the Black-and-White tool on (mono stocks do this themselves)
  grain       (iso, strength) or (iso, strength, colour) or (iso, strength, colour, size)
  fx          Light Effects: dict(glow=, radius=, flare=, length=, angle=, threshold=)
  vig         (strength in stops, feather) or (strength, feather, roundness)
  tilt        tilt-shift blur: (amount, focus position %, focus width %, angle)
  grad        graduated filter: (strength in stops, angle, feather, centre %)
  soft / dehaze / clarity / texture   amounts

Names describe the picture. Never name a look after a real film, camera, title or director.
"""

# Lab hue angles
RED, ORANGE, AMBER, YELLOW, LIME, GREEN, SEA = 28, 60, 74, 95, 125, 150, 172
TEAL, CYAN, BLUE, VIOLET, PURPLE, MAGENTA, PINK = 205, 222, 268, 295, 315, 340, 8

GROUPS = {}
RETIRED_GROUPS = []

DEFAULT_STRENGTH = {"heritage_gold": 65, "golden_hour": 70, "twilight_160": 70, "ember": 55, "porcelain_400": 85,
                    "nostalgia_200": 85, "street_800": 80, "fade_bloom": 75, "analog_dream": 75,
                    "vivid_chrome": 50, "desert_chrome": 55, "arctic": 65, "sovereign": 85}


# House strength. The first pass of this library was tuned to be safe and came out timid; these
# gains push every look's grade, curve and finish. A look can opt out with gain=1.
GAIN = dict(chroma=1.8, contrast=1.6, soften=1.3, mid=1.3, sat=1.4, warmth=1.3, vig=1.25, clarity=1.4, black=1.25)


def look(group, name, family, note, stock="cinema_reveal_35", **kw):
    gain = kw.pop("gain", None)
    g = {k: (1 + (v - 1) * gain if gain is not None else v) for k, v in GAIN.items()}
    entry = {"name": name, "family": family, "note": note, "stock": stock}
    for key in ("cg", "ct", "lab", "grain", "fx", "vig", "clarity", "texture", "tilt", "grad", "soft", "dehaze", "bw"):
        if key in kw:
            entry[key] = kw.pop(key)
    if "cg" in entry:
        cg = dict(entry["cg"])
        for zone in ("sh", "mid", "hi", "gl"):
            if zone in cg:
                cg[zone] = (cg[zone][0], round(min(cg[zone][1] * g["chroma"], 40), 2))
        entry["cg"] = cg
    if "lab" in entry:
        lab = dict(entry["lab"])
        if "c" in lab:
            lab["c"] = round(lab["c"] * (g["contrast"] if lab["c"] > 0 else g["soften"]), 3)
        if "mid" in lab:
            lab["mid"] = round(lab["mid"] * g["mid"], 3)
        if "black" in lab:
            lab["black"] = round(lab["black"] * g["black"], 3)
        entry["lab"] = lab
    if "vig" in entry:
        entry["vig"] = (round(entry["vig"][0] * g["vig"], 2),) + tuple(entry["vig"][1:])
    if "clarity" in entry:
        entry["clarity"] = int(max(-80, min(80, round(entry["clarity"] * g["clarity"]))))
    if stock is not None:
        mono = stock == "silver_gelatin" or kw.get("Process") == "bw"
        if "Saturation" in kw and not mono:
            kw["Saturation"] = int(max(-100, min(100, round(kw["Saturation"] * g["sat"]))))
        if "Warmth" in kw:
            kw["Warmth"] = int(max(-100, min(100, round(kw["Warmth"] * g["warmth"]))))
        # Stocks that run hot on bright skin or dark shadows are not used at full strength unless
        # a look asks for it, and the print outputs add contrast of their own.
        kw.setdefault("Strength", DEFAULT_STRENGTH.get(stock, 100))
        if kw.get("Output") in ("ra4", "labscan", "projection"):
            kw.setdefault("Contrast", -12)
    entry["film"] = kw
    GROUPS.setdefault(group, []).append(entry)


def layer(group, name, note, **sections):
    GROUPS.setdefault(group, []).append({"name": name, "family": "layer", "note": note, "layers": sections})


# --------------------------------------------------------------------------------------------
# Cinema
# --------------------------------------------------------------------------------------------
G = "Cinema"
# teal and orange, three strengths and a dark sibling
look(G, "Quiet Teal", "teal-orange", "A restrained complementary grade: a breath of teal under the shadows, warmth held in the highlights.",
     Output="cinema", Saturation=-5, cg=dict(sh=(TEAL, 6), hi=(ORANGE, 4)))
look(G, "Teal and Amber", "teal-orange", "The working teal-and-orange: clear separation between cool shadows and amber light, skin left alone.",
     Output="cinema", cg=dict(sh=(TEAL, 11), hi=(AMBER, 8), bal=80), lab=dict(c=0.25))
look(G, "Blockbuster Teal", "teal-orange", "The loud version: deep teal shadows, hot amber highlights, more contrast and colour.",
     Output="cinema", Saturation=8, cg=dict(sh=(TEAL, 18), mid=(ORANGE, 3), hi=(AMBER, 9), bal=100), lab=dict(c=0.5, white=0.97))
look(G, "Teal Noir", "teal-orange", "Teal pushed into dense, desaturated shadows for night exteriors and thrillers.",
     Output="cinema", Saturation=-25, cg=dict(sh=(TEAL, 10), hi=(ORANGE, 4), lum=(-4, 0, 0, 0)), lab=dict(c=0.6))
# bleach bypass
look(G, "Bleach Bypass Light", "bleach", "Partial silver retention: colour thinned by a third, contrast up, blacks firm.",
     Saturation=-35, Contrast=20, lab=dict(c=0.35, chroma=-8))
look(G, "Bleach Bypass Full", "bleach", "Full skip-bleach: metallic, near-monochrome colour with hard contrast.",
     Saturation=-70, Contrast=40, cg=dict(gl=(CYAN, 2)), lab=dict(c=0.7, chroma=-22))
look(G, "Silver Retention Warm", "bleach", "Bleach bypass printed warm: bronze skin and tobacco highlights over thin colour.",
     Saturation=-55, Contrast=30, Warmth=15, cg=dict(hi=(AMBER, 4)), lab=dict(c=0.5, chroma=-12))
look(G, "Skip Bleach Steel", "bleach", "Bleach bypass printed cold: gunmetal shadows, colour almost gone.",
     Saturation=-60, Contrast=35, cg=dict(sh=(BLUE, 5), gl=(BLUE, 3)), lab=dict(c=0.55, chroma=-15))
# night
look(G, "Day for Night", "night", "Sunlit footage printed down and blue for night: lowered highlights, cool cast, thin colour.",
     "cinematic_500t", Warmth=-60, Saturation=-35, cg=dict(gl=(BLUE, 7), lum=(0, -4, -8, 0)), lab=dict(white=0.92, c=0.2))
look(G, "Moonlight Silver", "night", "Day for night taken further: silver-cyan, nearly colourless, highlights held well down.",
     "cinematic_500t", Warmth=-40, Saturation=-65, cg=dict(gl=(CYAN, 6), lum=(0, -5, -10, 0)), lab=dict(white=0.88, c=0.3))
look(G, "Tungsten Night", "night", "Tungsten-balanced motion stock at night: cool blue shadows, amber practicals, a red rim on lamps.",
     "cinematic_500t", Halation=20, cg=dict(sh=(BLUE, 4), hi=(AMBER, 4)))
look(G, "Neon Halation", "night", "Backing removed: every sign and lamp wears a red halo, violet in the shadows.",
     "cinematic_500t", Backing="removed", Halation=45, HalationSize=20, Saturation=12, cg=dict(sh=(PURPLE, 5)))
look(G, "Sodium Street", "night", "Streets under sodium lamps: orange light, green-black shadows.",
     "twilight_160", Warmth=15, Saturation=-10, cg=dict(sh=(SEA, 7), hi=(AMBER, 7)), lab=dict(white=0.97, c=0.2))
look(G, "Night Bazaar", "night", "Warm, saturated night colour with violet shadows and glowing lights.",
     "heritage_gold", Backing="removed", Saturation=12, Halation=30, Contrast=-10, cg=dict(sh=(PURPLE, 7), hi=(ORANGE, 5)))
# anamorphic era and palettes
look(G, "Seventies Anamorphic", "widescreen", "Seventies widescreen: warm, a little thin, yellowed highlights, soft glow and visible grain.",
     "twilight_160", Output="cinema", Warmth=6, Saturation=-15, cg=dict(sh=(GREEN, 3), hi=(YELLOW, 5)),
     lab=dict(black=0.04, c=-0.15), fx=dict(glow=15), grain=(400, 25))
look(G, "Blue Streak Widescreen", "widescreen", "Cool modern anamorphic: blue shadows and a horizontal streak across bright lights.",
     cg=dict(sh=(BLUE, 11), hi=(CYAN, 3), bal=80), lab=dict(c=0.3), fx=dict(flare=30, length=60, angle=0))
look(G, "Desert Epic", "widescreen", "Wide desert vistas: golden sand, warm shadows, strong but unclipped colour.",
     "desert_chrome", Strength=60, Warmth=12, Contrast=-10, cg=dict(sh=(ORANGE, 3), hi=(AMBER, 7)), lab=dict(c=0.2))
look(G, "Pastel Musical", "widescreen", "Soft pastel colour: lifted blacks, blush highlights, cyan in the shadows, gentle glow.",
     "porcelain_400", Saturation=5, cg=dict(sh=(CYAN, 6), hi=(PINK, 3.5)), lab=dict(black=0.03, white=0.97, c=-0.3, mid=0.1), fx=dict(glow=20))
# print and palette pieces
look(G, "Release Print Rich", "print", "A dense theatrical print: deep blacks, saturated mid-tones, slightly cold whites.",
     Output="cinema", Contrast=15, Saturation=8, lab=dict(c=0.3))
look(G, "Release Print Soft", "print", "A low-contrast print with long highlight roll-off, made for further grading.",
     Output="cinema", Contrast=-15, Rolloff=40, lab=dict(c=-0.2))
look(G, "Colour Noir", "palette", "Noir in colour: thin saturation, green-black shadows, corners falling away.",
     "street_800", Saturation=-30, Contrast=5, cg=dict(sh=(SEA, 9)), vig=(0.25, 55))
look(G, "Fluorescent Thriller", "palette", "Sick green office light: a green cast through the mids over teal shadows.",
     Tint=-35, Saturation=-15, cg=dict(sh=(TEAL, 5), gl=(LIME, 5)))
look(G, "Cold War Grey", "palette", "Drained, cold and grey: colour halved, a little blue everywhere, soft blacks.",
     Saturation=-50, Warmth=-20, cg=dict(gl=(BLUE, 2)), lab=dict(black=0.03))
look(G, "Sci-Fi Cyan", "palette", "Clinical cyan: cool reversal stock with cyan shadows and clean contrast.",
     "arctic", Strength=70, Saturation=-10, cg=dict(sh=(CYAN, 10), hi=(CYAN, 3)), lab=dict(c=0.35))
look(G, "Magenta Dusk", "palette", "Purple hour: violet shadows under orange highlights.",
     "ember", Strength=55, Contrast=-15, cg=dict(sh=(PURPLE, 10), hi=(ORANGE, 6)))
look(G, "Western Dust", "palette", "Dust and heat: yellow-brown, a third of the colour gone, soft blacks, grain.",
     "heritage_gold", Saturation=-30, Warmth=14, Contrast=-10, cg=dict(gl=(YELLOW, 5)), lab=dict(black=0.04), grain=(400, 30))
look(G, "Cross Process Acid", "cross-process", "Slide film in negative chemistry: yellow-green highlights, green shadows, hard colour.",
     "vivid_chrome", Process="c41", Strength=75, Contrast=-25, cg=dict(sh=(GREEN, 9), hi=(YELLOW, 9), bal=90), lab=dict(chroma=10))
look(G, "Cross Process Rose", "cross-process", "Negative film in slide chemistry: pink highlights over blue shadows.",
     "heritage_gold", Process="e6", Strength=85, Contrast=-25, cg=dict(sh=(BLUE, 9), hi=(PINK, 8), bal=90))
# --------------------------------------------------------------------------------------------
# Decades
# --------------------------------------------------------------------------------------------
G = "Decades"
look(G, "1940s Three-Strip", "period", "Dye-transfer colour: separated, saturated primaries with rich blacks.",
     "vivid_chrome", Strength=70, LayerCoupling=80, Saturation=10, Contrast=-20, lab=dict(c=0.3, chroma=18))
look(G, "1950s Travel Slide", "period", "A projected holiday slide: warm, clean, strong blue skies and red details.",
     "desert_chrome", Strength=80, Contrast=-20, Warmth=12, Saturation=10, cg=dict(sh=(BLUE, 4), hi=(YELLOW, 5)))
look(G, "1950s Studio Glamour", "period", "A studio colour portrait print: warm skin, soft contrast, a glow on the highlights.",
     "porcelain_400", Output="ra4", Warmth=12, lab=dict(c=-0.2, mid=0.2), fx=dict(glow=25))
look(G, "1960s Colour Supplement", "period", "Magazine colour of the sixties: bright, slightly cyan shadows, minilab snap.",
     "sovereign", Output="labscan", Saturation=22, cg=dict(sh=(CYAN, 8), hi=(YELLOW, 3)), lab=dict(c=0.3))
look(G, "1960s Faded Slide", "period", "A slide that sat in a box: dyes shifted to magenta, blacks gone grey.",
     "fade_bloom", Strength=80, cg=dict(gl=(MAGENTA, 4)), lab=dict(black=0.05, white=0.96))
look(G, "1970s Drugstore Print", "period", "A corner-shop print: warm, yellow highlights, green in the shadows, soft blacks, grain.",
     "heritage_gold", Output="ra4", Warmth=8, Contrast=-10, cg=dict(sh=(GREEN, 4), hi=(YELLOW, 6)), lab=dict(black=0.05), grain=(400, 30))
look(G, "1970s New Colour", "period", "Seventies colour documentary: muted, pale, slightly yellow, nothing dramatic.",
     "nostalgia_200", cg=dict(hi=(YELLOW, 3)), lab=dict(black=0.03, c=-0.2))
look(G, "1970s Brown Study", "period", "Brown and orange interiors: thin colour pulled toward tobacco.",
     "twilight_160", Saturation=-25, Warmth=12, cg=dict(gl=(ORANGE, 3.5)), lab=dict(black=0.04))
look(G, "1980s Flash Print", "period", "Eighties party print: hard contrast, saturated colour, blue shadows, dark corners.",
     "heritage_gold", Output="labscan", Contrast=6, Saturation=15, cg=dict(sh=(BLUE, 6)), vig=(0.25, 50))
look(G, "1980s Neon Arcade", "period", "Neon and video: blue shadows, magenta highlights, halos on every light.",
     "cinematic_500t", Backing="removed", Halation=30, Saturation=20, cg=dict(sh=(BLUE, 6), hi=(MAGENTA, 5)))
look(G, "1990s Disposable", "period", "A one-use camera: green shadows, grey blacks, heavy corners and coarse colour grain.",
     "street_800", Output="labscan", Saturation=5, cg=dict(sh=(GREEN, 5)), lab=dict(black=0.04), vig=(0.4, 45), grain=(800, 45, 30))
look(G, "1990s Grunge Cross", "period", "Nineties cross-processing: green overall, yellow highlights, hard edges.",
     "sovereign", Process="e6", Contrast=-5, cg=dict(hi=(YELLOW, 4), gl=(LIME, 5)))
look(G, "2000s Early Digital", "period", "An early digital compact: crisp, cool, colour a notch too strong.",
     "sovereign", Strength=35, Saturation=12, Contrast=10, FilmSharpness=100, cg=dict(gl=(BLUE, 2)), texture=20)
look(G, "2000s Minilab", "period", "A digital minilab print of the two-thousands: bright, slightly red, glossy.",
     "heritage_gold", Output="labscan", Strength=90, Saturation=18, cg=dict(hi=(RED, 5)), lab=dict(c=0.3, white=0.96), soft=15)
look(G, "2010s Matte Fade", "period", "The blog-era fade: grey blacks, soft contrast, teal shadows, thin colour.",
     "sovereign", Saturation=-15, cg=dict(sh=(TEAL, 6), hi=(ORANGE, 3)), lab=dict(black=0.09, white=0.97, c=-0.2))

# --------------------------------------------------------------------------------------------
# Editorial
# --------------------------------------------------------------------------------------------
G = "Editorial"
look(G, "Wire Service", "neutral", "Agency delivery: neutral colour, firm contrast, a little mid-tone bite.",
     "sovereign", Contrast=10, Saturation=-10, lab=dict(c=0.4), clarity=18)
look(G, "Magazine Rich", "neutral", "A glossy feature spread: paper-print density with a little more colour and contrast.",
     Output="ra4", Contrast=0, Saturation=22, lab=dict(c=0.4, chroma=10), soft=20)
look(G, "Long Read Muted", "long-read", "Muted long-form colour: a third of the colour gone, soft blacks, a trace of warmth.",
     "sovereign", Saturation=-30, cg=dict(gl=(ORANGE, 1.5)), lab=dict(black=0.03))
look(G, "Long Read Cool", "long-read", "The cool sibling: muted colour with blue in the shadows.",
     "sovereign", Saturation=-30, Warmth=-18, cg=dict(sh=(BLUE, 7), gl=(BLUE, 1.5)), lab=dict(black=0.03))
look(G, "Long Read Warm", "long-read", "The warm sibling: muted colour with amber in the highlights.",
     "sovereign", Saturation=-28, Warmth=20, cg=dict(hi=(AMBER, 7), gl=(AMBER, 2)), lab=dict(black=0.03))
look(G, "Reportage Colour", "reportage", "Fast colour negative for the street: a little grit, slightly thin colour.",
     "street_800", Strength=100, Saturation=-10, cg=dict(sh=(SEA, 4)), lab=dict(c=0.3), grain=(800, 35))
look(G, "Reportage Pushed", "reportage", "The same stock pushed: more contrast, coarser grain.",
     "street_800", PushPull=0.3, Contrast=5, grain=(1600, 50))
look(G, "Field Notes Green", "reportage", "Pale, slightly green field colour for land and rural work.",
     "nostalgia_200", cg=dict(gl=(LIME, 2)))
look(G, "Op-Ed Portrait", "portrait", "A serious portrait for the opinion page: restrained colour, firm contrast, quiet corners.",
     "porcelain_400", Saturation=-18, Contrast=10, vig=(0.25, 60))
look(G, "Photo Essay Soft", "portrait", "Gentle essay colour: soft contrast and grey-lifted blacks.",
     "porcelain_400", lab=dict(black=0.04, c=-0.3))
look(G, "Flash on Camera", "flash", "Direct flash: hard contrast, hot colour, blue shadows, light falling off to dark corners.",
     "heritage_gold", Output="labscan", Contrast=8, Rolloff=-40, Saturation=12, cg=dict(sh=(BLUE, 5)), vig=(0.4, 35), clarity=10)
look(G, "Tabloid Punch", "flash", "Loud front-page colour: slide stock at part strength with extra bite.",
     "vivid_chrome", Strength=45, Saturation=10, clarity=15)
look(G, "Sunday Supplement", "neutral", "Weekend magazine warmth: golden stock on paper.",
     "golden_hour", Output="ra4", Strength=100, Saturation=12, cg=dict(hi=(AMBER, 5)), lab=dict(c=0.25, white=0.96))
# --------------------------------------------------------------------------------------------
# Monochrome
# --------------------------------------------------------------------------------------------
G = "Monochrome"
S = "silver_gelatin"
# The silver stock is steep: at its own contrast it blocks the shadows of a dark picture, so the
# family sits around Contrast -30 and the hard looks are hard relative to that.
look(G, "Silver Classic", "silver", "A straight silver print: full range, neutral, gently toned.",
     S, Contrast=-30)
look(G, "Silver Soft", "silver", "A long-scale print on soft paper: open shadows, gentle highlights.",
     S, Contrast=-55, lab=dict(c=-0.15))
look(G, "Silver Hard", "silver", "A hard-paper print: strong blacks, bright whites, little in between.",
     S, Contrast=-18, lab=dict(c=0.2))
look(G, "Matte Mono", "silver", "Grey-black matte print: lifted blacks, soft contrast.",
     S, Contrast=-35, lab=dict(black=0.09))
# documentary
look(G, "Documentary Hard", "documentary", "Hard documentary black-and-white: orange filter, gritty and sharp.",
     S, Contrast=-18, BWFilter="orange", grain=(800, 50), clarity=12)
look(G, "Street Pushed", "documentary", "Fast film pushed for the street: coarse grain and heavy shadows.",
     S, PushPull=0.15, Contrast=-25, grain=(1600, 60))
look(G, "Newsprint Mono", "documentary", "A picture as the newspaper printed it: grey blacks, dull whites, coarse grain.",
     S, Contrast=-15, lab=dict(black=0.12, white=0.86, c=0.3), grain=(1600, 55))
look(G, "Wire Photo Mono", "documentary", "Agency black-and-white: crisp and firm, made to survive reproduction.",
     S, Contrast=-24, FilmSharpness=60, clarity=15)
look(G, "Flash Mono", "documentary", "Direct flash at night: bright foreground, falling to dark corners.",
     S, Contrast=-26, Rolloff=-40, vig=(0.4, 35))
# lens filters
look(G, "Yellow Filter Skies", "filter", "Yellow filter: skies a step darker, clouds separated, skin natural.",
     S, BWFilter="yellow", Contrast=-30)
look(G, "Orange Filter Landscape", "filter", "Orange filter: dark skies, bright stone and foliage edges.",
     S, BWFilter="orange", Contrast=-30)
look(G, "Red Filter Drama", "filter", "Red filter: near-black skies, glowing skin and brick.",
     S, BWFilter="red", Contrast=-32, vig=(0.25, 55))
look(G, "Green Filter Foliage", "filter", "Green filter: leaves and grass lifted, lips and brick darker.",
     S, BWFilter="green", Contrast=-30)
look(G, "Blue Filter Haze", "filter", "Blue filter: pale skies, heavy haze, weathered skin.",
     S, BWFilter="blue", Contrast=-40)
look(G, "Ortho Portrait", "filter", "Film blind to red: dark lips, strong skin texture, an early-studio look.",
     S, BWFilter="ortho", Contrast=-35)
look(G, "Deep Red Glow", "filter", "Red filter with glowing highlights and lifted mid-tones for a dreamlike landscape.",
     S, BWFilter="red", Contrast=-40, lab=dict(mid=0.4), fx=dict(glow=30))
# key
look(G, "High Key Mono", "key", "Bright and open: mid-tones lifted, contrast lowered.",
     S, Contrast=-50, lab=dict(mid=0.6, c=-0.2))
look(G, "Low Key Mono", "key", "Dark and enclosed: mid-tones dropped, corners shaded.",
     S, Contrast=-35, lab=dict(mid=-0.35), vig=(0.3, 50))
# print character
# Toned and duotone prints live in the Abstract group: they need Film Lab off (see there).
look(G, "Platinum Print", "print", "The platinum print's scale: very long, soft, grey-black shadows and quiet whites.",
     S, Format="large", Contrast=-55, lab=dict(black=0.07, white=0.92))
look(G, "Lith Print", "print", "Lith development: gritty, blocked blacks against delicate highlights, coarse grain.",
     S, Contrast=-25, lab=dict(white=0.95, c=0.5, mid=0.5), grain=(3200, 70))
# --------------------------------------------------------------------------------------------
# Texture
# --------------------------------------------------------------------------------------------
G = "Texture"
look(G, "Halation Soft", "halation", "A faint red rim around lamps and bright edges; otherwise neutral.",
     Backing="removed", Halation=-30)
look(G, "Halation Heavy", "halation", "Backing removed and pushed: wide red halos round every light.",
     "cinematic_500t", Backing="removed", Halation=40, HalationSize=25)
look(G, "Highlight Bloom", "diffusion", "Highlights spread into their surroundings, as through a diffusion filter.",
     "sovereign", Strength=60, lab=dict(black=0.03), fx=dict(glow=75, radius=45, threshold=55))
look(G, "Dream Diffusion", "diffusion", "Soft, glowing and low in contrast: a dream sequence.",
     "analog_dream", lab=dict(black=0.03, c=-0.3), fx=dict(glow=30))
look(G, "Pinhole", "diffusion", "Soft everywhere, dark at the edges, colour thinned.",
     "sovereign", Saturation=-10, fx=dict(glow=35), vig=(0.5, 40), clarity=-30)
look(G, "Matte Blacks", "matte", "Blacks lifted to dark grey; everything else left alone.",
     "sovereign", Strength=60, lab=dict(black=0.07, mid=-0.15))
look(G, "Deep Matte", "matte", "Heavy matte: grey blacks, dulled whites, thinner colour.",
     "sovereign", Strength=60, Saturation=-10, lab=dict(black=0.12, white=0.95, mid=-0.25))
look(G, "Print Density", "density", "The weight of a paper print: firm blacks and dense lower mid-tones.",
     Output="ra4", cg=dict(lum=(-4, 0, 0, 0)), lab=dict(c=0.4))
look(G, "Gritty Acutance", "grit", "High-acutance grit: hard edges, strong micro-contrast, coarse grain.",
     "street_800", Contrast=-10, clarity=30, texture=40, grain=(1600, 55))
look(G, "Pushed Two Stops", "grit", "Fast film pushed hard: contrast, blocked shadows and very coarse grain.",
     "street_800", PushPull=0.3, Contrast=10, grain=(3200, 70))
look(G, "Expired Roll", "decay", "Out-of-date film: magenta drift, grey blacks, mottled colour grain.",
     "fade_bloom", lab=dict(black=0.05), grain=(800, 45, 50))
look(G, "Toy Camera", "decay", "Plastic lens: heavy corners, glow, bright colour, hard contrast.",
     "heritage_gold", Saturation=15, lab=dict(c=0.2), fx=dict(glow=20), vig=(0.6, 30))
# --------------------------------------------------------------------------------------------
# Working - People
# --------------------------------------------------------------------------------------------
G = "Working - People"
look(G, "Portrait Clean", "portrait", "A clean portrait base: soft portrait stock at part strength, skin held true.",
     "porcelain_400", Strength=100, SkinProtection=80, cg=dict(mid=(ORANGE, 2)), lab=dict(c=0.2, white=0.97))
look(G, "Portrait Warm Skin", "portrait", "Warmer skin without touching white balance.",
     "porcelain_400", Warmth=15, SkinProtection=60, cg=dict(mid=(ORANGE, 2)))
look(G, "Portrait Cool Studio", "portrait", "Cool, controlled studio colour.",
     "sovereign", Strength=60, Warmth=-25, Saturation=-8, cg=dict(gl=(BLUE, 2)))
look(G, "Portrait Window Soft", "portrait", "Window light: soft contrast, lifted mid-tones, a little glow.",
     "porcelain_400", Contrast=-20, lab=dict(c=-0.2, mid=0.2), fx=dict(glow=12))
look(G, "Fashion Crisp", "portrait", "Fashion editorial: paper-print contrast, colour held back, hard detail.",
     Output="ra4", Contrast=0, Saturation=-8, clarity=12)
look(G, "Wedding Light and Airy", "wedding", "Light and airy: soft contrast, raised mid-tones, pale blacks, gentle glow.",
     "porcelain_400", Contrast=-30, Saturation=-8, cg=dict(hi=(ORANGE, 1.5)), lab=dict(black=0.04, mid=0.35, c=-0.3), fx=dict(glow=10))
look(G, "Wedding Airy Blush", "wedding", "Light and airy with a blush: a little pink warmth overall.",
     "porcelain_400", Contrast=-30, Saturation=-5, Warmth=14, cg=dict(gl=(PINK, 4)), lab=dict(black=0.04, mid=0.35, c=-0.3), fx=dict(glow=10))
look(G, "Wedding Dark and Moody", "wedding", "Dark and moody: lowered mid-tones, thin colour, amber highlights, shaded corners.",
     "twilight_160", Saturation=-20, cg=dict(sh=(GREEN, 3), hi=(AMBER, 4)), lab=dict(black=0.02, mid=-0.5, c=0.3), vig=(0.45, 55))
look(G, "Wedding Moody Emerald", "wedding", "Dark and moody with deep green shadows, for woodland and garden light.",
     "nostalgia_200", Saturation=-10, SkinProtection=70, cg=dict(sh=(GREEN, 5), hi=(AMBER, 4)), lab=dict(black=0.02, mid=-0.4, c=0.3), vig=(0.4, 55))
look(G, "Family Bright", "family", "Bright, friendly colour for family sessions.",
     "heritage_gold", Strength=75, Saturation=10, lab=dict(mid=0.5, c=0.15))
look(G, "Newborn Soft", "family", "Very soft: low contrast, pale colour, smoothed texture.",
     "porcelain_400", Strength=60, Contrast=-35, Saturation=-15, lab=dict(mid=0.4, c=-0.4), texture=-20)

# --------------------------------------------------------------------------------------------
# Working - Places and Things
# --------------------------------------------------------------------------------------------
G = "Working - Places and Things"
look(G, "Product Crisp White", "product", "White-background product work: brighter mid-tones, clean whites, fine detail.",
     "sovereign", Strength=40, Contrast=10, FilmSharpness=100, lab=dict(mid=0.5, c=0.3), texture=20, clarity=10)
look(G, "Commercial Punch", "product", "Advertising colour: saturated, contrasty, sharp.",
     Output="labscan", Contrast=0, Saturation=15, clarity=15)
look(G, "Architecture Neutral", "architecture", "Buildings as built: neutral colour, firm structure.",
     "sovereign", Strength=50, Saturation=-10, FilmSharpness=100, clarity=20)
look(G, "Architecture Cool Concrete", "architecture", "Concrete, steel and glass: cool, thin colour, strong structure.",
     "arctic", Strength=45, Saturation=-20, clarity=20)
look(G, "Interior Warm", "architecture", "Lived-in interiors: warm light, soft contrast.",
     "golden_hour", Strength=85, Contrast=-15, Warmth=10, cg=dict(hi=(AMBER, 4)), lab=dict(mid=0.2))
look(G, "Real Estate Bright", "architecture", "Listings: bright mid-tones, open shadows, pleasant colour.",
     "sovereign", Strength=40, Saturation=8, lab=dict(mid=0.4, c=-0.2))
look(G, "Landscape Natural", "landscape", "Landscape as seen: neutral stock, muted colours brought up, a little structure.",
     "sovereign", Strength=85, Saturation=12, Vibrance=45, lab=dict(c=0.3), clarity=14)
look(G, "Landscape Vivid", "landscape", "Slide-film landscape: strong colour kept inside the highlights.",
     "vivid_chrome", Strength=60, Contrast=-20, Saturation=8, clarity=12)
look(G, "Landscape Golden", "landscape", "Low sun: golden highlights and warm colour.",
     "golden_hour", Saturation=10, cg=dict(hi=(AMBER, 7), bal=80))
look(G, "Landscape Moody", "landscape", "Weather: thin colour, teal shadows, lowered mid-tones, strong structure.",
     Saturation=-20, Contrast=15, cg=dict(sh=(TEAL, 5)), lab=dict(mid=-0.3), vig=(0.3, 60), clarity=20)
look(G, "Seascape Cool", "landscape", "Sea and sky: cool reversal stock with cyan in the shadows.",
     "arctic", Strength=60, cg=dict(sh=(CYAN, 4)))
look(G, "Forest Deep Green", "landscape", "Woodland: deep, slightly cool greens and dense shade.",
     "nostalgia_200", Saturation=5, cg=dict(mid=(GREEN, 3)), lab=dict(mid=-0.2))
look(G, "Autumn Foliage", "landscape", "Autumn: reds and oranges lifted, warm overall.",
     "heritage_gold", Warmth=4, RedShift=45, Saturation=12, Contrast=-10, cg=dict(mid=(RED, 4)))
look(G, "Food Fresh", "food", "Fresh, bright food: lively colour, lifted mid-tones.",
     "sovereign", Strength=60, Saturation=30, Vibrance=40, Warmth=10, cg=dict(mid=(AMBER, 3)), lab=dict(mid=0.35, c=0.2, chroma=10))
look(G, "Food Warm Table", "food", "A warm table: golden light, soft corners.",
     "golden_hour", Strength=70, Warmth=10, vig=(0.25, 60))
look(G, "Food Dark Rustic", "food", "Dark, rustic food: low key, rich mid-tones, heavy corners.",
     "twilight_160", Saturation=-10, lab=dict(mid=-0.3, c=0.3), vig=(0.4, 50))
look(G, "Street Colour", "street", "Street colour: fast negative with the colour brought back up.",
     "street_800", Strength=100, Saturation=18, cg=dict(sh=(TEAL, 4)), lab=dict(c=0.35), clarity=10)
look(G, "Street Muted", "street", "Quiet street colour: thin, soft-black, unhurried.",
     "street_800", Saturation=-25, lab=dict(black=0.03))
look(G, "Travel Postcard", "street", "Postcard colour: warm slide stock at half strength.",
     "desert_chrome", Strength=75, Contrast=-15, Saturation=10, Vibrance=30, cg=dict(hi=(AMBER, 4)), lab=dict(c=0.2))
# --------------------------------------------------------------------------------------------
# Working - Events and Low Light
# --------------------------------------------------------------------------------------------
G = "Working - Events and Low Light"
look(G, "Sport Punch", "sport", "Sport in good light: contrast, colour and bite.",
     Output="labscan", Contrast=0, Saturation=15, clarity=18)
look(G, "Sport Overcast", "sport", "Sport under cloud: colour and warmth put back.",
     "sovereign", Contrast=4, Saturation=28, Warmth=10, clarity=15)
look(G, "Low Light Clean", "low-light", "Dim rooms: softer contrast and calmer colour so noise stays quiet.",
     "sovereign", Strength=50, Contrast=-15, Saturation=-10, lab=dict(black=0.02))
look(G, "High ISO Rescue", "low-light", "Very high ISO: colour and contrast reduced, texture eased, fine grain laid over the noise.",
     Saturation=-30, Contrast=-20, lab=dict(black=0.03), texture=-15, grain=(800, 30))
look(G, "Night City", "low-light", "City at night: tungsten stock, blue shadows, firm contrast.",
     "cinematic_500t", Strength=80, Contrast=10, cg=dict(sh=(BLUE, 4)))
look(G, "Indoor Tungsten Tame", "low-light", "Takes the orange out of warm indoor light without changing white balance.",
     "sovereign", Warmth=-55, Saturation=-12, cg=dict(gl=(BLUE, 3)))
look(G, "Concert Stage Colour", "concert", "Stage lighting: saturated LEDs kept from clipping to flat colour.",
     Rolloff=60, Saturation=-15, LayerCoupling=-30, Contrast=-10)
look(G, "Concert Halation", "concert", "Stage lights with red halos and glow.",
     "cinematic_500t", Backing="removed", Halation=35, Saturation=5, fx=dict(glow=15))
look(G, "Concert Mono", "concert", "Black-and-white for the pit: coarse grain, firm contrast.",
     "silver_gelatin", Contrast=-22, grain=(3200, 60))

# --------------------------------------------------------------------------------------------
# Editorial - Bold: the loud end of the magazine rack
# --------------------------------------------------------------------------------------------
G = "Editorial"
look(G, "Cover Gloss", "bold", "A newsstand cover: dense blacks, lacquered colour, hard detail.",
     Output="labscan", Contrast=5, Saturation=25, lab=dict(c=0.5, white=0.97, chroma=15), clarity=22, soft=18)
look(G, "Hard Flash Fashion", "bold", "On-camera flash for fashion: bleached highlights, inky blacks, cold shadows, falling off fast.",
     "heritage_gold", Output="labscan", Contrast=5, Rolloff=-60, Saturation=10, cg=dict(sh=(BLUE, 8), hi=(YELLOW, 3)),
     lab=dict(c=0.55, white=0.97), vig=(0.6, 30), clarity=18)
look(G, "Cold Issue", "bold", "A steel-blue feature: colour drained to cyan and grey, contrast hard.",
     "arctic", Saturation=-35, Warmth=-25, cg=dict(sh=(CYAN, 10), gl=(BLUE, 4)), lab=dict(c=0.4, white=0.97), clarity=18)
look(G, "Tobacco Issue", "bold", "A warm, brown-black feature: tobacco mids, olive shadows, colour held low.",
     "twilight_160", Saturation=-35, Warmth=10, cg=dict(sh=(LIME, 6), mid=(ORANGE, 5), hi=(AMBER, 5)), lab=dict(c=0.5, black=0.03, white=0.97))
look(G, "Washed Out Editorial", "bold", "Overexposed on purpose: pale, high-key, colour bleached to pastel.",
     "porcelain_400", Saturation=-30, Contrast=-30, cg=dict(hi=(PINK, 3)), lab=dict(black=0.08, mid=0.9, c=-0.3), fx=dict(glow=20))
look(G, "Ink and Paper", "bold", "Heavy ink on uncoated stock: near-monochrome colour, blocked blacks, grey whites.",
     Saturation=-65, Contrast=15, cg=dict(gl=(AMBER, 3)), lab=dict(c=0.7, white=0.94, chroma=-20), grain=(800, 35))
look(G, "Night Desk Green", "bold", "Late reportage under mixed light: green-cyan cast, crushed shadows, grain.",
     "street_800", Saturation=-20, Tint=-40, cg=dict(sh=(SEA, 10), gl=(LIME, 4)), lab=dict(c=0.5), grain=(1600, 45, 20), vig=(0.4, 45))
look(G, "Red Carpet", "bold", "Event flash at night: saturated reds and skin, black background, glossy.",
     "heritage_gold", Output="labscan", RedShift=30, Saturation=20, Rolloff=-30, lab=dict(c=0.5), vig=(0.7, 30), soft=30)
look(G, "Zine Photocopy", "bold", "A photocopied zine page: two tones, burnt highlights, toner grain.",
     "silver_gelatin", Contrast=0, lab=dict(c=0.8, white=0.95, black=0.05), grain=(3200, 80), clarity=25)
look(G, "Protest Mono", "bold", "Front-page black-and-white: red filter, hard, grainy, corners burned in.",
     "silver_gelatin", BWFilter="red", Contrast=-25, lab=dict(c=0.4), grain=(1600, 55), vig=(0.4, 40), clarity=18)
look(G, "Poster Colour", "bold", "Graphic poster colour: primaries pushed apart, flat shadows, hard edges.",
     "vivid_chrome", Strength=55, LayerCoupling=80, Saturation=15, Contrast=-20, lab=dict(c=0.3, white=0.96, chroma=20), clarity=25, texture=20)
look(G, "Supplement Pastel", "bold", "A style-section pastel: mint shadows, peach highlights, lifted blacks.",
     "porcelain_400", Saturation=-10, cg=dict(sh=(SEA, 9), hi=(ORANGE, 6), bal=90), lab=dict(black=0.07, c=-0.2, mid=0.3))

# --------------------------------------------------------------------------------------------
# Abstract: pictures taken somewhere else entirely
# --------------------------------------------------------------------------------------------
G = "Abstract"
INV = "1;0;1;1;0;"
SOLAR = "1;0;0;0.25;0.5;0.5;1;0.75;0.5;1;0;"
HALF_SOLAR = "1;0;0;0.35;0.45;0.65;0.9;0.85;0.6;1;0.3;"
STEEP = "1;0;0;0.4;0.1;0.6;0.9;1;1;"
EASE = "1;0;0.15;0.5;0.5;1;0.85;"
# solarisation and negatives
look(G, "Solarised", "solar", "Full solarisation: the top half of the tonal scale folds back on itself.",
     lab=dict(L=SOLAR), gain=1)
look(G, "Half Solarised", "solar", "Partial solarisation: only the highlights reverse, edges glow dark.",
     lab=dict(L=HALF_SOLAR), gain=1)
look(G, "Solarised Silver", "solar", "Darkroom solarisation in black-and-white, with the line of reversal showing.",
     "silver_gelatin", Contrast=-30, lab=dict(L=SOLAR), grain=(400, 30), gain=1)
look(G, "Colour Negative", "solar", "The picture as a negative: tones reversed, colours left as they were.",
     lab=dict(L=INV), gain=1)
look(G, "Full Negative", "solar", "A true negative: tones and colours both reversed.",
     lab=dict(L=INV, a=INV, b=INV), gain=1)
look(G, "X-Ray", "solar", "A blue-white negative on black, like a film on a light box.",
     None, bw=True, ct=(-4, -30, -2, -6), lab=dict(L=INV), gain=1)
# false colour
look(G, "Infrared Crimson", "false-colour", "Colour infrared: foliage turns crimson and rust, skies go violet.",
     lab=dict(a=INV, chroma=15), gain=1)
look(G, "Infrared Violet", "false-colour", "Both colour axes reversed: purple trees under an amber sky.",
     lab=dict(a=INV, b=INV, chroma=10), gain=1)
look(G, "Alien Dusk", "false-colour", "Blue and yellow traded: warm skies, cold skin, pink light.",
     lab=dict(b=INV, chroma=10), gain=1)
look(G, "Hyper Colour", "false-colour", "Every colour pushed to its extreme: flat, saturated, poster-like.",
     lab=dict(a=STEEP, b=STEEP), clarity=20, gain=1)
look(G, "Acid Wash", "false-colour", "Hyper colour with solarised highlights.",
     lab=dict(L=HALF_SOLAR, a=STEEP, b=STEEP), gain=1)
look(G, "Redscale", "false-colour", "Film exposed through its base: everything red, orange and black.",
     None, bw=True, ct=(30, 22, 10, 28), lab=dict(c=0.4, white=0.94), grain=(400, 35), gain=1)
# duotones: Film Lab is off because a film stage after the toning would undo it
look(G, "Duotone Blue Gold", "duotone", "Two inks: deep blue shadows, gold highlights.",
     None, bw=True, ct=(4, -34, -2, 30), lab=dict(c=0.3), gain=1)
look(G, "Duotone Red Cyan", "duotone", "Two inks: red shadows, cyan highlights.",
     None, bw=True, ct=(32, 14, -22, -10), lab=dict(c=0.3), gain=1)
look(G, "Duotone Magenta Mint", "duotone", "Two inks: magenta shadows, mint highlights.",
     None, bw=True, ct=(34, -14, -20, 10), lab=dict(c=0.3), gain=1)
look(G, "Duotone Violet Peach", "duotone", "Two inks: violet shadows, peach highlights.",
     None, bw=True, ct=(22, -30, 10, 16), lab=dict(c=0.3), gain=1)
look(G, "Duotone Forest Cream", "duotone", "Two inks: bottle-green shadows, cream highlights.",
     None, bw=True, ct=(-26, 8, 0, 14), lab=dict(c=0.3), gain=1)
look(G, "Risograph Pink", "duotone", "One fluorescent ink on paper: pink from black to white, coarse grain.",
     None, bw=True, ct=(38, -4, 20, 0), lab=dict(c=0.4, white=0.95), grain=(1600, 50), gain=1)
look(G, "Blueprint", "duotone", "A cyanotype: Prussian blue shadows fading to pale blue paper.",
     None, bw=True, ct=(-4, -36, -5, -10), lab=dict(c=0.3, white=0.95), gain=1)
look(G, "Sepia Print", "duotone", "A sepia-toned print: warm brown from shadow to highlight.",
     None, bw=True, ct=(4, 12, 3, 14), lab=dict(c=0.2), gain=1)
look(G, "Selenium Print", "duotone", "A selenium-toned print: aubergine shadows under neutral highlights.",
     None, bw=True, ct=(10, -8, 0, 1), lab=dict(c=0.3), gain=1)
look(G, "Night Vision", "duotone", "Image-intensifier green: bright, noisy, dark at the edges.",
     None, bw=True, ct=(-30, 26, -34, 30), lab=dict(c=0.3, mid=0.4), grain=(3200, 75), vig=(1.0, 35), fx=dict(glow=30), gain=1)
look(G, "Thermal", "duotone", "A heat map: cold violet shadows rising through to hot yellow.",
     None, bw=True, ct=(30, -40, 6, 34), lab=dict(c=0.5, white=0.93, chroma=40), gain=1)
# hard split grades on colour
look(G, "Split Violet Lime", "split", "Colour kept, split hard: violet shadows, lime highlights.",
     Saturation=-20, cg=dict(sh=(VIOLET, 40), hi=(LIME, 30), bal=100), lab=dict(c=0.4), gain=1)
look(G, "Split Crimson Cyan", "split", "Colour kept, split hard: crimson shadows, cyan highlights.",
     Saturation=-20, cg=dict(sh=(RED, 36), hi=(CYAN, 28), bal=100), lab=dict(c=0.4), gain=1)
look(G, "Vapour Pink Teal", "split", "Teal shadows, pink mids and highlights: synthetic dusk.",
     Saturation=10, cg=dict(sh=(TEAL, 36), mid=(MAGENTA, 12), hi=(PINK, 16), bal=100), lab=dict(c=0.2, black=0.04, white=0.96), fx=dict(glow=25), gain=1)
# --------------------------------------------------------------------------------------------
# Dynamic: looks that move the eye with focus, light and structure
# --------------------------------------------------------------------------------------------
G = "Dynamic"
look(G, "Miniature World", "focus", "Tilt-shift miniature: a thin band of focus, toy-bright colour.",
     "vivid_chrome", Strength=60, Saturation=25, Contrast=-15, lab=dict(c=0.3, chroma=20), tilt=(70, 58, 16, 0))
look(G, "Sliver of Focus", "focus", "A narrow slice sharp, the rest falling away, colour quiet.",
     "porcelain_400", Saturation=-15, lab=dict(c=0.2), tilt=(60, 42, 14, 0), vig=(0.3, 60))
look(G, "Diagonal Focus", "focus", "The focus band laid across the frame on a slant.",
     "sovereign", Saturation=5, lab=dict(c=0.3), tilt=(65, 50, 16, 35))
look(G, "Low Horizon Focus", "focus", "Sharp along the bottom third, dissolving upward into blur and glow.",
     "golden_hour", tilt=(75, 78, 18, 0), fx=dict(glow=25))
look(G, "Burned Sky", "light", "A heavy graduated filter: the sky printed down two stops, the land left open.",
     Saturation=5, cg=dict(sh=(TEAL, 3)), lab=dict(c=0.4), grad=(1.6, 0, 45, -15), clarity=15)
look(G, "Storm Front", "light", "Dark graduated sky, drained colour, hard structure.",
     "arctic", Saturation=-35, lab=dict(c=0.3), grad=(1.2, 0, 50, -10), clarity=22, dehaze=15)
look(G, "Side Light", "light", "Light falling off across the frame from one side.",
     "twilight_160", lab=dict(c=0.3), grad=(1.4, 90, 60, 0), cg=dict(hi=(AMBER, 4)))
look(G, "Spotlight", "light", "A tight pool of light: the centre held, everything else dropped to black.",
     Saturation=-10, lab=dict(c=0.5), vig=(1.6, 25, 20))
look(G, "Sun Wash", "light", "Shooting into the sun: glow flooding the highlights, warm haze, a streak of flare.",
     "golden_hour", Warmth=15, lab=dict(black=0.05, c=-0.2), fx=dict(glow=70, radius=50, flare=60, length=70, angle=-30, threshold=50))
look(G, "Anamorphic Night", "light", "Long blue horizontal streaks through every light, teal blacks.",
     "cinematic_500t", Backing="removed", Halation=30, cg=dict(sh=(TEAL, 8), hi=(BLUE, 3)), lab=dict(c=0.3),
     fx=dict(flare=100, length=90, angle=0, glow=25, threshold=45))
look(G, "Star Filter", "light", "Crossed streaks from every point of light.",
     "cinematic_500t", lab=dict(c=0.3), fx=dict(flare=90, length=55, angle=45, glow=15, threshold=50))
look(G, "Hard Light", "structure", "Soft-light contrast stacked on film: glossy, deep, saturated.",
     Contrast=-15, Saturation=10, soft=65, clarity=12)
look(G, "Graphic Structure", "structure", "Extreme local contrast: every edge drawn in, shadows opened flat.",
     "street_800", Contrast=-25, Saturation=-15, clarity=42, texture=50)
look(G, "Cut Through Haze", "structure", "Haze stripped out: dense distance, heavy colour, dark skies.",
     "desert_chrome", Strength=45, dehaze=40, clarity=12)
look(G, "Etched Mono", "structure", "Black-and-white with the structure of an engraving.",
     "silver_gelatin", Contrast=-45, BWFilter="orange", clarity=42, texture=55, grain=(400, 25))
look(G, "Velvet Blur", "structure", "Clarity run backwards: glowing, smooth, low in detail.",
     "analog_dream", lab=dict(c=-0.2, black=0.04), clarity=-55, fx=dict(glow=45, radius=40))

# --------------------------------------------------------------------------------------------
# Layers: stack on any look. They carry one section each and no Film Lab keys.
# --------------------------------------------------------------------------------------------
G = "Layers - Grain"
def _grain(name, note, iso, strength, colour=0, size=100):
    layer(G, name, note, Grain=dict(Enabled="true", ISO=iso, Strength=strength, Scale=size, Color=colour))
_grain("Grain Whisper", "Barely-there grain to take the digital edge off.", 200, 12)
_grain("Grain Fine 100", "Slow-film grain: fine and even.", 100, 25)
_grain("Grain Classic 400", "The grain of an everyday medium-speed film.", 400, 35)
_grain("Grain Street 800", "Fast-film grain, clearly visible.", 800, 45)
_grain("Grain Pushed 1600", "Pushed-film grain: coarse and assertive.", 1600, 55)
_grain("Grain Coarse 3200", "The coarsest: surveillance and concert grain.", 3200, 70)
_grain("Grain Dye Clouds 400", "Colour grain: soft dye clouds in all three layers.", 400, 40, 60)
_grain("Grain Dye Clouds 1600", "Coarse colour grain, as on pushed colour negative.", 1600, 55, 70)
_grain("Grain Small Format", "Half-frame and 16 mm: the same grain enlarged further.", 800, 50, 20, 100)
_grain("Grain Medium Format", "Roll film: the same grain enlarged less.", 400, 30, 0, 45)
layer(G, "Grain Off", "Removes grain.", Grain=dict(Enabled="false", ISO=400, Strength=0, Scale=100, Color=0))

G = "Layers - Finish"
def _vig(name, note, strength, feather, roundness=50):
    layer(G, name, note, PCVignette=dict(Enabled="true", Strength=strength, Feather=feather, Roundness=roundness))
def _fx(name, note, **kw):
    keys = dict(Enabled="true", Threshold=kw.get("threshold", 70), Glow=kw.get("glow", 0), GlowRadius=kw.get("radius", 25),
                Halation=kw.get("halation", 0), HalationSize=kw.get("size", 30), HalationWarmth=kw.get("warmth", 70),
                Flare=kw.get("flare", 0), FlareLength=kw.get("length", 40), FlareAngle=kw.get("angle", 0))
    layer(G, name, note, **{"Light Effects": keys})
_vig("Vignette Subtle", "A quarter stop off the corners, very soft.", 0.25, 70)
_vig("Vignette Portrait", "A rounder, tighter fall-off that holds the eye on a face.", 0.45, 60, 30)
_vig("Vignette Heavy", "Most of a stop: an old lens wide open.", 0.8, 45)
_vig("Vignette Bright Edges", "Corners lifted instead of darkened, for high-key pictures.", -0.25, 70)
layer(G, "Vignette Off", "Removes the vignette.", PCVignette=dict(Enabled="false", Strength=0, Feather=50, Roundness=50))
_fx("Glow Soft", "A light diffusion over the highlights.", glow=45, threshold=55)
_fx("Glow Bloom", "Highlights bloom well into their surroundings.", glow=85, radius=50, threshold=50)
_fx("Streak Anamorphic", "A horizontal streak through bright lights.", flare=90, length=85, angle=0, threshold=50)
_fx("Streak Diagonal", "A diagonal streak, as from a smeared front element.", flare=80, length=60, angle=35, threshold=50)
layer(G, "Glow Off", "Removes glow and streaks.",
      **{"Light Effects": dict(Enabled="false", Threshold=70, Glow=0, GlowRadius=25, Halation=0, HalationSize=30,
                               HalationWarmth=70, Flare=0, FlareLength=40, FlareAngle=0)})
layer(G, "Focus Miniature", "A thin horizontal band of focus.", TiltShift=dict(Enabled="true", Amount=70, FocusPos=55, FocusWidth=16, Feather=50, Angle=0))
layer(G, "Focus Diagonal", "A slanted band of focus.", TiltShift=dict(Enabled="true", Amount=60, FocusPos=50, FocusWidth=18, Feather=50, Angle=35))
layer(G, "Focus Off", "Removes the tilt-shift blur.", TiltShift=dict(Enabled="false", Amount=0, FocusPos=50, FocusWidth=20, Feather=50, Angle=0))
layer(G, "Sky Graduated", "Darkens the top of the frame by a stop and a half.", Gradient=dict(Enabled="true", Degree=0, Feather=45, Strength=1.5, CenterX=0, CenterY=-15))
layer(G, "Sky Graduated Off", "Removes the graduated filter.", Gradient=dict(Enabled="false", Degree=0, Feather=25, Strength=0.6, CenterX=0, CenterY=0))
layer(G, "Detail Micro Contrast", "Fine texture brought forward.", Texture=dict(Enabled="true", Radius=20, Amount=25))
layer(G, "Detail Clarity Punch", "Mid-tone structure brought forward.", Clarity=dict(Enabled="true", Radius=100, Amount=30))
layer(G, "Detail Soft Skin", "Fine texture eased back.", Texture=dict(Enabled="true", Radius=20, Amount=-30))
layer(G, "Detail Gritty", "Both structure and texture pushed hard.",
      Clarity=dict(Enabled="true", Radius=100, Amount=40), Texture=dict(Enabled="true", Radius=20, Amount=40))
layer(G, "Detail Off", "Removes clarity and texture.",
      Clarity=dict(Enabled="false", Radius=100, Amount=0), Texture=dict(Enabled="false", Radius=20, Amount=0))
