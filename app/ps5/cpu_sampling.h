#pragma once
#include <atomic>
struct PPCContext;
namespace DarkRecomp::Ps5 {
void runCpuSampler(PPCContext& context, const std::atomic<bool>& done);
// The threads whose host instructions a capture samples. Each calls this once,
// on itself, before its loop starts.
enum class SampledThread : unsigned { guest = 0, render = 1, count = 2 };
void registerSampledThread(SampledThread role);
}
