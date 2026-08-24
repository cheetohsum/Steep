#!/usr/bin/env python3
"""Replay Auto Edit's exposure rule over a real library, without building.

The thumbnail cache already holds, for every frame the browser has ever
metered, exactly what ``ImProcFunctions::getAutoExp`` handed back::

    AEExposureCompensation=2.2775452136993408
    AEHighlightCompression=43
    AEContrast=24
    AEBlack=120

That is the input to Auto Edit's exposure stage. So any change to the rule
that turns a metered exposure into a committed one can be checked against
thousands of real photographs in about a minute, with no build-and-click
loop::

    python tools/ae_cache_replay.py                      # today vs proposed
    python tools/ae_cache_replay.py --rule today         # one rule only
    python tools/ae_cache_replay.py --giveback 0.85      # tune and re-measure

Auto Edit's inputs have three times turned out to be saturated at their
rails -- values that looked like measurements but were constants in
practice. Counting them over a real library is the only way that has ever
been visible. Check the distribution before retuning anything.

WHAT THIS MODELS, AND WHAT IT DOES NOT
--------------------------------------
Modelled: metering -> the highlight over-range giveback -> the hlcompr
recomputation -> the exposure ceiling. That is the whole of the analytic
half of ``applySteepAutoEdit``.

NOT modelled: ``protection`` and the source highlight fraction (they need
the neutral analysis pass), and the render-space stages -- the chimp and
the Phase 4 verification probe -- which need an actual render. Those move
the committed exposure by a further -0.75..+0.20EV and -0.60..+0.20EV
respectively, so treat the numbers here as the exposure the render-space
stages START from, not the one that ships. For the shipped value, take a
STEEP_FILESEL_LOG trace and run ``tools/autoedit_probe.py``.
"""

import argparse
import math
import os
import statistics
import sys

# Keep these in step with rtgui/autoedit.cc. If they drift, this tool is
# measuring a pipeline that does not exist.
K_METER_HLCOMPR_GAIN = 2.3      # improcfun.cc getAutoExp: comp = (...) * 2.3
K_MAX_OVER_RANGE_GIVEBACK = 1.25
K_HARD_EXPOSURE_CEILING = 2.75
K_EXPOSURE_FLOOR = -1.5
K_HLCOMPR_FLOOR = 12
K_HLCOMPR_CEILING = 80
K_HLCOMPR_RELIANCE_LOW = 55.0
K_HLCOMPR_RELIANCE_HIGH = 100.0
K_GIVEBACK_UNDER_RELIANCE = 0.30

RAW_EXTENSIONS = {
    'RAF', 'ARW', 'CR2', 'CR3', 'NEF', 'DNG', 'RW2', 'ORF', 'PEF', 'SRW',
    'RAW', 'SRF', 'X3F', 'IIQ', 'MOS', '3FR', 'MEF', 'ERF', 'KDC', 'NRW',
}


def default_cache_dir():
    """Where the browser keeps its thumbnail cache on this platform."""
    if sys.platform.startswith('win'):
        base = os.environ.get('LOCALAPPDATA')
        if base:
            return os.path.join(base, 'RawTherapee', 'cache', 'data')
    xdg = os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache')
    return os.path.join(xdg, 'RawTherapee', 'cache', 'data')


def read_cache(directory, raw_only=True):
    """Every metered frame in the cache, as (name, expcomp, hlcompr, contrast, black)."""
    rows = []
    skipped = 0

    try:
        names = os.listdir(directory)
    except OSError as err:
        sys.exit(f'cannot read cache directory {directory}: {err}')

    for name in names:
        if not name.endswith('.txt'):
            continue

        parts = name.split('.')
        extension = parts[1].upper() if len(parts) > 2 else ''

        if raw_only and extension not in RAW_EXTENSIONS:
            continue

        values = {}
        try:
            with open(os.path.join(directory, name), encoding='utf-8',
                      errors='replace') as handle:
                for line in handle:
                    if '=' in line:
                        key, value = line.split('=', 1)
                        values[key.strip()] = value.strip()
        except OSError:
            skipped += 1
            continue

        try:
            expcomp = float(values['AEExposureCompensation'])
            hlcompr = int(values['AEHighlightCompression'])
            contrast = int(values['AEContrast'])
            black = int(values['AEBlack'])
        except (KeyError, ValueError):
            skipped += 1
            continue

        # aeValid=false serialises as a row of zeroes. A frame that was
        # genuinely metered at 0.00EV also needs a non-zero contrast, because
        # getAutoExp never returns contrast 0 for a real histogram.
        if expcomp == 0.0 and hlcompr == 0 and contrast == 0:
            continue

        rows.append((name, expcomp, hlcompr, contrast, black))

    return rows, skipped


