#!/bin/sh
# Builds dist/moza-farmsim-link-<version>-windows.zip: everything a user installs, except
# MOZA_SDK.dll, which is MOZA's to distribute (see docs/INSTALL.md).
# Needs mingw-w64, cmake, ninja, lua and the MOZA SDK under sdk/ (see README.md).
set -eu
cd "$(dirname "$0")/.."

version=$(sed -n 's/^project(moza_fs_bridge VERSION \([0-9.]*\).*/\1/p' bridge/CMakeLists.txt)
[ -n "$version" ] || { echo "could not read the version from bridge/CMakeLists.txt" >&2; exit 1; }

# The mod carries the same version in FS25's four-part form (0.1.1 -> 0.1.1.0), in two places.
mod_desc=$(sed -n 's|.*<version>\(.*\)</version>.*|\1|p' fs-mod/FS25_MozaYokeLink/modDesc.xml)
mod_lua=$(sed -n 's/^MozaYokeLink.VERSION = "\(.*\)"/\1/p' fs-mod/FS25_MozaYokeLink/scripts/MozaYokeLink.lua)
if [ "$mod_desc" != "$version.0" ] || [ "$mod_lua" != "$version.0" ]; then
    echo "version mismatch: bridge $version, modDesc.xml $mod_desc, MozaYokeLink.lua $mod_lua (both should be $version.0)" >&2
    exit 1
fi

cmake -S bridge -B bridge/build-win -G Ninja --toolchain cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build bridge/build-win
(cd fs-mod && ./package.sh >/dev/null)

name="moza-farmsim-link-$version-windows"
stage="dist/$name"
rm -rf "$stage" "dist/$name.zip"
mkdir -p "$stage"
cp bridge/build-win/moza_fs_bridge.exe "bridge/windows/Start FS25 yoke link.bat" fs-mod/dist/FS25_MozaYokeLink.zip "$stage/"
cp docs/INSTALL.md "$stage/INSTALL.md"
cp LICENSE "$stage/LICENSE.txt"
(cd dist && zip -q -r "$name.zip" "$name")
rm -rf "$stage"

echo "built dist/$name.zip:"
unzip -l "dist/$name.zip"
