# Interactive Edit Latency Review

## Measurements

Windows build with the RAW-open improvements, 26 MP DSCF8535.RAF copied to
isolated fixtures, separate settings/cache, 1386 x 974 window. Exposure was
driven through the actual Adjuster debounce at 30 ms intervals. Source RAWs,
source sidecars and the user's running editor were not changed.

The trace measures submission to CropHandler's updated canvas pixbuf/redraw
notification, not GPU presentation. It excludes the preceding widget debounce.
The first image-load frame is excluded. Samples are local diagnostic runs,
not a multi-camera or long-session benchmark.

| Exposure test | Published edit frames | Median latency | Observed range |
| --- | ---: | ---: | ---: |
| Edited profile, Film Lab enabled | 13 | 400.1 ms | 272.5-437.5 ms |
| Same profile, only Film Lab disabled | 65 | 70.0 ms | 39.3-93.5 ms |
| Neutral profile | 78 | 22.7 ms | 14.6-42.6 ms |

A native tone-curve drag with the edited profile produced two updates at
324.3 and 519.6 ms; the second waited about 302 ms for the first pass.
This small curve sample confirms the shared rendering bottleneck but is not
enough to establish curve-drag percentiles.

## Findings

1. **Film Lab dominates this edited-profile workload.** The whole-image
   overview is processed before the visible crop. Its post-RGB/film region
   costs around 85-95 ms; the visible crop's corresponding late-detail region
   costs around 135-150 ms. These regions include neighboring operations, so
   the isolated Film Lab on/off comparison is the stronger attribution.
   See `rtengine/improccoordinator.cc:2592` and `rtengine/dcrop.cc:1939`.
2. **The center image waits for work that mainly serves the overview.**
   `ImProcCoordinator::updatePreviewImage` reaches the crop loop only after
   the overview's full effects chain. Deferring analysis publication at the
   end already avoids some color conversions, but does not avoid its earlier
   Film Lab computation. See `rtengine/improccoordinator.cc:3128`.
3. **Pacing adds delay and does not eliminate render backlog.** Heavy passes
   measured around 260-295 ms. Adaptive pacing caps the maximum debounce
   floor at 150 ms and raises the quiet-period minimum to 75 ms. The engine
   coalesces pending parameters, so there is no evidence here of an unbounded
   edit queue, but an in-flight pass cannot use newer values. Curve changes
   bypass the Adjuster debounce and can also wait behind an older render.
   See `rtgui/toolpanelcoord.cc:3981`, `rtgui/delayed.h`, and
   `rtgui/widgets/curves/mycurve.cc:132`.
4. **Final refinement has a separate 650 ms debounce.** If the shared busy
   flag remains set, its timer can defer the request to the eighth poll,
   about 5.2 seconds. That is a code-path risk, not an observed 5.2-second
   stall in these tests. It does not explain the already-measured interactive
   lag. See `rtgui/editorpanel.cc:4996`.

GUI parameter handling was generally 0.4-2.5 ms; canvas scaling about 6 ms
for exposure or 13 ms when a curve needed analysis pixels. RAW preprocessing
and demosaicing were effectively skipped during ordinary tone edits. The
RAW-open optimization is not the dominant delay here.

## Recommended Implementation Order

1. Profile and optimize Film Lab's repeated pixel work without changing the
   look: redundant scene conversions, six-plane spatial sampling and blur
   setup are candidates. Hoist per-frame invariants out of pixel loops. Prove
   any change against saved renders, including bright boundaries and 100% zoom.
2. Prioritize the visible crop after its shared curve/state dependencies are
   ready. Do not blindly move the crop loop: wavelet/CIE state, monitor color
   transforms and shared `ipf` scale must stay valid. Pace overview effects
   separately during drags, always reconciling histogram/navigator at rest.
3. Make interactive scheduling explicitly latest-value and completion-aware.
   Keep at most one pending update, distinguish dragging from a single click,
   and flush the final value on release. Avoid simply lowering all timeouts,
   which would increase submissions without making a 270 ms render faster.
4. Decouple settled refinement from the unreliable aggregate busy flag, using
   the existing processor coalescing and generation guards. Retain full-detail
   completion/retry verification, rapid-switch cancellation and original quality.

## Verification For Changes

- Repeat neutral, identical-profile Film Lab on/off, curve and exposure tests.
- Test fit view, 100% zoom, large windows, before/after and local masks.
- Match settled output pixels and verify final slider/curve values survive
  rapid drags, undo/reset, navigation and a long editing session.
- Check CPU/memory and thumbnail responsiveness as well as canvas latency.
- Test Bayer and X-Trans files, plus Linux/macOS before claiming parity.

