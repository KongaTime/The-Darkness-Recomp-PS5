#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace DarkRecomp::Ps5 {
// Call only after the preceding render fence. Under budget, sweep one eighth
// of the buckets per frame instead of chasing every retained source pointer.
// Deferred entries still own their source, so their pointer keys cannot alias
// a new allocation. Over budget, retain the original full orphan/LRU cleanup.
// No iterator survives this call: uploads may rehash the map during the frame.
template<class Cache,class SizeOf,class Release>
void trimResourceCache(Cache& cache,size_t& bytes,size_t budget,size_t& cursor,
                       SizeOf sizeOf,Release release,bool incremental=true) {
    auto erase=[&](auto it) {
        bytes-=sizeOf(it->second);release(it->second);return cache.erase(it);
    };
    if(incremental && bytes<=budget) {
        const size_t buckets=cache.bucket_count();
        if(!buckets || cache.empty()) {cursor=0;return;}
        cursor%=buckets;
        const size_t count=buckets/8+(buckets%8!=0);
        for(size_t n=0;n<count;++n) {
            const size_t bucket=cursor;
            for(auto it=cache.begin(bucket);it!=cache.end(bucket);) {
                if(it->second.source.use_count()==1) {
                    const auto key=it->first;
                    ++it; // erase never rehashes or invalidates the next node
                    erase(cache.find(key));
                } else ++it;
            }
            if(++cursor==buckets)cursor=0;
        }
        return;
    }
    cursor=0;
    for(auto it=cache.begin();it!=cache.end();) {
        if(it->second.source.use_count()==1)it=erase(it);else ++it;
    }
    if(bytes<=budget)return;
    using Key=typename Cache::key_type;
    std::vector<std::pair<uint64_t,Key>> order;
    order.reserve(cache.size());
    for(const auto& entry:cache)order.emplace_back(entry.second.used,entry.first);
    std::sort(order.begin(),order.end(),[](const auto& a,const auto& b) {return a.first<b.first;});
    for(const auto& [used,key]:order) {
        if(bytes<=budget)break;
        erase(cache.find(key));
    }
}
}
