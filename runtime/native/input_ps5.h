#pragma once
// The console's pad, as the guest's input path sees it.
//
// The Windows build reads XInput through NativeInput (input.cpp). The console
// reads ScePad and presents the same XInput-shaped state here, so
// kernel_imports.cpp's XamInputGetState keeps its guest serialization and the
// guest's own input code is untouched.
#include <cstdint>

#if !defined(_WIN32)

namespace DarkRecomp::Native::Ps5Pad {

// The pad sample ScePadRead fills, at the layout the Edge of Time port uses.
// These are the console's units, not XInput's: 8-bit stick axes and triggers,
// and the pad's own button bits.
struct PadSample {
    uint32_t buttons;
    uint8_t leftX, leftY, rightX, rightY, l2, r2;
    uint8_t reserved[66];
    int32_t connected;
    uint64_t timestampUs;
    uint8_t extension[16];
    uint8_t connectedCount;
    uint8_t remaining[15];
};
static_assert(sizeof(PadSample) == 120, "PS5 pad samples are 120 bytes");

// XInput's button layout, which is what the guest reads.
enum : uint16_t {
    kDpadUp = 0x0001,
    kDpadDown = 0x0002,
    kDpadLeft = 0x0004,
    kDpadRight = 0x0008,
    kStart = 0x0010,
    kBack = 0x0020,
    kLeftThumb = 0x0040,
    kRightThumb = 0x0080,
    kLeftShoulder = 0x0100,
    kRightShoulder = 0x0200,
    kA = 0x1000,
    kB = 0x2000,
    kX = 0x4000,
    kY = 0x8000,
};

// The pad in the shape XInput reports: 16-bit signed sticks (positive Y is
// up), 8-bit triggers and the button bits above.
struct PadState {
    uint32_t packet = 0; // changes only when the state does, as XInput's does
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};

// Samples the pad. Returns false when no pad is attached, leaving `state`
// alone. A poll with no new sample keeps the previous state: ScePadRead
// reports only new samples, so returning a disconnect there would make an idle
// pad vanish between frames.
bool readPad(PadState& state);

} // namespace DarkRecomp::Native::Ps5Pad

#endif // !defined(_WIN32)
