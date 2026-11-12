#!/usr/bin/env python3
"""Rebuild rtdata/models/birefnet_subject.onnx (optionally an fp16 twin) from BiRefNet.

U^2-Net, the subject model Steep shipped first, works at 320x320 and was
trained on DUTS, so hair, fur and anything thin come back as a soft blob that
the edge refinement has to rescue. BiRefNet (Zheng et al., "Bilateral
Reference for High-Resolution Dichotomous Image Segmentation", CAAI AIR 2024)
is trained for exactly that: one foreground map at 1024x1024, Swin-L backbone,
the "general" weights covering people, animals and objects. MIT licence, code
and weights both, so it can ship inside a GPL-3.0 release.

What this script has to deal with:

* Deformable convolution. The decoder's ASPPDeformable blocks call
  torchvision's deform_conv2d. Upstream's ONNX notebook exports it through a
  third-party symbolic (deform_conv2d_onnx_exporter) or, at opset 19+, as the
  native DeformConv op -- and DirectML has no DeformConv kernel. Instead the
  op is rewritten here as plain bilinear gathers plus a matmul per kernel tap,
  which is the same arithmetic torchvision does (zero outside the image, the
  modulation mask multiplying each sample) and lowers to Floor, Gather,
  MatMul and friends that every execution provider has. check_deform() proves
  the rewrite against torchvision before anything is exported.

* The output. In eval mode BiRefNet returns a list of logit maps whose last
  entry is the final prediction; the deep-supervision maps only exist in
  training. Steep's engine expects a probability with the sigmoid inside the
  model (as U^2-Net had it), so the wrapper returns sigmoid(preds[-1]).

* fp16. Sampling positions are pixel coordinates up to ~256 plus a fractional
  offset; in half precision the fraction is gone above 128 and the gather
  indices above 2048 are not even integers. That arithmetic lives in a
  submodule named steep_fp32_positions and is kept fp32 by the converter;
  everything else halves. The published safetensors are themselves fp16,
  so halving the weights loses nothing; the fp32 file is twice the download
  only because they are widened on load.

  It does not pass, though, so it is opt-in (--fp16) and verify() reports
  it against the same 2e-2 bar. Measured 2026-10 against PyTorch on four
  inputs: mean error 5e-7..4e-4, but max error 1.3e-3..1.3e-1, the worst on
  soft hair edges -- about 1% of one portrait's pixels moved by more than
  0.01. The error comes from the Swin backbone: also keeping the decoder
  fp32 made it worse, and keeping LayerNorm/Softmax fp32 did not help. On the
  CPU provider it is also slower (~9.5 s against ~8 s); only a GPU provider
  stands to gain anything from it.

Usage:
    pip install torch torchvision onnx onnxruntime safetensors timm einops \\
                kornia huggingface_hub pillow numpy opencv-python
    python tools/export_birefnet_model.py [--fp16] [--images a.jpg b.jpg ...]

It clones the upstream repo and downloads model.safetensors (~424 MiB) from
huggingface.co/ZhengPeng7/BiRefNet into a working directory, then writes the
ONNX file into rtdata/models/. With --images it also compares ONNX against
PyTorch on real photos (they are only read, at 1024x1024).

The contract rtengine/aisubjectmodel.cc relies on:
    input  "input"   float32 [1, 3, 1024, 1024]  RGB in [0,1], ImageNet mean/std
    output "output"  float32 [1, 1, 1024, 1024]  PROBABILITY -- sigmoid inside
Both files keep float32 at the boundary; only the fp16 file's insides differ.
"""
import argparse
import os
import subprocess
import sys
import time
import urllib.request

REPO = "https://github.com/ZhengPeng7/BiRefNet.git"
WEIGHTS_URL = "https://huggingface.co/ZhengPeng7/BiRefNet/resolve/main/model.safetensors"
WEIGHTS_BYTES = 444473596

SIDE = 1024
MEAN = (0.485, 0.456, 0.406)
STD = (0.229, 0.224, 0.225)

POSITIONS_SCOPE = "steep_fp32_positions"


def fetch(workdir):
    repo = os.path.join(workdir, "birefnet")
    if not os.path.isdir(repo):
        subprocess.check_call(["git", "clone", "--depth", "1", REPO, repo])

    wdir = os.path.join(workdir, "birefnet-weights")
    os.makedirs(wdir, exist_ok=True)
    weights = os.path.join(wdir, "model.safetensors")
    if not (os.path.exists(weights) and os.path.getsize(weights) == WEIGHTS_BYTES):
        print("downloading model.safetensors (~424 MiB)")
        urllib.request.urlretrieve(WEIGHTS_URL, weights)
    return repo, weights


# --------------------------------------------------------------------------
# deform_conv2d as gathers