def over_range_ev(expcomp, hlcompr, gain=K_METER_HLCOMPR_GAIN):
    """How far past full scale the metered gain pushes the frame's white clip.

    getAutoExp derives hlcompr from precisely that overshoot::

        comp    = (gain * whiteclip / scale - 1) * 2.3
        hlcompr = 100 * comp / (max(0, expcomp) + 1)

    so inverting it recovers the overshoot in stops. hlcompr saturates at
    100, which makes this an UNDER-estimate on the frames that overshoot
    most -- conservative in the direction that matters.
    """
    if hlcompr <= 0:
        return 0.0

    comp = hlcompr * (max(0.0, expcomp) + 1.0) / 100.0
    return math.log2(max(1e-6, 1.0 + comp / gain))


def residual_hlcompr(expcomp, residual_ev, gain=K_METER_HLCOMPR_GAIN):
    """What compression the corrected exposure still calls for, by the same formula."""
    if residual_ev <= 0.0:
        return 0

    comp = (math.pow(2.0, residual_ev) - 1.0) * gain
    return int(round(100.0 * comp / (max(0.0, expcomp) + 1.0)))


def rule_today(expcomp, hlcompr, _args):
    """The rule as it stood before this work: an ad-hoc 0.35EV apology."""
    committed = expcomp
    compression = min(K_HLCOMPR_CEILING,
                      max(hlcompr, 30 if committed > 0.60 else 12))

    if compression > 30 and committed > 0.0:
        excess = min(1.0, max(0.0, (compression - 30) / 20.0))
        committed = max(0.0, committed - 0.35 * excess)
        compression = int(round(30.0 + 10.0 * excess))

    return (max(K_EXPOSURE_FLOOR, min(K_HARD_EXPOSURE_CEILING, committed)),
            compression)


def reliance(hlcompr):
    """How completely the metered exposure leans on highlight compression.

    getAutoExp clamps hlcompr to 100. A meter that has pinned it there is not
    reporting a metering error, it is reporting a scene that does not fit --
    and on those frames the compression is doing real work. Taking the full
    overshoot off them lands rendered medians near 0.11 and makes the whole
    correction anti-correlated with brightness.
    """
    span = K_HLCOMPR_RELIANCE_HIGH - K_HLCOMPR_RELIANCE_LOW
    return max(0.0, min(1.0, (hlcompr - K_HLCOMPR_RELIANCE_LOW) / span))


def rule_proposed(expcomp, hlcompr, args):
    """Give back the overshoot the meter's own hlcompr encodes, less what the
    frame genuinely relies on that compression for."""
    overshoot = over_range_ev(expcomp, hlcompr)
    given = (min(args.giveback_cap, overshoot * args.giveback)
             * (1.0 - (1.0 - K_GIVEBACK_UNDER_RELIANCE) * reliance(hlcompr)))
    committed = expcomp - given

    residual = max(0.0, overshoot - given)
    compression = residual_hlcompr(committed, residual)
    compression = min(K_HLCOMPR_CEILING, max(args.hlcompr_floor, compression))

    return (max(K_EXPOSURE_FLOOR, min(K_HARD_EXPOSURE_CEILING, committed)),
            compression)


RULES = {'today': rule_today, 'proposed': rule_proposed}


def quantiles(values):
    ordered = sorted(values)
    count = len(ordered)

    def at(fraction):
        return ordered[min(count - 1, int(fraction * count))]

    return at


