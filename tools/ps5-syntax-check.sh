#!/usr/bin/env bash
# Syntax-check the PS5 port's sources with the console toolchain.
#
# The Windows build compiles these files through the MSVC ClangCL toolset.
# This checks the same files against the PS5 SDK's clang, so a Win32 call that
# survived the port shows up here rather than at link time on the console.
#
# Usage: tools/ps5-syntax-check.sh [file ...]
#   With no arguments, checks every source the PS5 title builds.
set -uo pipefail

SDK=${PS5_SDK:-$HOME/ps5-sdk}
CXX="$SDK/bin/prospero-clang++"
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

if [ $# -eq 0 ]; then
    set -- \
        runtime/kernel/kernel_imports.cpp \
        runtime/kernel/host_filesystem.cpp \
        runtime/kernel/host_filesystem_posix.cpp \
        runtime/native/input_ps5.cpp \
        runtime/native/audio_ps5.cpp
fi

status=0
for file in "$@"; do
    if out=$("$CXX" -std=c++20 -fsyntax-only \
            -I. -Ibuild_native/generated \
            -Irefs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde \
            "$file" 2>&1); then
        errors=$(grep -c 'error:' <<<"$out")
        printf '%-52s %s\n' "$file" "ok (${errors} errors)"
        grep 'error:' <<<"$out" || true
        [ "$errors" -eq 0 ] || status=1
    else
        printf '%-52s FAILED\n' "$file"
        grep 'error:' <<<"$out" | head -20
        status=1
    fi
done
exit $status
