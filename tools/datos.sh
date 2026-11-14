#!/bin/sh
# Download the SingleStepTests/m68000 test vectors into tests/datos/ and
# convert them to JSON.
#
# Source: https://github.com/SingleStepTests/m68000 (MIT License, generated
# with MAME's microcoded 68000 core). The data is downloaded when testing and
# never committed (tests/datos/ is in .gitignore).
#
# Layout after running:
#   tests/datos/m68000-src/      shallow clone of the upstream repository
#   tests/datos/m68000/*.json    one JSON file per instruction group
#
# Upstream ships binary files (v1/*.json.bin); tools/sst2json.py turns each
# one into a JSON array of tests, one test per line.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DATA="$ROOT/tests/datos"
SRC="$DATA/m68000-src"
OUT="$DATA/m68000"
URL=https://github.com/SingleStepTests/m68000.git

mkdir -p "$DATA"
if [ -d "$SRC/.git" ]; then
    echo "Updating $SRC"
    git -C "$SRC" pull --ff-only --quiet
else
    echo "Cloning $URL"
    git clone --depth 1 --quiet "$URL" "$SRC"
fi

# The license must still be MIT; stop if upstream changed it.
if ! head -n 1 "$SRC/LICENSE" | grep -qx 'MIT License'; then
    echo "error: $SRC/LICENSE is not the MIT License any more; check it by hand" >&2
    exit 1
fi
echo "License: MIT ($(sed -n 3p "$SRC/LICENSE"))"

mkdir -p "$OUT"
n=0
for bin in "$SRC"/v1/*.json.bin; do
    name=$(basename "$bin" .bin)
    if [ ! -f "$OUT/$name" ] || [ "$bin" -nt "$OUT/$name" ]; then
        python3 -I "$ROOT/tools/sst2json.py" "$bin" "$OUT/$name.tmp"
        mv "$OUT/$name.tmp" "$OUT/$name"
    fi
    n=$((n + 1))
done
echo "$n files in $OUT"
