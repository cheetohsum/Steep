# Smart Tools: Quality, Feedback, and Responsiveness

Status: investigated and planned, not implemented. September 16, 2026.

Scope: Remove Object, Remove Reflections, Remove Dust, and Generative Fill in the current native GTK frontend. No app restart, build, model replacement, or photo edits performed for this planning task. Existing Film Lab work is untouched.

## Intended Outcome

Repairs should preserve the photo's geometry, illumination, texture, and grain; be identical at fit view, 100%, and export; and give truthful, responsive feedback while computing. A checkmark must mean that the final repair has actually reached the displayed image, not just that a stroke exists.

The supplied screenshot shows smeared/repeated structures and visible transitions in an out-of-focus scene. It cannot by itself identify tile seams versus mask seams, inference artifacts, or display quantization. The exact source photo and saved repair are requested as a regression fixture; do not assume this is the previously supplied DSCF8535.RAF.

## Verified Current Behavior

| Area | Current implementation | Consequence / investigation target |
| --- | --- | --- |
| Tool identity | Remove Object and Generative Fill use the same LaMa engine; Fill mainly requests more surrounding context. `rtgui/tools/spot.cc:308`, `:360` | There is no separate prompt-conditioned generation backend. Improve the existing local repair pipeline first. |
| Installed model | Read-only inspection of `build-hw/Release/models/lama_inpainting.onnx`: inputs are float NCHW with fixed 512x512 spatial dimensions; output is float, multiplied by 255 and clipped, without a final integer conversion. | Current 640px tiles are resized to 512px. Float output means the screenshot is not evidence of an 8-bit intermediate. |
| Model adapter | Native size is tried speculatively; failures fall back to a square 512 input. Output range is inferred from image brightness. `rtengine/aiinpainting.cc:283`, `:380` | A rectangular patch can be distorted; output range must be a model contract, not a scene-dependent guess. |
| Large fill | Global reduced-resolution structure plus 640px tiles, stride 512, linear overlap ramps, and a fixed-radius box-blur frequency split. `rtengine/spot.cc:1202` | Test model-induced lines, tile disagreement, blur footprints, and transitions separately. Tiling exists already; simply adding overlap is not a complete fix. |
| Patch reuse | Full-image-coordinate patches, LRU refresh, same-key in-flight deduplication, and limits of 24 patches / 36M cached pixels already exist. `rtengine/spot.cc:962` | Retain these foundations. Missing/oversized patches still fall back to a separate per-view inference path. |
| Preview composition | `blitAIPatch` averages patch color and mask independently, then multiplies them. `rtengine/spot.cc:1630` | `mean(color) * mean(mask)` differs from `mean(color * mask)` around feathered edges. Also test clipped block coordinates at patch boundaries. |
| Mask boundary | Union of brush discs with linear feathering; prior stroke cores are cut out at a hard radius. `rtengine/spot.cc:672`, `:1485` | The overlap policy can prevent deliberate touch-up of an earlier repair and creates abrupt boundaries worth testing. |
| Model color | Full-resolution path probes a camera-to-sRGB matrix, but skips that mapping if the profile is nonlinear. Legacy per-view path does not perform that same mapping. `rtengine/spot.cc:1074`, `:1521`, `:1678` | Color behavior can differ by profile and fallback path. Bright values are clipped by the model adapter. |
| Applied badge | A literal checkmark is appended to the button label when the entry count is nonzero. `rtgui/tools/spot.cc:800` | No separately styleable checkmark, and it can appear before inference succeeds. |
| Busy feedback | Rotating cursor arc exists. A 70ms timer redraws the canvas; completion depends on general render busy/idle and a 120-second backstop. `rtgui/tools/spot.cc:97`, `:542`, `:592`, `:1570` | It is not tied to a specific patch reaching the screen. The cursor is not a reliable persistent status location. |
| Reflections | Model-free, spatially varying veil subtraction from a blurred min-channel estimate. `rtengine/spot.cc:817` | Useful for glare, not recovery of scene detail hidden by a structured reflection. |
| Dust | Dark-blob detector on the current preview; scans up to 40 candidates and chooses nearby healing sources. Called synchronously while acquiring the processing mutex. `rtgui/tools/spot.cc:860`, `rtengine/improccoordinator.cc:3360` | Can wait behind rendering on the UI thread; small real scene features can be mistaken for dust. |

## 1. Reproducible Quality Baseline

