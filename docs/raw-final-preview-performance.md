# RAW Loading and Final Preview

## Measured Pipeline

Windows, 26 MP Fuji X-Trans RAF fixture copied from DSCF8535.RAF, isolated
settings/cache, full GUI at 1386 x 974. The user's running editor and original
photos were not modified. Logs and rendered PPM frames are under
`test-output/view-switch/raw-*-baseline` and `raw-*-candidate`.

1. FilePanel acquires the foreground RAW gate and takes a decoded image from
   the adjacent/recent cache, or runs InitialImage::load. Foreground decoding
   took about 300 ms here; the existing four-thread background decoder took
   about 800 ms. A preload is decoded sensor data, not a finished RGB image.
2. EditorPanel attaches the processor and loads the photo's parameters. The
   fast initial pass preprocesses and demosaics the RAW, publishes a small
   whole-image preview, then enables the visible detail crop.
3. The full-quality pass uses the selected sidecar demosaic method. In this
   fixture it took about 300-330 ms for demosaicing, 35 ms for preprocessing,
   and another 50-70 ms for downstream work and the visible crop.
4. The crop paints the main canvas. Whole-image publication updates the
   navigator/analysis views. The GUI watchdog observes completion on a 1.5 s
   polling interval: its "published" timestamp is not the rendering time.

## Confirmed Avoidable Delays

- Opening an image used the slider-settling debounce: a 650 ms pause after
  the initial preview, even with an idle processor and a decoded cache hit.
- The adjacent decoded cache had only one slot. Left and right neighbors
  could not both be warm, despite its existing 384 MiB budget accommodating
  two of these approximately 148 MiB decoded RAFs.
- Nondirectional preload scheduling waited 1800 ms before even checking the
  foreground-priority gate, and added 900 ms between neighbors.
- The recent-image cache deliberately flushes processed RGB buffers. Going
  back avoids RAW decoding, but still repeats demosaicing. This is unchanged.

## Implemented

- Image opens queue final refinement 35 ms after the first engine preview,
  allowing an initial frame to display without waiting for a slider debounce.
  They do not poll the shared busy flag first; processor flags coalesce behind
  any in-flight work. Session/generation cancellation and the bounded final
  refinement watchdog remain intact. Slider edits retain their existing delay.
- Allow two adjacent decoded images within the same 384 MiB limit. Ordinary
  directional navigation reserves a backtrack slot; rapid forward navigation
  can prioritize ahead. Recently cached images are not intentionally decoded
  again. Oversized images still obey the byte cap, even if only one fits.
- Check preload readiness after 250 ms, and use a 125 ms inter-image delay.
  Existing foreground ownership, active-editor checks, quiet periods,
  cancellation, worker count and thread limits are unchanged.
- Add an opt-in `settledPublished` trace with engine duration, rendered-crop
  count and filename so completion can be measured without the GUI poll delay.

No demosaic algorithm, color transform, exposure, preset, or export behavior
was changed. Full-quality demosaicing is still required; this is not a shortcut
that leaves the editor at fast-preview quality.

## Verification

Matched two-photo sequence, same settings and warm thumbnail cache:

| Stage | Baseline | Candidate |
| --- | ---: | ---: |
| Cached RAW: first engine preview after editor open | 231 ms | 240 ms |
| Wait before requesting final quality | 653 ms | 41 ms |
| Final-quality processing and publication | 425 ms | 432 ms |
| Cached RAW: total to final-quality publication | 1309 ms | 713 ms |

The decoded-cache handoff itself took 3 ms in both runs. These are measured
local samples, not promises for other sensors, effects, disks or view sizes.

- Whole-image preview frames matched byte-for-byte across the runs.
- The 100% zoom crop (804 x 615) matched byte-for-byte. At fit zoom, the final
  520 x 647 crop differed by one 8-bit level in only two color-channel samples;
  no sample differed by more than one level.
- Opening previously unvisited Sample-010 warmed both Sample-009 and
  Sample-011. Two entries totaled 310,802,304 bytes, below the unchanged cap.
  Navigating left used the decoded cache; reversing direction used the recent
  cache. Both finished with a rendered detail crop.
- Baseline and candidate pixel-validation windows closed with exit code 0.
- A 48-transition browser/editor stress run at 350 ms intervals exercised
  cancellation while advancing through photos. Returning to the editor
  completed the last photo's final-quality pass with one rendered crop;
  100% zoom then completed a `skip=1` crop in 59 ms. Normal shutdown exited 0.
  The harness ends in the browser, so the final hidden editor intentionally
  defers its initial processing until it becomes visible again.

Reproduce with `tools/view_switch_probe.ps1 -Cycles 2 -PeriodMs 2500 -Advance
-WaitForExit`, setting `STEEP_EDIT_TRACE=1`, `STEEP_PIPELINE_TRACE=1` and an
existing `STEEP_DUMP_CROP` directory. Choose the executable explicitly. Test
fixtures/settings are isolated; never point the benchmark at user sidecars.

## Remaining Work

Decoded preloads still do not contain demosaiced RGB, so final quality is not
instant. A subsequent optimization could retain one recently processed RAW
with an explicit preprocessing/demosaic parameter fingerprint and bounded
memory accounting. RGB alone costs about 300 MiB for this sensor, before RAW
and temporary buffers; simply retaining everything would worsen long-session
memory pressure. That larger cache change is deliberately not included here.
