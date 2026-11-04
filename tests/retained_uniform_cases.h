#include <thread>
static void testRetainedUniforms() {
    using namespace DarkRecomp::Native;
    using namespace DarkRecomp::Ps5;
    WorldDrawLayer::Impl renderer;
    std::vector<uint8_t> memory(1024*1024,0xcc);
    renderer.arena.mapped=memory.data();renderer.arena.bytes=memory.size();
    renderer.retainUniformShadows=true;
    auto make=[](uint8_t value) {
        auto draw=std::make_shared<WorldDraw>();
        std::memset(draw->constants.vectors.data(),value,4096);
        std::memset(draw->fragmentConstants.data(),value,1024);
        return QueuedWorldDrawAccess::seal(std::move(draw));
    };
    for(unsigned round=0;round<2;++round) {
        renderer.resetFrameConstants();
        auto first=make(0x81);std::weak_ptr<const WorldDraw> weak=first;
        std::array<uint8_t,4096> expected;expected.fill(0x81);
        const auto offset=renderer.uniformBlock(0,first->constants.vectors.data(),4096,nullptr,0,false,&first);
        assert(offset!=SIZE_MAX && bool(renderer.lastVertexUniformOwner));
        assert(std::memcmp(memory.data()+offset,expected.data(),4096)==0);
        std::thread retire([value=std::move(first)]() mutable {value.reset();});retire.join();
        assert(!weak.expired()); // Queue retirement cannot invalidate comparisons.
        auto same=make(0x81);
        assert(renderer.uniformBlock(0,same->constants.vectors.data(),4096,nullptr,0,false,&same)==offset);
        assert(!weak.expired());same.reset();
        auto changed=make(0x82);std::weak_ptr<const WorldDraw> changedWeak=changed;
        const auto replacement=renderer.uniformBlock(0,changed->constants.vectors.data(),4096,nullptr,0,false,&changed);
        assert(replacement!=offset && weak.expired());
        assert(std::memcmp(memory.data()+offset,expected.data(),4096)==0); // Prior GPU block unchanged.
        std::array<uint32_t,4> prefix{0x80000000,0x7fc01234,0x3f800000,0};
        const auto originalPrefix=prefix;
        const auto fragment=renderer.uniformBlock(4,prefix.data(),16,changed->fragmentConstants.data()+1,1008,false,&changed);
        assert(fragment!=SIZE_MAX && bool(renderer.lastFragmentUniformOwner));
        assert(std::memcmp(memory.data()+fragment,prefix.data(),16)==0);
        assert(std::memcmp(memory.data()+fragment+16,changed->fragmentConstants.data()+1,1008)==0);
        prefix.fill(0); // Computed stack prefix was copied, not borrowed.
        assert(renderer.uniformBlock(4,originalPrefix.data(),16,changed->fragmentConstants.data()+1,1008,false,&changed)==fragment);
        auto newPrefix=originalPrefix;newPrefix[1]^=1;
        const auto fragment2=renderer.uniformBlock(4,newPrefix.data(),16,changed->fragmentConstants.data()+1,1008,false,&changed);
        assert(fragment2!=fragment && std::memcmp(memory.data()+fragment,originalPrefix.data(),16)==0);
        std::array<uint8_t,1024> full;
        std::memcpy(full.data(),newPrefix.data(),16);std::memcpy(full.data()+16,changed->fragmentConstants.data()+1,1008);
        assert(renderer.uniformBlock(4,full.data(),full.size())==fragment2); // Segment layout is irrelevant.
        const auto used=renderer.arenaOffset,saved=renderer.retainedUniformBytes;
        renderer.arena.bytes=used;
        auto refused=make(0x83);
        assert(renderer.uniformBlock(0,refused->constants.vectors.data(),4096,nullptr,0,false,&refused)==SIZE_MAX);
        assert(renderer.arenaOffset==used && renderer.retainedUniformBytes==saved);
        assert(renderer.vertexUniformShadow()==reinterpret_cast<const uint8_t*>(changed->constants.vectors.data()));
        renderer.arena.bytes=memory.size();
        changed.reset();assert(!changedWeak.expired());
        renderer.resetFrameConstants();assert(changedWeak.expired());
        // Public mutable captures and aliased values do not grant retention.
        auto mutableDraw=std::make_shared<WorldDraw>();
        std::shared_ptr<const WorldDraw> publicOwner=mutableDraw;
        auto copied=renderer.uniformBlock(0,mutableDraw->constants.vectors.data(),4096,nullptr,0,false,&publicOwner);
        assert(!bool(renderer.lastVertexUniformOwner));
        mutableDraw->constants.vectors[0][0]=-0.0f;
        const auto copied2=renderer.uniformBlock(0,mutableDraw->constants.vectors.data(),4096,nullptr,0,false,&publicOwner);
        assert(copied2!=copied && memory[copied+3]==0 && memory[copied2+3]==0x80);
        auto alias=mutableDraw;auto unsealed=QueuedWorldDrawAccess::seal(std::move(mutableDraw));
        assert(!QueuedWorldDrawAccess::owned(*unsealed));
        renderer.uniformBlock(0,unsealed->constants.vectors.data(),4096,nullptr,0,true,&unsealed);
        assert(!bool(renderer.lastVertexUniformOwner));
        // A valid owner for a different source cannot authorize borrowed bytes.
        auto trusted=make(0x84);
        renderer.uniformBlock(0,unsealed->constants.vectors.data(),4096,nullptr,0,true,&trusted);
        assert(!bool(renderer.lastVertexUniformOwner));
        renderer.uniformBlock(0,trusted->constants.vectors.data(),4096,nullptr,0,true,&trusted);
        assert(bool(renderer.lastVertexUniformOwner));
        renderer.retainUniformShadows=false;renderer.constantReuseEnabled=false;
        renderer.uniformBlock(0,trusted->constants.vectors.data(),4096,nullptr,0,false,&trusted);
        assert(!bool(renderer.lastVertexUniformOwner));
        renderer.constantReuseEnabled=true;renderer.retainUniformShadows=true;
    }
    renderer.resetFrameConstants();renderer.arena.mapped=nullptr;
    std::puts("Retained uniforms: exact GPU bytes/offset reuse, split NaN/zero prefixes, batch retirement, replacement/reset, exhaustion, mutable/aliased/wrong-owner fallback pass.");
}
