#pragma once
// The engine's render targets on the console, and what the guest does with them.
//
// This is renderer/d3d11/world_renderer.cpp's surface half on Vulkan. The
// engine's own renderer never allocates a target itself: it publishes commands
// (world_mesh.h) naming Xenos surfaces by key, and the renderer behind
// `engine_preview.cpp` on Windows owns the D3D11 textures that answer for them.
// The console has no D3D11, so this class owns their Vulkan equivalents:
//
//   * a surface per key, colour in half-float and depth/stencil in 24:8, grown
//     on demand and never shrunk, cleared to zero when created -- the engine
//     resolves whatever it draws into them, and a resolve never allocates;
//   * a clear, which is the guest's ClearRenderTargetView / ClearDepthStencilView
//     equivalent, whole-surface or a rectangle of it;
//   * a resolve, which copies a surface's pixels into the guest's texture in
//     that texture's format and precision, which is the step that makes a
//     finished scene readable by the next pass;
//   * the frontbuffer copy, which puts the resolved frontbuffer into the
//     swapchain image so it reaches the display.
//
// It also opens the pass a world draw renders into (BeginDraw/EndDraw): the
// colour surface at slot 0 and the depth surface at slot 4, both grown to the
// union of the draw's viewport and their retained extents, exactly as the
// Windows renderer retains a matching pair. The drawing itself -- pipelines,
// constants, textures -- is renderer/ps5/world_draw.h, which records between the
// two calls; this class only owns the targets and their layouts.
//
// The decisions -- growth, the resolve rectangle, format mapping, clear
// regions -- are world_surface_policy.h, which the host build tests. This class
// only creates objects and records commands.
#include "world_surface_policy.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstddef>

namespace DarkRecomp {
namespace Native {
struct DisplayGamma;
struct WorldClear;
struct WorldDraw;
struct WorldResolve;
struct WorldTexture;
} // namespace Native
} // namespace DarkRecomp

namespace DarkRecomp::Ps5 {

class WorldSurfaces {
public:
    WorldSurfaces() = default;
    ~WorldSurfaces();
    WorldSurfaces(const WorldSurfaces&) = delete;
    WorldSurfaces& operator=(const WorldSurfaces&) = delete;

    // Builds the passes, layouts and pipelines on an open device. The modules
    // are the resolve's exact texel copy and the present's fitted copy, compiled
    // to SPIR-V at build time. A partially created layer is torn down, like the
    // graphics backend's.
    bool Create(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                const uint32_t* transferVertex, size_t vertexBytes, const uint32_t* transferFragment,
                size_t fragmentBytes, const uint32_t* presentFragment, size_t presentBytes,
                const char** error);
    void Destroy();
    bool created() const { return impl_ != nullptr; }

    // The swapchain the frontbuffer is copied into. Called at startup and after
    // every rebuild: the present pipeline is recorded against that render pass
    // and the frontbuffer must match the image's extent exactly, as the D3D11
    // preview's presentation target does. `pass` must be null before the first
    // call, which is how a rebuild releases the old pipeline.
    bool SetSwapchain(VkRenderPass pass, uint32_t width, uint32_t height, const char** error);

    // One frame. The descriptor pool is reset here, before the frame records a
    // resolve, so a set is never rewritten while an earlier frame may use it.
    void BeginFrame();
    // Every world command of this frame is recorded into `command`.
    void SetCommandBuffer(VkCommandBuffer command) { command_ = command; }

    // The framebuffer one world draw renders into, and the pass that owns it.
    // BeginDraw grows both attachments to the union of the draw's viewport and
    // their retained extents, creates the framebuffer on first use, transitions
    // both images back to attachment layouts and opens the pass. Consecutive
    // draws with matching targets share it. FlushDraws closes it before a
    // transfer, clear, resolve, swapchain draw or frame submission.
    struct DrawTarget {
        uint64_t serial = 0;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        bool color = false, depth = false;
    };
    bool BeginDraw(const Native::WorldDraw& draw, DrawTarget& target, const char** error);
    void EndDraw();
    void FlushDraws();

    // The pass a draw pipeline is built against for its combination of
    // attachments: colour + depth, colour alone or depth alone. A pipeline is
    // tied to the pass's attachments, not to a particular surface, so the draw
    // layer builds one per combination and keeps it.
    VkRenderPass drawPass(bool color, bool depth) const;
    uint32_t renderScale() const;

    // The sampled view of a texture the engine has resolved into, or null when
    // it has not been resolved (world_mesh.h's WorldTexture::key). Draws read
    // finished surfaces through this, as the Windows renderer reads its
    // resolved_ map.
    struct ResolvedInfo {
        uint32_t width = 0, height = 0, faces = 1;
    };
    VkImageView resolvedView(uint64_t key) const;
    // The extent and face count a resolved texture was allocated with, for the
    // draw layer's binding check (the Windows renderer compares the draw's
    // texture against its resolved surface the same way).
    bool resolvedInfo(uint64_t key, ResolvedInfo& info) const;

    // Executes one engine command. A refusal leaves `error` set and changes
    // nothing; the caller reports it and keeps the frame. The reject reasons are
    // the Windows renderer's (world_surface_policy.h).
    bool Clear(const Native::WorldClear& clear, const char** error);
    bool Resolve(const Native::WorldResolve& resolve, const char** error);
    // Copies the resolved frontbuffer into the swapchain image, filling it: at
    // the panel's own size the copy is exact, and a frontbuffer of another size
    // is fitted to it, as the Windows display path fits its target to the
    // window. The caller has that image's render pass open: the copy is a draw
    // into it, so the engine's UI, drawn afterwards, lands on top of the scene.
    bool Present(const Native::WorldTexture& texture, const char** error);
    // Draw an owned interpolated scene with the same ramp, brightness and
    // scaling as a real frontbuffer, without replacing the tracked real scene.
    bool PresentGenerated(VkImageView image,const char** error);

    // The engine's display ramp (the Windows preview's displayGammaPixel pass):
    // every later present maps the frontbuffer through it.
    void SetDisplayGamma(const Native::DisplayGamma& gamma);

    // The resolved frontbuffer the last successful present sampled. The debug
    // frame dump reads this image rather than the swapchain: on this stack the
    // swapchain image's copy to a buffer comes back with nothing written, while
    // a resolved frontbuffer is an ordinary image of the layer's own.
    struct PresentedFrame {
        VkImage image = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    PresentedFrame presentedFrame() const;

    size_t surfaceCount() const;
    size_t resolvedCount() const;
    size_t drawTargetCount() const;
    void printCounters() const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
};

} // namespace DarkRecomp::Ps5
