#pragma once
// The console's world draw path: the engine's geometry, materials and textures
// on Vulkan.
//
// renderer/d3d11/world_renderer.cpp is the Windows half of the engine's renderer:
// WorldRendererD3D11::draw takes a completed WorldDraw (world_mesh.h) -- an
// immutable geometry snapshot, a vertex program's options and constants, a
// fragment program name and flags, sixteen texture bindings and the captured
// rasterizer state -- and turns it into a D3D11 draw. This is the same function
// on RADV:
//
//   * the vertex stream is decoded once per engine geometry (Native::
//     decodeWorldVertices, the same decoder the Windows renderer uses) and
//     uploaded to a buffer, with the Windows renderer's 256/64 MB LRU budgets;
//   * the vertex program is the precompiled SPIR-V variant its options select
//     (world_shaders.h), whose attribute locations come from that module rather
//     than from a guess -- the template declares every input and only the used
//     ones survive the compiler;
//   * the fragment program is the precompiled original, with the same alpha-test
//     and bloom wrappers, selected by name and flags;
//   * the material constants are written into a per-frame host-visible arena and
//     bound as dynamic uniform buffers, one descriptor set for the vertex stage
//     and one for the fragment stage (their binding 2 names different buffers);
//   * textures are uploaded once per ColorImage, with the levels the image owns,
//     and sampled through a sampler built from the guest's own sampler state;
//     finished surfaces the engine resolved into a texture are sampled through
//     WorldSurfaces' resolved views instead;
//   * the render state -- depth test and write, stencil faces, blend factors and
//     write mask, culling, scissor, viewport and depth bias -- is read from the
//     captured attributes through world_draw_policy.h, which the host build
//     tests: this file only builds Vulkan objects from those answers.
//
// The draw renders into the engine's own targets: WorldSurfaces::BeginDraw opens
// the colour + depth pass for the draw's surfaces and EndDraw closes it. Nothing
// here presents; the resolved frontbuffer still goes through WorldSurfaces.
#include "world_draw_policy.h"
#include "world_surfaces.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace DarkRecomp {
namespace Native {
struct WorldDraw;
struct WorldQuery;
} // namespace Native
} // namespace DarkRecomp

namespace DarkRecomp::Ps5 {

class WorldDrawLayer {
public:
    WorldDrawLayer() = default;
    ~WorldDrawLayer();
    WorldDrawLayer(const WorldDrawLayer&) = delete;
    WorldDrawLayer& operator=(const WorldDrawLayer&) = delete;

    // Builds the two descriptor set layouts, the pipeline layout, the shader
    // modules and the constant arena on an open device. RADV's entry points are
    // resolved the same way the display and the surfaces resolve theirs, because
    // the ICD exports only vk_icdGetInstanceProcAddr.
    bool Create(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const char** error);
    void Destroy();
    bool created() const { return impl_ != nullptr; }

    // One frame: resets the constant arena, the per-frame descriptor sets and the
    // shader-module bookkeeping. The command buffer is the one the graphics
    // backend records the frame into, the same buffer the surfaces use.
    void BeginFrame(VkCommandBuffer command);
    void SetCommandBuffer(VkCommandBuffer command) { command_ = command; }

    // Executes one engine draw into the draw's own surfaces. A refusal leaves
    // `error` set, changes nothing on the target and is reported by the caller;
    // the reason numbering is WorldRendererD3D11::draw's (world_draw_policy.h).
    bool Draw(const Native::WorldDraw& draw, WorldSurfaces& targets, const char** error);
    bool Draw(const std::shared_ptr<const Native::WorldDraw>& draw,WorldSurfaces& targets,const char** error);

    // The engine's occlusion bracket (exposure histogram bin or flare patch):
    // the draws between begin and end are counted, and the passing samples are
    // published to the query's result once the frame has completed.
    void Query(const std::shared_ptr<Native::WorldQuery>& query, bool begin);

    uint64_t draws() const;
    uint64_t rejected() const;
    uint64_t pipelines() const;
    uint64_t textures() const;
    size_t vertexBytes() const;
    size_t textureBytes() const;
    void printCounters() const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
};

} // namespace DarkRecomp::Ps5