- Capture source image, sidecar, model checksum, processing profile, and exact stroke geometry in an isolated test fixture. Never overwrite the user's sidecar.
- Add a native CLI/self-test entry point that runs the same repair path as the editor and exports optional diagnostic buffers: model input/output, inference mask, blend mask, structure pass, tile residuals/weights, and final composite.
- Compare fit view, 100%, and 16-bit export before looking at 8-bit presentation. This distinguishes an actual repair defect from display banding.
- Build synthetic fixtures for flat gradients, defocused backgrounds, straight and diagonal edges, corners, repetitive texture, warm/cool highlights, small dust, image borders, and overlapping strokes.
- Use a model-free fake inference result for exact compositor and cancellation tests. Keep real-model visual tests as a separate tier so basic CI does not require a 200MB model.

## 2. Truthful Status and Hover Styling

Implementation areas: `rtgui/tools/spot.{cc,h}`, `rtgui/toolpanelcoord.cc`, preview delivery, `rtdata/themes/common/widgets.css`, and localization.

- Replace the appended checkmark character with a separate symbolic icon widget in a fixed-size trailing status slot. Preserve the existing tool grid and button positions.
- Give the whole tool button consistent hover, pressed, checked, keyboard-focus, disabled, and error styles. Give the status icon its own hover treatment and tooltip, without turning it into a misleading Apply button. Hover must never apply, reset, regenerate, or create a history entry.
- Use existing theme tokens, a restrained background/contrast change, and approximately 120ms color transitions. Keep the status and reset targets separate so neither steals the other's hitbox. No width changes when the spinner becomes a checkmark.
- Keep the existing brush arc, but add a persistent small circular spinner in the tool's status slot. It remains visible when the pointer leaves the canvas. Avoid a new full-image veil or blocking modal.
- Track each repair through `queued -> preparing -> generating -> blending -> presenting -> ready`, with explicit `failed` and `cancelled` states. Associate updates with photo/session ID, repair ID, and generation ID.
- Show Ready only after the matching final patch has been composited and its preview delivered. A fast draft or unrelated render finishing must not clear the spinner. A timeout must report a delayed/failed job, not fake success.
- For multiple repairs, retain an applied indication for existing ready work while accurately showing pending work; do not replace all status with a single unqualified checkmark.
- Use indeterminate animation for model loading/inference; use tile completion only where measurable. Never invent percentage progress. Stop timers when hidden/idle and honor disabled GTK animations.
- Repaint only the affected overlay/status area where practical; verify that the animation does not repeatedly reprocess the photo or force expensive full-canvas work.
- Provide Retry/Cancel on explicit actions, preserve Undo, and report missing/loading/failed model states without silently substituting a flat erase.

## 3. One Repair Across Every View

Implementation areas: `rtengine/spot.cc`, `aiinpainting.{cc,h}`, `improccoordinator`, `dcrop`, and export integration.

- Make the full-image repair job the authoritative result. Fast previews may show a marked draft, but zooming, panning, before/after, or export must not independently invent a different repair.
- Do not call expensive per-view inference merely because a final patch is pending. Display the last valid composite or a cached draft and schedule/subscribe to the authoritative job.
- Replace the >9M-pixel fallback with bounded tiled/streamed processing or an explicit quality/resource limit. Never silently switch final output to a view-dependent, lower-quality algorithm.
- Fix downsampling mathematically: store or derive the premultiplied repair delta `mask * (repair - source)` at the same pipeline stage, downsample it with the same footprint as the base, and add it to that base. This avoids independent mask/color averaging and preserves unchanged pixels outside the repair.
- Validate coordinates for rotated images, crops, image borders, non-integer fit ratios, and partial sampling blocks. Respect entry opacity exactly once.
- Include source revision, upstream dependencies, model fingerprint, algorithm version, and ordered repair dependencies in cache identity. Exclude downstream exposure/film controls that do not change inference inputs.
- Make pending-key cleanup exception-safe. Bound total working memory including inference activations and temporary buffers, not just the existing ~576MB patch cache.
- Split short cache-bookkeeping locks from image-source synchronization. A long source pull must not hold the global cache mutex and block unrelated cache hits.
- Keep shared model metadata immutable after initialization, or properly synchronized. `dynamicDims` and readiness currently deserve a concurrency audit.

## 4. Finalized Fill Quality

Implement and measure these independently so regressions can be attributed to one change.

### Model input contract

