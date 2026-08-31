# GTK4 readiness

steep is a GTK3 application and stays one until a trigger fires (upstream
RawTherapee ports, Windows GTK3 bitrot, or a concrete need for the GPU
renderer). Until then, code is written so the toolkit stays swappable.
This document is the contract; the Rem Regrade plan artifact tracks status.

## Policy

- **GTK4: yes, eventually — trigger-gated.** No big-bang port. When
  triggered, the port is priced first by a two-week spike on the two
  hardest surfaces: the virtualized thumbnail canvas
  (`thumbbrowserbase/-entrybase`) and one curve editor.
- **libadwaita: never in the main app.** Its design premise is "one
  Adwaita, don't restyle" — incompatible with the steep_* token contract
  and the five-theme roster. Its only sanctioned home is an out-of-process
  companion app (e.g. a culling remote speaking the existing MCP protocol),
  where a GNOME-native look is fine.

## Seam rules (apply to all new GTK3 code)

1. **Draw functions, not draw overrides.** New custom drawing goes in a
   free function of `(Cairo::Context&, <state struct>)`; the widget's
   `on_draw` only gathers state and calls it. GTK4 keeps Cairo via
   `set_draw_func` / `gtk_snapshot_append_cairo`, so code in this shape
   ports mechanically. Existing widgets get restructured opportunistically
   (first mechanical target: the Adjuster pill), never speculatively.
2. **Colors come from the token contract.** `themeColor()` in C++,
   `@steep_*` in CSS. `tools/gen_gtk4_tokens.py` emits the same palettes as
   GTK4 CSS custom properties (`rtdata/themes/gtk4/*.css`); regenerate
   after palette edits. Never introduce a color literal that the generator
   cannot carry across.
3. **Menus behind a factory (planned seam).** GTK4 removes GtkMenu; every
   popup will be rebuilt on popovers/GMenuModel. New menu surface area must
   not deepen GtkMenu idioms: build through a shared helper when the menu
   factory lands (Phase 7 work item), which also becomes the single home of
   the Windows popup-freeze workarounds.
4. **Quarantine GtkTreeView.** The folder tree and Places stay as they
   are, but new list/grid UIs keep model/view separation clean enough that
   GTK4's `GtkListView`/`GtkColumnView` is a swap, not a rewrite. No new
   `GtkTreeView` consumers.
5. **Event controllers over raw event signals** where a helper exists;
   at minimum, keep pointer/keyboard logic in named methods, not sprawling
   inline lambdas, so the controller rewrite is a re-wiring.

## Port order (when triggered)

leaf dialogs (About, rename, ICC creator) → browser chrome → tool panels →
canvas surfaces last, each initially as `snapshot_append_cairo` wrappers
around the existing draw functions; convert only proven-hot paths to real
render nodes afterward.
