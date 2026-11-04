// The console title's own code paths, driven on the host.
//
// It covers the frame capture layer (a synthetic guest UI draw, glyph atlas and
// movie frame must reach the preview queue the title consumes), the frame dump
// writer, and the pad reader the guest's XamInputGetState answers from -- the
// same translation units app/ps5/CMakeLists.txt links.
//
// renderer/engine/simple_mesh.cpp is the producer half of the console renderer:
// render_trace.cpp's hooks read a guest draw out of the engine's D3D state and
// hand it to these decoders, and app/ps5/title_main.cpp consumes the result with
// takePreviewFrame() and draws it through renderer/ps5/graphics_backend.cpp.
//
// The Windows build exercises that layer inside tests/engine_mesh_tests.cpp,
// which needs D3D11 and the native window: it cannot run on the console's code
// path. This test compiles the same translation units as the PS5 title and
// drives the two producers the console must see -- a UI draw and a movie frame
// -- from synthetic guest memory, then checks what the consumer receives. It is
// the host proof of the ported (non-Windows) guest-read branch.
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/render_trace.h"
#include "renderer/ps5/png_writer.h"
#include "runtime/native/input_ps5.h"
#include "ppc_context.h"
#include "engine_world_fragment_bindings.generated.h"
#include "renderer/engine/world_mesh.h"
#include "renderer/engine/queued_world_draw.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;

namespace {

constexpr uint32_t kContext = 0x82A69B00; // the engine's D3D context, from the guest
constexpr size_t kGuestSize = 0x100000000ull;
constexpr uint32_t kAttributes = kContext + 16896;
constexpr uint32_t kDescriptor = 0x10001, kPositions = 0x11001, kUv = 0x12001, kIndices = 0x14001;
constexpr uint32_t kModel = 0x01000000;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

// The guest's stores, as the engine writes them: 32-bit big-endian words.
void put16(uint8_t* base, uint32_t at, uint16_t value) {
    base[at] = uint8_t(value >> 8);
    base[at + 1] = uint8_t(value);
}
void put32(uint8_t* base, uint32_t at, uint32_t value) {
    base[at] = uint8_t(value >> 24);
    base[at + 1] = uint8_t(value >> 16);
    base[at + 2] = uint8_t(value >> 8);
    base[at + 3] = uint8_t(value);
}
void putFloat(uint8_t* base, uint32_t at, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put32(base, at, bits);
}

// The console's guest space is one committed 4 GB mapping (app/ps5/guest_image
// reserves it the same way), so the test reserves the same address space and
// the decoders index it exactly as they do on the console.
struct GuestSpace {
    uint8_t* base = nullptr;
    GuestSpace() {
        void* mapped = mmap(nullptr, kGuestSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        require(mapped != MAP_FAILED, "cannot reserve the console's 4 GB guest mapping");
        base = static_cast<uint8_t*>(mapped);
    }
    ~GuestSpace() {
        if (base) munmap(base, kGuestSize);
    }
};

// The original context's fixed pipeline state: an identity model matrix and
// projection, the 1280x720 canvas the original menus paint, and a vertex
// descriptor with float3 position, float2 UV0 and no other streams. Every
// console UI draw and movie quad arrives with this state.
void buildDrawState(uint8_t* base, uint32_t attributeBase) {
    std::memset(base + kContext, 0, 0x4400);
    put32(base, kContext + 8224, kModel);
    put32(base, kContext + 8224 + 8, 0);
    put32(base, kContext + 16512, kDescriptor);
    put32(base, kContext + 12564, 0);
    for (unsigned i = 0; i < 16; ++i) {
        const float identity = i % 5 == 0 ? 1.0f : 0.0f;
        putFloat(base, kModel + 16 + i * 4, identity);
        putFloat(base, kContext + 17088 + i * 4, identity);
    }
    put32(base, kContext + 17152, 0);
    put32(base, kContext + 17156, 0);
    put32(base, kContext + 17160, 1280);
    put32(base, kContext + 17164, 720);

    std::memset(base + kDescriptor, 0, 68);
    put16(base, kDescriptor, 3);
    put32(base, kDescriptor + 4, kPositions);
    put32(base, kDescriptor + 8, kUv);
    base[kDescriptor + 40] = 2; // float2 UV0
    // An old UI orientation triangle, in the engine's screen quad shape.
    const float positions[3][3] = {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned c = 0; c < 3; ++c) putFloat(base, kPositions + i * 12 + c * 4, positions[i][c]);
        putFloat(base, kUv + i * 8, i == 1 ? 0.0f : 1.0f);
        putFloat(base, kUv + i * 8 + 4, i == 1 ? 0.0f : 1.0f);
        put16(base, kIndices + i * 2, uint16_t(i));
    }
    std::memset(base + attributeBase, 0, 160);
}

} // namespace