- Read supported tensor shapes and types at load time. Record output range explicitly for the known model fingerprint; reject or explicitly validate unknown exports. Remove brightness-dependent output-range guessing.
- With the installed fixed-size model, preserve aspect ratio using documented padding/cropping and shared pixel-center conventions. Do not stretch a wide/narrow patch to a square. Compare reflect and replicate padding at true image edges.
- Separate the binary inference mask from the soft compositing mask. Expand the inference region enough to remove object fringes, but confine the visible repair to the intended feathered area.
- Use a consistent, reversible model-space adapter for both preview and final paths. Handle nonlinear input profiles explicitly rather than falling back to camera primaries. Test superwhite/HDR context and avoid destructive channel clipping before generation.

### Geometry and corners

- Keep a shared global structure pass. Verify that local tiles have enough unmasked context; a fully masked tile has no reliable scene structure to recover, even if a seed image was passed in.
- Benchmark tiles that genuinely match the installed model's 512px contract against an independently validated dynamic-resolution export. Do not claim 640px/native detail while resizing it away.
- Use continuous overlap weights with correct outer-edge handling. Normalize contributions and inspect weight maps; blend disagreement must not create doubled lines.
- Replace the fixed box-blur detail split only after ablation testing. Evaluate a smooth multiscale reconstruction that retains coherent global edges and suppresses tile residuals where they disagree with those edges. Prefer existing image-processing helpers where suitable.
- Test a narrow, boundary-constrained color/low-frequency correction for remaining transitions. Do not run unconstrained color matching or Poisson blending over the whole fill; that can change the intended illumination or bend contrast around corners.
- Use a continuous distance-based feather for the actual swept stroke. Avoid simple additional blur that broadens the repair onto protected features.
- Preserve existing overlap behavior for legacy edits, but add an explicit refinement of the selected repair so users can correct a bad corner instead of having their new stroke excluded by prior coverage. Record changes as one undoable action.

### Texture and banding

- Retain float precision through inference, transforms, masks, blending, and caching. Inspect gradients before adding noise.
- Estimate texture/noise from several clean surrounding regions, excluding edges, dust, and other repairs. Current matching uses a green-channel residual and identical Gaussian noise in all channels; distinguish sensor noise from useful texture.
- Match luminance-dependent noise and spatial frequency gently and deterministically in image coordinates. A clean defocused region must not gain coarse grain just to conceal a seam.
- Apply dither only at final output quantization if a demonstrated precision issue remains. Do not bake dither into the patch or use it to hide structural artifacts.
- For ambiguous removals, offer a small optional set of alternative context/scale candidates and store the chosen result/recipe. Re-running the same deterministic LaMa input is not a meaningful new variation.

## 5. Tool-Specific Improvements

### Remove Object

- Add optional object-aware selection refinement using the existing segmentation infrastructure, with add/subtract brush correction. Keep precise manual selection available.
- Treat cast shadows/reflections of the removed object as optional expanded selection, not automatic changes outside the user's region.
- Select context based on surrounding edges, available clean pixels, and mask size. Prefer conservative small-object repair over a large reconstruction when possible.
- Add inspectable repair selection and a per-repair reset/retry. Keep the existing reset-all control clearly distinct.

### Remove Reflections

- Improve the existing glare reduction first: edge-aware veil estimation using the existing guided-filter helper, bounded subtraction, and shadow/highlight/chroma protection.
- Expose a compact strength control and an affected-area preview; keep the unpainted image unchanged.
- Evaluate structured reflection separation as a distinct future backend with a labeled paired-image benchmark. A single photograph can contain unrecoverably occluded detail; do not promise restoration where only plausible reconstruction is possible.
- Do not relabel a stronger haze subtraction as true AI reflection removal. Any new model needs a separate accuracy, speed, packaging, and weight-license review.

### Remove Dust

- Move detection off the GTK thread. Copy a stable preview snapshot under a short lock and release it before analysis; discard stale results when switching photos.
- Add multiscale detection with confidence, size, circularity, and local texture/edge rejection; refine confident candidates at source resolution.
- Review low-confidence candidates instead of automatically healing them. Preserve real small scene detail and avoid selecting new source patches that contain other candidates.
- Expose a compact sensitivity control and candidate overlay. Support dismissing individual candidates, rescanning without duplicates, and one-step undo for the scan.
- Avoid silently stopping at 40 results: use a visible count and bounded incremental review for heavily dusty frames.

