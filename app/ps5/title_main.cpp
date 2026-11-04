// The Darkness on PS5: the title's entry point.
//
// The Windows build's main is native_main.cpp. It owns a window, a D3D11
// device, XAudio2, XInput and the native menus, then hands the guest its
// address space. The console has RADV and no window: this title opens the
// display, brings up the graphics backend (renderer/ps5/graphics_backend.cpp),
// and then runs the guest on its own thread while the main thread consumes the
// engine's render command stream and presents it.
//
// That split is what the engine expects. The guest publishes a frame's commands
// through simple_mesh.h's preview queue and waits for a consumer at the end of
// each frame (setPreviewFrameBackpressure); with no consumer, it stopped at the
// first draw call. The main thread is that consumer.
#include "display.h"
#include "graphics_backend.h"
#include "video_preferences.h"
#include "cpu_sampling.h"
#include "game_setup.h"
#include "renderer/engine/engine_performance.h"
#include "renderer/engine/scene_work.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/world_mesh.h"
#include "ppc_context.h"
#include "runtime/native/display_mode.h"
#include "runtime/native/guest_context.h"
#include "runtime/native/runtime.h"
#include "runtime/native/storage.h"
#include "runtime/native/video_settings_menu.h"
#include "runtime/native/graphics_settings.h"
#include "runtime/native/fov_settings.h"
#include "runtime/native/language_settings.h"

#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <streambuf>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ps5platform/heap.h>
#include <ps5platform/klog.h>

#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

PPC_EXTERN_FUNC(_xstart);

extern "C" PPC_FUNC(__imp__sub_820FE3B0);
extern "C" PPC_FUNC(__imp__sub_827A9F20);
PPC_FUNC(sub_827A9F20) {
    const uint32_t provider = ctx.r3.u32;
    __imp__sub_827A9F20(ctx, base);
    static unsigned reports = 0;
    if (reports++ < 24 && provider >= 0x10000 && uint64_t(provider) + 96 < PPC_MEMORY_SIZE) {
        auto& memory = *DarkRecomp::Native::memory;
        const uint32_t slots = memory.read32(provider + 76);
        std::fprintf(stderr, "[SaveProvider] provider=%08X slot=%d slots=%08X count=%u queue=%08X\n",
                     provider, ctx.r3.s32, slots,
                     slots && uint64_t(slots) + 4 < PPC_MEMORY_SIZE ? memory.read32(slots) : 0,
                     memory.read32(provider + 80));
    }
}
PPC_FUNC(sub_820FE3B0) {
    const uint32_t application = ctx.r3.u32;
    __imp__sub_820FE3B0(ctx, base);
    static unsigned reports = 0;
    if (reports++ < 24 && application >= 0x10000 && uint64_t(application) + 4908 < PPC_MEMORY_SIZE) {
        auto& memory = *DarkRecomp::Native::memory;
        std::fprintf(stderr, "[ProfileFlow] application=%08X flags=%08X session=%u operation=%08X device=%08X result=%08X pending=%08X,%08X\n",
                     application, memory.read32(application + 252), unsigned(base[application + 1820]),
                     memory.read32(application + 3728), memory.read32(application + 4876), ctx.r3.u32,
                     memory.read32(application + 4896), memory.read32(application + 4900));
    }
}

// D3D's GPU-hang report, reached from its ring-buffer wait (sub_8285D568)
// through the GPU fence hook. The console reaches it before the first frame
// and Windows never does: log the device's ring state - the read pointer the
// GPU would write back, its value, and that physical word through each of the
// physical windows - then report as the game does.
extern "C" PPC_FUNC(__imp__sub_8286CA60);
PPC_FUNC(sub_8286CA60) {
    namespace Native = DarkRecomp::Native;
    const uint32_t device = ctx.r3.u32;
    if (Native::memory && device >= 0x10000 && uint64_t(device) + 16384 <= PPC_MEMORY_SIZE) {
        const uint32_t pointer = Native::memory->read32(device + 10896);
        std::fprintf(stderr,
                     "[GpuRing] device=0x%08X rptrAt=0x%08X end(10908)=0x%08X owner(10888)=0x%08X "
                     "flag(10996)=0x%08X caller lr=0x%08X\n",
                     device, pointer, Native::memory->read32(device + 10908), Native::memory->read32(device + 10888),
                     Native::memory->read32(device + 10996), uint32_t(ctx.lr));
        if (pointer >= 0x10000 && pointer < 0xfffff000u) {
            const uint32_t physical = Native::Memory::physicalAddress(pointer);
            std::fprintf(stderr, "[GpuRing] rptr=0x%08X physical=0x%08X A=0x%08X C=0x%08X E=0x%08X\n",
                         Native::memory->read32(pointer), physical, Native::memory->read32(0xa0000000u + physical),
                         Native::memory->read32(0xc0000000u + physical),
                         physical >= 0x1000 ? Native::memory->read32(0xe0000000u + physical - 0x1000) : 0u);
        }
        std::fflush(stderr);
    }
    __imp__sub_8286CA60(ctx, base);
}

