// The console's world shader corpus, checked against its lookup.
//
// app/ps5/tools/world-shaders.py compiles the authored world vertex programs
// and the original fragment catalog to SPIR-V. This test includes the same
// generated header and proves renderer/ps5/world_shaders.cpp addresses every
// module by the key the world renderer will use, and rejects a variant the
// game never authored.
#include "world_shaders.h"
#include "world_shaders.generated.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <array>
#include <string>

namespace {

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

bool isSpirv(const uint32_t* words, uint32_t count) {
    return words && count >= 5 && words[0] == 0x07230203u;
}

} // namespace

int main() {
    using namespace DarkRecomp::Ps5;
    try {
        require(worldVertexShaderCount() == WorldShaders::kVertexVariantCount,
                "the vertex count differs from the generated table");
        require(worldFragmentShaderCount() == WorldShaders::kFragmentVariantCount,
                "the fragment count differs from the generated table");
        require(worldVertexShaderCount() > 0 && worldFragmentShaderCount() > 0,
                "the corpus is empty");

        for (const WorldShaders::VertexVariant& variant : WorldShaders::kVertexVariants) {
            const WorldShaderModule module = findWorldVertexShader(variant.key);
            require(module.words == variant.code && module.wordCount == variant.words,
                    "a vertex variant is not found by its key");
            require(isSpirv(module.words, module.wordCount), "a vertex module is not SPIR-V");
            require(variant.key[38] <= 2, "a vertex key has an invalid raster form");

            // The draw layer builds its vertex layout from this table: every
            // attribute the program keeps must be found, at a location the
            // module declares, and the position must always be one of them.
            const int index = findWorldVertexVariant(variant.key);
            require(index >= 0, "a vertex variant has no index");
            const WorldShaderModule byIndex = worldVertexShaderAt(uint32_t(index));
            require(byIndex.words == variant.code, "a vertex module is not found by its index");
            const WorldVertexInputs inputs = findWorldVertexInputs(variant.key);
            require(inputs.count == variant.inputCount && inputs.count > 0,
                    "a vertex input table differs from the generated one");
            bool hasPosition = false;
            uint32_t previous = 0;
            for (uint32_t input = 0; input < inputs.count; ++input) {
                const uint32_t semantic = inputs.entries[input][0];
                const uint32_t location = inputs.entries[input][1];
                require(semantic < 15, "a vertex semantic is outside the engine's vertex struct");
                require(location < 15, "a vertex location is outside the engine's vertex struct");
                if (input) require(location > previous, "vertex locations are not increasing");
                previous = location;
                if (semantic == 0) hasPosition = true;
            }
            require(hasPosition, "a vertex program lost its position input");
        }

        for (const WorldShaders::FragmentVariant& variant : WorldShaders::kFragmentVariants) {
            uint32_t textures = 0, cubes = 0;
            const WorldShaderModule module =
                findWorldFragmentShader(variant.name, variant.flags, &textures, &cubes);
            require(module.words == variant.code && module.wordCount == variant.words,
                    "a fragment variant is not found by its name and flags");
            const int fragmentIndex=findWorldFragmentVariant(variant.name,variant.flags);
            require(fragmentIndex>=0 && worldFragmentShaderAt(uint32_t(fragmentIndex)).words==module.words,
                    "fragment index selected a different module");
            require(textures == variant.textures && cubes == variant.cubes,
                    "a fragment's binding masks were not returned");
            require(isSpirv(module.words, module.wordCount), "a fragment module is not SPIR-V");
            uint32_t reflected = 0;
            for (uint32_t i = 5; i < module.wordCount;) {
                const uint32_t length = module.words[i] >> 16;
                require(length && i + length <= module.wordCount, "invalid SPIR-V instruction");
                if ((module.words[i] & 0xFFFFu) == 71 && length >= 4 && module.words[i + 2] == 33) {
                    const uint32_t binding = module.words[i + 3];
                    if (binding >= 16 && binding < 32) reflected |= 1u << (binding - 16);
                }
                i += length;
            }
            require(textures == reflected, "the draw requires textures removed by the compiler");
            require(!(cubes & ~textures), "a cube mask names an unused texture");
        }

        // The draw layer's fallbacks: a motion or lighting submission with no
        // captured name, and the shadow projector the depth pass draws through.
        require(bool(findWorldFragmentShader("XRShader_MotionMap", 0)), "the motion fallback is missing");
        uint32_t lightingTextures = 0;
        require(bool(findWorldFragmentShader("XRShader_FP20_NDSEATP", 0, &lightingTextures)),
                "the lighting material is missing");
        require(lightingTextures == 7, "unused lighting attributes still require slots 3, 4 or 8");
        require(bool(findWorldFragmentShader("XRShader_FP20_NDSP", 0)), "the lighting fallback is missing");
        require(bool(findWorldFragmentShader("XREngine_ShadowProj", 8)), "the shadow projector is missing");

        // Independent linear oracle: one-byte key changes include both real
        // authored variants and misses. Hash collisions must not change either.
        for(const auto& variant:WorldShaders::kVertexVariants)for(unsigned byte=0;byte<39;++byte) {
            std::array<uint8_t,39> changed;std::memcpy(changed.data(),variant.key,39);changed[byte]^=1;
            int expected=-1;
            for(unsigned i=0;i<WorldShaders::kVertexVariantCount;++i)
                if(std::memcmp(WorldShaders::kVertexVariants[i].key,changed.data(),39)==0) {expected=int(i);break;}
            require(findWorldVertexVariant(changed.data())==expected,"indexed vertex lookup differs from the linear oracle");
        }
        for(const auto& variant:WorldShaders::kFragmentVariants) {
            std::string borrowed(variant.name);
            require(findWorldFragmentVariant(borrowed,variant.flags)>=0,"borrowed fragment name did not match");
            borrowed+="#";
            require(findWorldFragmentVariant(borrowed,variant.flags)<0,"modified fragment name reused a stale index");
        }
        uint32_t untouchedTextures=0x1234,untouchedCubes=0x5678;
        require(!worldFragmentShaderAt(UINT32_MAX,&untouchedTextures,&untouchedCubes) &&
                untouchedTextures==0x1234 && untouchedCubes==0x5678,"out-of-range fragment index changed outputs");
        require(!worldVertexShaderAt(UINT32_MAX) && !worldVertexInputsAt(UINT32_MAX),"out-of-range vertex index was accepted");
        const uint8_t unknown[39] = {0xFF};
        require(!findWorldVertexShader(unknown), "an unauthored vertex key was accepted");
        require(!findWorldFragmentShader("NoSuchProgram", 0), "an unknown fragment was accepted");
        require(!findWorldFragmentShader(WorldShaders::kFragmentVariants[0].name, 0xFFFFFFFFu),
                "a fragment with the wrong flags was accepted");
        require(!WorldShaderModule{}, "an empty module claims to be valid");

        std::printf("WorldShaders: %zu vertex forms and %zu fragment programs, every "
                    "module found by its render key and every vertex input by its location.\n",
                    worldVertexShaderCount(), worldFragmentShaderCount());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
