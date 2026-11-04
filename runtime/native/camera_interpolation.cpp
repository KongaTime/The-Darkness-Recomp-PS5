#include "camera_interpolation.h"
#include <cmath>

namespace DarkRecomp::Native {
thread_local constinit WalkingCameraTrace walkingCameraTrace;
bool WalkingCameraMotion::carry(uint64_t owner, uint32_t tick, float phase,
    const std::array<float,3>& base, std::array<float,3>& history) noexcept {
    if (!std::isfinite(phase) || phase < 0 || phase > 1) { reset(); return false; }
    double jump = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(base[i]) || !std::isfinite(history[i])) { reset(); return false; }
        const double delta = double(base[i])-base_[i];
        jump += delta*delta;
    }
    const uint32_t ticks = tick-tick_;
    const bool continuous = have_ && owner == owner_ && ticks <= 1 &&
        (ticks != 0 || phase >= phase_) && jump <= 1600.0;
    auto result = history;
    if (continuous) for (unsigned i = 0; i < 3; ++i) {
        result[i] += base[i]-base_[i];
        if (!std::isfinite(result[i])) { reset(); return false; }
    }
    have_ = true; owner_ = owner; tick_ = tick; phase_ = phase; base_ = base;
    if (continuous) history = result;
    return continuous;
}
bool cameraRenderPhase(uint32_t clientVtable, float renderPhase, float& output) noexcept {
    if (clientVtable != 0x820807E0 && clientVtable != 0x82081BF0) return false;
    // The completed gameplay render clock wraps its fraction each tick. Leave
    // unrelated modes, malformed state and extrapolation paths untouched.
    if (!std::isfinite(renderPhase) || renderPhase < 0 || renderPhase > 1) return false;
    output = renderPhase;
    return true;
}
}
