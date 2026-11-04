#include "simple_mesh.h"
#include "display_gamma.h"
#include "prompt_icons.h"
#include "texture_mip_layout.h"
#include "texture_pixel_decode.h"
#include "engine_performance.h"
#include "stored_geometry.h"
#include "engine_texture_constants.h"
#include "engine_vertex_descriptor.h"
#include "engine_palette.h"
#include "engine_vertex_program.h"
#include "world_mesh.h"
#include "queued_world_draw.h"
#include "frame_resource_set.h"
#include "scene_work.h"
#include "render_trace.h"
#include "runtime/native/display_mode.h"
#include "runtime/native/runtime.h"
#include "runtime/native/stall_profiler.h"
#include "runtime/native/windows_shim.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace DarkRecomp::Native {
namespace {
thread_local PromptRenderContext promptRenderContext = PromptRenderContext::Menu;
thread_local bool subtitleRenderContext = false;
struct PromptContextFrame {
    uint8_t* base = nullptr;
    uint32_t manager = 0;
    uint64_t generation = 0;
    std::unordered_set<uint32_t> gameplayDraws;
    std::unordered_set<uint32_t> subtitleDraws;
};
constexpr size_t promptManagerLimit = 16, promptDrawLimit = 4096;
std::array<PromptContextFrame, promptManagerLimit> promptContextFrames;
std::mutex promptContextMutex;
uint64_t promptContextGeneration = 0;
PromptContextFrame* findPromptContextFrame(uint8_t* base, uint32_t manager) noexcept {
    for (auto& frame : promptContextFrames)
        if (frame.base == base && frame.manager == manager) return &frame;
    return nullptr;
}
bool TextureUploadTraceEnabled() {
    // Per-new-texture success logs cost unbuffered writes during area loads.
    // Failures stay on their bounded paths; gate success spam behind opt-in.
    static const bool enabled = std::getenv("DARK_TEXTURE_TRACE") != nullptr &&
        std::getenv("DARK_TEXTURE_TRACE")[0] == '1';
    return enabled;
}
constexpr uint32_t context = 0x82A69B00;
uint32_t u32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}
uint16_t u16(const uint8_t* bytes) { return uint16_t(bytes[0]) << 8 | bytes[1]; }
float f32(const uint8_t* bytes) { return std::bit_cast<float>(u32(bytes)); }
bool readWord(uint8_t* base, uint32_t address, uint32_t& value) {
    uint8_t bytes[4];
    if (!copyRenderMemory(base, address, bytes, 4)) return false;
    value = u32(bytes); return true;
}
std::atomic<bool> active{false},worldActive{false};
std::atomic<uint64_t> observed{}, accepted{}, unsupported{}, frames{}, images{}, rejectedImages{};
std::atomic<uint64_t> renderedFrames{}, renderedMeshes{};
std::atomic<uint64_t> videoImages{}, videoMeshes{};
std::atomic<uint64_t> colorImages{}, colorMeshes{};
std::atomic<uint64_t> decodedBatches{}, decodedIndices{}, decodedAccepted{}, decodedInvalid{};
std::array<std::atomic<uint64_t>, 10> rejectionReasons{};
std::mutex queueMutex;
std::condition_variable frameConsumed,frameAvailable;
bool frameBackpressure=false;
bool streamLargeFrames=false,pendingFirst=true;
struct ReadyPreviewPart {std::vector<SimpleMesh> commands;PreviewFramePart part;};
std::deque<ReadyPreviewPart> readyParts;
unsigned readyPartLimit=1;
// A live frame may contain many lighting passes. Bound the in-flight batch,
// not the logical frame: producer and consumer own one batch each, with a
// bounded ready queue between them. Each batch has 512 entries; immutable world
// buffers and images have independent byte limits and are charged once.
constexpr size_t livePartCommands=512,worldGeometryByteLimit=64*1024*1024;
constexpr size_t worldImageByteLimit=256*1024*1024;
std::unordered_map<uint32_t, std::shared_ptr<const AlphaImage>> textures;
std::unordered_map<uint32_t, std::shared_ptr<const ColorImage>> alphaColors;
struct CachedColor { std::shared_ptr<const ColorImage> image; uint64_t used = 0; uint32_t resource = 0; };
std::unordered_map<uint32_t, CachedColor> colorTextures;
// Completed authored images can reach the GPU while loading is still showing
// a prior frame. Never extend their CPU lifetime solely for this optional work.
constexpr size_t texturePreloadLimit=4096;
std::deque<std::weak_ptr<const ColorImage>> texturePreloads;
PreviewTexturePreloadCounters texturePreloadCounts;
// The original menu upload working set exceeds64MiB before its model is first
// drawn. Keep that working set resident; dropping an uploaded map cannot cause
// the guest to repeat its already-completed upload.
size_t colorBudget = 256 * 1024 * 1024;
bool colorBudgetReported = false;
size_t colorBytes = 0, pendingColorBytes = 0;
uint64_t colorUse = 0;
// Called with queueMutex held. Ordinary textures can be reconstructed from
// resident guest pixels after eviction; verified GUI prompts also carry the
// selected original button name, which a pixel-only decode cannot recover.
// Retain that owned generation until explicit invalidation or replacement.
bool makeColorCacheRoom(size_t bytes) {
    if (bytes > colorBudget) return false;
    while (colorBytes > colorBudget - bytes) {
        auto oldest = colorTextures.end();
        for (auto it = colorTextures.begin(); it != colorTextures.end(); ++it) {
            if (it->second.image->promptOrigin) continue;
            if (oldest == colorTextures.end() || it->second.used < oldest->second.used) oldest = it;
        }
        // Keep the cache bounded even if retained prompts fill its budget.
        if (oldest == colorTextures.end()) return false;
        colorBytes -= oldest->second.image->bytes();
        colorTextures.erase(oldest);
    }
    return true;
}
struct CachedVideo {
    uint32_t owner = 0, frame = 0;
    std::array<uint8_t, 8> timestamp{};
    std::shared_ptr<const VideoFrame> image;
};
std::unordered_map<uint32_t, CachedVideo> videos;
std::vector<SimpleMesh> pending, recycledFrame;
std::atomic<uint64_t> frameBufferReuses{};
size_t pendingVertices = 0, pendingWorldVertices=0;
FrameResourceSet<StoredGeometry> pendingGeometry;
size_t pendingGeometryBytes=0;
FrameResourceSet<ColorImage> pendingWorldImages;
size_t pendingWorldImageBytes=0,pendingGuiDraws=0;
std::atomic<uint64_t> worldQueued{},worldVertexBudgetDrops{},worldCommandBudgetDrops{};
// World image decodes the engine thread performs from the guest's textures.
std::atomic<uint64_t> worldTextureDecodeAttempts{},worldTextureDecodeFailures{},worldTextureDecodeNanoseconds{};
// Objects the engine resolved on the GPU. They are sampled through the
// renderer's resolved view; their guest pixels are not a CPU-readable image, so
// a decode of one reads every block of the surface only to fail. Remembering
// which objects were resolved keeps that read off the engine thread.
std::mutex resolvedObjectMutex;
// A resolved address is trusted for a while, not forever: the engine frees and
// reuses guest memory, and a recycled address may later hold a CPU texture.
constexpr uint32_t resolvedObjectLifetimeFrames=900;
struct ResolvedObject {uint32_t object=0,frames=0;};
std::array<ResolvedObject,256> resolvedObjects{};
size_t resolvedObjectCount=0;
void recordResolvedObject(uint32_t object) {
    if(!object)return;
    const uint32_t now=uint32_t(frames.load(std::memory_order_relaxed));
    std::lock_guard lock(resolvedObjectMutex);
    for(size_t i=0;i<resolvedObjectCount;++i)
        if(resolvedObjects[i].object==object) {resolvedObjects[i].frames=now;return;}
    if(resolvedObjectCount<resolvedObjects.size())
        resolvedObjects[resolvedObjectCount++]={object,now};
    else {
        std::move(resolvedObjects.begin()+1,resolvedObjects.end(),resolvedObjects.begin());
        resolvedObjects.back()={object,now};
    }
}
bool isResolvedObject(uint32_t object) {
    if(!object)return false;
    const uint32_t now=uint32_t(frames.load(std::memory_order_relaxed));
    std::lock_guard lock(resolvedObjectMutex);
    for(size_t i=0;i<resolvedObjectCount;++i)
        if(resolvedObjects[i].object==object && now-resolvedObjects[i].frames<resolvedObjectLifetimeFrames)return true;
    return false;
}
// Why the decode below repeats, as counters rather than a guess: a decode that
// succeeds and is cached should not run again for the same resource next frame,
// so cacheHits/installs/budgetRejects separate a cache that misses from one that
// cannot hold the image. The window tally names the resources that repeat.
std::atomic<uint64_t> worldTextureCacheHits{},worldTextureDecodeInstalls{};
std::atomic<uint64_t> worldTextureDecodeRepeats{},worldTextureDecodeBudgetRejects{};
std::atomic<uint64_t> worldTextureKeyClashes{};
std::atomic<unsigned> worldTextureDecodeReasonsPrinted{};
struct WindowDecodeTally {uint32_t object=0,id=0,count=0,width=0,height=0;};
std::mutex windowDecodeMutex;
std::array<WindowDecodeTally,64> windowDecodes{};
size_t windowDecodeCount=0;
// True when this object was already decoded in this window. Entries past the
// table's capacity cannot be tallied, but they still count as installs.
bool noteWindowDecode(uint32_t object,uint32_t id,uint32_t width,uint32_t height) {
    std::lock_guard lock(windowDecodeMutex);
    for(size_t i=0;i<windowDecodeCount;++i)
        if(windowDecodes[i].object==object) {++windowDecodes[i].count;return true;}
    if(windowDecodeCount<windowDecodes.size())
        windowDecodes[windowDecodeCount++]={object,id,1,width,height};
    return false;
}
// A failed decode is remembered per resource and mip for a second. Most of
// them fail because the surface is not a CPU image at all; retrying every draw
// of every frame re-reads the same blocks for the same answer.
struct FailedWorldDecode {uint32_t object=0,mip=0,frames=0;};
std::mutex failedDecodeMutex;
std::array<FailedWorldDecode,128> failedWorldDecodes{};
size_t failedWorldDecodeCount=0;
bool worldDecodeRecentlyFailed(uint32_t object,unsigned mip) {
    const uint32_t now=uint32_t(frames.load(std::memory_order_relaxed));
    std::lock_guard lock(failedDecodeMutex);
    for(size_t i=0;i<failedWorldDecodeCount;++i) {
        auto& failed=failedWorldDecodes[i];
        if(failed.object==object && failed.mip==mip && now-failed.frames<60)return true;
    }
    return false;
}
void recordWorldDecodeFailure(uint32_t object,unsigned mip) {
    const uint32_t now=uint32_t(frames.load(std::memory_order_relaxed));
    std::lock_guard lock(failedDecodeMutex);
    for(size_t i=0;i<failedWorldDecodeCount;++i)
        if(failedWorldDecodes[i].object==object && failedWorldDecodes[i].mip==mip) {
            failedWorldDecodes[i].frames=now;return;
        }
    if(failedWorldDecodeCount<failedWorldDecodes.size())
        failedWorldDecodes[failedWorldDecodeCount++]={object,mip,now};
    else {
        std::move(failedWorldDecodes.begin()+1,failedWorldDecodes.end(),failedWorldDecodes.begin());
        failedWorldDecodes.back()={object,mip,now};
    }
}
std::atomic<uint64_t> worldImageBudgetDrops{},streamedParts{};
struct QueueFrameCounts {
    size_t worldDraws=0, commandDrops=0, vertexDrops=0, clearDrops=0, resolveDrops=0, guiDrops=0;
    size_t commands=0,parts=0,imageDrops=0;
    std::array<size_t,5> passes{};
};
QueueFrameCounts pendingCounts;
std::atomic<size_t> queueHighWater{0}, oversizedFrames{0};
unsigned queueFrameReports=0;
void publishPreviewPart(std::unique_lock<std::mutex>& lock,bool last);
bool liveStreaming() {return frameBackpressure && streamLargeFrames;}
bool reservePreviewCommand(std::unique_lock<std::mutex>& lock) {
    if(liveStreaming() && pending.size()>=livePartCommands)publishPreviewPart(lock,false);
    return pending.size()<4096;
}
std::array<std::shared_ptr<WorldQueryResult>,8> histogramResults;
thread_local std::shared_ptr<WorldQuery> activeHistogram;
// Original flare instances carry a u16 query ID, including 0xFFFF. Their
// completed visibility counts must not share the eight exposure bins.
constexpr unsigned flareQueryMaximumId=65535;
std::unordered_map<unsigned,std::shared_ptr<WorldQueryResult>> flareResults;
thread_local std::shared_ptr<WorldQuery> activeFlare;
// Task-59 bounded pre-capture failure diagnostics: profile-only (--profile-engine),
// failures-only. Categories mirror previewObserveImmediateWorld bail stages:
// 0 attr, 1 state, 2 stream, 3 geometry, 4 bindings, 5 device, 6 exception.
// Smoke-identified failures cap 32 per category; unidentified (tex0 probe
// failed) failures cap 4 total; first 2 smoke successes log a reference.
// Normal runs never touch these and perform no extra guest reads.
std::atomic<unsigned> immediateFailLogged[7]{};
std::atomic<unsigned> immediateGenericLogged{0};
std::atomic<unsigned> immediateSmokeSuccessLogged{0};
void reportImmediateCapture(unsigned category,uint32_t smokeTex0,uint32_t device,uint32_t indicesAddr,uint32_t count,
    uint32_t beFlags,uint32_t boundId,uint32_t boundVal,uint32_t descriptor,const ImmediateCaptureReason* reason,bool haveAttr) {
    try {
        const bool smoke=smokeTex0==1477 || smokeTex0==1371;
        if(smoke) {
            if(category>=7 || immediateFailLogged[category].fetch_add(1,std::memory_order_relaxed)>=32)return;
        } else if(immediateGenericLogged.fetch_add(1,std::memory_order_relaxed)>=4)return;
        constexpr const char* stages[]{"attr","state","stream","geometry","bindings","device","exception"};
        constexpr const char* substages[]{"ok","count","headerRead","headerBounds","streamBounds","streamCopy",
            "indexCopy","indexOob","vertexDecode","retainIndices","exception"};
        const size_t sub=reason?size_t(reason->stage):0;
        std::fprintf(stderr,
            "[ImmediateCaptureFail] frame=%llu ms=%llu smoke=%u tex0=%u stage=%s device=%08X indices=%08X count=%u "
            "beFlags=%08X boundId=%u boundVal=%u descriptor=%08X sub=%s d0=%u d1=%u d2=%u badV=%u badS=%u badL=%u rawBE=%08X rawN=%u "
            "refKnown=%u ref=%u refPos=%u stride=%u vc=%u\n",
            frames.load(std::memory_order_relaxed),GetTickCount64(),unsigned(smoke),smokeTex0,
            category<7?stages[category]:"?",device,indicesAddr,count,
            haveAttr?beFlags:0u,boundId,boundVal,descriptor,
            sub<11?substages[sub]:"?",
            reason?reason->detail0:0u,reason?reason->detail1:0u,reason?reason->detail2:0u,
            reason?reason->badVertex:0u,reason?reason->badSlot:0u,reason?reason->badLane:0u,
            reason?reason->rawBE:0u,reason?reason->rawSize:0u,
            reason?unsigned(reason->membershipKnown):0u,reason?unsigned(reason->referenced):0u,reason?reason->firstRefPos:0u,
            reason?reason->stride:0u,reason?reason->vertexCount:0u);
        bool anyFormat=false;
        if(reason)for(auto f:reason->formats)if(f){anyFormat=true;break;}
        if(reason && (reason->vertexByteCount || anyFormat)) {
            char fhex[16*2+1]{},vhex[256*2+1]{};
            auto nibble=[](unsigned v){return char(v<10?'0'+v:'a'+v-10);};
            for(unsigned i=0;i<16;++i){fhex[i*2]=nibble(reason->formats[i]>>4);fhex[i*2+1]=nibble(reason->formats[i]&15);}
            const uint32_t bytes=reason->vertexByteCount<256?reason->vertexByteCount:256;
            for(uint32_t i=0;i<bytes;++i){vhex[i*2]=nibble(reason->vertexBytes[i]>>4);vhex[i*2+1]=nibble(reason->vertexBytes[i]&15);}
            std::fprintf(stderr,"[ImmediateCaptureBytes] frame=%llu tex0=%u formats=%s vertex=%s\n",
                frames.load(std::memory_order_relaxed),smokeTex0,fhex,vhex);
        }
    } catch (...) {}
}
size_t textureBytes = 0;
size_t pendingVideoBytes = 0;
// Keep the faulting instructions inside a call covered by the SEH scope.
// Clang may lower an intrinsic memcpy in __try to unprotected inline loads.
DARK_NOINLINE void copyEngineBytes(void* output,const void* input,size_t size) {
    std::memcpy(output,input,size);
}
DARK_NOINLINE bool equalEngineBytes(const void* expected,const void* input,size_t size) {
    return std::memcmp(expected,input,size)==0;
}
}

