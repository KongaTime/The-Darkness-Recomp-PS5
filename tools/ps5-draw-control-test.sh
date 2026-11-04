#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/app/ps5/build/host-tests
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -pthread -I"$root" "$root/tests/draw_control_allocator_tests.cpp" -o "$out/draw-control-tests"
"$out/draw-control-tests"
