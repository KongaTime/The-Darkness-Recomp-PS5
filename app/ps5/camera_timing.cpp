// Opt-in capture of the original camera's tick interpolation inputs and pose.
// No guest values are replaced. Arm with camera-timing.request in the writable
// data directory; camera-timing.bin contains a DCT4 header and 264-byte records.
#include "ppc_recomp_shared.h"
#include "runtime/native/camera_interpolation.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <xmmintrin.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* root = "/download0/darkness-ps5/";
thread_local float requestedPhase = 0;
struct PoseSelection {
    uint32_t root, selected;
    std::array<float,3> beforeEffects{};
    bool haveBeforeEffects = false;
    DarkRecomp::Native::WalkingCameraTrace walking;
    std::array<float,3> afterWalking{};
    uint32_t haveAfterWalking = 0;
};
thread_local PoseSelection* currentSelection = nullptr;
struct Record {
    uint64_t hostNs;
    uint32_t client, player, clientTick, playerTick;
    float phase, playerPhase, step, scale, requestedPhase, fov;
    double tickClock, renderClock;
    std::array<float,16> pose;
    uint32_t selectedPlayer, rootTick;
    float rootPhase;
    uint32_t selectedFlags;
    std::array<float,3> currentPosition, previousPosition, beforeEffects;
    uint32_t stageFlags;
    DarkRecomp::Native::WalkingCameraTrace walking;
    std::array<float,3> afterWalking;
    uint32_t haveAfterWalking;
};
static_assert(sizeof(Record) == 264);
float number(uint8_t* base, uint32_t address) {
    return std::bit_cast<float>(PPC_LOAD_U32(address));
}
double wideNumber(uint8_t* base, uint32_t address) {
    return std::bit_cast<double>(PPC_LOAD_U64(address));
}
bool span(uint32_t address, uint32_t bytes) {
    return address >= 0x10000 && uint64_t(address) + bytes <= 0x100000000ull;
}
void capture(uint8_t* base, uint32_t client, uint32_t player, const PoseSelection& selection, uint32_t pose) {
    const uint32_t selected = selection.selected;
    if (!span(client, 8980) || !span(player, 400) || !span(pose, 64)) return;
    const uint32_t vtable = PPC_LOAD_U32(client);
    if (vtable != 0x820807E0 && vtable != 0x82081BF0) return;
    // The square authored camera is a separate path, as in fov_camera.cpp.
    if (int32_t(PPC_LOAD_U32(client + 8976)) >= 0) return;
    const auto now = Clock::now();
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    static auto nextCheck = Clock::time_point{};
    static auto end = Clock::time_point{};
    static FILE* file = nullptr;
    static unsigned records = 0;
    if (now >= nextCheck) {
        nextCheck = now + std::chrono::seconds(1);
        std::error_code ec;
        const auto request = std::filesystem::path(root) / "camera-timing.request";
        if (std::filesystem::exists(request, ec) && !ec) {
            std::filesystem::remove(request, ec);
            if (file) std::fclose(file);
            file = std::fopen("/download0/darkness-ps5/camera-timing.bin", "wb");
            records = 0;
            if (file) {
                constexpr uint32_t header[] = {0x34544344, sizeof(Record)};
                if (std::fwrite(header, sizeof(header), 1, file) != 1) {
                    std::fclose(file); file = nullptr;
                } else {
                    end = now + std::chrono::seconds(30);
                    std::fprintf(stderr, "[CameraTiming] capture started\n");
                }
            }
        }
    }
    if (!file) return;
    if (now >= end || records >= 8192) {
        std::fclose(file); file = nullptr;
        std::fprintf(stderr, "[CameraTiming] capture complete records=%u\n", records);
        return;
    }
    const uint32_t rootState = PPC_LOAD_U32(player + 396);
    if (!span(selected, 400)) return;
    const uint32_t state = PPC_LOAD_U32(selected + 396);
    if (!span(state, 44) || !span(rootState, 44)) return;
    Record record{};
    record.hostNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    record.client = client; record.player = player;
    record.clientTick = PPC_LOAD_U32(client + 408);
    record.playerTick = PPC_LOAD_U32(state + 32);
    record.phase = number(base, client + 464);
    record.playerPhase = number(base, state + 40);
    record.selectedPlayer = selected;
    record.rootTick = PPC_LOAD_U32(rootState + 32);
    record.rootPhase = number(base, rootState + 40);
    record.selectedFlags = PPC_LOAD_U32(state + 20);
    for (unsigned i = 0; i < 3; ++i) {
        record.currentPosition[i] = number(base, selected + 128 + 4*i);
        record.previousPosition[i] = number(base, selected + 192 + 4*i);
    }
    record.beforeEffects = selection.beforeEffects;
    record.stageFlags = selection.haveBeforeEffects;
    record.walking = selection.walking;
    record.afterWalking = selection.afterWalking;
    record.haveAfterWalking = selection.haveAfterWalking;
    record.step = number(base, client + 412);
    record.scale = number(base, client + 424);
    record.requestedPhase = requestedPhase;
    record.fov = number(base, client + 2112 + 260);
    record.tickClock = wideNumber(base, client + 3136);
    record.renderClock = wideNumber(base, client + 3208);
    for (unsigned i = 0; i < 16; ++i) record.pose[i] = number(base, pose + i * 4);
    if (std::fwrite(&record, sizeof(record), 1, file) != 1) {
        std::fclose(file); file = nullptr;
        std::fprintf(stderr, "[CameraTiming] write failed\n");
        return;
    }
    if (++records % 120 == 0) std::fflush(file);
}
}

