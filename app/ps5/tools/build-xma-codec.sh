#!/usr/bin/env bash
# The Darkness on PS5 - the XMA decoder.
#
#   app/ps5/tools/build-xma-codec.sh [--rebuild]
#
# The native runtime decodes the guest's XMA audio with a pinned, minimal
# FFmpeg (libavcodec's xma1/xma2 decoders only), which tools/build_xma_codec.py
# downloads, verifies and patches for the Windows build. This script takes that
# same verified tree and cross-compiles it for the console as static archives:
#
#   build_native/deps/ffmpeg-darkxma-ps5/{include,lib/libavcodec-darkxma.a,lib/libavutil-darkxma.a}
#
# Needs the PS5 payload SDK (PS5_SDK, default $HOME/ps5-sdk) and python3.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
sdk=${PS5_SDK:-$HOME/ps5-sdk}
build=$root/build_native/deps/ffmpeg-darkxma-ps5-build
install=$root/build_native/deps/ffmpeg-darkxma-ps5

[[ -x $sdk/bin/prospero-clang ]] || { echo "error: no PS5 SDK at $sdk (set PS5_SDK)" >&2; exit 2; }

source=$(python3 "$root/tools/build_xma_codec.py" --source-only | tail -n 1)
[[ -x $source/configure ]] || { echo "error: no patched FFmpeg source at $source" >&2; exit 2; }

if [[ ${1:-} == --rebuild ]]; then rm -rf "$build"; fi
mkdir -p "$build"
cd "$build"

# Static archives for the title's link, compiled position-independent like the
# title itself. The decoder needs no threads, assembly or other library.
# prospero-clang is the SDK's compiler with the console target, sysroot and
# libraries, so configure's link tests produce console executables.
configure=("$source/configure" --prefix="$install" --enable-cross-compile --target-os=freebsd
    --arch=x86_64 --cc="$sdk/bin/prospero-clang" --ar="$sdk/bin/llvm-ar"
    --ranlib="$sdk/bin/llvm-ranlib" --nm="$sdk/bin/llvm-nm"
    --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-debug --disable-avdevice --disable-avfilter --disable-avformat --disable-swscale
    --disable-swresample --disable-asm --disable-pthreads
    --enable-decoder=xma1,xma2 --enable-avcodec --enable-avutil
    --enable-static --disable-shared --enable-pic --build-suffix=-darkxma)
stamp=$build/darkrecomp-configure.txt
if [[ ! -f $stamp ]] || [[ $(cat "$stamp") != "${configure[*]}" ]]; then
    "${configure[@]}"
    printf '%s' "${configure[*]}" > "$stamp"
fi
make -j"$(nproc)"
make install
echo "==> $install"
