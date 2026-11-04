#pragma once
// The guest context of the thread that is running guest code.
//
// runtime.cpp owns and sets this on Windows (the engine thread and every audio
// worker). The console title sets it on the thread that enters the guest. It is
// declared apart from runtime.h so the console's render and guest layers can
// name it without pulling in the host runtime's display, audio and archive
// types.
struct PPCContext;

namespace DarkRecomp::Native {
#if defined(_WIN32)
// The host runs guest code on the engine thread and on every audio worker at
// once, so each thread carries its own context.
extern thread_local PPCContext* currentContext;
#else
// The console's emulated TLS gives an externally visible thread_local a
// dynamic-initialisation symbol that the title's module layout cannot resolve,
// but a function-local one is fine. The runtime runs guest code on several
// threads at once, as on Windows, so the name stays per thread and reaches
// that slot (runtime.cpp) through an accessor.
PPCContext*& currentContextSlot();
#define currentContext currentContextSlot()
#endif
}
