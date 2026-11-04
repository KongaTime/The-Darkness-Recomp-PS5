#pragma once
#include <cstdint>
#include <array>

namespace DarkRecomp::Native {
// Read-only stage observations used by the opt-in PS5 camera capture.
struct WalkingCameraTrace {
    uint32_t flags = 0, rejection = 0, rootTick = 0, rootCut = 0, selectedCut = 0;
    float phase = 0, elapsed = 0;
    std::array<float,3> rawHead{}, filteredHead{}, smoothedHead{};
};
static_assert(sizeof(WalkingCameraTrace) == 64);
extern thread_local constinit WalkingCameraTrace walkingCameraTrace;
// Carry only the visual smoothing history with the current predicted base.
// The guest still computes bob, smoothing weights and wall collision.
class WalkingCameraMotion {
    bool have_ = false;
    uint64_t owner_ = 0;
    uint32_t tick_ = 0;
    float phase_ = 0;
    std::array<float,3> base_{};
public:
    void reset() noexcept { have_ = false; }
    bool carry(uint64_t owner, uint32_t tick, float phase,
        const std::array<float,3>& base, std::array<float,3>& history) noexcept;
};
// A fraction paired with client+408 must come from that same client's render
// clock (+464), not a predicted player copy's separately advancing state+40.
bool cameraRenderPhase(uint32_t clientVtable, float renderPhase, float& output) noexcept;
}
