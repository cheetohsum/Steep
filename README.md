# Steep

**A modern, AI-enhanced fork of [RawTherapee](https://rawtherapee.com) for professional RAW photo editing.**

Steep takes the powerful open-source RAW processing engine of RawTherapee and adds a modernized interface, AI-powered tools, creative effects, and an MCP server for AI agent integration — all while preserving every original RawTherapee feature.

> **Status:** Active development. Core editing, export, and all original RawTherapee features work fully. New features (AI masking, AI denoise, MCP server, watermarking, albums) are in various stages of completion. See [Feature Status](#feature-status) below.

![Steep — Editor](screenshots/rt-editor.png)

---

## Downloads

Pre-built binaries are published automatically from the `dev` branch. Grab the latest for your platform:

| Platform | Architecture | Download |
|----------|-------------|----------|
| **Windows** | x86_64 | [Installer (.exe)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_x86_64_release.exe) · [ZIP](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_x86_64_release.zip) |
| **Windows** | ARM64 | [Installer (.exe)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_arm64_release.exe) · [ZIP](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_win64_arm64_release.zip) |
| **macOS** | Apple Silicon | [DMG (.zip)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_macOS_arm64_Release.zip) |
| **macOS** | Intel x86_64 | [DMG (.zip)](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_macOS_x86_64_Release.zip) |
| **Linux** | x86_64 | [AppImage](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_x86_64_release.AppImage) |
| **Linux** | ARM64 | [AppImage](https://github.com/cheetohsum/Steep/releases/download/nightly-github-actions/Steep_dev_arm64_release.AppImage) |

Browse all builds (including debug and versioned releases) on the [Nightly Releases](https://github.com/cheetohsum/Steep/releases/tag/nightly-github-actions) page.

---

## What's New in Steep

### MCP Server (Model Context Protocol)

Steep includes a built-in **MCP server** that exposes the editor to AI agents, automation scripts, and external tools over HTTP. The server implements the Model Context Protocol with JSON-RPC 2.0 and runs on `localhost:39793`.

**Exposed tools:**

| Tool | Description |
|------|-------------|
| `get_image_info` | Image metadata and EXIF data |
| `get_params` / `set_params` | Read/write processing parameters (20+ sections) |
| `set_tool_enabled` | Toggle individual tool activation |
| `list_tools` | List all tools and their current state |
| `adjust_exposure` | Quick exposure adjustment |
| `adjust_white_balance` | Quick white balance adjustment |
| `load_profile` / `save_profile` | PP3 profile file I/O |
| `list_volumes` | Connected drives and mount points |
| `get_mount_events` | Volume mount/unmount event stream |
| `scan_photos` | Directory EXIF metadata scanning |
| `import_photos` | Smart import with date-based organization |

**Usage with Claude Code / AI agents:**

Add to your `.mcp.json`:
```json
{
  "mcpServers": {
    "rawtherapee": {
      "type": "http",
      "url": "http://localhost:39793/mcp"
    }
  }
}
```

The MCP server starts automatically with the application and includes a UI dialog for monitoring connections and requests.

### Mode-Based Editor UI

The traditional multi-tab notebook interface has been replaced with a clean, mode-based layout using a **ModeButtonBar**:

| Mode | Purpose |
|------|---------|
| **Presets** | Visual preset browser with thumbnail previews |
| **Edit** | All editing tools organized into collapsible groups (Light, Color, Detail, Effects, Advanced, Calibration) |
| **Crop** | Transform tools (Crop, Resize, Lens Geometry, Rotation, Perspective, Distortion) |
| **Mask** | LocalLab selective editing with optional AI masks |

### Visual Preset Browser

Replaces the old tree-based profile selector with a card-based grid:

- Thumbnail previews of each preset's effect
- Quick-access cards for "Custom" and "Last Saved" states
- Category-based organization with collapsible sections
- Background thumbnail generation
- Full save/load/copy/paste workflow
- Context menus for rename/overwrite/delete
- Drag-and-drop preset reordering

### File Browser

![Steep — File Browser](screenshots/rt-browser.jpg)

### 3-Way Color Grading

Professional color grading tool with four interactive **color wheels** for shadows, midtones, highlights, and global:

- Per-range hue, saturation, and luminance control
- Global color wheel with luminance in an advanced section
- Blending and balance sliders for fine-tuning tonal splits
- Custom Cairo-rendered color wheel widget with double-click reset

### Point Color (Hue-Selective HSL)

Targeted color adjustments by hue range:

- Multiple independent color targets
- Per-target hue shift, saturation, luminance, and range controls
- Color picker to sample targets directly from the image
- Auto-generated color name labels

### Film Presets

Parametric film emulation with a popover-based preset selector:

- Adjustable strength, contrast, saturation, warmth, tint, and fade
- Shadow/highlight specific hue and tint controls
- Halation, RGB shift, and rolloff controls
- Hover preview with timeout

### Tilt-Shift

Miniature/diorama effect with interactive geometry:

- Blur amount, focus position, and width controls
- Feather and rotation adjustment
- Draggable on-image geometry for visual placement

### AI Denoise

Integration with RawRefinery for AI-powered denoising:

- ISO conditioning control
- Blend slider to mix denoised result with original
- GPU acceleration toggle
- Background processing with cancel support
- Result caching per ISO level

### AI Semantic Masking (Optional)

Compile-time feature (`-DWITH_AI_MASKING=ON`) using ONNX Runtime for automatic subject detection:

- 9 semantic classes: background, person, sky, vegetation, building, vehicle, animal, foreground object
- Mask threshold, feather, blur, and opacity controls
- Edge refinement with configurable radius and epsilon
- DeepLabV3-MobileNetV3 model included
- Integrates with LocalLab for targeted adjustments on detected regions

### AI Inpainting

Content-aware fill for masked regions using a LaMa ONNX model.

### Watermarking

Full-featured watermark system applied at export:

- Custom text with font selection, size, bold/italic
- Text color with alpha transparency
- Stroke (outline) with configurable color and width
- Drop shadow with color, offset, and blur
- 9-point positioning grid with margin and rotation controls
- Live preview window

### Album / Collection Management

Organize images beyond the filesystem:

- **Regular Albums** — manual image collections
- **Smart Albums** — rule-based auto-filtering by rating, color label, file type, camera, lens, ISO, focal length, aperture, or edited status (with AND/OR logic)
- **Folders** — organizational hierarchy for albums
- Persistent storage, global sync across all browser instances
- Cover thumbnail previews and drag-and-drop organization

### Floating History & Navigator

History and Navigator panels can be undocked into independent floating windows.

### New Themes

- **RawTherapee - Modern.css** — contemporary dark theme with updated styling
- **Rem.css** — fantasy-inspired dark theme (deep navy/indigo, cyan accents, copper details)

### Custom Icon Set

12+ new SVG icons for mode buttons, navigation, window controls, and album management.

### UI & UX Improvements

Steep includes dozens of UI/UX refinements that collectively make the editor feel more modern and polished.

#### Modernized Slider Controls
- **Pill-style adjusters** — sliders render their value label directly via Cairo inside a dark pill shape, replacing the traditional GTK spin button + separate slider layout
- **Double-click reset** — double-click any slider or its label to reset to default
- **Clean appearance** — stepper buttons (+/-) hidden, transparent GTK trough (all drawing done by custom Cairo paint)

#### Edit Pane Layout
- **Single scrollable panel** — all editing tools in one continuous scroll instead of multiple tab pages
- **Collapsible ToolGroups** — tools organized into logical groups (Light, Color, Detail, Effects, Advanced, Calibration) with expand/collapse headers
- **AdvancedSection widget** — less-used controls tucked into expandable "Advanced" sections within individual tools, keeping the default view clean

#### Crop & Transform Refinements
- **Reset buttons** on crop and perspective section headers for quick clearing
- **Toggle behavior** — crop/perspective tool buttons toggle off when clicked while already active
- **Auto-deselect** — switching modes automatically deselects active crop/perspective tools

#### Masking & Spot Improvements
- **Spot panel redesign** — streamlined ControlSpotPanel layout
- **B&W masking bridge** — B&W enabled state bridged to LocalLab spots for mask-mode desaturation
- **Auto-expand** — spot/masking groups auto-expand when entering mask mode

#### Browser & Filmstrip
- **Configurable thumbnail sizes** — filmstrip and browser thumbnail sizes adjustable in Preferences
- **Toolbar polish** — consistent entry field heights, alignment, and spacing in the file browser toolbar

#### Before/After View
- Shows the original unedited image as the default "Before" state

#### Cross-Platform Fixes
- **Windows:** GTK theme CSS compatibility fixes, editor container background colors
- **macOS:** Cairo background fills for Quartz backend, icon fallback for theme lookup failures, browser toolbar alignment

---

## Feature Status

| Feature | Status |
|---------|--------|
| MCP server | Working |
| Mode-based editor UI | Working |
| Visual preset browser | Working |
| 3-way color grading | Working |
| Point color / HSL | Working |
| Film presets | Working |
| Tilt-shift | Working |
| Watermarking | Working |
| Album browser | Working |
| Floating history/navigator | Working |
| New themes | Working |
| AI denoise (RawRefinery) | Working (requires external Python backend) |
| AI semantic masking | Experimental (requires ONNX Runtime, optional build flag) |
| AI inpainting | Experimental (requires ONNX model) |
| UI & UX refinements | Working |
| All original RawTherapee features | Fully preserved |

---

## Building from Source

### Dependencies

Steep has the same base dependencies as RawTherapee, plus optional extras.

**Required (same as RawTherapee):**

- CMake >= 3.15
- GCC >= 4.9 (or Clang)
- GTK+ 3 / gtkmm 3.24
- libraw
- lensfun
- lcms2
- libiptcdata
- librsvg
- libtiff, libjpeg, libpng
- zlib
- expat
- fftw3
- exiv2
- libjxl (JPEG-XL)

**Optional — AI Masking:**

- ONNX Runtime >= 1.17

**Optional — AI Denoise:**

- Python 3 with RawRefinery installed

### Linux

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake git \
  libgtk-3-dev libgtkmm-3.0-dev \
  libraw-dev liblensfun-dev liblcms2-dev \
  libiptcdata0-dev librsvg2-dev \
  libtiff-dev libjpeg-dev libpng-dev \
  zlib1g-dev libexpat1-dev libfftw3-dev \
  libexiv2-dev libjxl-dev

# Clone
git clone https://github.com/cheetohsum/Steep.git
cd RawTherapee

# Configure
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Optional: enable AI masking
# cmake .. -DCMAKE_BUILD_TYPE=Release -DWITH_AI_MASKING=ON

# Build
make -j$(nproc)

# Install
sudo make install
```

Pre-built Linux AppImages (x86_64 and ARM64) are available from the [Nightly Releases](https://github.com/cheetohsum/Steep/releases/tag/nightly-github-actions).

### Docker (with AI Masking)

A Dockerfile is provided for building with AI masking support:

```bash
docker build -f Dockerfile.aimasking -t steep-aimasking .
```

### Windows

Use MSYS2/MinGW or WSL. The project builds and runs under WSL2 with WSLg for display:

```bash
# Inside WSL2
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
rawtherapee
```

Pre-built Windows installers (.exe) and ZIP archives are available from the [Nightly Releases](https://github.com/cheetohsum/Steep/releases/tag/nightly-github-actions).

### macOS

```bash
# Install dependencies via Homebrew
brew install cmake gtk+3 gtkmm3 libraw lensfun little-cms2 \
  libiptcdata librsvg libtiff jpeg libpng fftw exiv2 jpeg-xl

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
sudo make install
```

Pre-built DMGs (Intel and Apple Silicon) are available from the [Nightly Releases](https://github.com/cheetohsum/Steep/releases/tag/nightly-github-actions).

---

## Project Structure

```
Steep/
  rtengine/          Processing engine (demosaic, color, denoise, etc.)
  rtgui/             GTK3 GUI application
    mcp/             MCP server (JSON-RPC 2.0 over HTTP)
    tools/           Editing tool panels (tonecurve, color grading, film presets, etc.)
    widgets/         Custom widgets (adjuster, color wheel, etc.)
    windows/         Dialog windows (preferences, history, navigator)
  rtdata/            Runtime resources
    themes/          CSS themes
    languages/       Localization files
    icons/           SVG icon sets
    models/          AI model files (ONNX)
  cmake/             CMake modules
  tools/             Build and utility scripts
```

---

## Acknowledgments

Steep is built on top of [RawTherapee](https://rawtherapee.com), an outstanding open-source RAW photo processor. All credit for the core processing engine, demosaicing algorithms, color management, and decades of refinement goes to the RawTherapee team and its contributors.

## License

GNU General Public License v3.0 — same as RawTherapee. See [LICENSE](LICENSE) for details.