PromptRenderContext currentPromptRenderContext() noexcept { return promptRenderContext; }
PromptRenderContext setPromptRenderContext(PromptRenderContext context) noexcept {
    const auto previous = promptRenderContext;
    promptRenderContext = context == PromptRenderContext::Gameplay ? context : PromptRenderContext::Menu;
    return previous;
}
void resetQueuedPromptContexts(uint8_t* base, uint32_t manager) noexcept {
    if (!base || !manager || (manager & 3)) return;
    try {
        std::lock_guard lock(promptContextMutex);
        auto* frame = findPromptContextFrame(base, manager);
        if (!frame) {
            for (auto& candidate : promptContextFrames)
                if (!candidate.base) { frame = &candidate; break; }
        }
        if (!frame) return;
        frame->gameplayDraws.clear();
        frame->subtitleDraws.clear();
        frame->base = base;
        frame->manager = manager;
        frame->generation = promptContextGeneration < UINT64_MAX ? ++promptContextGeneration : 0;
    } catch (...) {}
}
void retireQueuedPromptContexts() noexcept {
    try {
        std::lock_guard lock(promptContextMutex);
        for (auto& frame : promptContextFrames) {
            frame.gameplayDraws.clear();
            frame.subtitleDraws.clear();
            frame.base = nullptr;
            frame.manager = 0;
            frame.generation = 0;
        }
    } catch (...) {}
}
void retireQueuedPromptContexts(uint8_t* base, uint32_t manager) noexcept {
    if (!base || !manager) return;
    try {
        std::lock_guard lock(promptContextMutex);
        if (auto* frame = findPromptContextFrame(base, manager)) {
            frame->gameplayDraws.clear();
            frame->subtitleDraws.clear();
            frame->base = nullptr;
            frame->manager = 0;
            frame->generation = 0;
        }
    } catch (...) {}
}
uint64_t queuedPromptGeneration(uint8_t* base, uint32_t manager) noexcept {
    if (!base || !manager) return 0;
    try {
        std::lock_guard lock(promptContextMutex);
        if (const auto* frame = findPromptContextFrame(base, manager)) return frame->generation;
    } catch (...) {}
    return 0;
}
void forgetQueuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw) noexcept {
    forgetQueuedPromptContext(base, manager, draw, queuedPromptGeneration(base, manager));
}
void forgetQueuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw, uint64_t generation) noexcept {
    if (!base || !manager || !draw || !generation) return;
    try {
        std::lock_guard lock(promptContextMutex);
        if (auto* frame = findPromptContextFrame(base, manager); frame && frame->generation == generation) {
            frame->gameplayDraws.erase(draw);
            frame->subtitleDraws.erase(draw);
        }
    } catch (...) {}
}
void recordQueuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw, PromptRenderContext context) noexcept {
    recordQueuedPromptContext(base, manager, draw, context, queuedPromptGeneration(base, manager));
}
void recordQueuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw, PromptRenderContext context, uint64_t generation) noexcept {
    if (!base || !manager || !draw || (draw & 3) || !generation) return;
    try {
        std::lock_guard lock(promptContextMutex);
        if (auto* frame = findPromptContextFrame(base, manager); frame && frame->generation == generation) {
            // Clear first, including Menu and failed/overflowed replacements.
            frame->gameplayDraws.erase(draw);
            if (frame->generation && context == PromptRenderContext::Gameplay && frame->gameplayDraws.size() < promptDrawLimit)
                frame->gameplayDraws.insert(draw);
        }
    } catch (...) {}
}
PromptRenderContext queuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw) noexcept {
    return queuedPromptContext(base, manager, draw, queuedPromptGeneration(base, manager));
}
PromptRenderContext queuedPromptContext(uint8_t* base, uint32_t manager, uint32_t draw, uint64_t generation) noexcept {
    if (!base || !manager || !draw || (draw & 3) || !generation) return PromptRenderContext::Menu;
    try {
        std::lock_guard lock(promptContextMutex);
        if (const auto* frame = findPromptContextFrame(base, manager); frame && frame->generation == generation && frame->gameplayDraws.contains(draw))
            return PromptRenderContext::Gameplay;
    } catch (...) {}
    return PromptRenderContext::Menu;
}

bool currentSubtitleRenderContext() noexcept { return subtitleRenderContext; }
bool setSubtitleRenderContext(bool subtitle) noexcept {
    return std::exchange(subtitleRenderContext, subtitle);
}
void recordQueuedSubtitleContext(uint8_t* base, uint32_t manager, uint32_t draw,
                                 bool subtitle, uint64_t generation) noexcept {
    if (!base || !manager || !draw || (draw & 3) || !generation) return;
    try {
        std::lock_guard lock(promptContextMutex);
        if (auto* frame = findPromptContextFrame(base, manager); frame && frame->generation == generation) {
            frame->subtitleDraws.erase(draw);
            if (subtitle && frame->subtitleDraws.size() < promptDrawLimit)
                frame->subtitleDraws.insert(draw);
        }
    } catch (...) {}
}
bool queuedSubtitleContext(uint8_t* base, uint32_t manager, uint32_t draw, uint64_t generation) noexcept {
    if (!base || !manager || !draw || (draw & 3) || !generation) return false;
    try {
        std::lock_guard lock(promptContextMutex);
        if (const auto* frame = findPromptContextFrame(base, manager);
            frame && frame->generation == generation) return frame->subtitleDraws.contains(draw);
    } catch (...) {}
    return false;
}

#if !defined(__PROSPERO__)
bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size) {
    if (!base || !output || !address || address >= 0x100000000ull || size > 0x100000000ull-address) return false;
    // The AOT engine shares this process. Avoid a kernel transition for each
    // small draw-state read while still rejecting inaccessible/decommitted
    // ranges, including a decommit that races the copy. Callers publish only
    // complete snapshots and keep their existing source-stability checks.
#if defined(_WIN32)
    __try {
        copyEngineBytes(output,base+address,size);
        return true;
    } __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        if(renderMemoryFaults.fetch_add(1,std::memory_order_relaxed)<4)
            std::fprintf(stderr,"[RenderMemoryFault] source=%08llX bytes=%zu\n",address,size);
        return false;
    }
#else
    // The console's guest space is one committed 4 GB mapping, so the range
    // check above is the whole guard: an address inside it cannot fault the
    // title the way a Windows decommit can. The copy stays out of line for the
    // same reason it does on Windows (the faulting loads live in the callee).
    copyEngineBytes(output,base+address,size);
    return true;
#endif
}
bool equalRenderMemory(uint8_t* base,uint64_t address,const void* expected,size_t size) {
    if(!base || !expected || !address || address>=0x100000000ull || size>0x100000000ull-address)return false;
#if defined(_WIN32)
    __try {
        return equalEngineBytes(expected,base+address,size);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        renderMemoryFaults.fetch_add(1,std::memory_order_relaxed);return false;
    }
#else
    return equalEngineBytes(expected,base+address,size);
#endif
}
#endif
void setPreviewTextureBudget(size_t bytes) {
    std::lock_guard lock(queueMutex);
    const auto next=std::clamp(bytes,size_t(256*1024*1024),size_t(2ull*1024*1024*1024));
    if(colorBudgetReported && next==colorBudget)return;
    const bool shrink=next<colorBudget;
    colorBudget=next;
    colorBudgetReported=true;
    // Ordinary entries can be reconstructed; prompt generations must retain
    // their original button names even if they alone exceed a reduced budget.
    if(shrink)makeColorCacheRoom(0);
    std::fprintf(stderr,"[Performance] Native decoded texture cache budgetMiB=%zu\n",colorBudget/(1024*1024));
}

uint32_t worldTextureTiledOffset(uint32_t x,uint32_t y,uint32_t width,uint32_t bytesPerBlock) {
    // Native inverse of the original CPU texture upload's 32x32 block layout.
    // The original AOT tile routine828AFB00 is the independent test oracle.
    const uint32_t log=std::countr_zero(bytesPerBlock),aligned=(width+31)&~31u;
    const uint32_t macro=((x>>5)+(y>>5)*(aligned>>5))<<(log+7);
    const uint32_t micro=((x&7)+((y&6)<<2))<<log;
    const uint32_t offset=macro+((micro&~15u)<<1)+(micro&15)+((y&8)<<(log+3))+((y&1)<<4);
    return ((offset&~511u)<<3)+((offset&448)<<2)+(offset&63)+((y&16)<<7)+((((y&8)>>2)+(x>>3)&3)<<6);
}

