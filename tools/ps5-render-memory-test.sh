#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/app/ps5/build/host-tests
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -D__PROSPERO__ -I"$root" \
    "$root/tests/ps5_render_memory_tests.cpp" -o "$out/ps5-render-memory-tests"
"$out/ps5-render-memory-tests"
