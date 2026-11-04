// Native features the console title does not have yet.
//
// The AOT output calls the Windows build's native hooks directly at patched
// guest sites (the "MidAsmHook" functions). Windows defines them in the feature
// files that own their state: FOV, mouse look, the developer tools, the render
// trace. Those files own a display, a Memory instance and a D3D11 device, and
// are not part of this target yet.
//
// A hook that does nothing is not a silent behaviour change. Each of these
// carries a *native setting* into the guest, and every one of them starts by
// checking whether that setting is armed; with nothing armed they already
// return without touching a register. Doing nothing therefore leaves the
// guest's own values in place, which is the unmodified game's behaviour.
//
// They are listed here by name so the gap is visible, and so porting a feature
// means deleting its line.
#include "ppc_context.h"

#include <cstdio>
#include <vector>

namespace {

// One line per absent feature, printed the first time the guest reaches it.
void reportOnce(const char* hook) {
    static std::vector<const char*> reported;
    for (const char* seen : reported) {
        if (seen == hook) return;
    }
    reported.push_back(hook);
    std::printf("[Title] Not ported: %s (guest values left as they are)\n", hook);
    std::fflush(stdout);
}

} // namespace

// --- runtime/native/fov_camera.cpp: the native field of view ---------------

void ConfigureGameplayFovBaselineMidAsmHook(PPCRegister& state, PPCRegister& value) {
    (void)state;
    (void)value;
    reportOnce("ConfigureGameplayFovBaselineMidAsmHook");
}

void ApplyGameplayFovMidAsmHook(PPCRegister& state, PPCRegister& viewport, PPCRegister& client,
                                PPCRegister& message, PPCRegister& fov) {
    (void)state;
    (void)viewport;
    (void)client;
    (void)message;
    (void)fov;
    reportOnce("ApplyGameplayFovMidAsmHook");
}

void CopyGameplayFovBaselineMidAsmHook(PPCRegister& destination, PPCRegister& source) {
    (void)destination;
    (void)source;
    reportOnce("CopyGameplayFovBaselineMidAsmHook");
}

void InitializeGameplayFovBaselineMidAsmHook(PPCRegister& state) {
    (void)state;
    reportOnce("InitializeGameplayFovBaselineMidAsmHook");
}

void ForgetGameplayFovBaselineMidAsmHook(PPCRegister& component) {
    (void)component;
    reportOnce("ForgetGameplayFovBaselineMidAsmHook");
}

// --- runtime/native/mouse_look.cpp: native mouse look ---------------------

void ApplyNativeMouseLookMidAsmHook(PPCRegister& client, PPCRegister& stack, PPCRegister& pitch,
                                    PPCRegister& yaw) {
    (void)client;
    (void)stack;
    (void)pitch;
    (void)yaw;
    reportOnce("ApplyNativeMouseLookMidAsmHook");
}

// --- runtime/native/developer_*: the developer tools ----------------------

void ApplyDeveloperInvincibilityMidAsmHook(PPCRegister& actor, PPCRegister& state,
                                           PPCRegister& flags) {
    (void)actor;
    (void)state;
    (void)flags;
    reportOnce("ApplyDeveloperInvincibilityMidAsmHook");
}

void ApplyDeveloperNoclipMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& mode) {
    (void)actor;
    (void)state;
    (void)mode;
    reportOnce("ApplyDeveloperNoclipMidAsmHook");
}

void ApplyDeveloperNoclipStateMidAsmHook(PPCRegister& state, PPCRegister& mode) {
    (void)state;
    (void)mode;
    reportOnce("ApplyDeveloperNoclipStateMidAsmHook");
}

void ApplyDeveloperNoclipCollisionMidAsmHook(PPCRegister& descriptor, PPCRegister& active) {
    (void)descriptor;
    (void)active;
    reportOnce("ApplyDeveloperNoclipCollisionMidAsmHook");
}

void ApplyDeveloperHorizontalSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state,
                                             PPCRegister& forward, PPCRegister& sidestep) {
    (void)actor;
    (void)state;
    (void)forward;
    (void)sidestep;
    reportOnce("ApplyDeveloperHorizontalSpeedMidAsmHook");
}

void ApplyDeveloperForwardSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state,
                                          PPCRegister& forward) {
    (void)actor;
    (void)state;
    (void)forward;
    reportOnce("ApplyDeveloperForwardSpeedMidAsmHook");
}

// --- renderer: the preview's clear tracking and the GPU fence --------------

void ObserveEngineClearRegionMidAsmHook(PPCRegister& device, PPCRegister& flags, PPCRegister& color,
                                        PPCRegister& depth, PPCRegister& stencil,
                                        PPCRegister& left, PPCRegister& top, PPCRegister& right,
                                        PPCRegister& bottom) {
    (void)device;
    (void)flags;
    (void)color;
    (void)depth;
    (void)stencil;
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    reportOnce("ObserveEngineClearRegionMidAsmHook");
}

// kernel.cpp's fence is already empty on Windows: the guest's commands complete
// in order there, and the same holds here.
void CompleteGpuFenceMidAsmHook() {}
