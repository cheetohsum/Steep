# Film Emulation and Auto Edit Improvement Plan

Date: 2026-09-15. Status: first implementation built and verified on Windows, on `codex/film-lab-realism`.

## Implementation Status

Implemented for the first test build:

- Thumbnail Film Lab rendering with a scene tap, full-image format scale, and RGB snapshot.
- Separate floating-point, crop-aware finished-look analysis in a fixed sRGB output space. It retains film settings and RGB curves rather than using the exposure-meter proxy.
- Auto Film V5 with bounded, measured print-exposure, contrast, and chroma correction; rejected corrections revert to the original candidate. Repeated runs are deterministic in the native regression test.
- More restrained upper-curve lift on high-risk auto edits, and preservation of existing local masks.
- Versioned V5 material optics: film-construction suppression independent of development chemistry, channel-dependent reflected light, separate source-colored bloom, fractional-radius Gaussian kernels, and crop support padding.
- Independent serialized Print Exposure control and predictable V5 overall mix. V1-V4 retain their existing optical/mix behavior; V4 remains the manual default and Auto Film explicitly chooses V5.
- Bounded immutable density/spectral table cache; halation and strength changes reuse the same tables.
- Restored visibility of advanced Film Lab controls in V4 and V5.
- V5 print highlight correction (2026-09-16): retain distinct super-white densities through the print LUT, then roll off display luminance after grading and Print Exposure. Smooth, luminance-preserving chroma compression fits highlights into linear-sRGB headroom so hot lights approach white instead of retaining colored plateaus. A small white reserve avoids rounding/mix clipping. Custom keeps its straight neutral response; V1-V4 rendering is unchanged.
- V5 grade reconstruction now uses the same working-space `filmlike_clip` as rgbProc before converting the reference to AP1. Independently clamping AP1 channels invented green suppression/pink rings even with identity edits. The source radiance itself remains unclipped; only the grade-measurement reference is corrected. This is a targeted fix, not the planned replacement of ratio-based grading.

Verified: native Windows GUI/CLI build; `tools/filmlab_v5_probe.py` passed all checks; existing `tools/v4_probe.py` passed; native auto-analysis self-tests passed on a synthetic image and `DSCF8436.RAF`, including film sensitivity, curve sensitivity, and repeat determinism. Tests use isolated settings/cache directories and do not overwrite photographic sidecars. CLI invocation timing is not an interactive-preview benchmark.

Highlight regression: a +2 EV five-step bright ladder previously rendered all five patches as white. Its corrected 16-bit output means are 0.95330, 0.96270, 0.96755, 0.97029, and 0.97164. The test distinguishes export precision from 8-bit rounding (the brightest two samples share an 8-bit code). Warm-channel and positive-print-exposure clipping checks pass. The comparison's shadow/midtone maximum difference is zero 8-bit codes, and V4 output matches the prior binary exactly. Additional scan, paper, cinema, monochrome, half-mix, and Golden Hour 82% cases pass. The user's saved `DSCF8436.RAF` Golden Hour edit was also rendered successfully without changing its parameters. These tests protect rendered highlight range; they cannot restore detail already clipped in the source capture.

Color-transition regression: `tools/filmlab_highlight_probe.py` exercises 16-bit neutral/warm/tungsten gradients, colored emitters, and radial lights with and without halation. The previous implementation produced a green-channel dip of up to 0.073 on an increasing warm-light ramp; the final tested ramps have no such dip. Tests also cover ProPhoto, sRGB, ACESp1, and unclamped input; red/blue emitters retain color. The reported ring was reproduced and substantially reduced on `DSCF8535.RAF` using the user's unchanged saved parameters, with a film-disabled comparison. Unlike the first brightness-only fix, these tests also require bright neutral/warm emitters to converge toward white.

Still pending from the broader plan: reference-pair stock calibration, replacement of the V4 ratio-based grading reconstruction, a density-domain grain model, exhaustive crop/export color matching, generation-identity changes, candidate-render caching, long-session soak testing, and Linux/macOS validation. The new optical coefficients are bounded engineering approximations, not measured reproductions of commercial emulsions. Current analysis shares film/tone stages but is still a thumbnail pipeline, not proof of parity for every local or spatial effect.

