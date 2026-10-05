#!/usr/bin/env bash
# Build fs-mod/dist/FS25_MozaYokeLink.zip in the layout Farming Simulator 25
# expects: modDesc.xml and the other files at the root of the zip, not inside
# a folder. Copy the zip, unchanged and without renaming it, into
#   Documents\My Games\FarmingSimulator2025\mods\
#
#   ./package.sh             syntax check, run the tests, build the zip
#   ./package.sh --no-test   skip the checks (when Lua is not installed)
set -euo pipefail

cd "$(dirname "$0")"

MOD=FS25_MozaYokeLink
OUT="dist/$MOD.zip"

if [[ "${1:-}" != "--no-test" ]]; then
    if command -v luac >/dev/null 2>&1; then
        for f in "$MOD"/scripts/*.lua tests/*.lua; do
            luac -p "$f"
        done
        echo "luac -p: ok"
    else
        echo "luac not found, skipping the syntax check"
    fi
    if command -v lua >/dev/null 2>&1; then
        lua tests/run_tests.lua | tail -n 1
    else
        echo "lua not found, skipping the tests"
    fi
fi

for required in modDesc.xml icon_MozaYokeLink.dds scripts/MozaTelemetry.lua scripts/MozaYokeLink.lua; do
    [[ -f "$MOD/$required" ]] || { echo "missing $MOD/$required" >&2; exit 1; }
done

mkdir -p dist
rm -f "$OUT"
# -X: no extra file attributes, -D: no directory entries; hidden files stay out
(cd "$MOD" && zip -q -r -X -D -9 "../$OUT" . -x '.*' -x '*/.*')

# modDesc.xml has to be at the root of the archive
listing="$(unzip -Z1 "$OUT")"
grep -qx 'modDesc.xml' <<<"$listing" || { echo "modDesc.xml is not at the zip root" >&2; exit 1; }
if grep -q "^$MOD/" <<<"$listing"; then
    echo "files are nested in a $MOD/ folder inside the zip" >&2
    exit 1
fi

echo "built $OUT:"
unzip -l "$OUT"
