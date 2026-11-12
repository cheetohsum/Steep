#!/usr/bin/env python3
"""Baseline lint for the steep UI contract.

Scans rtdata/themes/common/*.css and rtgui/**/*.{cc,h} for the things the
visual-polish pass removed, and fails on NEW ones only: what was already
there when the baseline was written is listed in tools/ui_lint_baseline.txt
and tolerated until somebody cleans it up.

    python tools/ui_lint.py                     # exit 1 on new findings
    python tools/ui_lint.py --update-baseline   # accept the current state
    python tools/ui_lint.py --all               # list every finding

Rules
  css-color     #hex / rgb()/rgba() literal in the common CSS (colours belong
                in palette-defaults.css and the theme palettes)
  css-radius    border-radius outside 0 / 3px / 6px / 10px / 50%
  css-font      font-size outside the ladder 0.75 / 0.85 / 1 / 1.15 / 1.25 em
  cxx-color     set_source_rgb[a]() with numeric literals, or Gdk::RGBA("#..")
                that is not a themeColor() fallback (data-colour files exempt)
  cxx-font      FontDescription("...") literal, or size='N' Pango markup
  cxx-name      set_name("X") with no #X selector in any theme CSS

Findings are keyed by rule, file and the offending text - not by line - so
moving code around does not make old findings look new.
"""

import argparse
import collections
import fnmatch
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS_DIR = os.path.join(ROOT, 'rtdata', 'themes', 'common')
THEMES_DIR = os.path.join(ROOT, 'rtdata', 'themes')
GUI_DIR = os.path.join(ROOT, 'rtgui')
BASELINE = os.path.join(ROOT, 'tools', 'ui_lint_baseline.txt')

RADII = {'0', '3px', '6px', '10px', '50%'}
FONT_SIZES = {'0.75em', '0.85em', '1em', '1.15em', '1.25em'}

# Code whose literal colours ARE the data: channel curves, crop guides,
# clipping indicators, on-canvas edit geometry, colour pickers and gradients
# that depict colour. Paths are relative to the repo root, '/'-separated.
DATA_COLOR_FILES = [
    'rtgui/editwidgets.*',
    'rtgui/cropwindow.cc',
    'rtgui/crophandler.cc',
    'rtgui/labgrid.cc',
    'rtgui/coloredbar.cc',
    'rtgui/colorprovider.*',
    'rtgui/previewwindow.cc',
    'rtgui/lockablecolorpicker.cc',
    'rtgui/widgets/curves/*',
    'rtgui/widgets/basic/thresholdselector.cc',
    'rtgui/tools/colortoning.cc',
    'rtgui/tools/labgrid*',
]


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def blank_comments(text, line_comments):
    """Replace comments with spaces, keeping every newline (so line numbers hold)."""
    def blank(match):
        return re.sub(r'[^\n]', ' ', match.group(0))

    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.S)

    if line_comments:
        # Good enough for a lint: does not try to understand '//' in strings
        # beyond skipping the obvious "scheme://" case.
        text = re.sub(r'(?<!:)//[^\n]*', blank, text)

    return text


