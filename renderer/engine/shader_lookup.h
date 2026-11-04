#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DarkRecomp::ShaderLookup {
constexpr uint32_t bytes(const uint8_t* data,size_t count) noexcept {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<count;++i)hash=(hash^data[i])*16777619u;
    return hash;
}
constexpr uint32_t fragment(std::string_view name,uint32_t flags) noexcept {
    uint32_t hash=2166136261u;
    for(unsigned char c:name)hash=(hash^c)*16777619u;
    for(unsigned i=0;i<4;++i)hash=(hash^uint8_t(flags>>(i*8)))*16777619u;
    return hash;
}
constexpr size_t capacity(size_t count) noexcept {
    size_t slots=8;
    while(slots<count*2+1)slots*=2;
    return slots;
}
// Own only indexes into immutable generated tables. Lookup callers verify the
// complete key after hashing; collisions can never select a different shader.
template<size_t Count,class HashAt> constexpr auto index(HashAt hashAt) noexcept {
    std::array<int,capacity(Count)> slots{};slots.fill(-1);
    for(size_t i=0;i<Count;++i) {
        size_t slot=hashAt(i)&(slots.size()-1);
        while(slots[slot]>=0)slot=(slot+1)&(slots.size()-1);
        slots[slot]=int(i);
    }
    return slots;
}
}