## Scope and Baseline

Review the current Film Lab implementation, not the older workspace-root version. The newest local source checkout found was `.claude/worktrees/happy-wright-a920d5` at `ce4221e5c` (2026-09-09). This does not establish which executable is currently running. Implementation should start on a new `codex/film-lab-realism` branch from the verified current integration point, preserving other contributors' work.

The current implementation already has V4 scene-input plumbing, log-exposure density curves, dye coupling, spectral print approximations, format-dependent halation, image-anchored grain, and shared auto-edit decisions. Preserve and improve those foundations. Do not replace them with another generic LUT or rewrite unrelated RAW decoding/UI code.

Photographic aim: believable separation and color in a well-exposed negative or print, with luminous highlights and restrained material texture. Film must not automatically mean faded blacks, muddy shadows, orange skin, exaggerated saturation, or red outlines. A deliberately faded or high-halation look remains an explicit creative choice.

## Findings in Current Code

These are source-level observations, not newly reproduced runtime failures or measured performance results.

1. **Auto Film verification does not currently measure the final film rendering.** `verifySteepAutoEditExposure()` calls `measureCurveAnchors()`, which clears `rgbCurves`, lowers exposure for an 8-bit headroom probe, and estimates the original brightness using a display-gamma gain. It uses `Thumbnail::processFullThumbImage()`, whose engine path, `rtengine/rtthumbnail.cc`, does not invoke Film Lab. The verification trigger also relies on pre-look statistics. This is the highest-priority gap: a check cannot reliably reject a dark or washed-out film result it never renders. The existing probe can remain useful for initial metering, but not as final-look validation.
2. **Halation backing is inferred from development chemistry.** `filmPresetsV3()` uses a 1.30 versus 0.30 factor for motion-negative halation according to C-41 versus other processing. The film's construction during exposure and its development process are different properties. They should not be interchangeable controls.
3. **Halation and bloom share an artistic approximation.** V4 already retains above-white input, area-averages the highlight field, and scales radii by film format. However, it uses fixed 0.140/0.480 mm radii, a scalar highlight signal, source-subtracted/clamped blur fields, and substantially shared kernels for halation and bloom. These are useful approximations, not calibrated optical measurements for every stock.
4. **Film and user grading remain entangled.** `FilmLabV4SceneFetch::fetch()` reconstructs edits using per-channel Lab-derived ratios, with bounded gains, and reapplies display grading to the film output. Auto Film then compensates with fixed contrast/curve reductions. This deserves interaction tests before more recipe tuning; it is not yet evidence that every ratio produces a visible defect.
5. **Strength does more than mix the result.** It drives both final blending and `filmCharacterScale()`, affecting material effects as well. Also, a halation adjustment of zero is a stock-relative trim, not necessarily no halation. Clarify and separate these meanings without changing old saved edits.
6. **The spectral model is physically informed but partly generic.** Stock tables, dye lobes, receiver bands, and several interactions are authored approximations. More spectral arithmetic alone will not establish accurate film behavior without reference calibration.
7. **Grain has already been deliberately separated.** The standalone Grain tool has image-coordinate anchoring and preview attenuation. Auto Film intentionally does not add grain. Keep that choice; obsolete film-grain parameter writes should not become a second active grain pass.

Primary code locations: `rtengine/ipfilmlab.cc`, `rtengine/improcfun.cc`, `rtengine/improcfun.h`, `rtengine/improccoordinator.cc`, `rtengine/dcrop.cc`, `rtengine/simpleprocess.cc`, `rtengine/rtthumbnail.cc`, `rtengine/ipgrain.cc`, `rtengine/ipgraineffect.cc`, `rtgui/autoedit.cc`, `rtgui/thumbnail.cc`, and `rtgui/tools/filmpresets.cc`.

