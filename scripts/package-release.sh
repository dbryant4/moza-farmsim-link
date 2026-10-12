#!/bin/sh
# Builds dist/moza-farmsim-link-<version>-windows.zip: everything a user installs, except
# MOZA_SDK.dll, which is MOZA's to distribute (see docs/INSTALL.md).
# Also builds dist/moza-farmsim-link-<version>-setup.exe, the installer.
# <version> is 0.1.3 for a finished version and 0.1.3-pre1 for a pre-release of it.
# Needs mingw-w64, makensis, cmake, ninja, lua and the MOZA SDK under sdk/ (see README.md).
set -eu
cd "$(dirname "$0")/.."

version=$(sed -n 's/^project(moza_fs_bridge VERSION \([0-9.]*\).*/\1/p' bridge/CMakeLists.txt)
[ -n "$version" ] || { echo "could not read the version from bridge/CMakeLists.txt" >&2; exit 1; }
# A pre-release adds "-preN" to the name of everything; the number alone stays in $version.
pre=$(sed -n 's/^set(BRIDGE_PRERELEASE *\([0-9]*\))/\1/p' bridge/CMakeLists.txt)
[ "$pre" != 0 ] || pre=""
full="$version${pre:+-pre$pre}"

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

name="moza-farmsim-link-$full-windows"
stage="dist/$name"
rm -rf "$stage" "dist/$name.zip"
mkdir -p "$stage"
cp bridge/build-win/moza_fs_link.exe bridge/build-win/moza_fs_bridge.exe fs-mod/dist/FS25_MozaYokeLink.zip "$stage/"
cp docs/INSTALL.md "$stage/INSTALL.md"
cp LICENSE "$stage/LICENSE.txt"
(cd dist && zip -q -r "$name.zip" "$name")

# The installer: the same files, plus a first-run page that takes MOZA's SDK zip from the user.
setup="dist/moza-farmsim-link-$full-setup.exe"
rm -f "$setup"
# makensis aborts with bad_alloc while building its language tables when no locale is set.
LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$full" -DNUMVERSION="$version" -DSOURCE="$PWD/$stage" -DOUTFILE="$PWD/$setup" installer/moza-farmsim-link.nsi
rm -rf "$stage"

# The mod on its own, for the other players in a multiplayer game: they need the same file and
# nothing else. It keeps its name, because the game takes a mod's name from its zip.
cp fs-mod/dist/FS25_MozaYokeLink.zip dist/FS25_MozaYokeLink.zip

echo "built dist/$name.zip:"
unzip -l "dist/$name.zip"
echo "built $setup: $(wc -c < "$setup" | tr -d ' ') bytes"
echo "built dist/FS25_MozaYokeLink.zip: $(wc -c < dist/FS25_MozaYokeLink.zip | tr -d ' ') bytes"
