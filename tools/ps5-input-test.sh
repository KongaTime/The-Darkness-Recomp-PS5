#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/app/ps5/build/host-tests
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O2 -pthread -I"$root" \
    "$root/tests/ps5_pad_tests.cpp" "$root/runtime/native/input_ps5.cpp" \
    -o "$out/ps5-pad-tests"
"$out/ps5-pad-tests"
