#include "resource_cache_trim.h"
#include "world_draw.h"

#include "display_vulkan.h"
#include "world_draw_policy.h"
#include "world_shaders.h"

#include "renderer/d3d11/world_darkness_bloom.h"
#include "renderer/engine/world_mesh.h"
#include "renderer/engine/queued_world_draw.h"
#include "runtime/native/graphics_settings.h"
#include "renderer/engine/engine_performance.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace DarkRecomp::Ps5 {

namespace Policy = WorldDrawPolicy;

namespace {
// Front cache for map-owned nodes. Rehash preserves node addresses; erasure
// must clear this table first. It owns no resources and cannot extend lifetime.
template<class Entry> struct ResourceFrontCache {
    // Two ways per set. The renderer walks several streams in lockstep (a world
    // and its shadow, vertex and index), and a direct-mapped front cache let
    // those evict each other: the live counters showed indices at 48% hits
    // against 84% for vertices, with the difference landing in the hash map.
    // Both ways are still validated by identity, so a wider cache cannot serve
    // a stale entry; it only removes map probes.
    std::array<Entry*,1024> primary{};
    std::array<Entry*,1024> secondary{};
    uint64_t hits = 0, misses = 0;
    static size_t slot(const void* source) {
        const auto key = reinterpret_cast<uintptr_t>(source) >> 4;
        return (key ^ (key >> 10) ^ (key >> 20)) & 1023;
    }
    template<class Map, class Key> Entry* find(Map& map, Key source) {
        const size_t index = slot(source);
        if (primary[index] && primary[index]->source.get() == source) {++hits; return primary[index];}
        if (secondary[index] && secondary[index]->source.get() == source) {
            ++hits;
            std::swap(primary[index], secondary[index]);
            return primary[index];
        }
        ++misses;
        const auto found = map.find(source);
        if (found == map.end()) return nullptr;
        secondary[index] = primary[index];
        return primary[index] = &found->second;
    }
    Entry* remember(Entry& entry) {
        const size_t index = slot(entry.source.get());
        secondary[index] = primary[index];
        return primary[index] = &entry;
    }
    void clear() {primary.fill(nullptr); secondary.fill(nullptr);}
};
// Exact prefix test without the out-of-line char_traits compare the standard
// starts_with performs; a draw asks these once per sampled slot.
inline bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (text[i] != prefix[i]) return false;
    return true;
}
// Jump between populated runs instead of testing all 256 vector positions.
bool equalWrittenVectors(const void* left, const void* right,
                         const std::array<uint64_t,4>& written) {
    const auto* a = static_cast<const uint8_t*>(left);
    const auto* b = static_cast<const uint8_t*>(right);
    for (unsigned word = 0; word < written.size(); ++word) {
        uint64_t bits = written[word];
        while (bits) {
            const unsigned first = std::countr_zero(bits);
            const unsigned count = std::countr_one(bits >> first);
            const size_t offset = (word * 64 + first) * 16;
            if (std::memcmp(a + offset, b + offset, count * 16) != 0) return false;
            // Avoid shifting by 64 for a fully populated word.
            if (first + count == 64) break;
            bits &= ~((uint64_t(1) << (first + count)) - 1);
        }
    }
    return true;
}

// The surfaces are half-float colour and D32S8 depth (world_surfaces.cpp); a draw
// pipeline is built against the pass that owns them.
constexpr VkFormat kSurfaceColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

constexpr size_t kVertexBudgetBytes = 768u * 1024u * 1024u;
constexpr size_t kIndexBudgetBytes = 192u * 1024u * 1024u;
// A level held at the cap evicts and re-uploads every frame (run38 sat at 510/512
// MiB of textures and 255/256 of vertices); the console has the memory to keep it.
constexpr size_t kTextureBudgetBytes = 1536u * 1024u * 1024u;
// One draw writes about 6 KiB of constants (4 KiB is the engine's constant bank
// alone). Busy scenes exceeded the old 32 MiB bank and lost material/post
// draws. Keep room for about eighteen thousand aligned draws per frame.
constexpr size_t kArenaBytes = 128u * 1024u * 1024u;
// Occlusion queries per frame: the histogram's eight bins take one or two draws
// each, and every flare patch one more.
constexpr uint32_t kQueryCapacity = 4096;
constexpr uint32_t kSetsPerPool = 256u;
constexpr uint32_t kTextureSlots = 16u;
constexpr size_t kBlockCount = 10;

// The two stages' bindings, as app/ps5/tools/world-shaders.py shifted them. The
// vertex stage keeps set 0 and the fragment stage owns set 1 because both author
// binding 2: the vertex viewport constants and the fragment alpha test.
constexpr uint32_t kVertexSet = 0;
constexpr uint32_t kFragmentSet = 1;
constexpr uint32_t kFragmentUniformCount = 6; // bindings 0..5

struct MemoryBlock {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    void* mapped = nullptr;
    uint32_t type = 0;
    bool image = false;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> freeRanges;
    ~MemoryBlock() {
        if (mapped) unmapMemory(device, memory);
        if (memory) freeMemory(device, memory, nullptr);
    }
};

struct MemorySlice {
    std::shared_ptr<MemoryBlock> block;
    VkDeviceSize offset = 0, bytes = 0;
    ~MemorySlice() {
        if (!block) return;
        auto& ranges = block->freeRanges;
        ranges.emplace_back(offset, bytes);
        std::sort(ranges.begin(), ranges.end());
        size_t count = 0;
        for (const auto range : ranges) {
            if (count && ranges[count - 1].first + ranges[count - 1].second == range.first)
                ranges[count - 1].second += range.second;
            else ranges[count++] = range;
        }
        ranges.resize(count);
    }
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t bytes = 0;
    std::shared_ptr<MemorySlice> allocation;
    void* mapped = nullptr;
};

// The engine's vertex struct is fourteen float4 attributes; the compiled program
// says which of them it consumes and at which location.
struct VertexAttribute {
    uint32_t semantic = 0;
    uint32_t location = 0;
};

VkDeviceSize alignment(VkDeviceSize value, VkDeviceSize multiple) {
    return multiple <= 1 ? value : (value + multiple - 1) / multiple * multiple;
}

// The engine's fetch scale writes a surface's exponent into the guest's texture
// coordinates; a sampler with a nonzero bias reproduces it without touching the
// shader. Vulkan clamps the bias to the device's limits, which is what D3D11 does
// with a bias past 15 too.
struct SamplerKey {
    uint8_t address[3] = {0, 0, 0};
    uint8_t minLinear = 0, magLinear = 0, mipLinear = 1;
    uint8_t anisotropy = 1, minLevel = 0, maxLevel = 0;
    float bias = 0.0f;
    bool operator==(const SamplerKey&) const = default;
};

struct FragmentSetKey {
    uint32_t mask = 0;
    std::array<VkImageView, kTextureSlots> images{};
    std::array<VkSampler, kTextureSlots> samplers{};
    bool operator==(const FragmentSetKey&) const = default;
};
struct FragmentSetHash {
    size_t operator()(const FragmentSetKey& key) const noexcept {
        size_t value = key.mask;
        for (uint32_t bits = key.mask; bits; bits &= bits - 1) {
            const unsigned slot = std::countr_zero(bits);
            value = value * 131 + std::hash<VkImageView>{}(key.images[slot]);
            value = value * 131 + std::hash<VkSampler>{}(key.samplers[slot]);
        }
        return value;
    }
};

struct PipelineKey {
    uint32_t vertexVariant = 0;
    int fragmentVariant = -1;
    uint32_t colorWrite = 0;
    uint8_t blendEnabled = 0;
    uint32_t srcColor = 0, dstColor = 0;
    uint8_t depthTest = 0, depthWrite = 0;
    uint32_t depthCompare = 0;
    uint8_t stencil = 0, stencilReadMask = 0, stencilWriteMask = 0;
    Policy::StencilFace front{}, back{};
    uint32_t cull = 0;
    uint8_t frontCounterClockwise = 0;
    uint8_t withColor = 0, withDepth = 0;
    bool operator==(const PipelineKey&) const = default;
};

struct PipelineHash {
    size_t operator()(const PipelineKey& key) const noexcept {
        size_t hash = uint32_t(key.fragmentVariant);
        const auto mix = [&](uint64_t value) { hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2); };
        mix(key.vertexVariant);
        mix(key.colorWrite);
        mix(key.blendEnabled);
        mix(key.srcColor);
        mix(key.dstColor);
        mix(key.depthTest);
        mix(key.depthWrite);
        mix(key.depthCompare);
        mix(key.stencil);
        mix(key.stencilReadMask);
        mix(key.stencilWriteMask);
        mix(uint32_t(key.front.compare) | uint32_t(key.front.fail) << 4 | uint32_t(key.front.pass) << 8 |
            uint32_t(key.front.depthFail) << 12);
        mix(uint32_t(key.back.compare) | uint32_t(key.back.fail) << 4 | uint32_t(key.back.pass) << 8 |
            uint32_t(key.back.depthFail) << 12);
        mix(key.cull);
        mix(key.frontCounterClockwise);
        mix(key.withColor);
        mix(key.withDepth);
        return hash;
    }
};

struct SamplerHash {
    size_t operator()(const SamplerKey& key) const noexcept {
        size_t hash = key.address[0];
        for (uint32_t i = 1; i < 3; ++i) hash = hash * 131 + key.address[i];
        hash = hash * 131 + key.minLinear;
        hash = hash * 131 + key.magLinear;
        hash = hash * 131 + key.mipLinear;
        hash = hash * 131 + key.anisotropy;
        hash = hash * 131 + key.minLevel;
        hash = hash * 131 + key.maxLevel;
        return hash ^ std::hash<float>{}(key.bias);
    }
};

} // namespace

