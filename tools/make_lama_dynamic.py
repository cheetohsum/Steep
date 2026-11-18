#!/usr/bin/env python3
"""Let a fixed-size LaMa ONNX export run at any input size.

The common LaMa exports (e.g. Carve/LaMa-ONNX lama_fp32.onnx) declare a
512x512 input, but the network itself is fully convolutional and its graph
derives every internal shape from the input. Only the declared input/output
dimensions stop ONNX Runtime from accepting other sizes. This rewrites those
declarations as free dimensions and drops the stale intermediate shape
annotations; the weights are untouched.

With the result in place of models/lama_inpainting.onnx, smart repairs
(version 3) run the model at up to 1024 px instead of squeezing every region
into 512. A fixed-size model keeps working as before.

Sides must be multiples of 16 at run time (the engine takes care of that).

Usage:
    python tools/make_lama_dynamic.py <in.onnx> <out.onnx> [--check]
"""
import argparse
import sys

import onnx


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source')
    parser.add_argument('target')
    parser.add_argument('--check', action='store_true',
                        help='run the result at 512 and 768x1024 with onnxruntime and verify a neutral stays neutral')
    args = parser.parse_args()

    model = onnx.load(args.source)
    changed = 0
    for tensor in list(model.graph.input) + list(model.graph.output):
        dims = tensor.type.tensor_type.shape.dim
        if len(dims) != 4:
            print(f'{tensor.name}: expected a 4-D tensor, found {len(dims)} dims', file=sys.stderr)
            return 1
        for axis, name in ((2, 'height'), (3, 'width')):
            if dims[axis].HasField('dim_value'):
                dims[axis].ClearField('dim_value')
                dims[axis].dim_param = name
                changed += 1
    del model.graph.value_info[:]
    onnx.save(model, args.target)
    print(f'{args.target}: {changed} fixed dimensions made free')

    if args.check:
        import numpy as np
        import onnxruntime as ort

        session = ort.InferenceSession(args.target, providers=['CPUExecutionProvider'])
        for height, width in ((512, 512), (768, 1024)):
            image = np.full((1, 3, height, width), .25, np.float32)
            mask = np.zeros((1, 1, height, width), np.float32)
            out = session.run(None, {'image': image, 'mask': mask})[0]
            mean = float(out.mean())
            ok = out.shape == image.shape and (abs(mean - .25) < .003 or abs(mean - 63.75) < .75)
            print(f'  {width}x{height}: output {out.shape}, mean {mean:.3f} -> {"ok" if ok else "FAILED"}')
            if not ok:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
