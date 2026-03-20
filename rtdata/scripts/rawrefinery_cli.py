#!/usr/bin/env python3
"""
CLI bridge between RawTherapee and RawRefinery denoise models.
Self-contained — only requires: torch, blended-tiling, tifffile, numpy, platformdirs, requests.
Does NOT require the RawRefinery package itself.

Usage: python rawrefinery_cli.py --input-tiff <tiff> --output <tiff> --iso-strength <0-100> [--gpu]
"""
import argparse
import sys
import os
import numpy as np

# ── Inlined from RawRefinery (avoids PySide6 / pidng / RawHandler deps) ──

MODEL_REGISTRY = {
    "Tree Net Denoise": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/ShadowWeightedL1.pt",
        "filename": "ShadowWeightedL1.pt",
    },
    "Tree Net Denoise Light": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/ShadowWeightedL1_light.pt",
        "filename": "ShadowWeightedL1_light.pt",
    },
    "Tree Net Denoise Super Light": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/ShadowWeightedL1_super_light.pt",
        "filename": "ShadowWeightedL1_super_light.pt",
    },
    "Tree Net Denoise Heavy": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/ShadowWeightedL1_24_deep_500.pt",
        "filename": "ShadowWeightedL1_24_deep_500.pt",
    },
    "Deblur": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/realblur_gamma_140.pt",
        "filename": "realblur_gamma_140.pt",
        "affine": True,
    },
    "DeepSharpen": {
        "url": "https://github.com/rymuelle/RawRefinery/releases/download/v1.2.1-alpha/Deblur_deep_24.pt",
        "filename": "Deblur_deep_24.pt",
        "affine": True,
    },
}


def can_use_gpu():
    """Check if CUDA GPU is usable."""
    import torch
    if not torch.cuda.is_available():
        return False
    try:
        torch.zeros(1, device="cuda")
        return True
    except Exception:
        return False


def match_colors_linear(src, tgt, sample_fraction=0.05):
    """Per-channel affine color matching: tgt ~ scale * src + bias."""
    import torch
    B, C, H, W = src.shape
    device = src.device

    src_flat = src.view(B, C, -1)
    tgt_flat = tgt.view(B, C, -1)

    N = src_flat.shape[-1]
    k = max(64, int(N * sample_fraction))
    idx = torch.randint(0, N, (k,), device=device)

    src_s = src_flat[..., idx]
    tgt_s = tgt_flat[..., idx]

    src_mean = src_s.mean(-1, keepdim=True)
    tgt_mean = tgt_s.mean(-1, keepdim=True)
    src_centered = src_s - src_mean
    tgt_centered = tgt_s - tgt_mean

    var_src = (src_centered ** 2).mean(-1)
    cov = (src_centered * tgt_centered).mean(-1)

    scale = cov / (var_src + 1e-8)
    bias = tgt_mean.squeeze(-1) - scale * src_mean.squeeze(-1)

    scale_ = scale.view(B, C, 1, 1)
    bias_ = bias.view(B, C, 1, 1)
    transformed = src * scale_ + bias_

    return transformed, scale, bias


# ── Main ──

