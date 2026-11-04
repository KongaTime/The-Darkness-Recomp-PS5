#!/usr/bin/env bash
# The console's world draw path, checked on the host.
#
#   tools/ps5-world-draw-test.sh [output-directory]
#
# Three checks, none of which needs a PS5 SDK or a GPU:
#   * world_draw_policy.h -- the depth, stencil, blend, scissor and bias readings
#     against the Windows renderer's tables (tests/world_draw_policy_tests.cpp);
#   * the generated shader corpus -- every module and every vertex input located
#     (tests/world_shaders_tests.cpp, the same corpus the title builds);
#   * world_draw.cpp compiles against Vulkan headers with warnings enabled.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}
corpus=$root/app/ps5/build/world-shaders
cxx=${CXX:-g++}

mkdir -p "$out"

"$cxx" -std=c++20 -O1 -g -Wall -Wextra -I"$root/renderer/ps5" \
    "$root/tests/world_draw_policy_tests.cpp" -o "$out/world-draw-policy-tests"
"$out/world-draw-policy-tests"

"$here/ps5-world-shaders-test.sh" "$out"

"$cxx" -std=c++20 -fsyntax-only -Wall -Wextra '-D__declspec(x)=' \
    -I"$root" -I"$root/renderer/engine" -I"$root/renderer/ps5" \
    -I"$root/app/ps5/build/shaders" -I"$root/build_native/generated" \
    -I"$root/build_native/shaders" -I"$corpus" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/renderer/ps5/world_draw.cpp"
echo "world_draw.cpp: compiles"

"$cxx" -std=c++20 -O1 -g -ffunction-sections -fdata-sections '-D__declspec(x)=' \
    -I"$root" -I"$root/build_native/generated" -I"$root/build_native/shaders" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_world_memory_tests.cpp" -Wl,--gc-sections -o "$out/world-memory-tests"
"$out/world-memory-tests"
