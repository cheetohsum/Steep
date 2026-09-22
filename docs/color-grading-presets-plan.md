# Color Grading Presets and Recommendations

Originally planned September 20, 2026. Implemented for the subsequent Windows
testing build: pagination button, 24 built-in grades, personal-grade
save/manage, main-image hover and click application, and cached-image ranking.
Native regression tests cover grading-only ownership, repeatability, storage,
validation, color-sensitive ranking, and neutral-image fallbacks.

The second tuning pass expands the original eight recipes and gives midtones
substantially more color. Bundled luminance offsets remain zero. Palette chips
are larger and use display-only chroma emphasis while accounting for global
grading and blending. Saved personal grades and previously resolved photo edits
are unchanged.

The third tuning pass corrects still-restrained midtones in the expressive
recipes: the median native midpoint chroma shift rises from 4.06 to 8.74 Lab
units after blending. Soft Portrait stays gentle, with Warm Paper and Quiet
Plum below the expressive range. No control remapping, luminance offsets, or
shadow/highlight saturation increases are involved. Recommendations accept
this stronger character while keeping the existing skin/white/gamut penalties.
Midtone-only native tests check the full-strength plateau, untouched endpoints,
and continuous transitions, independently of the menu swatches.

Recommendations now evaluate up to 1,024 spatially distributed Lab samples using
the native tonal weighting, reward visible character instead of favoring the
weakest grade, estimate risks to white highlights and skin-like colors, and
diversify the first four choices. These are local ranking estimates, not an
exact simulation of all downstream effects or person recognition. Analysis is
prewarmed only while the grading button is mapped, with caching and retry backoff.
Invalid/monochrome results are cached too, avoiding repeated analysis of them.

Verification: `steep-grading-tests` checks every bundled grade against the native
engine at 101 lightness levels, exact application/storage behavior, scene-aware
ordering, duplicate suppression, and palette-chip behavior. The rendering probe
in `tools/grading_preset_probe.py` renders all 24 recipes and a real-photo subset.
Results and inspected contact sheets are in `test-output/grading-presets/render-v2`.
The follow-up comparison in `test-output/grading-presets/render-mids-v3` renders
all 24 old/new recipes through the same native CLI and measures a 2.00x median
increase in midtone RGB difference from neutral on the test chart. This is a
fixture comparison, not a claim that every photo doubles its perceived color.

The initial recommender uses a private, bounded cached-thumbnail render with
grading disabled, then conservative Lab color/light heuristics. It does not
decode missing RAW thumbnails or hold the shared thumbnail lock during render.
Missing analysis uses stable ordering. Exact downstream candidate simulation,
a dedicated editor analysis tap, and broader photographic/platform validation
remain follow-up work; the detailed target design below includes those items.

## Intended Experience

When the Grading page is selected, a small preset button appears immediately
to the right of the existing Color Mixer / Grading / Point Color pagination.
Clicking it opens a compact menu of built-in and personal color grades, ordered
by their suitability for the current photo. Hover previews the grade on the
main editor image; clicking applies it. A footer action saves the user's
current wheel settings as a personal grade.

This is a color-grading library, not a second whole-photo profile browser or an
Auto Edit operation. It must not change exposure, white balance, tone curves,
Film Lab, cropping, perspective, masks, repairs, or other processing settings.

## Findings in the Current Code

- `rtgui/toolpanelcoord.cc`, `populateEditPanel()`: the three pagination buttons
  and `ColorToolStack` are constructed together. This is the insertion point;
  the standalone Color Grading section header is hidden in this layout.
- `rtgui/tools/colorgrading.{h,cc}`: shadows, midtones, highlights, and global
  controls already read and write a complete `ColorGradingParams` group.
- `rtengine/procparams.{h,cc}`: `[Color Grading]` already serializes enabled,
  four hue/saturation/luminance triplets, blending, and balance.
- `rtengine/improcfun.cc`, `colorGrading()`: hue is an angle in Lab a/b, not HSV.
  Blending currently scales the entire grade, including luminance. Balance
  shifts tonal weighting boundaries. Preserve these meanings.
- Preview, detail crop, export, and thumbnail pipelines invoke this grading
  implementation. Film Lab and other nonlinear operations occur downstream.
- `rtgui/autoedit.h`: reusable feature concepts include luminance percentiles,
  warm/cool distribution, saturation, and skin-like color fractions. Its
  existing analysis uses a neutral profile, so calling it unchanged would not
  describe the user's current edited image.
- `Thumbnail::processAnalysisImage()` provides monitor-independent analysis,
  but can trigger full thumbnail generation and holds a thumbnail lock. It
  must not run synchronously when opening this menu.
- `ToolPanelCoordinator` has quick-preview snapshots and asynchronous request
  generation checks. Reuse their ownership conventions, but explicitly handle
  conflicts with Auto Edit, B&W, and profile previews.
- `steepui::PopupMenu` is the designated popup abstraction and includes the
  Windows popup-window workaround. Extend it minimally if custom swatch rows
  need support; do not introduce a separate popup implementation.

