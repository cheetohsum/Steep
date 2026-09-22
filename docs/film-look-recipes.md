# Film-Only Recipes

## Scope

The first roadmap implementation refreshes the eight bundled Film Looks and adds Sovereign Everyday and Cinema Reveal. It does not claim measured matches to physical film or change the interpretation of existing saved edits. No look is automatically applied when opening a RAW.

The refreshed recipes use the existing V5 renderer. They remove preset-imposed global exposure, fixed Kelvin WB, secondary tone curves, sharpening, vignette, and grain. Warmth and contrast are expressed inside the film stage. Street Silver now uses the monochrome material instead of a desaturated color stock. Both monochrome recipes disable skin-color restoration while retaining the existing subtle silver-paper toning.

## File Contract

```ini
[Steep Look]
Version=1
Kind=film
Id=sovereign-everyday
Family=negative

[Film Presets]
Enabled=true
Preset=sovereign
ModelVersion=5
```

- The marker opts in. Ordinary full and partial PP3 profiles retain their existing semantics.
- Version 1 accepts only Film Presets, Steep Look, and optional Version groups, and requires model V5. Unknown policy versions and unrelated processing groups are rejected.
- Loading a recipe resets its film group to constructor defaults before reading it. Bundled recipes explicitly enumerate the film fields so their defaults are reviewable.
- The edited-field mask selects the complete film group and nothing else. Switching between these recipes or applying one repeatedly cannot retain omitted settings or compound adjustments.
- The profile-panel fill toggle does not expand a film recipe into a full-photo reset. Hover uses the same fill-policy decision as clicking.
- Generated preset thumbnails use a captured current-photo parameter baseline, rather than an otherwise default image. The existing fast thumbnail renderer still determines which spatial effects it supports; this change does not claim full-resolution thumbnail parity.
- Application-policy metadata belongs to the loaded partial profile, not the edited photo. Saving a normal full image profile records the resolved film settings without imposing film-only semantics when that saved profile is later applied elsewhere.
- Applying a new recipe does not retrospectively undo WB or exposure changes made by an old recipe: those are now part of the photo's existing edits and cannot safely be distinguished from intentional manual adjustments.

## Verification

Enable the optional native test executable:

```sh
cmake -S . -B build -DSTEEP_BUILD_PROFILE_TESTS=ON
cmake --build build --target steep-profile-tests rth-cli rth
steep-profile-tests "rtdata/profiles/Film Looks" /existing/scratch/directory
python tools/film_look_probe.py --cli /path/to/steep-cli --output test-output/film-looks/render
python tools/filmlab_v5_probe.py --cli /path/to/steep-cli --output test-output/film-looks/v5-core
```

The CLI needs its normal packaged data and compatible runtime libraries. The Python rendering probes require NumPy, Pillow, and tifffile. `film_look_probe.py --photo /path/to/photo.RAF` additionally renders a real-photo contact sheet, without reading or changing that photo's sidecar.

Native tests cover unrelated edit preservation (including crop, perspective, local masks, repairs, grain, denoise and WB), recipe switching, repeat application, serialization, sparse recipes, invalid recipe groups/versions, and legacy V4 partial-profile behavior.

The render probe checks monotone neutral ramps, highlight separation, bounded warmtone monochrome tint, and pixel-identical direct/switch/repeat application. On the synthetic chart, all ten recipes retained highlight separation and had no fully clipped output channels. This is a fixture-specific result, not a guarantee that clipped source photographs can be recovered.

Windows GUI/CLI compilation and both rendering probes passed in the September 20 implementation pass. The DSCF8535.RAF comparison was visually inspected. No GUI interaction or long-session test was performed, and Linux/macOS binaries were not run in this pass.

## Remaining Work

The broader roadmap still covers reference-fitted film response, a dedicated B&W material family, general photo/Creative presets, a compatible manual-default migration, generation-safe preview finalization, shared Auto Film recipes, and measured cache/optics performance improvements. The current manual model default is deliberately unchanged until its older-profile migration is tested explicitly.