const char* decodeSimpleMesh(uint8_t* base, uint32_t descriptor, uint32_t indexAddress,
                             uint32_t triangles, SimpleMesh& result, bool allowWhiteColor) {
    if (!triangles || triangles >= 65535) return "empty or batched triangle request";
    uint8_t header[68];
    if (!copyRenderMemory(base, descriptor, header, sizeof(header))) return "unreadable vertex descriptor";
    const uint32_t count = u16(header);
    if (!count || count > 16384 || triangles > 16384) return "preview geometry budget exceeded";
    if (u16(header + 2) || u32(header + 48) || u32(header + 56) || u32(header + 60) || u32(header + 64))
        return "additional vertex streams are unsupported";
    for (unsigned stage = 1; stage < 8; ++stage)
        if (u32(header + 8 + stage * 4) || header[40 + stage]) return "multiple UV streams are unsupported";
    const bool colored = u32(header + 52) != 0;
    if (!u32(header + 4) || !u32(header + 8) || header[40] != 2 || (!colored && !allowWhiteColor))
        return "preview requires float3 position, float2 UV0 and vertex color";
    std::vector<uint8_t> positions(count * 12), uv(count * 8), colors(count * 4), indices(triangles * 6);
    if (!copyRenderMemory(base, u32(header + 4), positions.data(), positions.size()) ||
        !copyRenderMemory(base, u32(header + 8), uv.data(), uv.size()) ||
        (colored && !copyRenderMemory(base, u32(header + 52), colors.data(), colors.size())) ||
        !copyRenderMemory(base, indexAddress, indices.data(), indices.size())) return "unreadable geometry stream";
    SimpleMesh decoded;
    decoded.promptContext = currentPromptRenderContext();
    decoded.vertices.resize(count);
    decoded.indices.resize(triangles * 3);
    for (size_t i = 0; i < decoded.indices.size(); ++i) {
        decoded.indices[i] = u16(indices.data() + i * 2);
        if (decoded.indices[i] >= count) return "triangle index outside vertex stream";
    }
    for (uint32_t i = 0; i < count; ++i) {
        auto& vertex = decoded.vertices[i];
        for (unsigned j = 0; j < 3; ++j) {
            vertex.position[j] = f32(positions.data() + i * 12 + j * 4);
            if (!std::isfinite(vertex.position[j])) return "nonfinite vertex position";
        }
        for (unsigned j = 0; j < 2; ++j) {
            vertex.uv[j] = f32(uv.data() + i * 8 + j * 4);
            if (!std::isfinite(vertex.uv[j])) return "nonfinite UV";
        }
        // Guest D3DCOLOR bytes are A,R,G,B. Vertex colors use UNORM, unlike
        // the engine's separate clear-color conversion, which uses 1/256.
        if (!colored) {
            std::fill(std::begin(vertex.color), std::end(vertex.color), 1.0f);
            continue;
        }
        vertex.color[0] = colors[i * 4 + 1] / 255.0f;
        vertex.color[1] = colors[i * 4 + 2] / 255.0f;
        vertex.color[2] = colors[i * 4 + 3] / 255.0f;
        vertex.color[3] = colors[i * 4] / 255.0f;
    }
    result = std::move(decoded);
    return nullptr;
}

static const char* decodePlane(uint8_t* base, uint32_t image, uint32_t expectedFormat,
                               uint32_t bytesPerPixel, AlphaImage& result) {
    uint8_t header[48];
    if (!copyRenderMemory(base, image, header, sizeof(header))) return "unreadable CImage";
    const uint32_t width = u32(header + 16), height = u32(header + 20), pitch = u32(header + 24);
    const uint32_t flags = u32(header + 40), format = u32(header + 32);
    if (u32(header) != 0x82097610 || format != expectedFormat || u32(header + 28) != bytesPerPixel ||
        !(flags & 0x800) || (flags & 0x1200)) return "image is not the required linear CPU plane";
    if (!width || !height || width > 2048 || height > 2048 || pitch < uint64_t(width) * bytesPerPixel || pitch > 8192 ||
        uint64_t(pitch) * height > u32(header + 12)) return "invalid image extent or pitch";
    // Same alignment rules as original CImage lock 82788B60.
    const uint64_t allocationAddress = u32(header + 8);
    uint64_t address = allocationAddress;
    if (flags & 0x200000) address = (address + 31) & ~31ull;
    else if (flags & 0x100000) address = (address + 15) & ~15ull;
    const uint64_t size = uint64_t(pitch) * height;
    if (address - allocationAddress + size > u32(header + 12)) return "aligned image exceeds its storage";
    if (!address || address + size > 0x100000000ull) return "image pointer overflow";
    std::vector<uint8_t> source(size);
    if (!copyRenderMemory(base, uint32_t(address), source.data(), source.size())) return "unreadable image pixels";
    AlphaImage decoded;
    decoded.width = width; decoded.height = height;
    const uint32_t rowBytes = width * bytesPerPixel;
    decoded.pixels.resize(size_t(rowBytes) * height);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(decoded.pixels.data() + y * rowBytes, source.data() + y * pitch, rowBytes);
    result = std::move(decoded);
    return nullptr;
}

const char* decodeAlphaImage(uint8_t* base, uint32_t image, AlphaImage& result) {
    return decodePlane(base, image, 0x40000, 1, result);
}

static const char* decodeColorBlocks(const std::vector<uint8_t>& blocks,uint32_t width,uint32_t height,uint32_t codec,ColorImage& result);
const char* decodeColorImage(uint8_t* base, uint32_t image, ColorImage& result) {
    uint8_t header[48], stream[16];
    if (!copyRenderMemory(base, image, header, sizeof(header))) return "unreadable color CImage";
    const uint32_t width = u32(header + 16), height = u32(header + 20), flags = u32(header + 40);
    // Original 82257450 selects the 16-byte block header with 0x4000.
    // Compressed images do NOT require the linear-image 0x800 flag.
    // Other streams and linear color layouts require separate reconstruction.
    if (u32(header) != 0x82097610 || u32(header + 28) != 4 || u32(header + 32) != 0x800 ||
        (flags & 0x15200) != 0x5000) return "unsupported color image layout";
    if (!width || !height || width > 2048 || height > 2048) return "invalid color image dimensions";
    const uint32_t allocation = u32(header + 8), storage = u32(header + 12);
    if (storage < 16 || !copyRenderMemory(base, allocation, stream, sizeof(stream))) return "unreadable block header";
    auto le32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                                           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
    const uint32_t codec = le32(stream), length = le32(stream + 4), offset = le32(stream + 12);
    // Only observed non-premultiplied codecs are accepted. Original switch
    // maps 0 to DXT1 (8-byte blocks), 1/2 to DXT3, and 3/4 to DXT5.
    if (codec != 0 && codec != 4) return "unsupported block codec";
    const uint32_t blockSize = codec == 0 ? 8 : 16;
    const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
    const uint64_t expected = uint64_t(blocksX) * blocksY * blockSize;
    if (offset != 16 || le32(stream + 8) || length != expected || uint64_t(offset) + length > storage ||
        uint64_t(allocation) + offset + length > 0x100000000ull) return "invalid compressed image extent";
    std::vector<uint8_t> blocks(length);
    if (!copyRenderMemory(base, allocation + offset, blocks.data(), blocks.size())) return "unreadable compressed pixels";
    return decodeColorBlocks(blocks,width,height,codec,result);
}
static const char* decodeColorBlocks(const std::vector<uint8_t>& blocks,uint32_t width,uint32_t height,uint32_t codec,ColorImage& result) {
    const uint32_t blockSize=codec==0?8:16,blocksX=(width+3)/4,blocksY=(height+3)/4;
    if((codec!=0 && codec!=4) || blocks.size()!=uint64_t(blocksX)*blocksY*blockSize)return "invalid block image size";
    auto le32=[](const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
    ColorImage decoded;
    decoded.width = width; decoded.height = height; decoded.sourceCodec = codec;
    decoded.pixels.resize(size_t(width) * height * 4);
    // BC1/BC3 bit layout and interpolation follow the Direct3D block-compression contract:
    // https://learn.microsoft.com/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
    // Decode before the original uploader's temporary byte swap. Cropping only
    // unused edge texels preserves the original non-multiple-of-four dimensions.
    if(!parallelSceneRange(size_t(blocksX)*blocksY,1024,[&](size_t first,size_t end) {
    for(size_t blockIndex=first;blockIndex<end;++blockIndex) {
        const auto by=uint32_t(blockIndex/blocksX),bx=uint32_t(blockIndex%blocksX);
        const uint8_t* block = blocks.data() + (size_t(by) * blocksX + bx) * blockSize;
        const uint8_t* color = block + (codec == 4 ? 8 : 0);
        const uint16_t c0 = uint16_t(color[0]) | uint16_t(color[1]) << 8;
        const uint16_t c1 = uint16_t(color[2]) | uint16_t(color[3]) << 8;
        uint8_t palette[4][4]{};
        for (unsigned i = 0; i < 2; ++i) {
            const uint16_t c = i ? c1 : c0;
            const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            palette[i][0] = uint8_t((r << 3) | (r >> 2));
            palette[i][1] = uint8_t((g << 2) | (g >> 4));
            palette[i][2] = uint8_t((b << 3) | (b >> 2));
            palette[i][3] = 255;
        }
        if (codec == 4 || c0 > c1) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                palette[2][channel] = uint8_t((2 * palette[0][channel] + palette[1][channel]) / 3);
                palette[3][channel] = uint8_t((palette[0][channel] + 2 * palette[1][channel]) / 3);
            }
            palette[2][3] = palette[3][3] = 255;
        } else {
            for (unsigned channel = 0; channel < 3; ++channel)
                palette[2][channel] = uint8_t((palette[0][channel] + palette[1][channel]) / 2);
            palette[2][3] = 255; // BC1 index 3 stays transparent black.
        }
        uint8_t alpha[8]{};
        uint64_t alphaIndices = 0;
        if (codec == 4) {
            alpha[0] = block[0]; alpha[1] = block[1];
            if (alpha[0] > alpha[1]) {
                for (unsigned i = 1; i <= 6; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
            } else {
                for (unsigned i = 1; i <= 4; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
                alpha[6] = 0; alpha[7] = 255;
            }
            for (unsigned i = 0; i < 6; ++i) alphaIndices |= uint64_t(block[i + 2]) << (i * 8);
        }
        const uint32_t colorIndices = le32(color + 4);
        for (unsigned y = 0; y < 4 && by * 4 + y < height; ++y)
            for (unsigned x = 0; x < 4 && bx * 4 + x < width; ++x) {
                const unsigned index = y * 4 + x;
                uint8_t* out = decoded.pixels.data() + (size_t(by * 4 + y) * width + bx * 4 + x) * 4;
                std::memcpy(out, palette[(colorIndices >> (index * 2)) & 3], 4);
                if (codec == 4) out[3] = alpha[(alphaIndices >> (index * 3)) & 7];
            }
    }
    return true;
    }))return "block image decode failed";
    result = std::move(decoded);
    return nullptr;
}

const char* decodeWorldTextureImage(uint8_t* base,uint32_t object,ColorImage& result,unsigned firstMip) {
    uint8_t header[64]{};
    if(!copyRenderMemory(base,object,header,sizeof(header)))return "unreadable texture resource";
    uint32_t fetch[6]{};for(unsigned i=0;i<6;++i)fetch[i]=u32(header+28+i*4);
    const uint32_t baseWidth=(fetch[2]&8191)+1,baseHeight=((fetch[2]>>13)&8191)+1;
    if(baseWidth>2048 || baseHeight>2048)return "unsupported streamed texture dimensions";
    const uint32_t format=fetch[1]&63,endian=(fetch[1]>>6)&3,swizzle=fetch[3];
    const bool compressed=format==18 || format==20 || format==49,tiled=(fetch[0]&0x80000000u)!=0;
    const unsigned lastMip=(std::min)((fetch[4]>>6)&15,unsigned(std::bit_width((std::max)(baseWidth,baseHeight)))-1u);
    if(firstMip>lastMip)return "resident mip range exceeds resource";
    ColorImage image;image.width=baseWidth;image.height=baseHeight;image.authoredMips=true;
    image.firstMip=firstMip;image.mips.resize(lastMip);
    for(unsigned mip=firstMip;mip<=lastMip;++mip) {
        TextureMipLayout layout;
        if(const auto error=getTextureMipLayout(fetch,0,mip,layout))return error;
        const unsigned width=layout.width,height=layout.height,faces=layout.faces;
        const unsigned bw=layout.blocksWide,bh=layout.blocksHigh,bytes=layout.bytesPerBlock;
        const uint64_t sourceBytes=uint64_t(layout.faceStrideBytes)*faces;
        if(sourceBytes>32*1024*1024)return "streamed texture extent overflow";
        std::vector<uint8_t> linear(size_t(bw)*bh*bytes);
        std::vector<uint8_t> combined;combined.reserve(size_t(width)*height*4*faces);
        for(unsigned face=0;face<faces;++face) {
            // Fast path: copy the whole face once (single SEH guard) and swizzle
            // from that snapshot without further kernel transitions. Padding gaps
            // can be uncommitted in sparse suballocations; on failure fall back
            // to per-block reads that touch only addressed blocks.
            const uint64_t faceBase=uint64_t(layout.allocationAddress)+layout.surfaceOffsetBytes+
                uint64_t(face)*layout.faceStrideBytes;
            std::vector<uint8_t> faceSnapshot;
            bool haveSnapshot=false;
            if(layout.faceStrideBytes && layout.faceStrideBytes<=8*1024*1024 &&
               faceBase+layout.faceStrideBytes<=0x100000000ull) {
                faceSnapshot.resize(layout.faceStrideBytes);
                if(copyRenderMemory(base,uint32_t(faceBase),faceSnapshot.data(),faceSnapshot.size()))
                    haveSnapshot=true;
                else
                    faceSnapshot.clear();
            }
            auto readSpan=[&](uint32_t alignedOffset,unsigned span,uint8_t* output)->bool {
                if(uint64_t(alignedOffset)+span>layout.faceStrideBytes) return false;
                if(haveSnapshot) {
                    std::memcpy(output,faceSnapshot.data()+alignedOffset,span);
                    return true;
                }
                return copyRenderMemory(base,uint32_t(faceBase+alignedOffset),output,span);
            };
            for(unsigned y=0;y<bh;++y)for(unsigned x=0;x<bw;++x) {
                const unsigned sx=x+layout.originBlockX,sy=y+layout.originBlockY;
                const uint32_t offset=tiled?worldTextureTiledOffset(sx,sy,layout.pitchBlocks,bytes):
                    sy*layout.rowPitchBytes+sx*bytes;
                if(uint64_t(offset)+bytes>layout.faceStrideBytes)return "texture block escaped face allocation";
                // Read only addressed blocks. Original suballocation can leave
                // gaps in padded faces; accessibility of a neighboring page is
                // not evidence that its padding belongs to this texture.
                const unsigned swap=endian==1?1:endian==2?3:endian==3?2:0;
                const unsigned span=(std::max)(bytes,1u<<std::bit_width(swap));
                const unsigned alignedOffset=offset&~(span-1);
                uint8_t input[16]{};
                if(!readSpan(alignedOffset,span,input))
                    return "unreadable streamed texture pixels";
                for(unsigned lane=0;lane<bytes;++lane) {
                    const unsigned swapped=(offset+lane)^swap;
                    if(swapped>=layout.faceStrideBytes)return "endian-swapped byte escaped texture extent";
                    linear[(size_t(y)*bw+x)*bytes+lane]=input[swapped-alignedOffset];
                }
            }
            ColorImage decoded;
            if(format==2) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=2;decoded.pixels.resize(size_t(width)*height*4);
                for(size_t i=0;i<linear.size();++i) {decoded.pixels[i*4]=linear[i];decoded.pixels[i*4+3]=255;}
            } else if(format==3) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=3;decoded.pixels.resize(size_t(width)*height*4);
                for(size_t i=0;i<linear.size()/2;++i) {
                    const auto rgba=decodeTexture1555(linear.data()+i*2);
                    std::copy(rgba.begin(),rgba.end(),decoded.pixels.begin()+i*4);
                }
            } else if(format==10) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=10;decoded.pixels.resize(size_t(width)*height*4);
                for(size_t i=0;i<linear.size()/2;++i) {
                    decoded.pixels[i*4]=linear[i*2];decoded.pixels[i*4+1]=linear[i*2+1];
                    decoded.pixels[i*4+3]=255;
                }
            } else if(format==49) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=49;decoded.pixels.resize(size_t(width)*height*4);
                auto channel=[](const uint8_t* p,std::array<uint8_t,16>& out) {
                    uint8_t table[8]{p[0],p[1]};
                    if(p[0]>p[1])for(unsigned k=1;k<=6;++k)table[k+1]=uint8_t(((7-k)*p[0]+k*p[1])/7);
                    else {for(unsigned k=1;k<=4;++k)table[k+1]=uint8_t(((5-k)*p[0]+k*p[1])/5);table[6]=0;table[7]=255;}
                    uint64_t indices=0;for(unsigned k=0;k<6;++k)indices|=uint64_t(p[k+2])<<(8*k);
                    for(unsigned k=0;k<16;++k)out[k]=table[(indices>>(3*k))&7];
                };
                for(unsigned by=0;by<bh;++by)for(unsigned bx=0;bx<bw;++bx) {
                    const auto* p=linear.data()+(size_t(by)*bw+bx)*16;std::array<uint8_t,16> red{},green{};
                    channel(p,red);channel(p+8,green);
                    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(bx*4+x<width && by*4+y<height) {
                        auto* pixel=decoded.pixels.data()+(size_t(by*4+y)*width+bx*4+x)*4;
                        pixel[0]=red[y*4+x];pixel[1]=green[y*4+x];pixel[2]=0;pixel[3]=255;
                    }
                }
            } else if(compressed) {
                if(const auto error=decodeColorBlocks(linear,width,height,format==18?0:4,decoded))return error;
            } else {decoded.width=width;decoded.height=height;decoded.sourceCodec=format;decoded.pixels=linear;}
            // The original resource's component selectors include constants0/1.
            for(size_t i=0;i<decoded.pixels.size();i+=4) {
                const std::array<uint8_t,6> channels{decoded.pixels[i],decoded.pixels[i+1],decoded.pixels[i+2],decoded.pixels[i+3],0,255};
                for(unsigned c=0;c<4;++c) {
                    const auto select=(swizzle>>(1+c*3))&7;
                    if(select>=channels.size())return "unsupported texture component selector";
                    decoded.pixels[i+c]=channels[select];
                }
            }
            image.sourceCodec=decoded.sourceCodec;
            combined.insert(combined.end(),decoded.pixels.begin(),decoded.pixels.end());
        }
        image.faces=faces;
        if(!mip)image.pixels=std::move(combined);else image.mips[mip-1]=std::move(combined);
    }
    result=std::move(image);return nullptr;
}

