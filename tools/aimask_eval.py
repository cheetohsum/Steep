#!/usr/bin/env python3
"""Score Steep's AI masks against ADE20K ground truth.

Steep's segmentation model (MIT CSAIL MobileNetV2dilated + C1, trained on
ADE20K) predicts 150 ADE classes that rtengine folds into 8 AISegClass values,
plus two composed pseudo-classes (SUBJECT, NOT_SUBJECT). This tool replicates
that fold on ADE20K annotations and measures how well a run of predicted masks
matches. The dataset is a local benchmark only - it is never shipped.

Prediction layout (one directory per run):

    RUN/<stem>/<class>.png     8-bit grayscale probability, 0..255 = 0..1,
                               any resolution (resized bilinearly to the GT).
                               <class> is the AISegClass number (0..9) or its
                               lower-case name (person.png, subject.png, ...).

A missing class file means "this run did not produce that class for this
image": it is skipped for that image, not scored as empty.

Ground truth (--gt) is the raw ADE20K annotation PNG: 0 = unlabeled,
k = ADE class index k-1. Unlabeled pixels are ignored by every metric.
SUBJECT ground truth is the plain union of person/vehicle/animal/foreground
pixels (no connected-component filtering); NOT_SUBJECT is its complement over
labelled pixels.

Metrics per class and image: IoU at threshold 0.5, soft IoU (sum min / sum
max of probability vs binary GT), and boundary F-score (BF: precision/recall
of predicted vs GT boundary pixels within 0.75% of the image diagonal). For
BF only, unlabeled seams are filled from the nearest labelled pixel - see
boundary_context().
A class is averaged over the images where it is present in GT or predicted
(>= 0.5 somewhere on labelled pixels). "overall" is the mean over classes 1-9
that were scored (BACKGROUND is a catch-all and is left out).

Usage:

    # score a run (prints a table, writes RUN/scores.csv + scores_per_image.csv)
    python tools/aimask_eval.py --pred RUN --gt EVAL/gt --images EVAL/images

    # side-by-side comparison (scores any run that has no scores.csv yet)
    python tools/aimask_eval.py --compare RUN_A RUN_B --gt EVAL/gt

    # contact sheet: image | GT | each run, for one class; works without
    # --gt for arbitrary photos (then image + overlays only)
    python tools/aimask_eval.py --sheet out.jpg --runs RUN_A RUN_B \\
        --stems ADE_val_00000001 ADE_val_00000002 --class 8 \\
        --images EVAL/images --gt EVAL/gt

    # build the evaluation subset from an unzipped ADEChallengeData2016
    python tools/aimask_eval.py --make-subset ADE_ROOT --out EVAL [--count 200]

    # scorer sanity checks: predictions made from the GT itself
    python tools/aimask_eval.py --make-fake RUN_GT --gt EVAL/gt
    python tools/aimask_eval.py --make-fake RUN_GT8 --gt EVAL/gt --fake-scale 8
"""

import argparse
import csv
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

# --------------------------------------------------------------------------
# Class definitions - mirror rtengine/aisegmentation.h (enum AISegClass) and
# adeToAIClass() in rtengine/aisegmentation.cc. When the C++ changes, change
# this too.
# --------------------------------------------------------------------------

BACKGROUND, PERSON, SKY, VEGETATION, BUILDING, VEHICLE, ANIMAL, FOREGROUND_OBJECT = range(8)
SUBJECT, NOT_SUBJECT = 8, 9
NUM_MODEL_CLASSES = 8
TOTAL_CLASSES = 10

CLASS_NAMES = ["background", "person", "sky", "vegetation", "building",
               "vehicle", "animal", "foreground", "subject", "not_subject"]

# Classes unioned into SUBJECT - rtengine/aisubject.h appendSubjectMasks().
SUBJECT_PARTS = (PERSON, VEHICLE, ANIMAL, FOREGROUND_OBJECT)

