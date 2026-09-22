# Film Emulation and Preset Library Roadmap

Date: 2026-09-20. Status: roadmap with the first film-recipe safety pass implemented; see progress below.

Scope: the current Film Lab feature, its model-versioned implementation, its film recipes, and bundled general photo presets. This is a follow-on to `film-emulation-auto-edit-plan.md`, not a proposal to rebuild the legacy Hald/LUT feature. No application code, running session, or photo settings were changed for this review.

## Implementation Progress

The subsequent implementation pass added a versioned film-only application policy, retuned the eight bundled Film Looks for V5, and added Sovereign Everyday and Cinema Reveal. New recipes preserve global exposure/WB, geometry, local edits, repairs, grain, and denoise. Film settings are replaced as a complete group, so omitted values cannot leak from the previous look. Preset hover/fill-mode handling and thumbnail parameter baselines now agree with those application rules.

Native Windows GUI/CLI builds, native recipe regression tests, the V5 core rendering probe, and the new 16-bit recipe probe passed. A real-RAW contact sheet was also rendered from DSCF8535.RAF. These checks are not physical calibration, a long-session GUI test, or Linux/macOS runtime validation. The running application was not installed over or restarted.

The six Creative profiles, clean-photo collection, manual model default, measured material calibration, generation-aware film scene buffers, and cache/performance changes remain pending. Saved edits and existing full/partial user-profile semantics remain unchanged. See `film-look-recipes.md` for the implemented contract and reproducible test commands.

## Assessment

The largest remaining gain is calibration and predictable composition of effects, not stronger halation or more global color tints. Good film emulation should retain believable illumination, useful black separation, skin variation, textured highlights, and material color while imparting a recognizable response. Deliberately faded, heavily glowing, or cross-processed looks belong in a separate creative collection.

The current source already has per-layer density curves, interlayer coupling, a spectral approximation for print rendering, format-scaled optics, deterministic standalone grain, and a bounded density/print table cache. V5 adds material-based optics, separate Print Exposure, and smoother print highlight handling. Preserve and validate these foundations rather than replacing them indiscriminately.

### Review Baseline Findings

| Finding | Evidence | Consequence |
| --- | --- | --- |
| Manual default and all eight bundled Film Looks select V4; Auto Film selects V5. | `rtengine/procparams.cc:3437`, `rtdata/profiles/Film Looks`, `rtgui/autoedit.cc:2289` | Manual and automatic entry points do not consistently benefit from the newer optics/highlight changes. Migration requires recipe retuning, not a blind version-number replacement. |
| Sixteen named choices plus Custom share compact hand-tuned stock descriptions. | `rtengine/ipfilmlab.cc:48`, `:68`, `makeV3Profile`, `makeV4Character` | There is considerable expressive range, but no checked-in calibration dataset establishing how closely each description matches measured film. |
| Dye spectra are approximated with Gaussian lobes; receiver/print parameters are also hand-selected. | `rtengine/ipfilmlab.cc:1372` | This is an approximate color model, not measured end-to-end spectral reproduction. |
| Existing display-domain edits are reconstructed using capped per-channel ratios. | `rtengine/ipfilmlab.cc:890` | Dark or nearly clipped channels can make film/curve/color interactions fragile. This is a risk to investigate, not a reproduced new defect in this review. |
| Missing scene buffers silently fall back to display-referred input; validation checks geometry but not a source/parameter generation. | `rtengine/ipfilmlab.cc:2006` | A preview can plausibly differ from the intended finalized film render. Need generation-aware validation and an explicit provisional state. |
| Bundled looks combine film response with substantial exposure/shadow/highlight shaping and sometimes fixed white balance. | Porcelain Portrait, Neon Tungsten Night, Golden Hour Warm, Creative/Cinematic Warm | A look may change scene illumination or stack tonal corrections rather than only change rendering character. |
| Street Silver 400 selects the color stock `street_800`, B&W process, a strong extra curve, sharpening, and standalone grain ISO 1600. | `rtdata/profiles/Film Looks/Street Silver 400.pp3` | The recipe's name, tonal model, and texture story are not coherent. B&W deserves its own calibrated response families. |
| Grain is a separate active pipeline stage; some Auto Film writes target old FilmPresets grain fields that the renderer does not read. | `rtengine/ipgraineffect.cc:27`, `rtengine/ipgrain.cc:447`, `rtgui/autoedit.cc:2317`, `:2535` | Keep one grain implementation and one settings owner. Do not introduce a second grain pass or silently start adding grain to Auto Film. |
| The immutable density/print cache already exists, holds four parameter sets, and does not promote hits. | `rtengine/ipfilmlab.cc:1897` | Measure hover-cache churn and duplicated in-flight table construction; do not treat table caching as absent. |

