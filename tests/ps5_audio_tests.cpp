// The console's audio path, driven on the host.
//
// The guest never talks to a device: the engine registers a render callback and
// submits 256-frame planar 6-channel float grains. On Windows XAudio2's worker
// pulls those callbacks; on the console the SceAudioOut feed thread does, and
// the grain is downmixed to 16-bit stereo for the console's output port.
//
// This test compiles the same two translation units the PS5 title links
// (runtime/native/audio_ps5_driver.cpp and audio_ps5.cpp) and scripts the
// console's SceAudioOut API. It exercises the real guest-callback handoff
// through the AOT dispatch table, the real planar conversion and downmix, and
// the real feed thread:
//
//   * the guest's register call publishes a handle it can submit against,
//   * each pulled grain enters the guest exactly once, with the registered
//     argument in r3, and its planar submit becomes frame-major PCM,
//   * the 5.1 -> stereo downmix and 16-bit saturation produce the expected
//     samples, and a guest that submits nothing yields silence,
//   * stale handles are rejected and an ahead-of-device queue returns busy,
//   * an output that will not open leaves the guest registered but silent,
//   * with a working output the feed thread delivers audible grains.
#include "runtime/native/audio_ps5_driver.h"

#include "ppc_context.h"
#include "runtime/native/audio_driver.h"
#include "runtime/native/audio_ps5.h"
#include "runtime/native/guest_context.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

using namespace DarkRecomp::Native;

