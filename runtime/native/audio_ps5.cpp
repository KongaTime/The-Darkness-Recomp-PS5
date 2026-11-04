// The console's audio output backend.
//
// The engine's render driver hands us 6-channel planar float PCM in 256-frame
// grains. SceAudioOut takes interleaved 16-bit stereo, so this backend runs a
// feed thread that pulls one grain per output call and downmixes it (FL, FR,
// FC, LFE, SL, SR -> L/R) with 16-bit saturation.
//
// SceAudioOutOutput blocks until the port takes the grain, which is what paces
// the feed: one guest callback per 5.3 ms of audio, never a spin.
#include "audio_ps5.h"

#if !defined(_WIN32)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pthread.h>

// The console's output API, as the Edge of Time title uses it on hardware.
// Declared here so the host test can link this file without the SDK.
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate,
                    uint32_t format);
int sceAudioOutOutput(int32_t port, const void* samples);
int sceAudioOutClose(int32_t port);
}

namespace DarkRecomp::Native::Ps5Audio {
namespace {

constexpr int32_t kNoPort = -1;
constexpr int32_t kAlreadyInitialised = int32_t(0x8026000e);
constexpr int32_t kSystemUser = 0xff;
constexpr uint32_t kFormatS16Stereo = 1;

std::atomic<int32_t> g_port{kNoPort};
std::atomic<bool> g_running{false};
pthread_t g_thread{};
std::atomic<bool> g_threadStarted{false};
FillFn g_fill = nullptr;
void* g_user = nullptr;

// One grain of interleaved stereo PCM, in the layout SceAudioOut expects.
struct Grain {
    int16_t samples[kGrainFrames][kOutChannels];
};

Grain g_out;

inline int16_t toS16(float value) {
    if (!std::isfinite(value)) return 0;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return int16_t(std::lrintf(value * 32767.0f));
}

// Xbox 5.1 order is FL, FR, FC, LFE, SL, SR. Fold the centre and LFE in at
// -3 dB and the surrounds at -6 dB, which keeps a full mix inside the stereo
// headroom instead of hard-clipping it.
void* feed(void*) {
    float frame[kGrainFrames * kEngineChannels];
    while (g_running.load(std::memory_order_acquire)) {
        const int32_t port = g_port.load(std::memory_order_acquire);
        if (port < 0) break;
        std::memset(frame, 0, sizeof(frame));
        if (g_fill) g_fill(frame, kGrainFrames, g_user);
        packStereo(frame, &g_out.samples[0][0], kGrainFrames);
        // Blocks for one grain: this is what paces the guest callbacks.
        if (sceAudioOutOutput(port, g_out.samples) < 0) {
            std::fprintf(stderr, "[Audio] SceAudioOut output failed\n");
            break;
        }
    }
    return nullptr;
}

} // namespace

void packStereo(const float* interleaved6, int16_t* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        const float* frame = interleaved6 + size_t(i) * kEngineChannels;
        const float left = frame[0] + frame[2] * 0.7071f + frame[4] * 0.5f;
        const float right = frame[1] + frame[2] * 0.7071f + frame[5] * 0.5f;
        out[size_t(i) * kOutChannels] = toS16(left);
        out[size_t(i) * kOutChannels + 1] = toS16(right);
    }
}

bool audioStart(FillFn fill, void* user) {
    if (g_running.load(std::memory_order_acquire)) return true;
    g_fill = fill;
    g_user = user;
    const int init = sceAudioOutInit();
    if (init < 0 && init != kAlreadyInitialised) {
        std::fprintf(stderr, "[Audio] SceAudioOut init failed (0x%08X)\n", unsigned(init));
        return false;
    }
    // The system user's main port: 256 frames, 48 kHz, 16-bit stereo.
    const int32_t port = sceAudioOutOpen(kSystemUser, 0, 0, kGrainFrames, kRate, kFormatS16Stereo);
    if (port < 0) {
        std::fprintf(stderr, "[Audio] SceAudioOut open failed (0x%08X)\n", unsigned(port));
        return false;
    }
    g_port.store(port, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    // The feed thread runs the guest's render callback (the engine's mixer),
    // so it gets a stack sized for guest code rather than the default.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 4u << 20);
    const int created = pthread_create(&g_thread, &attributes, feed, nullptr);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        g_running.store(false, std::memory_order_release);
        g_port.store(kNoPort, std::memory_order_release);
        sceAudioOutClose(port);
        std::fprintf(stderr, "[Audio] feed thread creation failed\n");
        return false;
    }
    g_threadStarted.store(true, std::memory_order_release);
    std::fprintf(stderr, "[Audio] SceAudioOut port %d: %u Hz, %u ch, %u-frame grains\n",
                 int(port), unsigned(kRate), unsigned(kOutChannels), unsigned(kGrainFrames));
    return true;
}

void audioStop() {
    const bool wasRunning = g_running.exchange(false, std::memory_order_acq_rel);
    const int32_t port = g_port.exchange(kNoPort, std::memory_order_acq_rel);
    if (g_threadStarted.load(std::memory_order_acquire) && !pthread_equal(g_thread, pthread_self())) {
        pthread_join(g_thread, nullptr);
        g_threadStarted.store(false, std::memory_order_release);
    }
    if (port >= 0) {
        // The port still holds the grain the feed thread queued; drain it so
        // the close does not cut a callback's audio short.
        sceAudioOutOutput(port, nullptr);
        sceAudioOutClose(port);
    }
    g_fill = nullptr;
    g_user = nullptr;
    if (wasRunning) std::fprintf(stderr, "[Audio] SceAudioOut stopped\n");
}

bool audioRunning() { return g_running.load(std::memory_order_acquire); }

bool onOutputThread() {
    return g_threadStarted.load(std::memory_order_acquire) && pthread_equal(g_thread, pthread_self()) != 0;
}

} // namespace DarkRecomp::Native::Ps5Audio

#endif // !defined(_WIN32)
