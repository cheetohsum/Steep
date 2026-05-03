#!/bin/bash
set -e

BUILD_DIR="$HOME/build-win/Release"

# Copy binaries from RelWithDebInfo (where ninja install puts them)
INSTALL_DIR="$HOME/build-win/RelWithDebInfo"
for exe in steep.exe steep-cli.exe rawtherapee-cli.exe; do
    if [ -f "$INSTALL_DIR/$exe" ] && [ "$INSTALL_DIR/$exe" -nt "$BUILD_DIR/$exe" ]; then
        cp "$INSTALL_DIR/$exe" "$BUILD_DIR/$exe"
    fi
done

# Copy all required DLLs
ldd "$BUILD_DIR/steep.exe" | grep mingw64 | awk '{print $3}' | while read dll; do
    cp -u "$dll" "$BUILD_DIR/" 2>/dev/null || true
done

# Also get DLLs from rawtherapee-cli
ldd "$BUILD_DIR/rawtherapee-cli.exe" | grep mingw64 | awk '{print $3}' | while read dll; do
    cp -u "$dll" "$BUILD_DIR/" 2>/dev/null || true
done

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
ORT_BIN_SRC="$(dirname "$0")/ext/onnxruntime/bin"
if [ -d "$ORT_BIN_SRC" ]; then
  echo "Bundling ONNX Runtime + DirectML DLLs for AI Denoise..."
  cp -u "$ORT_BIN_SRC/onnxruntime.dll" "$BUILD_DIR/" 2>/dev/null || true
  cp -u "$ORT_BIN_SRC/DirectML.dll"   "$BUILD_DIR/" 2>/dev/null || true
fi

# Drop the old embedded Python interpreter — no longer used. Removing it
# saves ~700 MB in the bundle. Skip removal if user wants to keep it for
# other tools (gated on whether RT_AI_DENOISE_KEEP_PYTHON env var is set).
if [ -d "$BUILD_DIR/python" ] && [ -z "${RT_AI_DENOISE_KEEP_PYTHON:-}" ]; then
  echo "Removing legacy embedded Python ($BUILD_DIR/python) — native ONNX engine in use."
  rm -rf "$BUILD_DIR/python"
fi

echo "DLL bundling done"
ls "$BUILD_DIR"/*.exe
