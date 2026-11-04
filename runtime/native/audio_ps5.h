#pragma once
// The console's audio output: SceAudioOut at 48 kHz, one 256-frame grain at a
// time. The engine's render driver pushes 6-channel float PCM; this backend
// pulls one grain per output callback and downmixes it to stereo 16-bit.
#include <cstdint>

#if !defined(_WIN32)

namespace DarkRecomp::Native::Ps5Audio {

constexpr uint32_t kGrainFrames = 256;  // one engine submit
constexpr uint32_t kOutChannels = 2;    // stereo output
constexpr uint32_t kRate = 48000;
constexpr uint32_t kEngineChannels = 6;

// The output thread's source: fills `frames` interleaved 6-channel float
// frames, silence when nothing is queued.
using FillFn = uint32_t (*)(float* interleaved6, uint32_t frames, void* user);

// Folds 6-channel interleaved float PCM to the console's 16-bit stereo with
// saturation (FL, FR, FC, LFE, SL, SR layout). `out` holds `frames * 2`
// samples. Also the unit the host test drives directly.
void packStereo(const float* interleaved6, int16_t* out, uint32_t frames);

// Opens the output and starts the feed thread. Returns false if the output is
// unavailable; the caller keeps running, silently.
bool audioStart(FillFn fill, void* user);
void audioStop();
bool audioRunning();
// True on the feed thread, where stopping (joining itself) is impossible.
bool onOutputThread();

} // namespace DarkRecomp::Native::Ps5Audio

#endif // !defined(_WIN32)
