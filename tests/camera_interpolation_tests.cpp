#include "runtime/native/camera_interpolation.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

using DarkRecomp::Native::cameraRenderPhase;
using DarkRecomp::Native::WalkingCameraMotion;

struct Sample { uint32_t tick; float render, predicted; };
static void monotonic(const Sample* rows, size_t count, bool expectLegacyFailure) {
    unsigned legacyBackwards = 0;
    double previous = -1, previousLegacy = -1;
    for (size_t i = 0; i < count; ++i) {
        float phase = -1;
        assert(cameraRenderPhase(0x820807E0, rows[i].render, phase));
        const double time = double(rows[i].tick) + phase;
        const double legacy = double(rows[i].tick) + rows[i].predicted;
        if (i) { assert(time > previous); legacyBackwards += legacy < previousLegacy; }
        previous = time; previousLegacy = legacy;
    }
    if (expectLegacyFailure) assert(legacyBackwards > 0);
    std::printf("Camera timeline: %zu samples, old backward steps=%u, corrected=0\n", count, legacyBackwards);
}

int main(int argc, char** argv) {
    // Original world-space history trails steady movement. Carrying the base
    // retains exactly the authored smoothing of the visual offset instead.
    WalkingCameraMotion motion;
    std::array<float,3> history{0,0,64};
    float expectedBob = 0;
    for (unsigned frame = 0; frame < 240; ++frame) {
        const float direction = frame < 120 ? 1.f : -1.f;
        const float x = frame < 120 ? frame*1.6f : (240-frame)*1.6f;
        const std::array<float,3> base{x,direction*frame*.1f,0};
        const float bob = std::sin(frame*.08f)*2;
        assert(motion.carry(7,100+frame/2,float(frame%2)*.5f,base,history) == (frame != 0));
        for (unsigned axis = 0; axis < 2; ++axis)
            history[axis] += (base[axis]-history[axis])*.1f;
        history[2] += (64+bob-history[2])*.1f;
        expectedBob += (bob-expectedBob)*.1f;
        assert(std::abs(history[0]-base[0]) < .001f);
        assert(std::abs(history[1]-base[1]) < .001f);
        assert(std::abs(history[2]-(64+expectedBob)) < .001f);
    }
    auto unchangedHistory = history;
    assert(!motion.carry(7,220,0,{1000,0,0},history) && history == unchangedHistory); // cut
    assert(!motion.carry(8,220,.5f,{1000,0,0},history) && history == unchangedHistory); // owner
    assert(!motion.carry(8,220,.25f,{1000,0,0},history)); // backward phase
    assert(!motion.carry(8,225,0,{1000,0,0},history)); // skipped ticks
    assert(!motion.carry(8,225,0,{NAN,0,0},history) && history == unchangedHistory);
    assert(!motion.carry(8,UINT32_MAX,0,{0,0,0},history));
    assert(motion.carry(8,0,.5f,{1,2,0},history));
    assert(history[0] == unchangedHistory[0]+1 && history[1] == unchangedHistory[1]+2);
    float output = 99;
    assert(cameraRenderPhase(0x820807E0, .25f, output) && output == .25f);
    assert(cameraRenderPhase(0x82081BF0, .75f, output) && output == .75f);
    for (float invalid : {-1.f, 1.1f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        output = 99;
        assert(!cameraRenderPhase(0x820807E0, invalid, output) && output == 99);
    }
    assert(!cameraRenderPhase(0x8207F438, .5f, output) && output == 99);
    // Prediction wraps in a different frame from the render clock. Mixing its
    // phase with the render tick makes zoom/animation time run backwards.
    constexpr Sample regression[] = {{100, .59f, .109375f}, {101, .09f, .625f},
                                    {101, .58f, .109375f}, {102, .08f, .625f}};
    monotonic(regression, 4, true);
    if (argc > 1) {
        FILE* file = std::fopen(argv[1], "rb");
        assert(file);
        std::array<char,8> header{};
        assert(std::fread(header.data(), header.size(), 1, file) == 1);
        assert(std::memcmp(header.data(), "DCT2", 4) == 0 || std::memcmp(header.data(), "DCT3", 4) == 0 ||
               std::memcmp(header.data(), "DCT4", 4) == 0);
        uint32_t size;
        std::memcpy(&size, header.data()+4, 4);
        assert(size == 144 || size == 184 || size == 264);
        std::array<Sample,8192> rows{};
        std::array<char,264> record{};
        size_t count = 0;
        while (std::fread(record.data(), size, 1, file) == 1) {
            assert(count < rows.size());
            std::memcpy(&rows[count].tick, record.data()+16, 4);
            std::memcpy(&rows[count].render, record.data()+24, 4);
            std::memcpy(&rows[count].predicted, record.data()+28, 4);
            ++count;
        }
        assert(!std::ferror(file) && count > 1);
        std::fclose(file);
        monotonic(rows.data(), count, true);
    }
    std::puts("Camera interpolation: clock pairing, visual offsets, responsive base movement and cuts pass.");
}
