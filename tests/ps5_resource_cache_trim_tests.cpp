#include "renderer/ps5/resource_cache_trim.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <unordered_map>
struct Entry {std::shared_ptr<const unsigned> source;uint64_t used;size_t bytes;};
using Cache=std::unordered_map<unsigned,Entry>;
using DarkRecomp::Ps5::trimResourceCache;
int main() {
    Cache cache;size_t bytes=0,cursor=0,releases=0;
    auto size=[](const Entry& e) {return e.bytes;};
    auto release=[&](Entry& e) {assert(e.source);++releases;};
    std::vector<std::shared_ptr<const unsigned>> retained;
    for(unsigned i=0;i<12000;++i) {
        auto source=std::make_shared<const unsigned>(i);
        cache.emplace(i,Entry{source,i,32});bytes+=32;
        if(i%2)retained.push_back(source);
    }
    for(unsigned frame=0;frame<8;++frame)trimResourceCache(cache,bytes,500000,cursor,size,release);
    assert(cache.size()==6000 && bytes==6000*32 && releases==6000);
    for(const auto& [id,e]:cache)assert(id%2 && *e.source==id);
    // Rehash between frames: no stored iterator may be reused afterward.
    for(unsigned frame=0;frame<20;++frame) {
        for(unsigned i=0;i<100;++i) {
            const unsigned id=12000+frame*100+i;
            cache[id]={std::make_shared<const unsigned>(id),id,32};bytes+=32;
        }
        cache.rehash(cache.bucket_count()+101);
        trimResourceCache(cache,bytes,1000000,cursor,size,release);
    }
    for(unsigned frame=0;frame<8;++frame)trimResourceCache(cache,bytes,1000000,cursor,size,release);
    assert(cache.size()==6000 && bytes==6000*32);
    // Budget pressure must do a full orphan sweep, then the same LRU eviction
    // immediately; retained external/queued owners survive GPU cache eviction.
    trimResourceCache(cache,bytes,32*10,cursor,size,release);
    assert(cache.size()==10 && bytes==320);
    for(unsigned id=11981;id<12000;id+=2)assert(cache.contains(id));
    for(unsigned i=0;i<retained.size();++i)assert(*retained[i]==i*2+1);
    retained.clear();
    trimResourceCache(cache,bytes,0,cursor,size,release);
    assert(cache.empty() && bytes==0);
    trimResourceCache(cache,bytes,0,cursor,size,release);
    // Reuse removed keys and cover randomized ownership/budget combinations.
    cache[9]={std::make_shared<const unsigned>(9),9,1};bytes=1;
    trimResourceCache(cache,bytes,100,cursor,size,release,false);
    assert(cache.empty() && !bytes); // live A/B restores the full sweep
    std::mt19937 random(812);
    for(unsigned round=0;round<200;++round) {
        for(unsigned i=0;i<200;++i) {
            auto source=std::make_shared<const unsigned>(i);
            cache[i]={source,i,1};++bytes;if(random()%2)retained.push_back(source);
        }
        const size_t budget=random()%201;
        for(unsigned frame=0;frame<8;++frame)trimResourceCache(cache,bytes,budget,cursor,size,release);
        assert(bytes==cache.size() && bytes<=budget);
        for(const auto& [id,e]:cache)assert(*e.source==id && e.source.use_count()>1);
        retained.clear();trimResourceCache(cache,bytes,0,cursor,size,release);
        assert(cache.empty() && !bytes);
    }
    struct CollisionHash {size_t operator()(unsigned) const {return 0;}};
    std::unordered_map<unsigned,Entry,CollisionHash> collisions;
    size_t collisionBytes=512,collisionCursor=0;
    for(unsigned i=0;i<512;++i)collisions[i]={std::make_shared<const unsigned>(i),i,1};
    for(unsigned frame=0;frame<8;++frame)
        trimResourceCache(collisions,collisionBytes,1000,collisionCursor,size,release);
    assert(collisions.empty() && !collisionBytes);
    std::puts("Cache trim: deferred orphan sweep, rehash, key reuse, hard budgets, LRU and retained-owner lifetimes pass.");
    retained.clear();
    for(unsigned i=0;i<16000;++i) {
        auto owner=std::make_shared<const unsigned>(i);retained.push_back(owner);
        cache[i]={owner,i,1};++bytes;
    }
    for(bool bounded:{false,true}) {
        auto start=std::chrono::steady_clock::now();size_t observed=0;
        for(unsigned frame=0;frame<2000;++frame) {
            if(bounded)trimResourceCache(cache,bytes,20000,cursor,size,release);
            else for(const auto& [id,e]:cache)observed+=e.source.use_count()==1;
        }
        std::printf("%s %.3f us/frame, observed=%zu\n",bounded?"bounded":"full",
            std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/2000,observed);
    }
}
