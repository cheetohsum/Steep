#!/usr/bin/env python3
"""Does the Transform-section rule actually reach a tool's parameter block?

The theme is loaded the way rtwindow.cc loads it, then a widget tree shaped
like the real one is built -- section content box, MyExpander name, ExpanderBox
name, ToolParamBlock -- and the padding GTK computes is read back. Left 4 and
right 8 is the rule taking effect; 10 and 0 is it being ignored, which is what
a typo in a selector looks like from the outside.
"""
import sys

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk

THEMES = sys.argv[1] if len(sys.argv) > 1 else "rtdata/themes"
screen = Gdk.Screen.get_default()
APP = Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION


def add(path):
    p = Gtk.CssProvider()
    p.load_from_path(path)
    Gtk.StyleContext.add_provider_for_screen(screen, p, APP)
    return p


add(THEMES + "/common/palette-defaults.css")
add(THEMES + "/common/steep-look.css")


def build(section_name):
    win = Gtk.OffscreenWindow()
    outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)

    if section_name:
        outer.set_name(section_name)

    expander = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    expander.set_name("MyExpander")
    box = Gtk.EventBox()
    box.set_name("ExpanderBox")
    block = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    block.get_style_context().add_class("ToolParamBlock")

    box.add(block)
    expander.pack_start(box, False, False, 0)
    outer.pack_start(expander, False, False, 0)
    win.add(outer)
    win.show_all()

    ctx = block.get_style_context()
    pad = ctx.get_padding(Gtk.StateFlags.NORMAL)
    return pad.left, pad.right


plain = build(None)
inside = build("TransformSection")
print("  a tool anywhere else        padding left/right = %d / %d" % plain)
print("  the same inside a section   padding left/right = %d / %d" % inside)

good = plain[0] == 10 and inside == (4, 8)
print("\n%s  the section rule reaches the parameter block"
      % ("PASS" if good else "FAIL"))
sys.exit(0 if good else 1)
