#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests"
mkdir -p "$out"
for cxx in g++ clang++; do
    "$cxx" -std=c++20 -O2 -Wall -Wextra "$root/app/ps5/fast_memcmp.cpp" \
        "$root/tests/ps5_fast_memcmp_tests.cpp" -o "$out/fast-memcmp-$cxx"
    "$out/fast-memcmp-$cxx"
    "$cxx" -std=c++20 -O2 -c "$root/app/ps5/fast_memcmp.cpp" -o "$out/fast-memcmp-$cxx.o"
    if nm -u "$out/fast-memcmp-$cxx.o" | grep -E '\b(memcmp|bcmp)$'; then
        echo 'error: comparison wrapper recursively calls an imported comparison' >&2
        exit 1
    fi
done
