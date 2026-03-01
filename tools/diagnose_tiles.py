#!/usr/bin/env python3
"""
Diagnostic tool: run the Tree Net Denoise model on an image and dump
per-tile outputs + overlap disagreement maps.

Usage:
    python diagnose_tiles.py --input-tiff <path> --outdir ./tile_diag \
        --iso-strength 50 [--gpu]

What it produces in --outdir:
  tile_YY_XX.tif        - Individual 256x256 tile outputs (a sample)
  input_tile_YY_XX.tif  - Corresponding input tiles (for comparison)
  overlap_h_YY_XX.tif   - Horizontal overlap difference (tile vs right neighbor)
  overlap_v_YY_XX.tif   - Vertical overlap difference (tile vs bottom neighbor)
  disagreement_map.tif  - Full-image map: max absolute disagreement at each pixel
  naive_stitch.tif      - Simple average stitch (no fancy blending) for reference
  summary.txt           - Statistics about tile disagreements
"""

import argparse
import os
import sys

import numpy as np
import torch
import tifffile


def main():
    parser = argparse.ArgumentParser(description="Tile diagnostic for AI denoise")
    parser.add_argument("--input-tiff", required=True, help="Pre-demosaiced TIFF from RT")
    parser.add_argument("--outdir", required=True, help="Output directory for diagnostics")
    parser.add_argument("--iso-strength", type=float, default=50.0)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--model", type=str, default="Tree Net Denoise")
    parser.add_argument("--max-sample-tiles", type=int, default=20,
                        help="Max number of individual tile TIFFs to save (to limit disk use)")
    parser.add_argument("--stride", type=int, default=128,
                        help="Tile stride (default 128 = 50%% overlap with 256 tiles)")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    # Load input
    img = tifffile.imread(args.input_tiff).astype(np.float32)
    img = np.clip(img, 0.0, 1.0)
    image_chw = img.transpose(2, 0, 1)  # (3, H, W)
    H, W = image_chw.shape[1], image_chw.shape[2]
    img_size = 256
    stride = args.stride

    print(f"Image: {W}x{H}, tile_size={img_size}, stride={stride}")

    # Tile positions
    def tile_positions(length, tile_sz, step):
        pos = list(range(0, max(1, length - tile_sz + 1), step))
        if not pos:
            pos = [0]
        if pos[-1] + tile_sz < length:
            pos.append(length - tile_sz)
        return sorted(set(pos))

    y_pos = tile_positions(H, img_size, stride)
    x_pos = tile_positions(W, img_size, stride)
    n_tiles = len(y_pos) * len(x_pos)
    print(f"Tile grid: {len(y_pos)} rows x {len(x_pos)} cols = {n_tiles} tiles")

    # Load model
    from RawRefinery.application.ModelHandler import MODEL_REGISTRY
    conf = MODEL_REGISTRY[args.model]
    from pathlib import Path
    from platformdirs import user_data_dir
    data_dir = Path(user_data_dir("RawRefinery"))
    model_path = data_dir / conf["filename"]

    device = "cuda" if (args.gpu and torch.cuda.is_available()) else "cpu"
    print(f"Loading model on {device}...")
    model = torch.jit.load(str(model_path), map_location='cpu').eval().to(device)

    # ISO conditioning
    cond = torch.tensor([[args.iso_strength / 6400.0]], device=device, dtype=torch.float32)

    # Run inference tile by tile, storing all outputs
    tensor_input = torch.from_numpy(image_chw).unsqueeze(0).float()

    tile_outputs = {}  # (yi, xi) -> numpy (3, 256, 256)
    dtype_map = {'mps': torch.float16, 'cuda': torch.float16, 'cpu': torch.bfloat16}
    autocast_dtype = dtype_map.get(device, torch.float32)

    print("Running inference...")
    with torch.no_grad():
        with torch.autocast(device_type=device, dtype=autocast_dtype):
            for yi, y in enumerate(y_pos):
                for xi, x in enumerate(x_pos):
                    tile_in = tensor_input[:, :, y:y+img_size, x:x+img_size].to(device)
                    tile_out = model(tile_in, cond)

                    if "affine" in conf:
                        from RawRefinery.application.postprocessing import match_colors_linear
                        tile_out, _, _ = match_colors_linear(tile_out, tile_in)

                    tile_outputs[(yi, xi)] = tile_out[0].cpu().numpy()  # (3, 256, 256)

                done_pct = 100 * (yi + 1) / len(y_pos)
                print(f"  Row {yi+1}/{len(y_pos)} done ({done_pct:.0f}%)")

    # === ANALYSIS ===
    summary_lines = []
    summary_lines.append(f"Image: {W}x{H}")
    summary_lines.append(f"Tile size: {img_size}, stride: {stride}")
    summary_lines.append(f"Grid: {len(y_pos)} x {len(x_pos)} = {n_tiles} tiles")
    summary_lines.append(f"Overlap: {img_size - stride} pixels")
    summary_lines.append("")

    # 1) Save sample tiles (input and output)
    saved = 0
    # Save a grid: corners, center, and some edges
    sample_indices = set()
    # Corners
    for yi in [0, len(y_pos)-1]:
        for xi in [0, len(x_pos)-1]:
            sample_indices.add((yi, xi))
    # Center
    sample_indices.add((len(y_pos)//2, len(x_pos)//2))
    # A few along the edges
    for yi in range(0, len(y_pos), max(1, len(y_pos)//4)):
        sample_indices.add((yi, len(x_pos)//2))
        sample_indices.add((yi, 0))
    for xi in range(0, len(x_pos), max(1, len(x_pos)//4)):
        sample_indices.add((len(y_pos)//2, xi))

    for (yi, xi) in sorted(sample_indices):
        if saved >= args.max_sample_tiles:
            break
        y, x = y_pos[yi], x_pos[xi]
        tile_out = tile_outputs[(yi, xi)]
        tile_in = image_chw[:, y:y+img_size, x:x+img_size]

        out_path = os.path.join(args.outdir, f"tile_{yi:03d}_{xi:03d}.tif")
        in_path = os.path.join(args.outdir, f"input_tile_{yi:03d}_{xi:03d}.tif")
        tifffile.imwrite(out_path, tile_out.transpose(1, 2, 0))
        tifffile.imwrite(in_path, tile_in.transpose(1, 2, 0))
        saved += 1

    summary_lines.append(f"Saved {saved} sample tile pairs")

    # 2) Overlap disagreement analysis
    # For every pair of adjacent tiles, look at their overlap region
    # and measure how much they disagree
    h_diffs = []  # horizontal neighbor disagreements
    v_diffs = []  # vertical neighbor disagreements
    overlap = img_size - stride

    # Full-image disagreement map: at each pixel, track max |diff| across
    # all tile pairs that cover it
    disagree_map = np.zeros((H, W), dtype=np.float32)
    # Also track per-position-in-tile statistics
    # This tells us if edges of tiles systematically differ from centers
    position_bias = np.zeros((img_size, img_size), dtype=np.float64)
    position_count = np.zeros((img_size, img_size), dtype=np.float64)

    # Compute per-tile mean to detect systematic position-dependent bias
    tile_mean_image = np.zeros((3, img_size, img_size), dtype=np.float64)

    for (yi, xi), tout in tile_outputs.items():
        tile_mean_image += tout.astype(np.float64)
    tile_mean_image /= n_tiles

    # Per-tile: compute (tile - input) to see if the model adds/subtracts
    # differently at edges vs center
    delta_mean = np.zeros((3, img_size, img_size), dtype=np.float64)
    for (yi, xi), tout in tile_outputs.items():
        y, x = y_pos[yi], x_pos[xi]
        tin = image_chw[:, y:y+img_size, x:x+img_size]
        delta_mean += (tout - tin).astype(np.float64)
    delta_mean /= n_tiles

    # Save delta_mean as a diagnostic: this is the average "what the model does"
    # at each position within a tile. If there's a systematic edge effect,
    # this will show it clearly.
    # Scale for visibility: map to [0, 1] with 0.5 = no change
    delta_vis = (delta_mean * 10 + 0.5).clip(0, 1).astype(np.float32)
    tifffile.imwrite(os.path.join(args.outdir, "delta_mean_10x.tif"),
                     delta_vis.transpose(1, 2, 0))

    # Also save raw delta_mean
    tifffile.imwrite(os.path.join(args.outdir, "delta_mean_raw.tif"),
                     delta_mean.astype(np.float32).transpose(1, 2, 0))

    summary_lines.append("")
    summary_lines.append("=== Position-dependent bias (delta_mean = avg(output - input)) ===")
    summary_lines.append(f"  Center 64x64: mean={delta_mean[:, 96:160, 96:160].mean():.6f}, "
                         f"std={delta_mean[:, 96:160, 96:160].std():.6f}")
    summary_lines.append(f"  Top edge (row 0-15): mean={delta_mean[:, :16, :].mean():.6f}")
    summary_lines.append(f"  Bottom edge (row 240-255): mean={delta_mean[:, 240:, :].mean():.6f}")
    summary_lines.append(f"  Left edge (col 0-15): mean={delta_mean[:, :, :16].mean():.6f}")
    summary_lines.append(f"  Right edge (col 240-255): mean={delta_mean[:, :, 240:].mean():.6f}")
    summary_lines.append(f"  Overall: mean={delta_mean.mean():.6f}, "
                         f"range=[{delta_mean.min():.6f}, {delta_mean.max():.6f}]")

    # Row/col profile of delta_mean (averaged across channels and other axis)
    row_profile = delta_mean.mean(axis=(0, 2))  # (256,) - avg delta per row
    col_profile = delta_mean.mean(axis=(0, 1))  # (256,) - avg delta per col

    summary_lines.append("")
    summary_lines.append("=== Row profile (avg delta_mean per row, first/last 16) ===")
    for i in list(range(16)) + list(range(240, 256)):
        summary_lines.append(f"  row {i:3d}: {row_profile[i]:+.6f}")

    summary_lines.append("")
    summary_lines.append("=== Col profile (avg delta_mean per col, first/last 16) ===")
    for i in list(range(16)) + list(range(240, 256)):
        summary_lines.append(f"  col {i:3d}: {col_profile[i]:+.6f}")

    # 3) Horizontal overlap disagreement
    h_saved = 0
    summary_lines.append("")
    summary_lines.append("=== Horizontal overlap disagreements ===")

    for yi in range(len(y_pos)):
        for xi in range(len(x_pos) - 1):
            left = tile_outputs[(yi, xi)]    # (3, 256, 256)
            right = tile_outputs[(yi, xi+1)]

            x_left = x_pos[xi]
            x_right = x_pos[xi + 1]
            ov = x_left + img_size - x_right  # overlap in pixels

            if ov <= 0:
                continue

            # Region in left tile: rightmost `ov` columns
            left_region = left[:, :, img_size - ov:]
            # Region in right tile: leftmost `ov` columns
            right_region = right[:, :, :ov]

            diff = left_region - right_region
            abs_diff = np.abs(diff)
            mean_diff = abs_diff.mean()
            max_diff = abs_diff.max()
            h_diffs.append((yi, xi, mean_diff, max_diff))

            # Update disagreement map
            y = y_pos[yi]
            for c in range(3):
                region_max = np.max(abs_diff, axis=0)  # (256, ov)
                disagree_map[y:y+img_size, x_right:x_right+ov] = np.maximum(
                    disagree_map[y:y+img_size, x_right:x_right+ov],
                    region_max)

            if h_saved < 10:
                # Save overlap diff (scaled 10x + 0.5 gray for visibility)
                diff_vis = (diff * 10 + 0.5).clip(0, 1).astype(np.float32)
                path = os.path.join(args.outdir, f"overlap_h_{yi:03d}_{xi:03d}.tif")
                tifffile.imwrite(path, diff_vis.transpose(1, 2, 0))
                h_saved += 1

    if h_diffs:
        mean_h = np.mean([d[2] for d in h_diffs])
        max_h = np.max([d[3] for d in h_diffs])
        summary_lines.append(f"  Avg mean diff: {mean_h:.6f}")
        summary_lines.append(f"  Max diff: {max_h:.6f}")
        summary_lines.append(f"  Worst 5:")
        for yi, xi, m, mx in sorted(h_diffs, key=lambda x: -x[3])[:5]:
            summary_lines.append(f"    tile ({yi},{xi})-({yi},{xi+1}): mean={m:.6f}, max={mx:.6f}")

    # 4) Vertical overlap disagreement
    v_saved = 0
    summary_lines.append("")
    summary_lines.append("=== Vertical overlap disagreements ===")

    for yi in range(len(y_pos) - 1):
        for xi in range(len(x_pos)):
            top = tile_outputs[(yi, xi)]
            bottom = tile_outputs[(yi+1, xi)]

            y_top = y_pos[yi]
            y_bottom = y_pos[yi + 1]
            ov = y_top + img_size - y_bottom

            if ov <= 0:
                continue

            top_region = top[:, img_size - ov:, :]
            bottom_region = bottom[:, :ov, :]

            diff = top_region - bottom_region
            abs_diff = np.abs(diff)
            mean_diff = abs_diff.mean()
            max_diff = abs_diff.max()
            v_diffs.append((yi, xi, mean_diff, max_diff))

            x = x_pos[xi]
            region_max = np.max(abs_diff, axis=0)
            disagree_map[y_bottom:y_bottom+ov, x:x+img_size] = np.maximum(
                disagree_map[y_bottom:y_bottom+ov, x:x+img_size],
                region_max)

            if v_saved < 10:
                diff_vis = (diff * 10 + 0.5).clip(0, 1).astype(np.float32)
                path = os.path.join(args.outdir, f"overlap_v_{yi:03d}_{xi:03d}.tif")
                tifffile.imwrite(path, diff_vis.transpose(1, 2, 0))
                v_saved += 1

    if v_diffs:
        mean_v = np.mean([d[2] for d in v_diffs])
        max_v = np.max([d[3] for d in v_diffs])
        summary_lines.append(f"  Avg mean diff: {mean_v:.6f}")
        summary_lines.append(f"  Max diff: {max_v:.6f}")
        summary_lines.append(f"  Worst 5:")
        for yi, xi, m, mx in sorted(v_diffs, key=lambda x: -x[3])[:5]:
            summary_lines.append(f"    tile ({yi},{xi})-({yi+1},{xi}): mean={m:.6f}, max={mx:.6f}")

    # 5) Save disagreement map
    # Normalize for visibility
    disagree_max = disagree_map.max()
    if disagree_max > 0:
        disagree_vis = (disagree_map / disagree_max * 255).astype(np.uint8)
    else:
        disagree_vis = disagree_map.astype(np.uint8)
    tifffile.imwrite(os.path.join(args.outdir, "disagreement_map.tif"), disagree_vis)
    tifffile.imwrite(os.path.join(args.outdir, "disagreement_map_raw.tif"), disagree_map)

    summary_lines.append("")
    summary_lines.append(f"Disagreement map: max={disagree_max:.6f}")

    # 6) Naive average stitch (for reference — no weighting at all)
    accum = np.zeros((3, H, W), dtype=np.float64)
    count = np.zeros((1, H, W), dtype=np.float64)
    for (yi, xi), tout in tile_outputs.items():
        y, x = y_pos[yi], x_pos[xi]
        accum[:, y:y+img_size, x:x+img_size] += tout.astype(np.float64)
        count[:, y:y+img_size, x:x+img_size] += 1.0
    naive = (accum / np.maximum(count, 1.0)).astype(np.float32)
    tifffile.imwrite(os.path.join(args.outdir, "naive_stitch.tif"),
                     naive.transpose(1, 2, 0))

    # 7) Center-crop stitch (only use center region of each tile, discard edges)
    # This is the gold-standard approach if the model has edge artifacts
    margin = overlap // 2 if overlap > 0 else 0
    if margin > 0:
        center_stitch = np.zeros((3, H, W), dtype=np.float32)
        center_count = np.zeros((1, H, W), dtype=np.float32)

        for (yi, xi), tout in tile_outputs.items():
            y, x = y_pos[yi], x_pos[xi]

            # Determine crop margins for this tile
            top_m = margin if yi > 0 else 0
            bot_m = margin if yi < len(y_pos) - 1 else 0
            left_m = margin if xi > 0 else 0
            right_m = margin if xi < len(x_pos) - 1 else 0

            # Crop region within tile
            t_top = top_m
            t_bot = img_size - bot_m
            t_left = left_m
            t_right = img_size - right_m

            # Corresponding position in full image
            iy = y + t_top
            ix = x + t_left

            center_stitch[:, iy:iy+(t_bot-t_top), ix:ix+(t_right-t_left)] += \
                tout[:, t_top:t_bot, t_left:t_right]
            center_count[:, iy:iy+(t_bot-t_top), ix:ix+(t_right-t_left)] += 1.0

        center_stitch = center_stitch / np.maximum(center_count, 1.0)
        tifffile.imwrite(os.path.join(args.outdir, "center_crop_stitch.tif"),
                         center_stitch.transpose(1, 2, 0))
        summary_lines.append(f"Center-crop stitch: margin={margin}px discarded per edge")

    # Write summary
    summary_text = "\n".join(summary_lines)
    print(summary_text)
    with open(os.path.join(args.outdir, "summary.txt"), "w") as f:
        f.write(summary_text)

    print(f"\nDiagnostics saved to {args.outdir}/")


if __name__ == "__main__":
    main()