namespace {

constexpr size_t kGuestSize = 0x100000000ull;
// Guest addresses the test owns: the descriptor the engine registers, the
// handle cell it reads back, a volume-mask cell and a planar PCM buffer.
constexpr uint32_t kDescriptor = 0x1000;
constexpr uint32_t kArgument = 0x1004;
constexpr uint32_t kDriverOut = 0x1008;
constexpr uint32_t kMaskCell = 0x1010;
constexpr uint32_t kPlanar = 0x50000000;
constexpr uint32_t kCallback = 0x820D0000; // inside the AOT code region
constexpr uint32_t kArgumentValue = 0x12345678;
constexpr int32_t kWorkingPort = 3;
constexpr uint32_t kBusy = 0x800700AAu;
constexpr uint32_t kBadHandle = 0xC000000Du;
constexpr uint32_t kFrames = kAudioFramesPerSubmit;
constexpr uint32_t kChannels = kAudioChannels;

uint8_t* g_base = nullptr;
uint32_t g_token = 0;

void put32(uint8_t* base, uint32_t at, uint32_t value) {
    base[at] = uint8_t(value >> 24);
    base[at + 1] = uint8_t(value >> 16);
    base[at + 2] = uint8_t(value >> 8);
    base[at + 3] = uint8_t(value);
}

uint32_t get32(const uint8_t* base, uint32_t at) {
    return uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 | uint32_t(base[at + 2]) << 8 |
           uint32_t(base[at + 3]);
}

void putFloat(uint8_t* base, uint32_t at, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put32(base, at, bits);
}

// The engine's submit layout: channel-major big-endian floats.
void putPlanar(uint32_t channel, uint32_t frame, float value) {
    putFloat(g_base, kPlanar + (size_t(channel) * kFrames + frame) * 4, value);
}

void clearPlanar() { std::memset(g_base + kPlanar, 0, size_t(kFrames) * kChannels * 4); }

// The guest's render callback: it runs where the feed thread invokes it, with
// r3 pointing at the argument cell, exactly as the Windows worker calls it.
struct Guest {
    std::atomic<uint32_t> entries{0};
    bool submit = true;
    bool sawArgumentCell = false;
    uint32_t submitted = 0;
};

Guest g_guest;

void guestRenderCallback(PPCContext& ctx, uint8_t* base) {
    g_guest.entries.fetch_add(1);
    const uint32_t argument = get32(base, ctx.r3.u32);
    g_guest.sawArgumentCell = argument == kArgumentValue;
    if (!g_guest.submit) return;
    if (Ps5AudioDriver::submitFrame(get32(base, kDriverOut), kPlanar) == 0) ++g_guest.submitted;
}

void resetGuest() {
    g_guest.entries.store(0);
    g_guest.submit = true;
    g_guest.sawArgumentCell = false;
    g_guest.submitted = 0;
}

// The scripted console output. One output call per granted grain, so the test
// steps the feed thread instead of racing a free-running one.
struct ScriptedOutput {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::array<int16_t, Ps5Audio::kGrainFrames * Ps5Audio::kOutChannels>> grains;
    size_t consumed = 0;
    int granted = 0;
    bool unlimited = false;
    int32_t port = -1;
} g_output;

void allowGrain() {
    std::lock_guard lock(g_output.mutex);
    ++g_output.granted;
    g_output.cv.notify_all();
}

void allowAllGrains() {
    std::lock_guard lock(g_output.mutex);
    g_output.unlimited = true;
    g_output.cv.notify_all();
}

std::array<int16_t, Ps5Audio::kGrainFrames * Ps5Audio::kOutChannels> nextGrain() {
    const size_t index = g_output.consumed++;
    allowGrain();
    std::unique_lock lock(g_output.mutex);
    const bool ready = g_output.cv.wait_for(lock, std::chrono::seconds(5),
                                            [index] { return g_output.grains.size() > index; });
    if (!ready) throw std::runtime_error("the console's output never received a grain");
    return g_output.grains[index];
}

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

uint32_t registerClient() {
    put32(g_base, kDescriptor, kCallback);
    put32(g_base, kDescriptor + 4, kArgumentValue);
    put32(g_base, kDriverOut, 0);
    require(Ps5AudioDriver::registerClient(kDescriptor, kDriverOut) == 0,
            "the guest's render client was rejected");
    g_token = get32(g_base, kDriverOut);
    require((g_token & 0xFFF00000u) == kAudioDriverTokenBase,
            "the published handle is not a driver token");
    require(Ps5AudioDriver::running(), "the driver did not stay registered");
    return g_token;
}

void testRegisterPublishesAHandle() {
    resetGuest();
    const uint32_t token = registerClient();
    require(Ps5AudioDriver::registerClient(kDescriptor, kDriverOut) == kBusy,
            "a second render client was accepted while one was active");
    require(Ps5AudioDriver::unregisterClient(token ^ 1u) == kBadHandle,
            "an unregister with the wrong handle was accepted");

    put32(g_base, kDescriptor, 0);
    require(Ps5AudioDriver::registerClient(kDescriptor, kDriverOut) == kBadHandle,
            "a null render callback was accepted");
    put32(g_base, kDescriptor, kCallback);
    std::printf("AudioRegister: handle 0x%08X published; duplicate and stale clients rejected.\n", token);
}

void testGrainEntersTheGuestAndPlays() {
    resetGuest();
    clearPlanar();
    putPlanar(0, 0, 0.25f);  // FL
    putPlanar(1, 0, -0.5f);  // FR
    putPlanar(5, 0, 0.5f);   // SR
    float interleaved[kFrames * kChannels];
    require(Ps5AudioDriver::pullGrain(interleaved, kFrames) == kFrames, "the pull did not return a grain");

    require(g_guest.entries.load() == 1, "one pull did not enter the guest exactly once");
    require(g_guest.sawArgumentCell, "the render callback did not receive the registered argument in r3");
    require(g_guest.submitted == 1, "the guest's submit was not accepted");

    // Frame-major conversion: frame 0 is FL, FR, FC, LFE, SL, SR.
    require(interleaved[0] == 0.25f && interleaved[1] == -0.5f && interleaved[5] == 0.5f,
            "the planar submit was not converted to frame-major order");
    require(interleaved[2] == 0.0f && interleaved[3] == 0.0f && interleaved[4] == 0.0f,
            "channels the guest never wrote were not preserved as zero");

    // Left = FL + FC*0.7071 + SL*0.5; right = FR + FC*0.7071 + SR*0.5.
    int16_t grain[kFrames * Ps5Audio::kOutChannels];
    Ps5Audio::packStereo(interleaved, grain, kFrames);
    const int expectedLeft = int(std::lrintf(0.25f * 32767.0f));
    const int expectedRight = int(std::lrintf((-0.5f + 0.5f * 0.5f) * 32767.0f));
    require(std::abs(grain[0] - expectedLeft) <= 1, "the left channel is not the planar submit's FL");
    require(std::abs(grain[1] - expectedRight) <= 1, "the right channel does not fold FR and SR");
    require(grain[2] == 0 && grain[3] == 0, "a frame beyond the guest's writes was not silent");

    // The centre channel folds into both sides at -3 dB.
    clearPlanar();
    putPlanar(2, 0, 0.5f);
    Ps5AudioDriver::pullGrain(interleaved, kFrames);
    Ps5Audio::packStereo(interleaved, grain, kFrames);
    const int expectedCentre = int(std::lrintf(0.5f * 0.7071f * 32767.0f));
    require(std::abs(grain[0] - expectedCentre) <= 1 && std::abs(grain[1] - expectedCentre) <= 1,
            "the centre channel does not fold into both sides");

    // Full scale on every channel saturates instead of wrapping.
    clearPlanar();
    for (uint32_t channel = 0; channel < kChannels; ++channel) putPlanar(channel, 0, 4.0f);
    Ps5AudioDriver::pullGrain(interleaved, kFrames);
    Ps5Audio::packStereo(interleaved, grain, kFrames);
    require(grain[0] == 32767 && grain[1] == 32767, "a full-scale mix did not saturate");
    std::printf("AudioGrain: callback handoff, planar conversion, 5.1 downmix and 16-bit saturation verified.\n");
}

void testSilenceAndStaleSubmits() {
    g_guest.submit = false;
    clearPlanar();
    float interleaved[kFrames * kChannels];
    Ps5AudioDriver::pullGrain(interleaved, kFrames);
    int16_t grain[kFrames * Ps5Audio::kOutChannels];
    Ps5Audio::packStereo(interleaved, grain, kFrames);
    require(std::all_of(grain, grain + kFrames * Ps5Audio::kOutChannels,
                        [](int16_t sample) { return sample == 0; }),
            "a guest that submits nothing did not produce silence");

    require(Ps5AudioDriver::submitFrame(g_token ^ 1u, kPlanar) == kBadHandle,
            "a submit with a stale handle was accepted");
    require(Ps5AudioDriver::submitFrame(g_token, 0) == kBadHandle,
            "a submit from a null guest buffer was accepted");
    require(Ps5AudioDriver::voiceCategoryVolumeChangeMask(g_token ^ 1u, kMaskCell) == kBadHandle,
            "a volume mask read with a stale handle was accepted");
    require(Ps5AudioDriver::voiceCategoryVolumeChangeMask(g_token, kMaskCell) == 0,
            "the volume mask read failed for the active client");
    require(get32(g_base, kMaskCell) == 0, "the volume change mask was not cleared");
    g_guest.submit = true;
    std::printf("AudioSilence: an idle guest is silent; stale handles are rejected.\n");
}

void testHealthLine() {
    // The line is captured rather than read off the console, so the counters the
    // checklist quotes are the ones the driver actually prints.
    const int saved = dup(fileno(stderr));
    require(saved >= 0, "cannot save stderr");
    std::FILE* capture = std::tmpfile();
    require(capture != nullptr, "cannot open the health line capture");
    require(dup2(fileno(capture), fileno(stderr)) >= 0, "cannot redirect stderr");
    Ps5AudioDriver::logHealth();
    std::fflush(stderr);
    require(dup2(saved, fileno(stderr)) >= 0, "cannot restore stderr");
    close(saved);
    std::rewind(capture);
    char line[256]{};
    const bool read = std::fgets(line, sizeof(line), capture) != nullptr;
    std::fclose(capture);
    require(read, "the health line printed nothing");
    require(std::strstr(line, "[Audio] health:") != nullptr, "the health line has no tag");
    require(std::strstr(line, "callbacks=") && std::strstr(line, "starved=") &&
                std::strstr(line, "dropped=") && std::strstr(line, "queued="),
            "the health line is missing a counter");
    std::printf("AudioHealth: %s", line);
}

void testPeriodicHealth() {
    const int saved = dup(fileno(stderr));
    require(saved >= 0, "cannot save stderr");
    std::FILE* capture = std::tmpfile();
    require(capture != nullptr, "cannot open the periodic health capture");
    require(dup2(fileno(capture), fileno(stderr)) >= 0, "cannot redirect stderr");

    // More than one 10-second interval (48,000 / 256 * 10 = 1,875 grains), so
    // the modulo trigger is exercised, not only logHealth().
    float interleaved[kFrames * kChannels];
    for (uint32_t i = 0; i < 2000; ++i) Ps5AudioDriver::pullGrain(interleaved, kFrames);

    std::fflush(stderr);
    require(dup2(saved, fileno(stderr)) >= 0, "cannot restore stderr");
    close(saved);
    std::rewind(capture);
    std::string text;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), capture)) text += buffer;
    std::fclose(capture);
    require(text.find("[Audio] health:") != std::string::npos,
            "the periodic health line never printed");
    std::printf("AudioHealthPeriodic: one line within 2,000 grains.\n");
}

