#pragma once
// <windows.h> on Windows, the console's stand-in everywhere else.
//
// The engine's render layer is shared by both builds: it includes this header
// where it used to include <windows.h>, and everything it takes from Windows
// (ticks, thread identity, byte swaps, affinity, the profiler's memory query)
// is named the same way on the console. The console title gets the full Win32
// subset the native runtime needs (win32_ps5.h); the render layer's host tests
// build the console branch with only its small stand-in (posix_compat.h).
#if defined(_WIN32)
#include <windows.h>
#define DARK_NOINLINE __declspec(noinline)
#elif defined(__PROSPERO__)
#include "win32_ps5.h"
#define DARK_NOINLINE __attribute__((noinline))
#else
#include "posix_compat.h"
#define DARK_NOINLINE __attribute__((noinline))
#endif
