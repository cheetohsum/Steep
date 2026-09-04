#!/usr/bin/env python3
"""Fetch rtdata/models/u2net_subject.onnx and check what it promises.

The ADE20K scene parser Steep uses for named classes (sky, vegetation,
buildings) is a 150-class semantic segmentation model, and it is poor at the
thing people ask a mask for most often: cut out the subject. It has one
generic "animal" class, rare in its training set, and its output is eight
times downsampled, so fur and hair are hopeless.

U^2-Net is trained for exactly that job -- salient object detection, one
foreground/background map, fine structure kept. Apache-2.0, so it can ship
inside a GPL-3.0 release.

The upstream release is already ONNX, so there is nothing to convert; this
downloads it and then proves the contract rtengine/aisubject*.cc relies on,
because an input laid out the wrong way round fails silently and looks like a
bad model rather than a bad assumption.

Usage:
    pip install numpy onnxruntime
    python tools/export_subject_model.py
"""
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "rtdata", "models", "u2net_subject.onnx")

# The rembg project's mirror of the released U^2-Net weights, already exported.
URL = "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2net.onnx"
EXPECT_BYTES = 175997641


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    if os.path.exists(OUT) and os.path.getsize(OUT) == EXPECT_BYTES:
        print("already present:", OUT)
    else:
        print(f"downloading {URL} (~168 MiB) ...")
        urllib.request.urlretrieve(URL, OUT)
        print("wrote", OUT, os.path.getsize(OUT), "bytes")

    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print("numpy/onnxruntime not installed; skipped the self-test")
        return

    sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])

    for i in sess.get_inputs():
        print(f"input  {i.name!r} {i.type} {i.shape}")

    for o in sess.get_outputs():
        print(f"output {o.name!r} {o.type} {o.shape}")

    name = sess.get_inputs()[0].name
    outs = sess.run(None, {name: np.random.rand(1, 3, 320, 320).astype(np.float32)})
    print(f"{len(outs)} output tensor(s); first is {outs[0].shape}, "
          f"range {outs[0].min():.4f}..{outs[0].max():.4f}")

    ok = True

    if outs[0].shape != (1, 1, 320, 320):
        print("FAIL first output is not a single-channel map at input size")
        ok = False

    if not (0.0 <= outs[0].min() and outs[0].max() <= 1.0):
        print("FAIL first output is not already a probability -- the engine "
              "assumes the sigmoid is inside the model")
        ok = False

    if outs[0].max() - outs[0].min() < 1e-4:
        print("FAIL first output is constant")
        ok = False

    print("self-test OK" if ok else "SELF-TEST FAILED")
    sys.exit(0 if ok else 1)


main()
