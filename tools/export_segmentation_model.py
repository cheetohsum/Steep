#!/usr/bin/env python3
"""Rebuild Steep's ADE20K scene-parsing models from the CSAIL weights.

Steep's smart masks used to run NVIDIA's SegFormer-B0, whose licence forbids
commercial use and so cannot be shipped inside a GPL-3.0 release. This script
produces the replacements, both from MIT CSAIL and released under BSD-3-Clause,
both trained on the same ADE20K 150 classes:

    --model mobilenetv2  (default)  rtdata/models/ade20k_mobilenetv2_c1.onnx
        MobileNetV2dilated + C1_deepsup, ~8 MB, output 1/8 of the input,
        34.8 mIoU on ADE20K.
    --model hrnetv2                 rtdata/models/ade20k_hrnetv2_c1.onnx
        HRNetV2-W48 + C1, ~260 MB, output 1/4 of the input, 43.2 mIoU.
        Heavier and slower, but twice the resolution and far fewer
        confusions between neighbouring classes.

Because every model here is ADE20K-150 with the same channel order, nothing in
rtengine/aisegmentation.cc has to change -- adeToAIClass() maps channel indices
straight through. The checks below exist to prove that, since an off-by-one
would quietly mask trees when the user asked for sky.

Usage:
    pip install torch onnx onnxruntime pillow numpy yacs
    python tools/export_segmentation_model.py [--model hrnetv2]

It clones the upstream repo and downloads the weights (~9 MB for MobileNetV2,
~267 MB for HRNetV2) into a working directory, then writes the ONNX file into
rtdata/models/.

The contract rtengine/aisegmentation.cc expects, identical for both models:
    input  "input"   float32 [1, 3, H, W]  RGB, ImageNet-normalised, dynamic
    output "output"  float32 [1, 150, h, w]  RAW LOGITS -- it softmaxes itself
h and w are not a fixed fraction of H and W the engine can assume; read them
from the output tensor. (They are ceil(H/stride), ceil(W/stride).)
"""
import argparse
import csv
import os
import subprocess
import sys
import time
import urllib.request

REPO = "https://github.com/CSAILVision/semantic-segmentation-pytorch.git"
WEIGHTS_ROOT = "http://sceneparsing.csail.mit.edu/model/pytorch"

MODELS = {
    "mobilenetv2": {
        "weights_dir": "ade20k-mobilenetv2dilated-c1_deepsup",
        "epoch": 20,
        "encoder": "mobilenetv2dilated",
        "decoder": "c1_deepsup",
        "fc_dim": 320,
        "out": "ade20k_mobilenetv2_c1.onnx",
        "stride": 8,
    },
    "hrnetv2": {
        "weights_dir": "ade20k-hrnetv2-c1",
        "epoch": 30,
        "encoder": "hrnetv2",
        "decoder": "c1",
        "fc_dim": 720,
        "out": "ade20k_hrnetv2_c1.onnx",
        "stride": 4,
    },
}

# What adeToAIClass() in rtengine/aisegmentation.cc keys off. If a future model
# reorders these, the mapping there has to change with it.
EXPECTED_CHANNELS = {0: "wall", 2: "sky", 4: "tree", 12: "person"}


def weight_names(spec):
    return ["encoder_epoch_%d.pth" % spec["epoch"],
            "decoder_epoch_%d.pth" % spec["epoch"]]


def fetch(workdir, name, spec):
    repo = os.path.join(workdir, "csail")
    if not os.path.isdir(repo):
        subprocess.check_call(["git", "clone", "--depth", "1", REPO, repo])

    # One directory per model: both releases name their files encoder_epoch_N,
    # and the MobileNetV2 checkout predates this script taking a --model.
    ckpt = os.path.join(workdir, "ckpt") if name == "mobilenetv2" \
        else os.path.join(workdir, "ckpt", name)
    os.makedirs(ckpt, exist_ok=True)
    for fname in weight_names(spec):
        dest = os.path.join(ckpt, fname)
        if not os.path.exists(dest):
            print("downloading", fname)
            urllib.request.urlretrieve(
                "%s/%s/%s" % (WEIGHTS_ROOT, spec["weights_dir"], fname), dest)
    return repo, ckpt


