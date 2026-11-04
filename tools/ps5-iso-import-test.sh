#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$root/work/iso-import-tests}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$root" \
    "$root/tests/ps5_iso_import_tests.cpp" "$root/app/ps5/iso_import.cpp" -o "$out/iso-import-tests"
"$out/iso-import-tests"