const char* decodeVideoFrame(uint8_t* base, uint32_t frame, VideoFrame& result) {
    if (!frame || uint64_t(frame) + 108 > 0x100000000ull) return "video frame pointer overflow";
    uint8_t time[8];
    if (!copyRenderMemory(base, frame, time, sizeof(time))) return "unreadable video timestamp";
    const double timestamp = std::bit_cast<double>((uint64_t(u32(time)) << 32) | u32(time + 4));
    if (!std::isfinite(timestamp) || timestamp < 0) return "invalid video timestamp";
    AlphaImage y, uv;
    if (const char* error = decodePlane(base, frame + 12, 0x2000, 1, y)) return error;
    if (const char* error = decodePlane(base, frame + 60, 0x20000, 2, uv)) return error;
    if (uv.width * 2 != y.width || uv.height * 2 != y.height) return "video chroma plane size mismatch";
    VideoFrame decoded{y.width, y.height, std::move(y.pixels), std::move(uv.pixels)};
    decoded.timestampSeconds = timestamp;
    result = std::move(decoded);
    return nullptr;
}

void previewObserveVideo(uint8_t* base, uint32_t container, uint32_t localId) {
    if (!active || localId % 3 != 2) return;
    uint8_t header[64], array[28], video[136];
    if (!copyRenderMemory(base, container, header, sizeof(header)) || u32(header) != 0x82097C50 ||
        !copyRenderMemory(base, u32(header + 56), array, sizeof(array)) || localId / 3 >= u32(array + 4)) return;
    const uint64_t entry = uint64_t(u32(array + 24)) + (localId / 3) * 4ull;
    uint32_t owner = 0;
    if (entry + 4 > 0x100000000ull || !readWord(base, uint32_t(entry), owner) ||
        !copyRenderMemory(base, owner, video, sizeof(video)) || u32(video) != 0x82097CF8) return;
    const uint32_t frame = u32(video + 80), key = u32(video + 126);
    std::array<uint8_t, 8> timestamp;
    if (!frame || !u16(video + 126) || !u16(video + 128) ||
        !copyRenderMemory(base, frame, timestamp.data(), timestamp.size())) return;
    {
        std::lock_guard lock(queueMutex);
        if (auto it = videos.find(key); it != videos.end() && it->second.owner == owner &&
            it->second.frame == frame && it->second.timestamp == timestamp) return;
    }
    auto image = std::make_shared<VideoFrame>();
    if (decodeVideoFrame(base, frame, *image)) return;
    // The original container retains this selected frame until its next
    // update on the engine thread. Only owned plane copies cross threads.
    std::lock_guard lock(queueMutex);
    if (!videos.contains(key) && videos.size() >= 4) videos.erase(videos.begin());
    videos[key] = {owner, frame, timestamp, image};
    if (++videoImages == 1) std::printf("[EnginePreview] Copied original YUV video frame %ux%u, texture pair=%08X\n",
                                     image->width, image->height, key);
}

