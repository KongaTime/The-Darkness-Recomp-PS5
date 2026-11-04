#include <random>
namespace {
constexpr uint32_t lockResource=0x04000000,lockTable=0x04100000,lockManager=0x04200000,lockValidity=0x04300000;
void initializeStoredLockFixture(uint8_t* base) {
    put32(base,kContext+16636,lockTable);put32(base,kContext+160,lockManager);
    put32(base,lockManager+24,lockValidity);
    for(unsigned id=1;id<=2;++id) {
        const auto at=lockResource+id*128;
        std::memset(base+at,0,108);
        put16(base,at+92,uint16_t(id));put16(base,at+94,2);base[at+96]=12;
        put32(base,at+8,0x12340000+id);
        put32(base,at+48,0x04500000+id*64);
        put32(base,at+88,0x04600000+id*64);
        put32(base,lockTable+(id+1)*4,at);base[lockValidity+id]=1;
    }
}
void publishStoredLockFixture(StoredGeometryCache& cache,uint8_t* base,unsigned id,uint8_t marker=0) {
    auto upload=cache.begin(base,lockResource+id*128);
    require(bool(upload.geometry),"Stored-lock fixture upload refused");
    auto& geometry=*upload.geometry;
    geometry.vertexCount=id==1?3:6;geometry.stride=12;
    geometry.vertices.resize(geometry.vertexCount*12,marker);
    geometry.indices={0,1,2,3,4,5};geometry.maximumIndex=5;
    require(cache.finish(std::move(upload),base),"Stored-lock fixture publication refused");
}
void testStoredLockPolicies(uint8_t* base) {
    initializeStoredLockFixture(base);
    StoredGeometryCache original,candidate;
    for(unsigned id=1;id<=2;++id) {publishStoredLockFixture(original,base,id);publishStoredLockFixture(candidate,base,id);}
    std::mt19937 random(0x7819f);
    for(unsigned trial=0;trial<10000;++trial) {
        const unsigned v=1+random()%2,i=1+random()%2;
        const uint32_t first=random()%8,count=random()%10;
        const uint32_t bound=(random()%5)?0x04600000+i*64:0;
        // Mutate every descriptor byte, including the binding identities and
        // ignored padding, while both policies see the exact same live state.
        const uint32_t mutation=lockResource+(1+random()%2)*128+random()%108;
        const auto saved=base[mutation];base[mutation]^=uint8_t(random());
        const auto a=original.testDrawWithLockPolicy(false,base,v,i,first,count,bound);
        const auto b=candidate.testDrawWithLockPolicy(true,base,v,i,first,count,bound);
        require(bool(a)==bool(b),"Stored lock policies disagree on acceptance");
        if(a)require(a.firstIndex==b.firstIndex && a.indexCount==b.indexCount &&
            a.vertices->vertices==b.vertices->vertices && a.indices->indices==b.indices->indices &&
            a.vertices->resource==b.vertices->resource && a.indices->resource==b.indices->resource,
            "Stored lock policy changed owned draw data");
        base[mutation]=saved;
        const auto sa=original.stats(),sb=candidate.stats();
        require(sa.matchedDraws==sb.matchedDraws && sa.missingDraws==sb.missingDraws && sa.invalidDraws==sb.invalidDraws,
                "Stored lock policies disagree on rejection reason");
    }
    // Concurrent cache replacement does not mutate guest bytes. This tests
    // publication/lifetime races without introducing a host-language data race.
    for(bool single:{false,true}) {
        StoredGeometryCache cache;publishStoredLockFixture(cache,base,1,17);
        auto retained=cache.testDrawWithLockPolicy(single,base,1,1,0,3,0x04600000+64);
        require(bool(retained),"Stored lock lifetime fixture refused");
        std::atomic<bool> start{},valid{true};
        std::thread worker([&] {
            while(!start.load(std::memory_order_acquire))std::this_thread::yield();
            for(unsigned generation=0;generation<5000;++generation)
                publishStoredLockFixture(cache,base,1,uint8_t(generation));
        });
        start.store(true,std::memory_order_release);
        for(unsigned draw=0;draw<20000;++draw) {
            const auto value=cache.testDrawWithLockPolicy(single,base,1,1,0,3,0x04600000+64);
            if(value && (value.vertices.get()!=value.indices.get() || value.vertices->vertices.size()!=36))valid=false;
        }
        worker.join();
        require(valid && retained.vertices->vertices[0]==17 && retained.vertices.get()==retained.indices.get(),
                "Stored lock replacement mixed or destroyed retained geometry");
    }
    std::puts("Stored locks: 10000 policy oracles, descriptor mutations/subsets/rejection reasons and concurrent replacement ownership pass.");
}
}
