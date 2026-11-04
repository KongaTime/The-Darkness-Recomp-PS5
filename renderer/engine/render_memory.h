#pragma once
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace DarkRecomp::Native {
#if defined(__PROSPERO__)
// The console owns one committed 4 GiB guest mapping. Keep exactly the same
// range checks as the guarded host helper, but expose constant-size copies to
// the optimizer. Windows still requires the out-of-line SEH-protected loads.
inline bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size) {
    if (!base || !output || !address || address >= 0x100000000ull || size > 0x100000000ull-address) return false;
    // Snapshot validation deliberately rereads live guest state. Keep those
    // observations distinct after inlining, without a hardware fence.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(output,base+address,size);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return true;
}
inline bool equalRenderMemory(uint8_t* base, uint64_t address, const void* expected, size_t size) {
    if (!base || !expected || !address || address >= 0x100000000ull || size > 0x100000000ull-address) return false;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const bool equal = std::memcmp(expected,base+address,size)==0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return equal;
}
#else
bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size);
bool equalRenderMemory(uint8_t* base, uint64_t address, const void* expected, size_t size);
#endif
}
