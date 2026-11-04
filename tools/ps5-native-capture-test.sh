#!/usr/bin/env bash
# Run the complete capture contract with PS5's inline reads and draw-control
# allocator selected in the two changed capture translation units. The host
# fixture still supplies the full committed guest mapping and platform stubs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests/native-capture"
mkdir -p "$out"
common=(-std=c++20 -O1 -g -w -include bits/stdc++.h -mssse3
    -fsanitize=address,undefined -fno-omit-frame-pointer '-D__declspec(x)='
    -D__PROSPERO__=1 -DDARK_TEST_NATIVE_CAPTURE=1 -DDARK_TEST_VERTEX_PLAN_CACHE=1
    -DDARK_TEST_STORED_LOCK=1 -I"$root" -I"$root/renderer/engine"
    -I"$root/build_native/generated" -I"$root/build_native/shaders"
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde")
clang++ "${common[@]}" -c "$root/renderer/engine/world_mesh.cpp" -o "$out/world.o"
clang++ "${common[@]}" -c "$root/renderer/engine/engine_vertex_program.cpp" -o "$out/vertex.o"
wrapper="$out/compiler.sh"
cat > "$wrapper" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
out=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
args=()
for arg in "$@"; do
    case "$arg" in
        */renderer/engine/world_mesh.cpp) args+=("$out/world.o");;
        */renderer/engine/engine_vertex_program.cpp) args+=("$out/vertex.o");;
        *) args+=("$arg");;
    esac
done
exec clang++ -DDARK_TEST_NATIVE_CAPTURE=1 -DDARK_TEST_VERTEX_PLAN_CACHE=1 -DDARK_TEST_STORED_LOCK=1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer "${args[@]}"
WRAPPER
chmod +x "$wrapper"
CXX="$wrapper" bash "$root/tools/ps5-capture-layer-test.sh" "$out/capture"
