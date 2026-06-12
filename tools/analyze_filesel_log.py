#!/usr/bin/env python3
"""
Summarize STEEP_FILESEL_LOG timing output.

Usage:
    python tools/analyze_filesel_log.py
    python tools/analyze_filesel_log.py C:/Users/me/steep-fileSel.log
    python tools/analyze_filesel_log.py steep.log --baseline lightroom.csv

The baseline file can be another steep-fileSel.log or a simple text/CSV file
containing one numeric timing per line in milliseconds.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
from pathlib import Path
from typing import Iterable


OPEN_RE = re.compile(r"\[(?:imageLoaded|fileSel)\] (?:cached )?opened after (\d+)ms openDuration=(\d+)ms file=(.*)")
CACHED_OPEN_RE = re.compile(r"\[fileSel\] cached opened after (\d+)ms openDuration=(\d+)ms file=(.*)")
CALLBACK_RE = re.compile(r"\[imageLoaded\] callback after (\d+)ms file=(.*)")
DECODE_RE = re.compile(
    r"\[loadInitial\] \+(\d+)ms decode done duration=(\d+)ms raw=(\d+) err=(-?\d+) result=(\d+) file=(.*)"
)
GATE_RE = re.compile(r"\[loadInitial\] \+(\d+)ms raw gate acquired wait=(\d+)ms file=(.*)")
HANDOFF_HIT_RE = re.compile(r"\[loadInitial\].*preload handoff HIT .* wait=(\d+)ms file=(.*)")
HANDOFF_MISS_RE = re.compile(r"\[loadInitial\].*preload handoff miss .* wait=(\d+)ms file=(.*)")
PRELOAD_DECODE_RE = re.compile(r"\[preload\] decode done duration=(\d+)ms err=(-?\d+) result=(\d+) file=(.*)")
PRELOAD_CACHED_RE = re.compile(r"\[preload\] cached bytes=(\d+) total=(\d+) entries=(\d+) file=(.*)")
PRELOAD_DROP_RE = re.compile(r"\[preload\] drop decoded")
PRELOAD_YIELD_RE = re.compile(r"\[preload\] yield (?:to foreground|superseded) before decode")
PRELOAD_CACHE_RELEASE_RE = re.compile(
    r"\[preload\] cache release duration=(\d+)ms count=(\d+) reason=(.*?) file=(.*)"
)
PRELOAD_RECYCLED_FOREGROUND_RE = re.compile(
    r"\[preload\] recycled superseded foreground bytes=(\d+) file=(.*)"
)
RECYCLE_TARGET_RE = re.compile(r"\[fileSel\] \+\d+ms marked recycle target file=(.*)")
AUX_DECODE_RE = re.compile(
    r"\[auxLoad\] \+(\d+)ms decode done duration=(\d+)ms raw=(\d+) err=(-?\d+) result=(\d+) file=(.*)"
)
AUX_DEFER_RE = re.compile(r"\[auxLoad\].*raw gate deferred")
AUX_CANCELED_RE = re.compile(r"\[auxLoad\].*canceled")
QUICK_PREVIEW_RE = re.compile(r"\[fileSel\] \+(\d+)ms getCachedPixbuf (HIT|MISS|BUSY)")
QUICK_PREVIEW_DONE_RE = re.compile(r"\[fileSel\] \+(\d+)ms setQuickPreview done")
ASYNC_CACHED_PREVIEW_RE = re.compile(r"\[fileSel\] async cached preview (done|miss|canceled) duration=(\d+)ms")
QUICK_WARM_SCHEDULE_RE = re.compile(
    r"\[quickWarm\] scheduled ready=(\d+) memory=(\d+) disk=(\d+) busy=(\d+) radius=(\d+)"
)
QUICK_WARM_DONE_RE = re.compile(
    r"\[quickWarm\] (done|canceled) duration=(\d+)ms loaded=(\d+) missed=(\d+) remaining=(\d+) total=(\d+)"
)
EDITOR_PLACEHOLDER_DETAIL_RE = re.compile(
    r"\[editorOpen\] placeholder pick duration=\d+ms quickMatch=(\d+) cached=(\d+) old=(\d+)(?: busy=(\d+))?"
)
RAW_LOAD_PHASE_RE = re.compile(
    r"\[rawLoad\] (identify|loadRaw|compress|setup|metadata|total) duration=(\d+)ms(?: .*?)? file=(.*)"
)
EDITOR_PHASE_RE = re.compile(
    r"\[editorOpen\] "
    r"(placeholder pick|close|processor setup|visual setup|phaseA|phaseB core|phaseB scheduled|dir sync opened|browser init) "
    r"duration=(\d+)ms(?: .*?)? file=(.*)"
)
EDITOR_DIR_QUEUED_RE = re.compile(r"\[editorOpen\] dir sync queued(?: .*?delay=(\d+)ms)?")
EDITOR_DIR_COALESCED_RE = re.compile(r"\[editorOpen\] dir sync coalesced")
EDITOR_DIR_POSTPONED_RE = re.compile(r"\[editorOpen\] dir sync postponed")
EDITOR_DIR_SKIPPED_RE = re.compile(r"\[editorOpen\] dir sync skipped")
DISCARDED_RELEASE_RE = re.compile(
    r"\[imageLoaded\] discarded image release duration=(\d+)ms reason=(.*?) file=(.*)"
)
SUPERSEDED_PENDING_RE = re.compile(
    r"\[fileSel\] \+\d+ms superseded pending loads=(\d+) clearedLoading=(\d+)(?: demoted=(\d+))? file=(.*)"
)
NUMBER_RE = re.compile(r"^\s*(?:[^,\s]+[,\s]+)*?(\d+(?:\.\d+)?)\s*(?:ms)?\s*$")
TIME_RE = re.compile(r"\[t=(\d+)ms\]")
LEGACY_FILESEL_ENTER_RE = re.compile(r"---- fileSel ENTER")
LEGACY_STARTFUNC_RE = re.compile(r"\[fileSel\] \+(\d+)ms startFunc kicked")
LEGACY_RAW_DECODE_RE = re.compile(r"\[imageLoaded\] RAW decode finished")
LEGACY_EPANEL_CALL_RE = re.compile(r"\[imageLoaded\] calling epanel->open")
LEGACY_EPANEL_RETURN_RE = re.compile(r"\[imageLoaded\] epanel->open returned")


def default_log_path() -> Path:
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME") or "."
    return Path(home) / "steep-fileSel.log"


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    rank = (len(ordered) - 1) * pct
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def stats(values: list[float]) -> str:
    if not values:
        return "n=0"
    return (
        f"n={len(values)} "
        f"median={statistics.median(values):.0f}ms "
        f"p95={percentile(values, 0.95):.0f}ms "
        f"min={min(values):.0f}ms "
        f"max={max(values):.0f}ms"
    )


def read_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        raise SystemExit(f"Log not found: {path}")


def extract_numeric_baseline(lines: Iterable[str]) -> list[float]:
    values: list[float] = []
    for line in lines:
        match = NUMBER_RE.match(line)
        if match:
            values.append(float(match.group(1)))
    return values


def summarize_log(path: Path) -> dict[str, list[float] | int]:
    opened: list[float] = []
    open_duration: list[float] = []
    cached_opened: list[float] = []
    callbacks: list[float] = []
    foreground_decode: list[float] = []
    raw_gate_wait: list[float] = []
    handoff_hit_wait: list[float] = []
    handoff_miss_wait: list[float] = []
    preload_decode: list[float] = []
    preload_cached = 0
    preload_dropped = 0
    preload_yielded = 0
    preload_cache_release: list[float] = []
    preload_cache_release_count = 0
    preload_recycled_foreground = 0
    preload_recycled_foreground_bytes = 0
    recycle_targets = 0
    aux_decode: list[float] = []
    aux_deferred = 0
    aux_canceled = 0
    quick_preview_hit = 0
    quick_preview_miss = 0
    quick_preview_busy = 0
    quick_preview_lookup: list[float] = []
    quick_preview_done: list[float] = []
    async_cached_preview_done: list[float] = []
    async_cached_preview_miss = 0
    async_cached_preview_canceled = 0
    quick_warm_scheduled = 0
    quick_warm_ready = 0
    quick_warm_memory = 0
    quick_warm_disk = 0
    quick_warm_busy = 0
    quick_warm_done: list[float] = []
    quick_warm_canceled = 0
    quick_warm_loaded = 0
    quick_warm_missed = 0
    quick_warm_remaining = 0
    foreground_start_kick: list[float] = []
    raw_identify: list[float] = []
    raw_loadraw: list[float] = []
    raw_compress: list[float] = []
    raw_setup: list[float] = []
    raw_metadata: list[float] = []
    raw_total: list[float] = []
    editor_placeholder_pick: list[float] = []
    editor_placeholder_quick_match = 0
    editor_placeholder_cached = 0
    editor_placeholder_old = 0
    editor_placeholder_busy = 0
    editor_close: list[float] = []
    editor_processor_setup: list[float] = []
    editor_visual_setup: list[float] = []
    editor_phase_a: list[float] = []
    editor_phase_b_core: list[float] = []
    editor_phase_b_scheduled: list[float] = []
    editor_dir_sync_open: list[float] = []
    editor_dir_sync_delay: list[float] = []
    editor_browser_init: list[float] = []
    editor_dir_sync_queued = 0
    editor_dir_sync_coalesced = 0
    editor_dir_sync_postponed = 0
    editor_dir_sync_skipped = 0
    discarded_release: list[float] = []
    superseded_pending = 0
    superseded_loading_cleared = 0
    superseded_demoted = 0
    fallback_opened: list[float] = []
    fallback_open_duration: list[float] = []
    fallback_foreground_decode: list[float] = []
    last_enter_abs: float | None = None
    active_click_abs: float | None = None
    active_start_abs: float | None = None
    last_epanel_call_abs: float | None = None

    for line in read_lines(path):
        line_time: float | None = None
        if time_match := TIME_RE.search(line):
            line_time = float(time_match.group(1))

        if match := OPEN_RE.search(line):
            opened.append(float(match.group(1)))
            open_duration.append(float(match.group(2)))
            if cached_match := CACHED_OPEN_RE.search(line):
                cached_opened.append(float(cached_match.group(1)))
        elif match := CALLBACK_RE.search(line):
            callbacks.append(float(match.group(1)))
        elif match := DECODE_RE.search(line):
            if match.group(4) == "0" and match.group(5) == "1":
                foreground_decode.append(float(match.group(2)))
        elif match := GATE_RE.search(line):
            raw_gate_wait.append(float(match.group(2)))
        elif match := HANDOFF_HIT_RE.search(line):
            handoff_hit_wait.append(float(match.group(1)))
        elif match := HANDOFF_MISS_RE.search(line):
            handoff_miss_wait.append(float(match.group(1)))
        elif match := PRELOAD_DECODE_RE.search(line):
            if match.group(2) == "0" and match.group(3) == "1":
                preload_decode.append(float(match.group(1)))
        elif PRELOAD_CACHED_RE.search(line):
            preload_cached += 1
        elif PRELOAD_DROP_RE.search(line):
            preload_dropped += 1
        elif PRELOAD_YIELD_RE.search(line):
            preload_yielded += 1
        elif match := PRELOAD_CACHE_RELEASE_RE.search(line):
            preload_cache_release.append(float(match.group(1)))
            preload_cache_release_count += int(match.group(2))
        elif match := PRELOAD_RECYCLED_FOREGROUND_RE.search(line):
            preload_recycled_foreground += 1
            preload_recycled_foreground_bytes += int(match.group(1))
        elif RECYCLE_TARGET_RE.search(line):
            recycle_targets += 1
        elif match := AUX_DECODE_RE.search(line):
            if match.group(4) == "0" and match.group(5) == "1":
                aux_decode.append(float(match.group(2)))
        elif AUX_DEFER_RE.search(line):
            aux_deferred += 1
        elif AUX_CANCELED_RE.search(line):
            aux_canceled += 1
        elif match := QUICK_PREVIEW_RE.search(line):
            quick_preview_lookup.append(float(match.group(1)))
            if match.group(2) == "HIT":
                quick_preview_hit += 1
            elif match.group(2) == "BUSY":
                quick_preview_busy += 1
            else:
                quick_preview_miss += 1
        elif match := QUICK_PREVIEW_DONE_RE.search(line):
            quick_preview_done.append(float(match.group(1)))
        elif match := ASYNC_CACHED_PREVIEW_RE.search(line):
            state = match.group(1)
            if state == "done":
                async_cached_preview_done.append(float(match.group(2)))
            elif state == "miss":
                async_cached_preview_miss += 1
            else:
                async_cached_preview_canceled += 1
        elif match := QUICK_WARM_SCHEDULE_RE.search(line):
            quick_warm_scheduled += 1
            quick_warm_ready += int(match.group(1))
            quick_warm_memory += int(match.group(2))
            quick_warm_disk += int(match.group(3))
            quick_warm_busy += int(match.group(4))
        elif match := QUICK_WARM_DONE_RE.search(line):
            if match.group(1) == "done":
                quick_warm_done.append(float(match.group(2)))
            else:
                quick_warm_canceled += 1
            quick_warm_loaded += int(match.group(3))
            quick_warm_missed += int(match.group(4))
            quick_warm_remaining += int(match.group(5))
        elif match := RAW_LOAD_PHASE_RE.search(line):
            phase = match.group(1)
            value = float(match.group(2))
            if phase == "identify":
                raw_identify.append(value)
            elif phase == "loadRaw":
                raw_loadraw.append(value)
            elif phase == "compress":
                raw_compress.append(value)
            elif phase == "setup":
                raw_setup.append(value)
            elif phase == "metadata":
                raw_metadata.append(value)
            elif phase == "total":
                raw_total.append(value)
        elif match := EDITOR_PHASE_RE.search(line):
            phase = match.group(1)
            value = float(match.group(2))
            if phase == "placeholder pick":
                editor_placeholder_pick.append(value)
                if detail := EDITOR_PLACEHOLDER_DETAIL_RE.search(line):
                    editor_placeholder_quick_match += int(detail.group(1))
                    editor_placeholder_cached += int(detail.group(2))
                    editor_placeholder_old += int(detail.group(3))
                    editor_placeholder_busy += int(detail.group(4) or 0)
            elif phase == "close":
                editor_close.append(value)
            elif phase == "processor setup":
                editor_processor_setup.append(value)
            elif phase == "visual setup":
                editor_visual_setup.append(value)
            elif phase == "phaseA":
                editor_phase_a.append(value)
            elif phase == "phaseB core":
                editor_phase_b_core.append(value)
            elif phase == "phaseB scheduled":
                editor_phase_b_scheduled.append(value)
            elif phase == "dir sync opened":
                editor_dir_sync_open.append(value)
            elif phase == "browser init":
                editor_browser_init.append(value)
        elif match := EDITOR_DIR_QUEUED_RE.search(line):
            editor_dir_sync_queued += 1
            if match.group(1):
                editor_dir_sync_delay.append(float(match.group(1)))
        elif EDITOR_DIR_COALESCED_RE.search(line):
            editor_dir_sync_coalesced += 1
        elif EDITOR_DIR_POSTPONED_RE.search(line):
            editor_dir_sync_postponed += 1
        elif EDITOR_DIR_SKIPPED_RE.search(line):
            editor_dir_sync_skipped += 1
        elif match := DISCARDED_RELEASE_RE.search(line):
            discarded_release.append(float(match.group(1)))
        elif match := SUPERSEDED_PENDING_RE.search(line):
            superseded_pending += int(match.group(1))
            superseded_loading_cleared += int(match.group(2))
            if match.group(3):
                superseded_demoted += int(match.group(3))
        elif match := LEGACY_STARTFUNC_RE.search(line):
            foreground_start_kick.append(float(match.group(1)))

        # Compatibility path for older/lightweight logs that only contain
        # absolute timestamps around fileSelected(), decode completion, and
        # epanel->open(). Detailed "opened after" lines above remain preferred.
        if line_time is None:
            continue
        if LEGACY_FILESEL_ENTER_RE.search(line):
            last_enter_abs = line_time
        elif LEGACY_STARTFUNC_RE.search(line):
            active_click_abs = last_enter_abs if last_enter_abs is not None else line_time
            active_start_abs = line_time
            last_epanel_call_abs = None
        elif LEGACY_RAW_DECODE_RE.search(line):
            if active_start_abs is not None and line_time >= active_start_abs:
                fallback_foreground_decode.append(line_time - active_start_abs)
        elif LEGACY_EPANEL_CALL_RE.search(line):
            last_epanel_call_abs = line_time
        elif LEGACY_EPANEL_RETURN_RE.search(line):
            if active_click_abs is not None and line_time >= active_click_abs:
                fallback_opened.append(line_time - active_click_abs)
            if last_epanel_call_abs is not None and line_time >= last_epanel_call_abs:
                fallback_open_duration.append(line_time - last_epanel_call_abs)
            active_click_abs = None
            active_start_abs = None
            last_epanel_call_abs = None

    return {
        "opened": opened or fallback_opened,
        "open_duration": open_duration or fallback_open_duration,
        "cached_opened": cached_opened,
        "callbacks": callbacks,
        "foreground_decode": foreground_decode or fallback_foreground_decode,
        "raw_gate_wait": raw_gate_wait,
        "handoff_hit_wait": handoff_hit_wait,
        "handoff_miss_wait": handoff_miss_wait,
        "preload_decode": preload_decode,
        "preload_cached": preload_cached,
        "preload_dropped": preload_dropped,
        "preload_yielded": preload_yielded,
        "preload_cache_release": preload_cache_release,
        "preload_cache_release_count": preload_cache_release_count,
        "preload_recycled_foreground": preload_recycled_foreground,
        "preload_recycled_foreground_bytes": preload_recycled_foreground_bytes,
        "recycle_targets": recycle_targets,
        "aux_decode": aux_decode,
        "aux_deferred": aux_deferred,
        "aux_canceled": aux_canceled,
        "quick_preview_hit": quick_preview_hit,
        "quick_preview_miss": quick_preview_miss,
        "quick_preview_busy": quick_preview_busy,
        "quick_preview_lookup": quick_preview_lookup,
        "quick_preview_done": quick_preview_done,
        "async_cached_preview_done": async_cached_preview_done,
        "async_cached_preview_miss": async_cached_preview_miss,
        "async_cached_preview_canceled": async_cached_preview_canceled,
        "quick_warm_scheduled": quick_warm_scheduled,
        "quick_warm_ready": quick_warm_ready,
        "quick_warm_memory": quick_warm_memory,
        "quick_warm_disk": quick_warm_disk,
        "quick_warm_busy": quick_warm_busy,
        "quick_warm_done": quick_warm_done,
        "quick_warm_canceled": quick_warm_canceled,
        "quick_warm_loaded": quick_warm_loaded,
        "quick_warm_missed": quick_warm_missed,
        "quick_warm_remaining": quick_warm_remaining,
        "foreground_start_kick": foreground_start_kick,
        "raw_identify": raw_identify,
        "raw_loadraw": raw_loadraw,
        "raw_compress": raw_compress,
        "raw_setup": raw_setup,
        "raw_metadata": raw_metadata,
        "raw_total": raw_total,
        "editor_placeholder_pick": editor_placeholder_pick,
        "editor_placeholder_quick_match": editor_placeholder_quick_match,
        "editor_placeholder_cached": editor_placeholder_cached,
        "editor_placeholder_old": editor_placeholder_old,
        "editor_placeholder_busy": editor_placeholder_busy,
        "editor_close": editor_close,
        "editor_processor_setup": editor_processor_setup,
        "editor_visual_setup": editor_visual_setup,
        "editor_phase_a": editor_phase_a,
        "editor_phase_b_core": editor_phase_b_core,
        "editor_phase_b_scheduled": editor_phase_b_scheduled,
        "editor_dir_sync_open": editor_dir_sync_open,
        "editor_dir_sync_delay": editor_dir_sync_delay,
        "editor_browser_init": editor_browser_init,
        "editor_dir_sync_queued": editor_dir_sync_queued,
        "editor_dir_sync_coalesced": editor_dir_sync_coalesced,
        "editor_dir_sync_postponed": editor_dir_sync_postponed,
        "editor_dir_sync_skipped": editor_dir_sync_skipped,
        "discarded_release": discarded_release,
        "superseded_pending": superseded_pending,
        "superseded_loading_cleared": superseded_loading_cleared,
        "superseded_demoted": superseded_demoted,
    }


def print_summary(label: str, summary: dict[str, list[float] | int]) -> None:
    print(f"{label}")
    print(f"  click-to-open:        {stats(summary['opened'])}")
    print(f"  cached click-to-open: {stats(summary['cached_opened'])}")
    print(f"  editor open section:  {stats(summary['open_duration'])}")
    print(f"  load callback:        {stats(summary['callbacks'])}")
    print(f"  foreground decode:    {stats(summary['foreground_decode'])}")
    print(f"  raw gate wait:        {stats(summary['raw_gate_wait'])}")
    print(f"  preload handoff hit:  {stats(summary['handoff_hit_wait'])}")
    print(f"  preload handoff miss: {stats(summary['handoff_miss_wait'])}")
    print(f"  preload decode:       {stats(summary['preload_decode'])}")
    print(f"  preload cached/drop:  {summary['preload_cached']}/{summary['preload_dropped']}")
    print(f"  preload yielded:      {summary['preload_yielded']}")
    print(
        "  preload release:      "
        f"{stats(summary['preload_cache_release'])} count={summary['preload_cache_release_count']}"
    )
    print(
        "  recycled foreground:  "
        f"{summary['preload_recycled_foreground']}/{summary['recycle_targets']} "
        f"bytes={summary['preload_recycled_foreground_bytes']}"
    )
    print(f"  aux decode:           {stats(summary['aux_decode'])}")
    print(f"  aux deferred/cancel:  {summary['aux_deferred']}/{summary['aux_canceled']}")
    print(
        "  quick preview hit/miss/busy: "
        f"{summary['quick_preview_hit']}/{summary['quick_preview_miss']}/{summary['quick_preview_busy']}"
    )
    print(f"  quick preview lookup: {stats(summary['quick_preview_lookup'])}")
    print(f"  quick preview done:   {stats(summary['quick_preview_done'])}")
    print(f"  async cached preview: {stats(summary['async_cached_preview_done'])} miss/cancel={summary['async_cached_preview_miss']}/{summary['async_cached_preview_canceled']}")
    print(
        "  quick warm schedule:  "
        f"n={summary['quick_warm_scheduled']} "
        f"ready/memory/disk/busy="
        f"{summary['quick_warm_ready']}/"
        f"{summary['quick_warm_memory']}/"
        f"{summary['quick_warm_disk']}/"
        f"{summary['quick_warm_busy']}"
    )
    print(
        "  quick warm disk:      "
        f"{stats(summary['quick_warm_done'])} "
        f"loaded/miss/cancel/remain="
        f"{summary['quick_warm_loaded']}/"
        f"{summary['quick_warm_missed']}/"
        f"{summary['quick_warm_canceled']}/"
        f"{summary['quick_warm_remaining']}"
    )
    print(f"  decode kick:          {stats(summary['foreground_start_kick'])}")
    print(f"  raw identify:         {stats(summary['raw_identify'])}")
    print(f"  raw loadRaw:          {stats(summary['raw_loadraw'])}")
    print(f"  raw compress:         {stats(summary['raw_compress'])}")
    print(f"  raw setup:            {stats(summary['raw_setup'])}")
    print(f"  raw metadata:         {stats(summary['raw_metadata'])}")
    print(f"  raw total:            {stats(summary['raw_total'])}")
    print(f"  editor phase A:       {stats(summary['editor_phase_a'])}")
    print(
        "  editor placeholder:   "
        f"quick={summary['editor_placeholder_quick_match']} "
        f"cached={summary['editor_placeholder_cached']} "
        f"old={summary['editor_placeholder_old']} "
        f"busy={summary['editor_placeholder_busy']}"
    )
    print(f"  editor close:         {stats(summary['editor_close'])}")
    print(f"  editor processor:     {stats(summary['editor_processor_setup'])}")
    print(f"  editor visual setup:  {stats(summary['editor_visual_setup'])}")
    print(f"  editor phase B core:  {stats(summary['editor_phase_b_core'])}")
    print(f"  editor phase B total: {stats(summary['editor_phase_b_scheduled'])}")
    print(f"  dir sync open:        {stats(summary['editor_dir_sync_open'])}")
    print(f"  dir sync delay:       {stats(summary['editor_dir_sync_delay'])}")
    print(f"  browser init:         {stats(summary['editor_browser_init'])}")
    print(
        "  dir sync queue/coal/post/skip: "
        f"{summary['editor_dir_sync_queued']}/"
        f"{summary['editor_dir_sync_coalesced']}/"
        f"{summary['editor_dir_sync_postponed']}/"
        f"{summary['editor_dir_sync_skipped']}"
    )
    print(f"  discarded release:    {stats(summary['discarded_release'])}")
    print(
        "  superseded pending:   "
        f"{summary['superseded_pending']} cleared={summary['superseded_loading_cleared']}"
        f" demoted={summary.get('superseded_demoted', 0)}"
    )


def print_comparison(current: list[float], baseline_path: Path) -> None:
    baseline_summary = summarize_log(baseline_path)
    baseline = baseline_summary["opened"]
    if not baseline:
        baseline = extract_numeric_baseline(read_lines(baseline_path))

    if not current or not baseline:
        print("comparison: not enough click-to-open timings")
        return

    current_median = statistics.median(current)
    baseline_median = statistics.median(baseline)
    delta = current_median - baseline_median
    pct = (delta / baseline_median * 100.0) if baseline_median else 0.0
    faster_slower = "faster" if delta < 0 else "slower"
    print(
        f"comparison median: current={current_median:.0f}ms "
        f"baseline={baseline_median:.0f}ms "
        f"delta={abs(delta):.0f}ms {faster_slower} ({abs(pct):.1f}%)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize STEEP_FILESEL_LOG timings.")
    parser.add_argument("log", nargs="?", type=Path, default=default_log_path())
    parser.add_argument("--baseline", type=Path, help="Baseline log or file with one timing per line.")
    args = parser.parse_args()

    summary = summarize_log(args.log)
    print_summary(str(args.log), summary)
    if args.baseline:
        print_comparison(summary["opened"], args.baseline)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
