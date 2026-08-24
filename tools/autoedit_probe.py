#!/usr/bin/env python3
"""Summarise what Auto Edit decided, from a STEEP_FILESEL_LOG trace.

Run the app with STEEP_FILESEL_LOG=1, exercise Auto Edit over a folder, then::

    python tools/autoedit_probe.py ~/steep-fileSel.log
    python tools/autoedit_probe.py before.log after.log     # compare two runs

The point of this is to make a threshold change checkable against real
photographs instead of argued about. Auto Edit's inputs have twice turned out
to be saturated at their rails -- values that looked like measurements but were
constants in practice -- and the only way that was visible was counting them
over a real library. Check the distributions before retuning anything.

Sections reported:

  scene       which label frames land on, and how the provisional (neutral
              render) label differs from the final (rendered frame) one
  scores      spread of each per-trait score, and how often frames are
              genuinely more than one thing
  saturation  inputs pinned at 0 or 1 -- a term stuck at a rail is a constant
              wearing a measurement's clothes, and is the bug to look for
  curve       toe/lift strengths and how often the symmetry clamp binds
  exposure    metered EV, the overshoot read back out of the meter's own
              hlcompr, how much of it was given back, what the render-space
              chimp then did, and what shipped -- plus a rail check on each
              of those terms
  verify      the Phase 4 pass over the finished profile: how often it ran
              and how far it moved things
  stocks      which film emulsions are actually being reached
  wb          how often white balance is corrected, and by how much
  noise       ISO-driven denoise
"""

import re
import sys
from collections import Counter, defaultdict


def parse(path):
    """Yield one dict per Auto Edit invocation found in the trace."""
    frames = []
    cur = None

    def num(pattern, line):
        m = re.search(pattern + r'=(-?[0-9.]+)', line)
        return float(m.group(1)) if m else None

    for line in open(path, encoding='utf-8', errors='replace'):
        if '[autoCurve] ==== ' in line:
            if cur:
                frames.append(cur)
            cur = {'file': line.split('==== ')[1].split(' ====')[0].strip()}
            continue
        if cur is None:
            continue

        if 'features: valid' in line:
            m = re.search(r'scene=(\S+)', line)
            if m:
                cur['sourceScene'] = m.group(1)
            cur['sourceMid'] = num('medLuma', line)
        elif 'shadowFrac=' in line:
            cur['sourceShadow'] = num('shadowFrac', line)
            cur['sourceHigh'] = num('hlFrac', line)
        elif 'picture from luma' in line:
            cur['renderMid'] = num('mid', line)
            cur['renderRange'] = num('range', line)
        elif 'scores:' in line:
            for k in ('portrait', 'lowSun', 'open', 'dark', 'urban'):
                cur[k] = num(k, line)
        elif 'overtuneRisk=' in line:
            cur['risk'] = num('overtuneRisk', line)
        elif 'strengths: toe' in line:
            cur['toe'] = num('toe', line)
            cur['lift'] = num('lift', line)
            m = re.search(r'clamped=(\d)', line)
            if m:
                cur['clamped'] = int(m.group(1))
        elif 'rooms:' in line:
            cur['shadowRoom'] = num('shadowRoom', line)
            cur['highlightRoom'] = num('highlightRoom', line)
        elif 'render fractions:' in line:
            cur['renderShadow'] = num('shadow', line)
            cur['renderHigh'] = num('high', line)
        elif 'exposure: expcomp' in line:
            cur['expcomp'] = num('expcomp', line)
            cur['hlcompr'] = num('hlcompr', line)
            cur['evAdd'] = num('evAdd', line)
        elif 'meter: metered=' in line:
            cur['metered'] = num('metered', line)
            cur['meteredHlcompr'] = num('meteredHlcompr', line)
            cur['overRange'] = num('overRange', line)
            cur['given'] = num('given', line)
        elif 'brightContent=' in line:
            cur['ceiling'] = num('ceiling', line)
            cur['brightContent'] = num('brightContent', line)
        elif 'highTolerance=' in line:
            cur['sourceClip'] = num('sourceClip', line)
            cur['addedClip'] = num('addedClip', line)
        elif 'reliance=' in line:
            cur['reliance'] = num('reliance', line)
            cur['srcDark'] = num('srcDark', line)
            cur['hold'] = num('hold', line)
        elif 'probe   headroom=' in line:
            cur['probeHeadroom'] = num('headroom', line.replace('EV', ''))
        elif '[autoFilm] stock=' in line:
            m = re.search(r'stock=(\S+?)/', line)
            if m:
                cur['stock'] = m.group(1)
        elif '[autoWB]' in line:
            cur['wbApplied'] = num('applied', line)
            cur['wbMove'] = num(r'\(mired move ', line.replace('+', ''))
            m = re.search(r'mired move ([+-][0-9.]+)', line)
            if m:
                cur['wbMove'] = float(m.group(1))
        elif '[autoNoise]' in line:
            cur['isoStops'] = num('stops', line)
            cur['nrLuma'] = num('luma', line)
            cur['nrChroma'] = num('chroma', line)
        elif '[autoVerify]' in line and 'skipped' not in line and 'could not' not in line:
            cur['verifyApplied'] = num('applied', line)
            cur['verifyMid'] = num('finishedMid', line)
        elif '[autoVerify]' in line and 'skipped' in line:
            cur['verifySkipped'] = 1.0
        elif '[autoScene]' in line:
            m = re.search(r'provisional=(\S+) final=(\S+)', line)
            if m:
                cur['provisional'], cur['final'] = m.group(1), m.group(2)

    if cur:
        frames.append(cur)

    # The same frame is re-analysed on every hover; keep one row per distinct
    # (file, rendered mid) so a long browse does not weight one photo heavily.
    seen, uniq = set(), []
    for f in frames:
        key = (f.get('file'), f.get('renderMid'), f.get('expcomp'))
        if key not in seen:
            seen.add(key)
            uniq.append(f)
    return uniq


