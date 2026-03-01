#!/usr/bin/env python3
"""
RawRefinery CLI wrapper for RawTherapee AI Denoise integration.

Supports all raw formats including Fuji X-Trans (via rawpy fallback).

Usage:
    python rawrefinery_cli.py --input raw.RAF --output denoised.tif \
        --iso-strength 50 [--gpu] [--model "Tree Net Denoise"]

Exit codes:
    0 - Success
    1 - General error
    2 - RawRefinery not installed
    3 - Input file not found
    5 - Model not found / download needed
"""

import argparse
import os
import sys


def report_progress(percentage):
    """Write progress percentage to stderr."""
    sys.stderr.write(f"PROGRESS:{percentage:.0f}\n")
    sys.stderr.flush()


def load_raw_rawpy(path):
    """
    Fallback raw loader using rawpy/libraw.
    Returns linear ProPhoto RGB, shape (C, H, W), float32 [0,1].
    Works with all sensors including Fuji X-Trans.
    Uses LibRaw's built-in camera color profiles for correct conversion.
    """
    import rawpy
    import numpy as np

    raw = rawpy.imread(path)

    # Get ISO from raw metadata
    iso = 100
    try:
        iso = raw.metadata.iso
    except Exception:
        pass

    # Demosaic to linear ProPhoto RGB using LibRaw's tested camera profiles
    rgb = raw.postprocess(
        output_color=rawpy.ColorSpace.ProPhoto,
        gamma=(1, 1),           # linear
        no_auto_bright=True,
        output_bps=16,
        use_camera_wb=True,
        half_size=False,
    )

    # Compute crop offset: rawpy outputs the full visible area but RT
    # trims to the crop area. Crop margins are relative to raw buffer.
    s = raw.sizes
    crop_top = s.crop_top_margin - s.top_margin
    crop_left = s.crop_left_margin - s.left_margin
    crop_w = s.crop_width
    crop_h = s.crop_height

    # Convert to float [0,1] and CHW for model
    rgb_float = rgb.astype(np.float32) / 65535.0
    image_RGB = np.clip(rgb_float, 0.0, 1.0).transpose(2, 0, 1)  # (3, H, W)

    raw.close()
    return image_RGB, iso, (crop_top, crop_left, crop_h, crop_w)


def load_raw_rawhandler(path):
    """
    Primary raw loader using RawRefinery's RawHandler.
    Returns linear RGB rec2020, shape (3, H, W), float32 [0,1].
    Only works with Bayer sensors.
    """
    import numpy as np
    from RawHandler.RawHandler import RawHandler
    from colour_demosaicing import demosaicing_CFA_Bayer_Malvar2004

    colorspace = 'lin_rec2020'
    rh = RawHandler(path, colorspace=colorspace)

    # Get ISO
    iso = 100
    if 'EXIF ISOSpeedRatings' in rh.full_metadata:
        iso = int(rh.full_metadata['EXIF ISOSpeedRatings'].values[0])

    image_RGB = rh.as_rgb(
        dims=None,
        demosaicing_func=demosaicing_CFA_Bayer_Malvar2004,
        colorspace='lin_rec2020',
        clip=True,
    )

    return image_RGB, iso


