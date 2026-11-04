#include "cpu_sampling.h"
#include "thread_placement.h"
#include "ppc_context.h"
#include "renderer/engine/engine_performance.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <cpuid.h>
#include <ps5platform/kernel.h>

namespace DarkRecomp::Ps5 {
// Host instruction sampling. During a capture the sampler sends SIGUSR2 to each
// registered thread every millisecond; the handler records the interrupted
// instruction and the top kStackWords words of its stack; the first of those
// that points into the title is the caller of a library routine (memcpy and
// kernel waits push registers first, so it is not always the top word). The raw samples go to
// cpu-samples.bin and tools/ps5-symbolize-samples.py names them against the
// unstripped darkness_ps5 using the anchor address written with them.
namespace {
constexpr size_t kHostSamples = 40000;
constexpr size_t kStackWords = 12;
struct HostSample {
    uint64_t rip;
    std::array<uint64_t, kStackWords> stack;
};
struct HostSampleRing {
    std::array<HostSample, kHostSamples> samples;
    std::atomic<uint32_t> used{0};
};
std::array<HostSampleRing, size_t(SampledThread::count)> hostRings;
std::array<std::atomic<bool>, size_t(SampledThread::count)> registered{};
std::array<pthread_t, size_t(SampledThread::count)> sampledThreads{};
std::atomic<bool> hostSampling{false};
std::array<std::atomic<uint64_t>,2> originalAffinity{};
std::array<std::atomic<int>,2> initialCpu{};
std::array<std::array<std::atomic<unsigned>,256>,2> sampledApic{};
thread_local int sampledRole = -1;

void onSampleSignal(int, siginfo_t*, void* raw) {
    const int role = sampledRole;
    if (role < 0 || !hostSampling.load(std::memory_order_relaxed)) return;
    const auto* context = static_cast<const ucontext_t*>(raw);
    auto& ring = hostRings[size_t(role)];
    const uint32_t slot = ring.used.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kHostSamples) return;
    const auto* rsp = reinterpret_cast<const uint64_t*>(context->uc_mcontext.mc_rsp);
    HostSample& sample = ring.samples[slot];
    unsigned eax,ebx,ecx,edx;
    __cpuid_count(1,0,eax,ebx,ecx,edx);
    sampledApic[size_t(role)][ebx>>24].fetch_add(1,std::memory_order_relaxed);
    sample.rip = uint64_t(context->uc_mcontext.mc_rip);
    for (size_t i = 0; i < kStackWords; ++i) sample.stack[i] = rsp ? rsp[i] : 0;
}

bool installSampleSignal() {
    struct sigaction action{};
    action.sa_sigaction = onSampleSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGUSR2, &action, nullptr) == 0;
}
}

void registerSampledThread(SampledThread role) {
    sampledRole = int(role);
    sampledThreads[size_t(role)] = pthread_self();
    uint64_t mask=0;
    const auto rc=scePthreadGetaffinity(pthread_self(),&mask);
    originalAffinity[size_t(role)].store(rc==0?mask:0,std::memory_order_relaxed);
    initialCpu[size_t(role)].store(sceKernelGetCurrentCpu(),std::memory_order_relaxed);
    registered[size_t(role)].store(true, std::memory_order_release);
}

