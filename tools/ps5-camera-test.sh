#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/app/ps5/build/host-tests
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -I"$root" \
    "$root/tests/camera_interpolation_tests.cpp" "$root/runtime/native/camera_interpolation.cpp" \
    -o "$out/camera-interpolation-tests"
"$out/camera-interpolation-tests" "$@"