## 1. Establish Trustworthy Rendering and Measurement

Implement this before retuning stock recipes.

- Separate initial exposure metering from finished-look validation. The latter must retain the actual RGB curves, Film Lab parameters, output transform, and framing. Do not infer a nonlinear film render by darkening the input and multiplying the final 8-bit result back up.
- Add a small floating-point analysis render through the same relevant stages as the editor/export. It should expose unclipped scene statistics separately from final display-referred statistics. Reuse the existing engine; avoid another independent implementation of film mathematics.
- Verify whether the pre-clipping tap also contains camera-profile look operations, and document its exact color space, white point, exposure units, and valid range. Do not label any arbitrary pre-clipping RGB buffer as unmodified scene radiance.
- Ensure thumbnails and auto analysis actually run the selected film model, with correct full-image dimensions and reduced-resolution scale. Keep cheap embedded previews for initial loading, but never treat them as authoritative edited renders.
- Give render inputs a source/parameter generation identity, not just matching dimensions. A V4 render must not silently become a final display-input fallback because the scene buffer is missing. During loading, keep a clearly provisional preview and schedule the valid final render.
- Add tests proving that changing Film Lab changes the analysis result, that RGB curves participate, and that the same saved settings produce equivalent editor, thumbnail, crop, and export results after matching scale and color conversion.

## 2. Make Halation Material-Based