namespace {

// The title's folder on the console. The game itself is not shipped in it; it
// reads the player's own dump from here.
constexpr const char* kDefaultGameDirectory = "/app0/Darkness";
// Frame dumps use the title's writable application-data mount. That mount is a
// disk image the console's FTP service cannot see, so the shared data partition
// is preferred when the title may write it: a dump there can be pulled off the
// console and inspected. The application-data mount stays the fallback.
constexpr const char* kHostDumpDirectory = "/data/darkness-ps5/frames";
constexpr const char* kDefaultDumpDirectory = "/download0/darkness-ps5/frames";

const char* selectDumpDirectory() {
    std::error_code failure;
    std::filesystem::create_directories(kHostDumpDirectory, failure);
    return failure ? kDefaultDumpDirectory : kHostDumpDirectory;
}

void usage(const char* program) {
    std::printf(
        "The Darkness - PS5 title\n"
        "\n"
        "  %s [--game-dir PATH] [--frames N] [--pipeline-test N]\n"
        "     [--dump-frames N] [--dump-dir PATH]\n"
        "\n"
        "  Reads default.xex from the game directory (default %s). The dump must\n"
        "  match a revision in config/game_revisions.toml; a different file is\n"
        "  rejected before the guest starts.\n"
        "\n"
        "  --frames N presents N solid frames before the guest starts (default 0;\n"
        "             useful for checking the display on its own).\n"
        "  --pipeline-test N draws N checkerboard quads through the renderer's\n"
        "             pipeline (default 0; the renderer's own hardware check).\n"
        "  --dump-frames N writes the first N presented frames to PNG files\n"
        "             (default 0). Each file is about 25 MB at 3840x2160.\n"
        "  --dump-dir PATH  where those frames go (default %s when it is\n"
        "             writable, else %s).\n",
        program, kDefaultGameDirectory, kHostDumpDirectory, kDefaultDumpDirectory);
}

// Draws `frames` frames through the renderer's own pipeline: an RGBA8 texture
// on a quad, uploaded, sampled, blended and presented exactly the way an
// engine mesh is. This is the backend's hardware check, independent of the
// guest: a solid clear proves the display, this proves the renderer.
bool presentPipelineTestFrames(DarkRecomp::Ps5::GraphicsBackend& graphics,
                               DarkRecomp::Ps5::Display& display, int frames) {
    // An 8x8 two-colour checker, so a stretched or wrongly sampled texture is
    // visible rather than hidden behind a solid fill.
    auto image = std::make_shared<DarkRecomp::ColorImage>();
    image->width = image->height = 8;
    image->pixels.resize(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t* texel = image->pixels.data() + (y * 8 + x) * 4;
            const bool light = ((x / 2) + (y / 2)) % 2 == 0;
            texel[0] = light ? 0xE0 : 0x30;
            texel[1] = light ? 0xC0 : 0x20;
            texel[2] = light ? 0x60 : 0x18;
            texel[3] = 0xFF;
        }
    }

    DarkRecomp::SimpleMesh mesh;
    mesh.colorTexture = image;
    // A centred square in clip space: the projection is the identity, which is
    // what a UI quad authored in screen coordinates carries.
    mesh.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    mesh.vertices = {
        {{-0.8f, -0.8f, 0.0f}, {0.0f, 1.0f}, {1, 1, 1, 1}},
        {{0.8f, -0.8f, 0.0f}, {1.0f, 1.0f}, {1, 1, 1, 1}},
        {{0.8f, 0.8f, 0.0f}, {1.0f, 0.0f}, {1, 1, 1, 1}},
        {{-0.8f, 0.8f, 0.0f}, {0.0f, 0.0f}, {1, 1, 1, 1}},
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};

    const std::vector<DarkRecomp::SimpleMesh> frame{mesh};
    DarkRecomp::Native::PreviewFramePart whole{true, true};
    for (int i = 0; i < frames; ++i) {
        graphics.render(frame, whole);
        if (graphics.error()) {
            std::printf("[Graphics] Pipeline test frame failed: %s\n", graphics.error());
            return false;
        }
    }
    (void)display;
    return true;
}

// Presents `frames` solid frames: the smallest true statement that the display,
// the swapchain and RADV on the console work, independent of the guest.
bool presentTestFrames(DarkRecomp::Ps5::Display& display, int frames) {
    for (int i = 0; i < frames; ++i) {
        // A slow sweep from the game's palette so a stuck image is obvious.
        const float t = frames > 1 ? float(i) / float(frames - 1) : 0.0f;
        if (!display.PresentClear(0.06f + 0.30f * t, 0.04f + 0.10f * t, 0.09f + 0.22f * (1.0f - t))) {
            std::printf("[Display] Present failed: %s\n", display.error());
            if (!display.RebuildSwapchain()) {
                std::printf("[Display] Swapchain rebuild failed: %s\n", display.error());
                return false;
            }
        }
    }
    return true;
}

// The Shell keeps a title's stdout in memory, so a console launch that dies
// leaves nothing to read. These milestones go to the console's kernel log
// (sceKernelDebugOutText, which the klog service on port 3232 streams); a title
// sees only its own sandbox, so the kernel log is the one readout that always
// arrives - a file the title writes is not always readable from outside it.
// The console's pools for a title's large allocations. Plain anonymous
// mappings draw on a small allowance (see traceMemoryLimits below); the
// flexible pool is the one games take their megabytes from.
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t* size);
extern "C" int sceKernelMapFlexibleMemory(void** address, size_t length, int protection, int flags);
extern "C" int sceKernelReleaseFlexibleMemory(void* address, size_t length);
// The pool a title's large allocations come from, and the call that says how
// much of it is left. RADV allocates its buffers here too, so its own OOM is
// the same pool.
extern "C" int64_t sceKernelGetDirectMemorySize(void);
extern "C" int sceKernelAvailableDirectMemorySize(int64_t searchStart, int64_t searchEnd, size_t alignment,
                                                  int64_t* physicalStart, size_t* available);