void testQueueDepth() {
    clearPlanar();
    // No feed thread is running (the scripted output is closed), so the queue
    // is fed only by the guest.
    for (uint32_t i = 0; i < 4; ++i)
        require(Ps5AudioDriver::submitFrame(g_token, kPlanar) == 0, "the queue rejected a grain inside its depth");
    require(Ps5AudioDriver::submitFrame(g_token, kPlanar) == kBusy, "the queue accepted a grain past its depth");

    float interleaved[kFrames * kChannels];
    Ps5AudioDriver::pullGrain(interleaved, kFrames);
    require(Ps5AudioDriver::submitFrame(g_token, kPlanar) == 0, "the queue did not free a slot after a drain");
    require(Ps5AudioDriver::unregisterClient(g_token) == 0, "the active client could not be unregistered");
    require(!Ps5AudioDriver::running(), "the driver stayed registered after unregister");
    require(Ps5AudioDriver::submitFrame(g_token, kPlanar) == kBadHandle, "a submit after unregister was accepted");
    std::printf("AudioQueue: depth bounds hold; an unavailable output leaves the client registered and silent.\n");
}

void testFeedThreadPlays() {
    std::lock_guard lock(g_output.mutex);
    g_output.port = kWorkingPort;
    g_output.granted = 0;
    g_output.unlimited = false;
}