Inventory: 32 bundled `.pp3` profiles, including eight Film Looks and six Creative profiles. Remaining profiles include technical/raw-development choices; do not reclassify those as aesthetic looks.

## 1. Establish a Calibration Baseline

- Freeze the existing V1-V5 outputs for saved edits. Build comparisons using the actual C++ renderer, not only Python copies of its formulas.
- Start with six representative families: neutral daylight negative, portrait negative, tungsten/motion negative, vivid reversal, medium-speed B&W, and fine-grain B&W. Expand only after these pass.
- Use published characteristic curves, spectral sensitivity, dye-density, granularity, and MTF data as initial constraints. Record source, illuminant, processing, scan/print settings, and uncertainty with each fit. Manufacturer data is representative, not proof of an exact stock match. [Manufacturer technical data](https://www.kodak.com/content/products-brochures/motion-picture/KODAK-VISION3-5219-7219-technical-information.pdf)
- Where available, add controlled paired digital captures and film scans: step wedge, color chart, several skin tones, foliage, fabrics, neutral objects, and colored lights. Disable scanner auto corrections and document remaining transformations. Uncontrolled internet scans are not calibration ground truth.
- Separate fitting photos from held-out evaluation photos. Include daylight, tungsten, mixed LED light, low-key/high-key scenes, backlight, colored speculars, and defocused highlights.
- Score density/curve fit, neutral-axis drift, hue/chroma behavior, highlight continuity, grain power spectrum, and spatial response separately. Use blinded visual A/B review as well; a single image-similarity score cannot define good film rendering.

## 2. Clarify the Processing Contract

Target conceptual order for a future model revision:

`documented scene-linear input -> virtual film exposure/filtering -> optical spread -> layer exposure/density/development -> print or scan -> output rendering -> explicit creative finishing`

This is a contract to implement behind a version boundary, not permission to reorder existing global edit tools for old photos.

- Document exactly which camera-profile, exposure, highlight-recovery, and color operations precede the scene tap. Specify primaries, white point, scale, and handling of negative/super-white values.
- Replace ratio-based grade reconstruction incrementally with explicit operations. First establish intended behavior for exposure, tone/RGB curves, B&W, local masks, and Film Lab interaction. Existing controls must remain effective and must not be applied twice.
- Keep negative exposure and print exposure independent. Negative exposure changes where the scene lands on the emulsion; print exposure sets final presentation. Push/pull is a development response, not a universal fixed exposure compensation.
- Carry source/parameter generation IDs through the scene tap, RGB snapshot, detail crop, thumbnail, and final render. If inputs are stale or absent, retain a provisional preview and schedule valid completion; do not certify the fallback as final.
- Maintain float precision until delivery. Keep smooth gamut compression and make display-gamut policy explicit: current V5 uses linear-sRGB highlight fitting even when the broader output chain may target another gamut. Evaluate output-target-aware handling under a new version; retain an sRGB-compatible option.
- Audit mix semantics: 0 must be an exact bypass, 100 the calibrated full look, intermediate positions continuous and repeatable. Halation, print placement, and creative fade must not secretly change merely because an overall mix changed.

## 3. Improve the Film Response

### Tone and Development

- Fit per-layer monotonic log-exposure/density curves with measured base/fog, straight-line slope, toe, shoulder, and crossover. Avoid deriving every layer from one stock contrast number and small multipliers.
- Preserve real differences between negative and reversal response. Do not force every stock to the same black, median, contrast, or white target.
- Fit development changes at supported push/pull settings, including tonal slope and crossover. Interpolate continuously between validated settings; label unsupported combinations as creative rather than physically matched.
- Make a healthy full-strength base recipe before creating reduced-strength variants. Do not conceal a problematic stock response by permanently blending it with a large amount of untreated digital output.

### Color, Skin, and Print

- Replace class-wide spectral approximations with versioned profile data where reliable measurements exist. Keep an offline reference calculation and bake bounded LUTs for interactive rendering.
- Calibrate negative dye formation, masking/coupling, and output-medium rendering separately. Avoid several compensating saturation/zone-tint layers whose combined effect is hard to predict.
- Define print/scan illuminants and color adaptation explicitly. A scanner interpretation, a photographic paper print, and projected reversal should not be mere temperature variations of one output.
- Treat skin as varied reflectance under varied lighting, not one preferred hue. Use continuous safeguards against extreme hue/chroma excursions while retaining undertones and colored illumination. Test that skin protection does not undo deliberate print exposure or create tonal transitions at its confidence boundary.
- Measure saturated red, blue, green, and orange response, especially near the shoulder. Require continuous highlight hue trajectories without colored plateaus, contour rings, or abrupt clipping.
- Be precise in naming: RGB-based reconstruction cannot uniquely recover the scene spectrum. Offer calibrated film-inspired rendering, not claims of exact physical reconstruction.

### B&W

- Add distinct panchromatic, fine-grain, and broader-grain/documentary responses instead of relying on a color-negative recipe followed by gray conversion.
- Put optional yellow/orange/red/green filtration at the virtual exposure/sensitivity stage, sharing semantics with the existing B&W controls.
- Separate emulsion/developer response from paper grade and warm/cool print toning. Keep neutral black and paper-white anchors unless a creative tone explicitly changes them.
- Use published B&W sensitivity and characteristic-curve data as calibration constraints. [B&W technical reference](https://www.ilfordphoto.com/amfile/file/download/file/1903/product/693/)

## 4. Refine Halation, Bloom, and Texture

- Keep the V5 separation of film construction from development chemistry. Manufacturer documentation confirms that anti-halation protection need not be a removable backing, so chemistry alone cannot identify its strength. [Construction reference](https://www.kodak.com/content/pdfs/motion/AHU-talking-points.pdf)
- Fit stock-family optical return energy, color, core/tail distribution, and spread against controlled bright-point and bright-edge references. Replace universal hard-coded radii/return fractions only where measurements justify a difference.
- Preserve bright-source energy before the output shoulder. Highlights may bloom/halate while retaining a smooth core; do not add rings after the image is already clipped.
- Audit energy across the direct and scattered paths. Bloom should redistribute source-colored light; halation models secondary exposure. Avoid broad haze, exaggerated loss of black contrast, and identical red glow on every emulsion.
- Verify off-crop contributions, physical format scale, subpixel point sources, and fit/100%/export consistency. Very small previews must integrate unresolved spread rather than invent a separate appearance.
- Retain the standalone Grain tool and existing image-coordinate anchoring. Add a shared optional emulsion-texture mode there, with density-dependent variance, size distribution, color covariance, and print/scan filtering.
- Benchmark a grain model against the published resolution-independent stochastic reference before adopting a production approximation. Avoid expensive Monte Carlo work in every interactive render. [Grain model and implementation](https://www.ipol.im/pub/art/2017/192/)
- Keep sensor noise and simulated grain distinct. Source digital ISO is not a film-stock identity. Use conservative texture defaults and offer texture as an explicit recipe component.
- Calibrate acutance/softness as a spatial response rather than generic sharpening plus blur. Do not add gate weave, dust, scratches, frame jitter, or color misregistration to still-photo realism defaults; these are optional creative effects.

## 5. Rebuild Presets as Coherent Recipes

### Separate Three Types

1. **Film stocks:** calibrated emulsion + default process/output pairing, without scene-specific correction.
2. **Photo looks:** intentional artistic finishing for portraits, travel, architecture, landscapes, night, and monochrome. Film is optional.
3. **Technical profiles:** raw processing, noise reduction, film-negative inversion, pixel shift, and related workflow settings. Preserve their full-profile semantics.

My Profiles remains above Bundled Profiles once populated. Keep existing invented names and stable IDs; never introduce commercial film-stock names into product labels or bundled profile names.

### Proposed First Curated Collection

| Family | Existing names to refine / proposed additions | Photographic target |
| --- | --- | --- |
| Everyday negative | Sovereign, Heritage Gold | Natural daylight color, dimensional midtones, restrained warmth, usable shadows. |
| Portrait negative | Porcelain Portrait | Skin separation and highlight latitude without forced pastel skin or a fixed Kelvin setting. |
| Warm daylight | Golden Hour | Preserve existing warm light; avoid painting tungsten warmth over neutral scenes. |
| Reversal | Vivid Chrome, Arctic, Desert Chrome | Distinct saturation/hue response with firmer tonal contrast; no automatic color clipping. |
| Motion/night | Twilight, Cinema Reveal, Neon Tungsten Night | Color separation under mixed light, readable low-key midtones, controlled bright lights. |
| Monochrome | Silver Print, Street Silver, proposed Fine Silver | Separate tonal/spectral/texture identities; internally consistent speed/format descriptions. |
| Clean photo looks | Proposed Natural Portrait, Open Landscape, Architectural Neutral, Quiet Night | Useful finishing without forcing film, fixed WB, lifted blacks, or universal shadow recovery. |
| Expressive looks | Faded Instant, Analog Dream, Ember | Deliberate fade, glow, or palette stylization, clearly separate from realism-oriented film stocks. |

These are recipe targets, not claims that calibrated assets already exist. Keep the initial collection small and distinct; do not multiply near-duplicates with different names.

### Application Rules

- Bundled aesthetic looks preserve crop, rotation, perspective, local masks, healing, lens correction, metadata, and noise reduction by default.
- Preserve the user's exposure and white balance unless an explicit adaptive/normalization option is selected. Encode palette warmth in the look, not by replacing camera WB with 3800K, 5400K, or 6200K.
- Give each bundled look a versioned ownership list of the settings it changes. Switching A -> B must not leave hidden parts of A active; applying B repeatedly must not intensify it.
- Reuse the existing profile infrastructure and structured parameter objects. Keep a separate compatibility path for saved full processing profiles; do not silently turn all My Profiles into partial looks.
- Reset only look-owned changes to the pre-look baseline, not unrelated edits. Hover has a temporary parameter snapshot, never commits history, and restores the prior state on leave; click commits exactly what was previewed.
- Make recipe amount a stable interpolation from the pre-look baseline, not a sequence of incremental edits. Interpolate meaningful continuous parameters; do not blend unrelated stock IDs or development-process enums.
- Store meaningful metadata with each recipe: category, profile/model version, supported input, output medium, contrast/saturation intent, highlight behavior, optional texture, and owned parameters. Validate all referenced stock/process/output IDs at build/test time.
- Remove incidental sharpening, global exposure offsets, generic shadow lifts, and fixed WB from aesthetic recipes unless they are justified and explicitly part of the look. Retune the eight existing Film Looks on the validated modern model rather than simply changing `ModelVersion`.

## 6. Preset Selection and Auto Film

- Reuse the compact preset UI. Group stocks, photo looks, creative effects, and technical profiles clearly; use actual current-image previews with search, favorites, and stable selection.
- Render only visible/requested candidates, prioritize the hovered one, and cancel obsolete requests. Avoid rendering every preset at full size when a menu opens.
- Keep Auto Edit, Auto Grade, and Auto Film separate. No default look on RAW open; no automatic crop/level changes from an aesthetic action.
- Let Auto Film select from the same validated recipe metadata used by the manual browser, then perform a bounded final-look check. Do not maintain a second divergent set of hard-coded film recipes.
- Score clipping, midtone readability, low-key intent, shadow separation, chroma extremes, and skin behavior against the candidate's intended family. Do not force all scenes toward one histogram or eliminate the character of reversal/low-key looks.
- Preview, click, batch, and context-menu actions use the same recipe and analysis result. Preserve determinism regardless of selection order or thumbnail readiness.

## 7. Performance and Release Gates

- Profile the existing table cache before changing it. Evaluate true LRU promotion, byte-budgeted capacity, and same-key in-flight deduplication for a hover session across the library.
- Cache source-dependent optical fields separately from cheap print/color/mix controls where dependencies permit. Bound memory and ensure source/profile/geometry invalidation is complete.
- Interactive and final renders share tone/color math and recipe identity. Only spatial sampling quality may vary, and it must converge without a visible scale or exposure jump.
- Require native C++ renderer tests for 0-strength identity, model-version compatibility, recipe serialization, A -> B reset behavior, repeated application, and hover rollback.
- Add exposure ladders, neutral/color ramps, skin patches, saturated lights, bokeh, crop-border lights, and noisy low-key frames. Inspect 16-bit exports before diagnosing 8-bit display banding.
- Compare thumbnail, fit view, detail crop, before/after, reopened sidecar, and export after matching color conversion and sampling. Confirm the final render is not a display-input fallback.
- Run a long-session edit/hover/switch/export test with bounded memory and latency, plus native Windows/Linux/macOS checks before claiming cross-platform parity.
- Extend existing `filmlab_v5_probe.py`, `filmlab_highlight_probe.py`, `filmskin_probe.py`, `filmzone_probe.py`, and native auto-edit tests. Existing formulas-only probes are supporting diagnostics, not the final oracle.

## Delivery Order

1. **Baseline and preset safety:** source/preview-generation audit, ownership/idempotence tests, WB/exposure-preserving recipe application, and renderer-backed contact sheets.
2. **Modern recipe refresh:** validate V5, retune the eight Film Looks and six Creative looks, add the small clean-photo collection, and preserve all old profile/model interpretations.
3. **Calibrated response revision:** fit tone/color/print families, introduce explicit grade-stage semantics behind a new model version, and add dedicated B&W material responses.
4. **Optics and optional texture:** tune measured halation/MTF, introduce shared density-aware texture, and verify scale/crop parity.
5. **Library/Auto Film integration and hardening:** unify recipe selection, optimize measured hot spots, and complete long-session/cross-platform gates.

First visible deliverable: a small preset set that is demonstrably distinct, maintains intentional exposure and WB, and behaves predictably across a representative photo set. Physical calibration, new model behavior, and claimed performance improvements remain unverified until their specific tests are run.
