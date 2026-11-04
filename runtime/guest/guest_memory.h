#pragma once
#include "guest_types.h"
#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#elif defined(__PROSPERO__)
#include <sys/mman.h>
// The console's kernel calls. The payload SDK links them but declares none of
// them, so the title names the ones it uses (the signatures are the public
// PS4 SDK's, which the console keeps).
extern "C" int32_t sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t length,
                                                 size_t alignment, int memoryType, int64_t* physicalStart);
extern "C" int32_t sceKernelReleaseDirectMemory(int64_t start, size_t length);
extern "C" int32_t sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                                             int64_t directStart, size_t alignment);
extern "C" int64_t sceKernelGetDirectMemorySize(void);
extern "C" int32_t sceKernelMunmap(void* address, size_t length);
#endif

namespace DarkRecomp {

class GuestMemory {
public:
    GuestMemory() : m_base(nullptr), m_size(0x100000000ULL), m_physical(-1), m_lastError(0),
                    m_lastStage("") {} // 4 GB space
    ~GuestMemory() {
        Shutdown();
    }

    // Where the console maps the guest space. Direct memory is placed by the
    // kernel unless an address is hinted, and RADV places its own buffers in
    // the window it reserves at 0x40_0000_0000: a mapping taken first fit can
    // land in that window and make the driver's later mappings fail. 64 GiB is
    // clear of it.
    static constexpr uint64_t kConsoleBase = 0x1000000000ULL;
    // CPU and GPU read and write (the protection every native homebrew uses for
    // direct memory). The guest is only touched by the CPU today, but a renderer
    // that later points the GPU straight at guest memory needs the GPU bits.
    static constexpr int kConsoleProtection = 0x33;
    // The direct-memory type CPU allocations use, as ps5vk, RADV and PS5SX2 ask
    // for it.
    static constexpr int kConsoleDirectType = 12;
    static constexpr size_t kConsoleAlignment = 2u << 20;

    bool Initialize() {
#if defined(_WIN32)
        m_base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (m_base == nullptr) {
            m_base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        // NOTE (2026-09-03): Xbox low memory (0x0-0xFFFF) contains kernel
        // structures that retail code reads very early (e.g. *(0x193) via
        // *(0x82000814)). Guarding it faults before we emulate those
        // structures. Keep it mapped as zeroed memory for now (reads return
        // 0, matching the pre-guard behavior that reached heap init); a
        // future change should emulate the specific kernel pointers and
        // guard only NULL writes (e.g. `stw r0,0(0)` asserts) via a vectored
        // handler that distinguishes read vs write.
        return m_base != nullptr;
#elif defined(__PROSPERO__)
        // PS5: the guest's 4 GB comes from the console's direct memory. A
        // title's own heap is the flexible pool, 448 MiB configured, so an
        // anonymous mapping this size fails with ENOMEM - measured on the
        // console on 2026-10-24 (320 MiB maps, 384 MiB does not). Direct memory
        // is the ~12 GiB pool a title gets for its large allocations, and this
        // is the recipe every other native homebrew uses: allocate 2 MiB
        // aligned, then map the physical range with a fixed address hint.
        int64_t physical = -1;
        if ((m_lastError = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PPC_MEMORY_SIZE,
                                                         kConsoleAlignment, kConsoleDirectType, &physical)) != 0) {
            m_lastStage = "sceKernelAllocateDirectMemory";
            m_base = nullptr;
            return false;
        }
        void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(kConsoleBase));
        if ((m_lastError = sceKernelMapDirectMemory(&mapped, PPC_MEMORY_SIZE, kConsoleProtection, 0, physical,
                                                    kConsoleAlignment)) != 0) {
            m_lastStage = "sceKernelMapDirectMemory";
            sceKernelReleaseDirectMemory(physical, PPC_MEMORY_SIZE);
            m_base = nullptr;
            return false;
        }
        m_lastError = 0;
        m_lastStage = "";
        m_physical = physical;
        m_base = static_cast<uint8_t*>(mapped);
        return m_base != nullptr;
#else
        return false;
#endif
    }

    void Shutdown() {
#if defined(_WIN32)
        if (m_base) {
            VirtualFree(m_base, 0, MEM_RELEASE);
            m_base = nullptr;
        }
#elif defined(__PROSPERO__)
        if (m_base) {
            sceKernelMunmap(m_base, PPC_MEMORY_SIZE);
            if (m_physical >= 0) sceKernelReleaseDirectMemory(m_physical, PPC_MEMORY_SIZE);
            m_base = nullptr;
            m_physical = -1;
        }
#endif
    }

    uint8_t* GetBase() const { return m_base; }

    // The console call that refused the reservation (0 when it succeeded), for
    // the title's log: an 0x8xxxxxxx kernel code, not an errno.
    int LastError() const { return m_lastError; }
    const char* LastStage() const { return m_lastStage; }

    inline void* Translate(uint32_t guestAddr) const {
        return m_base + guestAddr;
    }

    inline uint32_t MapVirtual(const void* hostPtr) const {
        return static_cast<uint32_t>(static_cast<const uint8_t*>(hostPtr) - m_base);
    }

    template<typename T>
    inline T Read(uint32_t guestAddr) const {
        return *reinterpret_cast<const be<T>*>(Translate(guestAddr));
    }

    template<typename T>
    inline void Write(uint32_t guestAddr, T value) {
        *reinterpret_cast<be<T>*>(Translate(guestAddr)) = value;
    }

private:
    uint8_t* m_base;
    size_t   m_size;
    // The console's direct-memory range behind m_base, so it can be released
    // with the mapping (Windows has no such thing: -1 there).
    int64_t  m_physical;
    int      m_lastError;
    const char* m_lastStage;
};

extern GuestMemory g_guestMemory;

} // namespace DarkRecomp