# 0-based ADE20K class index -> AISegClass. Anything not listed is BACKGROUND.
# Copied from adeToAIClass() in rtengine/aisegmentation.cc - keep in sync.
_ADE_GROUPS = {
    PERSON: [12, 92],
    SKY: [2],
    VEGETATION: [4, 9, 16, 17, 29, 66, 68, 72, 94],
    BUILDING: [0, 1, 5, 8, 14, 25, 32, 38, 40, 42, 48, 53, 58, 59, 61, 79,
               84, 86, 88, 95, 106, 121, 140],
    VEHICLE: [20, 76, 80, 83, 90, 102, 103, 116, 122, 127],
    ANIMAL: [126],
    FOREGROUND_OBJECT: [7, 10, 15, 18, 19, 22, 23, 24, 27, 28, 30, 31, 33, 35,
                        36, 37, 39, 41, 43, 44, 45, 47, 49, 50, 55, 56, 57,
                        62, 63, 64, 65, 67, 69, 70, 71, 73, 74, 75, 77, 78,
                        81, 82, 85, 87, 89, 93, 97, 98, 99, 100, 101, 104,
                        105, 107, 108, 110, 111, 112, 114, 115, 117, 118,
                        119, 120, 123, 124, 125, 129, 130, 131, 132, 133,
                        134, 135, 136, 137, 138, 139, 141, 142, 143, 144,
                        145, 146, 147, 148, 149],
}


def ade_to_steep(ade_index):
    """0-based ADE class index -> AISegClass (adeToAIClass in the C++)."""
    for cls, members in _ADE_GROUPS.items():
        if ade_index in members:
            return cls
    return BACKGROUND


IGNORE = 255
# Annotation pixel value -> AISegClass; 0 (unlabeled) -> IGNORE.
GT_LUT = np.full(256, IGNORE, dtype=np.uint8)
for _v in range(1, 151):
    GT_LUT[_v] = ade_to_steep(_v - 1)

OVERLAY_COLORS = {
    BACKGROUND: (150, 150, 150), PERSON: (255, 140, 0), SKY: (40, 140, 255),
    VEGETATION: (40, 210, 60), BUILDING: (170, 90, 255), VEHICLE: (255, 40, 40),
    ANIMAL: (255, 220, 0), FOREGROUND_OBJECT: (0, 220, 220),
    SUBJECT: (255, 0, 200), NOT_SUBJECT: (0, 170, 140),
}

IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp")
BF_TOLERANCE = 0.0075  # fraction of the image diagonal


# --------------------------------------------------------------------------
# Ground truth / prediction loading
# --------------------------------------------------------------------------

def load_gt_labels(path):
    """Raw ADE annotation -> HxW uint8 AISegClass labels (IGNORE = unlabeled)."""
    ann = np.asarray(Image.open(path))
    if ann.ndim == 3:
        ann = ann[..., 0]
    return GT_LUT[ann]


def gt_class_mask(labels, cls):
    """Binary GT mask for any of the 10 classes (unlabeled is never set)."""
    if cls < NUM_MODEL_CLASSES:
        return labels == cls
    subject = np.isin(labels, SUBJECT_PARTS)
    if cls == SUBJECT:
        return subject
    return (labels != IGNORE) & ~subject


def find_pred_file(run_dir, stem, cls):
    for name in (str(cls), CLASS_NAMES[cls]):
        p = os.path.join(run_dir, stem, name + ".png")
        if os.path.isfile(p):
            return p
    return None


def load_prob(path, size_wh):
    """8-bit probability PNG -> float32 HxW in [0,1], bilinear to size."""
    im = Image.open(path)
    if im.mode not in ("L", "I", "I;16", "F"):
        im = im.convert("L")
    scale = 65535.0 if im.mode.startswith("I") and np.asarray(im).max() > 255 else 255.0
    arr = np.asarray(im, dtype=np.float32) / scale
    if (arr.shape[1], arr.shape[0]) != tuple(size_wh):
        arr = np.asarray(Image.fromarray(arr, mode="F").resize(size_wh, Image.BILINEAR))
    return np.clip(arr, 0.0, 1.0)


def find_image(images_dir, stem):
    if not images_dir:
        return None
    for ext in IMAGE_EXTS:
        p = os.path.join(images_dir, stem + ext)
        if os.path.isfile(p):
            return p
    return None


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------

def _boundary(mask):
    """Inner boundary pixels; the image border itself is not a boundary."""
    if not mask.any():
        return mask
    return mask & ~ndimage.binary_erosion(mask, border_value=1)