void testWorkingOutputDeliversGrains() {
    resetGuest();
    clearPlanar();
    for (uint32_t channel = 0; channel < kChannels; ++channel)
        for (uint32_t frame = 0; frame < kFrames; ++frame) putPlanar(channel, frame, 0.125f);

    // The output opens, so audioStart starts the real feed thread.
    const uint32_t token = registerClient();
    require(Ps5Audio::audioRunning(), "the console output did not start");
    const auto grain = nextGrain();
    require(g_guest.entries.load() >= 1, "the feed thread did not enter the guest");
    require(std::any_of(grain.begin(), grain.end(), [](int16_t sample) { return sample != 0; }),
            "the feed thread delivered silence while the guest submitted audio");

    allowAllGrains();
    require(Ps5AudioDriver::unregisterClient(token) == 0, "the playing client could not be unregistered");
    require(!Ps5Audio::audioRunning(), "the console output did not stop");
    std::printf("AudioOutput: the feed thread opened SceAudioOut and delivered audible grains.\n");
}

} // namespace

// The console's SceAudioOut API, scripted. audio_ps5.cpp links against these.
extern "C" {

int sceAudioOutInit(void) { return 0; }

int sceAudioOutOpen(int32_t /*user*/, int32_t /*type*/, int32_t /*index*/, uint32_t /*grain*/,
                    uint32_t /*rate*/, uint32_t /*format*/) {
    std::lock_guard lock(g_output.mutex);
    return g_output.port;
}

int sceAudioOutOutput(int32_t /*port*/, const void* samples) {
    if (samples == nullptr) return 0; // the stop path's drain
    std::unique_lock lock(g_output.mutex);
    g_output.cv.wait_for(lock, std::chrono::seconds(5),
                         [] { return g_output.unlimited || g_output.granted > 0; });
    if (!g_output.unlimited) {
        if (g_output.granted <= 0) return 0;
        --g_output.granted;
    }
    if (g_output.grains.size() < 4096) {
        std::array<int16_t, Ps5Audio::kGrainFrames * Ps5Audio::kOutChannels> grain{};
        std::memcpy(grain.data(), samples, sizeof(int16_t) * grain.size());
        g_output.grains.push_back(grain);
    }
    g_output.cv.notify_all();
    return 0;
}

int sceAudioOutClose(int32_t /*port*/) { return 0; }

} // extern "C"

// The pieces app/ps5/guest_support.cpp provides on the console.
namespace DarkRecomp::Native {
PPCContext* currentContext = nullptr;
}

[[noreturn]] void PPCRecompFailure(const PPCContext&, uint32_t address, const char* reason) {
    std::fprintf(stderr, "guest failure at 0x%08X: %s\n", address, reason);
    std::abort();
}

int main() {
    try {
        void* mapped = mmap(nullptr, kGuestSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        require(mapped != MAP_FAILED, "cannot reserve the console's 4 GB guest mapping");
        g_base = static_cast<uint8_t*>(mapped);
        PPC_LOOKUP_FUNC(g_base, kCallback) = &guestRenderCallback;

        Ps5AudioDriver::initialize(g_base, nullptr, 0);
        testRegisterPublishesAHandle();
        testGrainEntersTheGuestAndPlays();
        testSilenceAndStaleSubmits();
        testHealthLine();
        testPeriodicHealth();
        testQueueDepth();
        testFeedThreadPlays();
        testWorkingOutputDeliversGrains();
        Ps5AudioDriver::shutdown();

        munmap(g_base, kGuestSize);
        std::puts("PS5 audio: the guest's render client drives the console's output.");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