extern "C" PPC_FUNC(__imp__sub_8249B238);
PPC_FUNC(sub_8249B238) {
    const unsigned mode = _mm_getcsr();
    const float previous = requestedPhase;
    requestedPhase = float(ctx.f1.f64);
    _mm_setcsr(mode);
    __imp__sub_8249B238(ctx, base);
    requestedPhase = previous;
}

extern "C" PPC_FUNC(__imp__sub_821DC6F8);
PPC_FUNC(sub_821DC6F8) {
    const uint32_t player = ctx.r3.u32, client = ctx.r4.u32, message = ctx.r5.u32;
    // This is the original message-32 matrix query, not a render matrix hook.
    const uint32_t pose = span(message, 48) && PPC_LOAD_U32(message) == 32 &&
        PPC_LOAD_U32(message + 44) == 64 ? PPC_LOAD_U32(message + 40) : 0;
    PoseSelection selection{player, player};
    PoseSelection* previous = currentSelection;
    currentSelection = pose ? &selection : nullptr;
    __imp__sub_821DC6F8(ctx, base);
    currentSelection = previous;
    if (pose && ctx.r3.u32) {
        const unsigned mode = _mm_getcsr();
        capture(base, client, player, selection, pose);
        _mm_setcsr(mode);
    }
}

extern "C" PPC_FUNC(__imp__sub_8211B688);
PPC_FUNC(sub_8211B688) {
    const uint32_t player = ctx.r3.u32;
    __imp__sub_8211B688(ctx, base);
    // The camera can select a predicted copy instead of the authoritative
    // root. Observe the resolver's real result without changing that selection.
    if (currentSelection && player == currentSelection->root && ctx.r3.u32)
        currentSelection->selected = ctx.r3.u32;
}

extern "C" PPC_FUNC(__imp__sub_821DC078);
PPC_FUNC(sub_821DC078) {
    const auto previousTrace = DarkRecomp::Native::walkingCameraTrace;
    DarkRecomp::Native::walkingCameraTrace = {};
    const uint32_t pose = ctx.r4.u32;
    if (currentSelection && span(pose, 64)) {
        for (unsigned i = 0; i < 3; ++i)
            currentSelection->beforeEffects[i] = number(base, pose + 48 + 4*i);
        currentSelection->haveBeforeEffects = true;
    }
    __imp__sub_821DC078(ctx, base);
    if (currentSelection) currentSelection->walking = DarkRecomp::Native::walkingCameraTrace;
    DarkRecomp::Native::walkingCameraTrace = previousTrace;
}

extern "C" PPC_FUNC(__imp__sub_821DD7F8);
PPC_FUNC(sub_821DD7F8) {
    const uint32_t pose = ctx.r4.u32;
    __imp__sub_821DD7F8(ctx, base);
    if (currentSelection && span(pose, 64)) {
        const unsigned mode = _mm_getcsr();
        for (unsigned i = 0; i < 3; ++i)
            currentSelection->afterWalking[i] = number(base, pose + 48 + 4*i);
        currentSelection->haveAfterWalking = 1;
        _mm_setcsr(mode);
    }
}