def boundary_context(valid, tol_px):
    """Per-image helper for boundary_f's handling of unlabeled pixels.

    ADE leaves thin unlabeled seams between most objects (and some larger
    unlabeled blobs). Taken literally, every seam is a GT boundary against
    "unknown" that no prediction reproduces, and every predicted edge along a
    blob is a false positive: GT-derived predictions scored BF 0.50-0.57 on
    NOT_SUBJECT that way. So, for BF only:
      * the GT is filled from the nearest labelled pixel, so a seam becomes a
        boundary along its middle; GT boundary pixels more than tol_px inside
        an unlabeled area are dropped (nothing there to measure against);
      * a predicted boundary pixel within tol_px of an unlabeled pixel that
        does not match a GT boundary is "unknown", not a false positive.
    Returns (fill_indices, deep_mask, near_unlabeled) or None when every pixel
    is labelled.
    """
    if valid.all():
        return None
    dist, idx = ndimage.distance_transform_edt(~valid, return_indices=True)
    near = ndimage.distance_transform_edt(valid) <= tol_px
    return idx, dist > tol_px, near


def boundary_f(pred_bin, gt_bin, tol_px, ctx=None):
    """BF score: boundary precision/recall within tol_px pixels.

    ctx is boundary_context() for the image (None = fully labelled).
    """
    pb = _boundary(pred_bin)
    if ctx is not None:
        idx, deep, near = ctx
        gb = _boundary(gt_bin[idx[0], idx[1]]) & ~deep
    else:
        gb = _boundary(gt_bin)
    ng = int(gb.sum())
    if ng:
        dist_to_gt = ndimage.distance_transform_edt(~gb)
        hit = pb & (dist_to_gt <= tol_px)
    else:
        hit = np.zeros_like(pb)
    counted = hit | (pb & ~near) if ctx is not None else pb
    np_ = int(counted.sum())
    if np_ == 0 and ng == 0:
        return 1.0
    if np_ == 0 or ng == 0:
        return 0.0
    precision = int(hit.sum()) / np_
    dist_to_pred = ndimage.distance_transform_edt(~pb)
    recall = float((dist_to_pred[gb] <= tol_px).mean())
    if precision + recall == 0:
        return 0.0
    return 2 * precision * recall / (precision + recall)


def score_class(prob, gt_bin, valid, tol_px, ctx=False):
    """Metrics for one class on one image, or None if absent in both.

    ctx: boundary_context(valid, tol_px), computed here if not passed.
    """
    if ctx is False:
        ctx = boundary_context(valid, tol_px)
    p = prob[valid]
    g = gt_bin[valid]
    pb = p >= 0.5
    if not g.any() and not pb.any():
        return None
    inter = np.count_nonzero(pb & g)
    union = np.count_nonzero(pb | g)
    gf = g.astype(np.float32)
    soft_den = float(np.sum(p + gf - p * gf))
    return {
        "iou": inter / union if union else 1.0,
        "soft_iou": float(np.sum(p * gf)) / soft_den if soft_den > 0 else 1.0,
        "bf": boundary_f(prob >= 0.5, gt_bin, tol_px, ctx),
        "gt_frac": float(g.mean()),
        "pred_frac": float(pb.mean()),
    }


def score_image(run_dir, stem, gt_path, classes, tol_frac=BF_TOLERANCE):
    """{cls: metrics or None}; classes without a prediction file are omitted."""
    labels = load_gt_labels(gt_path)
    h, w = labels.shape
    valid = labels != IGNORE
    tol_px = tol_frac * float(np.hypot(h, w))
    ctx = boundary_context(valid, tol_px)
    out = {}
    for cls in classes:
        pf = find_pred_file(run_dir, stem, cls)
        if pf is None:
            continue
        prob = load_prob(pf, (w, h))
        out[cls] = score_class(prob, gt_class_mask(labels, cls), valid, tol_px, ctx)
    return out


def run_stems(run_dir):
    return sorted(d for d in os.listdir(run_dir)
                  if os.path.isdir(os.path.join(run_dir, d)))


