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

# Bundle embedded Python for AI Denoise
PYTHON_DIR="$BUILD_DIR/python"
if [ ! -f "$PYTHON_DIR/python.exe" ]; then
  echo "Bundling embedded Python for AI Denoise..."
  PYTHON_VERSION="3.11.9"
  mkdir -p "$PYTHON_DIR"
  EMBED_ZIP="/tmp/python-embed.zip"
  if [ ! -f "$EMBED_ZIP" ]; then
    curl -L -o "$EMBED_ZIP" \
      "https://www.python.org/ftp/python/${PYTHON_VERSION}/python-${PYTHON_VERSION}-embed-amd64.zip"
  fi
  unzip -q "$EMBED_ZIP" -d "$PYTHON_DIR"
  sed -i 's/#import site/import site/' "$PYTHON_DIR/python311._pth"
  curl -L -o "$PYTHON_DIR/get-pip.py" https://bootstrap.pypa.io/get-pip.py
  "$PYTHON_DIR/python.exe" "$PYTHON_DIR/get-pip.py" --no-warn-script-location
  "$PYTHON_DIR/python.exe" -m pip install \
    torch --index-url https://download.pytorch.org/whl/cpu \
    blended-tiling tifffile platformdirs requests numpy \
    --no-warn-script-location
  rm -f "$PYTHON_DIR/get-pip.py"
  echo "Embedded Python bundled."
else
  echo "Embedded Python already present, skipping."
fi

echo "DLL bundling done"
ls "$BUILD_DIR"/*.exe
