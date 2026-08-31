# Bundled AI models

Steep ships the models its AI tools need, so there is nothing for users to
download or configure. They are third-party work, and each carries its own
licence — see the note at the end, because one of them is a problem.

## In this directory

### `segformer_b0_ade20k.onnx` — 15 MB

Semantic segmentation, used by the smart mask tools to work out what is in a
photo. SegFormer-B0 trained on ADE20K.

- Upstream: NVIDIA's published weights,
  <https://huggingface.co/nvidia/segformer-b0-finetuned-ade-512-512>, cited in
  `rtengine/aisegmentation.cc`
- Paper: *SegFormer: Simple and Efficient Design for Semantic Segmentation with
  Transformers*, Xie et al., NeurIPS 2021
- Licence: **NVIDIA Source Code License — non-commercial.** See
  [UNRESOLVED: the SegFormer licence](#unresolved-the-segformer-licence).

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

### `lama_inpainting.onnx` — ~200 MB

The inpainting model behind Remove Object. It is **not** stored in git, because
GitHub refuses any file over 100 MiB and this one is roughly twice that. Git LFS
could hold it, but every clone would then pay for it.

- Upstream: [LaMa](https://github.com/advimman/lama), Suvorov et al.,
  *Resolution-robust Large Mask Inpainting with Fourier Convolutions*, WACV 2022
- Licence: Apache 2.0

Instead the build fetches it on demand. Give CMake a URL and it downloads the
model into this directory, after which the normal install and bundle rules
package it like anything else:

```bash
cmake .. -DWITH_AI_MASKING=ON -DAI_INPAINT_MODEL_URL=https://example.com/lama_inpainting.onnx
```

`-DAI_INPAINT_MODEL_SHA256=<hash>` will verify the download. A file already
present here is never re-fetched, and leaving the URL unset just means the build
carries on without Remove Object rather than failing.

CI passes this URL from the `AI_INPAINT_MODEL_URL` repository variable, so
setting that variable once is all it takes for every published build to include
the model.

## A note on licences

LaMa (Apache 2.0) and RawRefinery (MIT) are both fine. Their terms can be
satisfied inside a GPL-3.0 work, so redistributing those two with Steep is
allowed. Both require that the original copyright and attribution notices
survive, which is part of what this file is for.

### UNRESOLVED: the SegFormer licence

The SegFormer weights are the exception, and shipping them looks like a genuine
conflict rather than a technicality.

NVIDIA releases SegFormer under the NVIDIA Source Code License, which says the
work "may be used non-commercially, meaning for research or evaluation purposes
only". GPL-3.0 does not allow a distributed work to carry extra restrictions of
that kind, so a GPL-3.0 release that contains these weights cannot honour both
licences at once. Anyone using Steep commercially would also be outside
NVIDIA's terms, whatever our own licence says.

Ways out, roughly in order of least disruption:

1. **Swap the model.** Any reasonably capable segmentation network under a
   permissive licence would do, and the tool does not depend on SegFormer
   specifically. Steep used DeepLabV3-MobileNetV3 (BSD-3-Clause, via
   torchvision) before this, which is one ready answer.
2. **Stop shipping it.** Treat it the way the inpainting model is treated:
   fetched only if someone points the build at a copy, so the released binary
   carries nothing awkward.
3. **Licence it.** NVIDIA takes commercial enquiries for SegFormer.

Until one of those happens, this is a known issue rather than a settled
arrangement, and it should not be described as resolved anywhere else in the
documentation.
