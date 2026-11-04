// The guest's audio render driver on the console.
//
// SceAudioOut pulls one 256-frame grain at a time. For each grain this driver
// first enters the guest's render callback (one submit of planar 6-channel
// float PCM), then converts the oldest submitted buffer to interleaved float
// for the downmix in audio_ps5.cpp. Keeping the guest's own submit shape means
// the engine's mixing code runs unchanged; only the device under it is new.
#if !defined(_WIN32)

#include "audio_ps5_driver.h"

#include "audio_driver.h"
#include "audio_pcm_convert.h"
#include "audio_ps5.h"
#include "ppc_context.h"
#include "ps5_thread.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace DarkRecomp::Native::Ps5AudioDriver {
namespace {

// XAudio2's HRESULTs, which the guest already understands.
constexpr uint32_t kBadHandle = 0xC000000Du;   // AUDERR_INVALID_HANDLE
constexpr uint32_t kBusyHresult = 0x800700AAu; // HRESULT_FROM_WIN32(ERROR_BUSY)

// The audio worker's guest thread. The PCR page holds the static TLS copy and
// the thread object; the stack and the argument cell are separate pages so a
// stack overflow cannot scribble over the thread that reads it.
constexpr uint32_t kThreadId = 0x500;
constexpr uint32_t kPcr = 0x6f000000;
constexpr uint32_t kArgumentCell = 0x6f002000;
constexpr uint32_t kStackLow = 0x6f100000;
constexpr uint32_t kStackSize = 0x00010000;

// Four grains of headroom: one being filled by the output thread, one being
// consumed, and two absorbing a late submit. More only adds latency.
constexpr uint32_t kQueueDepth = 4;
constexpr uint32_t kPlanarBytes = kAudioSubmitBytes;
// One health line every 10 s of played audio (48,000 / 256 = 187.5 grains/s).
constexpr uint64_t kHealthGrains = (uint64_t(kAudioRateHz) * 10) / kAudioFramesPerSubmit;

struct Driver {
    std::mutex mutex;
    uint8_t* base = nullptr;
    const uint8_t* tls = nullptr;
    size_t tlsSize = 0;

    bool active = false;
    uint32_t callback = 0;
    uint32_t token = 0;
    uint32_t generation = 0;
    uint32_t volumeChangeMask = 0;

    // Stable for the client's lifetime: the output thread runs guest code
    // against it, and unregister joins that thread before anything is cleared.
    PPCContext context{};

    uint8_t planar[kQueueDepth][kPlanarBytes]{};
    uint32_t head = 0;
    uint32_t count = 0;

    uint64_t grains = 0;
    uint64_t callbacks = 0;
    uint64_t starvedGrains = 0;
    uint64_t droppedSubmits = 0;
    bool reportedOutputFailure = false;
};

Driver& driver() {
    static Driver shared;
    return shared;
}

inline bool guestRange(uint32_t address, size_t bytes) {
    return address != 0 && uint64_t(address) + bytes <= 0x100000000ull;
}

inline uint32_t readBE32(const uint8_t* at) {
    uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return __builtin_bswap32(value);
}

inline void writeBE32(uint8_t* base, uint32_t at, uint32_t value) {
    value = __builtin_bswap32(value);
    std::memcpy(base + at, &value, sizeof(value));
}

// Called from the feed thread, outside the driver lock.
void logHealthLine() {
    Driver& d = driver();
    uint64_t callbacks = 0, starved = 0, dropped = 0;
    uint32_t queued = 0;
    {
        std::lock_guard lock(d.mutex);
        if (!d.active) return;
        callbacks = d.callbacks;
        starved = d.starvedGrains;
        dropped = d.droppedSubmits;
        queued = d.count;
    }
    std::fprintf(stderr, "[Audio] health: callbacks=%llu starved=%llu dropped=%llu queued=%u\n",
                 static_cast<unsigned long long>(callbacks),
                 static_cast<unsigned long long>(starved),
                 static_cast<unsigned long long>(dropped), unsigned(queued));
}

// The SceAudioOut feed: one grain per call, on the output thread.
uint32_t pull(float* out, uint32_t frames, void* /*user*/) {
    Driver& d = driver();
    const size_t bytes = size_t(frames) * kAudioChannels * sizeof(float);
    std::memset(out, 0, bytes);
    if (frames != kAudioFramesPerSubmit) return frames;

    // The guest callback submits through submitFrame, which takes the same
    // mutex: read what is needed, then call guest code with the lock released.
    // A full queue asks the guest for nothing (backpressure) but still plays a
    // grain, or playback would stop for good the first time the guest ran ahead.
    uint32_t callback = 0;
    {
        std::lock_guard lock(d.mutex);
        if (!d.active) return frames;
        if (d.count < kQueueDepth) callback = d.callback;
    }
    if (callback) {
        // `currentContext` stays the main guest thread's: it is a plain global
        // on the console, and a failure report uses the context it is handed.
        PPCContext& ctx = d.context;
        ctx.r3.u64 = kArgumentCell;
        PPCSafeIndirect(ctx, d.base, callback);
        ++d.callbacks;
    }

    bool healthDue = false;
    {
        std::lock_guard lock(d.mutex);
        if (!d.active) return frames;
        if (d.count) {
            audioConvertPlanarBEFloatSamples(d.planar[d.head], out);
            d.head = (d.head + 1) % kQueueDepth;
            --d.count;
        } else {
            ++d.starvedGrains;
        }
        ++d.grains;
        healthDue = (d.grains % kHealthGrains) == 0;
    }
    if (healthDue) logHealthLine();
    return frames;
}

} // namespace