def read(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as handle:
        return handle.read().replace('\r\n', '\n')


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def squeeze(snippet):
    return ' '.join(snippet.split())


def lint_css(findings):
    for name in sorted(os.listdir(CSS_DIR)):
        if not name.endswith('.css') or name == 'palette-defaults.css':
            continue

        path = os.path.join(CSS_DIR, name)
        text = blank_comments(read(path), line_comments=False)
        where = rel(path)

        for match in re.finditer(r'#[0-9a-fA-F]{3,8}\b(?![\w-])|\brgba?\(', text):
            # '#abc' at the start of a selector is an id, not a colour: a
            # colour only ever appears after a ':' inside a declaration.
            line_start = text.rfind('\n', 0, match.start()) + 1
            before = text[line_start:match.start()]
            if match.group(0).startswith('#') and ':' not in before:
                continue
            line_end = text.find('\n', match.end())
            decl = squeeze(text[line_start:line_end if line_end >= 0 else len(text)])
            findings.append(('css-color', where, line_of(text, match.start()), decl))

        for match in re.finditer(r'(?<![\w-])border(?:-(?:top|bottom)-(?:left|right))?-radius\s*:\s*([^;}]+)', text):
            values = match.group(1).split()
            if not all(value in RADII for value in values):
                findings.append(('css-radius', where, line_of(text, match.start()), squeeze(match.group(0))))

        for match in re.finditer(r'(?<![\w-])font-size\s*:\s*([^;}]+)', text):
            if match.group(1).strip() not in FONT_SIZES:
                findings.append(('css-font', where, line_of(text, match.start()), squeeze(match.group(0))))


def css_ids():
    ids = set()

    for folder, _dirs, files in os.walk(THEMES_DIR):
        for name in files:
            if name.endswith('.css'):
                text = blank_comments(read(os.path.join(folder, name)), line_comments=False)
                ids.update(re.findall(r'#([A-Za-z_][\w-]*)', text))

    return ids


def is_data_color_file(where):
    return any(fnmatch.fnmatch(where, pattern) for pattern in DATA_COLOR_FILES)


def lint_cxx(findings):
    ids = css_ids()
    number = r'-?(?:\d+\.?\d*|\.\d+)f?'

    for folder, _dirs, files in os.walk(GUI_DIR):
        for name in sorted(files):
            if not name.endswith(('.cc', '.h')):
                continue

            path = os.path.join(folder, name)
            where = rel(path)
            text = blank_comments(read(path), line_comments=True)
            lines = text.split('\n')
            data_file = is_data_color_file(where)

            for number_of, line in enumerate(lines, 1):
                if not data_file:
                    for match in re.finditer(r'set_source_rgba?\s*\(\s*' + number + r'\s*,[^;]*', line):
                        findings.append(('cxx-color', where, number_of, squeeze(match.group(0))))

                    if 'Gdk::RGBA("#' in line or "Gdk::RGBA ('#" in line:
                        # A fallback sits in the themeColor() call, which may
                        # have started on the line above.
                        context = (lines[number_of - 2] if number_of > 1 else '') + line
                        if 'themeColor' not in context:
                            findings.append(('cxx-color', where, number_of, squeeze(line)))

                for match in re.finditer(r'FontDescription\s*\(\s*"[^"]*"\s*\)', line):
                    findings.append(('cxx-font', where, number_of, squeeze(match.group(0))))

                for match in re.finditer(r'''size=\\?['"]\d+\\?['"]''', line):
                    findings.append(('cxx-font', where, number_of, squeeze(match.group(0))))

                for match in re.finditer(r'set_name\s*\(\s*"([A-Za-z_][\w-]*)"\s*\)', line):
                    if match.group(1) not in ids:
                        findings.append(('cxx-name', where, number_of, 'set_name("%s")' % match.group(1)))


def key_of(finding):
    rule, where, _line, snippet = finding
    return '%s\t%s\t%s' % (rule, where, snippet)


def load_baseline():
    counts = collections.Counter()

    if not os.path.exists(BASELINE):
        return counts

    for line in read(BASELINE).split('\n'):
        if not line or line.startswith('#'):
            continue
        count, _tab, key = line.partition('\t')
        counts[key] = int(count)

    return counts


def write_baseline(findings):
    counts = collections.Counter(key_of(finding) for finding in findings)

    with open(BASELINE, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('# tools/ui_lint.py baseline: findings tolerated because they predate the lint.\n')
        handle.write('# Regenerate with: python tools/ui_lint.py --update-baseline\n')
        handle.write('# count<TAB>rule<TAB>file<TAB>text\n')
        for key in sorted(counts):
            handle.write('%d\t%s\n' % (counts[key], key))

    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--update-baseline', action='store_true', help='accept the current findings')
    parser.add_argument('--all', action='store_true', help='list every finding, not only new ones')
    args = parser.parse_args()

    findings = []
    lint_css(findings)
    lint_cxx(findings)

    if args.update_baseline:
        counts = write_baseline(findings)
        print('ui_lint: baseline written, %d findings in %d entries' % (len(findings), len(counts)))
        return 0

    if args.all:
        for rule, where, line, snippet in sorted(findings, key=lambda f: (f[1], f[2])):
            print('%s:%d: [%s] %s' % (where, line, rule, snippet))
        print('ui_lint: %d findings' % len(findings))
        return 0

    allowed = load_baseline()
    seen = collections.Counter()
    fresh = []

    for finding in sorted(findings, key=lambda f: (f[1], f[2])):
        key = key_of(finding)
        seen[key] += 1
        if seen[key] > allowed[key]:
            fresh.append(finding)

    for rule, where, line, snippet in fresh:
        print('%s:%d: [%s] %s' % (where, line, rule, snippet))

    if fresh:
        print('ui_lint: %d new finding(s) (%d tolerated by the baseline)' % (len(fresh), len(findings) - len(fresh)))
        return 1

    print('ui_lint: clean (%d tolerated by the baseline)' % len(findings))
    return 0


if __name__ == '__main__':
    sys.exit(main())
