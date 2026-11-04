#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace DarkRecomp::Native {
// Monotonic telemetry only: draw/resource publication has its own locks and
// ownership. Keep a Writer on its acquiring thread. Process-lifetime lanes
// preserve totals after that thread exits; overflow writers share an RMW lane.
template<size_t Count,size_t PrivateWriters=64>
class ThreadCounters {
    struct alignas(64) Lane {std::array<std::atomic<uint64_t>,Count> values{};};
    std::array<Lane,PrivateWriters+1> lanes_;
    std::atomic<size_t> next_{};
public:
    class Writer {
        friend class ThreadCounters;
        // Lane alignment leaves the low bit free. One scalar keeps emulated
        // TLS from resolving a separate address for pointer and shared flag.
        uintptr_t state_=0;
        Writer(Lane* lane,bool shared):state_(reinterpret_cast<uintptr_t>(lane)|uintptr_t(shared)) {}
        static_assert(alignof(Lane)>=2);
    public:
        // A constinit thread-local Writer avoids a separate C++ TLS guard on
        // platforms with emulated TLS. Initialize it from acquireWriter before
        // incrementing; keep it on its acquiring thread for this counter set.
        constexpr Writer() noexcept=default;
        explicit operator bool() const noexcept {return state_!=0;}
        void increment(size_t counter) const noexcept {
            auto& value=reinterpret_cast<Lane*>(state_&~uintptr_t(1))->values[counter];
            if(state_&1)value.fetch_add(1,std::memory_order_relaxed);
            else value.store(value.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        }
    };
    Writer acquireWriter() noexcept {
        auto next=next_.load(std::memory_order_relaxed);
        while(next<PrivateWriters) {
            if(next_.compare_exchange_weak(next,next+1,std::memory_order_relaxed))
                return Writer(&lanes_[next],false);
        }
        return Writer(&lanes_[PrivateWriters],true);
    }
    std::array<uint64_t,Count> snapshot() const noexcept {
        std::array<uint64_t,Count> sums{};
        for(const auto& lane:lanes_)for(size_t i=0;i<Count;++i)
            sums[i]+=lane.values[i].load(std::memory_order_relaxed);
        return sums;
    }
};
}
