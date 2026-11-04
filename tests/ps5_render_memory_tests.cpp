#include "renderer/engine/render_memory.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <sys/mman.h>
using namespace DarkRecomp::Native;
int main() {
    constexpr uint64_t bytes=0x100000000ull;
    auto* base=static_cast<uint8_t*>(mmap(nullptr,bytes,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
    assert(base!=MAP_FAILED);
    std::array<uint8_t,4096> pattern{}, output{};
    for(size_t i=0;i<pattern.size();++i)pattern[i]=uint8_t(i*37);
    std::memcpy(base+1,pattern.data(),pattern.size());
    for(size_t count : {size_t(0),size_t(1),size_t(4),size_t(8),size_t(12),size_t(24),size_t(160),size_t(840),size_t(4096)}) {
        assert(copyRenderMemory(base,1,output.data(),count));
        assert(std::memcmp(output.data(),pattern.data(),count)==0);
        assert(equalRenderMemory(base,1,output.data(),count));
        if(count) {output[count-1]^=1;assert(!equalRenderMemory(base,1,output.data(),count));}
    }
    std::memcpy(base+bytes-4,pattern.data(),4);
    assert(copyRenderMemory(base,bytes-4,output.data(),4));
    assert(equalRenderMemory(base,bytes-4,pattern.data(),4));
    output.fill(0xA5);
    for(auto address : {uint64_t(0),bytes,UINT64_MAX}) {
        assert(!copyRenderMemory(base,address,output.data(),4));
        assert(!equalRenderMemory(base,address,output.data(),4));
    }
    assert(!copyRenderMemory(base,bytes-3,output.data(),4));
    assert(!copyRenderMemory(base,1,output.data(),std::numeric_limits<size_t>::max()));
    assert(!equalRenderMemory(base,bytes-3,output.data(),4));
    assert(!copyRenderMemory(nullptr,1,output.data(),4));
    assert(!copyRenderMemory(base,1,nullptr,4));
    assert(!equalRenderMemory(nullptr,1,output.data(),4));
    assert(!equalRenderMemory(base,1,nullptr,4));
    for(auto value:output)assert(value==0xA5); // Invalid copies never publish bytes.
    munmap(base,bytes);
    std::puts("PS5 render reads: exact bytes, fixed/bank sizes, end boundary, overflow, nulls and failure preservation pass.");
}
