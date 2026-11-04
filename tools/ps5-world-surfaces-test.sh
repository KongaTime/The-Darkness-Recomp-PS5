#!/usr/bin/env bash
# Build and run the console world-surface policy's host test.
#
#   tools/ps5-world-surfaces-test.sh [output-directory]
#
# renderer/ps5/world_surfaces.cpp allocates the engine's render targets on the
# console, clears them and copies a resolved surface into the guest's texture.
# The rules it follows -- growth, the resolve rectangle and its fringe, which
# guest format is which texture -- are WorldRendererD3D11's, and live in
# world_surface_policy.h free of Vulkan so they can be checked here rather than
# on the console.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}

mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra \
    -I"$root/renderer/ps5" \
    "$root/tests/world_surface_policy_tests.cpp" \
    -o "$out/world-surface-policy-tests"

"$out/world-surface-policy-tests"

"$cxx" -std=c++20 -O1 -g -ffunction-sections -fdata-sections '-D__declspec(x)=' \
    -I"$root" -I"$root/app/ps5/build/shaders" -I"$root/build_native/generated" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_world_surface_clear_tests.cpp" -Wl,--gc-sections -o "$out/world-surface-clear-tests"
"$out/world-surface-clear-tests"

"$cxx" -std=c++20 -O1 -g -ffunction-sections -fdata-sections '-D__declspec(x)=' \
    -I"$root" -I"$root/app/ps5/build/shaders" -I"$root/build_native/generated" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_world_pass_batch_tests.cpp" -Wl,--gc-sections -o "$out/world-pass-batch-tests"
"$out/world-pass-batch-tests"
