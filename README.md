# Steep

**A modern, AI-enhanced fork of [RawTherapee](https://rawtherapee.com) for RAW photo editing.**

Steep keeps RawTherapee's processing engine — which is excellent, and decades in the making — and rebuilds the experience around it: a cleaner interface, a set of AI-assisted tools, some creative effects that weren't there before, and a way for AI agents to drive the editor directly. Nothing from the original was removed.

> **Status:** In active development. Editing, export, and everything RawTherapee could already do are solid. The newer additions vary in maturity — see [Feature status](#feature-status) for an honest breakdown.

![Steep — Editor](screenshots/rt-editor.png)

---

## Contents

- [Downloads](#downloads)
- [What Steep adds](#what-steep-adds)
  - [Editing tools](#editing-tools)
  - [AI-assisted tools](#ai-assisted-tools)
  - [Interface](#interface)
  - [Browsing and organising](#browsing-and-organising)
  - [Export](#export)
  - [MCP server for AI agents](#mcp-server-for-ai-agents)
- [Feature status](#feature-status)
- [Building from source](#building-from-source)
  - [What you need](#what-you-need)
  - [Linux](#linux)
  - [Windows](#windows)
  - [macOS](#macos)
  - [Docker](#docker)
- [Project layout](#project-layout)
- [Acknowledgments](#acknowledgments)
- [License](#license)

---

## Downloads

Builds are published automatically from the `dev` branch every time it changes.

| Platform | Architecture | Download |
|----------|-------------|----------|
| **Windows** | x86_64 | [Installer (.exe)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_x86_64_release.exe) · [ZIP](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_x86_64_release.zip) |
| **Windows** | ARM64 | [Installer (.exe)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_arm64_release.exe) · [ZIP](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_arm64_release.zip) |
| **macOS** | Apple Silicon | [DMG (.zip)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_macOS_arm64_Release.zip) |
| **macOS** | Intel x86_64 | [DMG (.zip)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_macOS_x86_64_Release.zip) |
| **Linux** | x86_64 | [AppImage](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_x86_64_release.AppImage) |
| **Linux** | ARM64 | [AppImage](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_arm64_release.AppImage) |

Everything else, including debug builds, is on the [nightly releases](https://github.com/cheetohsum/Steep/releases/tag/nightly-github-actions) page.

The Windows installer recognises an existing copy of Steep and updates it in place, keeping your settings, profiles and cache. If it finds installs left behind by much older builds, it offers to clear them out first.

---

## What Steep adds

### Editing tools

**3-way colour grading.** Four colour wheels — shadows, midtones, highlights, and one for the whole image — each with its own luminance control, plus blending and balance sliders for where those ranges hand off to each other. Right-click a wheel to send it back to neutral, or hold Shift while dragging to move it at quarter speed when you need to be precise.

**Point colour.** Pick a colour out of the photo and adjust just that one — hue, saturation, luminance, and how wide a range around it to include. You can stack as many targets as you need, and each gets a readable name automatically.

**Glow & Halation.** Three related light effects, each with its own controls: a soft glow that bleeds out of bright areas, halation with adjustable warmth for the red-orange fringe you get around highlights on film, and anamorphic-style flares with adjustable length and angle. A shared threshold decides how bright something has to be before it starts blooming.

**Double Exposure.** Blend a second photo into the one you're editing, the way a film camera stacks frames. It can meter each exposure down automatically as a real camera would, or leave it uncompensated for the blown, luminous look.

**Film Presets.** Film emulation you can actually steer — strength, contrast, saturation, warmth, tint and fade, plus separate hue and tint control over shadows and highlights, and halation, RGB shift and rolloff on top. Hovering a preset previews it. The stock names are invented rather than real brands, deliberately.

**Tilt-Shift.** The miniature effect, with blur strength, focus position, width, feathering and rotation. You can drag the focus band around on the photo itself instead of guessing at numbers.

**Lens corrections.** Steep looks up your camera and lens automatically and tells you what it matched, right under the button. If nothing matches you can pick a profile by hand, and there's a checkbox to make automatic matching the default for every photo.

### AI-assisted tools

**Smart Tools**, in the Spot Removal panel. Brush-based cleanup backed by an inpainting model: paint over something and it fills in from the surroundings the moment you let go — a single click for a dab, or drag for a freeform stroke. Remove Object is the mature one. Remove Dust, Remove Reflections and Generative Fill are there but newer and less proven.

**Smart masks.** Instead of tracing a selection, click what you want and a mask of it is built for you. There's also a lasso that clings to nearby edges as you draw, so a rough outline is usually enough. These feed the existing local-adjustment tools, so anything you can do to a whole photo you can do to just the subject, or just the sky.

**AI denoise.** Machine-learning denoising with an ISO control, a blend slider to dial back how much of the cleaned result you keep, optional GPU acceleration, and results cached per ISO so re-running is quick. It works in the background and can be cancelled. The model ships with Steep and inference runs inside the app, so there is nothing to install. If you already use RawRefinery, Steep will pick up its model too rather than making you keep a second copy.

**Auto Edit.** Right-click photos in the browser and Steep grades them for you — exposure, white balance, and a film stock chosen to suit the picture. There's a neutral variant if you want the correction without the look.

### Interface

**Mode-based editor.** The old stack of tabs is gone, replaced by four modes:

| Mode | What's in it |
|------|--------------|
| **Presets** | Browse presets as cards, with thumbnails showing what each one does |
| **Edit** | Every editing tool in one scrolling panel, grouped into Light, Colour, Detail, Effects, Advanced and Calibration |
| **Crop** | Crop, resize, rotation, perspective, distortion and lens geometry |
| **Mask** | Local adjustments and masking, including the AI masks |

**Preset browser.** Presets are a grid of cards with preview thumbnails, sorted into collapsible categories, with quick cards for Custom and Last Saved. Full save, load, copy, paste, rename and delete, and you can drag them into whatever order suits you.

**Sliders that behave.** Each slider draws its name and value inside a single pill, rather than a slider plus a separate spin box. Click the number to type an exact value. Double-click or right-click to reset it. Right-click the Edit pencil for a slider that scales the text and pill size, if the defaults don't suit your screen.

**Floating panels.** History and Navigator can be pulled out into their own windows.

**Themes.** Five of them — RawTherapee Modern, RawTherapee Legacy, Rem (deep navy and indigo, with cyan and copper), TooWa Blue, and TooWa Grey Bright. Each is a palette layered on one shared stylesheet, so switching themes changes the colours without disturbing the layout.

### Browsing and organising

![Steep — File Browser](screenshots/rt-browser.jpg)

**Albums.** Group photos independently of where they sit on disk. Regular albums are manual; smart albums fill themselves from rules — rating, colour label, file type, camera, lens, ISO, focal length, aperture, whether it's been edited — combined with AND/OR. Folders keep albums tidy. Everything syncs across open browser windows and survives a restart.

**Filmstrip and thumbnails.** Thumbnail size is adjustable for both the filmstrip and the browser, and the filmstrip can sit at the top or the bottom of the editor — the rating and sorting buttons move with it. Both are in Preferences.

**Before/After.** The "before" side shows the original, unedited photo.

### Export

**Export queue.** Queued photos appear as rows straight away, and get a badge on their thumbnail in the browser so you can see at a glance what's already waiting.

**Filename templates.** The help panel lists ready-made templates you can apply with one click — beside the original, into an "Exported" subfolder, and so on — and hovering one shows you exactly where an example photo would end up.

**Watermarks.** Text, an image, or both. Choose the font, size, weight and colour, add an outline and a drop shadow, and place it on a nine-point grid with margin and rotation. The image shares the outline, shadow, opacity, position and rotation settings with the text, and leaving the text empty gives you an image-only watermark. There's a live preview.

### MCP server for AI agents

Steep runs a built-in MCP (Model Context Protocol) server, which lets AI assistants and scripts drive the editor over HTTP. It starts with the application.

| Tool | What it does |
|------|--------------|
| `get_image_info` | Image metadata and EXIF |
| `get_params` / `set_params` | Read and write processing settings across 20+ sections |
| `set_tool_enabled` | Turn an individual tool on or off |
| `list_tools` | List every tool and its current state |
| `adjust_exposure` | Quick exposure change |
| `adjust_white_balance` | Quick white balance change |
| `load_profile` / `save_profile` | Read and write PP3 profile files |
| `list_volumes` | Connected drives and mount points |
| `get_mount_events` | Notifications when drives come and go |
| `scan_photos` | Read EXIF across a folder |
| `import_photos` | Import and file photos by date |

**Finding the port.** Steep picks a port at random between 39000 and 39999 each time it starts, so there is no fixed address to hard-code. Open **MCP Server...** in the app to see the current one, along with connection and request activity.

Then point your client at it — for Claude Code, in `.mcp.json`:

```json
{
  "mcpServers": {
    "steep": {
      "type": "http",
      "url": "http://localhost:PORT/mcp"
    }
  }
}
```

Replace `PORT` with the number the dialog shows.

---

## Feature status

| Feature | Status |
|---------|--------|
| Mode-based editor, preset browser, slider redesign | Working |
| 3-way colour grading, point colour | Working |
| Film presets, tilt-shift | Working |
| Glow & halation | Working |
| Double exposure | Working |
| Lens correction auto-matching | Working |
| Albums and smart albums | Working |
| Export queue, filename templates, watermarks | Working |
| Floating history and navigator | Working |
| Themes | Working |
| MCP server | Working |
| Auto Edit | Working |
| Smart masks (AI selection, lasso) | Working |
| AI denoise | Working |
| Smart Tools — Remove Object | Working, but see the note below |
| Smart Tools — Dust, Reflections, Generative Fill | In progress |
| Everything RawTherapee already did | Untouched |

Smart masks and AI denoise are switched on in every download here — there is
nothing extra to fetch or configure. They are only optional if you build it
yourself, where AI masking is off by default on Linux and macOS unless you
pass `-DWITH_AI_MASKING=ON`.

Remove Object is the exception, because its model is around 200 MB — too big to
keep in the repository, and enough to more than double the size of a download if
it were simply bundled. So Steep fetches it on demand instead: Smart Tools shows
a Download button the first time you need it, and the Windows installer offers
the same thing as an opt-in step. It is a one-off, kept with your settings, and
the tools work immediately afterwards without a restart. Everything else in
Steep is unaffected while it downloads.

---

## Building from source

### What you need

The same as RawTherapee, plus a couple of optional extras.

**Required:** CMake 3.15+, GCC 4.9+ or Clang, GTK+ 3 with gtkmm 3.24, libraw, lensfun, lcms2, libiptcdata, librsvg, libtiff, libjpeg, libpng, zlib, expat, fftw3, exiv2, and libjxl for JPEG-XL.

**Optional:** ONNX Runtime 1.17+, which turns on smart masks, Remove Object and
AI denoise. On Windows a copy is included in the repository and these are on by
default; on Linux and macOS, install it and add `-DWITH_AI_MASKING=ON`.

The mask and denoise models are in the repository and get installed for you.
The Remove Object model is not — at roughly 200 MB it does not belong in git —
so point the build at a copy and it will fetch it once and package it like any
other resource:

```bash
cmake .. -DWITH_AI_MASKING=ON -DAI_INPAINT_MODEL_URL=https://example.com/lama_inpainting.onnx
```

Add `-DAI_INPAINT_MODEL_SHA256=<hash>` to have the download verified. An
existing `rtdata/models/lama_inpainting.onnx` is never re-downloaded, and if the
URL is left out the build simply carries on without Remove Object.

One note on exiv2: Steep builds against both the 0.27 and 0.28 series, which changed a good deal of their API in between. Either will work.

### Linux

```bash
sudo apt update
sudo apt install -y build-essential cmake git libgtk-3-dev libgtkmm-3.0-dev liblensfun-dev liblensfun-bin liblcms2-dev libiptcdata0-dev librsvg2-dev libcanberra-gtk3-dev libtiff-dev libjpeg-dev libpng-dev libgif-dev libwebp-dev libwebpdemux2 zlib1g-dev libexpat1-dev libfftw3-dev libbrotli-dev libinih-dev gettext libexiv2-dev
```

JPEG-XL is optional, and only packaged on Ubuntu 24.04 / Mint 22 and newer:

```bash
sudo apt install -y libjxl-dev
```

Then build:

```bash
git clone https://github.com/cheetohsum/Steep.git
```

```bash
cd Steep && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && sudo make install
```

Add `-DWITH_AI_MASKING=ON` to the cmake line if you want the AI masking tools.

If cmake stops on a missing package such as `exiv2>=0.24 not found`, install the matching `-dev` package and run it again. On older releases `libtiff-dev` may be called `libtiff5-dev`.

### Windows

Build under MSYS2 in the MINGW64 environment — that is what the release builds use:

```bash
mkdir build && cd build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && ninja && ninja install
```

WSL2 with WSLg also works if you would rather build against the Linux instructions above and run it through X.

### macOS

```bash
brew install cmake gtk+3 gtkmm3 libraw lensfun little-cms2 libiptcdata librsvg libtiff jpeg libpng fftw exiv2 jpeg-xl
```

```bash
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(sysctl -n hw.ncpu) && sudo make install
```

### Docker

There is a Dockerfile set up for building with AI masking:

```bash
docker build -f Dockerfile.aimasking -t steep-aimasking .
```

However you build it, the application is called `steep` (and `steep-cli` for the command-line version), not `rawtherapee`.

---

## Project layout

```
Steep/
  rtengine/          Processing engine — demosaic, colour, denoise, effects
  rtgui/             The GTK3 application
    mcp/             MCP server
    tools/           Tool panels — curves, colour grading, film presets, the rest
    widgets/         Custom controls — sliders, colour wheels, curve editors
    windows/         Dialogs — preferences, history, navigator, double exposure
  rtdata/            Resources shipped with the app
    themes/          Themes, with the shared stylesheet in themes/common
    languages/       Translations
    icons/           SVG icons
    models/          AI model files
  cmake/             CMake modules
  tools/             Build and utility scripts
```

---

## Acknowledgments

Steep is built on [RawTherapee](https://rawtherapee.com). The processing engine, the demosaicing algorithms, the colour management and the many years of careful work behind them are the RawTherapee team's, not ours.

## License

GNU General Public License v3.0, the same as RawTherapee. See [LICENSE](LICENSE).