def make_positions_module():
    import torch
    import torch.nn as nn

    class SamplePositions(nn.Module):
        """Bilinear corner indices and weights for one deformable conv.

        Returns, per kernel tap k, a flat gather index [4, B*Ho*Wo] (int64)
        and the matching weights [4, B, Ho, Wo] with validity and the
        modulation mask already folded in. torchvision's bilinear_interpolate
        zeroes each corner that falls outside the image individually, which
        is what the validity term does.
        """

        def __init__(self, kh, kw, stride, padding, dilation):
            super().__init__()
            self.kh, self.kw = kh, kw
            self.stride, self.padding, self.dilation = stride, padding, dilation

        def forward(self, offset, mask, H, W):
            B, _, Ho, Wo = offset.shape
            sh, sw = self.stride
            ph, pw = self.padding
            dh, dw = self.dilation
            dt = offset.dtype
            ys = (torch.arange(Ho, dtype=dt) * sh - ph).view(1, Ho, 1)
            xs = (torch.arange(Wo, dtype=dt) * sw - pw).view(1, 1, Wo)
            base = (torch.arange(B, dtype=dt) * (H * W)).view(B, 1, 1)
            taps = []
            for k in range(self.kh * self.kw):
                i, j = divmod(k, self.kw)
                py = ys + i * dh + offset[:, 2 * k]
                px = xs + j * dw + offset[:, 2 * k + 1]
                y0 = torch.floor(py)
                x0 = torch.floor(px)
                ly = py - y0
                lx = px - x0
                idx, wts = [], []
                for cy, cx, w in ((y0, x0, (1 - ly) * (1 - lx)),
                                  (y0, x0 + 1, (1 - ly) * lx),
                                  (y0 + 1, x0, ly * (1 - lx)),
                                  (y0 + 1, x0 + 1, ly * lx)):
                    valid = (cy >= 0) & (cy <= H - 1) & (cx >= 0) & (cx <= W - 1)
                    flat = base + cy.clamp(0, H - 1) * W + cx.clamp(0, W - 1)
                    idx.append(flat.reshape(-1))
                    wts.append(w * valid.to(dt) * mask[:, k])
                taps.append((torch.stack(idx).to(torch.int64), torch.stack(wts)))
            return taps

    return SamplePositions


def gather_deform_forward(self, x):
    """Drop-in for BiRefNet's DeformableConv2d.forward."""
    import torch
    B, C, H, W = x.shape
    offset = self.offset_conv(x)
    modulator = 2. * torch.sigmoid(self.modulator_conv(x))
    weight = self.regular_conv.weight
    cout = weight.shape[0]
    Ho, Wo = offset.shape[-2:]

    flat = x.permute(0, 2, 3, 1).reshape(B * H * W, C)
    out = None
    for k, (idx, wts) in enumerate(getattr(self, POSITIONS_SCOPE)(offset, modulator, H, W)):
        i, j = divmod(k, self.regular_conv.kernel_size[1])
        samples = flat[idx.reshape(-1)].view(4, B, Ho, Wo, C)
        cols = (samples * wts.unsqueeze(-1)).sum(0).reshape(B * Ho * Wo, C)
        tap = cols @ weight[:, :, i, j].t()
        out = tap if out is None else out + tap
    out = out.view(B, Ho, Wo, cout).permute(0, 3, 1, 2)
    if self.regular_conv.bias is not None:
        out = out + self.regular_conv.bias.view(1, -1, 1, 1)
    return out


def install_gather_deform(model):
    from models.modules.deform_conv import DeformableConv2d
    SamplePositions = make_positions_module()
    n = 0
    for m in model.modules():
        if isinstance(m, DeformableConv2d):
            kh, kw = m.regular_conv.kernel_size
            pad = m.padding if isinstance(m.padding, tuple) else (m.padding, m.padding)
            setattr(m, POSITIONS_SCOPE, SamplePositions(kh, kw, m.stride, pad, (1, 1)))
            m.forward = gather_deform_forward.__get__(m)
            n += 1
    return n


