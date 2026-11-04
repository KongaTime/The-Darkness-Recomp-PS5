#pragma once
// Forced into translated translation units only, with ThinLTO enabled.
// Ordinary builds keep the recompiler's noinline weak aliases. ThinLTO sees
// the final strong native overrides before deciding which guest calls can be
// inlined. The aliases remain weak, and __imp__ originals remain callable.
// tools/ps5-lto-hook-test.sh checks both overridden and unoverridden calls,
// including calls in the alias's own translation unit and function pointers.
#include "ppc_recomp_shared.h"
#undef PPC_WEAK_FUNC
#define PPC_WEAK_FUNC(x) __attribute__((weak)) PPC_FUNC(x)
