#!/usr/bin/env python3
"""Export RORem (an SDXL-inpainting object remover, Apache-2.0) to ONNX for
Steep's high-quality Remove Object fill.

RORem is run with guidance 1.0 and one fixed quality prompt, so the text
encoders are not needed at run time: the prompt's embeddings are computed here
and baked into the UNet graph as constants. Three files are written:

  rorem_unet.onnx (+ rorem_unet.onnx.data)   half-precision weights, float32 in/out
      latent   [1, 9, h, w]  noisy latent (4) + mask (1) + masked-image latent (4),
                             already scaled for the current sigma
      timestep [1]
      time_ids [1, 6]        original h, w, crop top, left, target h, w
      -> noise [1, 4, h, w]
  rorem_vae_encoder.onnx   image [1, 3, H, W] in -1..1 -> latent [1, 4, H/8, W/8] (scaled)
  rorem_vae_decoder.onnx   latent (scaled) -> image in -1..1

H and W are free (multiples of 8). The sampler (Euler, scaled-linear betas
0.00085..0.012, 1000 training steps, leading spacing with offset 1, epsilon
prediction) lives in rtengine/airemover.cc.

Usage:
    python tools/export_rorem_model.py <out-dir> [--model LetsThink/RORem] [--check]

Needs torch (CUDA for the half-precision trace), diffusers, transformers, onnx.
"""
import argparse
import os
import sys

import torch

PROMPT = '4K, high quality, masterpiece, Highly detailed, Sharp focus, Professional, photorealistic, realistic'


class UNetWrapper(torch.nn.Module):
    def __init__(self, unet, prompt_embeds, pooled):
        super().__init__()
        self.unet = unet
        self.register_buffer('prompt_embeds', prompt_embeds)
        self.register_buffer('pooled', pooled)

    def forward(self, latent, timestep, time_ids):
        dtype = self.prompt_embeds.dtype
        out = self.unet(latent.to(dtype), timestep.to(dtype), encoder_hidden_states=self.prompt_embeds,
                        added_cond_kwargs={'text_embeds': self.pooled, 'time_ids': time_ids.to(dtype)},
                        return_dict=False)[0]
        return out.float()


class VaeEncoder(torch.nn.Module):
    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, image):
        return self.vae.encode(image).latent_dist.mean * self.vae.config.scaling_factor


class VaeDecoder(torch.nn.Module):
    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, latent):
        return self.vae.decode(latent / self.vae.config.scaling_factor, return_dict=False)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out_dir')
    parser.add_argument('--model', default='LetsThink/RORem')
    parser.add_argument('--check', action='store_true', help='compare the exported VAE against torch on CPU')
    args = parser.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    from diffusers import AutoPipelineForInpainting
    pipe = AutoPipelineForInpainting.from_pretrained(args.model, torch_dtype=torch.float16).to('cuda')
    with torch.no_grad():
        prompt_embeds, _, pooled, _ = pipe.encode_prompt(PROMPT, device='cuda', do_classifier_free_guidance=False)
    print('prompt embeddings', tuple(prompt_embeds.shape), tuple(pooled.shape))

    unet = UNetWrapper(pipe.unet, prompt_embeds, pooled).eval()
    latent = torch.randn(1, 9, 64, 80, device='cuda')
    timestep = torch.tensor([981.], device='cuda')
    time_ids = torch.tensor([[512., 640., 0., 0., 512., 640.]], device='cuda')
    unet_path = os.path.join(args.out_dir, 'rorem_unet.onnx')
    with torch.no_grad():
        reference = unet(latent, timestep, time_ids)
        torch.onnx.export(unet, (latent, timestep, time_ids), unet_path, opset_version=17, dynamo=False,
                          input_names=['latent', 'timestep', 'time_ids'], output_names=['noise'],
                          dynamic_axes={'latent': {2: 'h', 3: 'w'}, 'noise': {2: 'h', 3: 'w'}},
                          do_constant_folding=True)
    # One weight file beside the graph (the model is far past protobuf's 2 GB).
    import onnx
    model = onnx.load(unet_path)
    for stale in os.listdir(args.out_dir):
        full = os.path.join(args.out_dir, stale)
        if stale not in ('rorem_unet.onnx',) and not stale.startswith('rorem_') and os.path.isfile(full):
            os.remove(full)
    onnx.save(model, unet_path, save_as_external_data=True, all_tensors_to_one_file=True,
              location='rorem_unet.onnx.data', size_threshold=1024)
    print('unet exported; torch reference std %.4f' % reference.float().std().item())
    torch.save({'latent': latent.cpu(), 'timestep': timestep.cpu(), 'time_ids': time_ids.cpu(), 'noise': reference.cpu()},
               os.path.join(args.out_dir, 'unet_reference.pt'))

    vae = pipe.vae.float().eval()
    image = torch.rand(1, 3, 512, 640, device='cuda') * 2 - 1
    with torch.no_grad():
        encoded = VaeEncoder(vae)(image)
        torch.onnx.export(VaeEncoder(vae), (image,), os.path.join(args.out_dir, 'rorem_vae_encoder.onnx'),
                          opset_version=17, dynamo=False, input_names=['image'], output_names=['latent'],
                          dynamic_axes={'image': {2: 'h', 3: 'w'}, 'latent': {2: 'h', 3: 'w'}})
        decoded = VaeDecoder(vae)(encoded)
        torch.onnx.export(VaeDecoder(vae), (encoded,), os.path.join(args.out_dir, 'rorem_vae_decoder.onnx'),
                          opset_version=17, dynamo=False, input_names=['latent'], output_names=['image'],
                          dynamic_axes={'latent': {2: 'h', 3: 'w'}, 'image': {2: 'h', 3: 'w'}})
    print('vae exported')

    if args.check:
        import numpy as np
        import onnxruntime as ort
        enc = ort.InferenceSession(os.path.join(args.out_dir, 'rorem_vae_encoder.onnx'), providers=['CPUExecutionProvider'])
        dec = ort.InferenceSession(os.path.join(args.out_dir, 'rorem_vae_decoder.onnx'), providers=['CPUExecutionProvider'])
        test = (torch.rand(1, 3, 384, 448) * 2 - 1).numpy()
        with torch.no_grad():
            want_latent = VaeEncoder(vae)(torch.from_numpy(test).cuda())
            want_image = VaeDecoder(vae)(want_latent).cpu().numpy()
        got_latent = enc.run(None, {'image': test})[0]
        got_image = dec.run(None, {'latent': got_latent})[0]
        print('vae encoder max diff %.5f, decoder max diff %.5f' % (
            float(np.abs(got_latent - want_latent.cpu().numpy()).max()), float(np.abs(got_image - want_image).max())))
    for name in sorted(os.listdir(args.out_dir)):
        print('  %-28s %8.1f MB' % (name, os.path.getsize(os.path.join(args.out_dir, name)) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
