# Film Grain (Effects > Grain), model V3

Status: implemented October 8, 2026 in `rtengine/ipgrain.cc` (`grainv3` namespace, entered through `ImProcFunctions::filmGrainV2`), with the Film Lab link in `rtengine/graincharacter.h` and `filmGrainCharacter` in `rtengine/ipfilmlab.cc`. The tool's four sliders (Grain, ISO, Size, Color) and its pp3 keys are unchanged; only what they produce changed.

## What was wrong before

The Grain tool drew three octaves of simplex noise and pushed it through a paper-response lookup. Simplex noise is smooth, organic and evenly spaced: it reads as a soft texture laid over the picture, not as developed emulsion, and because it is additive in L* it sat equally on shadows, mid-tones and paper white. The Film Lab stage deliberately applies no grain of its own (one place to control texture), so every film simulation inherited that texture.

## The model

A developed emulsion is a random field of silver clumps or dye clouds, a few micrometres across, scattered irregularly and gathering into larger clumps. What a scan or print shows is that field integrated by its sampling aperture. The renderer builds the field literally:

- **Lattice of blobs.** A jittered lattice whose pitch is the grain diameter in image pixels. Each cell holds two soft blobs (`(1 - d²/r²)²` bump) with hashed random position, diameter (half a cell to a full cell, most near the small end) and signed density (triangular, slightly heavy toward dense clumps). The blobs are splatted into row bands, so the cost is a few additions per pixel regardless of image size.
- **Clumping.** A second lattice 2.7x coarser, weighted by the emulsion's clumping, supplies the mottle that fine grains gather into. Default weight 0.35.
- **Size.** Grain diameter in micrometres on a 24 mm frame: `6 + 2.6·(ISO/100)^0.6`, times the Size slider, mapped to pixels by the image's short side. ISO 100 sits near 9 µm, ISO 400 near 13 µm, ISO 3200 near 29 µm.
- **Anchoring.** Everything is evaluated at full-image coordinates, so the detail window, the main preview and the export show the same grains in the same places. The field's unit sigma is measured once on a reference patch so amplitude is expressed in sigma units.
- **Previews integrate.** An output pixel spanning `s` full-resolution pixels widens every blob by the pixel's own footprint and scales it to keep its integral. Once a pixel outspans a grain the lattice itself follows the pixel pitch with the blob density reduced to match (Selwyn's law). A zoomed-out preview, and a thumbnail, show the quiet fine noise a real reduction would, never a blow-up of the texture. Thumbnails now pass the full frame size and their sampling scale for this reason; before they sized grain against their own tiny dimensions.
- **Density, not addition.** Luminance is multiplied by `exp(noise · sigma · g(L*))`: grain multiplies light as density does. `g` is where real granularity lives in the tone scale: a hump centred on the stock's density peak (lighter mid-tones for a negative, whose dense areas print as highlights; the shadows for reversal film), wider toward black than toward white, with a floor so empty shadows are not glassily clean, and fading to nothing at paper white.
- **Colour.** The Color slider adds the same field built once per dye layer on coarser lattices, the blue-sensitive layer coarsest, riding the same granularity curve and staying out of the deepest shadows where a print holds no colour.

Strength 100 gives a peak sigma of 0.26 in log luminance. The locallab path keeps the legacy `GrainEvaluator::operator()`.

## Film Lab link

`grainEffect` receives the Film Lab parameters. When Film Lab is enabled with a non-custom stock, `filmGrainCharacter` derives a `GrainCharacter` from the stock table and the V3 profile: speed nudges grain size (`(speed/400)^0.2`, clamped 0.8 to 1.25, so the ISO slider stays the master), `grainClumping` and `grainColor` from the profile set the clump weight and the colour amplitude, monochrome stocks and the `bw` process silence colour grain, reversal stocks move the granularity peak into the shadows (0.34 versus 0.62 L*), and the stock's own `grain` entry with the process's grain factor scales the amount. The unused `grainSize`/`grainClumping`/`grainColor` fields of `FilmPresetsParams` are folded in as trims should a UI ever expose them. Film Lab off, custom or model 1 leaves the generic colour-negative character.

Pipeline order is unchanged: grain still runs before Film Lab, so the film's curves shape the grain as a print shapes a negative's.

## Verification

Headless renders through `steep-cli` with a partial pp3 (`[Grain] Enabled=true Strength=60 ISO=400 Color=35`) over a 1800x1200 fixture, compared with the ungrained render (sRGB 8-bit, luminance L = 0.2126R+0.7152G+0.0722B). Standard deviation of the grain's luminance change by base tone: L 0-40: 0.8, L 40-90: 3.6, L 90-150: 6.7, L 150-210: 7.6, L 210-255: 3.9 (mean |dL| 4.2 overall), i.e. quiet in the blacks, loudest in the lighter mid-tones, falling off toward paper white, as a negative prints. 3x crops at shadow, mid-tone and highlight positions were inspected visually. `tools/de_probe.py` and the Double Exposure motion suite were unaffected.
