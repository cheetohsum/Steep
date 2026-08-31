#!/usr/bin/env python3
"""Rebuild rtdata/models/ade20k_mobilenetv2_c1.onnx from the CSAIL weights.

Steep's smart masks used to run NVIDIA's SegFormer-B0, whose licence forbids
commercial use and so cannot be shipped inside a GPL-3.0 release. This script
produces the replacement: MIT CSAIL's MobileNetV2dilated + C1_deepsup, trained
on the same ADE20K 150 classes and released under BSD-3-Clause.

Because both models are ADE20K-150 with the same channel order, nothing in
rtengine/aisegmentation.cc had to change -- adeToAIClass() maps channel indices
straight through. The checks below exist to prove that, since an off-by-one
would quietly mask trees when the user asked for sky.

Usage:
    pip install torch onnx onnxruntime pillow numpy yacs
    python tools/export_segmentation_model.py

It clones the upstream repo and downloads ~9 MB of weights into a working
directory, then writes the ONNX file into rtdata/models/.

The contract rtengine/aisegmentation.cc expects:
    input  "input"   float32 [1, 3, H, W]  RGB, ImageNet-normalised, dynamic
    output "output"  float32 [1, 150, h, w]  RAW LOGITS -- it softmaxes itself
"""
import argparse
import csv
import os
import subprocess
import sys
import urllib.request

REPO = "https://github.com/CSAILVision/semantic-segmentation-pytorch.git"
WEIGHTS_BASE = ("http://sceneparsing.csail.mit.edu/model/pytorch"
                "/ade20k-mobilenetv2dilated-c1_deepsup")
WEIGHTS = ["encoder_epoch_20.pth", "decoder_epoch_20.pth"]

# What adeToAIClass() in rtengine/aisegmentation.cc keys off. If a future model
# reorders these, the mapping there has to change with it.
EXPECTED_CHANNELS = {0: "wall", 2: "sky", 4: "tree", 12: "person"}


def fetch(workdir):
    repo = os.path.join(workdir, "csail")
    if not os.path.isdir(repo):
        subprocess.check_call(["git", "clone", "--depth", "1", REPO, repo])

    ckpt = os.path.join(workdir, "ckpt")
    os.makedirs(ckpt, exist_ok=True)
    for name in WEIGHTS:
        dest = os.path.join(ckpt, name)
        if not os.path.exists(dest):
            print("downloading", name)
            urllib.request.urlretrieve(WEIGHTS_BASE + "/" + name, dest)
    return repo, ckpt


def build_model(repo, ckpt):
    import torch.nn as nn
    sys.path.insert(0, repo)
    from mit_semseg.models import ModelBuilder

    class SteepSegmenter(nn.Module):
        """Encoder plus the C1 head, returning raw logits.

        C1DeepSup.forward cannot be exported as-is: with use_softmax=False it
        returns a (main, deepsup) tuple of log_softmax maps, and with True it
        interpolates to a fixed size and softmaxes. Steep wants neither, so
        drive the two layers that matter directly.
        """

        def __init__(self, encoder, decoder):
            super().__init__()
            self.encoder = encoder
            self.cbr = decoder.cbr
            self.conv_last = decoder.conv_last

        def forward(self, x):
            return self.conv_last(self.cbr(self.encoder(x, return_feature_maps=True)[-1]))

    encoder = ModelBuilder.build_encoder(
        arch="mobilenetv2dilated", fc_dim=320,
        weights=os.path.join(ckpt, "encoder_epoch_20.pth"))
    decoder = ModelBuilder.build_decoder(
        arch="c1_deepsup", fc_dim=320, num_class=150,
        weights=os.path.join(ckpt, "decoder_epoch_20.pth"), use_softmax=True)
    return SteepSegmenter(encoder, decoder).eval()


def verify(onnx_path, model, repo):
    import numpy as np
    import torch
    import onnxruntime as ort

    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])

    x = np.random.randn(1, 3, 512, 768).astype(np.float32)
    onnx_out = sess.run(["output"], {"input": x})[0]
    with torch.no_grad():
        torch_out = model(torch.from_numpy(x)).numpy()
    diff = float(np.abs(onnx_out - torch_out).max())
    print("onnx vs torch, max abs diff: %.3e" % diff)
    assert diff < 1e-3, "the exported graph does not match the source model"

    for size in [(256, 384), (512, 512), (600, 800)]:
        d = np.random.randn(1, 3, *size).astype(np.float32)
        out = sess.run(["output"], {"input": d})[0]
        assert out.shape[1] == 150, out.shape
        print("dynamic shape %-10s -> %s" % (str(size), out.shape))

    # object150_info.csv is 1-indexed for humans; network channel 0 is its
    # row 1. Steep indexes channels directly, so channel c must be row c+1.
    names = {}
    with open(os.path.join(repo, "data", "object150_info.csv")) as f:
        for row in csv.reader(f):
            if row[0].isdigit():
                names[int(row[0])] = row[-1]

    bad = []
    for channel, want in EXPECTED_CHANNELS.items():
        got = names.get(channel + 1, "")
        status = "OK" if want in got else "MISMATCH"
        if status == "MISMATCH":
            bad.append((channel, want, got))
        print("channel %3d -> %-30s expected %-7s %s" % (channel, got, want, status))
    if bad:
        raise SystemExit(
            "channel order does not match adeToAIClass() in "
            "rtengine/aisegmentation.cc -- fix the mapping before shipping this")
    print("\nall checks passed")


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default=os.path.join(here, "build-segmodel"))
    ap.add_argument("--out", default=os.path.join(
        here, "rtdata", "models", "ade20k_mobilenetv2_c1.onnx"))
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    repo, ckpt = fetch(args.workdir)

    import torch
    model = build_model(repo, ckpt)

    dummy = torch.randn(1, 3, 512, 512)
    torch.onnx.export(
        model, dummy, args.out,
        input_names=["input"], output_names=["output"],
        dynamic_axes={"input": {0: "batch", 2: "height", 3: "width"},
                      "output": {0: "batch", 2: "out_height", 3: "out_width"}},
        opset_version=17, do_constant_folding=True)
    print("wrote %s (%d KiB)" % (args.out, os.path.getsize(args.out) // 1024))

    verify(args.out, model, repo)


if __name__ == "__main__":
    main()
