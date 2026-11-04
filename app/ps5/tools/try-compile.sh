#!/usr/bin/env bash
# Compile one source file the way the PS5 title build does, without linking,
# and print the first errors. A porting aid: app/ps5/tools/try-compile.sh FILE [N]
set -uo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
app=$(dirname "$here")
root=$(cd -- "$app/../.." && pwd)
sdk=${PS5_SDK:-$HOME/ps5-sdk}
file=$1
count=${2:-40}
"$sdk/bin/prospero-clang++" --sysroot="$sdk/target" -DDARK_NATIVE_SSSE3=1 \
    -I"$app" -I"$root" -I"$root/renderer/ps5" -I"$app/build/shaders" -I"$app/build/world-shaders" \
    -I"$root/build_native/shaders" -I"$root/build_native/generated" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" -I"$root/../PS5_Mesa/include" \
    -I"$app/build/native_assets" -I"$app/win32" -I"$root/build_native/deps/ffmpeg-darkxma-ps5/include" \
    -O0 -std=gnu++20 -fPIE -mssse3 -fsyntax-only -ferror-limit=0 "$file" 2>&1 |
    grep -E "error:" | sed "s#$root/##" | sort | uniq | head -n "$count"
