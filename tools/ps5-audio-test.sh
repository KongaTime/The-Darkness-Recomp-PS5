#!/usr/bin/env bash
# Build and run the console audio path's host test.
#
#   tools/ps5-audio-test.sh [output-directory]
#
# The PS5 title links runtime/native/audio_ps5_driver.cpp and audio_ps5.cpp:
# the guest's render-driver client, the SceAudioOut feed thread and the 5.1 ->
# stereo downmix. None of that can run under the Windows tests, which link the
# XAudio2 driver instead. This builds the console translation units and scripts
# the console's SceAudioOut API, so the callback handoff, conversion and queue
# are checked without a device.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}
generated=$root/build_native/generated

[[ -f $generated/ppc_config.h ]] ||
    { printf 'error: no AOT output in %s\n' "$generated" >&2; exit 2; }

mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread '-D__declspec(x)=' \
    -I"$root" -I"$generated" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_audio_tests.cpp" \
    "$root/runtime/native/audio_ps5_driver.cpp" \
    "$root/runtime/native/audio_ps5.cpp" \
    -o "$out/ps5-audio-tests"

"$out/ps5-audio-tests"