def score_run(run_dir, gt_dir, tol_frac=BF_TOLERANCE, quiet=False):
    """Score every <stem> in run_dir that has GT. Returns the summary rows."""
    stems = [s for s in run_stems(run_dir)
             if os.path.isfile(os.path.join(gt_dir, s + ".png"))]
    if not stems:
        sys.exit(f"no prediction folders in {run_dir} match GT in {gt_dir}")
    per_image = []
    acc = {c: [] for c in range(TOTAL_CLASSES)}
    missing = {c: 0 for c in range(TOTAL_CLASSES)}
    for i, stem in enumerate(stems):
        res = score_image(run_dir, stem, os.path.join(gt_dir, stem + ".png"),
                          range(TOTAL_CLASSES), tol_frac)
        for c in range(TOTAL_CLASSES):
            if c not in res:
                missing[c] += 1
                continue
            m = res[c]
            if m is None:
                continue
            acc[c].append(m)
            per_image.append({"stem": stem, "class": c, "name": CLASS_NAMES[c], **m})
        if not quiet and (i + 1) % 25 == 0:
            print(f"  scored {i + 1}/{len(stems)}", file=sys.stderr)

    summary = []
    for c in range(TOTAL_CLASSES):
        ms = acc[c]
        row = {"class": c, "name": CLASS_NAMES[c], "n": len(ms),
               "missing": missing[c]}
        for k in ("iou", "soft_iou", "bf"):
            row[k] = float(np.mean([m[k] for m in ms])) if ms else float("nan")
        summary.append(row)
    scored = [r for r in summary if r["class"] >= 1 and r["n"] > 0]
    overall = {"class": "overall", "name": "mean(1-9)",
               "n": sum(r["n"] for r in scored), "missing": "",
               "iou": float(np.mean([r["iou"] for r in scored])) if scored else float("nan"),
               "soft_iou": float(np.mean([r["soft_iou"] for r in scored])) if scored else float("nan"),
               "bf": float(np.mean([r["bf"] for r in scored])) if scored else float("nan")}
    summary.append(overall)

    fields = ["class", "name", "n", "missing", "iou", "soft_iou", "bf"]
    with open(os.path.join(run_dir, "scores.csv"), "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        for r in summary:
            wr.writerow({k: (f"{r[k]:.4f}" if isinstance(r[k], float) else r[k]) for k in fields})
    with open(os.path.join(run_dir, "scores_per_image.csv"), "w", newline="") as f:
        pf = ["stem", "class", "name", "iou", "soft_iou", "bf", "gt_frac", "pred_frac"]
        wr = csv.DictWriter(f, fieldnames=pf)
        wr.writeheader()
        for r in per_image:
            wr.writerow({k: (f"{r[k]:.4f}" if isinstance(r[k], float) else r[k]) for k in pf})
    return summary


def read_scores(run_dir):
    with open(os.path.join(run_dir, "scores.csv"), newline="") as f:
        return list(csv.DictReader(f))


def print_summary(run_dir, summary):
    print(f"\n{run_dir}")
    print(f"{'class':<14}{'n':>5}{'IoU@.5':>9}{'softIoU':>9}{'BF':>9}")
    for r in summary:
        def fmt(v):
            v = float(v)
            return f"{v:9.3f}" if v == v else f"{'-':>9}"
        name = r["name"] if str(r["class"]) != "overall" else "OVERALL 1-9"
        print(f"{name:<14}{int(r['n']):>5}{fmt(r['iou'])}{fmt(r['soft_iou'])}{fmt(r['bf'])}")


def compare(runs, gt_dir, tol_frac):
    tables = []
    for run in runs:
        if not os.path.isfile(os.path.join(run, "scores.csv")):
            if not gt_dir:
                sys.exit(f"{run} has no scores.csv; pass --gt to score it")
            score_run(run, gt_dir, tol_frac)
        tables.append({r["class"]: r for r in read_scores(run)})
    names = [os.path.basename(os.path.normpath(r)) for r in runs]
    colw = max(17, max(len(n) for n in names) + 2)
    for metric, label in (("iou", "IoU@0.5"), ("soft_iou", "soft IoU"), ("bf", "boundary F")):
        print(f"\n{label}")
        print(f"{'class':<14}" + "".join(f"{n:>{colw}}" for n in names))
        keys = [str(c) for c in range(TOTAL_CLASSES)] + ["overall"]
        for k in keys:
            cells = []
            for t in tables:
                r = t.get(k)
                if r is None or r[metric] in ("", "nan"):
                    cells.append(f"{'-':>{colw}}")
                else:
                    cells.append(f"{float(r[metric]):>{colw - 8}.3f} (n{int(r['n']):>4})")
            name = CLASS_NAMES[int(k)] if k != "overall" else "OVERALL 1-9"
            print(f"{name:<14}" + "".join(cells))


# --------------------------------------------------------------------------
# Contact sheet
# --------------------------------------------------------------------------

def _font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def _overlay(base, prob, color, alpha=0.6):
    rgb = np.asarray(base, dtype=np.float32)
    a = (prob * alpha)[..., None]
    out = rgb * (1 - a) + np.array(color, dtype=np.float32) * a
    # Outline the 0.5 contour so the thresholded mask edge reads clearly.
    edge = _boundary(prob >= 0.5)
    edge = ndimage.binary_dilation(edge, iterations=max(1, base.height // 300))
    out[edge] = color
    return Image.fromarray(out.clip(0, 255).astype(np.uint8))


def _labelled(tile, text, font, strip):
    canvas = Image.new("RGB", (tile.width, tile.height + strip), (24, 24, 24))
    canvas.paste(tile, (0, strip))
    ImageDraw.Draw(canvas).text((4, 2), text, fill=(235, 235, 235), font=font)
    return canvas


def contact_sheet(out_path, runs, stems, cls, images_dir, gt_dir, tile_h, tol_frac):
    if not stems:
        stems = run_stems(runs[0])
    font = _font(max(11, tile_h // 16))
    strip = max(16, tile_h // 12)
    color = OVERLAY_COLORS[cls]
    rows = []
    for stem in stems:
        img_path = find_image(images_dir, stem)
        gt_path = os.path.join(gt_dir, stem + ".png") if gt_dir else None
        if gt_path and not os.path.isfile(gt_path):
            gt_path = None
        if img_path is None:
            print(f"warning: no image for {stem} in {images_dir}", file=sys.stderr)
            continue
        image = Image.open(img_path).convert("RGB")
        w, h = image.size
        labels = load_gt_labels(gt_path) if gt_path else None
        if labels is not None and labels.shape != (h, w):
            labels = np.asarray(Image.fromarray(labels).resize((w, h), Image.NEAREST))
        tw = max(1, round(w * tile_h / h))
        small = image.resize((tw, tile_h), Image.LANCZOS)
        tiles = [_labelled(small, stem, font, strip)]
        valid = gt_bin = None
        if labels is not None:
            valid = labels != IGNORE
            gt_bin = gt_class_mask(labels, cls)
            g = np.asarray(Image.fromarray(gt_bin.astype(np.float32), "F")
                           .resize((tw, tile_h), Image.BILINEAR))
            gt_tile = _overlay(small, g, color)
            # Hatch unlabeled pixels dark so "ignored" is visible.
            v = np.asarray(Image.fromarray(valid.astype(np.uint8) * 255)
                           .resize((tw, tile_h), Image.NEAREST)) > 127
            arr = np.asarray(gt_tile).copy()
            arr[~v] = (arr[~v] * 0.35).astype(np.uint8)
            tiles.append(_labelled(Image.fromarray(arr), f"GT {CLASS_NAMES[cls]}", font, strip))
        tol_px = tol_frac * float(np.hypot(h, w))
        for run in runs:
            name = os.path.basename(os.path.normpath(run))
            pf = find_pred_file(run, stem, cls)
            if pf is None:
                blank = Image.new("RGB", (tw, tile_h), (40, 40, 40))
                tiles.append(_labelled(blank, f"{name}: missing", font, strip))
                continue
            prob = load_prob(pf, (w, h))
            text = name
            if labels is not None:
                m = score_class(prob, gt_bin, valid, tol_px)
                text += "  absent" if m is None else f"  IoU {m['iou']:.2f} BF {m['bf']:.2f}"
            p_small = np.asarray(Image.fromarray(prob, "F").resize((tw, tile_h), Image.BILINEAR))
            tiles.append(_labelled(_overlay(small, p_small, color), text, font, strip))
        rows.append(tiles)
    if not rows:
        sys.exit("nothing to draw")
    ncols = max(len(r) for r in rows)
    colw = [max(r[i].width for r in rows if i < len(r)) for i in range(ncols)]
    pad = 4
    sheet = Image.new("RGB", (sum(colw) + pad * (ncols + 1),
                              len(rows) * (tile_h + strip + pad) + pad), (12, 12, 12))
    y = pad
    for r in rows:
        x = pad
        for i, t in enumerate(r):
            sheet.paste(t, (x, y))
            x += colw[i] + pad
        y += tile_h + strip + pad
    sheet.save(out_path, quality=90)
    print(f"wrote {out_path} ({len(rows)} rows, class {cls} {CLASS_NAMES[cls]})")


# --------------------------------------------------------------------------
# Subset builder and fake predictions
# --------------------------------------------------------------------------

def class_coverage(labels):
    """Fraction of all pixels per class 0..8 (SUBJECT = union)."""
    n = labels.size
    counts = np.bincount(labels.ravel(), minlength=256)
    cov = [counts[c] / n for c in range(NUM_MODEL_CLASSES)]
    cov.append(sum(counts[c] for c in SUBJECT_PARTS) / n)
    return cov


def make_subset(ade_root, out_dir, count, per_class, min_cov, min_short, seed):
    img_dir = os.path.join(ade_root, "images", "validation")
    ann_dir = os.path.join(ade_root, "annotations", "validation")
    stems = sorted(os.path.splitext(f)[0] for f in os.listdir(ann_dir) if f.endswith(".png"))
    print(f"scanning {len(stems)} validation annotations...", file=sys.stderr)
    info = {}
    for s in stems:
        labels = load_gt_labels(os.path.join(ann_dir, s + ".png"))
        h, w = labels.shape
        info[s] = (np.array(class_coverage(labels)), min(h, w))
    strat = list(range(SUBJECT + 1))  # 0..8; NOT_SUBJECT is SUBJECT's complement
    rng = np.random.default_rng(seed)
    pool = [s for s in stems if info[s][1] >= min_short]
    small = [s for s in stems if info[s][1] < min_short]
    order = list(rng.permutation(pool)) + list(rng.permutation(small))
    chosen = []
    have = np.zeros(len(strat), dtype=int)
    # Greedy: each pick covers the most still-deficient classes; images below
    # min_short pay a half-class penalty, seeded shuffle breaks ties.
    while len(chosen) < count:
        deficit = np.maximum(per_class - have, 0)
        if not deficit.any():
            break
        best, best_gain = None, 0.0
        for s in order:
            if s in chosen:
                continue
            gain = float(((info[s][0][strat] >= min_cov) & (deficit > 0)).sum())
            if gain > 0 and info[s][1] < min_short:
                gain -= 0.5
            if gain > best_gain:
                best, best_gain = s, gain
        if best is None:
            break
        chosen.append(best)
        have += info[best][0][strat] >= min_cov
    # ADE is scene-centric: some classes (ANIMAL: 3 val images at >= 15%, 23
    # with any) cannot reach per_class at min_cov. Top them up with the
    # images where they appear at all (>= 0.5%), highest coverage first.
    for c in strat:
        present = int(sum(info[s][0][c] >= 0.005 for s in chosen))
        extra = sorted((s for s in stems if s not in chosen and info[s][0][c] >= 0.005),
                       key=lambda s: -info[s][0][c])
        for s in extra[:max(0, min(per_class - present, count - len(chosen)))]:
            chosen.append(s)
    # Fill the rest with mixed scenes: >= 3 model classes (1..7) at >= 10%.
    def is_mixed(s):
        return int((info[s][0][1:NUM_MODEL_CLASSES] >= 0.10).sum()) >= 3
    mixed = [s for s in order if s not in chosen and is_mixed(s)]
    for s in mixed[:max(0, count - len(chosen))]:
        chosen.append(s)
    for s in order:
        if len(chosen) >= count:
            break
        if s not in chosen:
            chosen.append(s)
    chosen.sort()

    os.makedirs(os.path.join(out_dir, "images"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "gt"), exist_ok=True)
    lines = []
    with open(os.path.join(out_dir, "subset_coverage.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["stem", "short_side"] + CLASS_NAMES[:SUBJECT + 1])
        for s in chosen:
            dst = os.path.join(out_dir, "images", s + ".jpg")
            shutil.copyfile(os.path.join(img_dir, s + ".jpg"), dst)
            shutil.copyfile(os.path.join(ann_dir, s + ".png"), os.path.join(out_dir, "gt", s + ".png"))
            lines.append(os.path.abspath(dst))
            wr.writerow([s, info[s][1]] + [f"{v:.4f}" for v in info[s][0]])
    with open(os.path.join(out_dir, "subset.txt"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")

    covs = np.array([info[s][0] for s in chosen])
    print(f"chosen {len(chosen)} images (seed {seed}); "
          f"{sum(info[s][1] >= min_short for s in chosen)} with short side >= {min_short}; "
          f"{sum(is_mixed(s) for s in chosen)} mixed (>=3 classes 1-7 at >=10%)")
    print(f"{'class':<14}{'>=' + str(int(min_cov * 100)) + '%':>7}{'>=0.5%':>8}{'mean cov':>10}")
    for c in strat:
        print(f"{CLASS_NAMES[c]:<14}{int((covs[:, c] >= min_cov).sum()):>7}"
              f"{int((covs[:, c] >= 0.005).sum()):>8}{covs[:, c].mean():>10.3f}")


def make_fake(out_dir, gt_dir, scale):
    """Predictions taken from the GT itself; scale > 1 box-downsamples the
    masks by that factor and stores them small (the scorer upsamples)."""
    stems = sorted(os.path.splitext(f)[0] for f in os.listdir(gt_dir) if f.endswith(".png"))
    for s in stems:
        labels = load_gt_labels(os.path.join(gt_dir, s + ".png"))
        h, w = labels.shape
        d = os.path.join(out_dir, s)
        os.makedirs(d, exist_ok=True)
        for c in range(TOTAL_CLASSES):
            m = Image.fromarray(gt_class_mask(labels, c).astype(np.float32), "F")
            if scale > 1:
                m = m.resize((max(1, round(w / scale)), max(1, round(h / scale))), Image.BOX)
            arr = (np.asarray(m) * 255 + 0.5).clip(0, 255).astype(np.uint8)
            Image.fromarray(arr, "L").save(os.path.join(d, f"{c}.png"))
    print(f"wrote fake predictions for {len(stems)} images to {out_dir} (scale 1/{scale})")


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--pred", metavar="DIR", help="score one run")
    mode.add_argument("--compare", nargs="+", metavar="DIR", help="compare runs side by side")
    mode.add_argument("--sheet", metavar="OUT.jpg", help="render a contact sheet")
    mode.add_argument("--make-subset", metavar="ADE_ROOT", help="build the evaluation subset")
    mode.add_argument("--make-fake", metavar="DIR", help="write GT-derived predictions")
    ap.add_argument("--gt", metavar="DIR", help="raw ADE annotation PNGs, <stem>.png")
    ap.add_argument("--images", metavar="DIR", help="photos, <stem>.jpg (for --sheet)")
    ap.add_argument("--runs", nargs="+", metavar="DIR", help="runs to show in --sheet")
    ap.add_argument("--stems", nargs="+", help="images to show in --sheet (default: all of the first run)")
    ap.add_argument("--class", dest="cls", type=int, default=SUBJECT,
                    help="class for --sheet, 0-9 (default 8 = subject)")
    ap.add_argument("--tile-h", type=int, default=240, help="--sheet tile height")
    ap.add_argument("--bf-tol", type=float, default=BF_TOLERANCE,
                    help="boundary tolerance as a fraction of the diagonal (default 0.0075)")
    ap.add_argument("--out", metavar="DIR", help="output dir for --make-subset")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--per-class", type=int, default=30)
    ap.add_argument("--min-cov", type=float, default=0.15)
    ap.add_argument("--min-short", type=int, default=512)
    ap.add_argument("--seed", type=int, default=20261022)
    ap.add_argument("--fake-scale", type=int, default=1)
    a = ap.parse_args()

    if a.pred:
        if not a.gt:
            ap.error("--pred needs --gt")
        print_summary(a.pred, score_run(a.pred, a.gt, a.bf_tol))
    elif a.compare:
        compare(a.compare, a.gt, a.bf_tol)
    elif a.sheet:
        if not a.runs:
            ap.error("--sheet needs --runs")
        if not 0 <= a.cls < TOTAL_CLASSES:
            ap.error("--class must be 0-9")
        contact_sheet(a.sheet, a.runs, a.stems, a.cls, a.images, a.gt, a.tile_h, a.bf_tol)
    elif a.make_subset:
        if not a.out:
            ap.error("--make-subset needs --out")
        make_subset(a.make_subset, a.out, a.count, a.per_class, a.min_cov, a.min_short, a.seed)
    elif a.make_fake:
        if not a.gt:
            ap.error("--make-fake needs --gt")
        make_fake(a.make_fake, a.gt, a.fake_scale)


if __name__ == "__main__":
    main()
