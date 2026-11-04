#!/usr/bin/env bash
# Build and run the console capture layer's host test.
#
#   tools/ps5-capture-layer-test.sh [output-directory]
#
# The console title links renderer/engine/simple_mesh.cpp and its decoders and
# consumes their frames with takePreviewFrame. That code has a Windows branch
# (SEH-guarded guest reads, D3D11 preview) and the console branch; the Windows
# test suite only covers the first. This builds the same translation units with
# the console branch selected and drives a synthetic UI draw and movie frame
# through the producer/consumer handshake.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
out=${1:-$root/app/ps5/build/host-tests}
generated=$root/build_native/generated
shaders=$root/build_native/shaders

[[ -f $generated/sources.cmake ]] ||
    { printf 'error: no AOT output in %s\n' "$generated" >&2; exit 2; }
[[ -f $shaders/engine_world_fragment_bindings.generated.h ]] ||
    { printf 'error: no engine material tables in %s (build app/ps5 first)\n' "$shaders" >&2; exit 2; }

mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++20 -O1 -w '-D__declspec(x)=' \
    -I"$root" -I"$root/renderer/engine" -I"$generated" -I"$shaders" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_capture_layer_tests.cpp" \
    "$root/renderer/engine/simple_mesh.cpp" \
    "$root/renderer/engine/stored_geometry.cpp" \
    "$root/renderer/engine/world_mesh.cpp" \
    "$root/renderer/engine/scene_work.cpp" \
    "$root/renderer/engine/engine_transforms.cpp" \
    "$root/renderer/engine/engine_texture_constants.cpp" \
    "$root/renderer/engine/engine_vertex_descriptor.cpp" \
    "$root/renderer/engine/engine_palette.cpp" \
    "$root/renderer/engine/engine_vertex_program.cpp" \
    "$root/renderer/ps5/png_writer.cpp" \
    "$root/runtime/native/input_ps5.cpp" \
    -o "$out/ps5-capture-layer-tests"

png=$out/frame-dump-test.png
rm -f "$png"
"$out/ps5-capture-layer-tests" "$png"

# The writer has no decoder to check itself against, so the file it produced is
# decoded here: the deflate stream, every chunk CRC and every pixel.
python3 - "$png" <<'PY'
import struct, sys, zlib

data = open(sys.argv[1], 'rb').read()
assert data[:8] == b'\x89PNG\r\n\x1a\n', 'bad signature'
pos, idat, size = 8, b'', None
while pos < len(data):
    length = struct.unpack('>I', data[pos:pos+4])[0]
    kind = data[pos+4:pos+8]
    payload = data[pos+8:pos+8+length]
    crc = struct.unpack('>I', data[pos+8+length:pos+12+length])[0]
    assert zlib.crc32(kind + payload) & 0xffffffff == crc, f'bad CRC in {kind!r}'
    if kind == b'IHDR':
        width, height, depth, colour = struct.unpack('>IIBB', payload[:10])
        assert (depth, colour) == (8, 2), f'IHDR is {depth}-bit colour {colour}'
        size = (width, height)
    elif kind == b'IDAT':
        idat += payload
    elif kind == b'IEND':
        break
    pos += 12 + length
raw = zlib.decompress(idat)
stride = size[0] * 3 + 1
assert len(raw) == stride * size[1], 'decoded size does not match the header'
for y in range(size[1]):
    row = raw[y*stride:(y+1)*stride]
    assert row[0] == 0, 'unexpected filter byte'
    for x in range(size[0]):
        expected = (x * 40 & 255, y * 80 & 255, (255 - x * 10) & 255)
        assert tuple(row[1 + x*3:4 + x*3]) == expected, f'pixel {x},{y} differs'
print(f'PNG check: {size[0]}x{size[1]}, {size[0]*size[1]} pixels and every chunk CRC match')
PY
