#pragma once
// The Win32 surface the engine's render layer touches, for the console.
//
// renderer/engine is portable C++ apart from a handful of Win32 calls it uses
// for timing, thread identity, byte swapping, process affinity and the
// memory-query path of its profiler. The Windows build takes those from
// <windows.h>; the console has no Win32, so this header supplies exactly that
// surface. It is included wherever <windows.h> is included, behind the same
// conditional, so the layer's own sources read the same on both targets.
//
// Nothing here emulates a Win32 behaviour the layer depends on for correctness:
// the only semantic shims are the value-returning queries. The two functions
// that relied on structured exception handling (guarded guest reads) do not use
// SEH on this target at all; see copyRenderMemory in simple_mesh.cpp and
// jobBytes in render_trace.cpp.
#if defined(_WIN32)
#error "posix_compat.h is the console's stand-in for <windows.h>"
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include <pthread.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

using DWORD = uint32_t;
using DWORD_PTR = uintptr_t;
using ULONG = unsigned long;
using LONG = int32_t;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using SIZE_T = size_t;
using HANDLE = void*;
using LPCVOID = const void*;
using LPVOID = void*;

struct LARGE_INTEGER {
    int64_t QuadPart;
};

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
};
using PMEMORY_BASIC_INFORMATION = MEMORY_BASIC_INFORMATION*;

struct EXCEPTION_POINTERS; // Declared only: no console code walks one.

// The two fault codes the guarded readers named. The console's replacements
// reject a bad range instead of catching the fault, but the names stay so the
// Windows and console paths read alike where they are mentioned.
constexpr DWORD EXCEPTION_ACCESS_VIOLATION = 0xC0000005u;
constexpr DWORD EXCEPTION_IN_PAGE_ERROR = 0xC0000006u;
constexpr LONG EXCEPTION_EXECUTE_HANDLER = 1;
constexpr LONG EXCEPTION_CONTINUE_SEARCH = 0;

inline uint32_t _byteswap_ulong(uint32_t value) { return __builtin_bswap32(value); }
inline uint16_t _byteswap_ushort(uint16_t value) { return __builtin_bswap16(value); }
inline uint64_t _byteswap_uint64(uint64_t value) { return __builtin_bswap64(value); }

inline uint64_t GetTickCount64() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

// The full Win32 subset (win32_ps5.h, the console title) implements these four
// itself; the renderer's host tests build with only this header.
#if !defined(DARK_FULL_WIN32_SUBSET)
inline uint32_t GetCurrentThreadId() {
    return uint32_t(reinterpret_cast<uintptr_t>(pthread_self()));
}
#endif

// The console's POSIX subset has no sched_getcpu and the layer only reads this
// while host CPU profiling is armed, which it is not here.
inline uint32_t GetCurrentProcessorNumber() { return 0; }

// std::this_thread rather than usleep: the console's libc subset does not carry
// every BSD-era entry point, and this one is the C++ library's.
inline void Sleep(uint32_t milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

inline void QueryPerformanceCounter(LARGE_INTEGER* value) {
    using Clock = std::chrono::steady_clock;
    value->QuadPart = int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  Clock::now().time_since_epoch())
                                  .count());
}

inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(intptr_t(-1)); }
#if !defined(DARK_FULL_WIN32_SUBSET)
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(pthread_self()); }
#endif

// The console does not expose a restricted affinity mask, so the answer is
// "not known" and the scene worker pool keeps its hardware_concurrency count.
#if !defined(DARK_FULL_WIN32_SUBSET)
inline int GetProcessAffinityMask(HANDLE, DWORD_PTR*, DWORD_PTR*) { return 0; }
#endif

// Thread names are a debugger convenience on the host; the console has no
// equivalent and the layer only ever calls this for diagnostics.
inline int SetThreadDescription(HANDLE, const wchar_t*) { return 0; }

// The engine's profiler asks the host whether a guest address is resident
// before reading it. The console's guest space is one committed 4 GB mapping,
// so an address inside it is always resident; the layer never reaches this
// path with profiling enabled (it is compiled out), and reporting "no
// information" is the honest answer for the addresses it cannot describe.
#if !defined(DARK_FULL_WIN32_SUBSET)
inline SIZE_T VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T) { return 0; }
#endif
