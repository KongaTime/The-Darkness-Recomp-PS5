#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <type_traits>

namespace DarkRecomp::Native {
namespace DrawControlDetail {
constexpr size_t blockBytes=128,blockAlignment=64,blockCount=16384;
struct alignas(64) Cache {
    std::array<void*,32> blocks{};
    size_t count=0;
    void* take();
    void put(void* value);
};
struct Pool {
    std::mutex mutex;
    void* memory=std::aligned_alloc(blockAlignment,blockBytes*blockCount);
    std::array<void*,blockCount> free{};
    size_t count=0;
    std::array<Cache,8> reserves;
    std::atomic<unsigned> assigned{0};
    Pool() {
        if(memory)for(size_t i=0;i<blockCount;++i)
            free[count++]=static_cast<std::byte*>(memory)+i*blockBytes;
    }
    bool owns(const void* value) const noexcept {
        const auto first=reinterpret_cast<uintptr_t>(memory),address=reinterpret_cast<uintptr_t>(value);
        return memory && address>=first && address-first<blockBytes*blockCount && (address-first)%blockBytes==0;
    }
};
inline Pool& pool() {
    // Process lifetime, as with the draw-object pool: a retained shared_ptr
    // may retire after another translation unit's statics and TLS have ended.
    static auto* value=new Pool;
    return *value;
}
inline void* Cache::take() {
    if(!count) {
        auto& shared=pool();std::lock_guard lock(shared.mutex);
        while(count<blocks.size() && shared.count)blocks[count++]=shared.free[--shared.count];
    }
    return count?blocks[--count]:nullptr;
}
inline void Cache::put(void* value) {
    if(count==blocks.size()) {
        auto& shared=pool();std::lock_guard lock(shared.mutex);
        while(count)shared.free[shared.count++]=blocks[--count];
    }
    blocks[count++]=value;
}
inline Cache* cache() {
    // The first eight thread reserves are permanently owned by the bounded
    // process pool. Later/short-lived threads use its shared free list.
    thread_local auto* local=[]() -> Cache* {
        auto& shared=pool();auto index=shared.assigned.load(std::memory_order_relaxed);
        while(index<shared.reserves.size())
            if(shared.assigned.compare_exchange_weak(index,index+1,std::memory_order_relaxed))return &shared.reserves[index];
        return nullptr;
    }();
    return local;
}
inline void* take() noexcept {
    try {
        if(auto* local=cache())return local->take();
        auto& shared=pool();std::lock_guard lock(shared.mutex);
        return shared.count?shared.free[--shared.count]:nullptr;
    } catch(...) {return nullptr;}
}
inline bool release(void* value) noexcept {
    try {
        auto& shared=pool();if(!shared.owns(value))return false;
        // If a shutdown-time lock fails, retain this bounded slab slot rather
        // than pass an interior slab pointer to the general heap.
        try {
            if(auto* local=cache())local->put(value);
            else {std::lock_guard lock(shared.mutex);shared.free[shared.count++]=value;}
        } catch(...) {}
        return true;
    } catch(...) {return false;}
}
}

// Only storage changes. std::shared_ptr retains its normal atomic strong/weak
// counts and invokes the original draw recycler at the last strong release.
template<class T> struct DrawControlAllocator {
    using value_type=T;
    using is_always_equal=std::true_type;
    DrawControlAllocator() noexcept=default;
    template<class U> DrawControlAllocator(const DrawControlAllocator<U>&) noexcept {}
    T* allocate(size_t count) {
        if constexpr(sizeof(T)<=DrawControlDetail::blockBytes && alignof(T)<=DrawControlDetail::blockAlignment)
            if(count==1)if(auto* value=DrawControlDetail::take())return static_cast<T*>(value);
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* value,size_t count) noexcept {
        if constexpr(sizeof(T)<=DrawControlDetail::blockBytes && alignof(T)<=DrawControlDetail::blockAlignment)
            if(count==1 && DrawControlDetail::release(value))return;
        std::allocator<T>{}.deallocate(value,count);
    }
    template<class U> bool operator==(const DrawControlAllocator<U>&) const noexcept {return true;}
};
}
