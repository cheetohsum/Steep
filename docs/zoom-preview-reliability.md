# Zoom preview reliability

## Findings

- `CropHandler::setZoom` trusted `getHighQualComputed()`, which treats the
  Sidecar preview preference as proof of completed detailed processing. That
  is not valid during progressive RAW loading. Crossing into 100% now queues
  a high-quality monitor refresh unconditionally; the engine's existing RAW
  cache still decides whether demosaicing is necessary.
- A bare `M_HIGHQUAL` request could miss the preview/crop processing branches
  in Sidecar mode. The coordinator now includes `M_MONITOR` for these requests.
- Crop worker ownership and pending requests were unsynchronized booleans.
  A request arriving as the worker exited could be lost. A small synchronized
  state machine now makes request coalescing and worker handoff atomic together.
- A rejected crop from an old zoom cleared the pending image buffers before
  checking its geometry. Rejected frames now leave the accepted pending frame
  alone. The GUI callback also leaves the displayed frame intact when it has
  no replacement to publish.

## Verification

`steep-crop-update-tests` covers coalescing, cancellation, worker reuse, and
4,000 concurrent final-request handoffs. Tests use explicit checks in Release
builds rather than disabled assertions.

## Interactive thread budget

Profiling the native Windows GUI exposed excessive OpenMP team overhead in
full-resolution crop processing, especially source conversion/denoise and Film
Lab. The preview executor now limits its descendant teams to four threads,
respecting any smaller existing limit. The setting is applied on the executor
thread, not globally: export and independent RAW-loading workers retain their
own limits. Rendering algorithms, demosaic selection and output resolution are
unchanged. Tests cover one-, two-, four- and many-thread inputs.

### Native measurements

One copied 26MP Fuji RAF with its edited Film Lab V5 profile was tested in
isolated settings/cache directories while the user's ordinary session stayed
open. These are crop-stage timings under desktop load, not cold-open timings
or a multi-image benchmark. The processed crop was 1386 x 1452 at skip=1;
the published visible crop was 810 x 876.

| Run | Full-resolution crop |
| --- | ---: |
| Installed binary, default 16-thread team | 15,771.9 ms |
| Same installed binary, OMP_NUM_THREADS=4 control | 1,350.7 ms |
| Rebuilt candidate, executor-local cap, no environment override | 1,295.5 ms |
| Candidate, repeat 100% zoom | 1,757.9 ms |
| Installed binary, additional output-comparison run | 20,643.4 ms |

The same-binary environment control isolates the thread-count effect from
other work in the checkout. Logs and frame dumps are under
`test-output/edit-latency/zoom-{baseline,four-threads,candidate,reference-frames}`.
The candidate logs `previewWorker threads=4 available=16`.

For matching rendered content and viewport, the initial fit frames matched
exactly. The final 100% frames differed by at most one 8-bit channel level:
mean absolute error 0.000956/255, with 99.9044% of channel values identical.
Compare reference `crop-ev+0.91-br4-co11-s1-0002.ppm` with candidate
`crop-ev+0.91-br4-co11-s1-0005.ppm`; earlier candidate full-resolution frames
cover a different viewport and are not valid pixel comparisons.

The native GUI build and `steep-crop-update-tests` passed. Automated pan input
timed out, so repeated pan behavior was not verified. Multi-image, before/after,
long-session and other-platform performance still need testing. No complete
resolution of long-session stalls is claimed. The installed running user
session is not replaced automatically; the new build is staged separately.
