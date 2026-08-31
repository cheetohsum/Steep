#!/bin/bash
set -e

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${RT_BUNDLE_DIR:-$SOURCE_DIR/build-win/Release}"
mkdir -p "$BUILD_DIR"

# Copy freshly built binaries into the bundle when needed.
INSTALL_DIR="${RT_BUILD_OUTPUT_DIR:-$SOURCE_DIR/build-win/rtgui}"
for exe in steep.exe steep-cli.exe rawtherapee-cli.exe; do
    if [ -f "$INSTALL_DIR/$exe" ] && { [ ! -f "$BUILD_DIR/$exe" ] || [ "$INSTALL_DIR/$exe" -nt "$BUILD_DIR/$exe" ]; }; then
        cp "$INSTALL_DIR/$exe" "$BUILD_DIR/$exe"
    fi
done

# Copy all required DLLs
if [ -f "$BUILD_DIR/steep.exe" ]; then
    ldd "$BUILD_DIR/steep.exe" | grep mingw64 | awk '{print $3}' | while read -r dll; do
        cp -u "$dll" "$BUILD_DIR/" 2>/dev/null || true
    done
fi

# Also get DLLs from rawtherapee-cli
if [ -f "$BUILD_DIR/rawtherapee-cli.exe" ]; then
    ldd "$BUILD_DIR/rawtherapee-cli.exe" | grep mingw64 | awk '{print $3}' | while read -r dll; do
        cp -u "$dll" "$BUILD_DIR/" 2>/dev/null || true
    done
fi

# Copy GTK helper executables
cp -u /mingw64/bin/gdbus.exe "$BUILD_DIR/" 2>/dev/null || true
cp -u /mingw64/bin/gspawn-win64-helper.exe "$BUILD_DIR/" 2>/dev/null || true
cp -u /mingw64/bin/gspawn-win64-helper-console.exe "$BUILD_DIR/" 2>/dev/null || true

# Bundle Adwaita icons
mkdir -p "$BUILD_DIR/share/icons/Adwaita/symbolic"
cp -r /mingw64/share/icons/Adwaita/symbolic/* "$BUILD_DIR/share/icons/Adwaita/symbolic/" 2>/dev/null || true
cp /mingw64/share/icons/Adwaita/index.theme "$BUILD_DIR/share/icons/Adwaita/" 2>/dev/null || true

# Bundle GDK pixbuf loaders
mkdir -p "$BUILD_DIR/lib/gdk-pixbuf-2.0"
cp -r /mingw64/lib/gdk-pixbuf-2.0/* "$BUILD_DIR/lib/gdk-pixbuf-2.0/" 2>/dev/null || true

# Bundle GLib schemas
mkdir -p "$BUILD_DIR/share/glib-2.0/schemas"
cp /mingw64/share/glib-2.0/schemas/gschemas.compiled "$BUILD_DIR/share/glib-2.0/schemas/" 2>/dev/null || true

# GTK settings
mkdir -p "$BUILD_DIR/share/gtk-3.0"
printf '[Settings]\ngtk-button-images=1\n' > "$BUILD_DIR/share/gtk-3.0/settings.ini"

# Bundle ONNX Runtime + DirectML for native AI Denoise.
# These ship in ext/onnxruntime/bin/ alongside the staged headers and import lib.
ORT_BIN_SRC="$SOURCE_DIR/ext/onnxruntime/bin"
if [ -d "$ORT_BIN_SRC" ]; then
    echo "Bundling ONNX Runtime + DirectML DLLs for AI Denoise..."
    cp -u "$ORT_BIN_SRC/onnxruntime.dll" "$BUILD_DIR/"
    cp -u "$ORT_BIN_SRC/DirectML.dll" "$BUILD_DIR/"
fi

# The converted RawRefinery model is part of Steep's data bundle.
MODEL_SRC="$SOURCE_DIR/rtdata/models/aidenoise/ShadowWeightedL1.onnx"
MODEL_DEST="$BUILD_DIR/models/aidenoise"
mkdir -p "$MODEL_DEST"
cp -u "$MODEL_SRC" "$MODEL_DEST/"

# Lens correction database. The build points LENSFUN_DB_PATH at
# <exe>/share/lensfun, but nothing populated it — so Lensfun loaded zero
# cameras and zero lenses, leaving the Lens Profile camera/lens pickers empty
# and automatic matching unable to match anything.
mkdir -p "$BUILD_DIR/share/lensfun"
cp /mingw64/share/lensfun/version_1/*.xml "$BUILD_DIR/share/lensfun/" 2>/dev/null || true

if ! ls "$BUILD_DIR/share/lensfun/"*.xml >/dev/null 2>&1; then
    echo "Missing Lensfun database: $BUILD_DIR/share/lensfun" >&2
    exit 2
fi

# Keep the portable bundle's UI strings and third-party notices in sync.
mkdir -p "$BUILD_DIR/languages" "$BUILD_DIR/licenses"
cp "$SOURCE_DIR/rtdata/languages/default" "$BUILD_DIR/languages/default"
cp "$SOURCE_DIR/licenses/RawRefinery_LICENSE" "$BUILD_DIR/licenses/"
cp "$SOURCE_DIR/licenses/ONNXRuntime_LICENSE" "$BUILD_DIR/licenses/"
rm -f "$BUILD_DIR/scripts/rawrefinery_cli.py"

for required in \
    "$BUILD_DIR/onnxruntime.dll" \
    "$BUILD_DIR/DirectML.dll" \
    "$MODEL_DEST/ShadowWeightedL1.onnx"; do
    if [ ! -s "$required" ]; then
        echo "Missing required AI Denoise bundle component: $required" >&2
        exit 2
    fi
done

echo "DLL bundling done"
ls "$BUILD_DIR"/*.exe