struct WorldDrawLayer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    // The name of a device entry point the driver does not provide, for the
    // error the caller reports: a console launch has no stdout to read it from.
    char missing[96] = {};

    VkDescriptorSetLayout vertexLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout fragmentLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    // The constant arena: one host-visible buffer, a bump pointer per frame.
    Buffer arena;
    size_t arenaOffset = 0;
    bool constantReuseEnabled = true;
    bool retainUniformShadows=false;
    uint64_t retainedUniformBlocks=0,retainedUniformBytes=0;
    std::array<size_t,kBlockCount> lastUniformOffsets = [] {std::array<size_t,kBlockCount> a; a.fill(SIZE_MAX); return a;}();
    std::array<size_t,kBlockCount> lastUniformSizes{};
    // A cached copy of each slot's last block. The arena is host-visible
    // coherent memory, which AMD maps write-combined: writing it streams, but
    // reading it back to compare bypasses the cache and dominated the render
    // thread (constants 26%, work/framerate-background-retire-cpu.txt).
    std::array<std::vector<uint8_t>,kBlockCount> lastUniformBytes;
    std::shared_ptr<const Native::WorldDraw> lastVertexUniformOwner,lastFragmentUniformOwner;
    std::array<uint8_t,16> lastFragmentUniformPrefix{};
    const uint8_t* vertexUniformShadow() const {
        return lastVertexUniformOwner?reinterpret_cast<const uint8_t*>(lastVertexUniformOwner->constants.vectors.data()):lastUniformBytes[0].data();
    }
    // The written-vector mask (WorldVertexConstants::written) of the vertex
    // bank last placed in slot 0; zero when unknown.
    std::array<uint64_t,4> lastVertexWritten{};
    // Cleared for the session if a sampled bank has a nonzero vector outside
    // its written mask, which would make the masked compare unsound.
    bool vertexMaskTrusted = true;
    uint64_t vertexMaskChecks = 0;
    uint64_t uniformReuses = 0, descriptorBinds = 0, descriptorReuses = 0;
    // Per uniform slot: blocks requested, compares made against the previous
    // block, and compares that matched. Says which slots' compares pay.
    std::array<uint64_t,kBlockCount> slotRequests{}, slotCompares{}, slotMatches{};
    uint64_t descriptorPass = 0;
    VkDescriptorSet boundVertexSet = VK_NULL_HANDLE, boundFragmentSet = VK_NULL_HANDLE;
    std::array<uint32_t,4> boundVertexOffsets{};
    std::array<uint32_t,6> boundFragmentOffsets{};
    size_t uniformBlock(unsigned slot, const void* prefix, size_t prefixBytes,
                        const void* suffix = nullptr, size_t suffixBytes = 0,
                        bool knownDifferent = false,const std::shared_ptr<const Native::WorldDraw>* owner=nullptr);
    size_t uniformBlockCopied(unsigned slot,const void* prefix,size_t prefixBytes,
        const void* suffix=nullptr,size_t suffixBytes=0,bool knownDifferent=false);
    void materializeUniformShadow(unsigned slot);
    void resetFrameConstants();
    void bindDescriptors(uint64_t pass, VkDescriptorSet vertex, VkDescriptorSet fragment,
                         const std::array<uint32_t,4>& vertexOffsets,
                         const std::array<uint32_t,6>& fragmentOffsets);
    VkDeviceSize uniformAlignment = 256;

    // Per-frame descriptor pools. Sets are never freed: the pools are reset with
    // the arena at the start of the next frame, after the GPU has finished.
    struct Pool {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t used = 0;
    };
    std::vector<Pool> pools;
    VkDescriptorSet frameVertexSet = VK_NULL_HANDLE;
    std::unordered_map<FragmentSetKey, VkDescriptorSet, FragmentSetHash> frameFragmentSets;
    uint64_t vertexSetUpdates = 0, fragmentSetUpdates = 0, fragmentSetHits = 0;
    uint64_t boundPass = 0;
    VkViewport boundViewport{};
    VkRect2D boundScissor{};
    uint32_t boundStencil = 0;
    float boundBias = 0, boundSlope = 0;
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkBuffer boundVertex = VK_NULL_HANDLE, boundIndex = VK_NULL_HANDLE;
    void bindDrawState(uint64_t pass, VkPipeline pipeline, VkBuffer vertex, VkBuffer index,
                       const VkViewport& viewport, const VkRect2D& scissor, uint32_t stencil,
                       float bias, float slope);
    VkDescriptorSet vertexSetForFrame(const char** error);
    VkDescriptorSet fragmentSetForFrame(const FragmentSetKey& key, const char** error);

    // Geometry and indices are cached separately, as the Windows renderer's two
    // maps are: a shared index buffer can outlive the vertex stream it was first
    // drawn with.
    struct Geometry {
        std::shared_ptr<const Native::StoredGeometry> source;
        Buffer buffer;
        uint64_t used = 0;
        uint32_t count = 0;
    };
    struct Indices {
        std::shared_ptr<const Native::StoredGeometry> source;
        Buffer buffer;
        uint64_t used = 0;
        uint32_t maximum = 0;
    };
    std::unordered_map<const Native::StoredGeometry*, Geometry> geometry;
    std::unordered_map<const Native::StoredGeometry*, Indices> indices;
    ResourceFrontCache<Geometry> hotGeometry;
    ResourceFrontCache<Indices> hotIndices;
    size_t geometryBytes = 0, indexBytes = 0;
    size_t geometryTrimCursor=0,indexTrimCursor=0,textureTrimCursor=0;
    bool boundedCacheTrim=false;
    uint64_t use = 0;

    struct Texture {
        std::shared_ptr<MemorySlice> allocation;
        // The image the upload came from stays alive with the GPU copy, so a
        // later ColorImage allocated at the same address cannot inherit it.
        std::shared_ptr<const ColorImage> source;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0, levels = 1, faces = 1;
        size_t bytes = 0;
        uint64_t used = 0;
    };
    std::unordered_map<const ColorImage*, Texture> textures;
    ResourceFrontCache<Texture> hotTextures;
    size_t textureBytes = 0;
    std::vector<std::shared_ptr<MemoryBlock>> memoryBlocks;
    // Uploads are recorded into the frame, so their source buffers must survive
    // until the display waits for that frame at the next BeginFrame.
    std::vector<Buffer> pendingUploads;

    std::unordered_map<SamplerKey, VkSampler, SamplerHash> samplers;
    // A 1x1 transparent image stands in for every slot a program does not sample,
    // so a descriptor set is never left with an unwritten binding.
    Texture dummy;

    std::unordered_map<uint32_t, VkShaderModule> vertexModules; // indexed by variant
    std::unordered_map<uint32_t, VkShaderModule> fragmentModules;
    std::unordered_map<PipelineKey, VkPipeline, PipelineHash> pipelines;

    uint64_t draws = 0, rejected = 0, drawPipelines = 0, uploads = 0, pipelineRejects = 0;

    // The engine's GPU measurements (WorldRendererD3D11::histogram): the
    // exposure histogram's eight bins and the flare patches. D3D11 brackets the
    // draws with one query; Vulkan cannot carry a query across render passes,
    // and every world draw opens its own, so each draw inside a bracket gets a
    // query of its own and the bracket's result is their sum. Results are read
    // at the next BeginFrame, after the display waited for the frame.
    struct RecordedQuery {
        std::shared_ptr<Native::WorldQuery> source;
        uint32_t first = 0, count = 0;
    };
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkQueryControlFlags queryControl = 0;
    uint32_t queryCount = 0;
    std::shared_ptr<Native::WorldQuery> activeQuery;
    uint32_t activeFirst = 0;
    bool activeValid = false;
    std::vector<RecordedQuery> recordedQueries;
    bool nearerIsGreater = true;
    uint64_t queriesPublished = 0, queriesDropped = 0;
    std::vector<std::string> rejects;

    // Entry points, resolved the way the display and the surfaces resolve theirs.
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdSetStencilReference CmdSetStencilReference = nullptr;
    PFN_vkCmdSetDepthBias2EXT CmdSetDepthBias2EXT = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCreateQueryPool CreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
    PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
    PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
    PFN_vkCmdBeginQuery CmdBeginQuery = nullptr;
    PFN_vkCmdEndQuery CmdEndQuery = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;

    bool loadEntryPoints(const char** error);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;
    std::shared_ptr<MemorySlice> allocateMemory(const VkMemoryRequirements& requirements,
                                              uint32_t type, bool image, bool hostVisible);

    bool createBuffer(Buffer& buffer, size_t bytes, VkBufferUsageFlags usage, bool hostVisible,
                      const char** error);
    bool writeBuffer(Buffer& buffer, const void* data, size_t bytes);
    void destroyBuffer(Buffer& buffer);
    bool createImage(Texture& texture, uint32_t width, uint32_t height, uint32_t levels, uint32_t faces,
                     const char** error);
    void destroyTexture(Texture& texture);
    bool createDummyTexture(const char** error);

    bool createLayouts(const char** error);
    bool allocateSet(VkDescriptorSetLayout layout, VkDescriptorSet& set, const char** error);
    size_t reserve(size_t bytes, size_t alignment);

    VkSampler sampler(const SamplerKey& key, const char** error);
    VkShaderModule vertexModule(uint32_t variant, const char** error);
    VkShaderModule fragmentModule(uint32_t variant, const char** error);
    VkPipeline pipeline(const PipelineKey& key, VkRenderPass pass, VkShaderModule vertex,
                        VkShaderModule fragment, const char** error);

    const Geometry* geometryFor(const Native::WorldDraw& draw, const char** error);
    const Indices* indicesFor(const Native::WorldDraw& draw, const char** error);
    const Texture* textureFor(const std::shared_ptr<const ColorImage>& image, const char** error,
                              WorldSurfaces* targets = nullptr);
    // Evicts the least recently used streams and textures back to their budgets.
    // Called at the start of a frame, never while a frame's commands may still
    // name the objects being released.
    void trimCaches();
    void collectQueries();
    void setDepthBias(float constant, float slope);

    bool draw(const Native::WorldDraw& draw, WorldSurfaces& targets, const char** error,
              const std::shared_ptr<const Native::WorldDraw>* owner=nullptr);
};

// ---------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------

