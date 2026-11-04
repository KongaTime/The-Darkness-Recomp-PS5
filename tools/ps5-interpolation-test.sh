#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests/interpolation"
mkdir -p "$out"
for shader in fg_luma fg_flow fg_cut fg_warp; do
    glslangValidator -V --target-env vulkan1.1 "$root/renderer/ps5/shaders/$shader.comp" -o "$out/$shader.spv"
    spirv-val --target-env vulkan1.1 "$out/$shader.spv"
done
python3 "$root/app/ps5/tools/spirv-header.py" "$out/ps5_shaders.h" \
    "kFgLuma=$out/fg_luma.spv" "kFgFlow=$out/fg_flow.spv" "kFgCut=$out/fg_cut.spv" "kFgWarp=$out/fg_warp.spv"
clang++ -std=c++20 -O2 -g -Wall -Wextra -Wno-missing-field-initializers \
    -I"$root" -I"$out" -I"$root/../PS5_Mesa/include" \
    "$root/renderer/ps5/frame_interpolation.cpp" "$root/tests/ps5_interpolation_tests.cpp" \
    -lvulkan -o "$out/interpolation-tests"
"$out/interpolation-tests"
