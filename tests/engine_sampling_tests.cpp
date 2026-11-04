#include "renderer/engine/engine_performance.h"
#include <cassert>
#include <thread>
using namespace DarkRecomp::Native;
int main() {
    const auto other=unsigned(EnginePhase::count);
    sampleEngineThread=true;
    {EngineCpuScope scope(EnginePhase::stored);assert(sampledEnginePhase==other);}
    sampleEngineCpu=true;
    setRenderSamplePhase(RenderSamplePhase::retire);
    assert(renderSamplePhase==unsigned(RenderSamplePhase::retire));
    {
        EngineCpuScope scope(EnginePhase::stored);
        assert(sampledEnginePhase==unsigned(EnginePhase::stored));
        {EngineCpuScope child(EnginePhase::snapshot);assert(sampledEnginePhase==unsigned(EnginePhase::snapshot));}
        assert(sampledEnginePhase==unsigned(EnginePhase::stored));
        {EngineCpuScope inactive(EnginePhase::textures,false);assert(sampledEnginePhase==unsigned(EnginePhase::stored));}
        std::thread worker([] {EngineCpuScope ignored(EnginePhase::queryAudio);});
        worker.join();assert(sampledEnginePhase==unsigned(EnginePhase::stored));
        sampleEngineCpu=false; // An active scope must still restore its parent.
    }
    assert(sampledEnginePhase==other);
    setRenderSamplePhase(RenderSamplePhase::other);
    assert(renderSamplePhase==unsigned(RenderSamplePhase::retire)); // Disarmed sampling leaves renderer tags untouched.
    for(const auto& phase:enginePhases)assert(phase.calls==0 && phase.nanoseconds==0);
    std::puts("Engine sampling: nesting, inactive scopes, worker isolation, disarm and no timing counters pass.");
}