// The Shell draws the title's splash (sce_sys/pic1) over the app from launch
// until the app says it is ready, and a title that never says so sits behind it
// with its game running underneath - which is what a launch with pic1 looked
// like on 2026-10-25: sound, no picture, the art still on the panel. Hiding it
// once the game has presented a frame of its own keeps the launch animation and
// then hands the panel over, instead of leaving the game hidden behind it.
extern "C" int sceSystemServiceHideSplashScreen(void);

// A driver's messages go to standard error, and nothing reads a title's
// standard error on the console. The platform's capture moves stderr onto a
// pipe whose reader writes each line to klog (a title cannot dup2: the console
// refuses it with EPERM), so RADV's own words about a refused call arrive in
// the same log as this title's milestones.
// std::cerr does not follow: the platform moves the stdio stream, and libc++'s
// cerr keeps writing where stderr pointed when it was constructed, so the file
// system's "[FS]" lines never arrived (2026-10-24). This buffer writes cerr
// through the moved stream.
class StderrBuffer : public std::streambuf {
protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) return traits_type::not_eof(character);
        return std::fputc(character, stderr) == EOF ? traits_type::eof() : character;
    }
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        return static_cast<std::streamsize>(std::fwrite(text, 1, static_cast<size_t>(count), stderr));
    }
    int sync() override { return std::fflush(stderr) == 0 ? 0 : -1; }
};

void forwardOutputToKernelLog() {
    ps5_klog_capture_stderr("[Title] ");
    static StderrBuffer buffer;
    std::cerr.rdbuf(&buffer);
    // The kernel bridge reports through std::cout: the first calls to each
    // import ("[KernelTrace]") and every unimplemented one ("[Unimpl]"). Both
    // are capped per import, and they are what shows which call a stuck guest
    // made last, so they go to the kernel log as well.
    std::cout.rdbuf(&buffer);
}

void traceText(const char* text) {
    char line[512];
    std::snprintf(line, sizeof line, "[Darkness] %s", text);
    ::sceKernelDebugOutText(0, line);
}

__attribute__((format(printf, 1, 2))) void tracef(const char* format, ...) {
    char line[448];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    traceText(line);
}

void trace(const char* step) { traceText(step); }

// The console launch's memory budget, in the only place a console launch can
// report it: the kernel log. A 4 GB guest reservation and RADV's own
// allocations both failed with ENOMEM on 2026-10-24, and the limit that
// explains it (RLIMIT_AS, or an overcommit policy) is not visible from here
// any other way.
void traceMemoryLimits() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_AS, &limit) == 0) {
        tracef("rlimit as: cur %llu MiB, max %llu MiB",
               static_cast<unsigned long long>(limit.rlim_cur) >> 20,
               static_cast<unsigned long long>(limit.rlim_max) >> 20);
    }
    if (::getrlimit(RLIMIT_DATA, &limit) == 0) {
        tracef("rlimit data: cur %llu MiB, max %llu MiB",
               static_cast<unsigned long long>(limit.rlim_cur) >> 20,
               static_cast<unsigned long long>(limit.rlim_max) >> 20);
    }
    // The two pools a title has. Flexible memory backs ordinary allocations and
    // is 448 MiB configured; direct memory is the ~12 GiB pool the GPU and the
    // guest's address space come from, and RADV takes its buffers from it.
    const int64_t pool = ::sceKernelGetDirectMemorySize();
    tracef("direct memory pool: %lld MiB", static_cast<long long>(pool) >> 20);
    if (pool > 0) {
        int64_t start = -1;
        size_t directAvailable = 0;
        const int result = ::sceKernelAvailableDirectMemorySize(0, pool, 2u << 20, &start, &directAvailable);
        tracef("direct memory available: %llu MiB at %lld (ret 0x%x)",
               static_cast<unsigned long long>(directAvailable) >> 20, static_cast<long long>(start), result);
    }
    size_t flexible = 0;
    const int flexibleResult = ::sceKernelAvailableFlexibleMemorySize(&flexible);
    tracef("flexible memory: %llu MiB available (ret 0x%x)",
           static_cast<unsigned long long>(flexible) >> 20, flexibleResult);
    if (flexibleResult == 0) {
        void* guest = nullptr;
        const int mapped = ::sceKernelMapFlexibleMemory(&guest, PPC_MEMORY_SIZE,
                                                        PROT_READ | PROT_WRITE, 0);
        tracef("map 4 GiB flexible: ret 0x%x, address %p", mapped, guest);
        if (mapped == 0) ::sceKernelReleaseFlexibleMemory(guest, PPC_MEMORY_SIZE);
    }
    // Where the plain mapping stops fitting, in steps fine enough to tell a
    // fixed allowance apart from one the module's own 127 MB already ate.
    for (const unsigned long long mebibytes : {128ull, 192ull, 256ull, 320ull, 384ull, 448ull, 512ull}) {
        const size_t bytes = static_cast<size_t>(mebibytes) << 20;
        void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        tracef("mmap %llu MiB: %s (errno %d)", mebibytes,
               mapped == MAP_FAILED ? "failed" : "ok", errno);
        if (mapped != MAP_FAILED) ::munmap(mapped, bytes);
    }
}

