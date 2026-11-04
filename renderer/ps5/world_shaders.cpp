#include "world_shaders.h"

#include "world_shaders.generated.h"
#include "renderer/engine/shader_lookup.h"

#include <array>
#include <cstring>

namespace DarkRecomp::Ps5 {
namespace {

namespace Generated = WorldShaders;
// Exact, inline comparison for the generated program names. Resolving a draw's
// fragment program compares its name against the last used variant every draw,
// and the out-of-line char_traits compare that reached was the caller of 2.46%
// of the render thread's memcmp samples (work/compare_sample_callers.py). The
// names are short, so word loads inline beat a libc call.
inline bool sameProgramName(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    const char* a = left.data();
    const char* b = right.data();
    size_t size = left.size();
    for (; size >= 8; a += 8, b += 8, size -= 8) {
        uint64_t x = 0, y = 0;
        __builtin_memcpy(&x, a, 8);
        __builtin_memcpy(&y, b, 8);
        if (x != y) return false;
    }
    for (; size; --size, ++a, ++b)
        if (*a != *b) return false;
    return true;
}
// The world pass and the shadow pass alternate a handful of fragment programs,
// and bloom shells interleave with them, so a one-entry memory misses on nearly
// every draw: the fast path's word compare alone was 2.21% of the render thread
// (work/slow-scene-driver-draw-sample/profile.txt). Four recent variants cover
// that rotation. Entries are index+1 so the table's zero initialisation is
// constant and needs no thread-local guard; every hit is still verified by its
// flags and its complete name, and a miss falls through to the generated index.
constexpr size_t kRecentFragmentVariants = 4;
inline int recentFragmentFind(std::array<uint32_t,kRecentFragmentVariants>& recent,
                              std::string_view name,uint32_t flags) noexcept {
    for(size_t at=0;at<recent.size();++at) {
        const uint32_t entry=recent[at];
        if(!entry) continue;
        const int index=int(entry-1u);
        if(index>=int(Generated::kFragmentVariantCount)) continue;
        const auto& variant=Generated::kFragmentVariants[index];
        if(variant.flags!=flags || !sameProgramName(name,variant.name)) continue;
        for(size_t move=at;move;--move) recent[move]=recent[move-1];
        recent[0]=entry;
        return index;
    }
    return -1;
}
inline void recentFragmentRemember(std::array<uint32_t,kRecentFragmentVariants>& recent,int index) noexcept {
    const uint32_t entry=uint32_t(index)+1u;
    size_t at=0;
    while(at<recent.size() && recent[at]!=entry) ++at;
    if(at==recent.size()) at=recent.size()-1;
    for(size_t move=at;move;--move) recent[move]=recent[move-1];
    recent[0]=entry;
}
constexpr auto vertexIndex = ShaderLookup::index<Generated::kVertexVariantCount>([](size_t i) {
    return ShaderLookup::bytes(Generated::kVertexVariants[i].key,39);
});
constexpr auto fragmentIndex = ShaderLookup::index<Generated::kFragmentVariantCount>([](size_t i) {
    const auto& variant=Generated::kFragmentVariants[i];
    return ShaderLookup::fragment(variant.name,variant.flags);
});

} // namespace

WorldShaderModule findWorldVertexShader(const uint8_t key[39]) {
    const int index=findWorldVertexVariant(key);
    return index<0?WorldShaderModule{}:worldVertexShaderAt(uint32_t(index));
}

int findWorldVertexVariant(const uint8_t key[39]) {
    thread_local int previous=-1;
    if(previous>=0 && std::memcmp(Generated::kVertexVariants[previous].key,key,39)==0)return previous;
    size_t slot=ShaderLookup::bytes(key,39)&(vertexIndex.size()-1);
    while(vertexIndex[slot]>=0) {
        const int index=vertexIndex[slot];
        if(std::memcmp(Generated::kVertexVariants[index].key,key,39)==0) {previous=index;return index;}
        slot=(slot+1)&(vertexIndex.size()-1);
    }
    return -1;
}

WorldShaderModule worldVertexShaderAt(uint32_t index) {
    if (index >= Generated::kVertexVariantCount) return {};
    const Generated::VertexVariant& variant = Generated::kVertexVariants[index];
    return {variant.code, variant.words};
}

WorldVertexInputs findWorldVertexInputs(const uint8_t key[39]) {
    const int index=findWorldVertexVariant(key);
    return index<0?WorldVertexInputs{}:worldVertexInputsAt(uint32_t(index));
}

WorldVertexInputs worldVertexInputsAt(uint32_t index) {
    if(index>=Generated::kVertexVariantCount)return {};
    const auto& variant=Generated::kVertexVariants[index];
    return {variant.inputs,variant.inputCount};
}

int findWorldFragmentVariant(std::string_view name,uint32_t flags) {
    thread_local std::array<uint32_t,kRecentFragmentVariants> recent{};
    if(const int hit=recentFragmentFind(recent,name,flags);hit>=0) return hit;
    size_t slot=ShaderLookup::fragment(name,flags)&(fragmentIndex.size()-1);
    while(fragmentIndex[slot]>=0) {
        const int index=fragmentIndex[slot];
        const auto& variant=Generated::kFragmentVariants[index];
        if(variant.flags==flags && sameProgramName(name,variant.name)) {
            recentFragmentRemember(recent,index);
            return index;
        }
        slot=(slot+1)&(fragmentIndex.size()-1);
    }
    return -1;
}

WorldShaderModule worldFragmentShaderAt(uint32_t index,uint32_t* textures,uint32_t* cubes) {
    if(index>=Generated::kFragmentVariantCount)return {};
    const auto& variant=Generated::kFragmentVariants[index];
    if(textures)*textures=variant.textures;
    if(cubes)*cubes=variant.cubes;
    return {variant.code,variant.words};
}

WorldShaderModule findWorldFragmentShader(std::string_view name, uint32_t flags,
                                          uint32_t* textures, uint32_t* cubes) {
    const int index=findWorldFragmentVariant(name,flags);
    return index<0?WorldShaderModule{}:worldFragmentShaderAt(uint32_t(index),textures,cubes);
}

size_t worldVertexShaderCount() { return Generated::kVertexVariantCount; }
size_t worldFragmentShaderCount() { return Generated::kFragmentVariantCount; }

} // namespace DarkRecomp::Ps5
