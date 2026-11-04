#include "renderer/engine/draw_control_allocator.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>
using namespace DarkRecomp::Native;
namespace {
std::shared_ptr<unsigned> staticRelease;
struct ExitCheck {
    ~ExitCheck(){staticRelease.reset();}
} exitCheck;
}
int main() {
    DrawControlAllocator<uint64_t> allocator;
    std::vector<uint64_t*> pointers;
    std::unordered_set<void*> distinct;
    unsigned pooled=0,fallback=0;
    for(unsigned i=0;i<DrawControlDetail::blockCount+128;++i) {
        auto* value=allocator.allocate(1);*value=i;
        assert(distinct.insert(value).second);
        if(DrawControlDetail::pool().owns(value)) {++pooled;assert(reinterpret_cast<uintptr_t>(value)%64==0);}
        else ++fallback;
        pointers.push_back(value);
    }
    assert(pooled==DrawControlDetail::blockCount && fallback==128);
    std::thread retire([&] {for(unsigned i=0;i<pointers.size();++i){assert(*pointers[i]==i);allocator.deallocate(pointers[i],1);}});
    retire.join();
    std::atomic<unsigned> destroyed{0};
    std::vector<std::shared_ptr<unsigned>> held;
    for(unsigned i=0;i<3000;++i)held.emplace_back(new unsigned(i),[&](unsigned* value){++destroyed;delete value;},DrawControlAllocator<std::byte>{});
    auto retained=held[17];std::weak_ptr<unsigned> weak=retained;
    std::thread release([items=std::move(held)]() mutable {items.clear();});release.join();
    assert(destroyed==2999 && !weak.expired() && *retained==17);
    retained.reset();assert(destroyed==3000 && weak.expired() && !weak.lock());
    // A dead weak reference must never become live when the slab is reused.
    for(unsigned i=0;i<1000;++i) {
        std::shared_ptr<unsigned> next(new unsigned(i),std::default_delete<unsigned>{},DrawControlAllocator<std::byte>{});
        assert(weak.expired());
    }
    weak.reset();
    std::vector<std::thread> threads;
    for(unsigned thread=0;thread<12;++thread)threads.emplace_back([thread] {
        DrawControlAllocator<uint64_t> local;
        for(unsigned i=0;i<10000;++i) {
            auto* value=local.allocate(1);const uint64_t expected=uint64_t(thread)<<32|i;
            *value=expected;if(i%64==0)std::this_thread::yield();
            assert(*value==expected);local.deallocate(value,1);
        }
    });
    for(auto& thread:threads)thread.join();
    struct alignas(128) Large {char bytes[256];};
    DrawControlAllocator<Large> large;auto* big=large.allocate(1);
    assert(!DrawControlDetail::pool().owns(big) && reinterpret_cast<uintptr_t>(big)%128==0);large.deallocate(big,1);
    auto* array=allocator.allocate(4);assert(!DrawControlDetail::pool().owns(array));allocator.deallocate(array,4);
    staticRelease=std::shared_ptr<unsigned>(new unsigned(7),std::default_delete<unsigned>{},DrawControlAllocator<std::byte>{});
    std::puts("Draw control pool: unique aligned slots, exhaustion fallback, cross-thread/weak ownership, concurrency, alignment and static retirement pass.");
}