void initialize(uint8_t* base, const uint8_t* tls, size_t tlsSize) {
    Driver& d = driver();
    std::lock_guard lock(d.mutex);
    d.base = base;
    d.tls = tls;
    d.tlsSize = tlsSize;
}

uint32_t registerClient(uint32_t descriptor, uint32_t driverOut) {
    Driver& d = driver();
    if (!d.base) return kBadHandle;
    if (!guestRange(descriptor, 8) || !guestRange(driverOut, 4)) return kBadHandle;
    const uint32_t callback = readBE32(d.base + descriptor);
    const uint32_t argument = readBE32(d.base + descriptor + 4);
    if (!callback) return kBadHandle;

    uint32_t token = 0;
    {
        std::lock_guard lock(d.mutex);
        if (d.active) return kBusyHresult;
        d.active = true;
        d.callback = callback;
        d.head = 0;
        d.count = 0;
        d.grains = 0;
        d.callbacks = 0;
        d.starvedGrains = 0;
        d.droppedSubmits = 0;
        d.volumeChangeMask = 0;
        d.generation = (d.generation + 1) & kAudioDriverTokenGenMask;
        if (d.generation == 0) d.generation = 1;
        d.token = kAudioDriverTokenBase | d.generation;
        token = d.token;
        writeBE32(d.base, kArgumentCell, argument);
        initGuestThread(d.base, d.context, d.tls, d.tlsSize, kThreadId, kStackLow, kStackSize, kPcr);
        // Published before the output thread can ask for a grain, so the
        // guest's first submit validates against the handle it will be told.
        writeBE32(d.base, driverOut, d.token);
    }

    if (!Ps5Audio::audioStart(&pull, nullptr)) {
        // The guest keeps its render client and keeps submitting; it just has
        // nowhere to play yet. Report once and leave playback silent.
        std::lock_guard lock(d.mutex);
        if (!d.reportedOutputFailure) {
            d.reportedOutputFailure = true;
            std::fprintf(stderr, "[Audio] no console output; the game will be silent\n");
        }
    }
    std::fprintf(stderr, "[Audio] render client registered (console) callback=0x%08X token=0x%08X\n",
                 callback, token);
    return 0;
}

uint32_t submitFrame(uint32_t token, uint32_t samples) {
    Driver& d = driver();
    std::lock_guard lock(d.mutex);
    if (!d.active || token != d.token) return kBadHandle;
    if (!guestRange(samples, kPlanarBytes)) return kBadHandle;
    if (d.count >= kQueueDepth) {
        // The guest is ahead of the device. Dropping the newest grain keeps
        // playback on the timeline instead of letting latency grow.
        ++d.droppedSubmits;
        return kBusyHresult;
    }
    std::memcpy(d.planar[(d.head + d.count) % kQueueDepth], d.base + samples, kPlanarBytes);
    ++d.count;
    return 0;
}

uint32_t unregisterClient(uint32_t token) {
    Driver& d = driver();
    // The guest cannot tear the client down from inside the callback that the
    // output thread is running: stopping would join the calling thread.
    if (Ps5Audio::onOutputThread()) return kBusyHresult;
    {
        std::lock_guard lock(d.mutex);
        if (!d.active || token != d.token) return kBadHandle;
        // Stop pulling before the callback context goes away. The output
        // thread may be mid-callback: audioStop joins it.
        d.active = false;
    }
    Ps5Audio::audioStop();
    std::lock_guard lock(d.mutex);
    d.callback = 0;
    d.count = 0;
    d.head = 0;
    std::fprintf(stderr, "[Audio] render client unregistered token=0x%08X "
                         "(callbacks=%llu starved=%llu dropped=%llu)\n",
                 token, static_cast<unsigned long long>(d.callbacks),
                 static_cast<unsigned long long>(d.starvedGrains),
                 static_cast<unsigned long long>(d.droppedSubmits));
    return 0;
}

uint32_t voiceCategoryVolumeChangeMask(uint32_t token, uint32_t outMask) {
    Driver& d = driver();
    std::lock_guard lock(d.mutex);
    if (!d.active || token != d.token) return kBadHandle;
    if (!guestRange(outMask, 4)) return kBadHandle;
    writeBE32(d.base, outMask, d.volumeChangeMask);
    return 0;
}

uint32_t pullGrain(float* interleaved6, uint32_t frames) {
    return pull(interleaved6, frames, nullptr);
}

void logHealth() { logHealthLine(); }

bool running() {
    std::lock_guard lock(driver().mutex);
    return driver().active;
}

void shutdown() {
    {
        std::lock_guard lock(driver().mutex);
        driver().active = false;
    }
    Ps5Audio::audioStop();
}

} // namespace DarkRecomp::Native::Ps5AudioDriver

#endif // !defined(_WIN32)
