# Bundled AI models

Steep ships the models its AI tools need, so there is nothing for users to
download or configure. They are third-party work, and each is under a licence
that permits redistribution inside a GPL-3.0 release.

## In this directory

### `ade20k_mobilenetv2_c1.onnx` — 8 MB

Semantic segmentation, used by the smart mask tools to work out what is in a
photo. MobileNetV2dilated encoder with a C1_deepsup head, trained on ADE20K's
150 classes.

- Upstream: [CSAILVision/semantic-segmentation-pytorch](https://github.com/CSAILVision/semantic-segmentation-pytorch),
  MIT CSAIL Computer Vision
- Licence: BSD-3-Clause
- Rebuild it with `tools/export_segmentation_model.py`, which downloads the
  upstream weights, exports the ONNX, and re-runs the checks below

Steep previously used NVIDIA's SegFormer-B0 here. That model is published under
the NVIDIA Source Code License, which allows use "non-commercially, meaning for
research or evaluation purposes only" — a restriction GPL-3.0 does not permit a
distributed work to carry, so it could not be shipped. This model is trained on
the same dataset with the same class order, so `adeToAIClass()` in
`rtengine/aisegmentation.cc` needed no changes.

Verified at export time, because a silent mismatch here would mask the wrong
things rather than fail:

- the exported graph agrees with the source PyTorch model to 2.6e-05
- dynamic input sizes work, output is input/8 per side
- channel 0 is wall, 2 is sky, 4 is tree, 12 is person — the four
  `adeToAIClass()` keys off

One visible trade-off: SegFormer emitted masks at a quarter of the input
resolution and this emits an eighth, so raw masks are coarser before the edge
refinement step. It also scores a little lower on ADE20K (34.8 mIoU against
37.4). The upstream repo has heavier variants; HRNetV2, at 43.2, is now
exported too — see `ade20k_hrnetv2_c1.onnx` below.

Installed with `-DWITH_AI_MASKING=ON`, which is the default on Windows and is
set explicitly by all three CI workflows.

### `aidenoise/ShadowWeightedL1.onnx` — 19 MB

The denoising model behind the AI Denoise tool. It comes from
[RawRefinery](https://github.com/rymuelle/RawRefinery) by Ryan Mueller, an
open-source raw denoiser.

- Licence: MIT

Steep runs this model itself through ONNX Runtime — it does not call
RawRefinery, and Python is not involved. If you already have RawRefinery
installed, Steep will find and use its copy of the model instead of this one,
so the two do not duplicate a 19 MB file between them.

This model is installed unconditionally; the tool switches itself on whenever
ONNX Runtime is present.

## Not in this directory

### `ade20k_hrnetv2_c1.onnx` — 254 MiB

The heavier sibling of `ade20k_mobilenetv2_c1.onnx`: same upstream, same
ADE20K 150 classes in the same order, HRNetV2-W48 encoder with a plain C1 head
(epoch 30). It scores 43.2 mIoU against MobileNetV2's 34.8, and its output is a
quarter of the input per side rather than an eighth, so raw masks carry twice
the resolution into edge refinement.

- Upstream: [CSAILVision/semantic-segmentation-pytorch](https://github.com/CSAILVision/semantic-segmentation-pytorch),
  weights `ade20k-hrnetv2-c1/{encoder,decoder}_epoch_30.pth`
- Licence: BSD-3-Clause
- Rebuild it with `tools/export_segmentation_model.py --model hrnetv2`

It is a drop-in for the MobileNetV2 file: the tensors are named `input` and
`output`, the input is dynamic `[1, 3, H, W]` ImageNet-normalised RGB, and the
output is raw logits `[1, 150, ceil(H/4), ceil(W/4)]`. Read the output size
from the tensor rather than assuming a stride — MobileNetV2's is
`ceil(H/8)`, and both round up.

Verified at export time:

- the exported graph agrees with the source PyTorch model to 9.9e-06
- dynamic input sizes work: 450×675 gives 113×169, 600×900 gives 150×225
- channel 0 is wall, 2 is sky, 4 is tree, 12 is person
- the checkpoint loads strictly, since `ModelBuilder` alone would silently
  leave any renamed layer at random initialisation

The price is speed: about 0.85 s for a 600×900 frame on the CPU provider,
against 0.09 s for MobileNetV2, measured on a 16-thread desktop that was busy
with other work at the time.

Not in git: GitHub refuses files over 100 MiB.

### `birefnet_subject.onnx` — 901 MiB

Subject (salient object) segmentation: one foreground probability map, used
where a mask should "cut out the subject". It replaces U²-Net
(`u2net_subject.onnx`), which works at 320×320 and loses hair, fur and thin
structure; BiRefNet works at 1024×1024 with a Swin-L backbone and is built
for exactly those edges. These are the "general" weights — people, animals
and objects.

- Upstream: [ZhengPeng7/BiRefNet](https://github.com/ZhengPeng7/BiRefNet),
  Zheng et al., *Bilateral Reference for High-Resolution Dichotomous Image
  Segmentation*, CAAI AIR 2024; weights
  [ZhengPeng7/BiRefNet](https://huggingface.co/ZhengPeng7/BiRefNet),
  `model.safetensors`
- Licence: MIT, for the code and the published weights alike
- Rebuild it with `tools/export_birefnet_model.py`

The contract: `input` float32 `[1, 3, 1024, 1024]`, RGB in 0..1 normalised
with the ImageNet mean and standard deviation — the same preprocessing as
upstream's own inference code and as U²-Net — and `output` float32
`[1, 1, 1024, 1024]`, already a probability. The sigmoid is inside the graph,
and only the final map is returned, not the deep-supervision side outputs.
The size is fixed; the engine resizes to and from it.

BiRefNet's decoder uses deformable convolution. Upstream exports it with a
third-party symbolic or as ONNX's native `DeformConv`, which DirectML does not
implement. The export script instead rewrites it as bilinear gathers and a
matmul per kernel tap, the same arithmetic torchvision performs, so the graph
contains only ordinary opset-17 operators (Conv, MatMul, Gather, Floor,
LayerNormalization, Softmax, Resize, Erf, ScatterND and shape arithmetic) and
no `DeformConv`.

Verified at export time:

- the rewritten deformable convolution matches torchvision to 7.2e-07,
  including samples that fall outside the image
- the exported graph agrees with PyTorch to at most 2.9e-04 (mean under
  1e-06) on three photos and a synthetic frame
- the output is a probability, 0..1 (the CPU provider's sigmoid can land one
  ulp above 1.0; the engine clamps)

On the CPU provider one inference takes about 8 s and creating the session
about 12 s, on a 16-thread desktop that was busy at the time. This model wants
a GPU provider.

The published weights are themselves fp16, which is why this file is twice
their 424 MiB download. An fp16 export is half the size, but it misses the
bar: on soft hair edges it moves some pixels by up to 0.13, about 1% of one
portrait's pixels by more than 0.01. `--fp16` still writes one for anyone who
wants to judge that on a GPU; it is not shipped.

Not in git: GitHub refuses files over 100 MiB.

### `lama_inpainting.onnx` — ~200 MB

The inpainting model behind Remove Object. It is **not** stored in git, because
GitHub refuses any file over 100 MiB and this one is roughly twice that. Git LFS
could hold it, but every clone would then pay for it.

- Upstream: [LaMa](https://github.com/advimman/lama), Suvorov et al.,
  *Resolution-robust Large Mask Inpainting with Fourier Convolutions*, WACV 2022
- Licence: Apache 2.0

It is fetched from where it came from originally:

- Source: [Carve/LaMa-ONNX](https://huggingface.co/Carve/LaMa-ONNX), file
  `lama_fp32.onnx` — an ONNX export of upstream big-lama
- Licence: Apache 2.0
- sha256 `1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6`,
  which is byte-identical to the copy Steep was developed against

That URL and hash are the defaults in `CMakeLists.txt`, so nothing has to be
hosted and no configuration is needed. Override `AI_INPAINT_MODEL_URL` to point
elsewhere.

To bake the model into a build instead — worth it for an offline or otherwise
self-contained install, at the cost of the download size:

```bash
cmake .. -DWITH_AI_MASKING=ON -DAI_INPAINT_MODEL_BUNDLE=ON
```

That is off by default on purpose. A file already present here is never
re-fetched.

CI passes this URL from the `AI_INPAINT_MODEL_URL` repository variable. Setting
that variable once is all it takes.

Baking it in is not the only route, and usually not the one you want: at ~200 MB
it would take the AppImage from 146 MB to roughly 340 MB for a feature many
people never touch. So a build without the model still offers it, from two
places, and both land it in the same spot — `<settings>/models/`, which
`rtengine/init.cc` searches after the install tree:

- **In the app.** Smart Tools shows a Download button when the model is absent
  and the build knows a URL. It loads straight away afterwards, without a
  restart.
- **In the Windows installer.** An opt-in task on the tasks page, skipped
  automatically when the build already bundled the model.

The transfer is handed to the platform's curl rather than done in process. In
process would mean HTTPS, and the vendored httplib is built without TLS while
neither the AppImage nor the macOS bundle ships glib-networking — so it would
mean adding a TLS stack to three bundles and trusting it inside each. curl is
part of macOS, part of Windows since 10/1803, and on essentially every desktop
Linux; where it is genuinely missing the dialog says so and gives the manual
path. `--fail` is passed so an HTTP error page can never be saved as though it
were a model, and the file downloads to `.part` and is renamed only on success,
so a partial file cannot satisfy the existence check on the next start.

## A note on licences

Every model here is under a licence whose terms can be satisfied inside a
GPL-3.0 work: BSD-3-Clause, MIT or Apache 2.0. Each requires the original
copyright and attribution notices to survive, which is part of what this file
is for.

If you swap a model, check that direction of compatibility before shipping it.
The trap is not obscure: NVIDIA's SegFormer weights sat here for a while and
are non-commercial, which GPL-3.0 cannot carry. A permissive-sounding project
licence does not always cover the published weights, so read the terms attached
to the weights themselves.
