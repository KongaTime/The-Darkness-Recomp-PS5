#!/usr/bin/env python3
"""Build the world shader corpus the PS5 backend draws with.

The Windows world renderer compiles HLSL at runtime with D3DCompile. The console
has no HLSL compiler, so the same programs are compiled here, once, by glslang:

  * every distinct authored world vertex program (ProgramCache.xpc descriptor +
    the original VP.xrg template) in its three raster forms (no user clip,
    clip-distance, cull-distance);
  * every original fragment program in the catalog, with the same alpha-test
    (and bloom, where the Windows renderer adds it) wrapper the D3D11 renderer
    appends.

Bindings are shifted so one descriptor layout fits all programs: cbuffers
0..4, textures 16..31, samplers 32..47. The vertex stage keeps set 0 and the
fragment stage is moved to set 1, because both authored stages name binding 2
(the vertex viewport constants and the fragment alpha test) and one set cannot
hold two different buffers at one binding.

The output header is plain C++ arrays plus the lookup keys the renderer uses, so
the console never compiles a shader. It also carries each vertex variant's
attribute table -- which semantic glslang put at which location -- because the
template declares every input and only the used ones survive, so a location is
a property of the compiled program, not of the vertex struct.

    app/ps5/tools/world-shaders.py [--game-dir Darkness] [--force]

Needs python3, glslangValidator, and the game's System files (the same ones the
code generator and the Windows shader build use).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TEMPLATE_TOOL = ROOT / "tools" / "compile_world_template.py"
FRAGMENT_HEADER = ROOT / "build_native" / "shaders" / "engine_world_fragments.generated.h"

# Appended to the generated template by WorldVertexShaderD3D11 when it compiles
# a raster form; kept byte-identical to renderer/d3d11/world_renderer.cpp.
WRAPPER_CLIP = r"""
cbuffer NativeViewport : register(b2) {float4 nativeDepthRange;float4 nativePromptTransform;};
cbuffer NativeUserClip : register(b3) {float4 nativeClipPlanes[6];uint4 nativeClipControl;};
struct NativeRasterOutput {
    float4 position : SV_Position;
    float4 tex[8] : TEXCOORD0;
    float4 color : COLOR0;
#if NATIVE_USER_CLIP_CULL
    float4 distances0 : SV_CullDistance0;
    float2 distances1 : SV_CullDistance1;
#else
    float4 distances0 : SV_ClipDistance0;
    float2 distances1 : SV_ClipDistance1;
#endif
};
NativeRasterOutput rasterMain(VertexInput input) {
    VertexOutput result=vertexMain(input);
    NativeRasterOutput output;
    float distances[6];
    [unroll] for(uint plane=0;plane<6;++plane)
        distances[plane]=(nativeClipControl.x & (1u<<plane))?dot(result.position,nativeClipPlanes[plane]):1.0;
    output.distances0=float4(distances[0],distances[1],distances[2],distances[3]);
    output.distances1=float2(distances[4],distances[5]);
    if(nativePromptTransform.w != 0)
        result.position.xy=result.position.xy*nativePromptTransform.x+result.position.w*nativePromptTransform.yz;
    result.position.z=nativeDepthRange.x*result.position.w+(nativeDepthRange.y-nativeDepthRange.x)*result.position.z;
    output.position=result.position;
    [unroll] for(uint stage=0;stage<8;++stage)output.tex[stage]=result.tex[stage];
    output.color=result.color;
    return output;
}
"""

WRAPPER_PLAIN = r"""
cbuffer NativeViewport : register(b2) {float4 nativeDepthRange;float4 nativePromptTransform;};
VertexOutput rasterMain(VertexInput input) {
    VertexOutput result=vertexMain(input);
    if(nativePromptTransform.w != 0)
        result.position.xy=result.position.xy*nativePromptTransform.x+result.position.w*nativePromptTransform.yz;
    result.position.z=nativeDepthRange.x*result.position.w+(nativeDepthRange.y-nativeDepthRange.x)*result.position.z;
    return result;
}
"""

BLOOM_WRAPPER = r"""
cbuffer NativeBloom : register(b4) {float bloomRenderScale;uint bloomSnap;uint2 bloomPadding;};
float4 bloomPixelMain(Fragment input) {
    // Evaluate the original filter once per guest output texel. Its taps and
    // atlas clamps retain their original pitch even at 1440p and 4K.
    float4 dx=ddx(input.tex[0]),dy=ddy(input.tex[0]);
    if(bloomSnap) {
        float2 offset=(floor(input.position.xy/bloomRenderScale)+0.5)*bloomRenderScale-input.position.xy;
        input.tex[0]+=dx*offset.x+dy*offset.y;
    }
    return originalPixelMain(input);
}
#define originalPixelMain bloomPixelMain
"""

ALPHA_WRAPPER = r"""
#undef pixelMain
cbuffer NativeAlphaTest : register(b2) {uint alphaFunction;float alphaReference;uint2 alphaPadding;};
float4 pixelMain(Fragment input) : SV_Target {
    float4 color=originalPixelMain(input);
    bool alphaPasses=alphaFunction==8 ||
        (alphaFunction==2 && color.a<alphaReference) ||
        (alphaFunction==3 && color.a==alphaReference) ||
        (alphaFunction==4 && color.a<=alphaReference) ||
        (alphaFunction==5 && color.a>alphaReference) ||
        (alphaFunction==6 && color.a!=alphaReference) ||
        (alphaFunction==7 && color.a>=alphaReference);
    if(!alphaPasses)discard;
    return color;
}
"""

BLOOM_NAMES = {"XRUtil_ShrinkTexture8", "XREngine_GaussClamped"}
SHIFTS = ["--shift-cbuffer-binding", "0", "--shift-texture-binding", "16",
          "--shift-sampler-binding", "32"]

FRAGMENT_SOURCE_RE = re.compile(
    r'inline constexpr char (\w+)\[\] = R"DARKFP\((.*?)\)DARKFP";', re.S)
FRAGMENT_ENTRY_RE = re.compile(
    r'\{"([^"]+)", (\d+), (\d+), (\d+), (\w+)\}')


def fail(message: str) -> None:
    print(f"world shaders: {message}", file=sys.stderr)
    raise SystemExit(2)


def hash_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"{what} failed:\n{result.stdout}{result.stderr}")


def compile_hlsl(source: str, stage: str, entry: str, macros: list[str], out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    hlsl = out.with_suffix(".hlsl")
    hlsl.write_text(source)
    command = ["glslangValidator", "-D", "-V", "-S", stage, "-e", entry,
               "--source-entrypoint", entry, *SHIFTS]
    # -D alone selects HLSL; a macro must stay attached (-DNAME=VALUE).
    command += [f"-D{macro}" for macro in macros]
    command += [str(hlsl), "-o", str(out)]
    run(command, f"glslang {entry} ({out.name})")


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def world_vertex_options(descriptor_hex: str) -> dict:
    """Native::worldVertexOptions for an authored 20-byte descriptor key."""
    key = bytes.fromhex(descriptor_hex)
    if len(key) != 20:
        fail(f"authored descriptor is {len(key)} bytes, expected 20")
    data = key + bytes(80 - len(key))
    coordinate_mapping = be32(data, 0)
    flags = be32(data, 4)
    coordinates = [(coordinate_mapping >> (8 + 3 * slot)) & 7 for slot in range(8)]
    return {
        "weights": (flags >> 16) & 15,
        "positionConversion": bool(flags & 0x04000000),
        "normal": bool(flags & 0x00200000),
        "tangents": bool(flags & 0x00400000),
        "normalizeNormal": bool(flags & 0x00800000),
        "vertexColor": (flags & 0x01000000) == 0,
        "modes": list(data[8:16]),
        "coordinates": coordinates,
        "conversions": [bool(flags & (1 << coordinates[slot])) for slot in range(8)],
        "matrices": [bool(flags & (1 << (8 + slot))) for slot in range(8)],
    }


def shader_key(options: dict, clip: int) -> bytes:
    """Native::WorldVertexOptions as WorldVertexShaderD3D11::shaderKey encodes it."""
    key = bytearray([
        options["weights"],
        int(options["positionConversion"]),
        int(options["normal"]),
        int(options["tangents"]),
        int(options["normalizeNormal"]),
        int(options["vertexColor"]),
    ])
    for slot in range(8):
        key += bytes([options["modes"][slot], options["coordinates"][slot],
                      int(options["conversions"][slot]), int(options["matrices"][slot])])
    key.append(clip)
    return bytes(key)


def vertex_macros(options: dict, clip: int) -> list[str]:
    macros = [
        f"MWCOMP={options['weights']}",
        f"POSITION_TRANS={int(options['positionConversion'])}",
        f"USE_NORMAL={int(options['normal'])}",
        f"USE_TANGENTS={int(options['tangents'])}",
        f"NORMALIZE_NORMAL={int(options['normalizeNormal'])}",
        f"VERTEX_COLOR={int(options['vertexColor'])}",
    ]
    for slot in range(8):
        macros += [
            f"MODE_{slot}={options['modes'][slot]}",
            f"COORD_{slot}={options['coordinates'][slot]}",
            f"CONVERT_{slot}={int(options['conversions'][slot])}",
            f"MATRIX_{slot}={int(options['matrices'][slot])}",
        ]
    if clip:
        macros.append(f"NATIVE_USER_CLIP_CULL={1 if clip == 2 else 0}")
    return macros


def parse_fragments(header: Path) -> list[dict]:
    text = header.read_text()
    sources = {name: source for name, source in FRAGMENT_SOURCE_RE.findall(text)}
    fragments = []
    for name, flags, textures, cubes, variable in FRAGMENT_ENTRY_RE.findall(text):
        if variable not in sources:
            fail(f"fragment {name}:{flags} has no source variable {variable}")
        fragments.append({"name": name, "flags": int(flags),
                          "textures": int(textures), "cubes": int(cubes),
                          "source": sources[variable]})
    if not fragments:
        fail(f"no fragments found in {header}")
    return fragments


def spirv_words(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        fail(f"{path.name} is not a SPIR-V module")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != 0x07230203:
        fail(f"{path.name} has no SPIR-V magic")
    return words

OP_NAME = 5
OP_DECORATE = 71
DECORATION_DESCRIPTOR_SET = 34
DECORATION_LOCATION = 30

# The template's VertexInput members, in the order the vertex buffer holds
# them. Each member becomes one float4 attribute of Native::WorldVertex.
VERTEX_SEMANTICS = {"position": 0, "normal": 1}
for _slot in range(8):
    VERTEX_SEMANTICS[f"tex[{_slot}]"] = 2 + _slot
VERTEX_SEMANTICS.update({"color": 10, "indices": 11, "weights": 12,
                         "indices2": 13, "weights2": 14})


def spirv_name(words: list[int], target: int) -> str | None:
    """The OpName of one result id, or None when the module carries no name."""
    offset = 5
    while offset < len(words):
        word = words[offset]
        opcode, length = word & 0xFFFF, word >> 16
        if not length:
            fail("a SPIR-V instruction has no length")
        if opcode == OP_NAME and words[offset + 1] == target:
            data = struct.pack(f"<{length - 2}I", *words[offset + 2:offset + length])
            return data.split(b"\0", 1)[0].decode("utf-8", "replace")
        offset += length
    return None


def vertex_inputs(words: list[int]) -> list[tuple[int, int]]:
    """(semantic, location) for every surviving vertex input, by location."""
    locations: dict[int, int] = {}
    names: dict[int, str] = {}
    offset = 5
    while offset < len(words):
        word = words[offset]
        opcode, length = word & 0xFFFF, word >> 16
        if not length:
            fail("a SPIR-V instruction has no length")
        if opcode == OP_DECORATE and length >= 4 and words[offset + 2] == DECORATION_LOCATION:
            locations[words[offset + 1]] = words[offset + 3]
        elif opcode == OP_NAME and length >= 3:
            data = struct.pack(f"<{length - 2}I", *words[offset + 2:offset + length])
            names[words[offset + 1]] = data.split(b"\0", 1)[0].decode("utf-8", "replace")
        offset += length
    inputs = []
    for target, location in locations.items():
        name = names.get(target, "")
        if not name.startswith("input."):
            continue
        member = name[len("input."):]
        if member not in VERTEX_SEMANTICS:
            fail(f"vertex input {name} is not a template member")
        inputs.append((VERTEX_SEMANTICS[member], location))
    if not inputs:
        fail("a vertex module declares no inputs")
    inputs.sort(key=lambda entry: entry[1])
    return inputs


def move_to_set(words: list[int], descriptor_set: int) -> None:
    """Rewrite every DescriptorSet decoration in place."""
    moved = 0
    offset = 5
    while offset < len(words):
        word = words[offset]
        opcode, length = word & 0xFFFF, word >> 16
        if not length:
            fail("a SPIR-V instruction has no length")
        if (opcode == OP_DECORATE and length >= 4 and
                words[offset + 2] == DECORATION_DESCRIPTOR_SET):
            words[offset + 3] = descriptor_set
            moved += 1
        offset += length
    if not moved:
        fail("a module with resources has no DescriptorSet decoration")


def fragment_texture_mask(words: list[int]) -> int:
    """Only textures that survive compilation are required at draw time."""
    mask = 0
    offset = 5
    while offset < len(words):
        opcode, length = words[offset] & 0xFFFF, words[offset] >> 16
        if not length or offset + length > len(words):
            fail("a SPIR-V instruction has an invalid length")
        # Binding decoration 33; textures were shifted into bindings 16..31.
        if opcode == OP_DECORATE and length >= 4 and words[offset + 2] == 33:
            binding = words[offset + 3]
            if 16 <= binding < 32:
                mask |= 1 << (binding - 16)
        offset += length
    return mask


def emit_header(out: Path, vertices: list[dict], fragments: list[dict],
                template_hash: str) -> None:
    lines = [
        "// Generated by app/ps5/tools/world-shaders.py. Do not edit.",
        "//",
        "// The world renderer's shader corpus. The Windows renderer compiles",
        "// these HLSL programs at runtime; the console cannot, so every authored",
        "// program is compiled here, once, and selected by the same keys.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace DarkRecomp::Ps5::WorldShaders {",
        "",
        f"inline constexpr char kSourceHash[] = \"{template_hash}\";",
        f"inline constexpr uint32_t kVertexVariantCount = {len(vertices)};",
        f"inline constexpr uint32_t kFragmentVariantCount = {len(fragments)};",
        "",
        "struct VertexVariant { const uint8_t* key; const uint32_t* code; uint32_t words;",
        "                       const uint8_t (*inputs)[2]; uint32_t inputCount; };",
        "struct FragmentVariant { const char* name; uint32_t flags, textures, cubes;",
        "                          const uint32_t* code; uint32_t words; };",
        "",
    ]

    for index, variant in enumerate(vertices):
        words = variant["words"]
        inputs = ",".join(f"{{{semantic},{location}}}" for semantic, location in variant["inputs"])
        lines.append(f"inline constexpr uint8_t kVertexKey{index}[39] = {{"
                     + ",".join(str(b) for b in variant["key"]) + "};")
        lines.append(f"inline constexpr uint32_t kVertexCode{index}[] = {{"
                     + ",".join(f"{word:#x}" for word in words) + "};")
        lines.append(f"inline constexpr uint8_t kVertexInputs{index}[][2] = {{{inputs}}};")
        lines.append("")

    lines.append("inline constexpr VertexVariant kVertexVariants[] = {")
    for index, variant in enumerate(vertices):
        lines.append(f"    {{kVertexKey{index}, kVertexCode{index}, {len(variant['words'])},"
                     f" kVertexInputs{index}, {len(variant['inputs'])}}},")
    lines.append("};")
    lines.append("")

    for index, variant in enumerate(fragments):
        words = variant["words"]
        lines.append(f"inline constexpr uint32_t kFragmentCode{index}[] = {{"
                     + ",".join(f"{word:#x}" for word in words) + "};")
        lines.append("")

    lines.append("inline constexpr FragmentVariant kFragmentVariants[] = {")
    for index, variant in enumerate(fragments):
        lines.append(f'    {{"{variant["name"]}", {variant["flags"]}, {variant["textures"]}, '
                     f'{variant["cubes"]}, kFragmentCode{index}, {len(variant["words"])}}},')
    lines.append("};")
    lines.append("")
    lines.append("} // namespace DarkRecomp::Ps5::WorldShaders")
    lines.append("")

    out.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", type=Path, default=ROOT / "Darkness")
    parser.add_argument("--output", type=Path, default=ROOT / "app" / "ps5" / "build" / "world-shaders")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    if not (args.game_dir / "System" / "Gl" / "VP.xrg").is_file():
        fail(f"no game System files in {args.game_dir}")
    if not FRAGMENT_HEADER.is_file():
        fail(f"no fragment catalog at {FRAGMENT_HEADER}; run "
             f"python3 tools/compile_world_fragment.py --game-dir {args.game_dir} "
             f"--output-dir build_native/shaders")
    if not TEMPLATE_TOOL.is_file():
        fail(f"no template generator at {TEMPLATE_TOOL}")

    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    header = out / "world_shaders.generated.h"
    stamp_path = out / "world_shaders.stamp.json"

    print("world shaders: generating the original vertex template")
    run([sys.executable, str(TEMPLATE_TOOL), "--game-dir", str(args.game_dir),
         "--output-dir", str(out / "template")], "world vertex template")
    template_path = out / "template" / "engine_world_template.hlsl"
    manifest_path = out / "template" / "engine_world_template.source.json"
    template = template_path.read_text()
    manifest = json.loads(manifest_path.read_text())
    template_hash = hash_file(template_path)
    fragment_hash = hash_file(FRAGMENT_HEADER)

    stamp = {"template": template_hash, "fragments": fragment_hash,
             "shift": SHIFTS, "version": 4}
    if (not args.force and header.is_file() and stamp_path.is_file()
            and json.loads(stamp_path.read_text()) == stamp):
        print(f"world shaders: unchanged, keeping {header.name}")
        return 0

    descriptors = manifest["vertex_program_descriptors"]
    options = {}
    for descriptor in descriptors:
        decoded = world_vertex_options(descriptor)
        options[tuple(json.dumps(decoded, sort_keys=True))] = decoded
    ordered = [options[key] for key in sorted(options)]
    print(f"world shaders: {len(descriptors)} authored programs, "
          f"{len(ordered)} distinct option sets")

    vertices = []
    for options_index, option in enumerate(ordered):
        for clip in range(3):
            source = template + (WRAPPER_CLIP if clip else WRAPPER_PLAIN)
            if clip and clip == 2:
                source = source.replace("NATIVE_USER_CLIP_CULL", "1")
            elif clip:
                source = source.replace("NATIVE_USER_CLIP_CULL", "0")
            name = f"vertex-{options_index:03}-{clip}"
            spv = out / "spv" / f"{name}.spv"
            compile_hlsl(source, "vert", "rasterMain", vertex_macros(option, clip), spv)
            words = spirv_words(spv)
            vertices.append({"key": shader_key(option, clip), "words": words,
                             "inputs": vertex_inputs(words)})
        if options_index % 25 == 0:
            print(f"world shaders: vertex {options_index + 1}/{len(ordered)}")

    fragments = []
    for fragment in parse_fragments(FRAGMENT_HEADER):
        source = "#define pixelMain originalPixelMain\n" + fragment["source"]
        if fragment["name"] in BLOOM_NAMES:
            source += BLOOM_WRAPPER
        source += ALPHA_WRAPPER
        name = f"fragment-{fragment['name']}-{fragment['flags']}"
        spv = out / "spv" / f"{name}.spv"
        compile_hlsl(source, "frag", "pixelMain", [], spv)
        words = spirv_words(spv)
        # The fragment stage owns set 1: its binding 2 (the alpha test) and the
        # vertex stage's binding 2 (the viewport constants) are different buffers.
        move_to_set(words, 1)
        live_textures = fragment_texture_mask(words)
        if live_textures & ~fragment["textures"]:
            fail(f"compiled texture bindings exceed the authored mask: {name}")
        fragment["textures"] = live_textures
        fragment["cubes"] &= live_textures
        spv.write_bytes(struct.pack(f"<{len(words)}I", *words))
        fragment["words"] = words
        fragments.append(fragment)

    emit_header(header, vertices, fragments, template_hash)
    stamp_path.write_text(json.dumps(stamp, indent=2) + "\n")
    (out / "world_shaders.manifest.json").write_text(json.dumps({
        "template": template_hash,
        "authoredDescriptors": len(descriptors),
        "vertexOptionSets": len(ordered),
        "vertexVariants": len(vertices),
        "fragmentVariants": len(fragments),
        "bindings": {"cbufferShift": 0, "textureShift": 16, "samplerShift": 32,
                     "vertexSet": 0, "fragmentSet": 1},
    }, indent=2) + "\n")
    print(f"world shaders: {len(vertices)} vertex and {len(fragments)} fragment "
          f"modules -> {header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
