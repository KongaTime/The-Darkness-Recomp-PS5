#include "renderer/engine/thread_counters.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace DarkRecomp::Native;
template<size_t Private> static void exercise() {
    ThreadCounters<3,Private> counts;
    std::atomic<bool> start{},stop{},valid{true};
    std::thread reader([&] {
        std::array<uint64_t,3> previous{};
        while(!stop.load(std::memory_order_relaxed)) {
            const auto current=counts.snapshot();
            for(unsigned i=0;i<3;++i)if(current[i]<previous[i])valid=false;
            previous=current;
        }
    });
    constexpr unsigned writers=80,iterations=20000;
    std::vector<std::thread> threads;
    for(unsigned i=0;i<writers;++i)threads.emplace_back([&,i] {
        typename ThreadCounters<3,Private>::Writer writer;
        if(writer)throw std::runtime_error("default writer is already initialized");
        writer=counts.acquireWriter();
        if(!writer)throw std::runtime_error("acquired writer is not initialized");
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned j=0;j<iterations;++j) {
            writer.increment(i%3);
            if((j&3)==0)writer.increment((i+1)%3);
        }
    });
    start.store(true,std::memory_order_release);
    for(auto& thread:threads)thread.join();
    stop=true;reader.join();
    std::array<uint64_t,3> expected{};
    for(unsigned i=0;i<writers;++i) {
        expected[i%3]+=iterations;expected[(i+1)%3]+=(iterations+3)/4;
    }
    if(!valid || counts.snapshot()!=expected)throw std::runtime_error("lost or decreasing counter totals");
    // Exited writers retain their totals; later threads use new/overflow lanes.
    std::thread late([&] {const auto writer=counts.acquireWriter();writer.increment(2);});late.join();
    ++expected[2];
    if(counts.snapshot()!=expected)throw std::runtime_error("lost exited writer total");
}
int main() {
    exercise<64>();exercise<2>();exercise<0>();
    std::puts("Thread counters: 80 concurrent writers, private/shared/zero-private lanes, live monotonic snapshots and exited writers pass.");
}
