#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/app/ps5/build/host-tests
mkdir -p "$out"
python3 "$root/tests/test_video_menu.py"
cxx=${CXX:-g++}
"$cxx" -std=c++20 -O1 -I"$root" "$root/tests/ps5_video_preferences_tests.cpp" \
    "$root/app/ps5/video_preferences.cpp" "$root/runtime/native/graphics_settings.cpp" -o "$out/video-preferences-tests"
"$out/video-preferences-tests" "$out/video-test.ini"
"$cxx" -std=c++20 -O1 -I"$root" "$root/tests/ps5_render_fence_tests.cpp" -o "$out/render-fence-tests"
"$out/render-fence-tests"