## 1. Button and Menu

Use a small themed preset-library icon with a corner chevron and the tooltip
"Grading presets". Match existing pagination sizing and restrained styling.
Reserve a fixed-width end slot and balancing start space so the pagination
stays centered and does not jump when changing pages. No new panel or bubbles.

The menu contains:

1. A ranked list of built-in and personal grades. The strongest few matches
   appear first; all remaining grades remain accessible below them.
2. Each row has a name, three small shadow/midtone/highlight swatches, and room
   for a current-grade checkmark. Personal origin and the reason for a
   recommendation can be shown in a tooltip, without long explanatory rows.
3. A separator followed by "Reset grade", "Save current grade...", and
   "Manage my grades...". The management view supports rename, update from
   the current committed grade, duplicate, and delete. Built-ins are read-only
   and can be duplicated into personal grades.

Use a viewport-bounded, vertically scrollable menu. Keep actual widget hitboxes
aligned with visible rows. Test at the minimum supported sidebar width and high
DPI. Click reliably opens/closes the menu; keyboard arrows, Enter, and Escape
must work. Leaving the button/menu region closes it with a short transition
grace, except while an associated dialog or submenu has focus.

Freeze ordering for the lifetime of an open menu. If analysis is not ready,
show a stable fallback list immediately and use the completed ranking on the
next opening. Do not move a preset out from under the cursor.

## 2. Built-In Grades

Begin with eight distinct, modest recipes rather than dozens of similar looks:

| Working name | Direction |
| --- | --- |
| Warm Paper | Restrained warm highlights, nearly neutral shadows |
| Cool Daylight | Gentle cool highlights with neutral midtones |
| Amber & Slate | Warm light separated from cool shadows |
| Rose & Olive | Muted rose highlights and green-biased shadows |
| Soft Portrait | Low-strength color separation and restrained midtones |
| Copper Dusk | Copper warmth with cool, intact dark tones |
| Night Cyan | Subtle cool shadows without additional darkening |
| Quiet Plum | Muted plum shadows and slightly warm highlights |

These are design directions, not validated photographic claims. Tune their
actual values with the native renderer. Do not copy HSV hue angles into the
Lab-based wheels, or use real film-stock names.

Start with zero luminance offsets in bundled grades. Make their character come
from color separation, not added brightness, crushed shadows, or raised blacks.
Keep global and midtone saturation conservative. Store every grading field
explicitly, including global settings, blending, and balance, to prevent values
from the previously selected grade leaking into the next one.

Use portraits, low-key scenes, bright neutral surfaces, foliage, sunset light,
and saturated artificial light when tuning. The current grading engine has no
explicit skin protection; "Soft Portrait" must not imply that it does.

## 3. Image-Based Recommendations

### Analyze the relevant image

Use the current edited framing and processing settings, with only Color
Grading disabled for the recommendation baseline. Preserve Film Lab and other
edits. This prevents the currently selected or hovered grade from reinforcing
its own ranking and avoids mistaking an intentionally dark photo for a problem
that needs brightening.

Obtain a bounded, immutable analysis sample through the existing processing
pipeline once decoded data is available. Prefer a small editor-pipeline sample
over starting another RAW decode. If a new sample handoff is needed, copy only
the bounded sample under synchronization, not a full-resolution image. First
verify its color space, transfer function, numeric scale, crop, and orientation.

Extract:

- Robust luminance percentiles, shadow/highlight proportions, and bright-neutral
  headroom using the same documented tonal interpretation as the renderer.
- Per-tonal-zone hue and chroma distributions, using circular hue statistics
  rather than averaging hue numbers.
- Existing color separation, extreme saturation, and conservative skin-like
  color estimates. These estimates are not reliable person recognition.
- A coarse spatial grid to distinguish large color regions from tiny saturated
  lights and to respect the composition of the current crop.

### Rank, do not silently edit

Score each recipe for useful color separation, compatibility with existing
warm/cool light, preservation of neutral highlights, and risk of unwanted
skin-like color shifts or oversaturation. Avoid rewarding saturation alone.
Select diverse leading suggestions instead of four nearly identical grades.
Personal grades participate using traits derived from their saved values;
users should not need to tag them manually.

Use the actual grading math for cheap candidate checks on a small pre-grading
sample. A grade applied to an already rendered bitmap is not equivalent when
Film Lab or other nonlinear operations follow it. Where such effects matter,
validate only the leading candidates through a bounded background render at
the correct pipeline position. Never render every preset at full resolution.
Any shared math extraction must preserve existing output numerically.

Recommendations only reorder presets. They never secretly change preset
strength, luminance, or any other saved value. The same grade remains the same
grade on every image. Use stable IDs to break score ties deterministically.

For insufficient data or weak color evidence, show a stable default order
without claiming a strong recommendation. Respect downstream forced-neutral
B&W behavior; do not disable B&W or Film Lab to make a grade appear effective.

### Keep the interface responsive

