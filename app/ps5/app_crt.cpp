// The Darkness on PS5 - the title module's entry point.
//
// The console starts a title as a *module*: the loader maps the eboot and calls
// its entry with the process parameters it wrote into the module's parameter
// segment, plus a teardown callback of its own. The SDK's crt1.o is a *process*
// startup instead - it reads argc/argv from the stack a launcher set up - and a
// module entered through it is what the console refused on 2026-10-24:
//
//     [GAME] started: PPSA99301 pid=167
//     [MDBG] fatal error for PPSA99301:
//     0xa0020102 (PRX_SCE_MODULE_LOAD_ERROR)
//     thread name: eboot.bin
//
// before a single instruction of this title ran (/data/darkness-ps5/boot.log,
// which main's first line writes, was never created). The titles that do load
// on this console - PS5_Vulkan's apps among them - enter through their own
// startup like this one, which is why their modules carry DT_INIT/DT_FINI and
// an entry that takes the console's parameters.
//
// What it does, in the order the loader expects: put the FP unit in IEEE mode
// (the console starts a title flushing denormals), hand the process parameters
// to the console's libc, run the static constructors, run main, then run the
// destructors and let libc end the process.
//
// _init_env is libSceLibcInternal's, ps5_fp_ieee is the SDK platform layer's;
// both are already linked (see CMakeLists.txt).

#include <cstddef>
#include <cstdint>

using Initializer = void (*)();
using Destructor = void (*)();

extern "C" {
    // libSceLibcInternal: installs the process parameters for the C runtime
    // (TLS, heap, argument strings) before any libc call is made.
    void _init_env(void* process_parameters);
    // The platform layer's IEEE setup; the console boots a title with
    // flush-to-zero and denormals-are-zero on, and the engine's float math
    // (and RADV's) is compiled for the IEEE state.
    void ps5_fp_ieee(void);
    int atexit(Destructor callback);
    [[noreturn]] void exit(int status);

    [[noreturn]] void _start(void* process_parameters, Destructor loader_teardown);
    void _init();
    void _fini();

    int application_main(int argc, char** argv, char** environment) __asm__("main");

    // The layout script (ps5-pie.ld) places these around the arrays the
    // compiler empties the module's constructors into.
    extern Initializer __preinit_array_start[] __attribute__((weak));
    extern Initializer __preinit_array_end[] __attribute__((weak));
    extern Initializer __init_array_start[] __attribute__((weak));
    extern Initializer __init_array_end[] __attribute__((weak));
    extern Initializer __fini_array_start[] __attribute__((weak));
    extern Initializer __fini_array_end[] __attribute__((weak));

    // crtbegin.o normally defines this; with -nostartfiles it is ours. C++
    // statics register their destructors against its address.
    void* __dso_handle = nullptr;
}

namespace {

void runForward(Initializer* first, Initializer* last) {
    if (first == nullptr || last == nullptr) return;
    while (first != last) (*first++)();
}

void runBackward(Initializer* first, Initializer* last) {
    if (first == nullptr || last == nullptr) return;
    while (last != first) (*--last)();
}

} // namespace

// The SDK's libc hands dlopen and friends to the kernel loader through these
// weak hooks, which nothing in a title's import set exports: the web kernel
// module has them, libkernel does not. Left undefined, the module converter
// refuses the image ("no public SDK stub exports required symbol __dlopen"),
// and the loader would have a symbol no module can satisfy. This title never
// opens a module at run time - its libc and libc++ are linked in, and dladdr is
// bound to the platform layer - so the hooks report failure instead.
extern "C" __attribute__((weak, visibility("default"))) void* __dlopen(const char* path,
                                                                     int flags) {
    (void)path;
    (void)flags;
    return nullptr;
}

extern "C" __attribute__((weak, visibility("default"))) void* __dlsym(void* handle,
                                                                    const char* name) {
    (void)handle;
    (void)name;
    return nullptr;
}

extern "C" __attribute__((weak, visibility("default"))) int __dlclose(void* handle) {
    (void)handle;
    return -1;
}

extern "C" __attribute__((weak, visibility("default"))) char* __dlerror() {
    return nullptr;
}

extern "C" __attribute__((weak, visibility("default"))) int __dladdr(const void* address,
                                                                    void* info) {
    (void)address;
    (void)info;
    return 0;
}

// The loader's teardown runs after main's destructors, in registration order
// reversed: _fini first, then the console's own callback.
extern "C" void _init() {
    runForward(__preinit_array_start, __preinit_array_end);
    runForward(__init_array_start, __init_array_end);
}

extern "C" void _fini() {
    runBackward(__fini_array_start, __fini_array_end);
}

// Weak so a build that wants to observe main's return can define it.
extern "C" __attribute__((weak)) void catchReturnFromMain(int status) {
    (void)status;
}

extern "C" [[noreturn]] __attribute__((visibility("default"))) void
_start(void* process_parameters, Destructor loader_teardown) {
    // The parameters start with argc, then argv as 64-bit pointers, which is
    // the shape the console's loader leaves in the module's parameter segment.
    const auto* parameters = static_cast<const std::uint8_t*>(process_parameters);
    const int argc = process_parameters != nullptr ? *static_cast<const int*>(process_parameters) : 0;
    char** argv = parameters != nullptr
        ? reinterpret_cast<char**>(const_cast<std::uint8_t*>(parameters) + sizeof(std::uint64_t))
        : nullptr;

    ps5_fp_ieee();
    _init_env(process_parameters);
    if (loader_teardown != nullptr) atexit(loader_teardown);
    atexit(_fini);
    _init();

    const int status = application_main(argc, argv, nullptr);
    catchReturnFromMain(status);
    exit(status);
}