// The title heap's ledger (ps5platform/heap.h): the range it reserved, what it
// has mapped out of direct memory, and how often it had to leave an allocation
// to libc's own private heap. It is what tells a driver's OOM apart from the
// pool being empty.
void traceHeap(const char* when) {
    struct ps5_heap_stats stats {};
    ps5_heap_stats(&stats);
    tracef("heap %s: range %p+%llu MiB, mapped %llu MiB, peak %llu MiB, libc fallbacks %llu, arenas %u",
           when, reinterpret_cast<void*>(stats.range_base),
           static_cast<unsigned long long>(stats.range_bytes) >> 20,
           static_cast<unsigned long long>(stats.mapped_bytes) >> 20,
           static_cast<unsigned long long>(stats.peak_bytes) >> 20, stats.libc_fallbacks, stats.arenas);
}

// Whether an allocation of this size succeeds right now, so a failure after a
// driver call can be told apart from a heap that was already refusing them.
void traceAllocations(const char* when) {
    for (const size_t mebibytes : {size_t{1}, size_t{8}, size_t{64}, size_t{256}}) {
        void* block = std::malloc(mebibytes << 20);
        tracef("malloc %zu MiB %s: %s", mebibytes, when, block ? "ok" : "failed");
        std::free(block);
    }
}

// The mutex primitives RADV's C11 threads layer builds its instance on. RADV's
// vkCreateInstance stopped at its first mutex with VK_ERROR_INITIALIZATION_FAILED
// (2026-10-25); these four calls say which one the console's libkernel refuses.
// The C11 layer RADV links (its own threads_posix.c), declared here so the
// probe calls the driver's copy: mtx_t is pthread_mutex_t for that build.
extern "C" int mtx_init(pthread_mutex_t* mutex, int type);
extern "C" int pthread_mutexattr_init(pthread_mutexattr_t* attribute);
extern "C" int pthread_mutexattr_settype(pthread_mutexattr_t* attribute, int type);
extern "C" int pthread_mutexattr_destroy(pthread_mutexattr_t* attribute);

void traceMutexPrimitives() {
    pthread_mutexattr_t attribute{};
    const int attributeInit = ::pthread_mutexattr_init(&attribute);
    // FreeBSD's types: 0 normal, 1 error-check, 2 recursive, 3 adaptive. The
    // console answered EINVAL for 0, so what it accepts is what matters.
    int setTypes[4] = {0, 0, 0, 0};
    for (int type = 0; type < 4; ++type) setTypes[type] = ::pthread_mutexattr_settype(&attribute, type);
    pthread_mutex_t withAttribute{};
    const int initWithAttribute = ::pthread_mutex_init(&withAttribute, &attribute);
    const int destroyAttribute = ::pthread_mutexattr_destroy(&attribute);
    pthread_mutex_t plain{};
    const int initPlain = ::pthread_mutex_init(&plain, nullptr);
    tracef("mutex: attr_init %d, settype 0/1/2/3 = %d/%d/%d/%d, attr_destroy %d, init(attr) %d, init(NULL) %d",
           attributeInit, setTypes[0], setTypes[1], setTypes[2], setTypes[3], destroyAttribute,
           initWithAttribute, initPlain);
    // The same call RADV makes, from the C11 layer it links: mtx_init(plain).
    // Called through the driver's own copy, so its answer here is its answer
    // there (2 means it refused, 4 is that build's thrd_success).
    for (const int type : {1, 0, 4, 6}) {
        pthread_mutex_t mutex{};
        tracef("mtx_init(%d) = %d", type, ::mtx_init(&mutex, type));
    }
}

} // namespace

