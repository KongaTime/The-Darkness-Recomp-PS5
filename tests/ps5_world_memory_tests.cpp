// Exercise the renderer's actual allocator with scripted Vulkan memory calls.
#include "renderer/ps5/world_draw.cpp"
#include <cassert>
#include <map>

namespace {
std::map<VkDeviceMemory, std::vector<uint8_t>> allocations;
uintptr_t nextHandle = 1;
unsigned allocated = 0, freed = 0;
VkResult VKAPI_PTR allocate(VkDevice, const VkMemoryAllocateInfo* info,
                          const VkAllocationCallbacks*, VkDeviceMemory* out) {
    *out = (VkDeviceMemory)nextHandle++;
    allocations[*out].resize(info->allocationSize);
    ++allocated;
    return VK_SUCCESS;
}
void VKAPI_PTR release(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    assert(allocations.erase(memory) == 1);
    ++freed;
}
VkResult VKAPI_PTR map(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
                     VkDeviceSize, VkMemoryMapFlags, void** out) {
    *out = allocations.at(memory).data() + offset;
    return VK_SUCCESS;
}
void VKAPI_PTR unmap(VkDevice, VkDeviceMemory) {}
bool biasRecorded = false;
std::array<unsigned, 7> stateCalls{};
void VKAPI_PTR viewportMock(VkCommandBuffer, uint32_t, uint32_t count, const VkViewport*) { assert(count == 1); ++stateCalls[0]; }
void VKAPI_PTR scissorMock(VkCommandBuffer, uint32_t, uint32_t count, const VkRect2D*) { assert(count == 1); ++stateCalls[1]; }
void VKAPI_PTR stencilMock(VkCommandBuffer, VkStencilFaceFlags, uint32_t value) { assert(value == 128); ++stateCalls[2]; }
void VKAPI_PTR pipelineMock(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) { ++stateCalls[3]; }
void VKAPI_PTR vertexMock(VkCommandBuffer, uint32_t, uint32_t count, const VkBuffer*, const VkDeviceSize* offset) {
    assert(count == 1 && *offset == 0); ++stateCalls[4];
}
void VKAPI_PTR indexMock(VkCommandBuffer, VkBuffer, VkDeviceSize offset, VkIndexType type) {
    assert(offset == 0 && type == VK_INDEX_TYPE_UINT16); ++stateCalls[5];
}
unsigned setAllocations = 0, descriptorUpdates = 0, expectedTextureMask = 0;
struct BoundSets {uint32_t first,count;std::vector<uint32_t> offsets;};
std::vector<BoundSets> boundSets;
void VKAPI_PTR bindSetsMock(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t first,
    uint32_t count,const VkDescriptorSet*,uint32_t offsetsCount,const uint32_t* offsets) {
    assert((first==0 && count==1 && offsetsCount==4) || (first==1 && count==1 && offsetsCount==6) ||
           (first==0 && count==2 && offsetsCount==10));
    boundSets.push_back({first,count,{offsets,offsets+offsetsCount}});
}
VkResult VKAPI_PTR allocateSetMock(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* set) {
    assert(info->descriptorSetCount == 1);
    *set = (VkDescriptorSet)(uintptr_t(++setAllocations));
    return VK_SUCCESS;
}
void VKAPI_PTR updateSetsMock(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                             uint32_t copies, const VkCopyDescriptorSet*) {
    assert(!copies);
    assert(count == 4 || count == 6 + 2 * std::popcount(expectedTextureMask));
    for (uint32_t i = 0; i < count; ++i) {
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            assert(expectedTextureMask & (1u << (writes[i].dstBinding - 16)));
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
            assert(expectedTextureMask & (1u << (writes[i].dstBinding - 32)));
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            assert(writes[i].pBufferInfo->offset == 0); // Per-draw offsets are dynamic.
    }
    ++descriptorUpdates;
}
void VKAPI_PTR recordBias(VkCommandBuffer, const VkDepthBiasInfoEXT* bias) {
    assert(bias->sType == VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT);
    const auto* representation = static_cast<const VkDepthBiasRepresentationInfoEXT*>(bias->pNext);
    assert(representation && representation->sType == VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT);
    assert(representation->depthBiasRepresentation ==
           VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT);
    assert(representation->depthBiasExact);
    assert(bias->depthBiasConstantFactor == 2.0f);
    assert(bias->depthBiasSlopeFactor == -0.5f);
    assert(bias->depthBiasClamp == 0.0f);
    biasRecorded = true;
    ++stateCalls[6];
}
}

