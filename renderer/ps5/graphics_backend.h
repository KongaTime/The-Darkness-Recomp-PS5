#pragma once
// The console's graphics backend: the Vulkan renderer behind the title.
//
// The Windows build draws with D3D11: renderer/d3d11/engine_preview.cpp owns an
// ID3D11Device and hands the engine's commands to WorldRendererD3D11. The
// console has no D3D11, only RADV, so this class is that same seam on the
// console: it drains the engine's render command stream (the SimpleMesh frames
// simple_mesh.h publishes, one part at a time) and draws them into the
// display's swapchain.
//
// What it draws today: the engine's simple meshes -- A8 glyph atlases, RGBA8
// artwork and the full-screen YUV movie path -- with the D3D11 preview's
// shaders, projection convention and blend state translated to SPIR-V. It also
// owns the engine's own render targets, through renderer/ps5/world_surfaces.h:
// the guest's surface clears, the resolves that copy a finished surface into the
// texture the next pass samples, and the frontbuffer copy that puts the resolved
// scene into the swapchain image before the UI lands on top of it.
//
// What it draws through the engine's targets: the world draws themselves, in
// renderer/ps5/world_draw.h. Each carries its own vertex program, fragment
// program, textures and render state, and renders into the surfaces above.
// What it still does not execute is the display gamma ramp and the occlusion
// queries; those are counted and reported rather than dropped silently, and the
// frame still presents, so the guest's producer never waits on a consumer that
// cannot keep up.
#include <cstdint>
#include <string>
#include <vector>

namespace DarkRecomp {
struct SimpleMesh;
namespace Native {
struct PreviewFramePart;
}
} // namespace DarkRecomp

namespace DarkRecomp::Ps5 {

class Display;

class GraphicsBackend {
public:
    GraphicsBackend() = default;
    ~GraphicsBackend();
    GraphicsBackend(const GraphicsBackend&) = delete;
    GraphicsBackend& operator=(const GraphicsBackend&) = delete;

    // Builds the pipelines, sampler and upload buffers on `display`'s device
    // and swapchain. The display must outlive the backend and stay open.
    bool Initialize(Display& display);
    void Shutdown();

    // Consumes one part of an engine frame. The first part acquires the next
    // image and clears it, the last submits and presents. Never throws: a
    // failure is reported through error(), the frame is closed, and the next
    // frame acquires a fresh image.
    void render(const std::vector<SimpleMesh>& meshes, Native::PreviewFramePart part);

    // Debug frame dump: writes the next `frames` presented frames as PNG files
    // in `directory`. A dumped frame is copied to a staging buffer in the same
    // command buffer that drew it and written after the display has taken it,
    // so the file is exactly what the console showed. Each file waits for the
    // GPU once; this is for a few diagnostic frames, not recording.
    bool beginFrameDump(const std::string& directory, int frames);

    const char* error() const { return error_; }
    uint64_t frames() const { return frames_; }
    uint64_t generatedFrames() const;
    uint64_t meshes() const { return meshes_; }
    uint64_t rejectedMeshes() const { return rejected_; }
    uint64_t worldCommands() const { return worldCommands_; }
    uint64_t worldDraws() const { return worldDraws_; }
    uint64_t worldClears() const { return worldClears_; }
    uint64_t worldResolves() const { return worldResolves_; }
    uint64_t worldPresents() const { return worldPresents_; }
    uint64_t worldRejected() const { return worldRejected_; }
    void printCounters() const;

    struct Impl;

private:
    void closeFrame();
    void writeFrameDump();

    Impl* impl_ = nullptr;
    const char* error_ = "not initialised";
    uint64_t frames_ = 0;
    uint64_t meshes_ = 0;
    uint64_t rejected_ = 0;
    uint64_t worldCommands_ = 0;
    uint64_t worldDraws_ = 0;
    uint64_t worldClears_ = 0;
    uint64_t worldResolves_ = 0;
    uint64_t worldPresents_ = 0;
    uint64_t worldRejected_ = 0;
};

} // namespace DarkRecomp::Ps5