void enableEnginePreview() { active = true; }
bool enginePreviewEnabled() { return active.load(std::memory_order_relaxed); }
void previewPrepareTexture(uint32_t id) {
    if (!active || !id) return;
    std::lock_guard lock(queueMutex);
    alphaColors.erase(id);
    if (auto it = textures.find(id); it != textures.end()) {
        textureBytes -= it->second->pixels.size(); textures.erase(it);
    }
    if (auto it = colorTextures.find(id); it != colorTextures.end()) {
        colorBytes -= it->second.image->bytes(); colorTextures.erase(it);
    }
}
std::shared_ptr<const ColorImage> previewCapturedTexture(uint32_t id,uint32_t resource) {
    std::lock_guard lock(queueMutex);
    const auto it=colorTextures.find(id);
    return it!=colorTextures.end() && it->second.resource==resource?it->second.image:nullptr;
}
void previewPublishTexture(uint32_t id,uint32_t resource,std::shared_ptr<const ColorImage> image) {
    if(!active || !id || !resource || !image || !image->valid())return;
    std::lock_guard lock(queueMutex);
    if(image->bytes()>colorBudget)return;
    if(auto it=colorTextures.find(id);it!=colorTextures.end()) {colorBytes-=it->second.image->bytes();colorTextures.erase(it);}
    if (!makeColorCacheRoom(image->bytes())) return;
    colorBytes+=image->bytes();colorTextures[id]={image,++colorUse,resource};++colorImages;
    if(image->authoredMips && !image->firstMip) {
        if(texturePreloads.size()==texturePreloadLimit) {
            texturePreloads.pop_front();++texturePreloadCounts.overflow;
        }
        texturePreloads.emplace_back(image);++texturePreloadCounts.queued;
    }
    if (TextureUploadTraceEnabled())
        std::fprintf(stderr,"[EngineImageUpload] authored id=%u resource=%08X size=%ux%u faces=%u first=%u levels=%zu bytes=%zu\n",
            id,resource,image->width,image->height,image->faces,image->firstMip,image->mips.size()+1,image->bytes());
}
bool takePreviewTexturePreload(std::shared_ptr<const ColorImage>& image) {
    // A caller's previous selection may own large pixel vectors. Release it
    // before taking the handoff lock, like the frame consumer's retire path.
    image.reset();
    std::lock_guard lock(queueMutex);
    if(texturePreloads.empty())return false;
    image=texturePreloads.front().lock();texturePreloads.pop_front();
    if(image)++texturePreloadCounts.taken;else ++texturePreloadCounts.expired;
    return true;
}
PreviewTexturePreloadCounters previewTexturePreloadCounters() {
    std::lock_guard lock(queueMutex);
    auto counters=texturePreloadCounts;counters.pending=texturePreloads.size();return counters;
}
std::shared_ptr<const ColorImage> promptReplacementFor(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context) {
    const auto nativeSource = source == PromptRenderSource::Controller ?
        Prompts::Source::Controller : Prompts::Source::KeyboardMouse;
    const auto nativeContext = context == PromptRenderContext::Gameplay ?
        Prompts::Context::Gameplay : Prompts::Context::Menu;
    return Prompts::replacementFor(textureId, image, nativeSource, nativeContext);
}
std::shared_ptr<const ColorImage> promptReplacementForUv(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context,
    float u0, float v0, float u1, float v1) {
    const auto nativeSource = source == PromptRenderSource::Controller ?
        Prompts::Source::Controller : Prompts::Source::KeyboardMouse;
    const auto nativeContext = context == PromptRenderContext::Gameplay ?
        Prompts::Context::Gameplay : Prompts::Context::Menu;
    return Prompts::replacementForUv(textureId, image, nativeSource, nativeContext, u0, v0, u1, v1);
}
std::atomic<uint64_t> gPromptAttempts{0}, gPromptClassified{0};
void countPromptOriginAttempt(bool classified) {
    const uint64_t attempts = ++gPromptAttempts;
    if (classified) ++gPromptClassified;
    if ((attempts & 255) == 0)
        std::fprintf(stderr, "[PromptOrigin] attempts=%llu classified=%llu\n",
            (unsigned long long)attempts, (unsigned long long)gPromptClassified.load());
}
void printPromptOriginCounters() {
    std::fprintf(stderr, "[PromptOrigin] attempts=%llu classified=%llu\n",
        (unsigned long long)gPromptAttempts.load(), (unsigned long long)gPromptClassified.load());
}
uint64_t promptOriginAttempts() { return gPromptAttempts.load(); }
uint64_t promptOriginClassified() { return gPromptClassified.load(); }
namespace {
struct PromptPending { uint32_t image = 0; uint8_t origin = 0; bool valid = false; };
thread_local PromptPending pendingPrompt;
thread_local PromptPending pendingStack[8];
thread_local unsigned pendingDepth = 0;
thread_local unsigned pendingOverflow = 0;
}
void previewBeginPromptCapture(uint32_t imageAddress) {
    if (pendingDepth < 8) {
        pendingStack[pendingDepth++] = pendingPrompt;
        pendingPrompt.image = imageAddress;
        pendingPrompt.origin = 0;
        pendingPrompt.valid = false;
    } else {
        // Fail closed: leave the saved depth-8 state untouched. Commits and
        // consumes while overflowed yield unknown without storing or clearing.
        ++pendingOverflow;
    }
}
bool previewCommitPromptOrigin(uint32_t imageAddress, uint8_t origin) {
    if (!imageAddress) return false;
    if (!pendingDepth && !pendingOverflow) return false;
    if (pendingOverflow) return false;
    if (!pendingPrompt.valid || pendingPrompt.image == imageAddress || pendingPrompt.image == 0) {
        pendingPrompt.image = imageAddress;
        pendingPrompt.origin = origin;
        pendingPrompt.valid = true;
        return true;
    }
    return false;
}
uint8_t takePromptOriginFor(uint32_t imageAddress) {
    if (pendingOverflow) return 0;
    uint8_t origin = 0;
    if (pendingPrompt.valid && pendingPrompt.image == imageAddress) origin = pendingPrompt.origin;
    pendingPrompt = {};
    return origin;
}
void previewEndPromptCapture() {
    if (pendingOverflow) { --pendingOverflow; return; }
    if (pendingDepth) pendingPrompt = pendingStack[--pendingDepth];
    else pendingPrompt = {};
}
void previewSetPendingPromptOrigin(uint32_t imageAddress, uint8_t origin) {
    previewCommitPromptOrigin(imageAddress, origin);
}
void previewObserveImage(uint8_t* base, uint32_t address, uint32_t id, uint32_t mip) {
    const uint8_t scopedOrigin = takePromptOriginFor(address);
    if (!active || !id || mip) return;
    auto image = std::make_shared<AlphaImage>();
    if (decodeAlphaImage(base, address, *image)) {
        auto color = std::make_shared<ColorImage>();
        if (decodeColorImage(base, address, *color)) { ++rejectedImages; return; }
        color->promptOrigin = scopedOrigin;
        std::lock_guard lock(queueMutex);
        if (auto it = colorTextures.find(id); it != colorTextures.end()) {
            colorBytes -= it->second.image->bytes(); colorTextures.erase(it);
        }
        if (!makeColorCacheRoom(color->bytes())) { ++rejectedImages; return; }
        colorBytes += color->bytes(); colorTextures[id] = {color, ++colorUse}; ++colorImages;
        if (TextureUploadTraceEnabled())
            std::printf("[EnginePreview] Copied original color texture id=%u size=%ux%u codec=%u\n",
                        id, color->width, color->height, color->sourceCodec);
        return;
    }
    std::lock_guard lock(queueMutex);
    if (textureBytes + image->pixels.size() > 32 * 1024 * 1024) { ++rejectedImages; return; }
    if (auto it = textures.find(id); it != textures.end()) textureBytes -= it->second->pixels.size();
    textureBytes += image->pixels.size(); textures[id] = image; alphaColors.erase(id); ++images;
    if (TextureUploadTraceEnabled())
        std::printf("[EnginePreview] Copied original alpha texture id=%u size=%ux%u\n", id, image->width, image->height);
}
std::shared_ptr<const AlphaImage> previewAlphaImageForSubtitleFont(uint32_t id) noexcept {
    try {
        std::lock_guard lock(queueMutex);
        const auto found = textures.find(id);
        return found == textures.end() ? nullptr : found->second;
    } catch (...) { return {}; }
}
std::shared_ptr<const ColorImage> previewColorImageForSubtitleFont(uint32_t id) noexcept {
    try {
        std::lock_guard lock(queueMutex);
        const auto found = colorTextures.find(id);
        return found == colorTextures.end() ? nullptr : found->second.image;
    } catch (...) { return {}; }
}
const char* decodeUploadImage(uint8_t* base,uint32_t address,uint32_t pixels,ColorImage& result) {
    uint8_t header[48]{};
    if (!copyRenderMemory(base,address,header,sizeof(header))) return "unreadable upload CImage";
    const auto width=u32(header+16),height=u32(header+20),format=u32(header+32),flags=u32(header+40);
    if(flags&0x200)return "upload CImage is metadata only";
    const bool alpha=format==0x40000 && u32(header+28)==1 && (flags&0x15800)==0x800;
    const bool luminance=format==0x2000 && u32(header+28)==1 && (flags&0x15800)==0x800;
    const bool luminanceAlpha=format==0x20000 && u32(header+28)==2 && (flags&0x15800)==0x800;
    const bool dxn=format==0x20000 && u32(header+28)==2 && (flags&0x15000)==0x11000;
    const bool rgb=(format==0x40 || format==0x800) && u32(header+28)==4 && (flags&0x15800)==0x800;
    if (!alpha && !luminance && !luminanceAlpha && !dxn && !rgb) {
        if(uint64_t(u32(header+8))+16!=pixels)return "upload pixels do not identify owned BC stream";
        return decodeColorImage(base,address,result);
    }
    if (u32(header)!=0x82097610 || !width || !height || width>2048 || height>2048) return "invalid upload image layout";
    const uint32_t allocation=u32(header+8),storage=u32(header+12);
    const uint32_t pitch=u32(header+24);
    if (!dxn && (pitch<uint64_t(width)*(rgb?4:luminanceAlpha?2:1) || pitch>8192)) return "invalid upload pitch";
    const size_t bytes=dxn?size_t((width+3)/4)*((height+3)/4)*16:size_t(pitch)*height;
    if (pixels<allocation || uint64_t(pixels)+bytes>uint64_t(allocation)+storage || uint64_t(pixels)+bytes>0x100000000ull)
        return "upload pixels escape owned CImage allocation";
    std::vector<uint8_t> input(bytes);
    if (!copyRenderMemory(base,pixels,input.data(),bytes)) return "unreadable upload pixels";
    ColorImage image;image.width=width;image.height=height;image.sourceCodec=alpha?0x40000:dxn?49:luminanceAlpha?10:rgb?format:2;image.pixels.resize(size_t(width)*height*4);
    if (rgb) {
        // 8226977C/B8 map CImage0x40/0x800 to XDK X8R8G8B8/A8R8G8B8.
        // This observer runs before the temporary 82257E8C word byte-swap;
        // original CImage storage is B,G,R,X/A, with row pitch retained.
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto* in=input.data()+size_t(y)*pitch+x*4;auto* out=image.pixels.data()+(size_t(y)*width+x)*4;
            out[0]=in[2];out[1]=in[1];out[2]=in[0];out[3]=format==0x800?in[3]:255;
        }
    } else if (alpha) {
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const size_t i=size_t(y)*width+x;image.pixels[i*4]=image.pixels[i*4+1]=image.pixels[i*4+2]=255;
            image.pixels[i*4+3]=input[size_t(y)*pitch+x];
        }
    } else if (luminanceAlpha) {
        // Original82269810 maps linear CImage0x20000 to A8L8 (fetch10),
        // distinct from compressed DXN's use of the same CImage format.
        // Before the original upload's byte swap BE A8L8 stores A,L (V,U).
        // Preserve its logical sampler view L,L,L,A for the YUV program.
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto* in=input.data()+size_t(y)*pitch+x*2;
            auto* out=image.pixels.data()+(size_t(y)*width+x)*4;
            out[0]=out[1]=out[2]=in[1];out[3]=in[0];
        }
    } else if (luminance) {
        // 82269700 maps CImage format0x2000 to XDK L8=0x28000102.
        for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const size_t i=size_t(y)*width+x;image.pixels[i*4]=image.pixels[i*4+1]=image.pixels[i*4+2]=input[size_t(y)*pitch+x];image.pixels[i*4+3]=255;
        }
    } else {
        auto block=[](const uint8_t* p,std::array<uint8_t,16>& values) {
            uint8_t table[8]{p[0],p[1]};
            if (p[0]>p[1]) for (unsigned k=1;k<=6;++k) table[k+1]=uint8_t(((7-k)*p[0]+k*p[1])/7);
            else {for (unsigned k=1;k<=4;++k)table[k+1]=uint8_t(((5-k)*p[0]+k*p[1])/5);table[6]=0;table[7]=255;}
            uint64_t indices=0;for(unsigned k=0;k<6;++k)indices|=uint64_t(p[k+2])<<(8*k);
            for(unsigned k=0;k<16;++k)values[k]=table[(indices>>(3*k))&7];
        };
        const unsigned blocksX=(width+3)/4;
        for (unsigned by=0;by<(height+3)/4;++by) for(unsigned bx=0;bx<blocksX;++bx) {
            const auto* p=input.data()+(size_t(by)*blocksX+bx)*16;
            std::array<uint8_t,16> luma{},alpha{};block(p,luma);block(p+8,alpha);
            // Original82257450 chooses0x08000171: DXN/BC5 with component
            // selectors0,0,0,1 (bits18,21,24,27). Match its L,L,L,A view.
            for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(bx*4+x<width && by*4+y<height) {
                auto* out=image.pixels.data()+(size_t(by*4+y)*width+bx*4+x)*4;const auto k=y*4+x;
                out[0]=out[1]=out[2]=luma[k];out[3]=alpha[k];
            }
        }
    }
    result=std::move(image);return nullptr;
}
static bool submitTriangles(uint8_t* base, uint32_t indices, uint32_t triangles) {
    if (!active || worldActive) return false;
    ++observed;
    // 65535 describes a list of child requests. Original recursion reaches
    // these wrappers again; the parent must not duplicate its child meshes.
    if (triangles == 65535 || !triangles || !indices) return false;
    uint8_t attributes[160], projection[64], model[64], viewport[16];
    uint32_t matrixBase = 0, matrixIndex = 0, descriptor = 0, vbId = 0;
    auto reject = [&](unsigned reason) { ++unsupported; ++rejectionReasons[reason]; return false; };
    if (!copyRenderMemory(base, context + 16896, attributes, sizeof(attributes)) ||
        !copyRenderMemory(base, context + 17088, projection, sizeof(projection)) ||
        !copyRenderMemory(base, context + 17152, viewport, sizeof(viewport)) ||
        !readWord(base, context + 8224, matrixBase) || !readWord(base, context + 8232, matrixIndex) ||
        !readWord(base, context + 16512, descriptor) || !readWord(base, context + 12564, vbId) ||
        matrixIndex >= 256 || uint64_t(matrixBase) + matrixIndex * 656ull + 80 > 0x100000000ull ||
        !copyRenderMemory(base, matrixBase + matrixIndex * 656 + 16, model, 64)) return reject(0);
    // Original 8225D780 selects the explicit CPU descriptor before the
    // stored-buffer ID. The ID may remain set while immediate data is active.
    // Inline data is a fallback only when both selectors are absent.
    if (!descriptor && vbId) return reject(1);
    // Initial bring-up supports the captured fixed-function text material:
    // identity model-view, one alpha texture, vertex tint and alpha blending.
    // Other material/pass contracts stay explicitly counted as unsupported.
    const bool textMaterial = !u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100218 &&
                             attributes[144] == 5 && attributes[145] == 6;
    const bool opaqueMaterial = !u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100212 &&
                               attributes[144] == 2 && attributes[145] == 1;
    bool videoMaterial = false;
    if (u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100212 &&
        attributes[144] == 2 && attributes[145] == 1 && u32(attributes + 108) == 0xFFFFFFFF &&
        u32(attributes + 116) == 0xFFFFFFFF) {
        uint8_t program[20];
        constexpr char name[] = "CMWnd_ModTexture_PaintVideo_YUV2RGB";
        char actual[sizeof(name)];
        videoMaterial = copyRenderMemory(base, u32(attributes), program, sizeof(program)) && u32(program) == 5 &&
            !u32(program + 8) && !u32(program + 12) && !u32(program + 16) &&
            copyRenderMemory(base, u32(program + 4), actual, sizeof(actual)) && !std::memcmp(actual, name, sizeof(name));
    }
    if (!textMaterial && !opaqueMaterial && !videoMaterial) return reject(2);
    const auto displayMode = nativeVideoMode();
    const bool originalViewport = u32(viewport + 8) == 1280 && u32(viewport + 12) == 720;
    const bool nativeViewport = u32(viewport + 8) == displayMode.width && u32(viewport + 12) == displayMode.height;
    if (u32(viewport) || u32(viewport + 4) || (!originalViewport && !nativeViewport))
        return reject(3);
    for (unsigned stage = videoMaterial ? 2 : 1; stage < 16; ++stage)
        if (u16(attributes + 8 + stage * 2)) return reject(4);
    for (unsigned i = 0; i < 16; ++i)
        if (f32(model + i * 4) != (i % 5 == 0 ? 1.0f : 0.0f)) return reject(5);
    std::shared_ptr<const AlphaImage> texture;
    std::shared_ptr<const VideoFrame> video;
    std::shared_ptr<const ColorImage> colorTexture;
    {
        std::lock_guard lock(queueMutex);
        if (videoMaterial) {
            auto it = videos.find(u32(attributes + 8));
            if (it != videos.end()) video = it->second.image;
        } else {
            auto it = textures.find(u16(attributes + 8));
            if (textMaterial && it != textures.end()) texture = it->second;
            auto color = colorTextures.find(u16(attributes + 8));
            if (!texture && color != colorTextures.end() && color->second.image->faces==1) {
                colorTexture = color->second.image; color->second.used = ++colorUse;
            }
        }
    }
    if (!texture && !video && !colorTexture) {
        if(profileEngineCpu) {
            static unsigned reports=0;
            if(reports++<64)std::fprintf(stderr,
                "[GuiMissingTexture] id=%u pair=%08X material=%s descriptor=%08X triangles=%u flags=%08X\n",
                unsigned(u16(attributes+8)),u32(attributes+8),videoMaterial?"video":textMaterial?"text":"opaque",
                descriptor,triangles,u32(attributes+92));
        }
        return reject(6);
    }
    SimpleMesh mesh;
    if (!descriptor) descriptor = context + 12480;
    const bool whiteColor = videoMaterial || (colorTexture && u32(attributes + 108) == 0xFFFFFFFF &&
                                             u32(attributes + 116) == 0xFFFFFFFF);
    if (decodeSimpleMesh(base, descriptor, indices, triangles, mesh, whiteColor)) return reject(7);
    for (unsigned i = 0; i < 16; ++i) {
        mesh.projection[i] = f32(projection + i * 4);
        if (!std::isfinite(mesh.projection[i])) return reject(8);
    }
    mesh.texture = texture;
    mesh.video = video;
    mesh.colorTexture = colorTexture;
    mesh.textureId = u16(attributes + 8);
    mesh.opaque = opaqueMaterial;
    mesh.subtitle = currentSubtitleRenderContext();
    std::unique_lock lock(queueMutex);
    const size_t videoBytes = video ? video->luma.size() + video->chroma.size() : 0;
    auto newColorBytes = colorTexture && std::none_of(pending.begin(), pending.end(),
        [&](const auto& queued) { return queued.colorTexture == colorTexture; }) ? colorTexture->bytes() : 0;
    // World commands must not exhaust the separate UI draw allowance.
    if(liveStreaming() && !pending.empty() && (pending.size()>=livePartCommands ||
       pendingVertices+mesh.vertices.size()>262144 || pendingVideoBytes+videoBytes>32*1024*1024 ||
       pendingColorBytes+newColorBytes>64*1024*1024)) {
        publishPreviewPart(lock,false);newColorBytes=colorTexture?colorTexture->bytes():0;
    }
    if ((!liveStreaming() && pendingGuiDraws >= 512) || !reservePreviewCommand(lock) || pendingVertices + mesh.vertices.size() > 262144 ||
        pendingVideoBytes + videoBytes > 32 * 1024 * 1024 || pendingColorBytes + newColorBytes > 64 * 1024 * 1024) {
        ++pendingCounts.guiDrops;return reject(9);
    }
    pendingVideoBytes += videoBytes;
    pendingColorBytes += newColorBytes;
    if (video) ++videoMeshes;
    if (colorTexture) ++colorMeshes;
    ++pendingGuiDraws;pendingVertices += mesh.vertices.size();pending.push_back(std::move(mesh)); ++accepted;
    return true;
}
void previewObserveTriangles(uint8_t* base, uint32_t indices, uint32_t triangles) {
    submitTriangles(base, indices, triangles);
}
template<class Geometry>
static void previewObserveWorldInto(uint8_t* base,Geometry&& geometry,std::shared_ptr<WorldDraw> draw={}) {
    EngineCpuScope profile(EnginePhase::snapshot);
    if (!active) return;
    worldActive=true;
    try {
        if(!draw) {
            // Split the read-only guest capture from the queue work below: the
            // two together are this draw's share of the engine thread's frame.
            EngineCpuScope capture(EnginePhase::worldCapture);
            draw=captureWorldDraw(base,std::forward<Geometry>(geometry));
        }
        if(!draw)return;
        std::unique_lock lock(queueMutex,std::defer_lock);
        {EngineCpuScope sample(EnginePhase::queueLock);lock.lock();}
        ++pendingCounts.worldDraws;
        // Draws retain shared immutable buffers. Count each retained resource
        // once, not once per depth/light/fog/material pass. UI vertices are
        // copied per mesh and use a separate budget.
        size_t additionalVertices=0,additionalBytes=0;
        // The captured draw owns these resources through
        // publication. Accounting needs identities, not extra shared_ptr copies
        // that contend with resource releases on the render thread.
        const std::array resources{draw->geometry.vertices.get(),draw->geometry.indices.get()};
        auto measureGeometry=[&] {
            additionalVertices=additionalBytes=0;
            for(size_t i=0;i<resources.size();++i) {
                const auto* resource=resources[i];
                if(i && resource==resources[0])continue;
                if(!pendingGeometry.contains(resource)) {
                    additionalVertices+=resource->vertexCount;additionalBytes+=resource->bytes();
                }
            }
        };
        measureGeometry();
        if(liveStreaming() && !pending.empty() && (pending.size()>=livePartCommands ||
           pendingWorldVertices+additionalVertices>4*1024*1024 ||
           pendingGeometryBytes+additionalBytes>worldGeometryByteLimit)) {
            publishPreviewPart(lock,false);measureGeometry();
        }
        if (!reservePreviewCommand(lock) || pendingWorldVertices+additionalVertices>4*1024*1024 ||
            pendingGeometryBytes+additionalBytes>worldGeometryByteLimit) {
            if(pending.size()>=4096)++pendingCounts.commandDrops;else ++pendingCounts.vertexDrops;
            const auto dropped=pending.size()>=4096?++worldCommandBudgetDrops:++worldVertexBudgetDrops;
            if(dropped<=8)std::fprintf(stderr,"[EngineWorldQueueDrop] frame=%llu program=%s commands=%zu uniqueVertices=%zu uniqueGeometry=%zu uniqueBytes=%zu additionalVertices=%zu\n",
                frames.load()+1,draw->fragmentName.c_str(),pending.size(),pendingWorldVertices,pendingGeometry.size(),pendingGeometryBytes,additionalVertices);
            return;
        }
        {
        EngineCpuScope sample(EnginePhase::queueTextures);
        for (uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const unsigned s=std::countr_zero(slots);
            if(!draw->textureObjects[s].object || !draw->textureIds[s])continue;
            const auto it=colorTextures.find(draw->textureIds[s]);
            if(it==colorTextures.end())continue;
            const auto& texture=draw->textureObjects[s];const auto& sampler=draw->samplers[s];
            if(it->second.resource && it->second.resource!=texture.object)
                worldTextureKeyClashes.fetch_add(1,std::memory_order_relaxed);
            if(it->second.resource && (it->second.resource!=texture.object ||
               it->second.image->width!=texture.width || it->second.image->height!=texture.height ||
               it->second.image->faces!=texture.faces))continue;
            const auto required=(std::max)(texture.firstMip,sampler.lodValid?unsigned(sampler.minLevel):0u);
            if(it->second.image->firstMip>required)continue;
            draw->textures[s]=it->second.image;it->second.used=++colorUse;
            worldTextureCacheHits.fetch_add(1,std::memory_order_relaxed);
        }
        for (uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const unsigned s=std::countr_zero(slots);
            if(draw->textures[s])continue;
            if(!draw->textureObjects[s].object)continue;
            if(auto it=textures.find(draw->textureIds[s]);draw->textureIds[s] && it!=textures.end()) {
                auto& cached=alphaColors[draw->textureIds[s]];
                if(cached) {draw->textures[s]=cached;continue;}
                auto color=std::make_shared<ColorImage>();color->width=it->second->width;color->height=it->second->height;
                color->sourceCodec=2;color->pixels.resize(it->second->pixels.size()*4);
                for(size_t i=0;i<it->second->pixels.size();++i) {
                    color->pixels[i*4]=color->pixels[i*4+1]=color->pixels[i*4+2]=255;color->pixels[i*4+3]=it->second->pixels[i];
                }
                cached=color;draw->textures[s]=std::move(color);
            }
            if(!draw->textures[s] && draw->textureObjects[s].object) {
                traceMissingWorldTexture(base,draw->textureIds[s],draw->textureObjects[s].object,draw->textureObjects[s].storage);
                if(draw->material==WorldMaterial::ndsp || draw->material==WorldMaterial::fixed ||
                   draw->fragmentName.starts_with("XRShader_") || draw->fragmentName=="XRUtil_RenderSurface" ||
                   draw->fragmentName=="VBOp_GenEnv2" || draw->fragmentName=="VBOp_Fresnel" ||
                   draw->fragmentName=="CMWnd_ModTexture_PaintVideo_YUV2RGB" ||
                   draw->fragmentName=="GUIRGB2Grey" ||
                   // Water normals and fog maps are authored CPU images.
                   // Planar reflection0 and scene refraction4 are GPU inputs;
                   // CubeWater's reflection0 is its authored environment cube.
                   ((draw->fragmentName=="VBOp_FP20_Water" || draw->fragmentName=="VBOp_FP20_Water2" ||
                     draw->fragmentName=="VBOp_FP20_CubeWater" || draw->fragmentName=="VBOp_FP20_CubeWater2") &&
                    (s==1 || s==2 || s==6 || s==7 || (s==0 &&
                     (draw->fragmentName=="VBOp_FP20_CubeWater" || draw->fragmentName=="VBOp_FP20_CubeWater2")))) ||
                   // Effect shaders mix authored diffuse/distortion/masks
                   // with framebuffer/depth resolves. Recover only the former.
                   (draw->fragmentName=="WModel_FXRenderSurface" && s==1) ||
                   ((draw->fragmentName=="WModel_FXHeatHazeMask" || draw->fragmentName=="WModel_FXBlackHole") &&
                    (s==1 || s==2)) ||
                   // Darkness voice masks/noise are authored CPU images.
                   // Its scene input at slot0 remains a GPU resolve.
                   (draw->fragmentName=="XREngine_RadialBlurInvert" && s>=1 && s<=3) ||
                   // CCFuser texture1 is its original 324x18 RGB lookup map.
                   // Reload the source pixels if its CPU cache entry expired.
                   (draw->fragmentName=="XREngine_CCFuser" && s==1) ||
                   // DV5's pulse and perturbation tables are CPU noise images.
                   // Scene/depth/atlas slots0/1/5 remain GPU resolve inputs.
                   (draw->fragmentName=="WClientMod_DV5_0" && (s==2 || s==4)) ||
                   // Other World's grain uses authored noise at slot1; its
                   // scene0/depth2 and final OW1_2 inputs stay GPU resolves.
                   (draw->fragmentName=="WClientMod_OW1_1" && s==1)) {
                    const uint32_t object=draw->textureObjects[s].object;
                    const unsigned decodeMip=draw->textureObjects[s].firstMip;
                    // GPU-resolved surfaces have no CPU pixels to decode, and a
                    // decode that just failed for this resource and mip will
                    // fail again this second. Skipping either costs nanoseconds;
                    // attempting it costs milliseconds.
                    if(isResolvedObject(object) || worldDecodeRecentlyFailed(object,decodeMip))continue;
                    auto decoded=std::make_shared<ColorImage>();
                    // A world texture is decoded from the guest every time its id
                    // has no usable cache entry, and that decode walks the whole
                    // mip chain. Count, time and name it: at 300 draws per frame
                    // it is either a one-off per texture or the frame's budget.
                    const auto decodeStart=std::chrono::steady_clock::now();
                    const char* decodeError=decodeWorldTextureImage(base,object,*decoded,decodeMip);
                    worldTextureDecodeNanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()-decodeStart).count()),std::memory_order_relaxed);
                    worldTextureDecodeAttempts.fetch_add(1,std::memory_order_relaxed);
                    if(noteWindowDecode(object,draw->textureIds[s],decoded->width,decoded->height))
                        worldTextureDecodeRepeats.fetch_add(1,std::memory_order_relaxed);
                    if(!decodeError) {
                        // Original82256008 fills video storage directly with
                        // decoder V,U bytes, without the initial upload's
                        // temporary pair swap. Fetch10 endian/selector decoding
                        // already exposes the shader's logical U,U,U,V view.
                        if(auto old=colorTextures.find(draw->textureIds[s]);old!=colorTextures.end()) {
                            colorBytes-=old->second.image->bytes();colorTextures.erase(old);
                        }
                        if (makeColorCacheRoom(decoded->bytes())) {
                            colorBytes+=decoded->bytes();colorTextures[draw->textureIds[s]]={decoded,++colorUse,draw->textureObjects[s].object};
                            worldTextureDecodeInstalls.fetch_add(1,std::memory_order_relaxed);
                        } else {
                            // Decoded pixels this draw can use, but the cache
                            // cannot hold them: the next draw of the same
                            // resource decodes it again.
                            worldTextureDecodeBudgetRejects.fetch_add(1,std::memory_order_relaxed);
                        }
                        draw->textures[s]=decoded;++colorImages;
                        if (TextureUploadTraceEnabled())
                            std::fprintf(stderr,"[EngineStreamedTexture] owned id=%u size=%ux%u format=%u\n",draw->textureIds[s],decoded->width,decoded->height,draw->textureObjects[s].format);
                    } else {
                        recordWorldDecodeFailure(object,decodeMip);
                        worldTextureDecodeFailures.fetch_add(1,std::memory_order_relaxed);
                        if(worldTextureDecodeReasonsPrinted.fetch_add(1,std::memory_order_relaxed)<8)
                            std::fprintf(stderr,"[EngineWorldImageDecodeFail] frame=%llu object=%08X id=%u mip=%u reason=%s\n",
                                static_cast<unsigned long long>(frames.load(std::memory_order_relaxed)),object,draw->textureIds[s],decodeMip,decodeError);
                    }
                }
            }
        }
        }
        EngineCpuScope sampleAccounting(EnginePhase::queueAccounting);
        // Capture only distinct images used by this draw. Reuse this short
        // list if a budget boundary publishes the pending batch; all image
        // ownership stays in draw while the non-owning list is in use.
        std::array<const ColorImage*,16> images{};
        size_t imageCount=0;
        for(uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const auto* image=draw->textures[std::countr_zero(slots)].get();
            if(image && std::find(images.begin(),images.begin()+imageCount,image)==images.begin()+imageCount)
                images[imageCount++]=image;
        }
        size_t additionalImageBytes=0;
        auto measureImages=[&] {
            additionalImageBytes=0;
            for(size_t i=0;i<imageCount;++i)
                if(!pendingWorldImages.contains(images[i]))additionalImageBytes+=images[i]->bytes();
        };
        measureImages();
        if(liveStreaming() && !pending.empty() && pendingWorldImageBytes+additionalImageBytes>worldImageByteLimit) {
            publishPreviewPart(lock,false);measureGeometry();measureImages();
        }
        if(pendingWorldVertices+additionalVertices>4*1024*1024 ||
           pendingGeometryBytes+additionalBytes>worldGeometryByteLimit) {
            ++pendingCounts.vertexDrops;++worldVertexBudgetDrops;return;
        }
        if(pendingWorldImageBytes+additionalImageBytes>worldImageByteLimit) {
            ++pendingCounts.imageDrops;++worldImageBudgetDrops;return;
        }
        for(const auto* resource:resources)if(pendingGeometry.insert(resource))pendingGeometryBytes+=resource->bytes();
        pendingWorldVertices+=additionalVertices;
        for(size_t i=0;i<imageCount;++i)pendingWorldImages.insert(images[i]);
        pendingWorldImageBytes+=additionalImageBytes;
        SimpleMesh command;command.promptContext=draw->promptContext;
        command.world=QueuedWorldDrawAccess::seal(std::move(draw));pending.push_back(std::move(command));
        ++worldQueued;
    } catch (...) {}
}
void previewObserveWorld(uint8_t* base,const StoredDraw& geometry) {
    previewObserveWorldInto(base,geometry);
}
void previewObserveWorld(uint8_t* base,StoredDraw&& geometry) {
    previewObserveWorldInto(base,std::move(geometry));
}
void previewObserveWorldUnbound(uint8_t* base,StoredDraw&& geometry) {
    if(!active || !geometry)return;
    std::shared_ptr<WorldDraw> draw;
    {
        EngineCpuScope sample(EnginePhase::worldCapture);
        const bool native=nativeWorldCaptureEnabled.load(std::memory_order_relaxed) &&
            nativeWorldCaptureSafe.load(std::memory_order_relaxed);
        if(native && nativeWorldCaptureCompare.load(std::memory_order_relaxed)) {
            auto referenceGeometry=geometry;
            std::shared_ptr<WorldDraw> reference;
            if(captureEngineVertexBindings(base,referenceGeometry.vertexBindings))
                reference=captureWorldDrawCanonical(base,referenceGeometry);
            draw=captureNativeWorldDraw(base,geometry);
            if(draw && (!reference || !equivalentWorldCaptures(*draw,*reference))) {
                rejectNativeWorldCaptureComparison();
                std::fprintf(stderr,"[NativeWorldCapture] comparison mismatch; original path retained for this session\n");
                draw=std::move(reference);
            } else if(!draw)draw=std::move(reference);
            if(!draw)return;
        } else {
            if(native)draw=captureNativeWorldDraw(base,geometry);
            if(!draw) {
                if(!captureEngineVertexBindings(base,geometry.vertexBindings))return;
                draw=captureWorldDraw(base,std::move(geometry));
            }
        }
    }
    if(draw)previewObserveWorldInto(base,std::move(geometry),std::move(draw));
}
void previewObserveImmediateWorld(uint8_t* base,uint32_t device,uint32_t indices,uint32_t count) {
    if (!active || !worldActive || !device || uint64_t(device)+12456>0x100000000ull) return;
    // Task-59 bounded pre-capture failure diagnostics. diagnose is read once;
    // when false the original path below runs with no extra guest reads,
    // logging, hashing or allocations. Failure collection never stops after
    // successes; only the first 2 smoke successes log a reference line.
    const bool diagnose=profileEngineCpu;
    uint32_t smokeTex0=0;
    if(diagnose) {
        uint8_t idBytes[2]{};
        // File-scope context below (same 0x82A69B00 as the local below).
        if(copyRenderMemory(base,context+16896+8,idBytes,sizeof(idBytes)))smokeTex0=u16(idBytes);
    }
    const bool smoke=smokeTex0==1477 || smokeTex0==1371;
    uint32_t beFlags=0,id=0,boundVal=0,descriptor=0;
    bool haveAttr=false;
    auto fail=[&](unsigned category,const ImmediateCaptureReason* reason=nullptr) {
        if(diagnose)reportImmediateCapture(category,smokeTex0,device,indices,count,
            beFlags,id,boundVal,descriptor,reason,haveAttr);
    };
    try {
        constexpr uint32_t context=0x82A69B00;
        std::array<uint8_t,160> attributes{};std::array<uint8_t,24> state{};
        if (!copyRenderMemory(base,context+16896,attributes.data(),attributes.size())) {fail(0);return;}
        haveAttr=true;beFlags=u32(attributes.data()+92);
        if(!copyRenderMemory(base,context+16512,state.data(),state.size())) {fail(1);return;}
        descriptor=u32(state.data());id=u32(state.data()+20);
        std::shared_ptr<const StoredGeometry> stored;
        if (!descriptor && id && id!=0xFFFFFFFF) {
            std::array<uint8_t,4> bound{};
            if (!copyRenderMemory(base,device+12452,bound.data(),4)) {fail(2);return;}
            boundVal=u32(bound.data());
            if(!(stored=storedGeometryCache().vertexStream(base,id,boundVal))) {fail(2);return;}
        } else if (!descriptor) descriptor=context+12480;
        StoredDraw geometry;
        ImmediateCaptureReason reason;
        if (!snapshotImmediateWorldGeometry(base,descriptor,indices,count,std::move(stored),geometry,
            diagnose?&reason:nullptr)) {fail(3,diagnose?&reason:nullptr);return;}
        if(!captureEngineVertexBindings(base,geometry.vertexBindings)) {fail(4);return;}
        if(geometry.vertexBindings->deviceAddress!=device) {fail(5);return;}
        if(diagnose && smoke && immediateSmokeSuccessLogged.fetch_add(1,std::memory_order_relaxed)<2)
            std::fprintf(stderr,"[ImmediateCaptureOk] frame=%llu ms=%llu tex0=%u device=%08X indices=%08X count=%u "
                "descriptor=%08X boundId=%u vcount=%u indexCount=%u beFlags=%08X\n",
                frames.load(std::memory_order_relaxed),GetTickCount64(),smokeTex0,
                device,indices,count,descriptor,id,
                geometry.vertices?geometry.vertices->vertexCount:0u,geometry.indexCount,beFlags);
        previewObserveWorld(base,std::move(geometry));
    } catch (...) { fail(6); }
}
void previewObserveClear(uint8_t* base,uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,const std::array<int32_t,4>& rectangle) {
    if (!active) return;
    try {
        auto clear=std::make_shared<WorldClear>();
        if (!snapshotWorldClear(base,device,flags,color,depth,stencil,*clear)) return;
        if(rectangle[2]<rectangle[0] || rectangle[3]<rectangle[1])return;
        clear->rectangle=rectangle;
        std::unique_lock lock(queueMutex);
        if (!reservePreviewCommand(lock)) {++pendingCounts.clearDrops;return;}
        SimpleMesh command;command.worldClear=std::move(clear);pending.push_back(std::move(command));
    } catch (...) {}
}
void previewObserveDecodedTriangles(uint8_t* base, uint32_t indices, uint32_t capacity, uint32_t produced) {
    if (!active) return;
    ++decodedBatches;
    // Original 8225DE78 allocates at most 8192 BE16 indices per chunk and
    // passes the produced count directly to triangle-list DrawIndexed.
    if (!capacity || capacity > 8192 || produced > capacity || produced % 3) { ++decodedInvalid; return; }
    if (!produced) return;
    decodedIndices += produced;
    if (submitTriangles(base, indices, produced / 3)) ++decodedAccepted;
}
bool previewWorldActive() {return worldActive;}
void previewObserveResolve(uint8_t* base,uint32_t device,uint32_t flags,uint32_t rectangle,uint32_t destination,
                           uint32_t offset,uint32_t color,float depth,uint32_t stencil,uint32_t face,uint32_t mip) {
    if(!active || !worldActive) return;
    try {
        auto resolve=std::make_shared<WorldResolve>();
        if(!snapshotWorldResolve(base,device,flags,rectangle,destination,offset,color,depth,stencil,face,mip,*resolve)) return;
        recordResolvedObject(destination);
        std::unique_lock lock(queueMutex);if(!reservePreviewCommand(lock)) {++pendingCounts.resolveDrops;return;}
        SimpleMesh command;command.worldResolve=std::move(resolve);pending.push_back(std::move(command));
    }catch(...){}
}
void previewObservePresent(uint8_t* base,uint32_t frontbuffer) {
    if(!active || !worldActive) return;
    try {
        auto texture=std::make_shared<WorldTexture>();
        if(!snapshotWorldTexture(base,frontbuffer,*texture)) return;
        recordResolvedObject(frontbuffer);
        std::unique_lock lock(queueMutex);
        if(liveStreaming())reservePreviewCommand(lock);
        SimpleMesh command;command.worldPresent=std::move(texture);pending.push_back(std::move(command));
    }catch(...){}
}
void previewObserveDisplayGamma(uint8_t* base,uint32_t ramp,bool piecewise) {
    if(!active)return;
    try {
        std::array<uint8_t,1536> bytes{};
        if(!copyRenderMemory(base,ramp,bytes.data(),bytes.size()))return;
        DisplayGamma gamma;gamma.piecewise=piecewise;
        for(unsigned channel=0;channel<3;++channel) {
            for(unsigned entry=0;entry<(piecewise?128u:256u);++entry) {
                const auto* value=bytes.data()+channel*512+entry*(piecewise?4:2);
                gamma.entries[entry*3+channel]={uint32_t(u16(value)),piecewise?uint32_t(u16(value+2)):0u};
                if(!piecewise)gamma.entries[entry*3+channel][0]&=0xFFC0;
            }
        }
        std::unique_lock lock(queueMutex);
        static std::shared_ptr<const DisplayGamma> previous;
        if(previous && *previous==gamma)return;
        if(liveStreaming())reservePreviewCommand(lock);
        auto table=std::make_shared<DisplayGamma>(std::move(gamma));
        SimpleMesh command;command.displayGamma=table;pending.push_back(std::move(command));
        previous=std::move(table);
    }catch(...){}
}
void setPreviewFrameBackpressure(bool enabled,bool streaming,unsigned bufferedParts) {
    {std::lock_guard lock(queueMutex);frameBackpressure=enabled;streamLargeFrames=enabled && streaming;
     readyPartLimit=streamLargeFrames?std::clamp(bufferedParts,1u,8u):1u;}
    frameConsumed.notify_all();
}
void previewBeginHistogram(unsigned bin) {
    if(!active || !worldActive || bin>=histogramResults.size() || activeHistogram || activeFlare)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    auto& result=histogramResults[bin];if(!result)result=std::make_shared<WorldQueryResult>();
    activeHistogram=std::make_shared<WorldQuery>();activeHistogram->result=result;
    SimpleMesh command;command.worldQuery=activeHistogram;command.worldQueryBegin=true;
    pending.push_back(std::move(command));
}
void previewEndHistogram() {
    if(!activeHistogram)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    SimpleMesh command;command.worldQuery=std::move(activeHistogram);pending.push_back(std::move(command));
}
bool previewReadHistogram(unsigned bin,uint64_t& samples) {
    std::lock_guard lock(queueMutex);
    if(bin>=histogramResults.size() || !histogramResults[bin])return false;
    const auto value=histogramResults[bin]->samples.load(std::memory_order_acquire);
    if(value==UINT64_MAX)return false;
    samples=value;return true;
}
void previewBeginFlare(unsigned id) {
    if(!active || !worldActive || id>flareQueryMaximumId || activeHistogram || activeFlare)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    // The u16 key bounds this lazy map to at most 65536 result owners. Keep
    // the previous completion while a newer GPU query is still pending.
    auto& result=flareResults[id];if(!result)result=std::make_shared<WorldQueryResult>();
    activeFlare=std::make_shared<WorldQuery>();activeFlare->result=result;
    SimpleMesh command;command.worldQuery=activeFlare;command.worldQueryBegin=true;
    pending.push_back(std::move(command));
}
void previewEndFlare() {
    if(!activeFlare)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    SimpleMesh command;command.worldQuery=std::move(activeFlare);pending.push_back(std::move(command));
}
bool previewReadFlare(unsigned id,uint64_t& samples) {
    std::lock_guard lock(queueMutex);
    const auto found=flareResults.find(id);
    if(found==flareResults.end())return false;
    const auto value=found->second->samples.load(std::memory_order_acquire);
    if(value==UINT64_MAX)return false;
    samples=value;return true;
}
namespace {
void publishPreviewPart(std::unique_lock<std::mutex>& lock,bool last) {
    EngineCpuScope sample(EnginePhase::queuePublish);
    {
        const auto* ctx=currentContext;
        StallProfiler::Scope stallWait(StallProfiler::Section::Wait, "preview frame producer backpressure",
            ctx ? ctx->lastFunction : 0, ctx ? uint32_t(ctx->lr) : 0,
            reinterpret_cast<uint64_t>(&frameConsumed), "frameConsumed condition variable");
        EngineCpuScope sampleWait(EnginePhase::queueWait);
        frameConsumed.wait(lock,[]{return !frameBackpressure || (readyParts.size()<readyPartLimit &&
            (readyParts.empty() || !readyParts.back().part.last));});
    }
    pendingCounts.commands+=pending.size();++pendingCounts.parts;
    for(const auto& command:pending)if(command.world)++pendingCounts.passes[size_t(command.world->material)];
    if(!last)++streamedParts;
    if(last) {
    queueHighWater.store((std::max)(queueHighWater.load(std::memory_order_relaxed),pendingCounts.commands),std::memory_order_relaxed);
    if(pendingCounts.commands>4096 || pendingCounts.commandDrops || pendingCounts.vertexDrops ||
       pendingCounts.clearDrops || pendingCounts.resolveDrops || pendingCounts.guiDrops || pendingCounts.imageDrops) {
        ++oversizedFrames;
        if(queueFrameReports<32) {
            ++queueFrameReports;
            const auto& passes=pendingCounts.passes;
            std::fprintf(stderr,"[EngineQueueFrame] frame=%llu commands=%zu worldAttempts=%zu commandDrops=%zu vertexDrops=%zu clearDrops=%zu resolveDrops=%zu guiDrops=%zu lastPartVertices=%zu lastPartGeometry=%zu lastPartBytes=%zu depth=%zu motion=%zu ndsp=%zu fixed=%zu post=%zu parts=%zu imageDrops=%zu\n",
                frames.load()+1,pendingCounts.commands,pendingCounts.worldDraws,pendingCounts.commandDrops,pendingCounts.vertexDrops,
                pendingCounts.clearDrops,pendingCounts.resolveDrops,pendingCounts.guiDrops,pendingWorldVertices,
                pendingGeometry.size(),pendingGeometryBytes,passes[0],passes[1],passes[2],passes[3],passes[4],pendingCounts.parts,pendingCounts.imageDrops);
        }
    }
    pendingCounts={};pendingGuiDraws=0;++frames;
    }
    std::vector<ReadyPreviewPart> retired;
    if(!frameBackpressure) {
        // Latest-frame mode retains its replacement behavior. The live path
        // allocates no temporary retirement container on each publication.
        retired.reserve(readyParts.size());
        while(!readyParts.empty()) {retired.push_back(std::move(readyParts.front()));readyParts.pop_front();}
    }
    readyParts.push_back({std::move(pending),{pendingFirst,last}});pendingFirst=last;
    pending.clear();
    // The consumer returns only empty storage. Reuse capacity without moving
    // thousands of commands through the vector's growth stages every frame.
    pending.swap(recycledFrame);
    if(pending.capacity())frameBufferReuses.fetch_add(1,std::memory_order_relaxed);
    pendingVertices = pendingVideoBytes = pendingColorBytes = 0;
    pendingGeometry.clear();pendingGeometryBytes=pendingWorldVertices=0;
    pendingWorldImages.clear();pendingWorldImageBytes=0;
    lock.unlock();frameAvailable.notify_one();
    retired.clear(); // Retained resources must never be destroyed under queueMutex.
    lock.lock();
}
}
void previewEndFrame() {
    StallProfiler::Scope stallRender(StallProfiler::Section::Rendering, "previewEndFrame");
    EngineCpuScope profile(EnginePhase::handoff);
    recordEngineFrameProcessor();
    if (!active) return;
    std::unique_lock lock(queueMutex,std::defer_lock);
    {
        StallProfiler::Scope stallWait(StallProfiler::Section::Wait, "previewEndFrame queue mutex",
            0,0,reinterpret_cast<uint64_t>(&queueMutex),"preview queue mutex");
        lock.lock();
    }
    publishPreviewPart(lock,true);
}
// Background retirement: the consumer hands each finished part to a worker
// that destroys its draw snapshots, instead of spending its own time on that
// before it can take the next part (20% of the PS5 render thread in a busy
// scene). At most kRetireBacklog parts wait; past that the consumer retires
// inline, so a slow worker bounds memory rather than growing it.
namespace {
constexpr size_t kRetireBacklog=4;
std::mutex retireMutex;
std::condition_variable retireReady;
std::deque<std::vector<SimpleMesh>> retireQueue;
std::thread retireWorker;
bool retireStopping=false;
std::atomic<bool> backgroundRetire{false};
void recyclePreviewFrame(std::vector<SimpleMesh>& completed) {
    // Never destroy retained resources under queueMutex. Publish only the
    // cleared allocation, retaining at most one spare command buffer.
    std::lock_guard recycleLock(queueMutex);
    if(completed.capacity()>recycledFrame.capacity())completed.swap(recycledFrame);
}
void retireLoop() {
    std::unique_lock lock(retireMutex);
    for(;;) {
        retireReady.wait(lock,[]{return retireStopping || !retireQueue.empty();});
        if(retireQueue.empty())return;
        auto completed=std::move(retireQueue.front());retireQueue.pop_front();
        lock.unlock();
        completed.clear();
        recyclePreviewFrame(completed);
        lock.lock();
    }
}
}
void setPreviewBackgroundRetirement(bool enabled) {
    if(enabled==backgroundRetire.load())return;
    if(enabled) {
        {std::lock_guard lock(retireMutex);retireStopping=false;}
        retireWorker=std::thread(retireLoop);
        backgroundRetire.store(true);
        return;
    }
    backgroundRetire.store(false);
    {std::lock_guard lock(retireMutex);retireStopping=true;}
    retireReady.notify_all();
    if(retireWorker.joinable())retireWorker.join(); // drains the backlog first
}
bool takePreviewFrame(std::vector<SimpleMesh>& frame,unsigned waitMilliseconds,PreviewFramePart* part) {
    setRenderSamplePhase(RenderSamplePhase::wait);
    StallProfiler::Scope stallRender(StallProfiler::Section::Rendering, "takePreviewFrame");
    std::unique_lock lock(queueMutex,std::defer_lock);
    {
        StallProfiler::Scope stallWait(StallProfiler::Section::Wait, "takePreviewFrame queue mutex",
            0,0,reinterpret_cast<uint64_t>(&queueMutex),"preview queue mutex");
        lock.lock();
    }
    if(waitMilliseconds && readyParts.empty()) {
        StallProfiler::Scope stallWait(StallProfiler::Section::Wait, "preview frame consumer wait",
            0,0,reinterpret_cast<uint64_t>(&frameAvailable),"frameAvailable condition variable");
        frameAvailable.wait_for(lock,std::chrono::milliseconds(waitMilliseconds),[]{return !readyParts.empty();});
    }
    if (readyParts.empty()) return false;
    if(part)*part=readyParts.front().part;
    auto completed=std::move(readyParts.front().commands);readyParts.pop_front();
    lock.unlock();frameConsumed.notify_one();
    setRenderSamplePhase(RenderSamplePhase::retire);
    // Retiring thousands of draw snapshots must not hold the producer's
    // queue lock. Their immutable shared resources outlive either frame.
    frame.swap(completed);
    if(backgroundRetire.load(std::memory_order_relaxed)) {
        std::unique_lock retireLock(retireMutex);
        if(retireQueue.size()<kRetireBacklog) {
            retireQueue.push_back(std::move(completed));
            retireLock.unlock();retireReady.notify_one();
            setRenderSamplePhase(RenderSamplePhase::other);
            return true;
        }
    }
    completed.clear();
    recyclePreviewFrame(completed);
    setRenderSamplePhase(RenderSamplePhase::other);
    return true;
}
// The world-image decode window: what was attempted, what failed, and, when
// attempts repeat, which resources they repeat on. Every value resets on read.
static void printWorldImageDecodeCounters() {
    const uint64_t attempts=worldTextureDecodeAttempts.exchange(0,std::memory_order_relaxed);
    const uint64_t failures=worldTextureDecodeFailures.exchange(0,std::memory_order_relaxed);
    const uint64_t nanoseconds=worldTextureDecodeNanoseconds.exchange(0,std::memory_order_relaxed);
    const uint64_t hits=worldTextureCacheHits.exchange(0,std::memory_order_relaxed);
    const uint64_t installs=worldTextureDecodeInstalls.exchange(0,std::memory_order_relaxed);
    const uint64_t repeats=worldTextureDecodeRepeats.exchange(0,std::memory_order_relaxed);
    const uint64_t rejects=worldTextureDecodeBudgetRejects.exchange(0,std::memory_order_relaxed);
    const uint64_t clashes=worldTextureKeyClashes.exchange(0,std::memory_order_relaxed);
    worldTextureDecodeReasonsPrinted.store(0,std::memory_order_relaxed);
    std::fprintf(stderr,"[EngineWorldImageDecode] attempts=%llu failures=%llu ms=%.2f cacheHits=%llu installs=%llu repeats=%llu budgetRejects=%llu keyClashes=%llu\n",
        attempts,failures,double(nanoseconds)/1e6,hits,installs,repeats,rejects,clashes);
    std::array<WindowDecodeTally,4> top{};
    size_t topCount=0;
    {
        std::lock_guard lock(windowDecodeMutex);
        std::array<WindowDecodeTally,64> sorted{};
        const size_t count=(std::min)(windowDecodeCount,windowDecodes.size());
        std::copy_n(windowDecodes.begin(),count,sorted.begin());
        for(size_t i=0;i<count;++i) {
            size_t best=i;
            for(size_t j=i+1;j<count;++j) if(sorted[j].count>sorted[best].count) best=j;
            std::swap(sorted[i],sorted[best]);
        }
        topCount=(std::min)(count,top.size());
        std::copy_n(sorted.begin(),topCount,top.begin());
        windowDecodeCount=0;
        windowDecodes={};
    }
    if(!topCount)return;
    char line[420]{},entry[96]{};
    size_t used=std::snprintf(line,sizeof line,"[EngineWorldImageDecodeTop]");
    for(size_t i=0;i<topCount && used<sizeof line;++i) {
        std::snprintf(entry,sizeof entry," %08X id=%u %ux%u x%u",top[i].object,top[i].id,top[i].width,top[i].height,top[i].count);
        used+=std::snprintf(line+used,sizeof line-used,"%s",entry);
    }
    std::fprintf(stderr,"%s\n",line);
}
// Two engine-thread readings a frame-rate investigation takes mid-run: how
// often a skinned palette was reused, and where world image decoding went.
// Both reset on read like the window counters they belong beside.
void printWorldCaptureCounters() {
    printWorldPaletteProofCounters();
    printWorldImageDecodeCounters();
}
void printPreviewCounters() {
    if (!active) return;
    printPromptOriginCounters();
    printStoredGeometryCounters();
    printWorldCounters();
    std::fprintf(stderr,"[EngineWorldQueue] queued=%llu vertexBudgetDrops=%llu commandBudgetDrops=%llu\n",
        worldQueued.load(),worldVertexBudgetDrops.load(),worldCommandBudgetDrops.load());
    std::fprintf(stderr,"[EngineQueueFrames] highWater=%zu oversizedFrames=%zu\n",
        queueHighWater.load(std::memory_order_relaxed),oversizedFrames.load(std::memory_order_relaxed));
    std::fprintf(stderr,"[EngineQueueStreaming] intermediateParts=%llu imageBudgetDrops=%llu partCommandLimit=%zu geometryMiB=%zu imageMiB=%zu\n",
        streamedParts.load(),worldImageBudgetDrops.load(),livePartCommands,worldGeometryByteLimit/(1024*1024),worldImageByteLimit/(1024*1024));
    std::fprintf(stderr,"[EngineFrameBuffers] reused=%llu\n",frameBufferReuses.load(std::memory_order_relaxed));
    printWorldImageDecodeCounters();
    printEngineTextureCounters();
    printEngineVertexDescriptorCounters();
    printEnginePaletteCounters();
    printEngineVertexProgramCounters();
    std::fprintf(stderr, "[EnginePreview] observed=%llu accepted=%llu unsupported=%llu frames=%llu alphaImages=%llu rejectedImages=%llu\n",
        observed.load(), accepted.load(), unsupported.load(), frames.load(), images.load(), rejectedImages.load());
    std::fprintf(stderr, "[EnginePreview] nativeRenderedFrames=%llu nativeSubmittedMeshes=%llu\n",
                 renderedFrames.load(), renderedMeshes.load());
    std::fprintf(stderr, "[EnginePreview] videoImages=%llu acceptedVideoMeshes=%llu\n", videoImages.load(), videoMeshes.load());
    std::fprintf(stderr, "[EnginePreview] colorImages=%llu acceptedColorMeshes=%llu\n", colorImages.load(), colorMeshes.load());
    std::fprintf(stderr, "[EnginePreview] decodedBatches=%llu decodedIndices=%llu acceptedDecodedMeshes=%llu invalidDecodedBatches=%llu\n",
        decodedBatches.load(), decodedIndices.load(), decodedAccepted.load(), decodedInvalid.load());
    std::fprintf(stderr, "[EnginePreview] rejectedState=%llu rejectedStoredVB=%llu rejectedMaterial=%llu rejectedViewport=%llu rejectedTextureStages=%llu rejectedModel=%llu rejectedMissingTexture=%llu rejectedStreams=%llu rejectedProjection=%llu rejectedBudget=%llu\n",
        rejectionReasons[0].load(), rejectionReasons[1].load(), rejectionReasons[2].load(), rejectionReasons[3].load(),
        rejectionReasons[4].load(), rejectionReasons[5].load(), rejectionReasons[6].load(), rejectionReasons[7].load(),
        rejectionReasons[8].load(), rejectionReasons[9].load());
}
void recordPreviewRender(size_t meshCount) { ++renderedFrames; renderedMeshes += meshCount; }
}