bool WorldDrawLayer::Impl::loadEntryPoints(const char** error) {
    PFN_vkGetDeviceProcAddr getDeviceProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!getDeviceProc) {
        *error = "vkGetDeviceProcAddr is unavailable";
        return false;
    }
    bool complete = true;
    auto need = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::decay_t<decltype(slot)>>(getDeviceProc(device, name));
        if (!slot) {
            // The name is the whole diagnosis, and a console launch has no
            // stdout: it goes into the error the caller reports.
            std::snprintf(missing, sizeof missing, "RADV does not provide %s", name);
            *error = missing;
            complete = false;
        }
    };
    need(CreateBuffer, "vkCreateBuffer");
    need(DestroyBuffer, "vkDestroyBuffer");
    need(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    need(BindBufferMemory, "vkBindBufferMemory");
    need(CreateImage, "vkCreateImage");
    need(DestroyImage, "vkDestroyImage");
    need(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    need(CreateImageView, "vkCreateImageView");
    need(DestroyImageView, "vkDestroyImageView");
    need(AllocateMemory, "vkAllocateMemory");
    need(FreeMemory, "vkFreeMemory");
    need(BindImageMemory, "vkBindImageMemory");
    need(MapMemory, "vkMapMemory");
    need(UnmapMemory, "vkUnmapMemory");

    need(CreateSampler, "vkCreateSampler");
    need(DestroySampler, "vkDestroySampler");
    need(CreateShaderModule, "vkCreateShaderModule");
    need(DestroyShaderModule, "vkDestroyShaderModule");
    need(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    need(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    need(CreatePipelineLayout, "vkCreatePipelineLayout");
    need(DestroyPipelineLayout, "vkDestroyPipelineLayout");
    need(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    need(DestroyPipeline, "vkDestroyPipeline");
    need(CreateDescriptorPool, "vkCreateDescriptorPool");
    need(DestroyDescriptorPool, "vkDestroyDescriptorPool");
    need(ResetDescriptorPool, "vkResetDescriptorPool");
    need(AllocateDescriptorSets, "vkAllocateDescriptorSets");
    need(UpdateDescriptorSets, "vkUpdateDescriptorSets");
    need(CmdBindPipeline, "vkCmdBindPipeline");
    need(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    need(CmdSetViewport, "vkCmdSetViewport");
    need(CmdSetScissor, "vkCmdSetScissor");
    need(CmdSetStencilReference, "vkCmdSetStencilReference");
    need(CmdSetDepthBias2EXT, "vkCmdSetDepthBias2EXT");
    need(CmdBindVertexBuffers, "vkCmdBindVertexBuffers");
    need(CmdBindIndexBuffer, "vkCmdBindIndexBuffer");
    need(CmdDrawIndexed, "vkCmdDrawIndexed");
    need(CreateQueryPool, "vkCreateQueryPool");
    need(DestroyQueryPool, "vkDestroyQueryPool");
    need(GetQueryPoolResults, "vkGetQueryPoolResults");
    need(CmdResetQueryPool, "vkCmdResetQueryPool");
    need(CmdBeginQuery, "vkCmdBeginQuery");
    need(CmdEndQuery, "vkCmdEndQuery");
    need(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    need(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    need(DeviceWaitIdle, "vkDeviceWaitIdle");
    // A physical device's memory properties are an instance-level command: the
    // device's dispatch does not answer for it (it is NULL from
    // vkGetDeviceProcAddr, which is what stopped this layer from being created).
    GetMemoryProperties =
        Vk::instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!GetMemoryProperties) {
        std::snprintf(missing, sizeof missing, "RADV does not provide vkGetPhysicalDeviceMemoryProperties");
        *error = missing;
        complete = false;
    }
    return complete;
}

uint32_t WorldDrawLayer::Impl::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties properties{};
    GetMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

std::shared_ptr<MemorySlice> WorldDrawLayer::Impl::allocateMemory(
    const VkMemoryRequirements& requirements, uint32_t type, bool image, bool hostVisible) {
    if (type == UINT32_MAX) return {};
    auto take = [&](const std::shared_ptr<MemoryBlock>& block) -> std::shared_ptr<MemorySlice> {
        if (block->type != type || block->image != image || (hostVisible && !block->mapped)) return {};
        auto& ranges = block->freeRanges;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto [start, size] = ranges[i];
            const VkDeviceSize offset = alignment(start, requirements.alignment);
            if (offset - start > size || requirements.size > size - (offset - start)) continue;
            auto slice = std::make_shared<MemorySlice>();
            slice->block = block;
            slice->offset = offset;
            slice->bytes = requirements.size;
            ranges.erase(ranges.begin() + i);
            if (offset > start) ranges.emplace_back(start, offset - start);
            const VkDeviceSize end = offset + requirements.size;
            if (end < start + size) ranges.emplace_back(end, start + size - end);
            return slice;
        }
        return {};
    };
    for (auto& block : memoryBlocks)
        if (auto slice = take(block)) return slice;

    // Thousands of separate small allocations consume the console's page tables
    // even when physical memory is plentiful. Pack resources into larger blocks;
    // keep optimal images separate from buffers for bufferImageGranularity.
    auto block = std::make_shared<MemoryBlock>();
    block->device = device;
    block->freeMemory = FreeMemory;
    block->unmapMemory = UnmapMemory;
    block->type = type;
    block->image = image;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = alignment((std::max)(requirements.size, VkDeviceSize(16u << 20)),
                                    requirements.alignment);
    info.memoryTypeIndex = type;
    if (AllocateMemory(device, &info, nullptr, &block->memory) != VK_SUCCESS) return {};
    if (hostVisible && MapMemory(device, block->memory, 0, info.allocationSize, 0, &block->mapped) != VK_SUCCESS)
        return {};
    block->freeRanges.emplace_back(0, info.allocationSize);
    memoryBlocks.push_back(block);
    return take(block);
}

bool WorldDrawLayer::Impl::createBuffer(Buffer& buffer, size_t bytes, VkBufferUsageFlags usage,
                                        bool hostVisible, const char** error) {
    if (!bytes) {
        *error = "an empty buffer";
        return false;
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (CreateBuffer(device, &info, nullptr, &buffer.buffer) != VK_SUCCESS) {
        *error = "vkCreateBuffer failed for world geometry";
        return false;
    }
    VkMemoryRequirements requirements{};
    GetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const VkMemoryPropertyFlags wanted =
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const uint32_t type = memoryType(requirements.memoryTypeBits, wanted);
    buffer.allocation = allocateMemory(requirements, type, false, hostVisible);
    if (!buffer.allocation) {
        *error = "the world geometry's GPU memory could not be allocated";
        destroyBuffer(buffer);
        return false;
    }
    buffer.memory = buffer.allocation->block->memory;
    if (hostVisible) buffer.mapped = static_cast<uint8_t*>(buffer.allocation->block->mapped) + buffer.allocation->offset;
    if (BindBufferMemory(device, buffer.buffer, buffer.memory, buffer.allocation->offset) != VK_SUCCESS) {
        *error = "vkBindBufferMemory failed for world geometry";
        destroyBuffer(buffer);
        return false;
    }
    buffer.bytes = bytes;
    return true;
}

bool WorldDrawLayer::Impl::writeBuffer(Buffer& buffer, const void* data, size_t bytes) {
    if (bytes > buffer.bytes) return false;
    if (!buffer.mapped) return false;
    std::memcpy(buffer.mapped, data, bytes);
    return true;
}

void WorldDrawLayer::Impl::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer) DestroyBuffer(device, buffer.buffer, nullptr);
    buffer = {};
}

bool WorldDrawLayer::Impl::createImage(Texture& texture, uint32_t width, uint32_t height, uint32_t levels,
                                       uint32_t faces, const char** error) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kTextureFormat;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = faces;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (faces == 6) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (CreateImage(device, &info, nullptr, &texture.image) != VK_SUCCESS) {
        *error = "vkCreateImage failed for a world texture";
        return false;
    }
    VkMemoryRequirements requirements{};
    GetImageMemoryRequirements(device, texture.image, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    texture.allocation = allocateMemory(requirements, type, true, false);
    if (!texture.allocation) {
        *error = "the world texture's GPU memory could not be allocated";
        destroyTexture(texture);
        return false;
    }
    texture.memory = texture.allocation->block->memory;
    if (BindImageMemory(device, texture.image, texture.memory, texture.allocation->offset) != VK_SUCCESS) {
        *error = "vkBindImageMemory failed for a world texture";
        destroyTexture(texture);
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = texture.image;
    view.viewType = faces == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = kTextureFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, faces};
    if (CreateImageView(device, &view, nullptr, &texture.view) != VK_SUCCESS) {
        *error = "vkCreateImageView failed for a world texture";
        destroyTexture(texture);
        return false;
    }
    texture.width = width;
    texture.height = height;
    texture.levels = levels;
    texture.faces = faces;
    // A 2D image is one layer; a cube keeps its six in the same allocation.
    texture.bytes = size_t(width) * height * faces * 4u * 4u / 3u;
    return true;
}

void WorldDrawLayer::Impl::destroyTexture(Texture& texture) {
    if (texture.view) DestroyImageView(device, texture.view, nullptr);
    if (texture.image) DestroyImage(device, texture.image, nullptr);
    texture = {};
}

bool WorldDrawLayer::Impl::createDummyTexture(const char** error) {
    if (!createImage(dummy, 1, 1, 1, 1, error)) return false;
    // The stand-in is transparent black. It is only ever read by a shader that
    // declares the binding but no draw bound it, which the engine's programs do
    // not do; the zeroes keep that accident invisible and the descriptor legal.
    return true;
}

bool WorldDrawLayer::Impl::createLayouts(const char** error) {
    VkDescriptorSetLayoutBinding vertex[4]{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
        vertex[binding].binding = binding;
        vertex[binding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        vertex[binding].descriptorCount = 1;
        vertex[binding].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo vertexInfo{};
    vertexInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    vertexInfo.bindingCount = 4;
    vertexInfo.pBindings = vertex;
    if (CreateDescriptorSetLayout(device, &vertexInfo, nullptr, &vertexLayout) != VK_SUCCESS) {
        *error = "vkCreateDescriptorSetLayout failed for the vertex stage";
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, kFragmentUniformCount + 2 * kTextureSlots> fragment{};
    uint32_t count = 0;
    for (uint32_t binding = 0; binding < kFragmentUniformCount; ++binding) {
        auto& entry = fragment[count++];
        entry.binding = binding;
        entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        entry.descriptorCount = 1;
        entry.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        auto& image = fragment[count++];
        image.binding = 16u + slot;
        image.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        image.descriptorCount = 1;
        image.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        auto& sampler = fragment[count++];
        sampler.binding = 32u + slot;
        sampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        sampler.descriptorCount = 1;
        sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo fragmentInfo{};
    fragmentInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    fragmentInfo.bindingCount = count;
    fragmentInfo.pBindings = fragment.data();
    if (CreateDescriptorSetLayout(device, &fragmentInfo, nullptr, &fragmentLayout) != VK_SUCCESS) {
        *error = "vkCreateDescriptorSetLayout failed for the fragment stage";
        return false;
    }

    const VkDescriptorSetLayout layouts[2] = {vertexLayout, fragmentLayout};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 2;
    layout.pSetLayouts = layouts;
    if (CreatePipelineLayout(device, &layout, nullptr, &pipelineLayout) != VK_SUCCESS) {
        *error = "vkCreatePipelineLayout failed for the world draw";
        return false;
    }
    return true;
}

bool WorldDrawLayer::Impl::allocateSet(VkDescriptorSetLayout layout, VkDescriptorSet& set,
                                       const char** error) {
    for (auto& pool : pools) {
        if (pool.used >= kSetsPerPool) continue;
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = pool.pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        if (AllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) continue;
        ++pool.used;
        return true;
    }
    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = kSetsPerPool * 10u;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = kSetsPerPool * kTextureSlots;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[2].descriptorCount = kSetsPerPool * kTextureSlots;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kSetsPerPool;
    info.poolSizeCount = 3;
    info.pPoolSizes = sizes;
    Pool pool;
    if (CreateDescriptorPool(device, &info, nullptr, &pool.pool) != VK_SUCCESS) {
        *error = "the world draw descriptor pools are exhausted";
        return false;
    }
    pools.push_back(pool);
    return allocateSet(layout, set, error);
}

size_t WorldDrawLayer::Impl::reserve(size_t bytes, size_t align) {
    const size_t offset = size_t(alignment(arenaOffset, align));
    if (offset + bytes > arena.bytes) return SIZE_MAX;
    arenaOffset = offset + bytes;
    return offset;
}

// ---------------------------------------------------------------------------
// Samplers, modules and pipelines
// ---------------------------------------------------------------------------

VkSampler WorldDrawLayer::Impl::sampler(const SamplerKey& key, const char** error) {
    if (const auto found = samplers.find(key); found != samplers.end()) return found->second;
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = key.magLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = key.minLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = key.mipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VkSamplerAddressMode(Policy::addressMode(key.address[0]));
    info.addressModeV = VkSamplerAddressMode(Policy::addressMode(key.address[1]));
    info.addressModeW = VkSamplerAddressMode(Policy::addressMode(key.address[2]));
    info.mipLodBias = key.bias;
    info.minLod = float(key.minLevel);
    info.maxLod = float(key.maxLevel);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (key.anisotropy > 1) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = float(key.anisotropy);
    }
    VkSampler result = VK_NULL_HANDLE;
    if (CreateSampler(device, &info, nullptr, &result) != VK_SUCCESS) {
        *error = "vkCreateSampler failed for a world texture";
        return VK_NULL_HANDLE;
    }
    samplers.emplace(key, result);
    return result;
}

VkShaderModule WorldDrawLayer::Impl::vertexModule(uint32_t variant, const char** error) {
    if (const auto found = vertexModules.find(variant); found != vertexModules.end()) return found->second;
    const WorldShaderModule entry = worldVertexShaderAt(variant);
    if (!entry) {
        *error = "the vertex program index is outside the corpus";
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = entry.wordCount * 4u;
    info.pCode = entry.words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (CreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        *error = "vkCreateShaderModule failed for a world vertex program";
        return VK_NULL_HANDLE;
    }
    vertexModules.emplace(variant, module);
    return module;
}

VkShaderModule WorldDrawLayer::Impl::fragmentModule(uint32_t variant, const char** error) {
    if (const auto found = fragmentModules.find(variant); found != fragmentModules.end()) return found->second;
    const WorldShaderModule module = worldFragmentShaderAt(variant);
    if (!module) {
        *error = "no fragment program for this name and flags";
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = module.wordCount * 4u;
    info.pCode = module.words;
    VkShaderModule created = VK_NULL_HANDLE;
    if (CreateShaderModule(device, &info, nullptr, &created) != VK_SUCCESS) {
        *error = "vkCreateShaderModule failed for a world fragment program";
        return VK_NULL_HANDLE;
    }
    fragmentModules.emplace(variant, created);
    return created;
}

VkPipeline WorldDrawLayer::Impl::pipeline(const PipelineKey& key, VkRenderPass pass,
                                          VkShaderModule vertex, VkShaderModule fragment,
                                          const char** error) {
    if (const auto found = pipelines.find(key); found != pipelines.end()) return found->second;

    // The attribute table is the compiled program's, not the vertex struct's:
    // the template declares every input and glslang keeps only the used ones.
    const WorldVertexInputs inputs = worldVertexInputsAt(key.vertexVariant);
    if (!inputs.count) {
        *error = "the vertex program declares no inputs";
        return VK_NULL_HANDLE;
    }
    std::array<VkVertexInputAttributeDescription, 15> attributes{};
    for (uint32_t i = 0; i < inputs.count; ++i) {
        attributes[i].location = inputs.entries[i][1];
        attributes[i].binding = 0;
        attributes[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[i].offset = uint32_t(inputs.entries[i][0]) * 16u;
    }
    VkVertexInputBindingDescription binding{0, uint32_t(sizeof(Native::WorldVertex)),
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = inputs.count;
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = key.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE;
    // Depth bias is dynamic; the enable is fixed so a draw can set any bias
    // without rebuilding the pipeline (zero bias is a no-op).
    raster.depthBiasEnable = VK_TRUE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = key.depthTest;
    depth.depthWriteEnable = key.depthWrite;
    depth.depthCompareOp = VkCompareOp(key.depthCompare);
    depth.stencilTestEnable = key.stencil;
    depth.front = {VkStencilOp(key.front.fail), VkStencilOp(key.front.pass),
                   VkStencilOp(key.front.depthFail), VkCompareOp(key.front.compare),
                   key.stencilReadMask, key.stencilWriteMask, 0};
    depth.back = {VkStencilOp(key.back.fail), VkStencilOp(key.back.pass),
                  VkStencilOp(key.back.depthFail), VkCompareOp(key.back.compare),
                  key.stencilReadMask, key.stencilWriteMask, 0};

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = key.blendEnabled;
    blend.srcColorBlendFactor = VkBlendFactor(key.srcColor);
    blend.dstColorBlendFactor = VkBlendFactor(key.dstColor);
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VkBlendFactor(Policy::alphaBlendFactor(Policy::BlendFactor(key.srcColor)));
    blend.dstAlphaBlendFactor = VkBlendFactor(Policy::alphaBlendFactor(Policy::BlendFactor(key.dstColor)));
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = key.colorWrite;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = key.withColor ? 1u : 0u;
    blending.pAttachments = &blend;

    constexpr VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 4;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "rasterMain";
    uint32_t stageCount = 1;
    if (fragment) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "pixelMain";
        stageCount = 2;
    }
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout;
    // A pipeline is tied to the attachments' formats, so it names the pass it
    // will be recorded into; the framebuffer that carries them comes with the
    // draw. RADV on the console does not take Vulkan 1.3 dynamic rendering.
    info.renderPass = pass;
    info.subpass = 0;

    VkPipeline created = VK_NULL_HANDLE;
    if (CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &created) != VK_SUCCESS) {
        *error = "vkCreateGraphicsPipelines failed for a world draw";
        ++pipelineRejects;
        return VK_NULL_HANDLE;
    }
    ++drawPipelines;
    pipelines.emplace(key, created);
    return created;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

const WorldDrawLayer::Impl::Geometry* WorldDrawLayer::Impl::geometryFor(const Native::WorldDraw& draw,
                                                                       const char** error) {
    const auto* source = draw.geometry.vertices.get();
    if (!source) {
        *error = "the draw has no vertex stream";
        return nullptr;
    }
    if (auto* found = hotGeometry.find(geometry, source)) {
        found->used = ++use;
        return found;
    }

    std::vector<Native::WorldVertex> vertices;
    if (!Native::decodeWorldVertices(*source, vertices) || vertices.empty()) {
        *error = "the vertex stream did not decode";
        return nullptr;
    }

    Geometry entry;
    entry.source = draw.geometry.vertices;
    entry.count = uint32_t(vertices.size());
    const size_t bytes = vertices.size() * sizeof(Native::WorldVertex);
    if (!createBuffer(entry.buffer, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, error)) return nullptr;
    if (!writeBuffer(entry.buffer, vertices.data(), bytes)) {
        *error = "the vertex stream could not be written";
        destroyBuffer(entry.buffer);
        return nullptr;
    }
    entry.used = ++use;
    geometryBytes += bytes;
    ++uploads;
    return hotGeometry.remember(geometry.emplace(source, std::move(entry)).first->second);
}

const WorldDrawLayer::Impl::Indices* WorldDrawLayer::Impl::indicesFor(const Native::WorldDraw& draw,
                                                                     const char** error) {
    const auto* source = draw.geometry.indices.get();
    if (!source) {
        *error = "the draw has no index stream";
        return nullptr;
    }
    if (auto* found = hotIndices.find(indices, source)) {
        found->used = ++use;
        return found;
    }
    if (source->indices.empty()) {
        *error = "the index stream is empty";
        return nullptr;
    }

    Indices entry;
    entry.source = draw.geometry.indices;
    entry.maximum = *std::max_element(source->indices.begin(), source->indices.end());
    const size_t bytes = source->indices.size() * sizeof(uint16_t);
    if (!createBuffer(entry.buffer, bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, error)) return nullptr;
    if (!writeBuffer(entry.buffer, source->indices.data(), bytes)) {
        *error = "the index stream could not be written";
        destroyBuffer(entry.buffer);
        return nullptr;
    }
    entry.used = ++use;
    indexBytes += bytes;
    ++uploads;
    return hotIndices.remember(indices.emplace(source, std::move(entry)).first->second);
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static std::vector<VkBufferImageCopy> textureCopies(const ColorImage& image, size_t& stagingBytes,
                                                   std::array<size_t, 16>& levelOffset) {
    stagingBytes = 0;
    levelOffset.fill(SIZE_MAX);
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t mip = 0; mip <= image.mips.size() && mip < 16; ++mip) {
        const auto& pixels = mip ? image.mips[mip - 1] : image.pixels;
        if (pixels.empty()) continue;
        VkBufferImageCopy copy{};
        levelOffset[mip] = copy.bufferOffset = stagingBytes;
        // The mips vector has absolute authored indices, including empty
        // nonresident levels. Do not add firstMip to those indices again.
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, image.faces};
        copy.imageExtent = {std::max(1u, image.width >> mip), std::max(1u, image.height >> mip), 1};
        copies.push_back(copy);
        stagingBytes += pixels.size();
    }
    return copies;
}

const WorldDrawLayer::Impl::Texture* WorldDrawLayer::Impl::textureFor(
    const std::shared_ptr<const ColorImage>& source, const char** error, WorldSurfaces* targets) {
    if (!source) {
        *error = "the sampled texture has no pixels";
        return nullptr;
    }
    const ColorImage& image = *source;
    if (auto* found = hotTextures.find(textures, &image)) {
        found->used = ++use;
        return found;
    }
    if (!image.valid()) {
        *error = "the sampled texture has no pixels";
        return nullptr;
    }
    // Upload copies and transitions must be outside the current render pass.
    // Do this only on a cache miss, avoiding a separate contains() every draw.
    if (targets) targets->FlushDraws();

    // The image owns its base level and, when it was decoded with its authored
    // levels, every lower level; a level the image does not own is never sampled
    // because the sampler's LOD range stops at the last uploaded one.
    const uint32_t owned = uint32_t(image.mips.size()) + 1u;
    const uint32_t levels = (std::max)(1u, (std::min)(owned, 16u));
    Texture entry;
    entry.source = source;
    if (!createImage(entry, image.width, image.height, levels, image.faces, error)) return nullptr;

    // One staging buffer holds every owned level, one copy per level; the image
    // becomes shader-readable in one barrier at the end.
    Buffer staging;
    size_t stagingBytes = 0;
    std::array<size_t, 16> levelOffset{};
    const auto copies = textureCopies(image, stagingBytes, levelOffset);
    if (copies.empty()) {
        *error = "the sampled texture has no levels";
        destroyTexture(entry);
        return nullptr;
    }
    if (!createBuffer(staging, stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error)) {
        destroyTexture(entry);
        return nullptr;
    }
    void* mapped = staging.mapped;
    if (!mapped) {
        *error = "the texture staging buffer could not be mapped";
        destroyBuffer(staging);
        destroyTexture(entry);
        return nullptr;
    }
    for (uint32_t level = 0; level <= image.mips.size() && level < 16; ++level) {
        const auto& data = level ? image.mips[level - 1] : image.pixels;
        if (data.empty() || levelOffset[level] == SIZE_MAX) continue;
        std::memcpy(static_cast<uint8_t*>(mapped) + levelOffset[level], data.data(), data.size());
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, image.faces};
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
    CmdCopyBufferToImage(command, staging.buffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        uint32_t(copies.size()), copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
    pendingUploads.push_back(staging);

    entry.used = ++use;
    textureBytes += entry.bytes;
    ++uploads;
    return hotTextures.remember(textures.emplace(&image, std::move(entry)).first->second);
}

void WorldDrawLayer::Impl::collectQueries() {
    // The display waited for the previous frame, so its queries are final; one
    // that is not (a frame that was never submitted) keeps its last result, as
    // the Windows renderer never publishes an incomplete measurement.
    std::vector<uint64_t> values;
    for (const RecordedQuery& query : recordedQueries) {
        // A bracket with no world draw in it measured nothing this layer drew
        // (its draws, if any, went another way), so it publishes nothing rather
        // than a zero the exposure would read as a black scene.
        if (!query.count) {
            if (queriesDropped++ < 8) std::printf("[World] measurement enclosed no world draw\n");
            continue;
        }
        uint64_t samples = 0;
        values.assign(query.count, 0);
        if (GetQueryPoolResults(device, queryPool, query.first, query.count, values.size() * sizeof(uint64_t),
                                values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            ++queriesDropped;
            continue;
        }
        for (uint64_t value : values) samples += value;
        query.source->result->samples.store(samples, std::memory_order_release);
        ++queriesPublished;
    }
    recordedQueries.clear();
    // A bracket open across frames loses its measurement: its earlier draws'
    // queries are about to be reset.
    activeValid = false;
    queryCount = 0;
    if (queryPool && command) CmdResetQueryPool(command, queryPool, 0, kQueryCapacity);
}

void WorldDrawLayer::Impl::trimCaches() {
    hotGeometry.clear(); hotIndices.clear(); hotTextures.clear();
    // Only between frames: an image or buffer a draw named in the frame that is
    // still executing must not be released under it.
    //
    // An entry whose source only this cache still holds can never be looked up
    // again: the key is the source's address, and the engine has let go of it.
    // Immediate streams become eligible after retirement. Sweep them within
    // eight stable-capacity frames, or immediately when over budget, instead of
    // scanning every retained level resource each frame. Past that, the least
    // recently used go first,
    // sorted once rather than searched for per eviction (run39 spent whole
    // frames in that search once the cache held tens of thousands of entries).
    trimResourceCache(geometry, geometryBytes, kVertexBudgetBytes, geometryTrimCursor, [](const Geometry& entry) { return entry.buffer.bytes; },
         [this](Geometry& entry) { destroyBuffer(entry.buffer); },boundedCacheTrim);
    trimResourceCache(indices, indexBytes, kIndexBudgetBytes, indexTrimCursor, [](const Indices& entry) { return entry.buffer.bytes; },
         [this](Indices& entry) { destroyBuffer(entry.buffer); },boundedCacheTrim);
    trimResourceCache(textures, textureBytes, kTextureBudgetBytes, textureTrimCursor, [](const Texture& entry) { return entry.bytes; },
         [this](Texture& entry) { destroyTexture(entry); },boundedCacheTrim);
}

// ---------------------------------------------------------------------------
// The draw
// ---------------------------------------------------------------------------

namespace {

// The ten constant blocks one draw binds, in shader order: the vertex stage's
// four and the fragment stage's six. The sizes are their HLSL cbuffers'.
constexpr size_t kVertexConstantBytes = 4096; // float4 vectors[256]
constexpr size_t kReferenceBytes = 144;       // uint4 references[9]
constexpr size_t kViewportBytes = 32;         // depth range + prompt transform
constexpr size_t kClipBytes = 112;            // six planes + control
constexpr size_t kFragmentConstantBytes = 1024; // float4 fragmentConstants[64]
constexpr size_t kScaleBytes = 256;           // float4 scales[16]
constexpr size_t kAlphaBytes = 16;
constexpr size_t kLookupBytes = 16;
constexpr size_t kBloomBytes = 16;
constexpr size_t kShadowBytes = 16;

constexpr size_t kBlockBytes[kBlockCount] = {
    kVertexConstantBytes, kReferenceBytes, kViewportBytes, kClipBytes,
    kFragmentConstantBytes, kScaleBytes, kAlphaBytes, kLookupBytes, kBloomBytes, kShadowBytes,
};

struct ViewportConstants {
    Native::EngineVector depthRange;
    Native::EngineVector promptTransform;
};
static_assert(sizeof(ViewportConstants) == kViewportBytes);

struct ClipConstants {
    std::array<Native::EngineVector, 6> planes;
    std::array<uint32_t, 4> control;
};
static_assert(sizeof(ClipConstants) == kClipBytes);

struct AlphaTestConstants {
    uint32_t function;
    float reference;
    uint32_t padding[2];
};
static_assert(sizeof(AlphaTestConstants) == kAlphaBytes);

struct ColorLookupConstants {
    float inverseLookupScale;
    uint32_t logicalLookupMask;
    uint32_t padding[2];
};
static_assert(sizeof(ColorLookupConstants) == kLookupBytes);

struct BloomConstants {
    float renderScale;
    uint32_t snap;
    uint32_t padding[2];
};
static_assert(sizeof(BloomConstants) == kBloomBytes);

struct ShadowConstants {
    uint32_t scale;
    uint32_t padding[3];
};
static_assert(sizeof(ShadowConstants) == kShadowBytes);
} // namespace

void WorldDrawLayer::Impl::resetFrameConstants() {
    arenaOffset = 0;
    lastUniformOffsets.fill(SIZE_MAX);
    lastUniformSizes.fill(0);
    lastVertexUniformOwner.reset();lastFragmentUniformOwner.reset();
    descriptorPass = 0;
    boundVertexSet = boundFragmentSet = VK_NULL_HANDLE;
}

size_t WorldDrawLayer::Impl::uniformBlockCopied(unsigned slot, const void* prefix, size_t prefixBytes,
                                         const void* suffix, size_t suffixBytes, bool knownDifferent) {
    if (slot >= kBlockCount || !arena.mapped || (!prefix && prefixBytes) || (!suffix && suffixBytes) ||
        suffixBytes > SIZE_MAX-prefixBytes) return SIZE_MAX;
    const size_t bytes = prefixBytes+suffixBytes;
    auto* memory = static_cast<uint8_t*>(arena.mapped);
    const size_t previous = lastUniformOffsets[slot];
    std::vector<uint8_t>& shadow = lastUniformBytes[slot];
    ++slotRequests[slot];
    if (!knownDifferent && constantReuseEnabled && previous != SIZE_MAX && lastUniformSizes[slot] == bytes && shadow.size() == bytes) {
        ++slotCompares[slot];
        if ((!prefixBytes || std::memcmp(shadow.data(),prefix,prefixBytes)==0) &&
            (!suffixBytes || std::memcmp(shadow.data()+prefixBytes,suffix,suffixBytes)==0)) {
            ++slotMatches[slot];
            ++uniformReuses;
            return previous;
        }
    }
    const size_t offset = reserve(bytes,uniformAlignment);
    if (offset == SIZE_MAX) return SIZE_MAX;
    shadow.resize(bytes);
    if (prefixBytes) std::memcpy(shadow.data(),prefix,prefixBytes);
    if (suffixBytes) std::memcpy(shadow.data()+prefixBytes,suffix,suffixBytes);
    std::memcpy(memory+offset,shadow.data(),bytes);
    lastUniformOffsets[slot] = offset;
    lastUniformSizes[slot] = bytes;
    return offset;
}

void WorldDrawLayer::Impl::materializeUniformShadow(unsigned slot) {
    if(slot==0 && lastVertexUniformOwner) {
        auto& bytes=lastUniformBytes[0];bytes.resize(4096);
        std::memcpy(bytes.data(),lastVertexUniformOwner->constants.vectors.data(),4096);
        lastVertexUniformOwner.reset();
    } else if(slot==4 && lastFragmentUniformOwner) {
        auto& bytes=lastUniformBytes[4];bytes.resize(1024);
        std::memcpy(bytes.data(),lastFragmentUniformPrefix.data(),16);
        std::memcpy(bytes.data()+16,lastFragmentUniformOwner->fragmentConstants.data()+1,1008);
        lastFragmentUniformOwner.reset();
    }
}
size_t WorldDrawLayer::Impl::uniformBlock(unsigned slot,const void* prefix,size_t prefixBytes,
    const void* suffix,size_t suffixBytes,bool knownDifferent,const std::shared_ptr<const Native::WorldDraw>* owner) {
    const bool vertex=slot==0 && prefixBytes==4096 && !suffixBytes;
    const bool fragment=slot==4 && prefixBytes==16 && suffixBytes==1008;
    const bool owned=retainUniformShadows && (vertex || fragment) && owner && *owner && Native::QueuedWorldDrawAccess::owned(**owner) &&
        (vertex?prefix==(*owner)->constants.vectors.data():suffix==(*owner)->fragmentConstants.data()+1);
    if(!owned) {
        materializeUniformShadow(slot);
        return uniformBlockCopied(slot,prefix,prefixBytes,suffix,suffixBytes,knownDifferent);
    }
    if(!arena.mapped || (!prefix && prefixBytes) || (!suffix && suffixBytes))return SIZE_MAX;
    const size_t bytes=prefixBytes+suffixBytes,previous=lastUniformOffsets[slot];
    ++slotRequests[slot];
    if(!knownDifferent && constantReuseEnabled && previous!=SIZE_MAX && lastUniformSizes[slot]==bytes) {
        ++slotCompares[slot];bool equal=false;
        if(vertex && (lastVertexUniformOwner || lastUniformBytes[0].size()==4096))
            equal=std::memcmp(vertexUniformShadow(),prefix,4096)==0;
        else if(fragment && lastFragmentUniformOwner)
            equal=std::memcmp(lastFragmentUniformPrefix.data(),prefix,16)==0 &&
                std::memcmp(lastFragmentUniformOwner->fragmentConstants.data()+1,suffix,1008)==0;
        else if(lastUniformBytes[slot].size()==bytes)
            equal=std::memcmp(lastUniformBytes[slot].data(),prefix,prefixBytes)==0 &&
                std::memcmp(lastUniformBytes[slot].data()+prefixBytes,suffix,suffixBytes)==0;
        if(equal) {++slotMatches[slot];++uniformReuses;return previous;}
    }
    const size_t offset=reserve(bytes,uniformAlignment);
    if(offset==SIZE_MAX)return SIZE_MAX;
    auto* target=static_cast<uint8_t*>(arena.mapped)+offset;
    if(vertex) {lastVertexUniformOwner=*owner;std::memcpy(target,prefix,4096);}
    else {
        std::memcpy(lastFragmentUniformPrefix.data(),prefix,16);lastFragmentUniformOwner=*owner;
        std::memcpy(target,prefix,16);std::memcpy(target+16,suffix,1008);
    }
    ++retainedUniformBlocks;retainedUniformBytes+=vertex?4096:1008;
    lastUniformOffsets[slot]=offset;lastUniformSizes[slot]=bytes;return offset;
}

void WorldDrawLayer::Impl::bindDescriptors(uint64_t pass, VkDescriptorSet vertex, VkDescriptorSet fragment,
    const std::array<uint32_t,4>& vertexOffsets, const std::array<uint32_t,6>& fragmentOffsets) {
    if (descriptorPass != pass) boundVertexSet = boundFragmentSet = VK_NULL_HANDLE;
    const bool changeVertex = boundVertexSet != vertex || boundVertexOffsets != vertexOffsets;
    const bool changeFragment = fragment && (boundFragmentSet != fragment || boundFragmentOffsets != fragmentOffsets);
    const VkDescriptorSet sets[]{vertex,fragment};
    if (changeVertex && changeFragment) {
        std::array<uint32_t,10> offsets;
        std::copy(vertexOffsets.begin(),vertexOffsets.end(),offsets.begin());
        std::copy(fragmentOffsets.begin(),fragmentOffsets.end(),offsets.begin()+4);
        CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,2,sets,10,offsets.data());
    } else if (changeVertex) {
        CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,sets,4,vertexOffsets.data());
    } else if (changeFragment) {
        CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,1,1,sets+1,6,fragmentOffsets.data());
    }
    if (changeVertex || changeFragment) ++descriptorBinds; else ++descriptorReuses;
    descriptorPass = pass;
    boundVertexSet = vertex; boundVertexOffsets = vertexOffsets;
    if (fragment) {boundFragmentSet = fragment;boundFragmentOffsets = fragmentOffsets;}
}

namespace {
// WorldVertexOptions encoded as WorldVertexShaderD3D11::shaderKey does and as
// app/ps5/tools/world-shaders.py emits: six option bytes, then eight stages of
// mode/coordinate/conversion/matrix, then the raster form (0 plain, 1 clip
// distance, 2 cull distance).
void encodeVertexKey(const Native::WorldVertexOptions& options, uint32_t clip, uint8_t key[39]) {
    uint32_t at = 0;
    key[at++] = uint8_t(options.weights);
    key[at++] = uint8_t(options.positionConversion);
    key[at++] = uint8_t(options.normal);
    key[at++] = uint8_t(options.tangents);
    key[at++] = uint8_t(options.normalizeNormal);
    key[at++] = uint8_t(options.vertexColor);
    for (uint32_t stage = 0; stage < 8; ++stage) {
        key[at++] = options.modes[stage];
        key[at++] = options.coordinates[stage];
        key[at++] = uint8_t(options.conversions[stage]);
        key[at++] = uint8_t(options.matrices[stage]);
    }
    key[at++] = uint8_t(clip);
}

// The effective fragment name: a motion or lighting submission may have no
// captured program and falls back to the engine's own, as the Windows renderer
// does before it looks the program up.
std::string_view effectiveFragmentName(const Native::WorldDraw& draw) {
    if (!draw.fragmentName.empty()) return draw.fragmentName;
    if (draw.material == Native::WorldMaterial::motion) return "XRShader_MotionMap";
    if (draw.material == Native::WorldMaterial::ndsp) return "XRShader_FP20_NDSP";
    return {};
}

} // namespace

void WorldDrawLayer::Impl::setDepthBias(float constant, float slope) {
    VkDepthBiasRepresentationInfoEXT representation{};
    representation.sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT;
    representation.depthBiasRepresentation =
        VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
    representation.depthBiasExact = VK_TRUE;
    VkDepthBiasInfoEXT bias{};
    bias.sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT;
    bias.pNext = &representation;
    bias.depthBiasConstantFactor = constant;
    bias.depthBiasSlopeFactor = slope;
    CmdSetDepthBias2EXT(command, &bias);
}

VkDescriptorSet WorldDrawLayer::Impl::vertexSetForFrame(const char** error) {
    if (frameVertexSet) return frameVertexSet;
    VkDescriptorSet allocated = VK_NULL_HANDLE;
    if (!allocateSet(vertexLayout, allocated, error)) return VK_NULL_HANDLE;
    frameVertexSet = allocated;
    VkDescriptorBufferInfo buffers[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        buffers[i] = {arena.buffer, 0, kBlockBytes[i]};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frameVertexSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &buffers[i];
    }
    UpdateDescriptorSets(device, 4, writes, 0, nullptr);
    ++vertexSetUpdates;
    return frameVertexSet;
}

void WorldDrawLayer::Impl::bindDrawState(uint64_t pass, VkPipeline pipeline, VkBuffer vertex, VkBuffer index,
                                        const VkViewport& viewport, const VkRect2D& scissor, uint32_t stencil,
                                        float bias, float slope) {
    // A new pass may follow a UI/resolve pipeline, so invalidate every cached
    // binding there. Within a batch only emit states that actually change.
    const bool fresh = boundPass != pass;
    if (fresh || std::memcmp(&boundViewport, &viewport, sizeof(viewport))) CmdSetViewport(command, 0, 1, &viewport);
    if (fresh || std::memcmp(&boundScissor, &scissor, sizeof(scissor))) CmdSetScissor(command, 0, 1, &scissor);
    if (fresh || boundStencil != stencil) CmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, stencil);
    if (fresh || boundBias != bias || boundSlope != slope) setDepthBias(bias, slope);
    if (fresh || boundPipeline != pipeline) CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (fresh || boundVertex != vertex) {
        const VkDeviceSize offset = 0;
        CmdBindVertexBuffers(command, 0, 1, &vertex, &offset);
    }
    if (fresh || boundIndex != index) CmdBindIndexBuffer(command, index, 0, VK_INDEX_TYPE_UINT16);
    boundPass = pass; boundViewport = viewport; boundScissor = scissor;
    boundStencil = stencil; boundBias = bias; boundSlope = slope;
    boundPipeline = pipeline; boundVertex = vertex; boundIndex = index;
}

VkDescriptorSet WorldDrawLayer::Impl::fragmentSetForFrame(const FragmentSetKey& key, const char** error) {
    if (auto found = frameFragmentSets.find(key); found != frameFragmentSets.end()) {
        ++fragmentSetHits;
        return found->second;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocateSet(fragmentLayout, set, error)) return VK_NULL_HANDLE;
    std::array<VkDescriptorBufferInfo, kFragmentUniformCount> buffers{};
    std::array<VkDescriptorImageInfo, kTextureSlots> images{}, samplers{};
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(kFragmentUniformCount + 2 * std::popcount(key.mask));
    for (uint32_t i = 0; i < kFragmentUniformCount; ++i) {
        buffers[i] = {arena.buffer, 0, kBlockBytes[4 + i]};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set; write.dstBinding = i; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &buffers[i];
        writes.push_back(write);
    }
    for (uint32_t bits = key.mask; bits; bits &= bits - 1) {
        const unsigned slot = std::countr_zero(bits);
        images[slot] = {VK_NULL_HANDLE, key.images[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        samplers[slot].sampler = key.samplers[slot];
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set; write.dstBinding = 16 + slot; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &images[slot];
        writes.push_back(write);
        write.dstBinding = 32 + slot;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &samplers[slot];
        writes.push_back(write);
    }
    UpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    ++fragmentSetUpdates;
    frameFragmentSets.emplace(key, set);
    return set;
}

bool WorldDrawLayer::Impl::draw(const Native::WorldDraw& draw, WorldSurfaces& targets,
                                const char** error,const std::shared_ptr<const Native::WorldDraw>* owner) {
    if(owner && owner->get()!=&draw)owner=nullptr;
    Native::setRenderSamplePhase(Native::RenderSamplePhase::other);
    if (command == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    const auto reject = [&](Policy::RejectReason reason, const char* why) {
        *error = why;
        ++rejected;
        static constexpr size_t kReported = 64;
        if (rejects.size() < kReported) {
            const std::string line = std::string(Policy::rejectName(reason)) + ": " + why + " (" +
                                     draw.fragmentName + ")";
            if (std::find(rejects.begin(), rejects.end(), line) == rejects.end()) rejects.push_back(line);
        }
        return false;
    };

    const uint8_t* a = draw.attributes.data();
    const uint32_t flags = Policy::word(a + 92);
    const bool withColor = draw.targets[0] != 0u && draw.material != Native::WorldMaterial::depth;
    const bool withDepth = draw.targets[4] != 0u;
    if (!draw.geometry) return reject(Policy::RejectReason::target, "the draw has no geometry");
    if (!withColor && !withDepth) return reject(Policy::RejectReason::target, "the draw names no target");
    const auto& options = draw.options;
    const auto& constants = draw.constants;
    // The constant compares and the arena copy stream a cold 4 KiB bank and its
    // shadow once per draw, and the compares alone were 8.4% of this thread's
    // samples (work/compare_sample_callers.py). Touch the lines now, while the
    // texture and state work below runs, instead of stalling in the constants
    // stage.
    const auto* vertexBank = reinterpret_cast<const uint8_t*>(constants.vectors.data());
    const auto* fragmentBank = reinterpret_cast<const uint8_t*>(draw.fragmentConstants.data());
    for (size_t at = 0; at < kVertexConstantBytes; at += 64) __builtin_prefetch(vertexBank + at);
    for (size_t at = 0; at < kFragmentConstantBytes; at += 64) __builtin_prefetch(fragmentBank + at);
    const uint8_t* shadowBank = vertexUniformShadow();
    for (size_t at = 0; at < kVertexConstantBytes; at += 64) __builtin_prefetch(shadowBank + at);
    if ((flags & (Policy::kDepthTest | Policy::kStencil)) && !withDepth)
        return reject(Policy::RejectReason::target, "a depth-tested draw has no depth surface");

    const std::string_view fragmentName = effectiveFragmentName(draw);
    const auto settings = Native::graphicsSettings();
    const uint32_t fragmentFlags = Native::graphicsFragmentFlags(fragmentName, draw.fragmentFlags,
                                                                 settings.bloom, settings.motionBlur);
    uint32_t textureMask = 0, cubeMask = 0;
    WorldShaderModule fragment;
    int fragmentVariant = -1;
    // A depth-material draw usually has no pixel shader, but an alpha-tested
    // shadow caster does: its captured name is the program the original bound.
    // A name the corpus has no program for is fatal only when colour is written,
    // exactly as the Windows renderer treats fragFound.
    if (!fragmentName.empty()) {
        fragmentVariant = findWorldFragmentVariant(fragmentName,fragmentFlags);
        if(fragmentVariant>=0)fragment=worldFragmentShaderAt(uint32_t(fragmentVariant),&textureMask,&cubeMask);
        if (!fragment && draw.material != Native::WorldMaterial::depth)
            return reject(Policy::RejectReason::fragment, "no fragment program");
    } else if (draw.material != Native::WorldMaterial::depth) {
        return reject(Policy::RejectReason::fragment, "the draw names no fragment program");
    }

    const uint32_t clipForm =
        draw.clipPlanes.enabledMask() ? (draw.clipPlanes.cullOnly() ? 2u : 1u) : 0u;
    uint8_t vertexKeyBytes[39]{};
    encodeVertexKey(options, clipForm, vertexKeyBytes);
    const int vertexVariant = findWorldVertexVariant(vertexKeyBytes);
    if (vertexVariant < 0)
        return reject(Policy::RejectReason::fragment, "no vertex program for these options");

    if (options.vertexColor && !draw.geometry.vertices->formats[10])
        return reject(Policy::RejectReason::attributes, "the color attribute is absent");
    if (options.weights && (!draw.geometry.vertices->formats[12] ||
                                 !draw.geometry.vertices->formats[13]))
        return reject(Policy::RejectReason::attributes, "the blend weights are absent");
    if (options.weights > 4 && (!draw.geometry.vertices->formats[14] ||
                                      !draw.geometry.vertices->formats[15]))
        return reject(Policy::RejectReason::attributes, "the second weight stream is absent");
    for (uint32_t stage = 0; stage < 8; ++stage) {
        if (options.modes[stage] == 7 && stage != 0)
            return reject(Policy::RejectReason::coordinates, "a reflector stage is not slot zero");
        const bool textureInput = options.modes[stage] == 0 || options.modes[stage] == 7 ||
                                  options.modes[stage] == 13 || options.modes[stage] == 22;
        if ((textureInput || (options.tangents && (stage == 2 || stage == 3))) &&
            options.coordinates[stage] >= 8)
            return reject(Policy::RejectReason::coordinates, "a stage names a coordinate bank past eight");
        if (textureInput && !draw.geometry.vertices->formats[options.coordinates[stage] + 1])
            return reject(Policy::RejectReason::coordinates, "a texture coordinate stream is absent");
    }

    Native::setRenderSamplePhase(Native::RenderSamplePhase::geometry);
    const Geometry* vertexStream = geometryFor(draw, error);
    if (!vertexStream) return reject(Policy::RejectReason::decode, "the vertex stream did not upload");
    const Indices* indexStream = indicesFor(draw, error);
    if (!indexStream) return reject(Policy::RejectReason::indices, "the index stream did not upload");

    const uint32_t count = draw.geometry.indexCount;
    const auto& sourceIndices = draw.geometry.indices->indices;
    if (!count || draw.geometry.firstIndex > sourceIndices.size() ||
        count > sourceIndices.size() - draw.geometry.firstIndex)
        return reject(Policy::RejectReason::indices, "the index range leaves its stream");
    if (indexStream->maximum >= vertexStream->count) {
        for (size_t i = draw.geometry.firstIndex; i < draw.geometry.firstIndex + count; ++i)
            if (sourceIndices[i] >= vertexStream->count)
                return reject(Policy::RejectReason::indexOverflow, "an index names no vertex");
    }

    // Texture bindings. A finished surface the engine resolved is sampled through
    // its resolved view, exactly as the Windows renderer prefers its resolved_
    // entry over the CPU snapshot; anything else needs its owned pixels.
    Native::setRenderSamplePhase(Native::RenderSamplePhase::textures);
    WorldSurfaces::ResolvedInfo resolvedInfo;
    std::array<VkImageView, kTextureSlots> slotViews{};
    std::array<uint32_t, kTextureSlots> slotLevels{};
    std::array<uint32_t, kTextureSlots> slotFirstMip{};
    std::array<bool, kTextureSlots> slotResolved{};
    for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        slotViews[slot] = dummy.view;
        slotLevels[slot] = 1;
        if (!fragment || !(textureMask & (1u << slot))) continue;
        const Native::WorldTexture& texture = draw.textureObjects[slot];
        // The Windows renderer treats these explicitly unbound mask slots as
        // zero samples in the retail death-scene radial blur.
        if (fragmentName == "XREngine_RadialBlurInvert" && slot >= 1 && slot <= 3 &&
            !draw.textureIds[slot] && !texture.object && !texture.storage && !draw.textures[slot])
            continue;
        const bool cube = (cubeMask & (1u << slot)) != 0;
        const VkImageView resolved = texture.object ? targets.resolvedView(texture.key()) : VK_NULL_HANDLE;
        if (resolved) {
            if (!targets.resolvedInfo(texture.key(), resolvedInfo) || resolvedInfo.faces != texture.faces ||
                resolvedInfo.width != texture.width || resolvedInfo.height != texture.height)
                return reject(Policy::RejectReason::resolved, "a resolved texture changed extent");
            slotViews[slot] = resolved;
            slotResolved[slot] = true;
            continue;
        }
        const auto& image = draw.textures[slot];
        if (!image || image->faces != (cube ? 6u : 1u)) {
            static unsigned missingLogs = 0;
            if (missingLogs++ < 24)
                std::printf("[World] missing texture program=%.*s slot=%u id=%u object=%08X storage=%08X format=%u size=%ux%u faces=%u\n",
                            int(fragmentName.size()), fragmentName.data(), slot, draw.textureIds[slot], texture.object, texture.storage,
                            texture.format, texture.width, texture.height, texture.faces);
            return reject(Policy::RejectReason::texture, "a sampled texture has no pixels");
        }
        // Image transitions and upload copies must be outside a render pass.
        // Resident textures need no GPU commands, so their draws can batch.
        const Texture* uploaded = textureFor(image, error, &targets);
        if (!uploaded) return reject(Policy::RejectReason::texture, "a sampled texture did not upload");
        slotViews[slot] = uploaded->view;
        slotLevels[slot] = uploaded->levels;
        slotFirstMip[slot] = image->firstMip;
    }

    Native::setRenderSamplePhase(Native::RenderSamplePhase::constants);
    // Reuse immutable blocks within this GPU frame, comparing exact bytes.
    // A changed block gets fresh storage; earlier recorded draws stay intact.
    ViewportConstants viewportConstants{draw.depthRange, Native::EngineVector{1, 0, 0, 0}};
    if (draw.subtitle && settings.subtitleScalePercent != 100) {
        const float factor = settings.subtitleScalePercent / 100.0f;
        viewportConstants.promptTransform = {factor, 0.0f, factor - 1.0f, 1.0f};
    }
    ClipConstants clipConstants{};
    clipConstants.control[0] = draw.clipPlanes.enabledMask();
    for (uint32_t plane = 0; plane < 6; ++plane)
        if (clipConstants.control[0] & (1u << plane)) clipConstants.planes[plane] = draw.clipPlanes.planes[plane];
    const Native::EngineVector materialColor = darknessArmBloomColor(draw, settings.bloom);
    std::array<Native::EngineVector, kTextureSlots> scales{};
    for (uint32_t slot = 0; fragment && slot < kTextureSlots; ++slot) {
        const float scale = Policy::fetchScale(draw.textureObjects[slot].exponent);
        scales[slot] = Native::EngineVector{scale, scale, scale, scale};
    }
    AlphaTestConstants alpha{};
    alpha.function = (flags & 1u) ? 8u : uint32_t(a[97]);
    alpha.reference = float((uint32_t(a[98]) << 8) | a[99]) / 255.0f;
    const uint32_t scale = targets.renderScale();
    ColorLookupConstants lookup{1.0f / scale, 0u, {0, 0}};
    if (scale > 1) {
        const int slot = fragmentName == "XREngine_CCFuser" && fragmentFlags == 2 ? 1 :
            fragmentName == "XREngine_Final5" && (fragmentFlags & 4u) ? 2 : -1;
        if (slot >= 0 && slotResolved[slot] && draw.textureObjects[slot].width == 324 &&
            draw.textureObjects[slot].height == 18) lookup.logicalLookupMask |= 1u << slot;
    }
    const BloomConstants bloom{float(scale), 0u, {0, 0}};
    const ShadowConstants shadow{scale, {0, 0, 0}};
    const std::array<const void*,kBlockCount> sources{constants.vectors.data(),constants.references.data(),
        &viewportConstants,&clipConstants,nullptr,scales.data(),&alpha,&lookup,&bloom,&shadow};
    std::array<uint32_t,kBlockCount> blockOffsets{};
    for (unsigned i=0;i<(fragment?kBlockCount:4);++i) {
        bool knownDifferent = false;
        std::array<uint64_t,4> uploadedVertexWritten{};
        // Vertex banks hold zeros outside the vectors their preparation wrote,
        // so two banks with the same written mask are equal exactly when those
        // vectors are: compare a few dozen vectors instead of all 256 (slot 0
        // matched on 56% of draws, work/framerate-slot-counts-perf.txt).
        if (i == 0) {
            const auto& written = constants.written;
            bool known = vertexMaskTrusted && (written[0] | written[1] | written[2] | written[3]) != 0;
            if (known && (vertexMaskChecks++ & 1023) == 0) {
                for (unsigned v = 0; v < 256; ++v) {
                    if (written[v / 64] >> (v % 64) & 1) continue;
                    const auto& vector = constants.vectors[v];
                    if (vector[0] != 0.0f || vector[1] != 0.0f || vector[2] != 0.0f || vector[3] != 0.0f ||
                        std::signbit(vector[0]) || std::signbit(vector[1]) || std::signbit(vector[2]) ||
                        std::signbit(vector[3])) {
                        std::printf("[World] vertex vector %u is outside its written mask; masked compare off\n", v);
                        if (FILE* perf = std::fopen("/download0/darkness-ps5/perf.txt", "a")) {
                            std::fprintf(perf, "[World] vertex vector %u outside written mask; masked compare off\n", v);
                            std::fclose(perf);
                        }
                        vertexMaskTrusted = known = false;
                        Native::selectiveWorldDrawResetSafe.store(false,std::memory_order_relaxed);
                        Native::nativeWorldCaptureSafe.store(false,std::memory_order_relaxed);
                        Native::vertexProgramPlansSafe.store(false,std::memory_order_relaxed);
                        break;
                    }
                }
            }
            const size_t previous = lastUniformOffsets[0];
            const auto* shadow=vertexUniformShadow();
            ++slotRequests[0];
            if (known && constantReuseEnabled && previous != SIZE_MAX && written == lastVertexWritten &&
                (lastVertexUniformOwner || lastUniformBytes[0].size()==kVertexConstantBytes)) {
                ++slotCompares[0];
                const auto* bank = reinterpret_cast<const uint8_t*>(constants.vectors.data());
                const bool equal = equalWrittenVectors(shadow, bank, written);
                if (equal) {
                    ++slotMatches[0];
                    ++uniformReuses;
                    blockOffsets[0] = uint32_t(previous);
                    continue;
                }
                knownDifferent = true;
            }
            --slotRequests[0]; // uniformBlock below counts this request
            uploadedVertexWritten = known ? written : std::array<uint64_t,4>{};
        }
        const size_t offset = i==4 ? uniformBlock(i,&materialColor,sizeof(materialColor),
            draw.fragmentConstants.data()+1,kFragmentConstantBytes-sizeof(materialColor),false,owner) :
            i==0?uniformBlock(i,sources[i],kBlockBytes[i],nullptr,0,knownDifferent,owner):
            uniformBlockCopied(i,sources[i],kBlockBytes[i]);
        if (offset==SIZE_MAX) return reject(Policy::RejectReason::budget,"the constant arena is full");
        if (i == 0) lastVertexWritten = uploadedVertexWritten;
        blockOffsets[i]=uint32_t(offset);
    }

    Native::setRenderSamplePhase(Native::RenderSamplePhase::states);
    // Render state, read through the policy header the host build checks.
    int32_t biasConstant = 0;
    float biasSlope = 0.0f;
    if (!Policy::depthBias(flags, std::bit_cast<float>(Policy::word(a + 100)),
                           std::bit_cast<float>(Policy::word(a + 104)), biasConstant, biasSlope))
        return reject(Policy::RejectReason::state, "the depth bias is not finite");
    biasSlope *= float(scale);
    PipelineKey key{};
    key.vertexVariant = uint32_t(vertexVariant);
    key.fragmentVariant = fragmentVariant;
    key.colorWrite = Policy::colorWriteMask(flags);
    key.blendEnabled = Policy::blendEnabled(flags);
    if (key.blendEnabled) {
        if (a[144] >= Policy::kBlendFactorCount || a[145] >= Policy::kBlendFactorCount)
            return reject(Policy::RejectReason::state, "a blend factor is unknown");
        key.srcColor = uint32_t(Policy::blendFactor(a[144]));
        key.dstColor = uint32_t(Policy::blendFactor(a[145]));
    }
    key.depthTest = Policy::depthTestEnabled(flags);
    key.depthWrite = Policy::depthWriteEnabled(flags);
    key.depthCompare = uint32_t(Policy::depthCompareFor(flags, a[96]));
    key.stencil = Policy::stencilEnabled(flags);
    if (key.depthTest && key.depthWrite)
        nearerIsGreater = Policy::nearerIsGreaterFor(Policy::CompareOp(key.depthCompare), nearerIsGreater);
    const bool wasEqual = key.depthTest && key.depthCompare == uint32_t(Policy::CompareOp::equal);
    if (wasEqual) {
        const Policy::EqualDepth tolerant = Policy::tolerantEqual(nearerIsGreater);
        key.depthCompare = uint32_t(tolerant.compare);
        key.depthWrite = false;
        biasConstant += tolerant.bias;
    }
    key.front = Policy::stencilFace(a + 120);
    key.back = (flags & 0x80u) ? Policy::stencilFace(a + 122) : key.front;
    key.stencilReadMask = a[125];
    key.stencilWriteMask = a[126];
    key.cull = Policy::cullEnabled(flags);
    key.frontCounterClockwise = Policy::frontFaceCounterClockwise(flags);
    key.withColor = withColor;
    key.withDepth = withDepth;

    const VkRenderPass pass = targets.drawPass(withColor, withDepth);
    if (!pass) return reject(Policy::RejectReason::target, "the draw pass is unavailable");
    const VkShaderModule vertex = vertexModule(uint32_t(vertexVariant), error);
    if (!vertex) return reject(Policy::RejectReason::fragment, "the vertex module failed");
    const VkShaderModule pixel = fragment ? fragmentModule(uint32_t(fragmentVariant), error) : VK_NULL_HANDLE;
    if (fragment && !pixel) return reject(Policy::RejectReason::fragment, "the fragment module failed");
    const VkPipeline pipeline = this->pipeline(key, pass, vertex, pixel, error);
    if (!pipeline) return reject(Policy::RejectReason::constants, "the draw pipeline failed");

    // The uniform descriptors and their dynamic offsets. One set per stage: the
    // vertex stage's binding 2 is the viewport, the fragment stage's is the alpha
    // test, so they cannot share a set.
    const VkDescriptorSet vertexSet = vertexSetForFrame(error);
    VkDescriptorSet fragmentSet = VK_NULL_HANDLE;
    if (!vertexSet) return reject(Policy::RejectReason::budget, "the descriptor sets are exhausted");
    std::array<uint32_t,4> vertexOffsets{};
    std::copy_n(blockOffsets.begin(),4,vertexOffsets.begin());
    std::array<uint32_t, kFragmentUniformCount> fragmentOffsets{};
    for (uint32_t i = 0; i < kFragmentUniformCount; ++i)
        fragmentOffsets[i] = blockOffsets[4 + i];
    FragmentSetKey fragmentKey;
    Native::setRenderSamplePhase(Native::RenderSamplePhase::textures);
    fragmentKey.mask = textureMask;
    for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        if (!(textureMask & (1u << slot))) continue;
        fragmentKey.images[slot] = slotViews[slot];
        const Native::WorldSampler& captured = draw.samplers[slot];
        SamplerKey samplerKey;
        samplerKey.minLinear = samplerKey.magLinear = samplerKey.mipLinear = 1;
        samplerKey.address[0] = samplerKey.address[1] = samplerKey.address[2] = 2;
        if (!captured.valid && !(cubeMask & (1u << slot)) &&
            draw.material != Native::WorldMaterial::post && draw.material != Native::WorldMaterial::fixed)
            samplerKey.address[0] = samplerKey.address[1] = samplerKey.address[2] = 0;
        samplerKey.maxLevel = 15;
        if (captured.valid) {
            std::copy(captured.address.begin(), captured.address.end(), samplerKey.address);
            samplerKey.minLinear = captured.minLinear;
            samplerKey.magLinear = captured.magLinear;
            samplerKey.mipLinear = captured.mipLinear;
            samplerKey.anisotropy = uint8_t((std::max)(1u, (std::min)(unsigned(captured.anisotropy), 16u)));
            samplerKey.minLevel = captured.minLevel;
            samplerKey.maxLevel = captured.maxLevel;
            samplerKey.bias = captured.bias;
            const bool surfaceFiltering = startsWith(fragmentName, "XRShader_FP20_") ||
                startsWith(fragmentName, "XRShader_Decal") || startsWith(fragmentName, "VBOp_FP20_") ||
                fragmentName == "XRUtil_RenderSurface" || fragmentName == "TexEnvProj1";
            if (settings.anisotropyLevels > 1 && surfaceFiltering && !slotResolved[slot] &&
                !(cubeMask & (1u << slot)) && captured.minLinear && captured.magLinear && !captured.baseOnly)
                samplerKey.anisotropy = uint8_t(settings.anisotropyLevels);
            if (captured.baseOnly) samplerKey.maxLevel = samplerKey.minLevel;
        }
        else if (captured.lodValid) {
            samplerKey.minLevel = captured.minLevel;
            samplerKey.maxLevel = captured.baseOnly ? captured.minLevel : captured.maxLevel;
            samplerKey.bias = captured.bias;
        }
        const uint32_t lastLevel = slotLevels[slot] - 1u;
        samplerKey.minLevel = uint8_t((std::max)(unsigned(samplerKey.minLevel), slotFirstMip[slot]));
        samplerKey.minLevel = uint8_t((std::min)(unsigned(samplerKey.minLevel), lastLevel));
        samplerKey.maxLevel = uint8_t((std::min)(unsigned(samplerKey.maxLevel), lastLevel));
        if (samplerKey.maxLevel < samplerKey.minLevel) samplerKey.maxLevel = samplerKey.minLevel;
        if (slotResolved[slot]) samplerKey.minLevel = samplerKey.maxLevel = 0;
        fragmentKey.samplers[slot] = this->sampler(samplerKey, error);
        if (!fragmentKey.samplers[slot]) return reject(Policy::RejectReason::texture, "a sampler failed");
    }
    Native::setRenderSamplePhase(Native::RenderSamplePhase::states);
    if (fragment) {
        fragmentSet = fragmentSetForFrame(fragmentKey, error);
        if (!fragmentSet) return reject(Policy::RejectReason::budget, "the descriptor sets are exhausted");
    }

    // The targets and the pass. Everything that can refuse has already refused,
    // so the pass is opened last and closed by EndDraw from here on.
    Native::setRenderSamplePhase(Native::RenderSamplePhase::submit);
    WorldSurfaces::DrawTarget target;
    if (!targets.BeginDraw(draw, target, error))
        return reject(Policy::RejectReason::target, "the draw's targets are unavailable");

    const Policy::ViewportRect viewport = Policy::viewport(draw.viewport);
    const VkViewport viewportState{viewport.x * scale, viewport.y * scale, viewport.width * scale, viewport.height * scale, 0.0f, 1.0f};
    Policy::ScissorRect rect = (flags & Policy::kScissor)
                                   ? Policy::scissor(Policy::word(a + 112), Policy::word(a + 116),
                                                     target.width, target.height)
                                   : Policy::fullScissor(target.width, target.height);
    const VkRect2D scissor{{rect.x * int32_t(scale), rect.y * int32_t(scale)}, {uint32_t(rect.width) * scale, uint32_t(rect.height) * scale}};
    bindDrawState(target.serial, pipeline, vertexStream->buffer.buffer, indexStream->buffer.buffer,
                  viewportState, scissor, a[124], float(biasConstant), biasSlope);
    bindDescriptors(target.serial,vertexSet,fragment?fragmentSet:VK_NULL_HANDLE,vertexOffsets,fragmentOffsets);
    const bool measured = activeQuery && activeValid && queryPool && queryCount < kQueryCapacity;
    if (activeQuery && !measured) activeValid = false;
    if (measured) CmdBeginQuery(command, queryPool, queryCount, queryControl);
    CmdDrawIndexed(command, count, 1, draw.geometry.firstIndex, 0, 0);
    if (measured) CmdEndQuery(command, queryPool, queryCount++);
    targets.EndDraw();

    ++draws;
    Native::recordWorldSubmission(draw.material);
    return true;
}

// ---------------------------------------------------------------------------
// The title-facing interface
// ---------------------------------------------------------------------------

WorldDrawLayer::~WorldDrawLayer() { Destroy(); }

bool WorldDrawLayer::Create(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                            const char** error) {
    Destroy();
    impl_ = new Impl();
    Impl& impl = *impl_;
    impl.instance = instance;
    impl.physical = physical;
    impl.device = device;
    // Destroying the layer frees the state a reason may point into, so the
    // reason is copied to storage that outlives it.
    static char reason[160];
    const auto abandon = [&](const char* why) {
        std::snprintf(reason, sizeof reason, "%s",
                      why ? why : "the world draw backend could not be created");
        *error = reason;
        Destroy();
        return false;
    };
    if (!impl.loadEntryPoints(error)) return abandon(*error ? *error : "a device entry point is missing");

    VkPhysicalDeviceProperties properties{};
    if (const auto get = Vk::instanceProc<PFN_vkGetPhysicalDeviceProperties>(instance,
                                                                            "vkGetPhysicalDeviceProperties"))
        get(physical, &properties);
    impl.uniformAlignment = (std::max)(VkDeviceSize(1), properties.limits.minUniformBufferOffsetAlignment);

    if (!impl.createLayouts(error)) return abandon(*error);
    if (!impl.createBuffer(impl.arena, kArenaBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, error))
        return abandon(*error);
    if (!impl.createDummyTexture(error)) return abandon(*error);

    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
    queryInfo.queryCount = kQueryCapacity;
    if (impl.CreateQueryPool(device, &queryInfo, nullptr, &impl.queryPool) != VK_SUCCESS)
        return abandon("vkCreateQueryPool failed for the engine's measurements");
    // Display::Open enables precise occlusion whenever the device supports it.
    VkPhysicalDeviceFeatures features{};
    if (const auto get = Vk::instanceProc<PFN_vkGetPhysicalDeviceFeatures>(instance, "vkGetPhysicalDeviceFeatures"))
        get(physical, &features);
    if (features.occlusionQueryPrecise) impl.queryControl = VK_QUERY_CONTROL_PRECISE_BIT;
    std::printf("[World] occlusion queries: %u per frame, %s counts\n", kQueryCapacity,
                impl.queryControl ? "precise" : "approximate");
    std::printf("[World] draw path ready: %zu vertex and %zu fragment programs\n",
                worldVertexShaderCount(), worldFragmentShaderCount());
    std::fflush(stdout);
    return true;
}

void WorldDrawLayer::Destroy() {
    if (!impl_) return;
    Impl& impl = *impl_;
    if (impl.device != VK_NULL_HANDLE && impl.DeviceWaitIdle) impl.DeviceWaitIdle(impl.device);
    for (auto& buffer : impl.pendingUploads) impl.destroyBuffer(buffer);
    impl.pendingUploads.clear();
    if (impl.DestroyPipeline) {
        for (auto& entry : impl.pipelines)
            if (entry.second) impl.DestroyPipeline(impl.device, entry.second, nullptr);
        impl.pipelines.clear();
    }
    if (impl.DestroyShaderModule) {
        for (auto& entry : impl.vertexModules)
            if (entry.second) impl.DestroyShaderModule(impl.device, entry.second, nullptr);
        for (auto& entry : impl.fragmentModules)
            if (entry.second) impl.DestroyShaderModule(impl.device, entry.second, nullptr);
        impl.vertexModules.clear();
        impl.fragmentModules.clear();
    }
    if (impl.DestroySampler) {
        for (auto& entry : impl.samplers)
            if (entry.second) impl.DestroySampler(impl.device, entry.second, nullptr);
        impl.samplers.clear();
    }
    if (impl.DestroyImageView && impl.dummy.image) impl.destroyTexture(impl.dummy);
    impl.hotGeometry.clear(); impl.hotIndices.clear(); impl.hotTextures.clear();
    for (auto& entry : impl.textures) impl.destroyTexture(entry.second);
    impl.textures.clear();
    for (auto& entry : impl.geometry) impl.destroyBuffer(entry.second.buffer);
    impl.geometry.clear();
    for (auto& entry : impl.indices) impl.destroyBuffer(entry.second.buffer);
    impl.indices.clear();
    if (impl.DestroyBuffer) impl.destroyBuffer(impl.arena);
    impl.memoryBlocks.clear();
    if (impl.DestroyDescriptorPool)
        for (auto& pool : impl.pools)
            if (pool.pool) impl.DestroyDescriptorPool(impl.device, pool.pool, nullptr);
    impl.pools.clear();
    if (impl.queryPool) impl.DestroyQueryPool(impl.device, impl.queryPool, nullptr);
    if (impl.pipelineLayout) impl.DestroyPipelineLayout(impl.device, impl.pipelineLayout, nullptr);
    if (impl.vertexLayout) impl.DestroyDescriptorSetLayout(impl.device, impl.vertexLayout, nullptr);
    if (impl.fragmentLayout) impl.DestroyDescriptorSetLayout(impl.device, impl.fragmentLayout, nullptr);
    delete impl_;
    impl_ = nullptr;
    command_ = VK_NULL_HANDLE;
}

void WorldDrawLayer::BeginFrame(VkCommandBuffer command) {
    command_ = command;
    if (!impl_) return;
    impl_->command = command;
    for (auto& buffer : impl_->pendingUploads) impl_->destroyBuffer(buffer);
    impl_->pendingUploads.clear();
    impl_->resetFrameConstants();
    // Diagnostic A/B switch; normal play keeps exact-byte reuse enabled.
    using Clock = std::chrono::steady_clock;
    static auto nextCheck = Clock::time_point{};
    const auto now = Clock::now();
    if (now >= nextCheck) {
        nextCheck = now+std::chrono::seconds(1);
        std::error_code ec;
        impl_->constantReuseEnabled = !std::filesystem::exists(
            "/download0/darkness-ps5/constant-reuse.disabled",ec);
        ec.clear();
        impl_->retainUniformShadows=std::filesystem::exists(
            "/download0/darkness-ps5/retained-uniforms.enabled",ec) && !ec;
        ec.clear();
        Native::selectiveWorldDrawReset.store(!std::filesystem::exists(
            "/download0/darkness-ps5/full-draw-reset.enabled",ec),std::memory_order_relaxed);
        ec.clear();
        impl_->boundedCacheTrim=std::filesystem::exists(
            "/download0/darkness-ps5/bounded-cache-trim.enabled",ec) && !ec;
        ec.clear();
        const bool vertexPlans=std::filesystem::exists(
            "/download0/darkness-ps5/vertex-program-plans.enabled",ec) && !ec;
        Native::vertexProgramPlansEnabled.store(vertexPlans,std::memory_order_relaxed);
        ec.clear();
        Native::nativeWorldCaptureEnabled.store(std::filesystem::exists(
            "/download0/darkness-ps5/native-draw-capture.enabled",ec) && !ec,std::memory_order_relaxed);
        ec.clear();
        Native::nativeWorldCaptureCompare.store(std::filesystem::exists(
            "/download0/darkness-ps5/native-draw-capture-compare.enabled",ec) && !ec,std::memory_order_relaxed);
    }
    impl_->frameVertexSet = VK_NULL_HANDLE;
    impl_->boundPass = 0;
    impl_->frameFragmentSets.clear();
    impl_->collectQueries();
    impl_->trimCaches();
    std::erase_if(impl_->memoryBlocks, [](const auto& block) { return block.use_count() == 1; });
    for (auto& pool : impl_->pools) {
        impl_->ResetDescriptorPool(impl_->device, pool.pool, 0);
        pool.used = 0;
    }
}

void WorldDrawLayer::Query(const std::shared_ptr<Native::WorldQuery>& query, bool begin) {
    if (!impl_ || !query || !query->result) return;
    Impl& impl = *impl_;
    if (begin) {
        impl.activeQuery = query;
        impl.activeFirst = impl.queryCount;
        impl.activeValid = true;
        return;
    }
    if (impl.activeQuery != query) return;
    if (impl.activeValid)
        impl.recordedQueries.push_back({query, impl.activeFirst, impl.queryCount - impl.activeFirst});
    else
        ++impl.queriesDropped;
    impl.activeQuery.reset();
    impl.activeValid = false;
}

bool WorldDrawLayer::Draw(const Native::WorldDraw& draw, WorldSurfaces& targets, const char** error) {
    if (!impl_) {
        *error = "the world draw layer is not created";
        return false;
    }
    impl_->command = command_;
    return impl_->draw(draw, targets, error);
}
bool WorldDrawLayer::Draw(const std::shared_ptr<const Native::WorldDraw>& draw,WorldSurfaces& targets,const char** error) {
    if(!impl_ || !draw) {
        *error="the world draw layer or draw is not available";return false;
    }
    impl_->command=command_;
    return impl_->draw(*draw,targets,error,&draw);
}

uint64_t WorldDrawLayer::draws() const { return impl_ ? impl_->draws : 0; }
uint64_t WorldDrawLayer::rejected() const { return impl_ ? impl_->rejected : 0; }
uint64_t WorldDrawLayer::pipelines() const { return impl_ ? impl_->drawPipelines : 0; }
uint64_t WorldDrawLayer::textures() const { return impl_ ? impl_->textures.size() : 0; }
size_t WorldDrawLayer::vertexBytes() const { return impl_ ? impl_->geometryBytes : 0; }
size_t WorldDrawLayer::textureBytes() const { return impl_ ? impl_->textureBytes : 0; }

void WorldDrawLayer::printCounters() const {
    if (!impl_) {
        std::printf("[World] draws: not initialised\n");
        std::fflush(stdout);
        return;
    }
    std::printf("[World] draws=%llu rejected=%llu pipelines=%llu shaders=%zu/%zu textures=%zu "
                "vertexMB=%zu textureMB=%zu\n",
                static_cast<unsigned long long>(impl_->draws),
                static_cast<unsigned long long>(impl_->rejected),
                static_cast<unsigned long long>(impl_->drawPipelines), impl_->vertexModules.size(),
                impl_->fragmentModules.size(), impl_->textures.size(), impl_->geometryBytes >> 20,
                impl_->textureBytes >> 20);
    for (const std::string& line : impl_->rejects) std::printf("[World] reject %s\n", line.c_str());
    std::printf("[World] descriptors vertex=%llu fragment=%llu reused=%llu\n",
                static_cast<unsigned long long>(impl_->vertexSetUpdates),
                static_cast<unsigned long long>(impl_->fragmentSetUpdates),
                static_cast<unsigned long long>(impl_->fragmentSetHits));
    std::printf("[World] constants reused=%llu descriptorBinds=%llu descriptorSkips=%llu arenaMiB=%.2f\n",
        (unsigned long long)impl_->uniformReuses,(unsigned long long)impl_->descriptorBinds,
        (unsigned long long)impl_->descriptorReuses,double(impl_->arenaOffset)/(1024*1024));
    // Also to the FTP-reachable perf file, for a session without the log relay.
    FILE* perf = std::fopen("/download0/darkness-ps5/perf.txt", "a");
    if(perf)std::fprintf(perf,"[World] retained uniforms enabled=%u blocks=%llu cpuBytesSaved=%llu\n",
        unsigned(impl_->retainUniformShadows),(unsigned long long)impl_->retainedUniformBlocks,
        (unsigned long long)impl_->retainedUniformBytes);
    if(perf) {
        const auto native=Native::nativeWorldCaptureStats();
        std::fprintf(perf,"[World] native capture enabled=%u compare=%u safe=%u attempts=%llu captured=%llu fallbacks=%llu mismatches=%llu\n",
            unsigned(Native::nativeWorldCaptureEnabled.load(std::memory_order_relaxed)),
            unsigned(Native::nativeWorldCaptureCompare.load(std::memory_order_relaxed)),
            unsigned(Native::nativeWorldCaptureSafe.load(std::memory_order_relaxed)),
            (unsigned long long)native.attempts,(unsigned long long)native.captured,
            (unsigned long long)native.fallbacks,(unsigned long long)native.mismatches);
    }
    if(perf)std::fprintf(perf,"[World] vertex plans enabled=%u lookups=%llu hits=%llu safe=%u\n",
        unsigned(Native::vertexProgramPlansEnabled.load(std::memory_order_relaxed)),
        (unsigned long long)Native::vertexProgramPlanLookups.load(std::memory_order_relaxed),
        (unsigned long long)Native::vertexProgramPlanHits.load(std::memory_order_relaxed),
        unsigned(Native::vertexProgramPlansSafe.load(std::memory_order_relaxed)));
    if(perf)std::fprintf(perf,"[World] cache trim bounded=%u geometry=%zu indices=%zu textures=%zu\n",
        unsigned(impl_->boundedCacheTrim),impl_->geometry.size(),impl_->indices.size(),impl_->textures.size());
    if (perf) std::fprintf(perf,"[World] selective reset enabled=%u safe=%u vertexBytesSaved=%llu\n",
        unsigned(Native::selectiveWorldDrawReset.load()),unsigned(Native::selectiveWorldDrawResetSafe.load()),
        (unsigned long long)Native::worldVertexResetBytesSaved.load());
    if (perf) std::fprintf(perf,"[World] front cache hits/misses vertex=%llu/%llu index=%llu/%llu texture=%llu/%llu\n",
        (unsigned long long)impl_->hotGeometry.hits,(unsigned long long)impl_->hotGeometry.misses,
        (unsigned long long)impl_->hotIndices.hits,(unsigned long long)impl_->hotIndices.misses,
        (unsigned long long)impl_->hotTextures.hits,(unsigned long long)impl_->hotTextures.misses);
    for (unsigned slot = 0; slot < kBlockCount; ++slot) {
        char line[160];
        std::snprintf(line, sizeof line, "[World] uniform slot=%u bytes=%zu requests=%llu compares=%llu matches=%llu\n",
                      slot, kBlockBytes[slot], (unsigned long long)impl_->slotRequests[slot],
                      (unsigned long long)impl_->slotCompares[slot], (unsigned long long)impl_->slotMatches[slot]);
        std::fputs(line, stdout);
        if (perf) std::fputs(line, perf);
    }
    if (perf) std::fclose(perf);
    std::fflush(stdout);
}

} // namespace DarkRecomp::Ps5
