// The two guest-runtime entry points the AOT code calls, for the console.
//
// The Windows build gets these from runtime.cpp, which owns the display and the
// exception filter. The console title needs only the failure report and the
// timebase, so it carries its own rather than porting that whole file.
#include "ppc_context.h"

#include "runtime/native/guest_context.h"
#include "runtime/native/timebase_scale.h"

#include <ps5platform/klog.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace DarkRecomp::Native {
// runtime.cpp owns this on Windows, where it also sets it for the engine
// thread and every audio worker. The console title sets it before it enters the
// guest; the AOT code and the render layer's diagnostics read it.
PPCContext* currentContext = nullptr;
} // namespace DarkRecomp::Native

namespace {

constexpr uint64_t kTimebaseFrequency = 49875000;

// One line of the failure report. stderr reaches the kernel log through the
// platform's relay thread, and the abort that follows a failure ends the process
// before that thread has read the pipe: the report of 2026-10-24's first guest
// failure was lost that way. Each line therefore also goes to the kernel log
// directly.
__attribute__((format(printf, 1, 2))) void report(const char* format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    std::strncat(line, "\n", sizeof line - std::strlen(line) - 1);
    ::sceKernelDebugOutText(0, line);
}

// guest_support's backtrace: the recompiled code is a flat call tree, so
// listing the recent guest entries is what actually locates a failure.
void printGuestContext(const PPCContext& ctx) {
    report("guest function=0x%08X lr=0x%08X sp=0x%08X r3=0x%08X r4=0x%08X",
           ctx.lastFunction, uint32_t(ctx.lr), ctx.r1.u32, ctx.r3.u32, ctx.r4.u32);
    report("r5=0x%08X r6=0x%08X r7=0x%08X r13=0x%08X r31=0x%08X",
           ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r13.u32, ctx.r31.u32);
    // 64 entries at nine characters each fit the line.
    char entries[64 * 9 + 1] = "";
    const uint32_t count = ctx.traceIndex < 64 ? ctx.traceIndex : 64;
    for (uint32_t i = 0; i < count; ++i) {
        std::snprintf(entries + i * 9, sizeof entries - i * 9, " %08X",
                      ctx.trace[(ctx.traceIndex - count + i) & 63]);
    }
    report("recent guest entries:%s", entries);
}

} // namespace

// Called by a translated function whose import has no implementation, and by
// an indirect call to an address that was never translated. There is nothing
// to recover: report where the guest was and stop.
[[noreturn]] void PPCRecompFailure(const PPCContext& ctx, uint32_t address, const char* reason) {
    report("[UNSUPPORTED] guest=0x%08X: %s", address, reason ? reason : "(no reason)");
    printGuestContext(ctx);
    std::abort();
}

// The guest's timebase runs at 49,875,000 Hz. The console's monotonic clock is
// nanoseconds, which the same scale converts.
uint64_t PPCQueryTimebase() {
    static const DarkRecomp::Native::TimebaseScale<kTimebaseFrequency> scale(1000000000ull);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return scale(uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec));
}