int main(int argc, char** argv) {
    forwardOutputToKernelLog();
    std::fprintf(stderr, "stderr relay through the kernel log is live\n");
    std::fflush(stderr);
    // RADV's own logging, which reaches the console only through stderr: with
    // this the driver's init path is readable in the title's log.
    ::setenv("RADV_DEBUG", "startup", 0);
    tracef("main (argc %d)", argc);
    traceMemoryLimits();
    traceHeap("at start");
    traceAllocations("at start");
    traceMutexPrimitives();
    std::filesystem::path gameDirectory = kDefaultGameDirectory;
    bool explicitGameDirectory = false;
    int testFrames = 0;
    int pipelineFrames = 0;
    int dumpFrames = 0;
    std::filesystem::path dumpDirectory = selectDumpDirectory();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            gameDirectory = argv[++i];
            explicitGameDirectory = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            testFrames = std::atoi(argv[++i]);
            if (testFrames < 0) testFrames = 0;
        } else if (std::strcmp(argv[i], "--pipeline-test") == 0 && i + 1 < argc) {
            pipelineFrames = std::atoi(argv[++i]);
            if (pipelineFrames < 0) pipelineFrames = 0;
        } else if (std::strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            dumpFrames = std::atoi(argv[++i]);
            if (dumpFrames < 0) dumpFrames = 0;
        } else if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dumpDirectory = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::printf("[Title] Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    // The display first: opening it proves the console's output, RADV and the
    // swapchain before any of the guest's work, and a failure here is reported
    // as a display failure rather than as a hanging guest.
    const auto preferencesPath = std::filesystem::path(kConsoleDataRoot) / "video.ini";
    std::error_code preferencesError;
    std::filesystem::create_directories(preferencesPath.parent_path(), preferencesError);
    const auto preferences = DarkRecomp::Ps5::loadVideoPreferences(preferencesPath);
    DarkRecomp::Native::setGraphicsSettings(preferences.graphics);
    DarkRecomp::Native::setFieldOfViewSetting(float(preferences.fieldOfView));
    DarkRecomp::Native::initializeGameLanguageSetting(DarkRecomp::Native::GameLanguage(preferences.language));
    DarkRecomp::Native::initializeVideoSettingsMenu();
    DarkRecomp::Ps5::Display display;
    const bool displayOpen = display.Open("The Darkness");
    if (displayOpen) {
        tracef("display open: %ux%u", display.width(), display.height());
    } else {
        tracef("display failed: %s", display.error());
        traceHeap("after display");
        traceAllocations("after display");
    }
    DarkRecomp::Ps5::GraphicsBackend graphics;
    bool graphicsReady = false;
    if (!displayOpen) {
        std::printf("[Display] Cannot open: %s\n", display.error());
    } else {
        graphicsReady = graphics.Initialize(display);
        if (!graphicsReady) {
            std::printf("[Graphics] Cannot start: %s\n", graphics.error());
            tracef("graphics failed: %s", graphics.error());
        } else {
            tracef("graphics ready: %ux%u", display.width(), display.height());
            std::printf("[Graphics] %ux%u on RADV; simple meshes, glyph atlases and video\n",
                        display.width(), display.height());
            // Armed before the guest starts so the first frames the engine
            // publishes -- the boot logos and the front end -- are kept. It
            // reports its own refusal (an unreadable surface); being unable to
            // dump frames never stops the title.
            if (dumpFrames > 0) graphics.beginFrameDump(dumpDirectory.string(), dumpFrames);
        }
        if (pipelineFrames > 0 && graphicsReady) {
            std::printf("[Graphics] Drawing %d pipeline test frames.\n", pipelineFrames);
            std::fflush(stdout);
            if (!presentPipelineTestFrames(graphics, display, pipelineFrames)) {
                std::printf("[Graphics] The pipeline test did not present cleanly.\n");
            }
        }
        if (testFrames > 0) {
            std::printf("[Display] Presenting %d test frames.\n", testFrames);
            std::fflush(stdout);
            if (!presentTestFrames(display, testFrames)) {
                std::printf("[Display] The test frames did not present cleanly.\n");
            }
        }
    }
    std::fflush(stdout);

    // Import/menu runs before guest allocations or threads. Explicit developer
    // paths and pipeline diagnostics retain their original direct-start flow.
    if (graphicsReady && !explicitGameDirectory && !testFrames && !pipelineFrames) {
        try {
            if (!DarkRecomp::Ps5::runGameSetup(display, graphics, gameDirectory)) {
                return 0;
            }
        } catch (const std::exception& failure) {
            tracef("game setup failed: %s", failure.what());
            return 1;
        }
    }

    // The Windows build's runtime (runtime/native), started as native_main.cpp
    // starts it: the guest address space with the verified image, the main
    // guest thread, the kernel layer, the guest's video mode and the engine's
    // scene workers. The title's folder is read-only, so saves and the
    // runtime's own files go to the writable application-data mount.
    namespace Native = DarkRecomp::Native;
    std::error_code dataError;
    std::filesystem::create_directories(std::filesystem::path(kConsoleDataRoot) / "saves", dataError);
    Native::Storage::SetSaveRootOverride(std::filesystem::path(kConsoleDataRoot) / "saves");
    tracef("save root %s/saves: %s", kConsoleDataRoot,
           dataError ? dataError.message().c_str() : "directory ready");
    if (graphicsReady) Native::enableEnginePreview();
    std::unique_ptr<Native::Memory> addressSpace;
    PPCContext ctx{};
    try {
        addressSpace = std::make_unique<Native::Memory>();
        Native::memory = addressSpace.get();
        tracef("guest memory reserved at %p", static_cast<void*>(addressSpace->base()));
        addressSpace->load(gameDirectory);
        addressSpace->initThread(ctx);
        Native::initializeKernel();
    } catch (const std::exception& failure) {
        std::printf("[Title] Cannot start the game: %s\n", failure.what());
        std::printf("[Title] Put your own default.xex in %s\n", gameDirectory.c_str());
        tracef("startup failed: %s", failure.what());
        return 1;
    }
    // Preserve bounded guest allocations and scale native surfaces, as on PC.
    const unsigned renderScale = DarkRecomp::Ps5::renderScaleForHeight(preferences.graphics.renderHeight);
    const unsigned guestHeight = preferences.graphics.renderHeight / renderScale;
    const unsigned guestWidth = ((guestHeight * 16 / 9 + 3) / 4) * 4;
    if (!Native::setNativeVideoMode(guestWidth, guestHeight)) tracef("the guest video mode was refused");
    tracef("video: guest=%ux%u scale=%u output=%up filtering=%ux", guestWidth, guestHeight,
           renderScale, guestHeight * renderScale, preferences.graphics.anisotropyLevels);
    if (graphicsReady) Native::setPreviewFrameBackpressure(true, true, 3);
    Native::selectiveWorldDrawReset.store(graphicsReady,std::memory_order_relaxed);
    // Freeing each finished part's draw snapshots was a fifth of the render
    // thread's time (work/framerate-control-pool-cpu.txt); a worker does it.
    if (graphicsReady) Native::setPreviewBackgroundRetirement(true);
    Native::initializeSceneWorkers();
    // The Windows preview hands the decoded-texture cache its world renderer's
    // budget; left at the 256 MiB default, a level re-decoded thousands of
    // textures a window (run38: 77 s of decode per 30 s). Keep a whole level.
    Native::setPreviewTextureBudget(size_t(1536) * 1024 * 1024);
    // Per-draw clock calls and atomic counters distort frame times on the
    // console. Normal launches keep detailed profiling off; a marker opts in.
    std::error_code profileError;
    Native::profileEngineCpu = std::filesystem::exists(std::filesystem::path(kConsoleDataRoot) /
                                                      "profile-engine.enabled", profileError) && !profileError;

    trace("entering the guest");
    std::printf("[Title] Entering the guest.\n");
    std::fflush(stdout);

    // The engine thread. Windows gives every thread the executable's 64 MiB
    // stack reservation, and the translated engine relies on it; the Win32
    // layer allocates the console thread's stack from the title heap.
    std::atomic<bool> guestDone{false};
    struct GuestStart {
        PPCContext* ctx;
        uint8_t* base;
        std::atomic<bool>* done;
    } start{&ctx, addressSpace->base(), &guestDone};
    HANDLE guest = CreateThread(nullptr, 64u << 20, [](void* raw) -> DWORD {
        auto* start = static_cast<GuestStart*>(raw);
        Native::currentContext = start->ctx;
        Native::sampleEngineThread = true;
        DarkRecomp::Ps5::registerSampledThread(DarkRecomp::Ps5::SampledThread::guest);
        const int status = Native::runGuest(*start->ctx, start->base);
        std::fprintf(stderr, "[STOP] Guest execution ended with status %d.\n", status);
        start->done->store(true);
        return DWORD(status);
    }, &start, 0, nullptr);
    if (!guest) {
        tracef("cannot create the engine thread (error %u)", unsigned(GetLastError()));
        return 1;
    }
    std::thread cpuSampler([&] {DarkRecomp::Ps5::runCpuSampler(ctx,guestDone);});

    // A guest that stops making progress leaves a black screen and nothing in
    // the log (2026-10-24, after the file system resolved the game folder).
    // Every five seconds, say where the main guest thread is: the function it
    // last entered, its link register and the latest entries. The reads race
    // the guest thread, which is fine for a position report.
    // Every two seconds, from the start, with the memory the title has left:
    // the native runtime's guest space, thread stacks and the driver all draw
    // on direct memory, and a driver short of it faults on the GPU.
    std::thread watchdog([&] {
        uint32_t previousIndex = 0;
        while (!guestDone.load()) {
            const uint32_t index = ctx.traceIndex;
            char entries[16 * 9 + 1] = "";
            for (uint32_t i = 0; i < 16; ++i) {
                std::snprintf(entries + i * 9, sizeof entries - i * 9, " %08X",
                              ctx.trace[(index - 16 + i) & 63]);
            }
            struct ps5_heap_stats heap {};
            ps5_heap_stats(&heap);
            int64_t start = -1;
            size_t directAvailable = 0;
            ::sceKernelAvailableDirectMemorySize(0, ::sceKernelGetDirectMemorySize(), 2u << 20, &start,
                                                 &directAvailable);
            tracef("guest at 0x%08X lr=0x%08X entries+%u heap=%lluMiB direct-free=%lluMiB:%s", ctx.lastFunction,
                   uint32_t(ctx.lr), index - previousIndex,
                   static_cast<unsigned long long>(heap.mapped_bytes) >> 20,
                   static_cast<unsigned long long>(directAvailable) >> 20, entries);
            // D3D's GPU-hang check reads its clock from the current thread
            // object (+0x58) and the thread id (+0x14C), through PCR+0x100.
            const uint32_t thread = Native::memory->read32(0x7ff00100);
            const bool threadMapped = thread >= 0x10000 && thread < 0xfffff000u;
            tracef("main PCR thread=0x%08X clock(+0x58)=0x%08X id(+0x14C)=0x%08X", thread,
                   threadMapped ? Native::memory->read32(thread + 0x58) : 0,
                   threadMapped ? Native::memory->read32(thread + 0x14c) : 0);
            previousIndex = index;
            for (int i = 0; i < 20 && !guestDone.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // The render thread: drain the engine's frames and present them, in the
    // same shape as native_main.cpp's message loop. A 16 ms wait lets a block
    // of commands arrive together instead of spinning on an empty queue.
    std::vector<DarkRecomp::SimpleMesh> frame;
    // The first frames, and every hundredth, are logged around the render:
    // a GPU fault in the console's log then lines up with the frame that
    // submitted it.
    unsigned rendered = 0;
    bool captureRequested = false;
    bool splashHidden = false;
    auto nextCaptureCheck = std::chrono::steady_clock::now();
    // Where a frame's time goes: the renderer's own work against the wait for
    // the guest to publish the next frame. Without that split a console session
    // cannot tell a slow renderer from a slow guest, which is the first question
    // a frame-rate report asks.
    auto windowStart = std::chrono::steady_clock::now();
    double windowRenderMs = 0.0, windowRenderMaxMs = 0.0, windowIdleMs = 0.0;
    unsigned windowFrames = 0;
    // The part of the render time spent waiting for the GPU to finish the
    // previous frame (Display::BeginFrame), against recording on the CPU.
    double windowGpuWaitStart = display.gpuWaitMilliseconds();
    double windowAcquireStart = display.acquireWaitMilliseconds();
    double windowPresentStart = display.presentWaitMilliseconds();
    uint64_t windowPresentedStart = display.presentedFrames();
    uint64_t windowRealStart=graphics.frames(),windowGeneratedStart=graphics.generatedFrames();
    auto reportWindow = [&]() {
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - windowStart).count();
        const double span = (std::max)(elapsed, 1.0);
        const double gpuWait = display.gpuWaitMilliseconds() - windowGpuWaitStart;
        const double acquireWait = display.acquireWaitMilliseconds() - windowAcquireStart;
        const double presentWait = display.presentWaitMilliseconds() - windowPresentStart;
        const uint64_t presented = display.presentedFrames() - windowPresentedStart;
        const uint64_t real=graphics.frames()-windowRealStart,generated=graphics.generatedFrames()-windowGeneratedStart;
        windowRealStart=graphics.frames();windowGeneratedStart=graphics.generatedFrames();
        windowGpuWaitStart = display.gpuWaitMilliseconds();
        windowAcquireStart = display.acquireWaitMilliseconds();
        windowPresentStart = display.presentWaitMilliseconds();
        windowPresentedStart = display.presentedFrames();
        // fps counts render calls (a world frame streams in several parts);
        // displayFps counts successful presents queued to VideoOut, not the
        // actual scanout. VideoOutCadence observes the shown flip markers.
        tracef("[Perf] fps=%.2f displayFps=%.2f realFps=%.2f generatedFps=%.2f renderMeanMs=%.2f renderMaxMs=%.1f renderShare=%.0f%% "
               "gpuWaitShare=%.0f%% acquireShare=%.0f%% presentShare=%.0f%% idleMs=%.0f",
               windowFrames * 1000.0 / span, presented * 1000.0 / span,
               real*1000.0/span,generated*1000.0/span,
               windowRenderMs / (std::max)(windowFrames, 1u), windowRenderMaxMs, windowRenderMs * 100.0 / span,
               gpuWait * 100.0 / span, acquireWait * 100.0 / span, presentWait * 100.0 / span, windowIdleMs);
        // The same reading appended to a file reachable over FTP, for a session
        // whose kernel-log relay is not running. Capped so it cannot grow far.
        static unsigned perfLines = 0;
        // Roll the bounded log instead of permanently stopping FPS reports
        // after a long play session. World counters share this render-thread log.
        if(perfLines==2000)perfLines=0;
        {
            if (FILE* perf = std::fopen("/download0/darkness-ps5/perf.txt", perfLines++ == 0 ? "w" : "a")) {
                std::fprintf(perf, "fps=%.2f displayFps=%.2f realFps=%.2f generatedFps=%.2f renderShare=%.0f%% gpuWaitShare=%.0f%%\n",
                             windowFrames * 1000.0 / span, presented * 1000.0 / span,
                             real*1000.0/span,generated*1000.0/span,
                             windowRenderMs * 100.0 / span, gpuWait * 100.0 / span);
                // A diagnostic launch (profile-engine.enabled) also records the
                // per-window phase split here; the FTP-only session cannot read the
                // stderr stream these counters normally go to.
                if (Native::profileEngineCpu) Native::writeEngineCpuPerformance(perf);
                std::fclose(perf);
            }
        }
        windowStart = std::chrono::steady_clock::now();
        windowRenderMs = windowRenderMaxMs = windowIdleMs = 0.0;
        windowFrames = 0;
    };
    std::array<double, 120> frameIntervals{};
    size_t frameIntervalCount = 0;
    auto previousFrameEnd = std::chrono::steady_clock::time_point{};
    using MotionMatrices = std::array<Native::EngineVector, 4>;
    using MotionSamples = std::unordered_map<std::shared_ptr<const Native::StoredGeometry>, MotionMatrices>;
    MotionSamples previousMotion, currentMotion;
    unsigned motionFrames = 0, motionCompared = 0, motionRepeated = 0;
    auto renderLogged = [&](DarkRecomp::Native::PreviewFramePart& part) {
        if (part.first) currentMotion.clear();
        for (const auto& mesh : frame) {
            if (!mesh.world || currentMotion.size() >= 64) continue;
            const auto& draw = *mesh.world;
            if (draw.material != Native::WorldMaterial::depth || draw.options.weights || !draw.geometry.vertices) continue;
            MotionMatrices matrices;
            std::copy_n(draw.constants.vectors.begin(), matrices.size(), matrices.begin());
            currentMotion.try_emplace(draw.geometry.vertices, matrices);
        }
        if (part.first) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextCaptureCheck) {
                nextCaptureCheck = now + std::chrono::seconds(1);
                const auto request = std::filesystem::path(kConsoleDataRoot) / "capture.request";
                std::error_code ec;
                captureRequested = std::filesystem::exists(request, ec) && !ec;
                if (captureRequested) std::filesystem::remove(request, ec);
            }
        }
        if (part.last && captureRequested) {
            captureRequested = false;
            graphics.beginFrameDump(selectDumpDirectory(), 1);
        }
        const bool log = rendered < 20 || rendered % 100 == 0;
        if (log)
            tracef("render #%u: %zu meshes first=%u last=%u", rendered, frame.size(), unsigned(part.first),
                   unsigned(part.last));
        const auto renderStarted = std::chrono::steady_clock::now();
        graphics.render(frame, part);
        // The first presented frame is the moment the game has a picture of its
        // own on the panel, so that is when the Shell's launch splash comes
        // down: earlier and the loading screen would be a black panel, later and
        // the player would keep watching the splash over a running game.
        if (part.last && !splashHidden) {
            splashHidden = true;
            tracef("launch splash hidden (sceSystemServiceHideSplashScreen: %d)",
                   ::sceSystemServiceHideSplashScreen());
        }
        if (part.last) {
            for (const auto& [geometry, matrices] : currentMotion) {
                const auto found = previousMotion.find(geometry);
                if (found == previousMotion.end()) continue;
                ++motionCompared;
                motionRepeated += std::memcmp(matrices.data(), found->second.data(), sizeof(matrices)) == 0;
            }
            previousMotion.swap(currentMotion);
            if (++motionFrames == 120) {
                tracef("[MotionSamples] frames=%u matched=%u unchangedTransforms=%u", motionFrames, motionCompared, motionRepeated);
                motionFrames = motionCompared = motionRepeated = 0;
            }
            const auto end = std::chrono::steady_clock::now();
            if (previousFrameEnd != std::chrono::steady_clock::time_point{}) {
                frameIntervals[frameIntervalCount++] =
                    std::chrono::duration<double, std::milli>(end - previousFrameEnd).count();
                if (frameIntervalCount == frameIntervals.size()) {
                    auto sorted = frameIntervals;
                    std::sort(sorted.begin(), sorted.end());
                    unsigned longFrames = 0, shortFrames = 0;
                    double sum = 0;
                    for (double ms : frameIntervals) {
                        sum += ms;
                        longFrames += ms > 25.0;
                        shortFrames += ms < 8.0;
                    }
                    tracef("[Cadence] frames=%zu meanMs=%.2f minMs=%.2f p95Ms=%.2f maxMs=%.2f over25Ms=%u under8Ms=%u",
                           frameIntervals.size(), sum / frameIntervals.size(), sorted.front(),
                           sorted[113], sorted.back(), longFrames, shortFrames);
                    frameIntervalCount = 0;
                }
            }
            previousFrameEnd = end;
        }
        const auto renderMs = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now() - renderStarted).count();
        static unsigned slowReports = 0;
        if (renderMs > 250 && slowReports++ < 32)
            tracef("slow render #%u: %zu commands %lld ms first=%u last=%u", rendered, frame.size(),
                   static_cast<long long>(renderMs), unsigned(part.first), unsigned(part.last));
        if (log) tracef("render #%u done", rendered);
        windowRenderMs += double(renderMs);
        windowRenderMaxMs = (std::max)(windowRenderMaxMs, double(renderMs));
        if (++windowFrames >= 300) reportWindow();
        if (rendered % 300 == 0) {
            graphics.printCounters();
            // The per-window phase split goes to perf.txt when this run
            // opted in with profile-engine.enabled.
            // The palette-proof and world-image-decode counters reset on read;
            // they only answer a question if a run prints them while it is
            // still going, not at shutdown after the numbers matter.
            Native::printWorldCaptureCounters();
        }
        ++rendered;
    };
    auto nextPresentation = std::chrono::steady_clock::now();
    unsigned pacedLimit = 0;
    DarkRecomp::Ps5::registerSampledThread(DarkRecomp::Ps5::SampledThread::render);
    while (true) {
        if (Native::takeDisplaySettingsSaveRequest()) {
            DarkRecomp::Ps5::VideoPreferences updated;
            updated.graphics = Native::graphicsSettings();
            updated.fieldOfView = unsigned(Native::fieldOfViewSetting());
            updated.language = unsigned(Native::gameLanguageSetting());
            const bool saved = DarkRecomp::Ps5::saveVideoPreferences(preferencesPath, updated);
            Native::reportDisplaySettingsSave(saved);
            tracef("video settings %s", saved ? "saved" : "save failed");
        }
        const auto loopStarted = std::chrono::steady_clock::now();
        DarkRecomp::Native::PreviewFramePart part;
        if (graphicsReady && DarkRecomp::Native::takePreviewFrame(frame, 16, &part)) {
            renderLogged(part);
            if (part.last) {
                const unsigned limit = Native::graphicsSettings().frameRateLimit;
                const auto now = std::chrono::steady_clock::now();
                if (limit && limit == pacedLimit) {
                    const auto period = std::chrono::nanoseconds(1000000000ull / limit);
                    nextPresentation += period;
                    if (nextPresentation > now) std::this_thread::sleep_until(nextPresentation);
                    else if (now - nextPresentation > period) nextPresentation = now;
                } else nextPresentation = now;
                pacedLimit = limit;
            }
            continue;
        }
        if (graphicsReady)
            windowIdleMs += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - loopStarted)
                                .count();
        if (!graphicsReady) {
            // Nothing consumes the queue, so the guest must not be gated by it;
            // wait for the guest the way a window-less title can.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (guestDone.load()) {
            // The producer may have published its last frame just before it
            // finished: take what is queued before stopping.
            if (graphicsReady && DarkRecomp::Native::takePreviewFrame(frame, 0, &part)) {
                renderLogged(part);
                continue;
            }
            break;
        }
    }

    // Release a producer that could still be waiting, then let the guest finish.
    if (graphicsReady) DarkRecomp::Native::setPreviewFrameBackpressure(false);
    WaitForSingleObject(guest, INFINITE);
    if (graphicsReady) DarkRecomp::Native::setPreviewBackgroundRetirement(false);
    CloseHandle(guest);
    watchdog.join();
    cpuSampler.join();

    if (graphicsReady) {
        DarkRecomp::Native::printPreviewCounters();
        graphics.printCounters();
        if (graphics.frames() == 0) {
            std::printf("[Graphics] No frame was presented: the guest did not publish one.\n");
        }
    }

    // The engine does not return from its entry point while a frame is up.
    std::printf("[Title] The guest returned before a playable frame.\n");
    std::fflush(stdout);
    return 4;
}
