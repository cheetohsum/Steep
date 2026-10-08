# Photo collection tags

Tags live in `photo-tags` beside Steep's `options` and `albums` files. They are independent of processing presets and the thumbnail cache. Adding tags does not modify photographs, EXIF, XMP, or PP3 files. Preserve this catalog when migrating Steep settings.

## Using tags

- Browser: open the filter bar and its **Tags** menu. Right-click selected thumbnails and choose **Edit photo tags…** to tag one image or a batch.
- Editor: the filter bar offers **Tags** and **Edit photo tags…** for the open photograph. Filmstrip thumbnails also have the tagging context action.
- Album display: **Tags** filters grid, fit and collage views. **Tag visible photos…** applies to the currently displayed subset. Clearing tags restores all album members; tag filters do not change album membership. Sorting is preserved across filter changes.
- Smart albums: add a **Photo tag** rule. `is` and `is not` compare whole tag names without case sensitivity; `contains` searches tag names. Tags on photographs outside the current folder are discoverable from the durable catalog.
- Double Exposure: **Tags** narrows the folder or global selections alongside stars and pick flags. Initial tag criteria come from the browser filter. **Tag chosen layers…** edits the photographs currently in the layer stack.

The filter menu searches names and descriptions. Selecting several tags matches any of them by default; **Match every selected tag** requires all of them. **Untagged photos** selects photos without tags. The button label and the filter badge show an active filter. Photo tooltips list assigned tags.

The editor dialog reports how many selected photographs have each tag. An indeterminate checkbox preserves each photograph's individual membership. Checking it adds the tag to the entire selection; unchecking removes it. New tags can have a description explaining a collection idea. Case and Unicode normalization prevent duplicate names.

## First artistic study — October 2, 2026

Reviewed 850 selected photo files across 25 folders under `D:\Media\Photos` and `E:\Photos`. Selection combined recursive PP3 stars/picks, Steep's persistent selection index, and the `_Favorites` folder. This count includes RAW/JPEG pairs, alternate versions and copied favorites; it is not a count of unique exhibition photographs. Review used camera-embedded previews and JPEGs, so final print selection should be checked against developed edits.

The first trial applies 114 assignments to 44 selected files in `D:\Media\Photos\February 2026`. No other folders received assignments. The complete vocabulary is available throughout Steep:

| Tag | Collection reading |
| --- | --- |
| Thresholds | Doorways, caves, gates and passages hold the viewer between inside and outside. |
| Borrowed Light | Beams, reflections and apertures give dark spaces a temporary shape. |
| Quiet Geometry | Repetition, diagonals, grids and negative space turn objects into abstraction. |
| Rooms Remember | Furniture and intimate or empty rooms suggest life just outside the frame. |
| The Living Edge | Vegetation meets architecture, cultivated boundaries and stone. |
| Soft Horizons | Sea, haze and distance soften certainty and create spaciousness. |
| Deep Time | Strata, fissures, weathering and erosion make duration visible. |
| After Hours | Night streets and isolated lamps hold a pause between activity and quiet. |
| Small Presences | A figure, animal, shadow or personal object gives a scene an intimate pulse. |
| Human Scale | Small people set against large spaces reveal belonging and distance. |
| Fabricated Worlds | Exhibits, replicas and themed environments blur place and imagination. |
| In Transit | Paths, stairs, vehicles and infrastructure imply movement held still. |

Six proposed shows: **The Space Between**, **Instructions for Stillness**, **The Earth Keeps the Score**, **Someone Was Here**, **The Town After It Becomes a Dream**, and **At the Edge of Being There**. The illustrated review board and per-photo rationale are saved in the parent workspace's `.codex-tmp/photo-review/collections.html`; the reviewed trial is in `pilot-plan.json` and `pilot-plan.ini`.

## Validation and implementation

`tools/photo_tags_tests.cc` exercises save/reload, Windows path normalization, Unicode names, any/all matching, untagged filtering, bulk preservation, per-photo plans, rejected unknown tags, invalid names, failed writes and damaged/future catalogs. `tools/apply_photo_tag_plan.cc` applies a reviewed plan in one atomic catalog transaction and verifies readback.

Names and paths are normalized in memory; display titles retain their original casing. Save failure leaves in-memory data unchanged. A malformed or newer catalog is never overwritten. Tags currently identify a photograph by its absolute path: renaming or moving a file outside Steep requires reconciling the catalog. The artist's tags are interpretations, not automatic classifications, and can be refined in the UI.