## 6. Responsiveness and Reliability

- Use a bounded worker queue with current-photo jobs prioritized; coalesce obsolete jobs and cancel work on undo, reset, image switch, and shutdown. Do not launch one competing inference per view.
- Add per-job ONNX RunOptions plus cancellation checkpoints between tiles and compositing phases. ONNX supports terminating calls sharing a RunOptions instance; give each logical job its own instance so cancelling one photo cannot abort unrelated export work. Measure cancellation latency with the installed provider. [ONNX Runtime reference](https://onnxruntime.ai/docs/api/c/struct_ort_1_1_run_options.html)
- Preserve the existing CPU thread budget initially. Measure interaction latency and memory before considering GPU execution providers; acceleration is optional and must retain a CPU fallback.
- Keep filmstrip scrolling, photo switching, and ordinary edit controls responsive while repairs compute. Inference completion must not overwrite edits made after the job began.
- Add targeted job tracing: queued/start/end times, model dimensions, patch/tile count, cache hits, cancellation, memory high-water mark, and preview presentation generation. No image upload or cloud dependency.
- Preserve old repair output through an explicit algorithm version/default for existing sidecars. Offer a deliberate upgrade/recompute; do not silently regenerate all prior saved edits using a changed model or blending algorithm.
- Consider lossless disk-backed repair caches after the memory/job contract is stable, with a size limit, clear-cache control, atomic writes, and model/source version invalidation.

## 7. Validation and Release Gates

- UI: checkmark hover/focus visible on Windows, Linux, and macOS; spinner works with cursor off-image; no target overlap or layout shift at 100%, 150%, and 200% scaling and narrow sidebar widths.
- Status: warm/cold model start, missing/corrupt model, failed inference, timeout, cache hit, rapid repeated strokes, undo/reset during generation, switching photos, and app shutdown. No false Ready badge and no stale result publication.
- Composition: fake-model tests verify unchanged pixels outside support, exact identity bypass, mask-weighted resampling, border/corner behavior, entry opacity, and deterministic overlap handling.
- Output consistency: the same stored repair must agree across fit preview, 100% view, before/after, reopen, 16-bit export, and 8-bit export within the known resampling/quantization tolerance. No repeat inference on a warm pan/zoom or downstream exposure adjustment.
- Quality: compare line/corner continuity, boundary color/gradient discontinuity, noise spectrum, retained texture, and visible halos. Use human A/B review as well as metrics; a plausible fill need not match hidden ground truth pixel-for-pixel.
- Performance targets, to be measured rather than claimed: immediate stroke/status feedback; UI event p95 below 100ms during processing on the test machine; no monotonic memory growth during a 100-operation edit/switch/undo session; and no increase in warm repair latency after cache saturation.
- Protect existing film, exposure, crop, rotation, masks, and raw loading behavior with the current regression suite. Source files and sidecars in fixtures remain unchanged.
- Run native tests on each supported platform before claiming parity. Linux/macOS source compatibility is not proof of runtime or visual parity.

## Delivery Order

1. Add fixtures/diagnostics and reproduce the reported artifact. Lock down the model contract and compositor tests.
2. Add repair-specific job status, styled checkmark/spinner, cancellation, and error states. Remove blocking dust detection and per-view duplicate inference.
3. Correct resampling, mask composition, aspect-ratio handling, and corner/overlap transitions. Validate with the current model first.
4. Improve context selection, multiscale structure/detail blending, conservative texture matching, dust review, and glare controls.
5. Benchmark an optional higher-quality model/refiner only if remaining artifacts are demonstrably model-limited. Ship it only after quality, responsiveness, reproducibility, package size, and licensing checks.

Use small separately testable changes; retain existing behavior for old saved repairs unless explicitly upgraded. Build/relaunch only when implementation is requested, not during this plan.

## Research Boundaries

LaMa's upstream work emphasizes broad image context and reports high-resolution generalization. That motivates retaining coherent context; it does not mean our fixed-512 ONNX export accepts arbitrary shapes. [LaMa authors' repository](https://github.com/advimman/lama)

Feature-refinement research explicitly addresses the conflict between coherent downscaled structure and high-resolution detail. It is an optional experiment, not a drop-in equivalent to our current high-pass tile blend; its inference-time optimization would need separate runtime support and benchmarks. [Feature Refinement to Improve High Resolution Image Inpainting](https://arxiv.org/abs/2206.13644)
