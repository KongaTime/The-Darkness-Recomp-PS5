#pragma once
// The guest's audio render driver on the console.
//
// On Windows the engine registers a render-driver client with XAudio2 and the
// mastering voice pulls guest callbacks on a worker thread. The console has no
// XAudio2: SceAudioOut pulls grains on this driver's output thread, and each
// grain first asks the guest for one submit, exactly as the XAudio2 worker did.
// The guest's own submit path (planar 6-channel float PCM, 256 frames) and its
// handle validation are unchanged.
#include <cstddef>
#include <cstdint>

#if !defined(_WIN32)

namespace DarkRecomp::Native::Ps5AudioDriver {

// Points the driver at the guest address space and its static TLS template.
// Must be called before the guest registers; the image owns both.
void initialize(uint8_t* base, const uint8_t* tls, size_t tlsSize);

// The kernel imports' handoff. `descriptor` is the guest's
// {callback, argument} pair; the token is published to `driverOut`.
uint32_t registerClient(uint32_t descriptor, uint32_t driverOut);
uint32_t submitFrame(uint32_t token, uint32_t samples);
uint32_t unregisterClient(uint32_t token);
uint32_t voiceCategoryVolumeChangeMask(uint32_t token, uint32_t outMask);

// One grain for the console's output. Production reaches it through the
// SceAudioOut feed thread; the host test drives it directly.
uint32_t pullGrain(float* interleaved6, uint32_t frames);

// Prints the health line now. Production prints it every 10 s of played audio;
// this is also how the host test checks the line.
void logHealth();

bool running();
void shutdown();

} // namespace DarkRecomp::Native::Ps5AudioDriver

#endif // !defined(_WIN32)
