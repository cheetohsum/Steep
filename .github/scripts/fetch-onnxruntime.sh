#!/usr/bin/env bash
# Fetch a prebuilt ONNX Runtime and lay it out as the GitHub release archives
# did: <dest>/include and <dest>/lib.
#
#   fetch-onnxruntime.sh <version> <runtime id> <dest>
#   runtime ids: linux-x64, linux-arm64, osx-x64, osx-arm64, win-arm64
#
# The per-platform archives used to come from the microsoft/onnxruntime
# GitHub releases, which stopped resolving in November 2026. The same
# binaries ship in Microsoft's Microsoft.ML.OnnxRuntime package on NuGet:
# headers under build/native/include, one library per platform under
# runtimes/<id>/native. That package has a single unversioned file name per
# library, so the name the library records for itself (its SONAME or
# install name, which is what a program linked against it asks for at run
# time) is recreated beside it.
set -euo pipefail

version="$1"
rid="$2"
dest="$3"

work="$(mktemp -d)"
package="${work}/onnxruntime.nupkg"
url="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${version}/microsoft.ml.onnxruntime.${version}.nupkg"
echo "Downloading ONNX Runtime ${version} (${rid}) from ${url}"
curl --fail --location --retry 3 --retry-delay 5 --output "${package}" "${url}"

echo "Extracting."
unzip -q "${package}" "build/native/include/*" "runtimes/${rid}/native/*" -d "${work}/pkg"
native="${work}/pkg/runtimes/${rid}/native"
if [ ! -d "${native}" ] || [ -z "$(ls -A "${native}")" ]; then
    echo "The package has no runtimes/${rid}/native. It holds:" >&2
    unzip -l "${package}" | grep -o 'runtimes/[^/]*/native' | sort -u >&2
    exit 1
fi

mkdir -p "${dest}/include" "${dest}/lib"
cp -r "${work}/pkg/build/native/include/." "${dest}/include/"
cp "${native}"/* "${dest}/lib/"

case "${rid}" in
    linux-*)
        soname="$(objdump -p "${dest}/lib/libonnxruntime.so" | awk '/SONAME/ { print $2 }')"
        if [ -n "${soname}" ] && [ "${soname}" != "libonnxruntime.so" ]; then
            ln -sf libonnxruntime.so "${dest}/lib/${soname}"
        fi
        ;;
    osx-*)
        install_name="$(otool -D "${dest}/lib/libonnxruntime.dylib" | tail -n 1)"
        recorded="$(basename "${install_name}")"
        if [ -n "${recorded}" ] && [ "${recorded}" != "libonnxruntime.dylib" ]; then
            ln -sf libonnxruntime.dylib "${dest}/lib/${recorded}"
        fi
        ;;
esac

rm -rf "${work}"
echo "ONNX Runtime ${version} for ${rid} is in ${dest}:"
ls -l "${dest}/lib"
