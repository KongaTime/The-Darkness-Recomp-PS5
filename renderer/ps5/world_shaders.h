#pragma once
// The world renderer's shaders, precompiled for the console.
//
// WorldRendererD3D11 compiles HLSL on first use. The console has no compiler, so
// app/ps5/tools/world-shaders.py compiles every authored vertex program (in its
// three raster forms) and every original fragment program once, at build time,
// and this layer selects them by the same keys the Windows renderer uses.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DarkRecomp::Ps5 {

struct WorldShaderModule {
    const uint32_t* words = nullptr;
    uint32_t wordCount = 0;
    explicit operator bool() const { return words != nullptr; }
};

// `key` is WorldVertexOptions encoded as WorldVertexShaderD3D11::shaderKey does:
// weights, positionConversion, normal, tangents, normalizeNormal, vertexColor,
// then eight slots of mode/coordinate/conversion/matrix, then the user-clip form
// (0 none, 1 clip distance, 2 cull distance). Returns an empty module for a
// variant the game's own program cache never authored.
struct WorldVertexInputs {
    // {semantic, location}: the semantic is the index into Native::WorldVertex's
    // fourteen float4 attributes (position 0, normal 1, tex 2..9, colour 10,
    // indices 11, weights 12, indices2 13, weights2 14); the location is what the
    // compiled module declares. Empty for a variant the game never authored.
    const uint8_t(*entries)[2] = nullptr;
    uint32_t count = 0;
    explicit operator bool() const { return entries != nullptr && count; }
};

WorldShaderModule findWorldVertexShader(const uint8_t key[39]);
WorldVertexInputs findWorldVertexInputs(const uint8_t key[39]);
// The variant's index in the generated table, or -1 when the game's program
// cache never authored it. The draw layer creates its shader modules by index.
int findWorldVertexVariant(const uint8_t key[39]);
WorldShaderModule worldVertexShaderAt(uint32_t index);
WorldVertexInputs worldVertexInputsAt(uint32_t index);
int findWorldFragmentVariant(std::string_view name,uint32_t flags);
WorldShaderModule worldFragmentShaderAt(uint32_t index,uint32_t* textures=nullptr,uint32_t* cubes=nullptr);

// The original fragment program plus the alpha-test and bloom wrappers the
// Windows renderer appends. `textures`/`cubes` receive the original binding
// masks (bit n set means texture/n in that class).
WorldShaderModule findWorldFragmentShader(std::string_view name, uint32_t flags,
                                          uint32_t* textures = nullptr, uint32_t* cubes = nullptr);

size_t worldVertexShaderCount();
size_t worldFragmentShaderCount();

} // namespace DarkRecomp::Ps5
