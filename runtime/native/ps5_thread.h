#pragma once
// Laying out a guest thread on the console.
//
// Every guest thread authenticates through its PCR: the static TLS copy, the
// thread object with its stack bounds, and r13 pointing at the PCR. A thread
// that enters guest code without this faults on its first TLS access. Both the
// main thread (app/ps5/guest_image.cpp) and the engine's audio worker take
// their layout from here so the two cannot drift.
#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(_WIN32)
#include "ppc_context.h"

namespace DarkRecomp::Native {

inline constexpr uint32_t kGuestTlsOffset = 0xab0;
inline constexpr uint32_t kGuestThreadObjectOffset = 0xd00;

inline void initGuestThread(uint8_t* base, PPCContext& ctx, const uint8_t* tls, size_t tlsSize,
                            uint32_t threadId, uint32_t stackLow, uint32_t stackSize, uint32_t pcr) {
    ctx = PPCContext{};
    ctx.r1.u64 = uint64_t(stackLow) + stackSize - 0x100;
    ctx.r13.u64 = pcr;

    std::memset(base + pcr, 0, 0x1000);
    if (tls && tlsSize) std::memcpy(base + pcr + kGuestTlsOffset, tls, tlsSize);
    auto write32 = [base](uint32_t address, uint32_t value) {
        const uint32_t be = __builtin_bswap32(value);
        std::memcpy(base + address, &be, sizeof(be));
    };
    write32(pcr, pcr + kGuestTlsOffset);
    write32(pcr + 0x100, pcr + kGuestThreadObjectOffset);
    const uint32_t thread = pcr + kGuestThreadObjectOffset;
    write32(thread + 0x5c, stackLow + stackSize);
    write32(thread + 0x60, stackLow);
    write32(thread + 0x68, pcr + kGuestTlsOffset);
    write32(thread + 0x14c, threadId);

    // The guest expects the IEEE floating-point state; the console starts a
    // title with flush-to-zero and denormals-are-zero.
    ctx.fpscr.loadFromHost();
}

} // namespace DarkRecomp::Native

#endif // !defined(_WIN32)
