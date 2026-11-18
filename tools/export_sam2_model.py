#!/usr/bin/env python3
"""Export a SAM 2.1 image model to the two ONNX files Steep's click selection
loads (rtengine/samselector.cc):

  sam2_encoder.onnx   image [1, 3, 1024, 1024] (ImageNet-normalised sRGB)
                      -> high0 [1, 32, 256, 256], high1 [1, 64, 128, 128],
                         embedding [1, 256, 64, 64]
  sam2_decoder.onnx   high0, high1, embedding, points [1, n, 2] (pixels in the
                      1024 frame), labels [1, n] (1 include / 0 exclude)
                      -> masks [1, 3, 256, 256] (logits), scores [1, 3]

Every SAM 2.1 size shares this interface, so the files are interchangeable:
Tiny is what Steep was developed against; Large (facebook/sam2.1-hiera-large,
Apache-2.0) finds whole objects more reliably at about six times the encoder
cost.

Usage:
    python tools/export_sam2_model.py <out-dir> [--model facebook/sam2.1-hiera-large] [--check]

Needs torch, transformers (with Sam2Model) and onnx.
"""
import argparse
import os
import sys

import torch


class Encoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, image):
        high0, high1, embedding = self.model.get_image_embeddings(image)
        return high0, high1, embedding


class Decoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, high0, high1, embedding, points, labels):
        out = self.model(input_points=points[:, None], input_labels=labels[:, None].long(),
                         image_embeddings=[high0, high1, embedding], multimask_output=True)
        return out.pred_masks[:, 0], out.iou_scores[:, 0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out_dir')
    parser.add_argument('--model', default='facebook/sam2.1-hiera-large')
    parser.add_argument('--check', action='store_true', help='compare the ONNX files against torch on CPU')
    args = parser.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    from transformers import Sam2Model
    model = Sam2Model.from_pretrained(args.model).eval()
    encoder, decoder = Encoder(model), Decoder(model)
    image = torch.randn(1, 3, 1024, 1024)
    points = torch.tensor([[[400., 500.], [620., 300.]]])
    labels = torch.tensor([[1., 0.]])
    encoder_path = os.path.join(args.out_dir, 'sam2_encoder.onnx')
    decoder_path = os.path.join(args.out_dir, 'sam2_decoder.onnx')
    with torch.no_grad():
        features = encoder(image)
        torch.onnx.export(encoder, (image,), encoder_path, opset_version=17, dynamo=False,
                          input_names=['image'], output_names=['high0', 'high1', 'embedding'])
        masks, scores = decoder(*features, points, labels)
        torch.onnx.export(decoder, (*features, points, labels), decoder_path, opset_version=17, dynamo=False,
                          input_names=['high0', 'high1', 'embedding', 'points', 'labels'],
                          output_names=['masks', 'scores'],
                          dynamic_axes={'points': {1: 'point_count'}, 'labels': {1: 'point_count'}})

    if args.check:
        import numpy as np
        import onnxruntime as ort
        enc = ort.InferenceSession(encoder_path, providers=['CPUExecutionProvider'])
        dec = ort.InferenceSession(decoder_path, providers=['CPUExecutionProvider'])
        got = enc.run(None, {'image': image.numpy()})
        print('encoder max diff', [float(np.abs(g - f.numpy()).max()) for g, f in zip(got, features)])
        one = {'points': np.array([[[512., 512.]]], np.float32), 'labels': np.array([[1.]], np.float32)}
        with torch.no_grad():
            want_masks, want_scores = decoder(*features, torch.from_numpy(one['points']), torch.from_numpy(one['labels']))
        got_masks, got_scores = dec.run(None, dict(zip(['high0', 'high1', 'embedding'], got), **one))
        print('decoder (one point, traced with two) mask max diff %.4f, scores diff %.5f, shapes %s %s' % (
            float(np.abs(got_masks - want_masks.numpy()).max()), float(np.abs(got_scores - want_scores.numpy()).max()),
            got_masks.shape, got_scores.shape))
    for name in ('sam2_encoder.onnx', 'sam2_decoder.onnx'):
        print('  %-20s %7.1f MB' % (name, os.path.getsize(os.path.join(args.out_dir, name)) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