def main():
    parser = argparse.ArgumentParser(
        description="RawRefinery CLI - AI denoising for raw camera images"
    )
    parser.add_argument("--input", "-i", required=False, help="Input raw file path")
    parser.add_argument("--input-tiff", type=str, default=None,
                        help="Pre-demosaiced TIFF from RT (overrides --input)")
    parser.add_argument("--output", "-o", required=True, help="Output float32 TIFF path")
    parser.add_argument("--iso-strength", type=float, default=50.0,
                        help="ISO conditioning (0-6400, default: 50)")
    parser.add_argument("--gpu", action="store_true", help="Use GPU for inference")
    parser.add_argument("--model", type=str, default="Tree Net Denoise",
                        help="Model name (default: 'Tree Net Denoise')")

    args = parser.parse_args()

    use_tiff_input = args.input_tiff is not None

    # Validate input
    if use_tiff_input:
        if not os.path.isfile(args.input_tiff):
            sys.stderr.write(f"Error: Input TIFF not found: {args.input_tiff}\n")
            sys.exit(3)
    else:
        if not args.input:
            sys.stderr.write("Error: Either --input or --input-tiff is required\n")
            sys.exit(1)
        if not os.path.isfile(args.input):
            sys.stderr.write(f"Error: Input file not found: {args.input}\n")
            sys.exit(3)

    try:
        from RawRefinery.application.ModelHandler import MODEL_REGISTRY
    except ImportError:
        sys.stderr.write("Error: RawRefinery not installed. pip install rawrefinery\n")
        sys.exit(2)

    import torch
    import numpy as np

    report_progress(0)

    # Device selection
    if args.gpu and torch.cuda.is_available():
        device = "cuda"
    else:
        if args.gpu:
            sys.stderr.write("Warning: GPU not available, using CPU.\n")
        device = "cpu"

    report_progress(5)

    try:
        if use_tiff_input:
            # Load pre-demosaiced TIFF from RT (same color space as RT's pipeline)
            import tifffile
            sys.stderr.write(f"Loading RT TIFF: {args.input_tiff}...\n")
            img = tifffile.imread(args.input_tiff)  # (H, W, 3) float32

            # RT's saveTIFF with isFloat=true normalizes to [0, 1] range already
            img = np.clip(img.astype(np.float32), 0.0, None)

            # HWC -> CHW for model
            image_RGB = img.transpose(2, 0, 1)
            sys.stderr.write(f"[v5-bandpass] Loaded TIFF: {image_RGB.shape[1]}x{image_RGB.shape[2]}, "
                             f"range=[{image_RGB.min():.4f}, {image_RGB.max():.4f}]\n")

            # Save original with highlight values (> 1.0) for later restoration
            original_full = image_RGB.copy()  # (3, H, W), may have values > 1.0
            has_highlights = np.any(image_RGB > 1.0)

            # Clip to [0, 1] for the model — it was trained on this range
            image_RGB = np.clip(image_RGB, 0.0, 1.0)

            sys.stderr.write(f"Highlight preservation: has_highlights={has_highlights}, "
                             f"original_max={original_full.max():.4f}, "
                             f"pct_above_0.95={100.0 * np.mean(original_full > 0.95):.2f}%, "
                             f"pct_above_1.0={100.0 * np.mean(original_full > 1.0):.2f}%\n")
        else:
            # Load raw file — try RawHandler first, fall back to rawpy
            crop_info = None
            sys.stderr.write(f"Loading {args.input}...\n")
            try:
                image_RGB, iso = load_raw_rawhandler(args.input)
                sys.stderr.write(f"Loaded via RawHandler, ISO={iso}\n")
            except Exception as e:
                sys.stderr.write(f"RawHandler failed ({e}), trying rawpy fallback...\n")
                image_RGB, iso, crop_info = load_raw_rawpy(args.input)
                sys.stderr.write(f"Loaded via rawpy, ISO={iso}, crop={crop_info}\n")

            # Ensure image_RGB is (3, H, W) float32
            if image_RGB.ndim == 3 and image_RGB.shape[2] == 3:
                image_RGB = image_RGB.transpose(2, 0, 1)
            image_RGB = np.clip(image_RGB.astype(np.float32), 0.0, 1.0)

        report_progress(15)

        # Load model
        if args.model not in MODEL_REGISTRY:
            sys.stderr.write(f"Error: Model '{args.model}' not found. "
                             f"Available: {list(MODEL_REGISTRY.keys())}\n")
            sys.exit(5)

        conf = MODEL_REGISTRY[args.model]
        from pathlib import Path
        from platformdirs import user_data_dir

        data_dir = Path(user_data_dir("RawRefinery"))
        model_path = data_dir / conf["filename"]

        if not model_path.is_file():
            import requests
            url = conf.get("url", "").strip()
            if url:
                sys.stderr.write(f"Downloading model: {conf['filename']}...\n")
                data_dir.mkdir(parents=True, exist_ok=True)
                r = requests.get(url, stream=True)
                r.raise_for_status()
                with open(model_path, 'wb') as f:
                    for chunk in r.iter_content(chunk_size=8192):
                        f.write(chunk)
                # Try signature too
                try:
                    r = requests.get(url + '.sig', stream=True)
                    r.raise_for_status()
                    with open(str(model_path) + '.sig', 'wb') as f:
                        for chunk in r.iter_content(chunk_size=8192):
                            f.write(chunk)
                except Exception:
                    pass
                sys.stderr.write("Model downloaded.\n")
            else:
                sys.stderr.write(f"Error: Model not found at {model_path}\n")
                sys.exit(5)

        report_progress(20)

        torch_device = torch.device(device)
        sys.stderr.write(f"Loading model on {device}...\n")
        loaded = torch.jit.load(str(model_path), map_location='cpu')
        model = loaded.eval().to(torch_device)

        report_progress(30)
        sys.stderr.write("Running inference...\n")

        # Tiling and inference
        H, W = image_RGB.shape[1], image_RGB.shape[2]
        img_size = 256
        tile_overlap = 0.5
        batch_size = 2
        stride = int(img_size * (1 - tile_overlap))  # 128

        tensor_RGB = torch.from_numpy(image_RGB).unsqueeze(0).contiguous()

        if use_tiff_input:
            # 75% overlap: 4x more tiles but adjacent tiles share much more
            # context, making predictions consistent and eliminating tile grid
            # artifacts that no stitching algorithm could hide at 50% overlap.
            stride = img_size // 4  # 64
            # Custom tile extraction for overlap-crop stitching
            def _tile_positions(length, tile_sz, step):
                pos = list(range(0, max(1, length - tile_sz + 1), step))
                if not pos:
                    pos = [0]
                if pos[-1] + tile_sz < length:
                    pos.append(length - tile_sz)
                return sorted(set(pos))

            y_pos = _tile_positions(H, img_size, stride)
            x_pos = _tile_positions(W, img_size, stride)
            sys.stderr.write(f"[v12-bias] tiles: {len(y_pos)}x{len(x_pos)} = {len(y_pos)*len(x_pos)}, stride={stride}\n")

            tiles_list = []
            for y in y_pos:
                for x in x_pos:
                    tiles_list.append(tensor_RGB[:, :, y:y+img_size, x:x+img_size])
            tiles_rgb = torch.cat(tiles_list, dim=0).float().to(torch_device)
        else:
            from blended_tiling import TilingModule
            tile_size_rgb = [img_size, img_size]
            overlap_param = [tile_overlap, tile_overlap]
            full_size_rgb = [H, W]
            tiling_module_rgb = TilingModule(
                tile_size=tile_size_rgb, tile_overlap=overlap_param,
                base_size=full_size_rgb)
            tiling_module_rebuild = TilingModule(
                tile_size=tile_size_rgb, tile_overlap=overlap_param,
                base_size=full_size_rgb)
            tiles_rgb = tiling_module_rgb.split_into_tiles(tensor_RGB).float().to(torch_device)

        batches_rgb = torch.split(tiles_rgb, batch_size)

        # ISO conditioning
        conditioning = [args.iso_strength, 0.0]
        cond_tensor = torch.as_tensor(conditioning, device=torch_device).float().unsqueeze(0)
        cond_tensor[:, 0] /= 6400
        cond_tensor = cond_tensor[:, 0:1]

        processed_batches = []
        dtype_map = {'mps': torch.float16, 'cuda': torch.float16, 'cpu': torch.bfloat16}
        autocast_dtype = dtype_map.get(torch_device.type, torch.float32)
        total_batches = len(batches_rgb)

        with torch.no_grad():
            with torch.autocast(device_type=torch_device.type, dtype=autocast_dtype):
                for i, batch_rgb in enumerate(batches_rgb):
                    B = batch_rgb.shape[0]
                    curr_cond = cond_tensor.expand(B, -1)
                    output = model(batch_rgb, curr_cond)

                    if "affine" in conf:
                        from RawRefinery.application.postprocessing import match_colors_linear
                        output, _, _ = match_colors_linear(output, batch_rgb)
                    processed_batches.append(output.cpu())

                    pct = 30 + int(60 * (i + 1) / total_batches)
                    report_progress(pct)

        tiles_out = torch.cat(processed_batches, dim=0)

        if use_tiff_input:
            # Bias-corrected cosine blend (v12).
            # The model's convolution padding creates a systematic position-
            # dependent brightness bias in every tile (edges differ from center).
            # This bias is the SAME for all tiles, so averaging more tiles
            # reinforces it rather than cancelling it.
            # Fix: estimate the bias by averaging (tile − reference) across all
            # tiles, then subtract it before the final blend.
            tiles_np = tiles_out.detach().cpu().numpy()
            blend_half = stride // 2  # half the boundary spacing

            def _tile_weight_1d(tile_start, tile_sz, positions, idx):
                """1D weight: 1.0 in owned interior, sin²/cos² taper at boundaries."""
                centers = [p + tile_sz // 2 for p in positions]
                y = np.arange(tile_sz, dtype=np.float32) + tile_start
                w = np.ones(tile_sz, dtype=np.float32)

                if idx > 0:
                    bnd = (centers[idx - 1] + centers[idx]) / 2.0
                    in_ramp = (y >= bnd - blend_half) & (y <= bnd + blend_half)
                    below = y < bnd - blend_half
                    t = np.clip((y - bnd + blend_half) / (2.0 * blend_half), 0, 1)
                    ramp = np.sin(np.pi / 2 * t) ** 2
                    w = np.where(below, 0.0, np.where(in_ramp, ramp, w))

                if idx < len(positions) - 1:
                    bnd = (centers[idx] + centers[idx + 1]) / 2.0
                    in_ramp = (y >= bnd - blend_half) & (y <= bnd + blend_half)
                    above = y > bnd + blend_half
                    t = np.clip((y - bnd + blend_half) / (2.0 * blend_half), 0, 1)
                    ramp = np.cos(np.pi / 2 * t) ** 2
                    w = np.where(above, 0.0, np.where(in_ramp, ramp, w))

                return w

            # --- Pass 1: initial cosine blend → reference ---
            numerator = np.zeros((3, H, W), dtype=np.float32)
            denominator = np.zeros((1, H, W), dtype=np.float32)

            # Pre-compute all 1D weights (reused in pass 2)
            wy_all = [_tile_weight_1d(y, img_size, y_pos, yi) for yi, y in enumerate(y_pos)]
            wx_all = [_tile_weight_1d(x, img_size, x_pos, xi) for xi, x in enumerate(x_pos)]

            idx = 0
            for yi, y in enumerate(y_pos):
                for xi, x in enumerate(x_pos):
                    w2d = (wy_all[yi][:, None] * wx_all[xi][None, :])[None, :, :]
                    numerator[:, y:y+img_size, x:x+img_size] += tiles_np[idx] * w2d
                    denominator[:, y:y+img_size, x:x+img_size] += w2d
                    idx += 1
            reference = numerator / np.maximum(denominator, 1e-6)

            # --- Estimate position-dependent bias ---
            # Average (tile − reference_crop) over all tiles.
            # Content-dependent diffs cancel; systematic bias remains.
            bias_sum = np.zeros((3, img_size, img_size), dtype=np.float64)
            n_tiles = len(y_pos) * len(x_pos)
            idx = 0
            for yi, y in enumerate(y_pos):
                for xi, x in enumerate(x_pos):
                    ref_crop = reference[:, y:y+img_size, x:x+img_size]
                    bias_sum += (tiles_np[idx] - ref_crop).astype(np.float64)
                    idx += 1
            bias_map = (bias_sum / n_tiles).astype(np.float32)
            sys.stderr.write(f"[v12-bias] bias range=[{bias_map.min():.6f}, {bias_map.max():.6f}]\n")

            # --- Pass 2: blend bias-corrected tiles ---
            numerator2 = np.zeros((3, H, W), dtype=np.float32)
            idx = 0
            for yi, y in enumerate(y_pos):
                for xi, x in enumerate(x_pos):
                    w2d = (wy_all[yi][:, None] * wx_all[xi][None, :])[None, :, :]
                    corrected = tiles_np[idx] - bias_map
                    numerator2[:, y:y+img_size, x:x+img_size] += corrected * w2d
                    idx += 1
            stitched = numerator2 / np.maximum(denominator, 1e-6)

            sys.stderr.write(f"[v12-bias] stitched {H}x{W}, range=[{stitched.min():.4f}, {stitched.max():.4f}]\n")
            torch.cuda.empty_cache()
            report_progress(92)

            denoised_clipped = np.clip(stitched, 0.0, 1.0).astype(np.float32)

            if has_highlights:
                t = np.clip((original_full - 1.0) / 0.05, 0.0, 1.0)
                result_chw = (1.0 - t) * denoised_clipped + t * original_full
            else:
                result_chw = denoised_clipped

            result = result_chw.transpose(1, 2, 0).astype(np.float32)
        else:
            # Legacy raw-input path: stitch with blended_tiling, then ratio map
            stitched = tiling_module_rebuild.rebuild_with_masks(tiles_out).detach().cpu().numpy()[0]
            torch.cuda.empty_cache()
            report_progress(92)

            denoised = stitched.transpose(1, 2, 0)  # CHW -> HWC
            original_hwc = image_RGB.transpose(1, 2, 0)
            eps = 1e-3

            # Per-channel mean match
            denoised_corrected = denoised.copy()
            for c in range(3):
                orig_mean = np.mean(original_hwc[:, :, c])
                dn_mean = np.mean(denoised[:, :, c])
                if dn_mean > eps:
                    denoised_corrected[:, :, c] *= (orig_mean / dn_mean)

            ratio = denoised_corrected / (original_hwc + eps)
            ratio = np.clip(ratio, 0.3, 3.0)

            mean_ratio = np.mean(ratio, axis=2, keepdims=True)
            max_dev = np.max(np.abs(ratio - mean_ratio), axis=2, keepdims=True)
            fade = np.clip((max_dev - 0.15) / 0.15, 0.0, 1.0)
            ratio = (1.0 - fade) * ratio + fade * np.broadcast_to(mean_ratio, ratio.shape)

            if crop_info is not None:
                ct, cl, ch, cw = crop_info
                if ct > 0 or cl > 0:
                    sys.stderr.write(f"Cropping ratio: offset ({ct},{cl}) size {cw}x{ch}\n")
                    ratio = ratio[ct:ct+ch, cl:cl+cw, :]

            result = ratio.astype(np.float32)

        import tifffile
        tifffile.imwrite(args.output, result, photometric="rgb", compression=None, rowsperstrip=1)

        report_progress(100)
        sys.stderr.write(f"Success: Denoised image saved to {args.output}\n")

    except Exception as e:
        import traceback
        traceback.print_exc()
        sys.stderr.write(f"Error during processing: {e}\n")
        sys.exit(1)


if __name__ == "__main__":
    main()