Halation should be simulated as secondary exposure caused by transmitted/reflected light, not as a universal red border. Film construction is independent of the processing bath: current manufacturer documentation explicitly describes an anti-halation undercoat replacing a removable backing. [Manufacturer construction reference](https://www.kodak.com/en/motion/page/ahu-announcement/)

- Add an internal anti-halation/base profile independent of process: suppression, wavelength-dependent transmission/return, core/tail distribution, and physical spread. Start with a few calibrated construction families rather than many speculative parameters.
- Apply virtual negative exposure before deriving both direct exposure and the halation field. Halation energy must respond coherently when exposure into the emulsion changes, including Film Lab's own exposure adjustment.
- Use nonnegative, normalized point-spread kernels and an explicit reflected-light budget. Model each layer's returned exposure from a limited share of transmitted light. Calibrate the approximation rather than claiming a universal radius or mandatory ring.
- Preserve highlight radiance above display white. Use a smooth response and film-layer saturation; a hard highlight threshold can remain an optional creative control, not the sole physical definition of halation.
- Treat spectral return and optical bloom separately. Bloom should redistribute incident light with its source color and its own optical kernel. Do not generate identical white haze around red, blue, and neutral lights or force bloom to follow a halation ring.
- Keep the existing film-format scaling and area integration. Improve subpixel/fractional radii and filtering so zoom changes do not abruptly switch halo width or make point sources disappear.
- Include sufficient surrounding-image support for detail crops and processing tiles. Bright sources just outside the visible crop must still contribute. Measure full-render-versus-crop agreement before choosing a padding/cache strategy.
- Default to subtle edge coloration and highlight spread. Strong red glow belongs to a deliberately weak-suppression profile, not all motion-negative or night presets.

Tests: neutral and saturated point sources; exposure ladders; daylight backlight; bright windows; large white skies; small lights near crop borders; fit-screen/100%/export comparisons. Check halo energy, radius, channel balance, smooth response, and absence of broad unintended veiling. Physical calibration requires controlled reference captures, not attractive screenshots alone.

## 3. Calibrate Film, Processing, and Print Separately

Use three explicit stages: emulsion response, development, and print/scan rendering. Do not ask one toe/shoulder control to compensate for all three.

- Fit per-layer log-exposure/density curves against published sensitometry and controlled reference scans. Represent base density, useful straight-line region, toe, shoulder, and channel crossover. Keep monotonic behavior and smooth gradients without forcing every stock to share contrast or white placement.
- Calibrate dye absorption and interlayer coupling, then print/scan response, as separate problems. Use step wedges, neutral patches, skin tones across a broad range, foliage, saturated fabrics, and colored lights under daylight, tungsten, and mixed illumination.
- Define a reference illuminant and viewing transform. Match density and output measurements under known conditions rather than comparing random internet scans with unknown processing and scanner corrections.
- Develop a slower offline reference model, then validate the current LUT-based approximation against it. A spectral approximation reconstructed from camera RGB cannot recover the original spectrum uniquely; describe the result as calibrated emulation, not an exact reconstruction.
- Specify exposure ownership: capture/negative exposure changes emulsion response; print exposure sets final placement; print contrast shapes the print. Push/pull should alter an explicitly calibrated process response, not act as another brightness slider.
- Replace ratio-based reconstruction only after interaction tests define the intended behavior. Introduce explicit, ordered grade operations behind a new model version instead of globally rearranging the existing editor pipeline.
- Use smooth hue-preserving output gamut handling. Protect skin transitions with soft confidence weights, without forcing every skin tone toward one hue or creating mask-like boundaries.

Manufacturer data provides sensitometry, spectral sensitivity, dye-density, granularity, and MTF references, but explicitly describes representative test conditions rather than a guarantee for every roll. [Technical data](https://www.kodak.com/content/products-brochures/motion-picture/KODAK-VISION3-5219-7219-technical-information.pdf) A useful open reference implementation also separates negative, print, and scan stages and notes limitations of datasheet-only matching. [Spectral simulation reference](https://github.com/andreavolpato/spektrafilm/blob/main/README.md)

## 4. Make Auto Editing Conservative and Look-Aware

- Retain one deterministic auto-edit entry point for editor buttons, context menus, hover previews, and batch actions. Analyze the same neutral source, not a stale embedded JPEG or a thumbnail carrying previous edits.
- Preserve crop, perspective, rotation, auto-level, and existing masks. Film remains opt-in. Repeating the same action from the same source must produce the same parameter set, independent of thumbnail readiness or selection order.
- Distinguish intentional low-key/high-key images from poor exposure. Use subject or spatial statistics where reliable, scene distributions, clipping, and source-noise risk. When confidence is low, favor a smaller correction rather than forcing every image toward the same median.
- Give exposure, shadow recovery, tonal contrast, and film-print contrast a shared correction budget. Constrain upper-curve lift and local slope; do not lift exposure, steepen the right-hand curve, and then counteract both with strong film compression.
- Choose from a small, stable set of suitable stock/output recipes using the existing continuous scene scores. Avoid winner-takes-all threshold jumps; keep choices deterministic. Do not mix unrelated physical stocks solely to smooth a UI transition.
- Render each Auto Film candidate with the actual final settings. Assess subject/midtone placement, useful dark-tone separation, highlight detail, new clipping, extreme chroma, skin shifts, and lost local contrast. An exposure-only check cannot detect all washout or color amplification.
- Allow at most a bounded corrective step and verification render. Correct the offending stage: print placement for dark output, print contrast for flatness, chroma handling for oversaturation. Do not endlessly bounce exposure between independent heuristics. If constraints cannot be met, fall back to the less aggressive candidate.
- Always validate Auto Film's finished look, but reuse its cached render for hover, click, and batch analysis. A cheap proxy may rank candidates; it must not certify a different pipeline as final.
- Keep grain optional. Source ISO should inform noise/exposure caution, not automatically prescribe simulated grain or fake film speed.

## 5. Improve Texture and Control Semantics

- Retain the separate Grain tool and its existing coordinate anchoring. Evaluate a density-conditioned stochastic model with calibrated grain-size distribution and inter-channel covariance; avoid merely adding uniform RGB noise. Resolution-independent grain research provides a reference model, but production evaluation must include its cost. [Grain research and implementation](https://www.ipol.im/pub/art/2017/192/)
- For a future physical-grain mode, place the model in a documented density stage and share its settings with the standalone tool. Never run both the old noise pass and a new emulsion-grain pass for the same effect.
- Compare spatial frequency/power and density-dependent variance against reference scans. Account for existing sensor noise; keep the same realization while dragging sliders, zooming, cropping, and exporting.
- Preserve Overall Mix as predictable blending. Add separate material-character/halation control only where necessary; increasing halation should not silently raise black level, change saturation, or strengthen the whole tone curve.
- Keep a compact UI with existing film controls and advanced options collapsed. Distinguish stock-relative trims from absolute amounts, with accurate resets and aliases. No gate weave, dust, scratches, or accidental registration shifts in a still-photo realism default.
- Continue invented stock/preset names. Source references in engineering documentation are not product profile names or claims of exact commercial-stock matching.

## 6. Performance and Reliability

- Cache immutable density/print LUTs by all relevant parameters, model version, and color-space context. `makeV4PrintLUT()` currently runs when the film stage runs; measure its cost before adding cache complexity.
- Cache the reusable radiance pyramid and kernels separately from cheap appearance adjustments, with explicit source/parameter invalidation and bounded memory. Avoid retaining full-resolution images per hover candidate.
- Coalesce superseded preview requests, cancel old image generations, and keep filmstrip/UI interaction on the UI thread free of render work. A completed current generation must always progress to the finalized full-resolution result.
- Use the same film response at interactive and final quality. Lower spatial resolution or sampling cost may differ; stock choice, exposure, and tone/color mathematics must not.
- Benchmark cold/warm switching, drag-to-preview latency, finalization, export, and batch processing on the existing Windows machine and Linux/macOS builds. Log stages and cache hit rates. Set absolute timing budgets after collecting a baseline; no speed claims are supported by this planning review.
- Soak-test repeated switches, before/after, hover/cancel, masking, and exports. Check for increasing latency, unbounded memory, stale previews, and starvation of the final render.

## Delivery Order and Acceptance

1. **Rendering truth and baseline tests.** Add engine-backed analysis, fix missing film evaluation, separate metering from final verification, and establish a consented local reference corpus. Preserve existing artistic output outside corrected analysis paths.
2. **Auto Film safety.** Add finished-look constraints and bounded correction; validate single/batch and hover/click parity. Avoid more per-scene tuning until the analysis actually sees the film result.
3. **Versioned halation model.** Decouple construction from process, separate bloom, calibrate kernels, and verify crop/scale/exposure behavior.
4. **Calibrated film/print model and optional texture.** Refine sensitometry and color against references, clarify controls, and evaluate density-grain improvements. Do not silently migrate existing V1-V4 profiles.
5. **Performance, compatibility, and release gates.** Optimize measured hot spots; finish cross-platform and long-session validation before enabling the new model for new edits.

Extend `tools/v4_probe.py`, `v4_color_probe.py`, `filmskin_probe.py`, `filmzone_probe.py`, `autoedit_probe.py`, and existing film probes. Replace hard-coded executable paths with configured paths. Python copies of C++ formulas are useful for exploration but are not the authoritative regression oracle. Revisit tests that enforce universal contrast/white targets: those should distinguish intentional look properties from genuine pipeline failures.

Required gates:

- New auto results are identical across single selection, multi-selection, editor, and context-menu invocation for the same input state.
- Hover never persists edits; click commits the candidate actually previewed; leaving cancels it. Old work cannot overwrite a newly selected image.
- Final-analysis metrics agree with matched editor/export renders within established numerical tolerances; film and RGB-curve toggles demonstrably affect the analysis.
- Halation is continuous with exposure, format, and scale; no seams or missing off-crop contributions. Old process selections do not secretly become new backing selections.
- No new dark-scene crushing, aggressive shadow lift, clipped sky, broad haze, skin discontinuity, or extreme color amplification across the reference set. Review failures by scene class, not just aggregate averages.
- Old saved profiles retain their prior model behavior. New model selection is explicit until validated. No default film profile is applied on RAW open.
- No regression in finalized-preview reliability or sustained-session latency/memory on the benchmark workflow.

The implementation status above records completed work and tests; the remaining sections retain the broader proposed design. Do not treat uncompleted reference calibration or performance goals as verified results.