def report(label, values, ceiling=K_HARD_EXPOSURE_CEILING):
    if not values:
        print(f'{label:<34} (no frames)')
        return

    at = quantiles(values)
    count = len(values)
    over_two = 100.0 * sum(1 for v in values if v > 2.0) / count
    at_rail = 100.0 * sum(1 for v in values if v >= ceiling - 0.001) / count
    print(f'{label:<34} p10={at(.10):5.2f} p25={at(.25):5.2f} med={at(.50):5.2f} '
          f'p75={at(.75):5.2f} p90={at(.90):5.2f} max={max(values):5.2f} '
          f'mean={statistics.mean(values):5.2f}  >2EV:{over_two:4.1f}%  '
          f'at ceiling:{at_rail:4.1f}%')


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('cache', nargs='?', default=None,
                        help='thumbnail cache data directory (default: platform cache)')
    parser.add_argument('--rule', choices=sorted(RULES) + ['both'], default='both')
    parser.add_argument('--giveback', type=float, default=1.0,
                        help='share of the measured overshoot to give back (default 1.0)')
    parser.add_argument('--giveback-cap', type=float, default=K_MAX_OVER_RANGE_GIVEBACK,
                        help=f'ceiling on the giveback in EV (default {K_MAX_OVER_RANGE_GIVEBACK})')
    parser.add_argument('--hlcompr-floor', type=int, default=K_HLCOMPR_FLOOR,
                        help=f'residual highlight compression floor (default {K_HLCOMPR_FLOOR})')
    parser.add_argument('--all-formats', action='store_true',
                        help='include JPEG and other non-raw entries')
    parser.add_argument('--grep', default=None,
                        help='also print the full working for frames whose name matches')
    args = parser.parse_args()

    directory = args.cache or default_cache_dir()
    rows, skipped = read_cache(directory, raw_only=not args.all_formats)

    if not rows:
        sys.exit(f'no metered frames found in {directory}')

    print(f'cache: {directory}')
    print(f'metered frames: {len(rows)}'
          + (f'  (skipped {skipped} unreadable)' if skipped else ''))
    print()

    metered = [r[1] for r in rows]
    compressions = [r[2] for r in rows]
    overshoots = [over_range_ev(r[1], r[2]) for r in rows]

    report('metered (raw AE)', metered)
    report('over-range implied by hlcompr', overshoots)

    for name in (sorted(RULES) if args.rule == 'both' else [args.rule]):
        rule = RULES[name]
        committed = [rule(r[1], r[2], args)[0] for r in rows]
        report(f'committed: {name}', committed)

    print()
    saturated_low = sum(1 for c in compressions if c <= 0)
    saturated_high = sum(1 for c in compressions if c >= 100)
    print(f'hlcompr == 0   : {saturated_low:5d} ({100.0*saturated_low/len(rows):4.1f}%)'
          '  -- meter sees no overshoot; the giveback cannot reach these,')
    print('                          '
          '     they need the render-space stages (Phases 2-4)')
    print(f'hlcompr == 100 : {saturated_high:5d} ({100.0*saturated_high/len(rows):4.1f}%)'
          '  -- saturated, so the overshoot above is under-read')

    blind = [r for r in rows if r[2] <= 0 and r[1] > 2.0]
    if blind:
        blind_ev = [r[1] for r in blind]
        at = quantiles(blind_ev)
        print(f'  of the hlcompr == 0 frames, {len(blind)} still meter above +2EV '
              f'(med={at(.50):.2f} max={max(blind_ev):.2f})')

    if args.grep:
        print()
        needle = args.grep.lower()
        for name, expcomp, hlcompr, contrast, black in rows:
            if needle not in name.lower():
                continue
            overshoot = over_range_ev(expcomp, hlcompr)
            today_ev, today_hl = rule_today(expcomp, hlcompr, args)
            new_ev, new_hl = rule_proposed(expcomp, hlcompr, args)
            print(f'{name.split(".")[0]}.{name.split(".")[1]}: '
                  f'metered={expcomp:+.3f} hlcompr={hlcompr} contrast={contrast} black={black}')
            print(f'    over-range = {overshoot:.3f}EV  reliance = {reliance(hlcompr):.3f}')
            print(f'    today      = {today_ev:+.3f}EV  hlcompr={today_hl}')
            print(f'    proposed   = {new_ev:+.3f}EV  hlcompr={new_hl}')


if __name__ == '__main__':
    main()
