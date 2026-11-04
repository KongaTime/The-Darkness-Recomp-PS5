// PS5 controller backend.
//
// NativeInput reads the physical pad through ControllerApi, which is shaped
// like the XInput calls. This file fills that same shape from the console's pad
// API, and kernel_imports.cpp bridges it into XamInputGetState, so the guest's
// own input code reads a controller exactly where it read one on Windows.
//
// Only the PS5 build compiles this file. The Windows build keeps XInput.
#if !defined(_WIN32)

#include "input_ps5.h"

#include <cstdlib>
#include <mutex>

extern "C" {
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void* params);
int scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int scePadRead(int32_t handle, void* samples, int32_t capacity);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
}

namespace DarkRecomp::Native::Ps5Pad {
namespace {

// PS5 button bits, from the console's numbering.
enum : uint32_t {
    kPadL3 = 0x000002,
    kPadR3 = 0x000004,
    kPadOptions = 0x000008,
    kPadUp = 0x000010,
    kPadRight = 0x000020,
    kPadDown = 0x000040,
    kPadLeft = 0x000080,
    kPadL1 = 0x000400,
    kPadR1 = 0x000800,
    kPadTriangle = 0x001000,
    kPadCircle = 0x002000,
    kPadCross = 0x004000,
    kPadSquare = 0x008000,
    kPadTouchpad = 0x100000,
};

// Stick axes are 0..255 centred at 128. XInput wants signed 16-bit, positive
// up and right.
int16_t stickAxis(uint8_t raw, bool invert) {
    int value = (static_cast<int>(raw) - 128) * 256;
    // Invert before clamping: the full-up position is -32768, and negating it
    // inside a 16-bit return would wrap to full-down.
    if (invert) value = -value;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return static_cast<int16_t>(value);
}

// The pad's own bits onto XInput's: A/B/X/Y are Cross/Circle/Square/Triangle,
// which is the layout the guest's prompts expect on a PlayStation pad.
uint16_t xinputButtons(uint32_t buttons) {
    uint16_t mapped = 0;
    if (buttons & kPadCross) mapped |= kA;
    if (buttons & kPadCircle) mapped |= kB;
    if (buttons & kPadSquare) mapped |= kX;
    if (buttons & kPadTriangle) mapped |= kY;
    if (buttons & kPadOptions) mapped |= kStart;
    if (buttons & kPadTouchpad) mapped |= kBack;
    if (buttons & kPadUp) mapped |= kDpadUp;
    if (buttons & kPadDown) mapped |= kDpadDown;
    if (buttons & kPadLeft) mapped |= kDpadLeft;
    if (buttons & kPadRight) mapped |= kDpadRight;
    if (buttons & kPadL1) mapped |= kLeftShoulder;
    if (buttons & kPadR1) mapped |= kRightShoulder;
    if (buttons & kPadL3) mapped |= kLeftThumb;
    if (buttons & kPadR3) mapped |= kRightThumb;
    return mapped;
}

// The last sample and what the guest was last told, so an idle pad stays
// connected and its packet number changes only with the state.
struct Cache {
    std::mutex mutex;
    bool haveSample = false;
    PadSample sample{};
    bool haveState = false;
    PadState state{};
};

Cache& cache() {
    static Cache shared;
    return shared;
}

// Opened once, on the first poll. The function-local static's initialisation
// is thread-safe, so two guest threads polling at once cannot both open it.
int32_t openPad() {
    static const int32_t handle = [] {
        sceUserServiceInitialize(nullptr);
        scePadInit();
        // The pad belongs to a login user, not to slot 0. Fall back to user 0
        // where no user service answer is available (tests, odd boot order).
        int32_t user = 0;
        if (sceUserServiceGetInitialUser(&user) != 0 || user <= 0) user = 0;
        // type 0 = standard pad, index 0 = the first pad on that user.
        const int32_t opened = scePadOpen(user, 0, 0, nullptr);
        return opened >= 0 ? opened : scePadGetHandle(user, 0, 0);
    }();
    return handle;
}

} // namespace

bool readPad(PadState& state) {
    const int32_t handle = openPad();
    if (handle < 0) return false;

    // One poller drains the sample queue at a time; a second one must not take
    // the samples this one has not seen yet.
    Cache& shared = cache();
    std::lock_guard lock(shared.mutex);
    PadSample samples[64];
    const int count = scePadRead(handle, samples, 64);
    if (count > 0) {
        shared.sample = samples[count - 1];
        shared.haveSample = true;
    }
    if (!shared.haveSample || shared.sample.connected == 0) return false;

    PadState next;
    next.buttons = xinputButtons(shared.sample.buttons);
    next.leftTrigger = shared.sample.l2;
    next.rightTrigger = shared.sample.r2;
    next.leftX = stickAxis(shared.sample.leftX, false);
    next.leftY = stickAxis(shared.sample.leftY, true);
    next.rightX = stickAxis(shared.sample.rightX, false);
    next.rightY = stickAxis(shared.sample.rightY, true);

    bool changed = !shared.haveState || next.buttons != shared.state.buttons ||
                   next.leftTrigger != shared.state.leftTrigger ||
                   next.rightTrigger != shared.state.rightTrigger;
    if (!changed) {
        // A packet must change whenever the reported state changes. Filtering
        // each poll against the previous raw sample loses gradual stick motion:
        // every small step can stay below the threshold indefinitely. The game
        // already applies its own stick deadzones, as with XInput on Windows.
        changed = next.leftX != shared.state.leftX || next.leftY != shared.state.leftY ||
                  next.rightX != shared.state.rightX || next.rightY != shared.state.rightY;
    }
    next.packet = changed ? shared.state.packet + 1 : shared.state.packet;

    shared.state = next;
    shared.haveState = true;
    state = next;
    return true;
}

} // namespace DarkRecomp::Native::Ps5Pad

#endif // !defined(_WIN32)