def build_model(repo, ckpt, spec):
    import torch
    import torch.nn as nn
    sys.path.insert(0, repo)
    from mit_semseg.models import ModelBuilder

    class SteepSegmenter(nn.Module):
        """Encoder plus the C1 head, returning raw logits.

        Neither head's forward can be exported as-is: C1DeepSup returns a
        (main, deepsup) tuple of log_softmax maps with use_softmax=False, C1
        returns log_softmax, and with use_softmax=True both interpolate to a
        fixed size and softmax. Steep wants none of that, so drive the two
        layers that matter directly -- C1 and C1_deepsup share them.
        """

        def __init__(self, encoder, decoder):
            super().__init__()
            self.encoder = encoder
            self.cbr = decoder.cbr
            self.conv_last = decoder.conv_last

        def forward(self, x):
            return self.conv_last(self.cbr(self.encoder(x, return_feature_maps=True)[-1]))

    enc_path, dec_path = (os.path.join(ckpt, f) for f in weight_names(spec))
    encoder = ModelBuilder.build_encoder(
        arch=spec["encoder"], fc_dim=spec["fc_dim"], weights=enc_path)
    decoder = ModelBuilder.build_decoder(
        arch=spec["decoder"], fc_dim=spec["fc_dim"], num_class=150,
        weights=dec_path, use_softmax=True)

    # ModelBuilder loads with strict=False, so a renamed layer would leave
    # random init in place and still "work". Reload strictly to rule that out.
    # The decoder checkpoint may carry deep-supervision layers C1 lacks.
    encoder.load_state_dict(torch.load(enc_path, map_location="cpu"), strict=True)
    dec_state = torch.load(dec_path, map_location="cpu")
    missing, _ = decoder.load_state_dict(dec_state, strict=False)
    assert not missing, "decoder weights missing for %s" % missing
    return SteepSegmenter(encoder, decoder).eval()


def verify(onnx_path, model, repo, spec):
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

    stride = spec["stride"]
    for size in [(256, 384), (512, 512), (450, 675), (600, 800), (600, 900)]:
        d = np.random.randn(1, 3, *size).astype(np.float32)
        t0 = time.perf_counter()
        out = sess.run(["output"], {"input": d})[0]
        ms = (time.perf_counter() - t0) * 1000
        assert out.shape[1] == 150, out.shape
        want = tuple(-(-s // stride) for s in size)
        assert out.shape[2:] == want, (out.shape, want)
        print("dynamic shape %-10s -> %s  (1/%d, %.0f ms CPU)"
              % (str(size), out.shape, stride, ms))

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
    ap.add_argument("--model", choices=sorted(MODELS), default="mobilenetv2")
    ap.add_argument("--workdir", default=os.path.join(here, "build-segmodel"))
    ap.add_argument("--out", default=None,
                    help="default: rtdata/models/<the model's file name>")
    args = ap.parse_args()

    spec = MODELS[args.model]
    out = args.out or os.path.join(here, "rtdata", "models", spec["out"])

    os.makedirs(args.workdir, exist_ok=True)
    repo, ckpt = fetch(args.workdir, args.model, spec)

    import torch
    model = build_model(repo, ckpt, spec)

    dummy = torch.randn(1, 3, 512, 512)
    torch.onnx.export(
        model, dummy, out,
        input_names=["input"], output_names=["output"],
        dynamic_axes={"input": {0: "batch", 2: "height", 3: "width"},
                      "output": {0: "batch", 2: "out_height", 3: "out_width"}},
        opset_version=17, do_constant_folding=True)
    print("wrote %s (%d KiB)" % (out, os.path.getsize(out) // 1024))

    verify(out, model, repo, spec)


if __name__ == "__main__":
    main()