// A front-end panel: the guest's own UI draw, with a completed texture upload
// and the original opaque material. The console consumer must receive one
// command carrying the decoded geometry and that texture.
static void testUiDrawReachesTheConsumer(uint8_t* base) {
    buildDrawState(base, kAttributes);
    constexpr uint16_t textureId = 0x0042;
    auto image = std::make_shared<ColorImage>();
    image->width = image->height = 4;
    image->pixels.assign(4 * 4 * 4, 0xFF);
    image->pixels[3] = 0x80;
    previewPublishTexture(textureId, 0x16000, image);

    put16(base, kAttributes + 8, textureId);
    put32(base, kAttributes + 92, 0x01100212); // original opaque UI pixel program
    put32(base, kAttributes + 108, 0xFFFFFFFF);
    put32(base, kAttributes + 116, 0xFFFFFFFF);
    base[kAttributes + 144] = 2;
    base[kAttributes + 145] = 1;

    previewObserveTriangles(base, kIndices, 1);
    previewEndFrame();
    std::vector<SimpleMesh> frame;
    require(takePreviewFrame(frame), "the consumer received no frame for a UI draw");
    require(frame.size() == 1, "a UI draw did not become exactly one render command");
    const SimpleMesh& mesh = frame.front();
    require(mesh.vertices.size() == 3 && mesh.indices.size() == 3, "UI geometry did not reach the consumer");
    require(mesh.colorTexture && mesh.colorTexture->width == 4 && mesh.colorTexture->height == 4,
            "the UI draw lost its uploaded texture on the way to the renderer");
    require(mesh.indices[2] == 2 && mesh.vertices[1].position[1] == 1.0f,
            "UI vertex or index data was mangled by the console decode");
    require(mesh.projection[0] == 1.0f && mesh.projection[5] == 1.0f,
            "the UI draw lost the original projection");
    std::vector<SimpleMesh> repeated;
    require(!takePreviewFrame(repeated), "a consumed UI frame was delivered twice");
    // Preserve single-frame order with deeper streamed buffering. The fourth
    // batch must wait; a later logical frame must wait even if a slot opens.
    std::future<void> producer;
    struct ReleaseQueue {~ReleaseQueue(){setPreviewFrameBackpressure(false);}} releaseQueue;
    setPreviewFrameBackpressure(true,true,3);
    std::atomic<unsigned> produced{0};
    std::atomic<bool> firstComplete{false},secondComplete{false};
    producer=std::async(std::launch::async,[&] {
        for(unsigned i=0;i<2049;++i) {
            putFloat(base,kPositions,float(i));
            previewObserveTriangles(base,kIndices,1);
            produced.store(i+1);
        }
        previewEndFrame();firstComplete=true;
        previewEndFrame();secondComplete=true;
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(produced<2048 && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(produced==2048,"Three ready batches were not admitted before backpressure");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(produced==2048 && !firstComplete,"Ready batch queue exceeded its configured bound");
    PreviewFramePart part;
    std::vector<SimpleMesh> held;
    for(unsigned batch=0;batch<5;++batch) {
        require(takePreviewFrame(frame,5000,&part),"Buffered batch was lost");
        const size_t expected=batch<4?512:1;
        require(frame.size()==expected && part.first==(batch==0) && part.last==(batch==4),"Buffered frame boundaries changed");
        for(unsigned i=0;i<frame.size();++i)
            require(frame[i].vertices[0].position[0]==float(batch*512+i),"Buffered draw order changed");
        if(batch==0)held=frame;
        if(batch==2) {
            const auto finishBy=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!firstComplete && std::chrono::steady_clock::now()<finishBy)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(firstComplete,"Final current-frame batch did not fit in the ready queue");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(!secondComplete,"A later logical frame bypassed an unconsumed final batch");
        }
    }
    require(takePreviewFrame(frame,5000,&part) && frame.empty() && part.first && part.last,"Empty next frame was reordered");
    require(producer.wait_for(std::chrono::seconds(5))==std::future_status::ready,"Buffered producer remained blocked");
    producer.get();
    require(!takePreviewFrame(frame,1,&part),"Buffered batch was delivered twice");
    setPreviewFrameBackpressure(false);
    previewObserveTriangles(base,kIndices,1);previewEndFrame();previewEndFrame();
    require(takePreviewFrame(frame) && frame.empty(),"Latest-frame mode retained a stale buffered draw");
    previewPrepareTexture(textureId);
    require(held.size()==512 && held.front().colorTexture && held.front().colorTexture->pixels[3]==0x80,
            "Buffered/retained draw lost its texture lifetime");
    std::puts("Preview queue: three bounded batches, exact command/frame order, empty frames, latest mode and retained resources pass.");
    std::puts("UiDraw: one command, decoded geometry, texture, identity projection, delivered once.");
}

// Front-end text: the original A8 glyph atlas path. The upload's pixels are
// pitch-converted by the console decoder, and the draw must carry that atlas
// plus its vertex tint to the renderer.
static void testTextDrawReachesTheConsumer(uint8_t* base) {
    constexpr uint16_t textureId = 0x0043;
    constexpr uint32_t image = 0x17001, pixels = 0x18001, colors = 0x13001;
    buildDrawState(base, kAttributes);

    std::memset(base + image, 0, 48);
    put32(base, image, 0x82097610);
    put32(base, image + 8, pixels);
    put32(base, image + 12, 8);
    put32(base, image + 16, 2);
    put32(base, image + 20, 2);
    put32(base, image + 24, 4);
    put32(base, image + 28, 1);
    put32(base, image + 32, 0x40000);
    put32(base, image + 40, 0x814);
    const uint8_t rows[8] = {0, 255, 99, 99, 64, 128, 88, 88};
    std::memcpy(base + pixels, rows, sizeof(rows));
    previewObserveImage(base, image, textureId, 0);

    // A glyph atlas carries its tint per vertex, so the descriptor has a color
    // stream; the payload is white so the atlas coverage is what is drawn.
    put32(base, kDescriptor + 52, colors);
    for (unsigned i = 0; i < 3; ++i) put32(base, colors + i * 4, 0xFFFFFFFF);
    put16(base, kAttributes + 8, textureId);
    put32(base, kAttributes + 92, 0x01100218); // original text pixel program
    base[kAttributes + 144] = 5;
    base[kAttributes + 145] = 6;

    previewObserveTriangles(base, kIndices, 1);
    previewEndFrame();
    std::vector<SimpleMesh> frame;
    require(takePreviewFrame(frame), "the consumer received no frame for a text draw");
    require(frame.size() == 1 && frame.front().texture, "a glyph draw reached the renderer without its atlas");
    require(frame.front().texture->pixels == std::vector<uint8_t>({0, 255, 64, 128}),
            "the glyph atlas row pitch or coverage bytes changed in the console decode");
    require(frame.front().vertices[0].color[0] == 1.0f, "the text draw lost its vertex tint");
    require(!frame.front().colorTexture && !frame.front().video, "a text draw was given the wrong texture path");
    std::puts("TextDraw: A8 atlas pitch conversion, vertex tint and the original text material at the consumer.");
}

// A movie: the guest's decoded YUV frame is retained by previewObserveVideo and
// then selected by the original video material's texture pair. The consumer
// must get the planes the console's mesh_video shader samples.
static void testMovieFrameReachesTheConsumer(uint8_t* base) {
    constexpr uint32_t container = 0x30000, array = 0x31000, owner = 0x32000, videoFrame = 0x33000;
    constexpr uint32_t yPlane = 0x34000, uvPlane = 0x35000, program = 0x36000, programName = 0x36200;
    buildDrawState(base, kAttributes);

    // The original video container: a 0x82097C50 header whose array holds the
    // selected object, a 0x82097CF8 object with the frame pointer and the
    // 32-bit texture pair the material samples.
    std::memset(base + container, 0, 64);
    put32(base, container, 0x82097C50);
    put32(base, container + 56, array);
    std::memset(base + array, 0, 28);
    put32(base, array + 4, 1);
    put32(base, array + 24, array + 28);
    put32(base, array + 28, owner);
    std::memset(base + owner, 0, 136);
    put32(base, owner, 0x82097CF8);
    put32(base, owner + 80, videoFrame);
    put32(base, owner + 126, 0x12345678);

    // The frame itself: timestamp, a 2x2 L8 luma plane and a 1x1 A8L8 VU plane,
    // laid out as the original plane descriptors are.
    std::memset(base + videoFrame, 0, 108);
    put32(base, videoFrame, 0x3FE00000); // 0.5 seconds
    put32(base, videoFrame + 4, 0);
    auto plane = [&](uint32_t at, uint32_t width, uint32_t height, uint32_t pitch, uint32_t bpp,
                     uint32_t format, uint32_t pixels, uint32_t size) {
        std::memset(base + at, 0, 48);
        put32(base, at, 0x82097610);
        put32(base, at + 8, pixels);
        put32(base, at + 12, size);
        put32(base, at + 16, width);
        put32(base, at + 20, height);
        put32(base, at + 24, pitch);
        put32(base, at + 28, bpp);
        put32(base, at + 32, format);
        put32(base, at + 40, 0x810);
    };
    plane(videoFrame + 12, 2, 2, 4, 1, 0x2000, yPlane, 8);
    plane(videoFrame + 60, 1, 1, 4, 2, 0x20000, uvPlane, 4);
    const uint8_t luma[8] = {16, 235, 77, 77, 81, 145, 66, 66};
    std::memcpy(base + yPlane, luma, sizeof(luma));
    base[uvPlane] = 240;
    base[uvPlane + 1] = 90;

    previewObserveVideo(base, container, 2);
    std::memset(base + program, 0, 20);
    put32(base, program, 5);
    put32(base, program + 4, programName);
    const char name[] = "CMWnd_ModTexture_PaintVideo_YUV2RGB";
    std::memcpy(base + programName, name, sizeof(name));

    put32(base, kAttributes, program);
    put32(base, kAttributes + 8, 0x12345678);
    put32(base, kAttributes + 92, 0x01100212);
    put32(base, kAttributes + 108, 0xFFFFFFFF);
    put32(base, kAttributes + 116, 0xFFFFFFFF);
    base[kAttributes + 144] = 2;
    base[kAttributes + 145] = 1;

    previewObserveTriangles(base, kIndices, 1);
    previewEndFrame();
    std::vector<SimpleMesh> frame;
    require(takePreviewFrame(frame), "the consumer received no frame for a movie");
    require(frame.size() == 1 && frame.front().video, "the movie quad reached the renderer without its YUV frame");
    const VideoFrame& video = *frame.front().video;
    require(video.width == 2 && video.height == 2, "the movie's luma size changed in the console decode");
    require(video.luma == std::vector<uint8_t>({16, 235, 81, 145}),
            "the movie's luma plane was not pitch-converted the original way");
    require(video.chroma == std::vector<uint8_t>({240, 90}), "the movie's VU plane was reordered or dropped");
    require(std::abs(video.timestampSeconds - 0.5) < 1e-9, "the movie's timestamp was not carried to the renderer");
    std::puts("MovieFrame: YUV planes, VU order, timestamp and material selected at the consumer.");
}

// The ported guest-read guard: on the console the 4 GB space is one committed
// mapping, so an address inside it is readable and one outside it is rejected
// before the copy (the Windows branch relies on SEH here instead).
// The console hands finished parts to a worker for destruction. A part must
// stay alive while it is the consumer's current frame, every part it replaced
// must be released, and disabling the worker must drain its backlog first.
static void testBackgroundRetirement(uint8_t* base) {
    buildDrawState(base, kAttributes);
    constexpr uint16_t textureId = 0x0043;
    put16(base, kAttributes + 8, textureId);
    put32(base, kAttributes + 92, 0x01100212);
    put32(base, kAttributes + 108, 0xFFFFFFFF);
    put32(base, kAttributes + 116, 0xFFFFFFFF);
    base[kAttributes + 144] = 2;
    base[kAttributes + 145] = 1;
    setPreviewBackgroundRetirement(true);
    std::vector<SimpleMesh> frame;
    std::vector<std::weak_ptr<const ColorImage>> parts;
    for (unsigned part = 0; part < 12; ++part) {
        // Republishing the id drops the cache's reference, so only the queued
        // draw keeps each image alive.
        auto image = std::make_shared<ColorImage>();
        image->width = image->height = 4;
        image->pixels.assign(4 * 4 * 4, uint8_t(part));
        previewPublishTexture(textureId, 0x16000, image);
        parts.emplace_back(image);
        image.reset();
        previewObserveTriangles(base, kIndices, 1);
        previewEndFrame();
        require(takePreviewFrame(frame) && frame.size() == 1 && frame.front().colorTexture,
                "a part did not reach the consumer with background retirement on");
        require(!parts.back().expired(), "background retirement released the part being drawn");
    }
    setPreviewBackgroundRetirement(false);
    previewPublishTexture(textureId, 0x16000, std::make_shared<ColorImage>());
    for (size_t part = 0; part + 1 < parts.size(); ++part)
        require(parts[part].expired(), "background retirement kept a replaced part alive");
    require(!parts.back().expired(), "background retirement released the consumer's current part");
    std::puts("BackgroundRetirement: replaced parts are released off the consumer, the current one is kept.");
}

static void testGuestReadGuard(uint8_t* base) {
    uint8_t probe[16]{};
    require(copyRenderMemory(base, 0x400, probe, sizeof(probe)),
            "a committed guest range was rejected by the console read guard");
    require(!copyRenderMemory(base, 0xFFFFFFFFull, probe, 2), "a read past the guest space was accepted");
    require(!copyRenderMemory(base, 0x100000000ull, probe, 1), "a read at the guest-space limit was accepted");
    require(!copyRenderMemory(base, 1, probe, SIZE_MAX), "a wrapped guest read was accepted");
    const uint8_t expected[4] = {0x12, 0x34, 0x56, 0x78};
    std::memcpy(base + 0x400, expected, sizeof(expected));
    require(equalRenderMemory(base, 0x400, expected, sizeof(expected)), "an equal guest range compared unequal");
    base[0x402] = 0;
    require(!equalRenderMemory(base, 0x400, expected, sizeof(expected)), "a changed guest range compared equal");
    require(!equalRenderMemory(base, 0xFFFFFFFFull, expected, 2), "a comparison past the guest space was accepted");
    std::puts("GuestReadGuard: committed reads, wrapped spans and comparisons match the console's mapping.");
}

// The console's pad API, scripted: input_ps5.cpp is the file the title links,
// so the test drives the real reader instead of a copy of its mapping.
namespace {
DarkRecomp::Native::Ps5Pad::PadSample padSamples[4];
int padSampleCount = 0;
} // namespace
extern "C" {
int scePadInit(void) { return 0; }
int scePadOpen(int32_t, int32_t, int32_t, const void*) { return 7; }
int scePadGetHandle(int32_t, int32_t, int32_t) { return 7; }
int sceUserServiceInitialize(const void*) { return 0; }
int sceUserServiceGetInitialUser(int32_t* userId) {
    if (userId) *userId = 0x10000000;
    return 0;
}
int scePadRead(int32_t, void* samples, int32_t capacity) {
    const int count = (std::min)(padSampleCount, capacity);
    std::memcpy(samples, padSamples, sizeof(padSamples[0]) * size_t(count));
    // ScePadRead reports only samples taken since the last call.
    padSampleCount = 0;
    return count;
}
} // extern "C"

static void testPs5PadReachesTheGuest() {
    using namespace DarkRecomp::Native::Ps5Pad;
    PadState state;
    require(!readPad(state), "an unread pad reported a connected controller");

    PadSample sample{};
    sample.connected = 1;
    sample.buttons = 0x004000; // Cross
    sample.leftX = 255;        // full right
    sample.leftY = 0;          // full up
    sample.rightX = sample.rightY = 128;
    sample.l2 = 200;
    sample.timestampUs = 1;
    padSamples[0] = sample;
    padSampleCount = 1;
    require(readPad(state), "a connected pad was reported missing");
    require(state.buttons == kA, "Cross did not reach the guest as A");
    require(state.leftX == 32512 && state.leftY == 32767,
            "the left stick's direction or signed range is wrong");
    require(state.leftTrigger == 200 && state.rightTrigger == 0, "a trigger did not reach the guest");
    require(state.packet != 0, "the first pad state has no packet number");

    // ScePad hands out only new samples, so a poll with none must not make the
    // pad vanish or look like a change.
    PadState repeated;
    const uint32_t packet = state.packet;
    require(readPad(repeated) && repeated.packet == packet && repeated.buttons == kA,
            "an idle pad looked disconnected or changed its packet");

    // Every reported axis change is input; the guest owns the deadzone.
    sample.leftX = 254;
    padSamples[0] = sample;
    padSampleCount = 1;
    require(readPad(repeated) && repeated.packet == packet+1 && repeated.leftX==32256,
            "a small reported stick change did not reach the guest");
    sample.buttons = 0x002000 | 0x001000; // Circle + Triangle
    sample.leftX = 0;
    padSamples[0] = sample;
    padSampleCount = 1;
    require(readPad(repeated) && repeated.buttons == (kB | kY),
            "the face buttons did not map onto XInput's layout");
    require(repeated.packet == packet + 2, "a real pad change did not increment the packet");

    // Options, d-pad up, both shoulders and both stick clicks.
    sample.buttons = 0x000008 | 0x000010 | 0x000400 | 0x000800 | 0x000002 | 0x000004;
    padSamples[0] = sample;
    padSampleCount = 1;
    require(readPad(repeated) &&
                repeated.buttons == (kStart | kDpadUp | kLeftShoulder | kRightShoulder | kLeftThumb | kRightThumb),
            "the pad's remaining buttons did not map onto XInput's layout");

    sample.buttons = 0x000020 | 0x000040 | 0x000080 | 0x000008; // Right, Down, Left, Options
    padSamples[0] = sample;
    padSampleCount = 1;
    require(readPad(repeated) && (repeated.buttons & (kDpadRight | kDpadDown | kDpadLeft | kStart)) ==
                                     (kDpadRight | kDpadDown | kDpadLeft | kStart),
            "the d-pad's other directions did not map onto XInput's layout");

    sample.connected = 0;
    padSamples[0] = sample;
    padSampleCount = 1;
    require(!readPad(repeated), "a disconnected pad was reported connected");
    std::puts("Ps5Pad: XInput-shaped state, change-only packet, idle polls and the pad's full button map.");
}

// The console title gets these from the AOT code and from render_trace.cpp,
// which needs the translated guest and the PPC imports. None of them is on the
// UI or movie path; the stubs keep this test to the capture layer itself.
extern "C" PPC_FUNC(__imp__sub_8224AB18) {}
extern "C" PPC_FUNC(__imp__sub_82248C80) {}
namespace DarkRecomp::Native {
PPCContext*& currentContextSlot() {
    static thread_local PPCContext* context = nullptr;
    return context;
}
bool renderTraceEnabled() noexcept { return false; }
void traceMissingWorldTexture(uint8_t*, uint32_t, uint32_t, uint32_t) noexcept {}
void traceEngineVertexProgram(uint8_t*, const EngineVertexProgramObservation&) noexcept {}
} // namespace DarkRecomp::Native

// The frame dump writes PNG itself, since the console has no zlib or libpng.
// That writer is checked here by its container: the signature, the IHDR fields
// a decoder requires and a complete chunk table. The runner decodes the file
// with Python's zlib afterwards, which is the stronger pixel check.
static void testFrameDumpPng(const char* path) {
    // The swapchain's two 32-bit orders must both land in red, green, blue.
    const uint8_t texels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t packed[6]{};
    Ps5::packRgbRows(texels, 2, 1, true, packed);
    require(packed[0] == 3 && packed[1] == 2 && packed[2] == 1 && packed[3] == 7 && packed[4] == 6 && packed[5] == 5,
            "a B8G8R8A8 surface was not repacked into RGB");
    Ps5::packRgbRows(texels, 2, 1, false, packed);
    require(packed[0] == 1 && packed[1] == 2 && packed[2] == 3 && packed[3] == 5 && packed[4] == 6 && packed[5] == 7,
            "an R8G8B8A8 surface was not repacked into RGB");

    constexpr uint32_t width = 5, height = 3;
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* texel = rgb.data() + (size_t(y) * width + x) * 3;
            texel[0] = uint8_t(x * 40);
            texel[1] = uint8_t(y * 80);
            texel[2] = uint8_t(255 - x * 10);
        }
    }
    const char* error = nullptr;
    require(Ps5::writePngRgb(path, width, height, rgb.data(), size_t(width) * 3, &error),
            "the frame dump could not write a PNG");

    std::FILE* file = std::fopen(path, "rb");
    require(file != nullptr, "the frame dump's PNG cannot be reopened");
    uint8_t bytes[33]{}; // signature, IHDR length, type and 13 data bytes, CRC
    require(std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes), "the frame dump's PNG is truncated");
    std::fclose(file);
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    require(std::memcmp(bytes, signature, sizeof(signature)) == 0, "the frame dump is not a PNG");
    require(std::memcmp(bytes + 12, "IHDR", 4) == 0, "the frame dump has no IHDR");
    const auto word = [&](unsigned at) {
        return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 |
               uint32_t(bytes[at + 3]);
    };
    require(word(16) == width && word(20) == height, "the frame dump's dimensions differ from the image");
    require(bytes[24] == 8 && bytes[25] == 2 && bytes[26] == 0 && bytes[27] == 0 && bytes[28] == 0,
            "the frame dump's IHDR is not 8-bit RGB, deflate, unfiltered, non-interlaced");
    std::printf("FrameDumpPng: %s, %ux%u RGB, IHDR and signature verified.\n", path, width, height);
}