void runCpuSampler(PPCContext& context, const std::atomic<bool>& done) {
    namespace Native = DarkRecomp::Native;
    using Clock = std::chrono::steady_clock;
    const char* request = "/download0/darkness-ps5/cpu-sampling.request";
    while(!done.load(std::memory_order_relaxed)) {
        // A short FTP-reachable request applies or restores placement without
        // restarting gameplay. Only masks inside both original allowed sets
        // are accepted, and a failed pair is rolled back to its prior state.
        if(registered[0].load(std::memory_order_acquire) && registered[1].load(std::memory_order_acquire)) {
            if(FILE* placement=std::fopen("/download0/darkness-ps5/thread-placement.request","r")) {
                unsigned long long guestMask=0,renderMask=0;
                const bool parsed=std::fscanf(placement,"%llx %llx",&guestMask,&renderMask)==2;
                std::fclose(placement);
                std::error_code ec;
                std::filesystem::remove("/download0/darkness-ps5/thread-placement.request",ec);
                const auto result=parsed?applyThreadPlacement(
                    std::array<uint64_t,2>{originalAffinity[0].load(),originalAffinity[1].load()},
                    std::array<uint64_t,2>{guestMask,renderMask},
                    [&](unsigned role,uint64_t& mask){return scePthreadGetaffinity(sampledThreads[role],&mask);},
                    [&](unsigned role,uint64_t mask){return scePthreadSetaffinity(sampledThreads[role],mask);}) : ThreadPlacementResult{};
                if(FILE* out=std::fopen("/download0/darkness-ps5/thread-placement.txt","w")) {
                    std::fprintf(out,"status=%d target=%llx/%llx before=%llx/%llx after=%llx/%llx set=%d/%d rollback=%d/%d\n",
                        int(result.status),(unsigned long long)result.target[0],(unsigned long long)result.target[1],
                        (unsigned long long)result.before[0],(unsigned long long)result.before[1],
                        (unsigned long long)result.after[0],(unsigned long long)result.after[1],
                        result.setCodes[0],result.setCodes[1],result.rollbackCodes[0],result.rollbackCodes[1]);
                    std::fclose(out);
                }
            }
        }
        std::error_code error;
        if(!std::filesystem::exists(request,error) || error) {
            for(unsigned i=0;i<10 && !done.load(std::memory_order_relaxed);++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::filesystem::remove(request,error);
        std::array<unsigned,size_t(Native::EnginePhase::count)+1> phases{};
        std::array<unsigned,size_t(Native::RenderSamplePhase::count)> renderPhases{};
        std::unordered_map<uint64_t,unsigned> entries;
        unsigned samples=0;
        static const bool signalReady = installSampleSignal();
        for (auto& ring : hostRings) ring.used.store(0, std::memory_order_relaxed);
        for(auto& role:sampledApic)for(auto& cpu:role)cpu.store(0,std::memory_order_relaxed);
        hostSampling.store(signalReady, std::memory_order_relaxed);
        Native::sampleEngineCpu.store(true,std::memory_order_relaxed);
        const auto started=Clock::now(), end=started+std::chrono::seconds(30);
        std::fprintf(stderr,"[CpuSample] capture started (30 seconds)\n");
        while(Clock::now()<end && !done.load(std::memory_order_relaxed)) {
            const auto phase=Native::sampledEnginePhase.load(std::memory_order_relaxed);
            if(phase<phases.size())++phases[phase];
            const auto renderPhase=Native::renderSamplePhase.load(std::memory_order_relaxed);
            if(renderPhase<renderPhases.size())++renderPhases[renderPhase];
            // Best-effort diagnostic fields, as in the existing watchdog.
            // A sampled pair need not describe one instruction; no guest state
            // or execution decision depends on these observations.
            const uint32_t function=context.lastFunction, caller=uint32_t(context.lr);
            ++entries[uint64_t(function)<<32 | caller];
            ++samples;
            if (signalReady)
                for (size_t role = 0; role < sampledThreads.size(); ++role)
                    if (registered[role].load(std::memory_order_acquire)) pthread_kill(sampledThreads[role], SIGUSR2);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Native::sampleEngineCpu.store(false,std::memory_order_relaxed);
        hostSampling.store(false, std::memory_order_relaxed);
        // Let a handler that already passed its check finish its slot.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (FILE* raw = std::fopen("/download0/darkness-ps5/cpu-samples.bin", "wb")) {
            // Header: magic, anchor (this function's runtime address), stack
            // words per sample, then per role its sample count followed by
            // (rip, stack words) records.
            const uint64_t header[3]{0x32534D5048524B44ull, uint64_t(reinterpret_cast<uintptr_t>(&runCpuSampler)),
                                     kStackWords};
            std::fwrite(header, sizeof header, 1, raw);
            for (auto& ring : hostRings) {
                const uint64_t count = std::min<uint64_t>(ring.used.load(std::memory_order_relaxed), kHostSamples);
                std::fwrite(&count, sizeof count, 1, raw);
                std::fwrite(ring.samples.data(), sizeof(HostSample), size_t(count), raw);
            }
            std::fclose(raw);
        }
        FILE* file=std::fopen("/download0/darkness-ps5/cpu-sampling.txt","w");
        auto write=[&](const char* line) {std::fputs(line,stderr);if(file)std::fputs(line,file);};
        char line[256];
        for(unsigned role=0;role<2;++role) {
            uint64_t mask=0;
            const auto rc=scePthreadGetaffinity(sampledThreads[role],&mask);
            std::snprintf(line,sizeof(line),"[CpuThread] role=%u original=%llx current=%llx initialCpu=%d queryRc=%d\n",
                role,(unsigned long long)originalAffinity[role].load(),(unsigned long long)mask,initialCpu[role].load(),rc);write(line);
            for(unsigned cpu=0;cpu<256;++cpu)if(const auto count=sampledApic[role][cpu].load()) {
                std::snprintf(line,sizeof(line),"[CpuApic] role=%u apic=%u samples=%u\n",role,cpu,count);write(line);
            }
        }
        std::snprintf(line,sizeof(line),"[CpuSample] samples=%u elapsedMs=%.2f\n",samples,
            std::chrono::duration<double,std::milli>(Clock::now()-started).count());write(line);
        std::snprintf(line,sizeof(line),"[CpuHostSample] signal=%s guest=%u render=%u anchor=%p\n",
            signalReady?"yes":"no",hostRings[0].used.load(),hostRings[1].used.load(),
            reinterpret_cast<void*>(&runCpuSampler));write(line);
        constexpr const char* names[]{"stored","immediate","indexed","vertexProgram","descriptor","conversion","textures","palette",
            "snapshot","worldCapture","handoff","mainSleep","vqXma","vqKernel","vqDispatcher","vqAudio","vqFile","wsDispatcher",
            "geometryResolve","bindingCapture","drawAllocation","vertexConstants","queueLock","queueTextures",
            "queueAccounting","queuePublish","drawOriginal","queueWait","other"};
        static_assert(std::size(names)==std::tuple_size_v<decltype(phases)>);
        for(unsigned i=0;i<phases.size();++i) {
            std::snprintf(line,sizeof(line),"[CpuSamplePhase] %s=%u (%.2f%%)\n",names[i],phases[i],samples?100.0*phases[i]/samples:0);write(line);
        }
        for(unsigned i=0;i<renderPhases.size();++i) {
            std::snprintf(line,sizeof(line),"[CpuRenderPhase] %s=%u (%.2f%%)\n",Native::renderSamplePhaseNames[i],
                renderPhases[i],samples?100.0*renderPhases[i]/samples:0);write(line);
        }
        std::vector<std::pair<uint64_t,unsigned>> ranked(entries.begin(),entries.end());
        std::sort(ranked.begin(),ranked.end(),[](auto a,auto b){return a.second>b.second;});
        for(unsigned i=0;i<std::min(size_t(48),ranked.size());++i) {
            const auto [key,count]=ranked[i];
            std::snprintf(line,sizeof(line),"[CpuSampleEntry] function=%08X lr=%08X samples=%u (%.2f%%)\n",
                uint32_t(key>>32),uint32_t(key),count,samples?100.0*count/samples:0);write(line);
        }
        if(file)std::fclose(file);
    }
}
}
