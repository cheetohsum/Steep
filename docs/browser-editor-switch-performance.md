# Browser / Editor Switching

## Confirmed Blocking Path

The Windows build reproduced a 49.4-second UI stall when switching to Edit
while a cold folder of RAW thumbnails was being processed. A debugger capture
of the isolated test process showed the GUI thread blocked in:

```
RTWindow::MoveFileBrowserToEditor
FileCatalog::enableTabMode
ThumbBrowserBase::applyTabModeEntryGeometry_
ThumbBrowserEntryBase::resize
FileBrowserEntry::calcThumbnailSize
Thumbnail::getThumbnailSize
```

That dimension query acquired the same mutex held across RAW decoding and
thumbnail processing. Resizing a whole folder could wait on successive workers.
Reference increments also used that mutex, including while the thumbnail job
queue lock was held, extending contention to job submission and cached opens.

## Changes

- Publish the most recent valid thumbnail aspect ratio independently of the
  processing mutex. Browser layout reads this snapshot; explicit parameter-based
  dimensions used by the export queue retain the existing locked calculation.
- Protect reference counts with a separate short-lived lock. Reference ownership
  and cache deletion semantics remain unchanged.
- Let the viewport request missing thumbnail pixels instead of queuing all empty
  cells during a tab-mode resize.
- Do not cache a scaled placeholder as if it were a completed render at the new
  cell size. Existing placeholders still display while the correct render loads.
- Detach the editor's FilePanel pointer when its parent window is destroyed.
  Managed editor widgets can outlive FilePanel; close() previously accessed its
  already-destroyed recent-RAW cache, causing a shutdown use-after-free after
  visiting multiple photos. Finish deferred cleanup during editor destruction,
  and destroy queued callbacks' captures before reporting the executor drained.
  Ordinary image switches retain asynchronous cleanup.

No RAW demosaicing, color, edit, or export processing was changed. Filter and
selection ownership is unchanged.

## Windows Verification

Fixture: 96 isolated filenames linked to a copy of DSCF8535.RAF, separate
settings and thumbnail cache, native full GUI. The baseline and candidate each
started without a thumbnail cache. These are synthetic reproduction timings,
not a guarantee for every folder, disk, or machine.

| During the scripted switch sequence | Baseline | Candidate |
| --- | ---: | ---: |
| Median switch handler | 7.1 ms | 7.3 ms |
| Longest switch handler | 49,384.2 ms | 10.6 ms |
| Median destination paint | 13.4 ms | 13.2 ms |
| Longest destination paint | 50,905.8 ms | 39.0 ms |

The fix removes the large blocking outlier rather than materially changing the
already-fast uncontended switch. One 314 ms main-loop gap remains elsewhere in
the candidate run; this change is not a claim to eliminate all startup/loading
work. The browser's visible thumbnails were populated after the test.

A warm-cache stress run completed 100 scripted switches (101 handler samples
including the first selected-image open): median handler 6.8 ms, maximum
18.6 ms; median destination paint 12.2 ms, maximum 30.6 ms. An additional run
opened two fixture RAWs, changed exposure to +0.30, and switched back to Browser.
Both sidecars contained the expected value; save enqueue took 0.4-0.6 ms and the
two return-to-browser handlers took 7.9-8.5 ms. Background flushes completed.
The edit/advance/save sequence was repeated twice after the teardown fix. Both
test windows closed without a crash; the second run recorded process exit code
0. Both images also published their settled editor refinement before shutdown.

The `steep-thumbnail-size-tests` target checks aspect ratios, width limits,
invalid worker dimensions, and simultaneous publication/layout reads.

Reproduction helpers: `tools/view_switch_probe.ps1` and
`tools/stage_switch_test.ps1`. Run labels must be unique to keep diagnostic logs
separate. `-ColdCache` uses a new label-specific cache. `-Edit` intentionally
changes only the fixture photos' sidecars and requires `-PeriodMs 4500` or more.
`-WaitForExit` waits for normal window closure and records the process exit code.
Run standalone native tests with the MinGW runtime directory on `PATH`.

The candidate is staged separately; the user's running editor is not restarted.
