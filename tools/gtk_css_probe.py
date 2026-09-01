#!/usr/bin/env python3
"""Measure what GTK actually computes for steep's CSS stack, headlessly.

Loads the theme exactly the way rtwindow.cc does (palette-defaults, the theme
with its imports, widgets.css at +200, then the forced "* { font }" provider),
builds a few representative widgets, and prints their computed font sizes.
Use it before trusting any font-size rule: the forced-font provider is added
after the theme at the same priority and wins, so theme-level sizes are dead
unless placed in widgets.css.

Run (Linux/WSL):  xvfb-run -a python3 tools/gtk_css_probe.py [themes-dir]
"""
import sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, Pango

THEMES = sys.argv[1] if len(sys.argv) > 1 else "rtdata/themes"
screen = Gdk.Screen.get_default()
APP = Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION

def add(src, prio, data=False):
    p = Gtk.CssProvider()
    if data:
        p.load_from_data(src.encode())
    else:
        p.load_from_path(src)
    Gtk.StyleContext.add_provider_for_screen(screen, p, prio)
    return p

add(THEMES + "/common/palette-defaults.css", APP)
add(THEMES + "/Rem.css", APP)
add(THEMES + "/common/widgets.css", APP + 200)
add("* { font-family: Sans; font-size: 9pt }", APP, data=True)

win = Gtk.OffscreenWindow()
root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
win.add(root)

group = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
group.set_name("ToolGroup"); group.get_style_context().add_class("ToolGroup")
row = Gtk.Box()
btn = Gtk.Button(); btn.set_name("ToolGroupHeader"); btn.set_relief(Gtk.ReliefStyle.NONE)
glabel = Gtk.Label(); glabel.set_use_markup(True); glabel.set_markup("<small>\u25b8</small>  Effects")
btn.add(glabel); row.pack_start(btn, False, False, 0); group.pack_start(row, False, False, 0)
content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL); group.pack_start(content, False, False, 0)
heading = Gtk.Label(label="Glow & Halation"); heading.get_style_context().add_class("tool-heading-label"); content.pack_start(heading, False, False, 0)
section = Gtk.Label(label="Glow"); section.get_style_context().add_class("tool-section-label"); content.pack_start(section, False, False, 0)
plain = Gtk.Label(label="plain"); content.pack_start(plain, False, False, 0)
root.pack_start(group, False, False, 0)
win.show_all()
while Gtk.events_pending(): Gtk.main_iteration()

def fs(w, name):
    ctx = w.get_style_context()
    v = ctx.get_property("font-size", Gtk.StateFlags.NORMAL)
    pc = w.get_pango_context().get_font_description()
    unit = "px" if pc.get_size_is_absolute() else "pt"
    print(f"{name:20s} css font-size={v:6.2f}px   pango={pc.get_size()/Pango.SCALE:5.2f}{unit}  weight={pc.get_weight()}")

print("--- current CSS stack ---")
for w, n in ((glabel, "group label"), (btn, "group button"), (row, "header row"), (heading, "tool heading"), (section, "section label"), (plain, "plain label")):
    fs(w, n)

add("#ToolGroupHeader label { font-size: 1.15em; }", APP + 200, data=True)
while Gtk.events_pending(): Gtk.main_iteration()
print("--- + same rule at APPLICATION+200 (widgets layer) ---")
fs(glabel, "group label")

add("#ToolGroupHeader label { font-size: 14px; }", APP + 200, data=True)
while Gtk.events_pending(): Gtk.main_iteration()
print("--- + absolute 14px at +200 ---")
fs(glabel, "group label")