#include "tests/retained_uniform_cases.h"
int main() {
    testRetainedUniforms();
    {
        // Compare the sparse traversal with a byte-exact, per-vector oracle,
        // including word boundaries, full words, signed zero and NaN payloads.
        std::array<uint8_t,4096> a{}, b{};
        uint64_t rng = 0x37A12B49;
        auto next = [&] {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;};
        for (unsigned test = 0; test < 4000; ++test) {
            std::array<uint64_t,4> mask{};
            for (auto& word : mask) word = test < 4 ? (test == 0 ? 0 : test == 1 ? UINT64_MAX : test == 2 ? 1 : uint64_t(1) << 63) : next();
            for (auto& byte : a) byte = uint8_t(next());
            b = a;
            if (test & 1) b[next() % b.size()] ^= uint8_t(1u << (next() % 8));
            bool expected = true;
            for (unsigned v = 0; v < 256; ++v)
                if ((mask[v / 64] >> (v % 64)) & 1)
                    expected &= std::memcmp(a.data()+v*16,b.data()+v*16,16)==0;
            assert(DarkRecomp::Ps5::equalWrittenVectors(a.data(),b.data(),mask)==expected);
        }
    }
    using namespace DarkRecomp::Ps5;
    {
        using namespace DarkRecomp::Native;
        for (unsigned first=0;first<=257;++first) for (unsigned count=0;count<=257;++count) {
            WorldVertexConstants constants;
            constants.written={0x105,0,UINT64_MAX,0x8000000000000000ull};
            auto expected=constants.written;
            for (unsigned v=first;v<256 && v<uint64_t(first)+count;++v)
                expected[v/64]|=uint64_t(1)<<(v%64);
            constants.markWritten(first,count);
            assert(constants.written==expected);
        }
        WorldVertexConstants clipped;
        clipped.markWritten(63,UINT32_MAX);
        assert((clipped.written==std::array<uint64_t,4>{uint64_t(1)<<63,UINT64_MAX,UINT64_MAX,UINT64_MAX}));
        auto v=std::make_shared<StoredGeometry>();
        StoredDraw source{v,v,7,9};
        source.transforms.emplace(); source.transforms->matrixAddress=0x1234;
        source.vertexBindings.emplace(); source.vertexBindings->constantBytes.fill(0xAC);
        StoredDraw copy(source);
        assert(copy.vertices==v && copy.firstIndex==7 && copy.transforms->matrixAddress==0x1234);
        assert(copy.vertexBindings->constantBytes==source.vertexBindings->constantBytes);
        copy=copy; copy=std::move(copy);
        assert(copy.vertexBindings && copy.indices==v); // Self-assignment is unchanged.
        StoredDraw moved(std::move(copy));
        assert(!copy.vertices && !copy.indices && copy.vertexBindings && copy.transforms);
        assert(moved.vertexBindings->constantBytes==source.vertexBindings->constantBytes);
        StoredDraw empty;
        moved=empty;
        assert(!moved && !moved.vertexBindings && !moved.transforms);
        moved=std::move(source);
        assert(moved && !source && source.vertexBindings && source.transforms);
        assert(moved.vertexBindings->constantBytes[4095]==0xAC);
        moved=StoredDraw{};
        assert(!moved && !moved.vertexBindings && !moved.transforms);
    }
    {
        struct Entry {std::shared_ptr<const int> source; unsigned value;};
        ResourceFrontCache<Entry> front;
        std::unordered_map<const int*,Entry> map;
        std::vector<std::shared_ptr<const int>> sources;
        for (unsigned i=0;i<4096;++i) {
            auto source=std::make_shared<const int>(int(i));
            sources.push_back(source);
            map.emplace(source.get(),Entry{source,i});
        }
        // More keys than front slots guarantees collisions. Alternate both
        // colliding keys and hits, and force map rehash between lookups.
        for (unsigned round=0;round<3;++round) {
            map.rehash(map.bucket_count()*2);
            for (unsigned i=0;i<sources.size();++i) {
                auto* entry=front.find(map,sources[i].get());
                assert(entry && entry->value==i);
                assert(front.find(map,sources[i].get())==entry);
            }
        }
        auto missing=std::make_shared<const int>(-1);
        assert(!front.find(map,missing.get()));
        assert(front.hits>=3*sources.size() && front.misses>=sources.size());
        std::weak_ptr<const int> lifetime=sources.front();
        front.clear(); // Required before node erasure, including budget trims.
        sources.clear(); map.clear();
        assert(lifetime.expired()); // Front table retains no source ownership.
        assert(!front.find(map,lifetime.lock().get()));
        auto replacement=std::make_shared<const int>(42);
        auto& entry=map.emplace(replacement.get(),Entry{replacement,42}).first->second;
        assert(front.remember(entry)==&entry && front.find(map,replacement.get())->value==42);
        front.clear(); map.clear();
    }
    {
        WorldDrawLayer::Impl constants;
        std::array<uint8_t,2048> memory{};
        constants.arena.mapped=memory.data();constants.arena.bytes=memory.size();
        std::array<uint32_t,4> first{0x3f800000,0,0x7fc01234,0x80000000};
        const auto original=constants.uniformBlock(2,first.data(),sizeof(first));
        assert(original==0 && constants.uniformBlock(2,first.data(),sizeof(first))==original);
        assert(constants.arenaOffset==sizeof(first)); // A hit uses no new storage.
        auto changed=first;changed[2]^=1; // NaN payloads must compare by bits.
        const auto replacement=constants.uniformBlock(2,changed.data(),sizeof(changed));
        assert(replacement!=original && replacement%256==0);
        assert(std::memcmp(memory.data()+original,first.data(),sizeof(first))==0); // Prior GPU draw stays immutable.
        const auto split=constants.uniformBlock(4,first.data(),4,first.data()+1,12);
        assert(constants.uniformBlock(4,first.data(),sizeof(first))==split); // Same shader bytes, different source layout.
        changed[3]=0; // Signed zero is also a real byte change.
        assert(constants.uniformBlock(4,changed.data(),sizeof(changed))!=split);
        assert(constants.uniformBlock(1,nullptr,1)==SIZE_MAX);
        const auto used=constants.arenaOffset;
        constants.arena.bytes=used;
        assert(constants.uniformBlock(4,first.data(),sizeof(first))==SIZE_MAX); // Full arena fails without overwriting prior values.
        assert(constants.arenaOffset==used);
        const auto comparisons=constants.slotCompares[4];
        assert(constants.uniformBlock(4,first.data(),sizeof(first),nullptr,0,true)==SIZE_MAX);
        assert(constants.slotCompares[4]==comparisons && constants.arenaOffset==used);
        assert(std::memcmp(constants.lastUniformBytes[4].data(),changed.data(),sizeof(changed))==0);
        constants.arena.bytes=memory.size();constants.resetFrameConstants();
        memory.fill(0xCC);
        assert(constants.uniformBlock(2,first.data(),sizeof(first))==0);
        assert(std::memcmp(memory.data(),first.data(),sizeof(first))==0); // New GPU frame cannot reuse stale offsets.
        constants.constantReuseEnabled=false;
        const auto withoutReuse=constants.uniformBlock(2,first.data(),sizeof(first));
        assert(withoutReuse!=0 && std::memcmp(memory.data()+withoutReuse,first.data(),sizeof(first))==0);
        constants.constantReuseEnabled=true;
        assert(constants.uniformBlock(2,first.data(),sizeof(first))==withoutReuse);
        constants.arena.mapped=nullptr;
    }
    {
        WorldDrawLayer::Impl bindings;bindings.CmdBindDescriptorSets=bindSetsMock;
        std::array<uint32_t,4> vertex{0,256,512,768};
        std::array<uint32_t,6> fragment{1024,1280,1536,1792,2048,2304};
        bindings.bindDescriptors(1,(VkDescriptorSet)1,(VkDescriptorSet)2,vertex,fragment);
        assert(boundSets.size()==1 && boundSets.back().count==2);
        bindings.bindDescriptors(1,(VkDescriptorSet)1,(VkDescriptorSet)2,vertex,fragment);
        assert(boundSets.size()==1);
        vertex[0]=4096;bindings.bindDescriptors(1,(VkDescriptorSet)1,(VkDescriptorSet)2,vertex,fragment);
        assert(boundSets.size()==2 && boundSets.back().first==0 && boundSets.back().count==1);
        fragment[5]=4608;bindings.bindDescriptors(1,(VkDescriptorSet)1,(VkDescriptorSet)2,vertex,fragment);
        assert(boundSets.size()==3 && boundSets.back().first==1);
        bindings.bindDescriptors(1,(VkDescriptorSet)1,(VkDescriptorSet)3,vertex,fragment);
        assert(boundSets.size()==4 && boundSets.back().first==1); // Image/sampler set changed.
        bindings.bindDescriptors(2,(VkDescriptorSet)1,VK_NULL_HANDLE,vertex,fragment);
        assert(boundSets.size()==5 && boundSets.back().count==1);
        bindings.bindDescriptors(2,(VkDescriptorSet)1,(VkDescriptorSet)3,vertex,fragment);
        assert(boundSets.size()==6 && boundSets.back().first==1); // New pass invalidates fragment state even after depth-only draw.
        bindings.resetFrameConstants();
        bindings.bindDescriptors(2,(VkDescriptorSet)1,(VkDescriptorSet)3,vertex,fragment);
        assert(boundSets.size()==7 && boundSets.back().count==2); // Handles can be reused by a reset pool.
    }
    {
        WorldDrawLayer::Impl state;
        state.CmdSetViewport = viewportMock; state.CmdSetScissor = scissorMock;
        state.CmdSetStencilReference = stencilMock; state.CmdSetDepthBias2EXT = recordBias;
        state.CmdBindPipeline = pipelineMock; state.CmdBindVertexBuffers = vertexMock;
        state.CmdBindIndexBuffer = indexMock;
        VkViewport viewport{0, 720, 1280, -720, 0, 1};
        VkRect2D scissor{{0, 0}, {1280, 720}};
        state.bindDrawState(1, (VkPipeline)1, (VkBuffer)1, (VkBuffer)2, viewport, scissor, 128, 2, -0.5f);
        state.bindDrawState(1, (VkPipeline)1, (VkBuffer)1, (VkBuffer)2, viewport, scissor, 128, 2, -0.5f);
        for (auto count : stateCalls) assert(count == 1);
        state.bindDrawState(1, (VkPipeline)1, (VkBuffer)1, (VkBuffer)3, viewport, scissor, 128, 2, -0.5f);
        assert(stateCalls[5] == 2 && stateCalls[0] == 1);
        state.bindDrawState(2, (VkPipeline)1, (VkBuffer)1, (VkBuffer)3, viewport, scissor, 128, 2, -0.5f);
        for (unsigned i = 0; i < stateCalls.size(); ++i) assert(stateCalls[i] == (i == 5 ? 3u : 2u));
    }
    {
        WorldDrawLayer::Impl descriptors;
        descriptors.pools.push_back({(VkDescriptorPool)1, 0});
        descriptors.AllocateDescriptorSets = allocateSetMock;
        descriptors.UpdateDescriptorSets = updateSetsMock;
        const char* error = nullptr;
        const auto vertex = descriptors.vertexSetForFrame(&error);
        assert(vertex && descriptors.vertexSetForFrame(&error) == vertex && setAllocations == 1 && descriptorUpdates == 1);
        FragmentSetKey key;
        key.mask = expectedTextureMask = 5;
        key.images[0] = (VkImageView)1; key.images[2] = (VkImageView)2;
        key.samplers[0] = (VkSampler)1; key.samplers[2] = (VkSampler)2;
        const auto fragment = descriptors.fragmentSetForFrame(key, &error);
        assert(fragment && setAllocations == 2 && descriptorUpdates == 2);
        assert(descriptors.fragmentSetForFrame(key, &error) == fragment && descriptorUpdates == 2);
        key.images[0] = (VkImageView)3;
        assert(descriptors.fragmentSetForFrame(key, &error) != fragment && descriptorUpdates == 3);
        key.samplers[2] = (VkSampler)3;
        assert(descriptors.fragmentSetForFrame(key, &error) && descriptorUpdates == 4);
        descriptors.frameVertexSet = VK_NULL_HANDLE;
        descriptors.frameFragmentSets.clear();
        assert(descriptors.vertexSetForFrame(&error) != vertex && descriptorUpdates == 5);
        assert(descriptors.fragmentSetForFrame(key, &error) && descriptorUpdates == 6);
    }
    // Streamed mip 2 belongs to level 2 of the full resource, not level 4.
    DarkRecomp::ColorImage streamed;
    streamed.width = streamed.height = 64;
    streamed.faces = 6;
    streamed.authoredMips = true;
    streamed.firstMip = 2;
    streamed.mips.resize(4);
    for (unsigned mip = 2; mip <= 4; ++mip)
        streamed.mips[mip - 1].resize((64u >> mip) * (64u >> mip) * 6 * 4);
    assert(streamed.valid());
    size_t bytes;
    std::array<size_t, 16> offsets;
    const auto copies = textureCopies(streamed, bytes, offsets);
    assert(copies.size() == 3 && bytes == streamed.bytes());
    assert(offsets[0] == SIZE_MAX && offsets[1] == SIZE_MAX && offsets[2] == 0);
    for (unsigned i = 0; i < copies.size(); ++i) {
        assert(copies[i].imageSubresource.mipLevel == i + 2);
        assert(copies[i].imageSubresource.baseArrayLayer == 0 && copies[i].imageSubresource.layerCount == 6);
        assert(copies[i].imageExtent.width == (64u >> (i + 2)));
        assert(copies[i].imageExtent.height == copies[i].imageExtent.width);
    }
    WorldDrawLayer::Impl renderer;
    renderer.CmdSetDepthBias2EXT = recordBias;
    renderer.setDepthBias(float(Policy::tolerantEqual(true).bias), -0.5f);
    assert(biasRecorded);
    renderer.AllocateMemory = allocate;
    renderer.FreeMemory = release;
    renderer.MapMemory = map;
    renderer.UnmapMemory = unmap;
    const VkMemoryRequirements request{4096, 256, 1};
    std::vector<std::shared_ptr<MemorySlice>> slices;
    for (unsigned i = 0; i < 1000; ++i) {
        auto slice = renderer.allocateMemory(request, 0, false, true);
        assert(slice && slice->offset % request.alignment == 0);
        for (const auto& other : slices)
            assert(other->offset + other->bytes <= slice->offset ||
                   slice->offset + slice->bytes <= other->offset);
        slices.push_back(std::move(slice));
    }
    assert(allocated == 1); // One mapping, rather than a thousand.
    auto inFlight = slices.front();
    slices.clear();
    auto next = renderer.allocateMemory(request, 0, false, true);
    assert(next->offset != inFlight->offset); // A recorded upload still owns it.
    inFlight.reset();
    next.reset();
    auto merged = renderer.allocateMemory({8u << 20, 2u << 20, 1}, 0, false, true);
    assert(merged && merged->offset == 0 && allocated == 1);
    auto image = renderer.allocateMemory(request, 0, true, false);
    assert(image && image->block != merged->block && allocated == 2);
    auto otherType = renderer.allocateMemory(request, 1, false, true);
    assert(otherType && otherType->block != merged->block && allocated == 3);
    renderer.memoryBlocks.clear();
    assert(freed == 0); // Live resources retain their backing memory.
    merged.reset(); image.reset(); otherType.reset();
    assert(freed == allocated && allocations.empty());
    std::puts("World resources: mip uploads, descriptor reuse, state invalidation and memory lifetimes pass.");
}
