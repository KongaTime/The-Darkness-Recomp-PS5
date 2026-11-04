// The guest's audio render driver on the console (audio_driver.h).
//
// The Windows driver (audio_driver.cpp) runs the guest's render callback on its
// own worker thread and queues each submit to an XAudio2 source voice. The
// console has no XAudio2: its output (audio_ps5.cpp) pulls one 256-frame grain
// at a time from a feed thread, and SceAudioOutOutput blocks until the port
// takes it. So here the feed thread is the worker: for each grain it enters
// the guest callback, which submits planar 6-channel float PCM through
// submitFrame, then plays the oldest submitted grain. The guest side is the
// Windows driver's: the argument cell and the worker's guest stack, PCR and
// TLS come from Memory, the worker publishes its own currentContext, and a
// shutdown cancels the worker's dispatcher waits.
#if !defined(_WIN32)

#include "audio_driver.h"
#include "audio_pcm_convert.h"
#include "audio_ps5.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace DarkRecomp::Native {
namespace {

constexpr uint32_t kWorkerStackSize = 0x10000u;
constexpr uint32_t kBusyHresult = 0x800700AAu; // HRESULT_FROM_WIN32(ERROR_BUSY)
constexpr uint32_t kBadHandle = 0xC000000Du;   // AUDERR_INVALID_HANDLE
constexpr uint32_t kNoMemory = 0xC0000017u;
// Four grains: one being filled, one being played and two absorbing a late
// submit. More only adds latency on the console's blocking output.
constexpr uint32_t kQueueDepth = 4;
// One health line every 10 s of played audio (48,000 / 256 = 187.5 grains/s).
constexpr uint64_t kHealthGrains = (uint64_t(kAudioRateHz) * 10) / kAudioFramesPerSubmit;

struct Client {
    uint32_t token = 0;
    Memory* owner = nullptr;
    uint32_t guestCallback = 0;
    uint32_t argCell = 0;
    uint32_t workerAlloc = 0;
    PPCContext workerCtx{};
    DispatcherCancellation cancellation;
    std::atomic<bool> active{false};
    std::atomic<bool> cancelled{false};
    uint8_t planar[kQueueDepth][kAudioSubmitBytes]{};
    uint32_t head = 0, count = 0;
    std::vector<float> lastSubmitted = std::vector<float>(kAudioChannels * kAudioFramesPerSubmit);
    bool hasLastSubmitted = false;
    uint32_t volumeChangeMask = 0;
    bool workerContextReady = false;
};

struct Driver {
    std::mutex mutex;
    std::shared_ptr<Client> active;
    uint32_t nextGen = 1;
    bool muted = false;
    bool outputOpen = false;
    std::atomic<int32_t> injectedStartupError{0};
    std::atomic<int32_t> injectedVoiceError{0};

    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> framesSubmitted{0};
    std::atomic<uint64_t> samplesQueued{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> deviceErrors{0};
    std::atomic<uint64_t> starvationPasses{0};
    std::atomic<uint64_t> starvationBytes{0};
    std::atomic<uint64_t> maxCallbackMicros{0};
    std::atomic<uint64_t> callbacksWithoutSubmit{0};
    std::atomic<uint64_t> grains{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<int32_t> lastHresult{0};
};

Driver& driver() {
    static Driver shared;
    return shared;
}

bool guestSpan(Memory* owner, uint32_t address, uint32_t bytes) {
    return owner && address && uint64_t(address) + bytes <= PPC_MEMORY_SIZE;
}

void logHealth(const std::shared_ptr<Client>& client) {
    Driver& d = driver();
    uint32_t queued = 0;
    {
        std::lock_guard lock(d.mutex);
        queued = client->count;
    }
    std::fprintf(stderr, "[AudioHealth] callbacks=%llu submitted=%llu starved=%llu dropped=%llu queued=%u\n",
                 static_cast<unsigned long long>(d.callbacks.load()),
                 static_cast<unsigned long long>(d.framesSubmitted.load()),
                 static_cast<unsigned long long>(d.starvationPasses.load()),
                 static_cast<unsigned long long>(d.dropped.load()), unsigned(queued));
}

// The output feed: one grain per call, on the feed thread, which is the
// guest's audio worker.
uint32_t pull(float* out, uint32_t frames, void* /*user*/) {
    Driver& d = driver();
    std::memset(out, 0, size_t(frames) * kAudioChannels * sizeof(float));
    if (frames != kAudioFramesPerSubmit) return frames;
    std::shared_ptr<Client> client;
    bool wantCallback = false;
    {
        std::lock_guard lock(d.mutex);
        client = d.active;
        if (!client || !client->active.load(std::memory_order_acquire)) return frames;
        wantCallback = client->count < kQueueDepth;
    }
    if (!client->workerContextReady) {
        currentContext = &client->workerCtx;
        client->workerCtx.fpscr.loadFromHost();
        setDispatcherCancellation(&client->cancellation);
        client->workerContextReady = true;
    }
    if (wantCallback && !client->cancelled.load(std::memory_order_acquire)) {
        const uint64_t submittedBefore = d.framesSubmitted.load(std::memory_order_relaxed);
        LARGE_INTEGER begin{}, end{};
        QueryPerformanceCounter(&begin);
        try {
            client->workerCtx.r3.u64 = client->argCell;
            PPCSafeIndirect(client->workerCtx, client->owner->base(), client->guestCallback);
            d.callbacks++;
        } catch (const DispatcherWaitCancelled&) {
            client->cancelled.store(true, std::memory_order_release);
        }
        QueryPerformanceCounter(&end);
        const uint64_t micros = uint64_t(end.QuadPart - begin.QuadPart) / 1000; // the counter is in ns
        if (micros > d.maxCallbackMicros.load()) d.maxCallbackMicros.store(micros);
        if (d.framesSubmitted.load(std::memory_order_relaxed) == submittedBefore) d.callbacksWithoutSubmit++;
    }
    bool healthDue = false;
    {
        std::lock_guard lock(d.mutex);
        if (client->count) {
            if (!d.muted) audioConvertPlanarBEFloatSamples(client->planar[client->head], out);
            client->head = (client->head + 1) % kQueueDepth;
            --client->count;
            d.completed++;
        } else {
            d.starvationPasses++;
            d.starvationBytes += kAudioSubmitBytes;
        }
        healthDue = (++d.grains % kHealthGrains) == 0;
    }
    if (healthDue) logHealth(client);
    return frames;
}

void releaseClient(const std::shared_ptr<Client>& client) {
    if (!client) return;
    if (client->workerAlloc) client->owner->release(client->workerAlloc);
    if (client->argCell) client->owner->release(client->argCell);
}

// Stops the output (joining the feed thread, the guest's audio worker) and
// releases the client. The caller has already marked it inactive.
void stopAndRelease(const std::shared_ptr<Client>& client) {
    if (!client) return;
    {
        std::lock_guard lock(driver().mutex);
        cancelDispatcherWaits(client->cancellation);
    }
    Ps5Audio::audioStop();
    {
        std::lock_guard lock(driver().mutex);
        driver().outputOpen = false;
    }
    releaseClient(client);
}

} // namespace

void audioConvertPlanarBEFloat(const uint8_t* planarBE, float* interleavedOut) {
    audioConvertPlanarBEFloatSamples(planarBE, interleavedOut);
}

AudioRenderDriver& AudioRenderDriver::instance() {
    static AudioRenderDriver shared;
    return shared;
}

bool AudioRenderDriver::setMuted(bool muted) {
    std::lock_guard lock(driver().mutex);
    driver().muted = muted;
    return true;
}

uint32_t AudioRenderDriver::registerClient(uint32_t guestCallback, uint32_t rawArgument, uint32_t driverOut) {
    Driver& d = driver();
    Memory* owner = memory;
    if (!guestCallback || !guestSpan(owner, driverOut, 4)) return kBadHandle;
    if (const int32_t injected = d.injectedStartupError.exchange(0)) {
        d.lastHresult = injected;
        return uint32_t(injected);
    }
    auto client = std::make_shared<Client>();
    {
        std::lock_guard lock(d.mutex);
        if (d.active) return kBusyHresult;
        if (d.nextGen > kAudioDriverTokenGenMask) return kNoMemory;
        client->owner = owner;
        client->guestCallback = guestCallback;
        client->argCell = owner->allocate(4);
        if (!client->argCell) return kNoMemory;
        owner->write32(client->argCell, rawArgument);
        client->workerAlloc = owner->allocate(kWorkerStackSize + 0x2000, 0x1000, 0x71000000, 0x7F000000);
        if (!client->workerAlloc) {
            releaseClient(client);
            return kNoMemory;
        }
        client->workerCtx.r13.u64 = client->workerAlloc;
        client->workerCtx.r1.u64 = client->workerAlloc + 0x2000 + kWorkerStackSize - 0x100;
        owner->initThreadStorage(client->workerAlloc, client->workerAlloc + 0x2000, kWorkerStackSize, 0);
        DWORD oldProtection = 0;
        VirtualProtect(owner->base() + client->workerAlloc + 0x1000, 0x1000, PAGE_NOACCESS, &oldProtection);
        client->token = kAudioDriverTokenBase | d.nextGen++;
        client->active.store(true, std::memory_order_release);
        d.active = client;
        // Published before the feed can ask for a grain, so the guest's first
        // submit validates against the handle it is told.
        owner->write32(driverOut, client->token);
    }
    if (Ps5Audio::audioStart(&pull, nullptr)) {
        std::lock_guard lock(d.mutex);
        d.outputOpen = true;
    } else {
        // The guest keeps its render client; with no output nothing pulls, so
        // it is never called back and the game runs silent.
        d.deviceErrors++;
        std::fputs("[Audio] no console output; the game will be silent\n", stderr);
    }
    std::fprintf(stderr, "[Audio] render client registered callback=0x%08X token=0x%08X driver=0x%08X\n",
                 guestCallback, client->token, driverOut);
    return 0;
}

uint32_t AudioRenderDriver::submitFrame(uint32_t token, uint32_t samplesGuest) {
    Driver& d = driver();
    std::lock_guard lock(d.mutex);
    Client* client = d.active.get();
    if (!client || !client->active.load(std::memory_order_acquire) || token != client->token) return kBadHandle;
    if (const int32_t injected = d.injectedVoiceError.load()) return uint32_t(injected);
    if (!guestSpan(client->owner, samplesGuest, kAudioSubmitBytes)) return kBadHandle;
    if (client->count >= kQueueDepth) {
        // The guest is ahead of the device: dropping the newest grain keeps
        // playback on the timeline instead of letting latency grow.
        d.dropped++;
        return kBusyHresult;
    }
    const uint8_t* source = client->owner->base() + samplesGuest;
    std::memcpy(client->planar[(client->head + client->count) % kQueueDepth], source, kAudioSubmitBytes);
    ++client->count;
    audioConvertPlanarBEFloatSamples(source, client->lastSubmitted.data());
    client->hasLastSubmitted = true;
    d.framesSubmitted++;
    d.samplesQueued += kAudioFramesPerSubmit;
    return 0;
}

uint32_t AudioRenderDriver::unregisterClient(uint32_t token) {
    Driver& d = driver();
    // The guest cannot tear the client down from the callback the feed thread
    // is running: stopping joins that thread.
    if (Ps5Audio::onOutputThread()) return kBusyHresult;
    std::shared_ptr<Client> gone;
    {
        std::lock_guard lock(d.mutex);
        if (!d.active || token != d.active->token) return kBadHandle;
        d.active->active.store(false, std::memory_order_release);
        gone = std::move(d.active);
    }
    stopAndRelease(gone);
    std::fprintf(stderr, "[Audio] render client unregistered token=0x%08X\n", gone->token);
    return 0;
}

void AudioRenderDriver::shutdown() { shutdownForMemory(nullptr); }

void AudioRenderDriver::shutdownForMemory(Memory* owner) {
    Driver& d = driver();
    std::shared_ptr<Client> gone;
    {
        std::lock_guard lock(d.mutex);
        if (d.active && (!owner || d.active->owner == owner)) {
            d.active->active.store(false, std::memory_order_release);
            gone = std::move(d.active);
        }
    }
    if (gone && Ps5Audio::onOutputThread()) {
        std::fputs("[Audio] fatal: owner shutdown from the audio worker\n", stderr);
        std::fflush(stderr);
        ExitProcess(3);
    }
    stopAndRelease(gone);
}

AudioDriverCounters AudioRenderDriver::counters() {
    Driver& d = driver();
    AudioDriverCounters out{};
    out.callbacks = d.callbacks.load();
    out.framesSubmitted = d.framesSubmitted.load();
    out.samplesQueued = d.samplesQueued.load();
    out.buffersCompleted = d.completed.load();
    out.deviceErrors = d.deviceErrors.load();
    out.lastHresult = d.lastHresult.load();
    out.starvationPasses = d.starvationPasses.load();
    out.starvationBytes = d.starvationBytes.load();
    out.maxCallbackMicros = d.maxCallbackMicros.load();
    out.callbacksWithoutSubmit = d.callbacksWithoutSubmit.load();
    std::lock_guard lock(d.mutex);
    out.workerRunning = d.active && d.active->active.load();
    out.deviceReady = d.outputOpen;
    out.playbackStarted = d.outputOpen;
    if (d.active) {
        out.queuedBuffers = d.active->count;
        out.queuedFrames = uint64_t(d.active->count) * kAudioFramesPerSubmit;
    }
    return out;
}

bool audioDriverPeekLastSubmitted(std::vector<float>& out) {
    std::lock_guard lock(driver().mutex);
    if (!driver().active || !driver().active->hasLastSubmitted) return false;
    out = driver().active->lastSubmitted;
    return true;
}

uint32_t audioDriverInjectVoiceError(int32_t hr) {
    driver().injectedVoiceError.store(hr);
    return 0;
}

void audioDriverInjectStartupError(int32_t hr) { driver().injectedStartupError.store(hr); }

uint32_t audioDriverVoiceCategoryVolumeChangeMask(uint32_t token, uint32_t outMask) {
    std::lock_guard lock(driver().mutex);
    Client* client = driver().active.get();
    if (!client || token != client->token || !guestSpan(client->owner, outMask, 4)) return kBadHandle;
    client->owner->write32(outMask, client->volumeChangeMask);
    return 0;
}

void audioDriverShutdownForMemory(Memory* owner) { AudioRenderDriver::instance().shutdownForMemory(owner); }

} // namespace DarkRecomp::Native

#endif // !defined(_WIN32)