- Analyze asynchronously at low priority, using a roughly 256-512 pixel sample.
- Reuse bounded cached results keyed by image identity, relevant non-grading
  parameters, framing, pipeline version, and preset-library revision.
- Coalesce rapid changes and cancel obsolete requests. Check image identity,
  processor lifetime, and generation again before publishing results.
- Never hold processing locks while updating GTK or wait for analysis in an
  event handler. Hovering grades must not invalidate the analysis baseline.
- Measure latency and memory before setting performance guarantees. The target
  is immediate cached menu opening with no additional RAW decode on open.

## 4. Preview and Apply Semantics

Create a single preview owner for this interaction and a committed baseline.
Every hovered preset starts from that same baseline, replacing only the complete
Color Grading group. Do not progressively modify the previous hover result.

- Debounce short pointer crossings and allow only the latest preview request
  to publish. Preview must reach the main editor image, not just a thumbnail.
- Hover and keyboard focus preview temporarily. Only click/Enter commits.
- A commit applies the exact previewed values once and creates one undo entry.
- Mouse leave, Escape, or changing tool pages cancels the preview and restores
  the baseline. Photo switches discard old requests without restoring into the
  new photo. Manual edits take ownership and cannot be overwritten by a late
  preview completion or stale restore.
- Coordinate ownership with existing Auto Edit, B&W, and profile hover menus.
- Hover must not write sidecars, create history entries, or replace persisted
  edited filmstrip thumbnails. Commit follows normal save/thumbnail behavior.
- Cancel and commit must both allow the full-resolution preview to finalize;
  never leave the processor indefinitely in an interactive low-detail mode.

## 5. Saving Personal Grades

Use a versioned, grading-only keyfile format that reuses the existing
`[Color Grading]` field names and serialization. Add a small metadata group for
stable ID, display name, and schema version. Do not reuse the current Film Look
ownership marker, which intentionally rejects non-film parameter groups.

Store custom grades in a dedicated `grading-presets` directory under the
existing `Options::rtdir` configuration root, honoring portable mode and
Windows/Linux/macOS path handling. Bundle built-ins in installed resources;
never write custom files into the installation directory or the whole-photo
profile tree.

- Save all 14 numeric fields and the intended enabled state from committed
  controls, never from an uncommitted hover. Disable Save if there is no active
  grade to save. Applying an ordinary saved grade enables grading.
- Use stable IDs for filenames, independent of display names. Handle duplicate
  names without accidentally overwriting another preset.
- Validate finite numbers and field ranges, required fields, supported schema
  versions, and file size. Ignore a malformed entry without breaking the menu.
- Write atomically with a temporary file and rename. Surface persistence errors
  without losing the grade currently applied to the photo.
- Deletion is limited to personal grades and requires a clear confirmation.
- Sidecars continue saving resolved grading values. Renaming or deleting a
  preset must never change previously edited photos.
- Store parameters only; no source images or analysis samples belong in these
  preset files. External preset import/export can be a separate follow-up.

## 6. Delivery and Verification

### Phase A: Deterministic library and application

Add the pagination button and popup through `ToolPanelCoordinator`; provide
scoped read/apply helpers in `ColorGrading`; add a small preset store, bundled
recipes, icons, strings, and resource installation rules. Implement save/manage,
exact grade replacement, reset, and one-step undo before recommendations.

### Phase B: Ranking and preview lifecycle

Add the bounded analysis handoff, pure feature/ranking helpers, generation-safe
worker integration, frozen menu ordering, and coordinated hover previews.
Reuse existing code where it fits without adding a general preset framework.

### Phase C: Photographic tuning and platform QA

Tune the recipes and scoring against varied real photos. Validate native
Windows, Linux, and macOS builds and packaging rather than assuming GTK
behavior and installed resources match automatically.

Acceptance checks:

- Applying A twice is identical; A then B equals applying B directly. All
  non-grading parameters remain unchanged, including crop, masks, and Film Lab.
- Save/reload retains all fields; rename/delete cannot alter saved photo edits.
- Same baseline yields the same order; hue wraparound, grayscale, black frames,
  clipped lights, custom strong grades, and missing analysis data are handled.
- Crop and non-grading edits update relevant recommendations; zoom, monitor
  profile, hover, and the current committed grade do not create ranking loops.
- Rapid hover, leave, click, undo, manual adjustment, photo switch, and processor
  destruction cannot apply stale settings or preview the wrong photo.
- Menu opening or construction never commits a grade. Click and keyboard
  activation reliably commit once, and every visible row has the correct hitbox.
- Narrow sidebar, high DPI, long names, and many personal grades do not overflow.
- Main preview, full-resolution detail, thumbnails after commit, and export use
  the same grade. Existing forced-neutral B&W behavior is explicitly covered.
- A long editing session shows bounded cache/worker growth and no degradation
  in image switching, main-image preview latency, or full-resolution completion.

The implementation should not alter grading-engine aesthetics, Auto Edit, or
Film Lab algorithms incidentally. Any necessary engine helper extraction gets
output-equivalence tests before this feature is added on top.
