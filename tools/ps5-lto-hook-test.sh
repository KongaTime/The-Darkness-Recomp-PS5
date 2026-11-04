#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests"
mkdir -p "$out"
for lto in 0 1; do
    for override in 0 1; do
        flags=(); sources=()
        if [[ $lto == 1 ]]; then flags=(-flto=thin -fuse-ld=lld -include "$root/app/ps5/lto_guest_calls.h"); fi
        if [[ $override == 1 ]]; then sources=("$root/tests/ps5_lto_hooks/override.cpp"); fi
        clang++ -std=c++20 -O2 -mssse3 -fPIE "${flags[@]}" "-DEXPECT_OVERRIDE=$override" '-D__declspec(x)=' \
            -I"$root" -I"$root/build_native/generated" \
            -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
            "$root/tests/ps5_lto_hooks/provider.cpp" "${sources[@]}" "$root/tests/ps5_lto_hooks/main.cpp" \
            -o "$out/lto-hooks-$lto-$override"
        "$out/lto-hooks-$lto-$override"
    done
done