static void testQueuedVertexRecycle(uint8_t* base) {
    std::memset(base+kAttributes,0,160);base[kAttributes+97]=8;
    put32(base,kContext+17160,1280);put32(base,kContext+17164,720);
    putFloat(base,kContext+17168,0);putFloat(base,kContext+17172,1);
    EngineVertexBindingSnapshot binding;
    binding.deviceAddress=0x01000000;binding.descriptor.flags=0x01000000;binding.descriptor.modes.fill(4);
    const auto key=encodeEngineVertexDescriptor(binding.descriptor);
    for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(key[i*4])<<24|uint32_t(key[i*4+1])<<16|
        uint32_t(key[i*4+2])<<8|key[i*4+3];
    auto resource=std::make_shared<StoredGeometry>();
    resource->vertexCount=3;resource->vertices.resize(36);resource->indices={0,1,2};
    StoredDraw source{resource,resource,0,3};source.vertexBindings=binding;
    auto capture=[&] {
        auto draw=captureWorldDraw(base,source);
        require(bool(draw),"Recycle fixture capture failed");return draw;
    };
    auto checkClean=[&](const WorldDraw* expected=nullptr) {
        auto draw=capture();
        if(expected)require(draw.get()==expected,"Recycle check did not reuse the retired object");
        for(const auto& vector:draw->constants.vectors)
            for(float lane:vector)require(std::bit_cast<uint32_t>(lane)==0,"Recycled vertex bank retained a dirty lane");
        require(!QueuedWorldDrawAccess::owned(*draw),"Recycled object retained queue ownership");
    };
    selectiveWorldDrawReset.store(true);selectiveWorldDrawResetSafe.store(true);
#if defined(DARK_TEST_VERTEX_PLAN_CACHE)
    // A merged plan can write a later original range before encountering an
    // earlier original failure. That rejected, unpublished object must receive
    // a full reset rather than clearing only the original written prefix.
    auto failureSource=source;
    auto& failureBinding=*failureSource.vertexBindings;
    failureBinding.descriptor.color=200;
    failureBinding.descriptor.modes[0]=1;
    failureBinding.descriptor.parameters[0][0]=100;
    const auto failureKey=encodeEngineVertexDescriptor(failureBinding.descriptor);
    for(unsigned i=0;i<5;++i)failureBinding.key[i+1]=uint32_t(failureKey[i*4])<<24|
        uint32_t(failureKey[i*4+1])<<16|uint32_t(failureKey[i*4+2])<<8|failureKey[i*4+3];
    vertexProgramPlansEnabled=true;
    auto warm=captureWorldDraw(base,failureSource);
    require(bool(warm),"Merged-plan failure fixture did not warm its valid plan");
    warm.reset();warm=captureWorldDraw(base,failureSource); // second observation admits the recipe
    require(bool(warm),"Merged-plan failure fixture did not admit its valid plan");
    const auto* failedAddress=warm.get();warm.reset();
    for(unsigned row=100;row<104;++row)for(unsigned lane=0;lane<4;++lane)
        failureBinding.constantBytes[row*16+lane*4]=0x3f;
    failureBinding.constantBytes[200*16]=0x7f;
    failureBinding.constantBytes[200*16+1]=0x80;
    require(!captureWorldDraw(base,failureSource),"Merged-plan nonfinite constant was accepted");
    checkClean(failedAddress);
#endif
    auto publicDraw=capture();
    publicDraw->constants.vectors[200]={7,8,9,10};
    const auto* publicAddress=publicDraw.get();
    auto saved=worldVertexResetBytesSaved.load();publicDraw.reset();
    require(worldVertexResetBytesSaved.load()==saved,"Public mutable draw used selective clearing");checkClean(publicAddress);
    for(bool fullMask:{false,true}) {
        auto draw=capture();
        for(unsigned row:{0u,63u,64u,127u,128u,255u}) {
            draw->constants.markWritten(row,1);
            draw->constants.vectors[row]={1,-0.0f,std::bit_cast<float>(0x7FC01234u),3};
        }
        if(fullMask)draw->constants.written.fill(UINT64_MAX);
        size_t written=0;for(auto word:draw->constants.written)written+=std::popcount(word)*16;
        auto queued=QueuedWorldDrawAccess::seal(std::move(draw));
        require(!draw && QueuedWorldDrawAccess::owned(*queued),"Exclusive queue snapshot was not sealed");
        const auto* queuedAddress=queued.get();
        saved=worldVertexResetBytesSaved.load();queued.reset();
        require(worldVertexResetBytesSaved.load()-saved==4096-written,"Selective reset cleared the wrong vector ranges");checkClean(queuedAddress);
    }
    auto queued=QueuedWorldDrawAccess::seal(capture());
    auto copy=capture();*copy=*queued;
    require(!QueuedWorldDrawAccess::owned(*copy),"Copying a queued value leaked private ownership");
    copy->constants.vectors[201]={1,2,3,4};saved=worldVertexResetBytesSaved.load();copy.reset();
    require(worldVertexResetBytesSaved.load()==saved,"Mutable copy used selective clearing");checkClean();queued.reset();
    auto alias=capture();auto mutableAlias=alias;
    auto shared=QueuedWorldDrawAccess::seal(std::move(alias));
    require(!QueuedWorldDrawAccess::owned(*shared),"Mutable alias was sealed as exclusively owned");
    mutableAlias->constants.vectors[202]={1,2,3,4};shared.reset();
    saved=worldVertexResetBytesSaved.load();mutableAlias.reset();
    require(worldVertexResetBytesSaved.load()==saved,"Aliased draw used selective clearing");checkClean();
    auto unknown=capture();unknown->constants.written={};unknown->constants.vectors[203]={1,2,3,4};
    auto unknownQueued=QueuedWorldDrawAccess::seal(std::move(unknown));
    saved=worldVertexResetBytesSaved.load();unknownQueued.reset();
    require(worldVertexResetBytesSaved.load()==saved,"Unknown mask skipped full clearing");checkClean();
    auto unsafe=QueuedWorldDrawAccess::seal(capture());selectiveWorldDrawResetSafe.store(false);
    saved=worldVertexResetBytesSaved.load();unsafe.reset();
    require(worldVertexResetBytesSaved.load()==saved,"Disabled safety latch skipped full clearing");checkClean();
    selectiveWorldDrawResetSafe.store(true);selectiveWorldDrawReset.store(false);
    std::puts("Queued vertex recycling: exact ranges, immutable ownership, public mutation/alias/copy, unknown masks and safety fallback pass.");
}

