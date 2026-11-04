#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -I"$root" \
    "$root/tests/ps5_storage_path_tests.cpp" -o "$out/storage-path-tests"
"$out/storage-path-tests"
