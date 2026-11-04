#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests"
mkdir -p "$out"
for cxx in g++ clang++; do
    extra=()
    if [[ $cxx == g++ ]]; then extra=(-fno-tree-loop-distribute-patterns); fi
    "$cxx" -std=c++20 -O2 -Wall -Wextra "${extra[@]}" \
        "$root/app/ps5/fast_memory.cpp" "$root/tests/ps5_fast_memory_tests.cpp" \
        -o "$out/fast-memory-$cxx"
    "$out/fast-memory-$cxx"
    "$cxx" -std=c++20 -O2 "${extra[@]}" -c "$root/app/ps5/fast_memory.cpp" -o "$out/fast-memory-$cxx.o"
    if nm -u "$out/fast-memory-$cxx.o" | grep -E '\b(memcpy|memmove|memset)$'; then
        echo "error: memory wrapper recursively calls an imported memory function" >&2
        exit 1
    fi
done
