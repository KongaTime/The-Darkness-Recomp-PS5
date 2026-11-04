#!/usr/bin/env bash
# Throwaway probe: can a title link RADV with PS5_Vulkan's recipe?
set -euo pipefail
work=/tmp/vkprobe
sdk=/home/Kai/ps5-sdk
vulkan=/mnt/c/kb/PS5_Vulkan
mkdir -p "$work"
cd "$work"

cat > probe.cpp <<'EOF'
#include <cstdio>
extern "C" int main() { std::printf("linked\n"); return 0; }
EOF

cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

"$work/../vkprobe" 2>/dev/null || true
"$sdk/bin/prospero-clang++" -std=c++20 -O2 -c probe.cpp -o probe.o

source "$vulkan/tools/radv-link.sh"
archive=$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
radv_link_recipe "$vulkan" "$sdk" "$archive"

echo "recipe: ${#radv_link_inputs[@]} inputs, ${#radv_link_flags[@]} flags"

"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$vulkan/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o llvm-pie.elf "$work/../probe-crt.o" probe.o \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so 2>&1 | head -20
ls -la llvm-pie.elf 2>/dev/null || echo "LINK FAILED"