def spread(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    n = len(values)
    return (values[0], values[n // 4], values[n // 2], values[3 * n // 4], values[-1])


def report(frames, label=''):
    n = len(frames)
    if not n:
        print(f'{label}no Auto Edit activity in this trace')
        return
    head = f'{label}{n} frames'
    print(head)
    print('=' * len(head))

    scenes = Counter(f.get('final') or f.get('sourceScene') for f in frames)
    print('\nscene')
    for name, count in scenes.most_common():
        print(f'  {name:<14}{count:>4}  ({100 * count / n:3.0f}%)')
    changed = sum(1 for f in frames
                  if f.get('provisional') and f['provisional'] != f.get('final'))
    if changed:
        moves = Counter(f"{f['provisional']} -> {f['final']}" for f in frames
                        if f.get('provisional') and f['provisional'] != f['final'])
        print(f'  reclassified after rendering: {changed} ({100 * changed / n:.0f}%)')
        for move, count in moves.most_common(6):
            print(f'    {move:<32}{count:>4}')

    print('\nscores            min    p25    p50    p75    max   >0.5')
    traits = ('portrait', 'lowSun', 'open', 'dark', 'urban')
    for trait in traits:
        s = spread([f.get(trait) for f in frames])
        if s is None:
            continue
        strong = sum(1 for f in frames if (f.get(trait) or 0) > 0.5)
        print(f'  {trait:<12}' + ''.join(f'{v:7.3f}' for v in s) + f'{strong:6}')
    blended = [f for f in frames
               if sum(1 for t in traits if (f.get(t) or 0) > 0.25) > 1]
    if any(f.get('portrait') is not None for f in frames):
        print(f'  frames that are more than one thing (2+ traits > 0.25): '
              f'{len(blended)} ({100 * len(blended) / n:.0f}%)')

    print('\nsaturation  (a term pinned at a rail is a constant, not a measurement)')
    for name, key, rail in (('shadowRoom', 'shadowRoom', 0.0),
                            ('highlightRoom', 'highlightRoom', 1.0),
                            ('portrait', 'portrait', 0.0),
                            ('dark', 'dark', 0.0)):
        vals = [f[key] for f in frames if f.get(key) is not None]
        if not vals:
            continue
        pinned = sum(1 for v in vals if abs(v - rail) < 1e-3)
        print(f'  {name:<14} at {rail:.0f} on {pinned:>4} / {len(vals)} '
              f'({100 * pinned / len(vals):3.0f}%)')

    toes = [f['toe'] for f in frames if f.get('toe') is not None]
    if toes:
        print('\ncurve')
        for name, key in (('toe', 'toe'), ('lift', 'lift')):
            s = spread([f.get(key) for f in frames])
            print(f'  {name:<6}' + ''.join(f'{v:7.3f}' for v in s))
        clamped = [f['clamped'] for f in frames if f.get('clamped') is not None]
        if clamped:
            hit = sum(clamped)
            print(f'  lift clamped to toe on {hit} / {len(clamped)} '
                  f'({100 * hit / len(clamped):.0f}%)')

    stocks = Counter(f['stock'] for f in frames if f.get('stock'))
    if stocks:
        print(f'\nstocks reached: {len(stocks)}')
        for name, count in stocks.most_common():
            print(f'  {name:<18}{count:>4}  ({100 * count / sum(stocks.values()):3.0f}%)')

    wb = [f for f in frames if f.get('wbApplied') is not None]
    if wb:
        applied = [f for f in wb if f['wbApplied'] >= 1]
        print(f'\nwhite balance corrected on {len(applied)} / {len(wb)} '
              f'({100 * len(applied) / len(wb):.0f}%)')
        if applied:
            s_ = spread([abs(f['wbMove']) for f in applied if f.get('wbMove') is not None])
            if s_:
                print('  |mired move|' + ''.join(f'{v:7.1f}' for v in s_) + '   (cap 18)')

    nr = [f for f in frames if f.get('nrChroma') is not None]
    if nr:
        on = [f for f in nr if f['nrChroma'] > 0.5 or (f.get('nrLuma') or 0) > 0.5]
        print(f'\ndenoise engaged on {len(on)} / {len(nr)} frames')
        if on:
            for name, key in (('luma', 'nrLuma'), ('chroma', 'nrChroma')):
                s_ = spread([f.get(key) for f in on])
                if s_:
                    print(f'  {name:<8}' + ''.join(f'{v:7.1f}' for v in s_))

    evs = [f['expcomp'] for f in frames if f.get('expcomp') is not None]
    if evs:
        print('\nexposure')
        for name, key in (('metered', 'metered'), ('over-range', 'overRange'),
                          ('given back', 'given'), ('reliance', 'reliance'),
                          ('chimp move', 'evAdd'), ('committed', 'expcomp'),
                          ('hlcompr', 'hlcompr'), ('ceiling', 'ceiling')):
            s_ = spread([f.get(key) for f in frames])
            if s_:
                print(f'  {name:<11}' + ''.join(f'{v:7.2f}' for v in s_))

        # The ceiling is derived per frame now, so "at the rail" means the
        # frame reached ITS ceiling, not a shared constant.
        pinned = sum(1 for f in frames
                     if f.get('expcomp') is not None and f.get('ceiling') is not None
                     and f['expcomp'] >= f['ceiling'] - 0.01)
        over = sum(1 for v in evs if v > 2.751)
        print(f'  at their own ceiling: {pinned}/{len(evs)}   '
              f'above the hard +2.75 (should be 0): {over}')

        # A term stuck at a rail is a constant wearing a measurement's
        # clothes. This is the check that has caught it three times.
        for name, key in (('giveback', 'given'), ('chimp move', 'evAdd'),
                          ('verify move', 'verifyApplied')):
            vals = [f[key] for f in frames if f.get(key) is not None]
            if len(vals) >= 8:
                common = Counter(round(v, 3) for v in vals).most_common(1)[0]
                share = 100.0 * common[1] / len(vals)
                # A mode of exactly zero means the deadband held and no
                # correction was called for, which is the design working.
                # A mode at any OTHER value on most frames means the term
                # stopped measuring and became a constant -- the failure this
                # pipeline has hit three times.
                flag = ('   <-- PINNED, this term is a constant'
                        if share > 60 and abs(common[0]) > 0.001 else '')
                note = ' (deadband held)' if abs(common[0]) <= 0.001 else ''
                print(f'  {name} most common value {common[0]:+.3f} '
                      f'on {share:.0f}% of frames{note}{flag}')

    verify = [f for f in frames if f.get('verifyApplied') is not None]
    if verify:
        moved = [f for f in verify if abs(f['verifyApplied']) > 0.005]
        print('\nverification pass (Phase 4, on the finished profile)')
        print(f'  ran on {len(verify)} frames, moved {len(moved)}')
        s_ = spread([f['verifyApplied'] for f in moved])
        if s_:
            print('  applied' + ''.join(f'{v:7.3f}' for v in s_))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for i, path in enumerate(sys.argv[1:]):
        if i:
            print('\n')
        report(parse(path), label=f'{path}: ' if len(sys.argv) > 2 else '')
    return 0


if __name__ == '__main__':
    sys.exit(main())
