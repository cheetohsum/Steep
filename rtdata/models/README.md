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
37.4). The upstream repo has heavier variants, HRNetV2 among them at 43.2, if
that turns out to matter more than the speed.

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

All three models are under licences whose terms can be satisfied inside a
GPL-3.0 work: BSD-3-Clause, MIT and Apache 2.0. Each requires the original
copyright and attribution notices to survive, which is part of what this file
is for.

If you swap a model, check that direction of compatibility before shipping it.
The trap is not obscure: NVIDIA's SegFormer weights sat here for a while and
are non-commercial, which GPL-3.0 cannot carry. A permissive-sounding project
licence does not always cover the published weights, so read the terms attached
to the weights themselves.
