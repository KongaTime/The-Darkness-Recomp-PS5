#include "world_surfaces.h"

#include "display_vulkan.h"
#include "world_surface_policy.h"

#include "renderer/engine/display_gamma.h"
#include "renderer/engine/world_mesh.h"
#include "runtime/native/graphics_settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace DarkRecomp::Ps5 {
namespace {

namespace Policy = WorldSurfacePolicy;

// The engine's own render target formats. Half-float colour is what the
// original renderer's surfaces hold (DXGI_FORMAT_R16G16B16A16_FLOAT). Its
// depth/stencil is D24S8, which AMD hardware does not have: RADV refuses
// D24_UNORM_S8_UINT as a depth/stencil format (radv_is_zs_format_supported),
// and surfaces made in it on the console left depth testing broken - walls and
// bodies vanished while unsorted effects and eyes stayed (2026-10-25). D32 float
// with 8-bit stencil holds every D24 value and the same stencil. The resolve is
// what narrows a surface into the guest's texture format, never the surface.
constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

// Bounded like the Windows renderer's caches. The engine keeps a handful of
// targets per frame (scene, mask and the bloom chain), not one per name, and a
// frame with more than this is reported rather than allowed to grow without end.
constexpr size_t kMaxSurfaces = 96;
constexpr size_t kMaxResolved = 64;
// One descriptor set per resolve and present, reset once per frame.
constexpr uint32_t kDescriptorSets = 512;

VkImageAspectFlags imageAspect(VkFormat format) {
    return format == kDepthFormat ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageAspectFlags sampledAspect(VkFormat format) {
    // A depth image is sampled through its depth plane; the stencil plane is not
    // sampleable on every device and the engine reads depth as a float.
    return format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkFormat resolveFormat(Policy::DestinationFormat format) {
    using enum Policy::DestinationFormat;
    switch (format) {
        case rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        // The same packed layout as DXGI_FORMAT_R10G10B10A2_UNORM: red in the
        // low ten bits. The original display ramp consumes all ten.
        case rgb10a2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
        case r32f: return VK_FORMAT_R32_SFLOAT;
        case unsupported: break;
    }
    return VK_FORMAT_UNDEFINED;
}

// One GPU image and the views the renderer uses it through. `attachment` is the
// view a pass renders into (a depth view must carry both planes), `sampled` is
// the view the copy reads it through (a depth image is read as its depth plane
// alone), and `framebuffer` binds it to its single-attachment pass.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView attachment = VK_NULL_HANDLE;
    VkImageView sampled = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A surface and the guest texture its resolves land in are both images; a
// surface also remembers its kind, and a resolved texture its faces.
struct Surface : Image {
    bool depth = false;
};

struct Resolved : Image {
    uint32_t faces = 1;
    std::array<VkImageView, 6> faceViews{};
    std::array<VkFramebuffer, 6> faceFramebuffers{};
};

} // namespace

// The layer's own objects. Entry points are resolved from the ICD the same way
// the display does it: RADV exports vk_icdGetInstanceProcAddr and nothing else.
struct WorldSurfaces::Impl {
    uint32_t scale = 1;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, Surface> surfaces;
    std::unordered_map<uint64_t, Resolved> resolved;

    VkRenderPass colorClearPass = VK_NULL_HANDLE;
    VkRenderPass depthClearPass = VK_NULL_HANDLE;
    // The passes one world draw renders through: colour and depth together, and
    // depth alone for the original's shadow and depth-only materials. Both load
    // the attachments -- a draw is always part of a scene that is already there.
    VkRenderPass colorDrawPass = VK_NULL_HANDLE;
    VkRenderPass drawPass = VK_NULL_HANDLE;
    VkRenderPass depthDrawPass = VK_NULL_HANDLE;
    // A framebuffer binds one colour and one depth surface at their retained
    // extents. Surfaces never shrink, so a pair is stable until either surface is
    // recreated: `surfaceEpoch` counts those events and `framebufferEpoch` says
    // which one the cached framebuffers belong to.
    std::unordered_map<uint64_t, VkFramebuffer> drawFramebuffers;
    // A framebuffer a recorded draw still names is never destroyed in the frame
    // that recorded it; it waits here until the next frame.
    std::vector<VkFramebuffer> retiredFramebuffers;
    // Surfaces replaced by a grown copy while this frame's commands still use
    // them; freed at the next frame, once the GPU has finished this one.
    std::vector<Image> retiredImages;
    std::vector<Resolved> retiredResolved;
    uint64_t surfaceEpoch = 0, framebufferEpoch = 0;
    bool drawPassOpen = false;
    DrawTarget activeTarget{};
    uint64_t activeColorKey = 0, activeDepthKey = 0, renderPasses = 0;
    void flushDraws() {
        if (!drawPassOpen) return;
        CmdEndRenderPass(command);
        drawPassOpen = false;
    }
    uint64_t draws = 0;
    std::array<VkRenderPass, 5> resolvePasses{}; // indexed by DestinationFormat

    VkShaderModule transferVertex = VK_NULL_HANDLE;
    VkShaderModule transferFragment = VK_NULL_HANDLE;
    VkShaderModule presentFragment = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> resolvePipelines{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    // A resolve reads whole texels; the present fits the frontbuffer to the
    // panel, which needs a filter.
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler presentSampler = VK_NULL_HANDLE;
    // The engine's display ramp (Native::DisplayGamma: 768 interleaved RGB
    // base/delta pairs), host-visible so a new ramp is a plain copy. The present
    // reads it at binding 1; gammaMode is 0 before the engine sets one, 1 for a
    // 256-entry table and 2 for the piecewise-linear one.
    VkBuffer gammaBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gammaMemory = VK_NULL_HANDLE;
    void* gammaMapped = nullptr;
    uint32_t gammaMode = 0;

    // The swapchain the frontbuffer is copied into. The present pipeline is
    // recorded against that render pass, so a rebuilt swapchain gets a new one.
    VkRenderPass swapchainPass = VK_NULL_HANDLE;
    VkPipeline presentPipeline = VK_NULL_HANDLE;
    uint32_t displayWidth = 0, displayHeight = 0;
    std::string presentError;
    // The frontbuffer the last present drew from, for the frame dump.
    WorldSurfaces::PresentedFrame presented;

    uint64_t clears = 0, resolves = 0, presents = 0, rejected = 0;

    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage = nullptr;
    PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;

    VkCommandBuffer command = VK_NULL_HANDLE;

    bool loadEntryPoints(const char** error);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;

    bool createImage(Image& image, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                     VkImageUsageFlags usage, VkImageViewType viewType, const char** error);
    void destroyImage(Image& image);
    void transition(Image& image, VkImageLayout to, VkImageAspectFlags aspect, VkAccessFlags dstAccess,
                    VkPipelineStageFlags dstStage, uint32_t layerCount = 1, uint32_t baseLayer = 0);
    bool createColorPass(VkFormat format, VkAccessFlags dstAccess, VkRenderPass& pass, const char** error);
    bool createDepthPass(VkRenderPass& pass, const char** error);
    bool createDrawPass(bool withColor, bool withDepth, VkRenderPass& pass, const char** error);
    VkFramebuffer drawFramebuffer(uint64_t colorKey, uint64_t depthKey, uint32_t width, uint32_t height,
                                  bool withColor, const char** error);
    void releaseDrawFramebuffers();
    bool beginDraw(const Native::WorldDraw& draw, DrawTarget& target, const char** error);
    bool createCopyPipelines(const uint32_t* vertexWords, size_t vertexBytes,
                             const uint32_t* fragmentWords, size_t fragmentBytes,
                             const uint32_t* presentWords, size_t presentBytes, const char** error);
    bool createCopyPipeline(VkRenderPass pass, VkShaderModule fragment, VkPipeline& pipeline,
                            const char** error);
    VkShaderModule module(const uint32_t* words, size_t bytes) const;

    bool ensureSurface(uint64_t key, Policy::Extent requested, bool depth, Surface*& surface,
                       const char** error);
    bool createSurface(uint64_t key, Policy::Extent extent, bool depth, const char** error);
    bool growSurface(Surface& surface, Policy::Extent extent, const char** error);
    bool clearImage(Surface& surface, const Policy::ClearRegion& region, const float color[4],
                    float depth, uint32_t stencil, uint32_t flags, const char** error);
    Resolved* ensureResolved(const Native::WorldTexture& destination,
                             Policy::DestinationFormat format, const char** error);
    bool resolve(const Native::WorldResolve& engineCommand, const char** error);
    bool present(const Native::WorldTexture& texture, const char** error);
    bool presentView(VkImageView image,const char** error);
    bool allocateSet(VkImageView source, VkSampler sourceSampler, VkDescriptorSet& set,
                     const char** error);
};

// ---------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------

bool WorldSurfaces::Impl::loadEntryPoints(const char** error) {
    PFN_vkGetDeviceProcAddr getDeviceProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!getDeviceProc) {
        *error = "vkGetDeviceProcAddr is unavailable";
        return false;
    }
    bool complete = true;
    auto need = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::decay_t<decltype(slot)>>(getDeviceProc(device, name));
        if (!slot) {
            std::printf("[World] RADV does not provide %s\n", name);
            complete = false;
        }
    };
    need(CreateImage, "vkCreateImage");
    need(DestroyImage, "vkDestroyImage");
    need(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    need(CreateImageView, "vkCreateImageView");
    need(DestroyImageView, "vkDestroyImageView");
    need(CreateFramebuffer, "vkCreateFramebuffer");
    need(DestroyFramebuffer, "vkDestroyFramebuffer");
    need(CreateRenderPass, "vkCreateRenderPass");
    need(DestroyRenderPass, "vkDestroyRenderPass");
    need(CreateShaderModule, "vkCreateShaderModule");
    need(DestroyShaderModule, "vkDestroyShaderModule");
    need(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    need(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    need(CreatePipelineLayout, "vkCreatePipelineLayout");
    need(DestroyPipelineLayout, "vkDestroyPipelineLayout");
    need(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    need(DestroyPipeline, "vkDestroyPipeline");
    need(CreateDescriptorPool, "vkCreateDescriptorPool");
    need(DestroyDescriptorPool, "vkDestroyDescriptorPool");
    need(ResetDescriptorPool, "vkResetDescriptorPool");
    need(AllocateDescriptorSets, "vkAllocateDescriptorSets");
    need(UpdateDescriptorSets, "vkUpdateDescriptorSets");
    need(CreateSampler, "vkCreateSampler");
    need(DestroySampler, "vkDestroySampler");
    need(AllocateMemory, "vkAllocateMemory");
    need(FreeMemory, "vkFreeMemory");
    need(BindImageMemory, "vkBindImageMemory");
    need(CreateBuffer, "vkCreateBuffer");
    need(DestroyBuffer, "vkDestroyBuffer");
    need(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    need(BindBufferMemory, "vkBindBufferMemory");
    need(MapMemory, "vkMapMemory");
    need(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    need(CmdClearColorImage, "vkCmdClearColorImage");
    need(CmdClearDepthStencilImage, "vkCmdClearDepthStencilImage");
    need(CmdClearAttachments, "vkCmdClearAttachments");
    need(CmdCopyImage, "vkCmdCopyImage");
    need(CmdBeginRenderPass, "vkCmdBeginRenderPass");
    need(CmdEndRenderPass, "vkCmdEndRenderPass");
    need(CmdBindPipeline, "vkCmdBindPipeline");
    need(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    need(CmdPushConstants, "vkCmdPushConstants");
    need(CmdSetViewport, "vkCmdSetViewport");
    need(CmdSetScissor, "vkCmdSetScissor");
    need(CmdDraw, "vkCmdDraw");
    need(DeviceWaitIdle, "vkDeviceWaitIdle");
    GetMemoryProperties =
        Vk::instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!complete || !GetMemoryProperties) {
        *error = "a device entry point RADV should provide is missing";
        return false;
    }
    return true;
}

uint32_t WorldSurfaces::Impl::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties properties{};
    GetMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

bool WorldSurfaces::Impl::createImage(Image& image, VkFormat format, uint32_t width, uint32_t height,
                                      uint32_t layers, VkImageUsageFlags usage, VkImageViewType viewType,
                                      const char** error) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width * scale, height * scale, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (CreateImage(device, &info, nullptr, &image.image) != VK_SUCCESS) {
        *error = "vkCreateImage failed for a world surface";
        return false;
    }
    VkMemoryRequirements requirements{};
    GetImageMemoryRequirements(device, image.image, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || AllocateMemory(device, &allocation, nullptr, &image.memory) != VK_SUCCESS) {
        *error = "the world surface's GPU memory could not be allocated";
        return false;
    }
    if (BindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS) {
        *error = "vkBindImageMemory failed for a world surface";
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = viewType;
    viewInfo.format = format;
    viewInfo.subresourceRange = {imageAspect(format), 0, 1, 0, layers};
    if (CreateImageView(device, &viewInfo, nullptr, &image.attachment) != VK_SUCCESS) {
        *error = "vkCreateImageView failed for a world surface";
        return false;
    }
    viewInfo.subresourceRange.aspectMask = sampledAspect(format);
    if (viewInfo.subresourceRange.aspectMask == imageAspect(format)) {
        image.sampled = image.attachment;
    } else if (CreateImageView(device, &viewInfo, nullptr, &image.sampled) != VK_SUCCESS) {
        *error = "vkCreateImageView failed for a world surface's sampled view";
        return false;
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void WorldSurfaces::Impl::destroyImage(Image& image) {
    // A cached draw framebuffer names this image's view; it cannot outlive it.
    // The next draw rebuilds what it needs.
    if (image.image) ++surfaceEpoch;
    if (image.framebuffer) {
        DestroyFramebuffer(device, image.framebuffer, nullptr);
        image.framebuffer = VK_NULL_HANDLE;
    }
    if (image.sampled && image.sampled != image.attachment) {
        DestroyImageView(device, image.sampled, nullptr);
    }
    image.sampled = VK_NULL_HANDLE;
    if (image.attachment) {
        DestroyImageView(device, image.attachment, nullptr);
        image.attachment = VK_NULL_HANDLE;
    }
    if (image.image) {
        DestroyImage(device, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory) {
        FreeMemory(device, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
    image.width = image.height = 0;
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// A layout change with the access and stage masks its current state implies: the
// surfaces pass between being rendered into, being read and being copied, and
// each of those transitions needs the right pair or the driver may reorder them.
void WorldSurfaces::Impl::transition(Image& image, VkImageLayout to, VkImageAspectFlags aspect,
                                     VkAccessFlags dstAccess, VkPipelineStageFlags dstStage,
                                     uint32_t layerCount, uint32_t baseLayer) {
    if (image.image == VK_NULL_HANDLE || image.layout == to) return;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    switch (image.layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            srcAccess = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        default:
            srcAccess = 0;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
    }
    VkImageMemoryBarrier memory{};
    memory.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    memory.srcAccessMask = srcAccess;
    memory.dstAccessMask = dstAccess;
    memory.oldLayout = image.layout;
    memory.newLayout = to;
    memory.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    memory.image = image.image;
    memory.subresourceRange = {aspect, 0, 1, baseLayer, layerCount};
    CmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &memory);
    image.layout = to;
}

VkShaderModule WorldSurfaces::Impl::module(const uint32_t* words, size_t bytes) const {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule result = VK_NULL_HANDLE;
    return CreateShaderModule(device, &info, nullptr, &result) == VK_SUCCESS ? result : VK_NULL_HANDLE;
}

bool WorldSurfaces::Impl::createColorPass(VkFormat format, VkAccessFlags dstAccess, VkRenderPass& pass,
                                          const char** error) {
    // The attachment keeps its contents: the engine draws into a surface across
    // many commands and resolves only when the pass is finished, so a load-op
    // that discarded them would lose the scene between two commands.
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = dstAccess;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (CreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) {
        *error = "vkCreateRenderPass failed for a world surface";
        return false;
    }
    return true;
}

bool WorldSurfaces::Impl::createDepthPass(VkRenderPass& pass, const char** error) {
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depth;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (CreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) {
        *error = "vkCreateRenderPass failed for a world depth surface";
        return false;
    }
    return true;
}

bool WorldSurfaces::Impl::createDrawPass(bool withColor, bool withDepth, VkRenderPass& pass,
                                         const char** error) {
    // A draw pass loads every attachment: the scene it adds to is already there.
    // The pass's attachment count must match the framebuffer's, so colour alone,
    // colour + depth and depth alone are three passes.
    VkAttachmentDescription attachments[2]{};
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthReference{withColor ? 1u : 0u,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t attachmentCount = 0;
    if (withColor) {
        VkAttachmentDescription& color = attachments[attachmentCount++];
        color.format = kColorFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (withDepth) {
        VkAttachmentDescription& depth = attachments[attachmentCount++];
        depth.format = kDepthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = withColor ? 1u : 0u;
    subpass.pColorAttachments = withColor ? &colorReference : nullptr;
    subpass.pDepthStencilAttachment = withDepth ? &depthReference : nullptr;

    // Both stages that write these attachments are covered, so a draw that reads
    // a resolve from the previous command sees its pixels.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = attachmentCount;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (CreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) {
        *error = withColor ? (withDepth ? "vkCreateRenderPass failed for a world draw"
                                        : "vkCreateRenderPass failed for a colour-only world draw")
                           : "vkCreateRenderPass failed for a depth-only world draw";
        return false;
    }
    return true;
}

void WorldSurfaces::Impl::releaseDrawFramebuffers() {
    // A surface the engine grew was destroyed while a draw of this frame may
    // still name the framebuffer that bound it. The handles are retired and
    // destroyed at the start of the next frame instead.
    for (auto& entry : drawFramebuffers)
        if (entry.second) retiredFramebuffers.push_back(entry.second);
    drawFramebuffers.clear();
    framebufferEpoch = surfaceEpoch;
}

VkFramebuffer WorldSurfaces::Impl::drawFramebuffer(uint64_t colorKey, uint64_t depthKey, uint32_t width,
                                                   uint32_t height, bool withColor, const char** error) {
    if (framebufferEpoch != surfaceEpoch) releaseDrawFramebuffers();
    // The key holds both surfaces; a colour-only or depth-only draw uses zero for
    // the missing half, and the extent is implied by the pair because surfaces
    // never shrink.
    const uint64_t key = (colorKey * 0x9E3779B97F4A7C15ull) ^ (depthKey * 0xC2B2AE3D27D4EB4Full) ^
                         (uint64_t(width) << 32 | height);
    if (const auto found = drawFramebuffers.find(key); found != drawFramebuffers.end())
        return found->second;

    std::array<VkImageView, 2> views{};
    uint32_t viewCount = 0;
    if (withColor) {
        const auto color = surfaces.find(colorKey);
        if (color == surfaces.end()) {
            *error = "the draw named a colour surface that does not exist";
            return VK_NULL_HANDLE;
        }
        views[viewCount++] = color->second.attachment;
    }
    if (depthKey) {
        const auto depth = surfaces.find(depthKey);
        if (depth == surfaces.end()) {
            *error = "the draw named a depth surface that does not exist";
            return VK_NULL_HANDLE;
        }
        views[viewCount++] = depth->second.attachment;
    }
    if (!viewCount) {
        *error = "the draw named no attachment";
        return VK_NULL_HANDLE;
    }

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = withColor ? (depthKey ? drawPass : colorDrawPass) : depthDrawPass;
    info.attachmentCount = viewCount;
    info.pAttachments = views.data();
    info.width = width * scale;
    info.height = height * scale;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (CreateFramebuffer(device, &info, nullptr, &framebuffer) != VK_SUCCESS) {
        *error = "vkCreateFramebuffer failed for a world draw";
        return VK_NULL_HANDLE;
    }
    drawFramebuffers.emplace(key, framebuffer);
    return framebuffer;
}

bool WorldSurfaces::Impl::beginDraw(const Native::WorldDraw& draw, DrawTarget& target, const char** error) {
    if (command == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    // The original binds its pixel shader only when a colour attachment exists:
    // a depth-only material still draws through the fragment stage for alpha
    // testing, but writes no colour.
    const bool withColor = draw.targets[0] != 0u && draw.material != Native::WorldMaterial::depth;
    const bool withDepth = draw.targets[4] != 0u;
    if (!withColor && !withDepth) {
        *error = "the draw named no render target";
        return false;
    }
    if (!draw.viewport[2] || !draw.viewport[3]) {
        *error = "the draw has no viewport";
        return false;
    }

    // The original grows both attachments to the union so they always match; a
    // Vulkan framebuffer cannot bind two images of different extents.
    Policy::Extent extent{draw.viewport[0] + draw.viewport[2], draw.viewport[1] + draw.viewport[3]};
    const auto retain = [&](uint64_t key) {
        const auto found = surfaces.find(key);
        if (found == surfaces.end()) return;
        extent.width = (std::max)(extent.width, found->second.width);
        extent.height = (std::max)(extent.height, found->second.height);
    };
    if (withDepth) retain(draw.surfaceKey(4));
    if (withColor) retain(draw.surfaceKey(0));
    const uint64_t colorKey = withColor ? draw.surfaceKey(0) : 0;
    const uint64_t depthKey = withDepth ? draw.surfaceKey(4) : 0;
    if (drawPassOpen && activeColorKey == colorKey && activeDepthKey == depthKey &&
        activeTarget.width == extent.width && activeTarget.height == extent.height) {
        target = activeTarget;
        return true;
    }
    flushDraws();

    Surface* color = nullptr;
    Surface* depth = nullptr;
    if (withColor && !ensureSurface(draw.surfaceKey(0), extent, false, color, error)) return false;
    if (withDepth && !ensureSurface(draw.surfaceKey(4), extent, true, depth, error)) return false;

    const VkFramebuffer framebuffer =
        drawFramebuffer(withColor ? draw.surfaceKey(0) : 0, withDepth ? draw.surfaceKey(4) : 0,
                        extent.width, extent.height, withColor, error);
    if (!framebuffer) return false;

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = withColor ? (withDepth ? drawPass : colorDrawPass) : depthDrawPass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {extent.width * scale, extent.height * scale}};
    CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    drawPassOpen = true;

    target.pass = begin.renderPass;
    target.framebuffer = framebuffer;
    target.width = extent.width;
    target.height = extent.height;
    target.color = withColor;
    target.depth = withDepth;
    target.serial = ++renderPasses;
    activeTarget = target;
    activeColorKey = colorKey;
    activeDepthKey = depthKey;
    return true;
}

bool WorldSurfaces::Impl::createCopyPipeline(VkRenderPass pass, VkShaderModule fragment,
                                             VkPipeline& pipeline, const char** error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = transferVertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    // No vertex buffer and no input layout: the triangle is generated from the
    // vertex index, exactly as the D3D11 resolve's `engine_resolve_copy` does.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // The viewport and scissor carry the copy rectangle.
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthState{};
    depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthState.depthTestEnable = VK_FALSE;
    depthState.depthWriteEnable = VK_FALSE;

    // A copy writes the source's value; D3D11 binds no blend state for it.
    VkPipelineColorBlendAttachmentState blending{};
    blending.blendEnable = VK_FALSE;
    blending.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blending;

    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;

    VkGraphicsPipelineCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertexInput;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pDepthStencilState = &depthState;
    create.pColorBlendState = &colorBlend;
    create.pDynamicState = &dynamic;
    create.layout = pipelineLayout;
    create.renderPass = pass;
    create.subpass = 0;
    if (CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline) != VK_SUCCESS) {
        *error = "vkCreateGraphicsPipelines failed for the resolve copy";
        return false;
    }
    return true;
}

bool WorldSurfaces::Impl::createCopyPipelines(const uint32_t* vertexWords, size_t vertexBytes,
                                              const uint32_t* fragmentWords, size_t fragmentBytes,
                                              const uint32_t* presentWords, size_t presentBytes,
                                              const char** error) {
    transferVertex = module(vertexWords, vertexBytes);
    transferFragment = module(fragmentWords, fragmentBytes);
    presentFragment = module(presentWords, presentBytes);
    if (!transferVertex || !transferFragment || !presentFragment) {
        *error = "a world copy shader module is invalid";
        return false;
    }

    // Binding 0 is the image a copy reads; binding 1 the display ramp, which
    // only the present reads.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        *error = "vkCreateDescriptorSetLayout failed for the resolve copy";
        return false;
    }

    // The engine's `Transfer` constants: the source rectangle's origin and the
    // destination origin plus the fetch scale, as one 32-byte block.
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = 8 * sizeof(float);
    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &setLayout;
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &range;
    if (CreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        *error = "vkCreatePipelineLayout failed for the resolve copy";
        return false;
    }

    // The copy reads by integer texel, so filtering never applies; nearest is
    // what the original implied and what a texture write must not smooth.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (CreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        *error = "vkCreateSampler failed for the resolve copy";
        return false;
    }
    // The present fits the frontbuffer to the panel, so it filters: at equal
    // extents a sample lands on a texel centre and returns it unchanged.
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (CreateSampler(device, &samplerInfo, nullptr, &presentSampler) != VK_SUCCESS) {
        *error = "vkCreateSampler failed for the frontbuffer copy";
        return false;
    }

    for (uint32_t format = 1; format < resolvePasses.size(); ++format) {
        if (!createCopyPipeline(resolvePasses[format], transferFragment, resolvePipelines[format], error))
            return false;
    }

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = kDescriptorSets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = kDescriptorSets;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kDescriptorSets;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (CreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        *error = "vkCreateDescriptorPool failed for the resolve copy";
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(Native::DisplayGamma{}.entries);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (CreateBuffer(device, &bufferInfo, nullptr, &gammaBuffer) != VK_SUCCESS) {
        *error = "vkCreateBuffer failed for the display ramp";
        return false;
    }
    VkMemoryRequirements requirements{};
    GetBufferMemoryRequirements(device, gammaBuffer, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || AllocateMemory(device, &allocation, nullptr, &gammaMemory) != VK_SUCCESS ||
        BindBufferMemory(device, gammaBuffer, gammaMemory, 0) != VK_SUCCESS ||
        MapMemory(device, gammaMemory, 0, VK_WHOLE_SIZE, 0, &gammaMapped) != VK_SUCCESS) {
        *error = "the display ramp's memory could not be allocated";
        return false;
    }
    std::memset(gammaMapped, 0, bufferInfo.size);
    return true;
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

bool WorldSurfaces::Impl::createSurface(uint64_t key, Policy::Extent extent, bool depth, const char** error) {
    Surface fresh;
    fresh.depth = depth;
    const VkImageUsageFlags usage =
        depth ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
              : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!createImage(fresh, depth ? kDepthFormat : kColorFormat, extent.width, extent.height, 1, usage,
                     VK_IMAGE_VIEW_TYPE_2D, error)) {
        destroyImage(fresh);
        return false;
    }

    // The clear pass's framebuffer. A later draw pass will add one that binds
    // colour and depth together; nothing needs that until draws exist.
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = depth ? depthClearPass : colorClearPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &fresh.attachment;
    framebufferInfo.width = extent.width * scale;
    framebufferInfo.height = extent.height * scale;
    framebufferInfo.layers = 1;
    if (CreateFramebuffer(device, &framebufferInfo, nullptr, &fresh.framebuffer) != VK_SUCCESS) {
        *error = "vkCreateFramebuffer failed for a world surface";
        destroyImage(fresh);
        return false;
    }

    // A new surface is cleared, as the original clears every target it allocates:
    // a resolve of a region nothing drew would otherwise read uninitialised
    // memory onto the screen.
    Policy::ClearRegion fresh0;
    fresh0.valid = true;
    fresh0.full = true;
    fresh0.rect = {0, 0, int32_t(extent.width), int32_t(extent.height)};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!clearImage(fresh, fresh0, zero, 0.0f, 0, depth ? 48u : 1u, error)) {
        destroyImage(fresh);
        return false;
    }
    surfaces.emplace(key, std::move(fresh));
    return true;
}

bool WorldSurfaces::Impl::growSurface(Surface& surface, Policy::Extent extent, const char** error) {
    Surface grown;
    grown.depth = surface.depth;
    const VkImageUsageFlags usage =
        surface.depth ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                      : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!createImage(grown, surface.format, extent.width, extent.height, 1, usage, VK_IMAGE_VIEW_TYPE_2D,
                     error)) {
        destroyImage(grown);
        return false;
    }
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = surface.depth ? depthClearPass : colorClearPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &grown.attachment;
    framebufferInfo.width = extent.width * scale;
    framebufferInfo.height = extent.height * scale;
    framebufferInfo.layers = 1;
    if (CreateFramebuffer(device, &framebufferInfo, nullptr, &grown.framebuffer) != VK_SUCCESS) {
        *error = "vkCreateFramebuffer failed for a grown world surface";
        destroyImage(grown);
        return false;
    }

    // The border the old surface did not cover keeps the cleared zeroes a new
    // allocation has, exactly as the original's CopySubresourceRegion leaves
    // them; the drawn pixels move across unchanged.
    Policy::ClearRegion fresh0;
    fresh0.valid = true;
    fresh0.full = true;
    fresh0.rect = {0, 0, int32_t(extent.width), int32_t(extent.height)};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!clearImage(grown, fresh0, zero, 0.0f, 0, surface.depth ? 48u : 1u, error)) {
        destroyImage(grown);
        return false;
    }

    const VkImageAspectFlags aspect = imageAspect(surface.format);
    const VkImageLayout working = surface.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAccessFlags write =
        surface.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags stage =
        surface.depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    transition(surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition(grown, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {(std::min)(surface.width, extent.width) * scale, (std::min)(surface.height, extent.height) * scale, 1};
    CmdCopyImage(command, surface.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, grown.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(surface, working, aspect, write, stage);
    transition(grown, working, aspect, write, stage);

    // The copy above, and this frame's earlier draws, still use the old image:
    // it is retired, not freed, until the GPU has finished the frame.
    ++surfaceEpoch;
    retiredImages.push_back(static_cast<const Image&>(surface));
    static_cast<Image&>(surface) = static_cast<const Image&>(grown);
    return true;
}

bool WorldSurfaces::Impl::ensureSurface(uint64_t key, Policy::Extent requested, bool depth,
                                        Surface*& surface, const char** error) {
    if (!key) {
        *error = "the command named no surface";
        return false;
    }
    Policy::Extent extent;
    if (!Policy::grow(requested, {}, extent)) {
        *error = "the surface extent is empty or beyond the GPU limit";
        return false;
    }
    auto found = surfaces.find(key);
    if (found != surfaces.end()) {
        if (found->second.depth != depth) {
            // A key answers for one kind of attachment; the guest reusing one for
            // the other is a scene change, not a resize.
            retiredImages.push_back(static_cast<const Image&>(found->second));
            ++surfaceEpoch;
            surfaces.erase(found);
        } else {
            if (!Policy::grow(requested, {found->second.width, found->second.height}, extent)) {
                *error = "the surface extent is empty or beyond the GPU limit";
                return false;
            }
            if (extent.width != found->second.width || extent.height != found->second.height) {
                if (!growSurface(found->second, extent, error)) return false;
            }
            surface = &found->second;
            return true;
        }
    }
    if (surfaces.size() >= kMaxSurfaces) {
        *error = "the world surface cache is full";
        return false;
    }
    if (!createSurface(key, extent, depth, error)) return false;
    surface = &surfaces[key];
    return true;
}

// ---------------------------------------------------------------------------
// Clears
// ---------------------------------------------------------------------------

bool WorldSurfaces::Impl::clearImage(Surface& surface, const Policy::ClearRegion& region,
                                     const float color[4], float depth, uint32_t stencil, uint32_t flags,
                                     const char** error) {
    (void)error;
    if (!region.valid) return true; // nothing of the surface is inside the region
    const VkImageAspectFlags aspect = imageAspect(surface.format);
    const VkImageLayout working = surface.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAccessFlags write =
        surface.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags stage =
        surface.depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (region.full) {
        // A whole-surface clear is a transfer clear: no pass, no geometry, and
        // it covers the allocation's padding as well.
        // A stencil-only clear is used between lighting/shadow passes. Clearing
        // both planes here also discarded the scene's prepass depth, so later
        // EQUAL lighting could no longer find the surfaces it was shading.
        const VkImageAspectFlags clearAspect = surface.depth
            ? ((flags & 16u ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
               (flags & 32u ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u))
            : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!clearAspect) return true;
        transition(surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkImageSubresourceRange range{clearAspect, 0, 1, 0, 1};
        if (surface.depth) {
            VkClearDepthStencilValue value{};
            value.depth = (std::min)((std::max)(depth, 0.0f), 1.0f);
            value.stencil = stencil;
            CmdClearDepthStencilImage(command, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                      &range);
        } else {
            VkClearColorValue value{};
            std::memcpy(value.float32, color, sizeof(value.float32));
            CmdClearColorImage(command, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        }
        transition(surface, working, aspect, write, stage);
        return true;
    }

    VkClearAttachment attachment{};
    if (surface.depth) {
        // The original's clear helper writes depth and replaces stencil only for
        // the planes the command names.
        if (flags & 16u) attachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & 32u) attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!attachment.aspectMask) return true;
        attachment.clearValue.depthStencil.depth = (std::min)((std::max)(depth, 0.0f), 1.0f);
        attachment.clearValue.depthStencil.stencil = uint8_t(stencil);
    } else {
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        std::memcpy(attachment.clearValue.color.float32, color, sizeof(attachment.clearValue.color.float32));
    }

    // The pass keeps the pixels outside the rectangle: the engine clears part of
    // a target it is still drawing into.
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = surface.depth ? depthClearPass : colorClearPass;
    begin.framebuffer = surface.framebuffer;
    begin.renderArea = {{0, 0}, {surface.width * scale, surface.height * scale}};
    CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkClearRect rect{};
    rect.rect = {{region.rect[0] * int32_t(scale), region.rect[1] * int32_t(scale)},
                 {uint32_t(region.rect[2] - region.rect[0]) * scale, uint32_t(region.rect[3] - region.rect[1]) * scale}};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    CmdClearAttachments(command, 1, &attachment, 1, &rect);
    CmdEndRenderPass(command);
    return true;
}

// ---------------------------------------------------------------------------
// Resolves
// ---------------------------------------------------------------------------

Resolved* WorldSurfaces::Impl::ensureResolved(const Native::WorldTexture& destination,
                                              Policy::DestinationFormat destinationFormat,
                                              const char** error) {
    // The framebuffer's pass fixes the attachment's format, so the image and the
    // pass that renders into it are chosen together.
    const VkRenderPass pass = resolvePasses[uint32_t(destinationFormat)];
    const VkFormat format = resolveFormat(destinationFormat);
    const uint64_t key = destination.key();
    const uint32_t faces = destination.faces;
    auto release = [&](Resolved& image) {
        for (uint32_t face = 0; face < 6; ++face) {
            if (image.faceFramebuffers[face]) {
                DestroyFramebuffer(device, image.faceFramebuffers[face], nullptr);
                image.faceFramebuffers[face] = VK_NULL_HANDLE;
            }
            if (image.faceViews[face]) {
                DestroyImageView(device, image.faceViews[face], nullptr);
                image.faceViews[face] = VK_NULL_HANDLE;
            }
        }
        destroyImage(image);
    };

    auto found = resolved.find(key);
    if (found != resolved.end()) {
        Resolved& existing = found->second;
        if (existing.width == destination.width && existing.height == destination.height &&
            existing.format == format && existing.faces == faces)
            return &existing;
        // The original allocates a new texture when the guest's format or extent
        // changes, clearing it; the old contents are not carried over.
        // Earlier resolves and descriptor sets in this frame still name it.
        retiredResolved.push_back(existing);
        resolved.erase(found);
    }
    if (resolved.size() >= kMaxResolved) {
        *error = "the resolved texture cache is full";
        return nullptr;
    }

    Resolved fresh;
    fresh.faces = faces;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!createImage(fresh, format, destination.width, destination.height, faces, usage,
                     faces == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, error)) {
        release(fresh);
        return nullptr;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = pass;
    framebufferInfo.width = destination.width * scale;
    framebufferInfo.height = destination.height * scale;
    framebufferInfo.layers = 1;
    if (faces == 1) {
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &fresh.attachment;
        if (CreateFramebuffer(device, &framebufferInfo, nullptr, &fresh.framebuffer) != VK_SUCCESS) {
            *error = "vkCreateFramebuffer failed for a resolved texture";
            release(fresh);
            return nullptr;
        }
    } else {
        for (uint32_t face = 0; face < faces; ++face) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = fresh.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
            if (CreateImageView(device, &viewInfo, nullptr, &fresh.faceViews[face]) != VK_SUCCESS) {
                *error = "vkCreateImageView failed for a resolved cube face";
                release(fresh);
                return nullptr;
            }
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &fresh.faceViews[face];
            if (CreateFramebuffer(device, &framebufferInfo, nullptr, &fresh.faceFramebuffers[face]) !=
                VK_SUCCESS) {
                *error = "vkCreateFramebuffer failed for a resolved cube face";
                release(fresh);
                return nullptr;
            }
        }
    }

    if (faces == 6) {
        // The attachment view is a 2D array (a face is rendered one layer at a
        // time); the original shaders read a resolved cube as TextureCube, which
        // needs a cube view over the same six layers.
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = fresh.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faces};
        VkImageView cube = VK_NULL_HANDLE;
        if (CreateImageView(device, &viewInfo, nullptr, &cube) != VK_SUCCESS) {
            *error = "vkCreateImageView failed for a resolved cube";
            release(fresh);
            return nullptr;
        }
        fresh.sampled = cube;
    }

    // A new resolved texture is transparent black, which is what the original
    // clears a freshly allocated one to and what a partial resolve leaves around
    // its copy.
    transition(fresh, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, faces);
    VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faces};
    CmdClearColorImage(command, fresh.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    transition(fresh, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, faces);

    Resolved& stored = resolved.emplace(key, std::move(fresh)).first->second;
    return &stored;
}

bool WorldSurfaces::Impl::resolve(const Native::WorldResolve& engineCommand, const char** error) {
    if (!engineCommand.destination.object) {
        *error = Policy::resolveStatusName(Policy::ResolveStatus::descriptor);
        ++rejected;
        return false;
    }

    Policy::ResolveRequest request;
    request.flags = engineCommand.flags;
    request.rectangle = engineCommand.rectangle;
    request.offset = engineCommand.offset;
    request.destinationWidth = engineCommand.destination.width;
    request.destinationHeight = engineCommand.destination.height;
    request.destinationFormat = engineCommand.destination.format;
    request.faces = engineCommand.destination.faces;
    request.face = engineCommand.face;
    request.mip = engineCommand.mip;
    request.viewport = engineCommand.viewport;
    const uint32_t attachment = engineCommand.flags & 7u;
    const bool depth = attachment == 4u;
    if (attachment < engineCommand.targets.size() && engineCommand.targets[attachment]) {
        const auto source = surfaces.find(engineCommand.surfaceKey(attachment));
        if (source != surfaces.end()) request.source = {source->second.width, source->second.height};
    }

    Policy::ResolvePlan plan;
    const Policy::ResolveStatus status = Policy::planResolve(request, plan);
    if (status != Policy::ResolveStatus::ok) {
        *error = Policy::resolveStatusName(status);
        ++rejected;
        return false;
    }

    Surface* source = nullptr;
    if (!ensureSurface(engineCommand.surfaceKey(attachment), plan.source, depth, source, error)) {
        ++rejected;
        return false;
    }
    Resolved* destination = ensureResolved(engineCommand.destination, plan.format, error);
    if (!destination) {
        ++rejected;
        return false;
    }

    // The copy reads the surface through the fragment stage, and the surface goes
    // back to being a render target afterwards: the engine keeps drawing its
    // scene after it has been resolved into a texture.
    const VkImageAspectFlags sourceAspect = imageAspect(source->format);
    const VkImageLayout sourceWorking = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                              : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition(*source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sourceAspect, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    transition(*destination, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocateSet(source->sampled, sampler, set, error)) {
        ++rejected;
        return false;
    }

    const uint32_t copiedWidth = plan.copied[2] - plan.copied[0];
    const uint32_t copiedHeight = plan.copied[3] - plan.copied[1];
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = resolvePasses[uint32_t(plan.format)];
    begin.framebuffer = destination->faces == 1 ? destination->framebuffer
                                                : destination->faceFramebuffers[plan.face];
    begin.renderArea = {{0, 0}, {destination->width * scale, destination->height * scale}};
    CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{float(plan.offset[0] * scale), float(plan.offset[1] * scale), float(copiedWidth * scale),
                              float(copiedHeight * scale), 0.0f, 1.0f};
    CmdSetViewport(command, 0, 1, &viewport);
    const VkRect2D scissor{{int32_t(plan.offset[0] * scale), int32_t(plan.offset[1] * scale)}, {copiedWidth * scale, copiedHeight * scale}};
    CmdSetScissor(command, 0, 1, &scissor);

    CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resolvePipelines[uint32_t(plan.format)]);
    CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
    struct Transfer {
        float area[4];
        float originScale[4];
    } push{};
    push.area[0] = float(plan.copied[0] * scale);
    push.area[1] = float(plan.copied[1] * scale);
    push.area[2] = float(plan.copied[2] * scale);
    push.area[3] = float(plan.copied[3] * scale);
    push.originScale[0] = float(plan.offset[0] * scale);
    push.originScale[1] = float(plan.offset[1] * scale);
    // The engine's fetch exponent: what makes a half-float surface readable as
    // the unsigned format the guest asked for.
    push.originScale[2] = std::exp2(float(engineCommand.exponent));
    CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    CmdDraw(command, 3, 1, 0, 0);
    CmdEndRenderPass(command);

    // A resolved texture is read as a texture from now on, by the frontbuffer
    // copy and by whatever draws sample it.
    transition(*destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    transition(*source, sourceWorking, sourceAspect,
               depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                     : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    ++resolves;

    // A resolve can reset the region it copied, in the source, for the next
    // effect pass: the original clears its rounded rectangle, not the clipped one.
    const std::array<int32_t, 4> expanded =
        Policy::expandedResolveRectangle(engineCommand.rectangle);
    if (engineCommand.flags & 0x100u) {
        Policy::ClearRequest reset;
        reset.flags = 1;
        reset.rectangle = expanded;
        reset.surface = {source->width, source->height};
        clearImage(*source, Policy::planClear(reset), engineCommand.color.data(), engineCommand.depth,
                   engineCommand.stencil, 1, error);
    }
    if ((engineCommand.flags & 0x200u) && engineCommand.targets[4]) {
        const auto sourceDepth = surfaces.find(engineCommand.surfaceKey(4));
        if (sourceDepth != surfaces.end()) {
            Policy::ClearRequest reset;
            reset.flags = 48;
            reset.rectangle = expanded;
            reset.surface = {sourceDepth->second.width, sourceDepth->second.height};
            clearImage(sourceDepth->second, Policy::planClear(reset), engineCommand.color.data(),
                       engineCommand.depth, engineCommand.stencil, 48, error);
        }
    }
    return true;
}

bool WorldSurfaces::Impl::allocateSet(VkImageView source, VkSampler sourceSampler, VkDescriptorSet& set,
                                      const char** error) {
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    if (AllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
        *error = "the copy descriptor sets are exhausted for this frame";
        return false;
    }
    const VkDescriptorImageInfo image{sourceSampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    const VkDescriptorBufferInfo ramp{gammaBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{write, write};
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pImageInfo = nullptr;
    writes[1].pBufferInfo = &ramp;
    UpdateDescriptorSets(device, 2, writes, 0, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Frontbuffer
// ---------------------------------------------------------------------------

bool WorldSurfaces::Impl::present(const Native::WorldTexture& texture, const char** error) {
    if (!presentPipeline || !swapchainPass) {
        *error = "the swapchain is not bound to the world renderer";
        ++rejected;
        return false;
    }
    const auto found = resolved.find(texture.key());
    if (found == resolved.end()) {
        *error = "no resolved frontbuffer";
        ++rejected;
        return false;
    }
    Resolved& frontbuffer = found->second;
    if (!Policy::presentable({frontbuffer.width, frontbuffer.height}, {displayWidth, displayHeight})) {
        *error = "frontbuffer-extent";
        ++rejected;
        return false;
    }
    if (frontbuffer.faces != 1) {
        *error = "a cube cannot be a frontbuffer";
        ++rejected;
        return false;
    }

    if(!presentView(frontbuffer.sampled,error)) {
        ++rejected;
        return false;
    }
    presented = {frontbuffer.image, frontbuffer.width * scale, frontbuffer.height * scale, frontbuffer.format};
    ++presents;
    return true;
}
bool WorldSurfaces::Impl::presentView(VkImageView image,const char** error) {
    if(!image || !presentPipeline || !swapchainPass) {*error="no presentation image or pipeline";return false;}
    VkDescriptorSet set=VK_NULL_HANDLE;
    if(!allocateSet(image,presentSampler,set,error))return false;

    const VkViewport viewport{0.0f, 0.0f, float(displayWidth), float(displayHeight), 0.0f, 1.0f};
    CmdSetViewport(command, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {displayWidth, displayHeight}};
    CmdSetScissor(command, 0, 1, &scissor);
    CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipeline);
    CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
    struct Present {
        float extent[4];
        float padding[4];
    } push{};
    push.extent[0] = float(displayWidth);
    push.extent[1] = float(displayHeight);
    push.extent[2] = float(gammaMode);
    const auto settings = Native::graphicsSettings();
    push.padding[0] = settings.brightnessPercent / 100.0f;
    push.padding[1] = settings.antialiasing == Native::AntialiasingMode::FXAA ? 1.0f : 0.0f;
    CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    CmdDraw(command, 3, 1, 0, 0);
    return true;
}

// ---------------------------------------------------------------------------
// The title-facing interface
// ---------------------------------------------------------------------------

WorldSurfaces::~WorldSurfaces() { Destroy(); }

bool WorldSurfaces::Create(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                           const uint32_t* transferVertex, size_t vertexBytes,
                           const uint32_t* transferFragment, size_t fragmentBytes,
                           const uint32_t* presentFragment, size_t presentBytes, const char** error) {
    Destroy();
    impl_ = new Impl();
    Impl& impl = *impl_;
    impl.instance = instance;
    impl.scale = (Native::graphicsSettings().renderHeight + 719) / 720;
    impl.physical = physical;
    impl.device = device;
    const auto abandon = [&](const char* why) {
        *error = why;
        Destroy();
        return false;
    };
    if (!impl.loadEntryPoints(error)) return abandon(*error);

    // The passes: a colour surface's clears, a depth surface's clears, one per
    // resolve format, and the two a world draw renders through.
    if (!impl.createColorPass(kColorFormat, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, impl.colorClearPass, error))
        return abandon(*error);
    if (!impl.createDepthPass(impl.depthClearPass, error)) return abandon(*error);
    if (!impl.createDrawPass(true, true, impl.drawPass, error)) return abandon(*error);
    if (!impl.createDrawPass(true, false, impl.colorDrawPass, error)) return abandon(*error);
    if (!impl.createDrawPass(false, true, impl.depthDrawPass, error)) return abandon(*error);
    for (uint32_t format = 1; format < impl.resolvePasses.size(); ++format) {
        const VkFormat imageFormat = resolveFormat(Policy::DestinationFormat(format));
        if (imageFormat == VK_FORMAT_UNDEFINED) continue;
        if (!impl.createColorPass(imageFormat,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                  impl.resolvePasses[format], error))
            return abandon(*error);
    }
    if (!impl.createCopyPipelines(transferVertex, vertexBytes, transferFragment, fragmentBytes,
                                  presentFragment, presentBytes, error))
        return abandon(*error);
    return true;
}

void WorldSurfaces::Destroy() {
    if (!impl_) return;
    Impl& impl = *impl_;
    if (impl.device != VK_NULL_HANDLE && impl.DeviceWaitIdle) impl.DeviceWaitIdle(impl.device);
    if (!impl.DestroyImage) {
        // The device's entry points never loaded, so nothing was created.
        delete impl_;
        impl_ = nullptr;
        command_ = VK_NULL_HANDLE;
        return;
    }
    if (impl.presentPipeline && impl.DestroyPipeline)
        impl.DestroyPipeline(impl.device, impl.presentPipeline, nullptr);
    impl.presentPipeline = VK_NULL_HANDLE;
    impl.releaseDrawFramebuffers();
    for (VkFramebuffer framebuffer : impl.retiredFramebuffers)
        impl.DestroyFramebuffer(impl.device, framebuffer, nullptr);
    impl.retiredFramebuffers.clear();
    for (auto& image : impl.retiredResolved) {
        for (auto framebuffer : image.faceFramebuffers)
            if (framebuffer) impl.DestroyFramebuffer(impl.device, framebuffer, nullptr);
        for (auto view : image.faceViews)
            if (view) impl.DestroyImageView(impl.device, view, nullptr);
        impl.destroyImage(image);
    }
    impl.retiredResolved.clear();
    for (auto& image : impl.retiredImages) impl.destroyImage(image);
    impl.retiredImages.clear();
    for (auto& entry : impl.resolved) {
        for (uint32_t face = 0; face < 6; ++face) {
            if (entry.second.faceFramebuffers[face] && impl.DestroyFramebuffer)
                impl.DestroyFramebuffer(impl.device, entry.second.faceFramebuffers[face], nullptr);
            if (entry.second.faceViews[face] && impl.DestroyImageView)
                impl.DestroyImageView(impl.device, entry.second.faceViews[face], nullptr);
        }
        impl.destroyImage(entry.second);
    }
    impl.resolved.clear();
    for (auto& entry : impl.surfaces) impl.destroyImage(entry.second);
    impl.surfaces.clear();
    for (VkPipeline& pipeline : impl.resolvePipelines) {
        if (pipeline) impl.DestroyPipeline(impl.device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (impl.descriptorPool) impl.DestroyDescriptorPool(impl.device, impl.descriptorPool, nullptr);
    if (impl.gammaBuffer) impl.DestroyBuffer(impl.device, impl.gammaBuffer, nullptr);
    if (impl.gammaMemory) impl.FreeMemory(impl.device, impl.gammaMemory, nullptr);
    if (impl.sampler) impl.DestroySampler(impl.device, impl.sampler, nullptr);
    if (impl.presentSampler) impl.DestroySampler(impl.device, impl.presentSampler, nullptr);
    if (impl.pipelineLayout) impl.DestroyPipelineLayout(impl.device, impl.pipelineLayout, nullptr);
    if (impl.setLayout) impl.DestroyDescriptorSetLayout(impl.device, impl.setLayout, nullptr);
    if (impl.transferVertex) impl.DestroyShaderModule(impl.device, impl.transferVertex, nullptr);
    if (impl.transferFragment) impl.DestroyShaderModule(impl.device, impl.transferFragment, nullptr);
    if (impl.presentFragment) impl.DestroyShaderModule(impl.device, impl.presentFragment, nullptr);
    if (impl.colorClearPass) impl.DestroyRenderPass(impl.device, impl.colorClearPass, nullptr);
    if (impl.depthClearPass) impl.DestroyRenderPass(impl.device, impl.depthClearPass, nullptr);
    if (impl.drawPass) impl.DestroyRenderPass(impl.device, impl.drawPass, nullptr);
    if (impl.colorDrawPass) impl.DestroyRenderPass(impl.device, impl.colorDrawPass, nullptr);
    if (impl.depthDrawPass) impl.DestroyRenderPass(impl.device, impl.depthDrawPass, nullptr);
    for (VkRenderPass& pass : impl.resolvePasses) {
        if (pass) impl.DestroyRenderPass(impl.device, pass, nullptr);
        pass = VK_NULL_HANDLE;
    }
    delete impl_;
    impl_ = nullptr;
    command_ = VK_NULL_HANDLE;
}

bool WorldSurfaces::SetSwapchain(VkRenderPass pass, uint32_t width, uint32_t height, const char** error) {
    if (!impl_) {
        *error = "the world surfaces are not created";
        return false;
    }
    Impl& impl = *impl_;
    if (impl.presentPipeline) {
        impl.DestroyPipeline(impl.device, impl.presentPipeline, nullptr);
        impl.presentPipeline = VK_NULL_HANDLE;
    }
    impl.swapchainPass = pass;
    impl.displayWidth = width;
    impl.displayHeight = height;
    if (!pass) {
        // A rebuild releases the pipeline with the pass it was recorded against.
        impl.displayWidth = impl.displayHeight = 0;
        return true;
    }
    if (!impl.createCopyPipeline(pass, impl.presentFragment, impl.presentPipeline, error)) return false;
    std::printf("[World] frontbuffer copies into %ux%u\n", width, height);
    std::fflush(stdout);
    return true;
}

void WorldSurfaces::BeginFrame() {
    if (!impl_) return;
    for (VkFramebuffer framebuffer : impl_->retiredFramebuffers)
        impl_->DestroyFramebuffer(impl_->device, framebuffer, nullptr);
    impl_->retiredFramebuffers.clear();
    // Cached draw framebuffers must be destroyed before their retired views.
    if (impl_->framebufferEpoch != impl_->surfaceEpoch) {
        impl_->releaseDrawFramebuffers();
        for (VkFramebuffer framebuffer : impl_->retiredFramebuffers)
            impl_->DestroyFramebuffer(impl_->device, framebuffer, nullptr);
        impl_->retiredFramebuffers.clear();
        impl_->framebufferEpoch = impl_->surfaceEpoch;
    }
    for (auto& image : impl_->retiredResolved) {
        for (auto framebuffer : image.faceFramebuffers)
            if (framebuffer) impl_->DestroyFramebuffer(impl_->device, framebuffer, nullptr);
        for (auto view : image.faceViews)
            if (view) impl_->DestroyImageView(impl_->device, view, nullptr);
        impl_->destroyImage(image);
    }
    impl_->retiredResolved.clear();
    for (Image& image : impl_->retiredImages) impl_->destroyImage(image);
    impl_->retiredImages.clear();
    impl_->ResetDescriptorPool(impl_->device, impl_->descriptorPool, 0);
    command_ = VK_NULL_HANDLE;
}

void WorldSurfaces::SetDisplayGamma(const Native::DisplayGamma& gamma) {
    if (!impl_ || !impl_->gammaMapped) return;
    // BeginFrame waited for the queue, so no submitted present still reads the
    // old ramp; a change mid-frame applies to this frame's present, as the
    // Windows preview's calibrated copy does.
    std::memcpy(impl_->gammaMapped, gamma.entries.data(), sizeof(gamma.entries));
    impl_->gammaMode = gamma.piecewise ? 2u : 1u;
    static int reported = 0;
    if (reported++ < 8) {
        // Black, mid-grey and white per channel, as the ramp maps them.
        const auto& e = gamma.entries;
        const unsigned mid = gamma.piecewise ? 64u : 128u, top = gamma.piecewise ? 127u : 255u;
        std::printf("[Display] ramp %s: r %u/%u/%u g %u/%u/%u b %u/%u/%u\n",
                    gamma.piecewise ? "piecewise" : "256", e[0][0], e[mid * 3][0], e[top * 3][0], e[1][0],
                    e[mid * 3 + 1][0], e[top * 3 + 1][0], e[2][0], e[mid * 3 + 2][0], e[top * 3 + 2][0]);
    }
}

bool WorldSurfaces::Clear(const Native::WorldClear& clear, const char** error) {
    FlushDraws();
    if (!impl_ || command_ == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    Impl& impl = *impl_;
    impl.command = command_;
    // The viewport names the extent a clear allocates its target at, as the
    // original's clear does.
    const Policy::Extent requested{clear.viewport[0] + clear.viewport[2],
                                   clear.viewport[1] + clear.viewport[3]};
    bool cleared = false;
    auto clearOne = [&](unsigned slot, bool depth) {
        if (!clear.targets[slot]) return true;
        Surface* surface = nullptr;
        if (!impl.ensureSurface(clear.surfaceKey(slot), requested, depth, surface, error)) return false;
        Policy::ClearRequest request;
        request.flags = clear.flags;
        request.viewport = clear.viewport;
        request.rectangle = clear.rectangle;
        request.surface = {surface->width, surface->height};
        const Policy::ClearRegion region = Policy::planClear(request);
        if (!region.valid) return true; // an empty region clears nothing
        if (!impl.clearImage(*surface, region, clear.color.data(), clear.depth, clear.stencil, clear.flags,
                             error))
            return false;
        cleared = true;
        return true;
    };
    if (clear.flags & 1u) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (!clearOne(slot, false)) {
                ++impl.rejected;
                return false;
            }
        }
    }
    if ((clear.flags & 48u) && !clearOne(4, true)) {
        ++impl.rejected;
        return false;
    }
    if (cleared) ++impl.clears;
    return true;
}

bool WorldSurfaces::Resolve(const Native::WorldResolve& resolve, const char** error) {
    FlushDraws();
    if (!impl_ || command_ == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    impl_->command = command_;
    return impl_->resolve(resolve, error);
}

bool WorldSurfaces::Present(const Native::WorldTexture& texture, const char** error) {
    FlushDraws();
    if (!impl_ || command_ == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    impl_->command = command_;
    return impl_->present(texture, error);
}
bool WorldSurfaces::PresentGenerated(VkImageView image,const char** error) {
    FlushDraws();
    if(!impl_ || command_==VK_NULL_HANDLE) {*error="no frame is being recorded";return false;}
    impl_->command=command_;
    return impl_->presentView(image,error);
}

bool WorldSurfaces::BeginDraw(const Native::WorldDraw& draw, DrawTarget& target, const char** error) {
    if (!impl_ || command_ == VK_NULL_HANDLE) {
        *error = "no frame is being recorded";
        return false;
    }
    impl_->command = command_;
    return impl_->beginDraw(draw, target, error);
}

void WorldSurfaces::EndDraw() {
    if (!impl_ || !impl_->drawPassOpen) return;
    ++impl_->draws;
}

void WorldSurfaces::FlushDraws() {
    if (impl_) impl_->flushDraws();
}

uint32_t WorldSurfaces::renderScale() const { return impl_ ? impl_->scale : 1; }

VkRenderPass WorldSurfaces::drawPass(bool color, bool depth) const {
    if (!impl_) return VK_NULL_HANDLE;
    if (color) return depth ? impl_->drawPass : impl_->colorDrawPass;
    return depth ? impl_->depthDrawPass : VK_NULL_HANDLE;
}

VkImageView WorldSurfaces::resolvedView(uint64_t key) const {
    if (!impl_ || !key) return VK_NULL_HANDLE;
    const auto found = impl_->resolved.find(key);
    return found == impl_->resolved.end() ? VK_NULL_HANDLE : found->second.sampled;
}

bool WorldSurfaces::resolvedInfo(uint64_t key, ResolvedInfo& info) const {
    if (!impl_ || !key) return false;
    const auto found = impl_->resolved.find(key);
    if (found == impl_->resolved.end()) return false;
    info.width = found->second.width;
    info.height = found->second.height;
    info.faces = found->second.faces;
    return true;
}

WorldSurfaces::PresentedFrame WorldSurfaces::presentedFrame() const {
    return impl_ ? impl_->presented : PresentedFrame{};
}

size_t WorldSurfaces::surfaceCount() const { return impl_ ? impl_->surfaces.size() : 0; }

size_t WorldSurfaces::drawTargetCount() const { return impl_ ? impl_->drawFramebuffers.size() : 0; }

size_t WorldSurfaces::resolvedCount() const { return impl_ ? impl_->resolved.size() : 0; }

void WorldSurfaces::printCounters() const {
    if (!impl_) {
        std::printf("[World] not initialised\n");
        std::fflush(stdout);
        return;
    }
    std::printf("[World] surfaces=%zu resolved=%zu draws=%llu passes=%llu clears=%llu resolves=%llu "
                "presents=%llu rejected=%llu\n",
                impl_->surfaces.size(), impl_->resolved.size(),
                static_cast<unsigned long long>(impl_->draws),
                static_cast<unsigned long long>(impl_->renderPasses),
                static_cast<unsigned long long>(impl_->clears),
                static_cast<unsigned long long>(impl_->resolves),
                static_cast<unsigned long long>(impl_->presents),
                static_cast<unsigned long long>(impl_->rejected));
    std::fflush(stdout);
}

} // namespace DarkRecomp::Ps5
