#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests/upstream-capture"
mkdir -p "$out"
for cxx in g++ clang++; do
    for name in vertex_constant_ranges frame_resource_set thread_counters; do
        "$cxx" -std=c++20 -O2 -pthread -Wall -Wextra -I"$root" "$root/tests/${name}_tests.cpp" -o "$out/$name-$cxx"
        "$out/$name-$cxx"
    done
done
for name in vertex_constant_ranges frame_resource_set thread_counters; do
    clang++ -std=c++20 -O1 -g -pthread -fsanitize=address,undefined -I"$root" \
        "$root/tests/${name}_tests.cpp" -o "$out/$name-sanitized"
    "$out/$name-sanitized"
done
wrapper="$out/clang-capture.sh"
printf '#!/usr/bin/env bash\nexec clang++ -DDARK_TEST_VERTEX_PLAN_CACHE=1 -DDARK_TEST_STORED_LOCK=1 -DDARK_TEST_NATIVE_CAPTURE=1 -fsanitize=address,undefined -fno-omit-frame-pointer "$@"\n' > "$wrapper"
chmod +x "$wrapper"
CXX="$wrapper" bash "$root/tools/ps5-capture-layer-test.sh" "$out/capture"
