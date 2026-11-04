#include "runtime/native/input_ps5.h"
#include <cassert>
#include <cstdio>

using namespace DarkRecomp::Native::Ps5Pad;
static PadSample sample{};
static bool fresh = true;
extern "C" int scePadInit() { return 0; }
extern "C" int scePadOpen(int32_t, int32_t, int32_t, const void*) { return 1; }
extern "C" int scePadGetHandle(int32_t, int32_t, int32_t) { return 1; }
extern "C" int sceUserServiceInitialize(const void*) { return 0; }
extern "C" int sceUserServiceGetInitialUser(int32_t* user) { *user = 1; return 0; }
extern "C" int scePadRead(int32_t, void* out, int32_t) {
    if (!fresh) return 0;
    *static_cast<PadSample*>(out) = sample;
    fresh = false;
    return 1;
}
int main() {
    sample.connected = 1;
    sample.leftX = sample.leftY = sample.rightX = sample.rightY = 128;
    PadState previous{}, next{};
    assert(readPad(previous));
    assert(readPad(next) && next.packet == previous.packet);
    // Each one-unit physical movement must reach a packet-aware guest. The
    // former 512-unit per-poll threshold suppressed all of these changes.
    for (unsigned raw = 129; raw < 140; ++raw) {
        sample.rightX = raw;
        fresh = true;
        assert(readPad(next));
        assert(next.rightX == (raw - 128) * 256);
        assert(next.packet == previous.packet + 1);
        previous = next;
    }
    assert(readPad(next) && next.packet == previous.packet);
    sample.buttons = 0x100000;
    fresh = true;
    assert(readPad(next) && next.buttons == kBack);
    sample.buttons = 0;
    fresh = true;
    assert(readPad(previous) && previous.buttons == 0 && previous.packet == next.packet + 1);
    sample.rightY = 0;
    fresh = true;
    assert(readPad(next) && next.rightY == 32767);
    std::puts("PS5 pad: gradual stick motion, idle packets, touchpad press/release and axis limits pass.");
}