def check_deform(repo):
    """The rewrite against torchvision, including taps that leave the image."""
    import torch
    sys.path.insert(0, repo)
    from models.modules.deform_conv import DeformableConv2d
    torch.manual_seed(0)
    worst = 0.0
    for k in (1, 3, 7):
        ref = DeformableConv2d(16, 24, k, 1, k // 2, bias=True).eval()
        for conv in (ref.offset_conv, ref.modulator_conv):
            torch.nn.init.normal_(conv.weight, std=0.5)
            torch.nn.init.normal_(conv.bias, std=3.0)
        x = torch.randn(1, 16, 19, 23)
        with torch.no_grad():
            want = ref(x)
            install_gather_deform(ref)
            got = ref(x)
        worst = max(worst, float((want - got).abs().max()))
    print("deform rewrite vs torchvision, max abs diff: %.3e" % worst)
    assert worst < 1e-4, "gather deform_conv2d does not match torchvision"


# --------------------------------------------------------------------------
# model


def build_model(repo, weights):
    import torch
    import torch.nn as nn
    from safetensors.torch import load_file
    sys.path.insert(0, repo)
    from models.birefnet import BiRefNet
    from utils import check_state_dict

    net = BiRefNet(bb_pretrained=False)
    net.load_state_dict(check_state_dict(load_file(weights)), strict=True)
    net.eval()

    class SteepSubject(nn.Module):
        def __init__(self, net):
            super().__init__()
            self.net = net

        def forward(self, x):
            return self.net(x)[-1].sigmoid()

    return SteepSubject(net).eval()


def load_image(path):
    import numpy as np
    from PIL import Image
    img = Image.open(path).convert("RGB").resize((SIDE, SIDE), Image.BILINEAR)
    a = np.asarray(img, dtype=np.float32) / 255.0
    a = (a - np.array(MEAN, np.float32)) / np.array(STD, np.float32)
    return a.transpose(2, 0, 1)[None].copy()


def synthetic_image():
    """A bright disc on a gradient: something with an obvious subject."""
    import numpy as np
    yy, xx = np.mgrid[0:SIDE, 0:SIDE].astype(np.float32) / SIDE
    bg = np.stack([0.3 + 0.3 * yy, 0.4 + 0.2 * xx, 0.5 - 0.2 * yy])
    disc = ((xx - 0.5) ** 2 + (yy - 0.55) ** 2 < 0.06).astype(np.float32)
    rgb = bg * (1 - disc) + np.array([0.85, 0.35, 0.2], np.float32)[:, None, None] * disc
    return ((rgb - np.array(MEAN, np.float32)[:, None, None])
            / np.array(STD, np.float32)[:, None, None])[None].astype(np.float32)


def export_fp16(src, dst, keep_fp32=lambda node: False):
    import onnx
    from onnxruntime.transformers.float16 import convert_float_to_float16
    m = onnx.load(src)
    keep = [n.name for n in m.graph.node if POSITIONS_SCOPE in n.name or keep_fp32(n)]
    m16 = convert_float_to_float16(m, keep_io_types=True, node_block_list=keep)

    # Where one fp16 tensor feeds several kept-fp32 nodes, the converter adds
    # an identical Cast for each consumer, same name and same output, which
    # onnxruntime rejects as an invalid graph. Keep the first of each.
    seen, nodes = set(), []
    for n in m16.graph.node:
        key = (n.name, n.op_type, tuple(n.input), tuple(n.output))
        if n.name and key in seen:
            continue
        seen.add(key)
        nodes.append(n)
    dropped = len(m16.graph.node) - len(nodes)

    # It also routes every edge between two kept nodes through a
    # Cast(fp16) -> Cast(fp32) round trip, which throws away exactly the
    # precision the block list was for (4095 comes back as 4096 and the
    # gather walks off the end). Wire each such edge straight through.
    keep_set = set(keep)
    producer = {o: n for n in nodes for o in n.output}

    def cast_to(n):
        return next(a.i for a in n.attribute if a.name == "to")

    bypass = {}
    for n in nodes:
        if n.op_type != "Cast" or cast_to(n) != onnx.TensorProto.FLOAT:
            continue
        up = producer.get(n.input[0])
        if up is None or up.op_type != "Cast" or cast_to(up) != onnx.TensorProto.FLOAT16:
            continue
        src = producer.get(up.input[0])
        if src is not None and src.name in keep_set:
            bypass[n.output[0]] = up.input[0]
    for n in nodes:
        for k, i in enumerate(n.input):
            if i in bypass:
                n.input[k] = bypass[i]
    # Drop whatever the rewiring left without a consumer.
    graph_outputs = {o.name for o in m16.graph.output}
    while True:
        used = {i for n in nodes for i in n.input} | graph_outputs
        alive = [n for n in nodes if any(o in used for o in n.output)]
        if len(alive) == len(nodes):
            break
        dropped += len(nodes) - len(alive)
        nodes = alive

    # The converter also appends its casts at the end of the node list.
    # onnxruntime re-sorts, but the format requires topological order, and
    # other tools (and the checker) hold the file to it.
    ready = {""} | {i.name for i in m16.graph.input} | {t.name for t in m16.graph.initializer}
    consumers = {}
    waiting = []
    for n in nodes:
        need = {i for i in n.input if i not in ready}
        waiting.append([n, need])
        for i in need:
            consumers.setdefault(i, []).append(waiting[-1])
    order = [w[0] for w in waiting if not w[1]]
    k = 0
    while k < len(order):
        for out in order[k].output:
            for w in consumers.pop(out, []):
                w[1].discard(out)
                if not w[1]:
                    order.append(w[0])
        k += 1
    assert len(order) == len(nodes), "fp16 graph has a cycle or a dangling input"
    del m16.graph.node[:]
    m16.graph.node.extend(order)
    onnx.checker.check_model(m16)

    onnx.save(m16, dst)
    print("wrote %s (%d KiB), %d position nodes kept fp32, %d round-trip edges "
          "bypassed, %d redundant casts dropped"
          % (dst, os.path.getsize(dst) // 1024, len(keep), len(bypass), dropped))


def op_report(path):
    import collections
    import onnx
    m = onnx.load(path, load_external_data=False)
    ops = collections.Counter(n.op_type for n in m.graph.node)
    print("op types in %s: %s" % (os.path.basename(path),
                                   ", ".join("%s x%d" % kv for kv in sorted(ops.items()))))
    assert "DeformConv" not in ops


def verify(paths, ref_model, inputs, threads):
    import numpy as np
    import torch
    import onnxruntime as ort

    with torch.no_grad():
        refs = [ref_model(torch.from_numpy(x)).numpy() for _, x in inputs]

    opts = ort.SessionOptions()
    if threads:
        opts.intra_op_num_threads = threads
    ok = True
    for path, tol in paths:
        sess = ort.InferenceSession(path, opts, providers=["CPUExecutionProvider"])
        i, o = sess.get_inputs()[0], sess.get_outputs()[0]
        print("%s: input %r %s %s, output %r %s %s" % (
            os.path.basename(path), i.name, i.type, i.shape, o.name, o.type, o.shape))
        for (label, x), want in zip(inputs, refs):
            t0 = time.perf_counter()
            got = sess.run(["output"], {"input": x})[0]
            ms = (time.perf_counter() - t0) * 1000
            diff = float(np.abs(got - want).max())
            mean = float(np.abs(got - want).mean())
            fg = float((want > 0.5).mean())
            print("  %-22s max abs diff %.2e, mean %.2e, range %.3f..%.3f, "
                  "subject %.0f%% of frame, %.0f ms CPU"
                  % (label, diff, mean, got.min(), got.max(), fg * 100, ms))
            # Sigmoid on the CPU provider can land one ulp above 1.0; the
            # engine clamps, so only a real excursion is a failure.
            if got.shape != (1, 1, SIDE, SIDE) or got.min() < -1e-6 or got.max() > 1 + 1e-6:
                print("  FAIL output is not a [1,1,%d,%d] probability" % (SIDE, SIDE))
                ok = False
            if diff > tol:
                print("  FAIL differs from PyTorch by more than %.0e" % tol)
                ok = False
    return ok


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default=os.path.join(here, "build-birefnet"))
    ap.add_argument("--out", default=os.path.join(here, "rtdata", "models", "birefnet_subject.onnx"))
    ap.add_argument("--fp16", action="store_true",
                    help="also write <out>_fp16.onnx (see the docstring: it misses the bar)")
    ap.add_argument("--images", nargs="*", default=[],
                    help="photos to compare ONNX against PyTorch on (read only)")
    ap.add_argument("--threads", type=int, default=0, help="onnxruntime intra-op threads")
    ap.add_argument("--reuse", action="store_true",
                    help="keep an existing --out file instead of re-exporting it")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    repo, weights = fetch(args.workdir)
    # config.py looks for a train.sh in the current and parent directory and
    # parses it if found; the repo root has one. Run from the workdir instead.
    os.chdir(args.workdir)

    check_deform(repo)

    import torch
    ref = build_model(repo, weights)          # torchvision deform_conv2d
    model = build_model(repo, weights)        # gather rewrite, for export
    print("rewrote %d deformable convolutions" % install_gather_deform(model))

    if not (args.reuse and os.path.exists(args.out)):
        dummy = torch.from_numpy(synthetic_image())
        with torch.no_grad():
            torch.onnx.export(
                model, dummy, args.out,
                input_names=["input"], output_names=["output"],
                opset_version=17, do_constant_folding=True)
        print("wrote %s (%d KiB)" % (args.out, os.path.getsize(args.out) // 1024))
    op_report(args.out)

    paths = [(args.out, 1e-3)]
    if args.fp16:
        out16 = os.path.splitext(args.out)[0] + "_fp16.onnx"
        export_fp16(args.out, out16)
        paths.append((out16, 2e-2))

    inputs = [("synthetic disc", synthetic_image())]
    inputs += [(os.path.basename(p), load_image(p)) for p in args.images]
    ok = verify(paths, ref, inputs, args.threads)
    print("\nall checks passed" if ok else "\nCHECKS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
