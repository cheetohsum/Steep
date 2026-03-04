#!/usr/bin/env python3
"""
CLI bridge between RawTherapee and RawRefinery.
Usage: python rawrefinery_cli.py --input <raw> --output <tiff> --iso-strength <0-100> [--gpu]
       python rawrefinery_cli.py --input-tiff <tiff> --output <tiff> --iso-strength <0-100> [--gpu]
"""
import argparse
import sys
import os
import numpy as np

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
    from RawRefinery.application.ModelHandler import MODEL_REGISTRY
    from RawRefinery.application.utils import can_use_gpu
    from blended_tiling import TilingModule
    from RawRefinery.application.postprocessing import match_colors_linear

    # Device selection
    if args.gpu and can_use_gpu():
        device = torch.device("cuda")
    elif args.gpu and torch.backends.mps.is_available():
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
        try:
            r = requests.get(conf["url"] + ".sig", stream=True)
            r.raise_for_status()
            sig_path = model_path.with_suffix(f"{model_path.suffix}.sig")
            with open(sig_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)
        except Exception:
            pass

    print(f"Loading model from {model_path}...", file=sys.stderr)
    model = torch.jit.load(str(model_path), map_location="cpu").eval().to(device)

    # Load image
    if args.input_tiff:
        # Pre-demosaiced TIFF from RawTherapee — 32-bit float, HWC, [0,1]
        import tifffile
        print(f"Loading pre-demosaiced TIFF: {args.input_tiff}", file=sys.stderr)
        image_np = tifffile.imread(args.input_tiff).astype(np.float32)
        # RT's saveTIFF already outputs [0,1] float data.  Highlights may
        # exceed 1.0 when OOG clamping is disabled — just clip them.
        image_np = np.clip(image_np, 0.0, 1.0)
        # HWC -> CHW
        image_RGB = image_np.transpose(2, 0, 1)
        print(f"Input image stats: shape={image_RGB.shape} min={image_RGB.min():.6f} max={image_RGB.max():.6f} mean={image_RGB.mean():.6f}", file=sys.stderr)

        # Exposure normalization: the model expects data roughly in [0,1] range.
        # Very dark/underexposed images have max << 1 which causes float16
        # precision issues and poor model output. Scale up so the 99.9th
        # percentile maps to ~0.9, then scale back after inference.
        p999 = np.percentile(image_RGB, 99.9)
        if p999 > 0 and p999 < 0.5:
            exposure_scale = 0.9 / p999
            image_RGB = image_RGB * exposure_scale
            print(f"Exposure normalization: p99.9={p999:.6f} scale={exposure_scale:.2f}", file=sys.stderr)
        else:
            exposure_scale = 1.0
    else:
        # Raw file — use RawHandler for demosaicing
        from RawHandler.RawHandler import RawHandler
        from colour_demosaicing import demosaicing_CFA_Bayer_Malvar2004
        print(f"Loading raw file: {args.input}", file=sys.stderr)
        colorspace = "lin_rec2020"
        rh = RawHandler(args.input, colorspace=colorspace)
        image_RGB = rh.as_rgb(
            dims=None,
            demosaicing_func=demosaicing_CFA_Bayer_Malvar2004,
            colorspace=colorspace,
            clip=True,
        )
        exposure_scale = 1.0  # RawHandler already normalizes properly

    # Conditioning: The model expects conditioning[0] = ISO value (divided by 6400 later).
    # RawRefinery passes the actual ISO directly (range 0-65534).
    # iso_strength (0-100) scales the EXIF ISO to control denoise intensity:
    #   0 = no denoising (conditioning=0), 100 = full denoising at EXIF ISO
    actual_iso = args.iso if args.iso > 0 else 1600
    iso_conditioning = args.iso_strength / 100.0 * actual_iso
    conditioning = [iso_conditioning, 0]
    print(f"Conditioning: exif_iso={actual_iso} strength={args.iso_strength}% -> model_iso={iso_conditioning} -> cond={iso_conditioning/6400:.4f}", file=sys.stderr)

    # Run tiled inference — match RawRefinery's tiling geometry exactly
    # RawRefinery uses RGGB (half-res) as the base, then doubles for RGB
    img_size = 128
    tile_overlap = 0.25

    tensor_RGB = torch.from_numpy(image_RGB).unsqueeze(0).contiguous()

    # Half-resolution base size (matches RGGB dimensions in original code)
    half_size = [image_RGB.shape[1] // 2, image_RGB.shape[2] // 2]
    tile_size = [img_size, img_size]
    overlap = [tile_overlap, tile_overlap]

    # RGB tiling at 2x the base (full resolution)
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
    torch.cuda.empty_cache()

    # CHW -> HWC
    denoised = stitched.transpose(1, 2, 0)
    print(f"Output image stats (pre-scale): shape={denoised.shape} min={denoised.min():.6f} max={denoised.max():.6f} mean={denoised.mean():.6f}", file=sys.stderr)

    # Reverse exposure normalization
    if exposure_scale != 1.0:
        denoised = denoised / exposure_scale
        print(f"Reversed exposure scale: /{exposure_scale:.2f} -> max={denoised.max():.6f} mean={denoised.mean():.6f}", file=sys.stderr)

    # Save as 32-bit float TIFF (matches RT's expected input format)
    print(f"Saving result to {args.output}...", file=sys.stderr)
    import tifffile
    denoised_clipped = np.clip(denoised, 0, 1).astype(np.float32)
    tifffile.imwrite(args.output, denoised_clipped)

    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