def main():
    parser = argparse.ArgumentParser(description="RawRefinery CLI for RawTherapee")
    parser.add_argument("--input", required=False, help="Path to raw file")
    parser.add_argument("--input-tiff", required=False, help="Path to pre-demosaiced TIFF from RT")
    parser.add_argument("--output", required=True, help="Path for output TIFF")
    parser.add_argument("--iso-strength", type=float, default=50, help="ISO conditioning (0-100)")
    parser.add_argument("--iso", type=float, default=0, help="Actual ISO from EXIF (0=auto)")
    parser.add_argument("--gpu", action="store_true", help="Use GPU acceleration")
    parser.add_argument("--model", default="Tree Net Denoise", help="Model name from registry")
    args = parser.parse_args()

    if not args.input and not args.input_tiff:
        print("Error: either --input or --input-tiff is required", file=sys.stderr)
        sys.exit(1)

    import torch
    from blended_tiling import TilingModule

    # Device selection
    if args.gpu and can_use_gpu():
        device = torch.device("cuda")
    elif args.gpu and hasattr(torch.backends, 'mps') and torch.backends.mps.is_available():
        device = torch.device("mps")
    else:
        device = torch.device("cpu")
    print(f"Using device: {device}", file=sys.stderr)

    # Load model
    from pathlib import Path
    from platformdirs import user_data_dir

    model_key = args.model
    if model_key not in MODEL_REGISTRY:
        print(f"Error: Unknown model '{model_key}'", file=sys.stderr)
        sys.exit(1)

    conf = MODEL_REGISTRY[model_key]
    data_dir = Path(user_data_dir("RawRefinery"))
    model_path = data_dir / conf["filename"]

    if not model_path.is_file():
        print(f"Downloading model {model_key}...", file=sys.stderr)
        import requests
        model_path.parent.mkdir(parents=True, exist_ok=True)
        r = requests.get(conf["url"], stream=True)
        r.raise_for_status()
        with open(model_path, "wb") as f:
            for chunk in r.iter_content(chunk_size=8192):
                f.write(chunk)

    print(f"Loading model from {model_path}...", file=sys.stderr)
    model = torch.jit.load(str(model_path), map_location="cpu").eval().to(device)

    # Load image
    if args.input_tiff:
        # Pre-demosaiced TIFF from RawTherapee — 32-bit float, HWC, [0,1]
        import tifffile
        print(f"Loading pre-demosaiced TIFF: {args.input_tiff}", file=sys.stderr)
        image_np = tifffile.imread(args.input_tiff).astype(np.float32)
        image_np = np.clip(image_np, 0.0, 1.0)
        # HWC -> CHW
        image_RGB = image_np.transpose(2, 0, 1)
        print(f"Input image stats: shape={image_RGB.shape} min={image_RGB.min():.6f} max={image_RGB.max():.6f} mean={image_RGB.mean():.6f}", file=sys.stderr)

        # Exposure normalization for dark images
        p999 = np.percentile(image_RGB, 99.9)
        if p999 > 0 and p999 < 0.5:
            exposure_scale = 0.9 / p999
            image_RGB = image_RGB * exposure_scale
            print(f"Exposure normalization: p99.9={p999:.6f} scale={exposure_scale:.2f}", file=sys.stderr)
        else:
            exposure_scale = 1.0
    elif args.input:
        # Raw file — try RawHandler if available, otherwise error
        try:
            from RawHandler.RawHandler import RawHandler
            from colour_demosaicing import demosaicing_CFA_Bayer_Malvar2004
        except ImportError:
            print("Error: --input requires RawHandler and colour_demosaicing packages.\n"
                  "Use --input-tiff instead (RawTherapee provides pre-demosaiced TIFF).",
                  file=sys.stderr)
            sys.exit(1)
        print(f"Loading raw file: {args.input}", file=sys.stderr)
        colorspace = "lin_rec2020"
        rh = RawHandler(args.input, colorspace=colorspace)
        image_RGB = rh.as_rgb(
            dims=None,
            demosaicing_func=demosaicing_CFA_Bayer_Malvar2004,
            colorspace=colorspace,
            clip=True,
        )
        exposure_scale = 1.0

    # Conditioning
    actual_iso = args.iso if args.iso > 0 else 1600
    iso_conditioning = args.iso_strength / 100.0 * actual_iso
    conditioning = [iso_conditioning, 0]
    print(f"Conditioning: exif_iso={actual_iso} strength={args.iso_strength}% -> model_iso={iso_conditioning} -> cond={iso_conditioning/6400:.4f}", file=sys.stderr)

    # Run tiled inference
    img_size = 128
    tile_overlap = 0.25

    tensor_RGB = torch.from_numpy(image_RGB).unsqueeze(0).contiguous()

    half_size = [image_RGB.shape[1] // 2, image_RGB.shape[2] // 2]
    tile_size = [img_size, img_size]
    overlap = [tile_overlap, tile_overlap]

    tiling_module = TilingModule(
        tile_size=[s * 2 for s in tile_size],
        tile_overlap=overlap,
        base_size=[s * 2 for s in half_size],
    )
    tiling_module_rebuild = TilingModule(
        tile_size=[s * 2 for s in tile_size],
        tile_overlap=overlap,
        base_size=[s * 2 for s in half_size],
    )

    tiles_rgb = tiling_module.split_into_tiles(tensor_RGB).float().to(device)

    batch_size = 2
    batches_rgb = torch.split(tiles_rgb, batch_size)

    cond_tensor = torch.as_tensor(conditioning, device=device).float().unsqueeze(0)
    cond_tensor[:, 0] /= 6400
    cond_tensor[:, 1] = 0
    cond_tensor = cond_tensor[:, 0:1]

    dtype_map = {"mps": torch.float16, "cuda": torch.float16, "cpu": torch.bfloat16}
    autocast_dtype = dtype_map.get(device.type, torch.float32)

    processed_batches = []
    total_batches = len(batches_rgb)

    print(f"Running inference ({total_batches} batches)...", file=sys.stderr)
    with torch.no_grad():
        with torch.autocast(device_type=device.type, dtype=autocast_dtype):
            for i, batch_rgb in enumerate(batches_rgb):
                B = batch_rgb.shape[0]
                curr_cond = cond_tensor.expand(B, -1)
                output = model(batch_rgb, curr_cond)
                if "affine" in conf:
                    output, _, _ = match_colors_linear(output, batch_rgb)
                processed_batches.append(output.cpu())
                pct = (i + 1) / total_batches * 100
                print(f"\rProgress: {pct:.0f}%", end="", file=sys.stderr)

    print("", file=sys.stderr)

    tiles_out = torch.cat(processed_batches, dim=0)
    stitched = tiling_module_rebuild.rebuild_with_masks(tiles_out).detach().cpu().numpy()[0]
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    # CHW -> HWC
    denoised = stitched.transpose(1, 2, 0)
    print(f"Output image stats (pre-scale): shape={denoised.shape} min={denoised.min():.6f} max={denoised.max():.6f} mean={denoised.mean():.6f}", file=sys.stderr)

    # Reverse exposure normalization
    if exposure_scale != 1.0:
        denoised = denoised / exposure_scale
        print(f"Reversed exposure scale: /{exposure_scale:.2f} -> max={denoised.max():.6f} mean={denoised.mean():.6f}", file=sys.stderr)

    # Save as 32-bit float TIFF
    import tifffile
    print(f"Saving result to {args.output}...", file=sys.stderr)
    denoised_clipped = np.clip(denoised, 0, 1).astype(np.float32)
    tifffile.imwrite(args.output, denoised_clipped)

    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