`tools/edit_latency_probe.ps1` reproduces the exposure test. Use a new label
for each run; `-WithProfile` copies the original example's current sidecar.
For a controlled comparison, copy the baseline fixture's sidecar into the
new fixture before launch and omit `-WithProfile`. `-Steps 0` leaves the
isolated image ready for native curve testing. Logs are under
`test-output/edit-latency/`. Close test windows normally after measurements.

## Implemented And Tested

The isolated Windows candidate now includes:

- Film Lab V3-V5 hoists the process-name comparison and coupler strength out
  of the pixel loop. Full-size optical planes use direct reads instead of
  six redundant bilinear samples per pixel. Reduced planes retain the
  original interpolation; no film parameters or rendering quality were reduced.
- Master/R/G/B curves, RGB enable and luma-mode refresh from the tone stage,
  retaining local-adjustment refresh but avoiding source/repair/HDR rebuilds.
- Sustained slider pacing uses one measured render pass, capped at 150 ms,
  instead of three quarters of a pass. The existing latest-value coalescing
  and immediate release flush are retained.
- Final refinement queues after 250 ms of quiet instead of 650 ms. It no
  longer polls the aggregate busy flag. Generation/image guards and bounded
  completion retries are unchanged. Image-open refinement stays at 35 ms.
- Faster short exports exposed a subject-model lifetime race. Its readiness
  is now atomic, and its shared instance has process lifetime, matching the
  inpainting engine. This prevents static destruction of a model whose
  detached loader is still initializing. It adds no per-image allocation or
  shutdown wait. An intermediate join-at-exit approach was rejected after
  testing exposed Windows teardown deadlock and multi-second exit delays.

No crop/overview reordering was needed for these gains. That broader change
remains deferred because of shared CIE/wavelet state and processor scale;
the image-quality and dependency risks need separate coverage.

### Measured Results

Same 26 MP fixture, profile and 1386 x 974 window. The controlled second
baseline used the exact same copied RAW path as the candidate. Samples
measure submission to canvas-pixbuf notification, excluding widget debounce.

| Test | Baseline | Candidate |
| --- | ---: | ---: |
| Film exposure, 100 inputs / 30 ms | 303.1 ms median, 14 frames | 96.2 ms median, 34 frames (film optimization) |
| Film exposure, 500 inputs / 30 ms | n/a | 103.8 ms median, 137 frames (final pacing) |
| Master-curve drag, two published updates | 318.7 / 497.3 ms | 102.7 / 139.5 ms |
| Same edited profile, Film Lab disabled | 70.0 ms median | 67.8 ms median |
| Neutral exposure | 22.7 ms median | 22.7 ms median |

One intermediate exposure rerun with the old pacing measured 162.9 ms,
despite render passes around 90 ms, motivating the pacing adjustment.
The 500-input run ended with the last requested exposure and a settled
publication about 0.35 seconds after its final submission. No timeouts or
missed final publication were observed in the completed GUI probes.

### Quality And Reliability

- Saved fit-view pixels: 22 of 529,254 pixels differed by at most 1/255.
- Saved 100% crop: 15 of 709,560 pixels differed by at most 1/255.
- Matching final master-curve settings: 3 of 529,254 pixels differed by
  at most 1/255. Saved curve coordinates match exactly.
- All 13 sixteen-bit CLI reference renders are pixel-identical: neutral,
  ten bundled film looks, repeated-look and switched-look cases.
- Two complete final export suites passed, including highlight/ramp and
  monochrome checks. Short exports took roughly 0.55-0.84 seconds including
  process startup, without the rejected shutdown wait.
- Native GUI fit/100% and before/after toggle checks completed normally.
- `steep-interactive-refresh-tests` passed normal, explicit legacy-off and
  legacy-on mappings. Native GUI and CLI targets build successfully.

Trace and frame folders: `film-optimized`, `identical-baseline`,
`curve-optimized`, `paced-final`, `no-film-final`, `neutral-final`,
`export-baseline`, `export-final`, and `export-final-repeat`, under
`test-output/edit-latency`. `tools/edit_latency_results.py` summarizes logs
and compares saved frames. `edit_latency_probe.ps1 -Photo` can reuse one
isolated copied fixture for identical-path comparisons; never point this
editing probe at an original photo.

This is local Windows/X-Trans verification, not a multi-hour soak, broad
camera/mask-configuration test, or Linux/macOS validation. No additional
claims about those cases are made. Existing worktree edits were preserved.
The installed Release binary was not replaced or restarted. The candidate
is staged at `test-output/view-switch/runtime/steep.exe`.
