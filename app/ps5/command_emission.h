#pragma once
#include <cstdint>
struct PPCContext;
namespace DarkRecomp::Ps5 {
// Bounded, byte-identical fast path for original 828714A0. A miss leaves guest
// memory and registers untouched; the wrapper then executes the original.
bool tryEmitVectorConstants(PPCContext&,uint8_t* base) noexcept;
}
