#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests"
mkdir -p "$out"
for cxx in g++ clang++; do
    "$cxx" -std=c++20 -O2 -Wall -Wextra -I"$root" \
        "$root/tests/ps5_resource_cache_trim_tests.cpp" -o "$out/resource-cache-$cxx"
    "$out/resource-cache-$cxx"
done
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$root" \
    "$root/tests/ps5_resource_cache_trim_tests.cpp" -o "$out/resource-cache-sanitized"
"$out/resource-cache-sanitized"
