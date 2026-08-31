#!/usr/bin/env python3
"""Emit each steep theme's palette as GTK4 CSS custom properties.

The steep token contract lives in GTK3 syntax (`@define-color steep_x ...`).
GTK4 (4.16+) uses real CSS custom properties instead. This script derives
`rtdata/themes/gtk4/<Theme>.css` from every palette so a future GTK4 port —
or an out-of-process GTK4 companion app — consumes the exact same design
tokens with zero hand-maintenance.

Run from the repo root:  python tools/gen_gtk4_tokens.py
Outputs are generated files; regenerate after palette edits, don't hand-edit.
"""

import re
import sys
from pathlib import Path

DEFINE_RE = re.compile(r"^@define-color\s+(steep_[a-z0-9_]+)\s+(.+?);\s*$")

def extract_tokens(css_path: Path) -> dict[str, str]:
    tokens: dict[str, str] = {}
    for line in css_path.read_text(encoding="utf-8").splitlines():
        m = DEFINE_RE.match(line.strip())
        if m:
            tokens[m.group(1)] = m.group(2).strip()
    return tokens

def to_css_var_block(tokens: dict[str, str], source_name: str) -> str:
    lines = [
        f"/* Generated from rtdata/themes/{source_name} by tools/gen_gtk4_tokens.py.",
        "   Do not edit — change the theme's palette block and regenerate. */",
        "",
        ":root {",
    ]
    for name, value in tokens.items():
        lines.append(f"  --{name.replace('_', '-')}: {value};")
    lines.append("}")
    return "\n".join(lines) + "\n"

def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    themes_dir = repo / "rtdata" / "themes"
    out_dir = themes_dir / "gtk4"
    out_dir.mkdir(exist_ok=True)

    defaults = themes_dir / "common" / "palette-defaults.css"
    sources = [defaults] + sorted(
        p for p in themes_dir.glob("*.css")
    )

    default_tokens = extract_tokens(defaults)
    if not default_tokens:
        print("error: no steep_* tokens found in palette-defaults.css", file=sys.stderr)
        return 1

    for src in sources:
        tokens = extract_tokens(src)
        if not tokens:
            print(f"skip (no palette block): {src.name}")
            continue
        # A theme may override a subset; fill gaps from the contract defaults,
        # mirroring the runtime provider layering.
        merged = dict(default_tokens)
        merged.update(tokens)

        out_name = "palette-defaults.css" if src == defaults else src.name
        out_path = out_dir / out_name
        rel_src = f"common/{src.name}" if src == defaults else src.name
        out_path.write_text(to_css_var_block(merged, rel_src), encoding="utf-8")
        print(f"wrote {out_path.relative_to(repo)}  ({len(merged)} tokens)")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