#if defined(DARK_TEST_VERTEX_PLAN_CACHE)
#include "tests/vertex_program_plan_cases.h"
#endif
#if defined(DARK_TEST_STORED_LOCK)
#include "tests/stored_lock_cases.h"
#endif
#if defined(DARK_TEST_NATIVE_CAPTURE)
#include "tests/native_capture_cases.h"
#endif
int main(int argc, char** argv) {
#if defined(DARK_TEST_VERTEX_PLAN_CACHE)
    testVertexProgramPlans();
#endif
    const char* pngPath = argc > 1 ? argv[1] : "/tmp/darkness-frame-dump.png";
    try {
        GuestSpace guest;
#if defined(DARK_TEST_STORED_LOCK)
        testStoredLockPolicies(guest.base);
#endif
        enableEnginePreview();
        for(const auto& binding:worldFragmentBindings)
            require(worldFragmentTextureMask(binding.name,binding.flags)==uint16_t(binding.textures),
                    "Indexed native shader binding selected a different texture mask");
        require(worldFragmentTextureMask("NoSuchProgram",0)==0xFFFF &&
                worldFragmentTextureMask(worldFragmentBindings[0].name,UINT32_MAX)==0xFFFF,
                "Unknown native shader binding was accepted");
        testPs5PadReachesTheGuest();
        testGuestReadGuard(guest.base);
        testUiDrawReachesTheConsumer(guest.base);
        testTextDrawReachesTheConsumer(guest.base);
        testMovieFrameReachesTheConsumer(guest.base);
        testBackgroundRetirement(guest.base);
#if defined(DARK_TEST_NATIVE_CAPTURE)
        testNativeCapture(guest.base);
#endif
        testQueuedVertexRecycle(guest.base);
        testFrameDumpPng(pngPath);
        printPreviewCounters();
        std::puts("PS5 capture layer: the guest's UI and movie frames reach the console consumer.");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
