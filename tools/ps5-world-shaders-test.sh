#!/usr/bin/env bash
# Build the console's world shader corpus and check its lookup.
#
#   tools/ps5-world-shaders-test.sh [output-directory]
#
# Runs app/ps5/tools/world-shaders.py (cached by a stamp, so a repeat run is
# seconds) and then compiles the generated header with the lookup the renderer
# uses. Every generated module must be addressable by its render key.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}
corpus=$root/app/ps5/build/world-shaders

python3 "$root/app/ps5/tools/world-shaders.py" --output "$corpus"

mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra \
    -I"$root" -I"$root/app/ps5" -I"$root/renderer/ps5" -I"$corpus" \
    "$root/tests/world_shaders_tests.cpp" \
    "$root/renderer/ps5/world_shaders.cpp" \
    -o "$out/world-shaders-tests"

"$out/world-shaders-tests"
