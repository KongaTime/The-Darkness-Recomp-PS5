#include "graphics_backend.h"

#include "display.h"
#include "display_vulkan.h"
#include "png_writer.h"
#include "vertex_preparation_capture.h"
#include "renderer/engine/engine_vertex_program.h"
#include "ps5_shaders.h"
#include "world_draw.h"
#include "world_surfaces.h"
#include "frame_interpolation.h"
#include "renderer/engine/display_gamma.h"
#include "renderer/engine/engine_performance.h"

#include "renderer/engine/display_layout.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/world_mesh.h"
#include "renderer/engine/video_layout.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace DarkRecomp::Ps5 {
namespace {

// The D3D11 preview's own caps: one immediate mesh is at most this large, so a
// frame's uploads fit in one buffer pair per swapchain image.
constexpr uint32_t kMaxVertices = 16384;
constexpr uint32_t kMaxIndices = 49152;
constexpr VkDeviceSize kVertexBytes = VkDeviceSize(kMaxVertices) * sizeof(SimpleVertex);
constexpr VkDeviceSize kIndexBytes = VkDeviceSize(kMaxIndices) * sizeof(uint16_t);
// One descriptor set per draw, because a set bound by an earlier draw in the
// same command buffer cannot be rewritten. A UI frame is tens of meshes.
constexpr uint32_t kDescriptorSets = 1024;
// The texture caches are bounded like the D3D11 renderer's budget: the guest
// reuses a small set of atlases, and the least recently used entry is dropped.
constexpr size_t kMaxTextures = 256;

enum Channel { kChannelAlpha = 0, kChannelColor = 1, kChannelLuma = 2, kChannelChroma = 3, kChannelCount = 4 };
enum PipelineIndex { kPipelineAlpha = 0, kPipelineColor = 1, kPipelineVideo = 2, kPipelineCount = 3 };

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Texture {
    std::shared_ptr<const void> source;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t used = 0;
};

template <typename Fn>
bool loadDevice(PFN_vkGetDeviceProcAddr proc, VkDevice device, Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(proc(device, name));
    return slot != nullptr;
}

VkShaderModule shaderModule(VkDevice device, PFN_vkCreateShaderModule create, const uint32_t* words,
                            size_t bytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule result = VK_NULL_HANDLE;
    return create(device, &info, nullptr, &result) == VK_SUCCESS ? result : VK_NULL_HANDLE;
}

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits bit, VkShaderModule shader) {
    VkPipelineShaderStageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage = bit;
    info.module = shader;
    info.pName = "main";
    return info;
}

} // namespace

// Everything the backend owns. Objects are created in Initialize() and
// destroyed in Shutdown(); a partially created backend is torn down by the same
// path, so a failed start on the console reports one error instead of leaving
// the device half-configured.
struct GraphicsBackend::Impl {
    Display* display = nullptr;
    Display::Impl* vk = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;

    // The swapchain image is shared by two kinds of command. The engine's UI
    // meshes and the world's frontbuffer copy draw into it, and the engine's own
    // render targets are cleared and resolved while it is not a render target at
    // all. So the image's pass is opened when something draws and closed again
    // when a world command arrives; the two passes differ only in whether the
    // frame's first one clears the image.
    VkRenderPass renderPassFirst = VK_NULL_HANDLE;
    VkRenderPass renderPassResume = VK_NULL_HANDLE;
    std::vector<std::array<VkFramebuffer, 2>> framebuffers;
    bool passOpen = false;
    bool passOpened = false;
    FrameInterpolation interpolation;
    VertexPreparationCapture vertexPreparationCapture{"/download0/darkness-ps5"};
    std::array<VkFramebuffer,2> interpolationFramebuffers{};
    bool fgRequested=false,fgFailed=false,offscreenFrame=false,renderOffscreen=false;
    bool fgScene=false,fgPresented=false,fgOverlaySafe=true;
    std::vector<SimpleMesh> fgOverlays;
    Buffer fgCaptureBuffer;
    bool fgCaptureRequested=false,fgCapturePending=false;
    uint32_t fgCaptureWidth=0,fgCaptureHeight=0;
    uint64_t fgGenerated=0,fgReal=0,fgWarmup=0,fgCuts=0;
    uint64_t fgReportedReal=0,fgReportedGenerated=0;
    unsigned fgReports=0;
    std::chrono::steady_clock::time_point fgNextCheck{},fgReportStart=std::chrono::steady_clock::now();

    // The engine's own render targets and the copies out of them.
    WorldSurfaces world;
    // The world's draws into those targets: geometry, materials and textures.
    WorldDrawLayer draws;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, kPipelineCount> pipelines{};
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    std::array<VkShaderModule, kPipelineCount> fragmentModules{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkFence uploadFence = VK_NULL_HANDLE;
    VkImageView whiteView = VK_NULL_HANDLE; // 1x1, sampled where a variant uses one plane.

    // One vertex/index pair per swapchain image: an image is not acquired until
    // its previous presentation completed, so its slice of the buffers is free.
    std::vector<Buffer> vertexBuffers;
    std::vector<Buffer> indexBuffers;

    std::unordered_map<const void*, Texture> textures[kChannelCount];
    std::vector<Texture> retiredTextures;
    uint64_t useCounter = 0;
    uint64_t uploads = 0;
    uint64_t evictions = 0;

    // Debug frame dumps. The staging buffer holds one image in the swapchain's
    // own format; the RGB copy is the PNG writer's input.
    bool dumping = false;
    std::string dumpDirectory;
    int dumpRemaining = 0;
    uint64_t dumpWritten = 0;
    Buffer dumpBuffer;
    std::vector<uint8_t> dumpRgb;
    // The resolved frontbuffer the present sampled, read back instead of the
    // swapchain image: the swapchain's copy never reaches the staging buffer on
    // this stack (the prefill is still there afterwards, in menus and in the
    // world), while the frontbuffer is one of the layer's own images and is what
    // the screen is showing.
    Buffer worldDumpBuffer;
    std::vector<uint8_t> worldDumpRgb;
    uint32_t worldDumpWidth = 0, worldDumpHeight = 0;
    VkFormat worldDumpFormat = VK_FORMAT_UNDEFINED;
    uint64_t worldDumpWritten = 0;
    bool worldDumpPending = false;

    bool open = false;
    uint32_t image = 0;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset = 0;
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;

    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkResetCommandPool ResetCommandPool = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = nullptr;

    bool create(Display& target, const char** error);
    void destroy();
    bool createSwapchainObjects(const char** error);
    void destroySwapchainObjects();
    bool createPipelines(const char** error);
    bool createBuffers(const char** error);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      Buffer& out, const char** error);
    VkImageView upload(Channel channel, const void* key, VkFormat format, uint32_t width, uint32_t height,
                       const uint8_t* pixels, uint32_t rowPitch, const char** error,
                       std::shared_ptr<const void> source = {});
    void evictIfNeeded(Channel channel);
    void release(Texture& texture);
    void destroyTextures();
    // Opens the swapchain image's pass, clearing it the first time this frame.
    void ensurePass();
    // Ends it, leaving the image ready for another pass or for the display.
    void closePass();
    // Moves the presented image into PRESENT_SRC_KHR for the display and dump.
    void finishImage();
    bool draw(const SimpleMesh& mesh, const char** error);
    bool ensureDumpBuffer(const char** error);
    bool ensureWorldDumpBuffer(uint32_t width, uint32_t height, const char** error);
    void beginInterpolation();
    bool canReplayOverlays() const;
    void reportInterpolation();
    void captureInterpolation();
    void writeInterpolationCapture();

private:
    bool copies(uint32_t width, uint32_t height, VkImage image, Buffer& staging, const char** error);
};

// ---------------------------------------------------------------------------
// Device-side setup
// ---------------------------------------------------------------------------

bool GraphicsBackend::Impl::create(Display& target, const char** error) {
    display = &target;
    vk = target.impl();
    if (!vk || vk->device == VK_NULL_HANDLE || vk->images.empty()) {
        *error = "the display is not open";
        return false;
    }
    instance = vk->instance;
    physical = vk->physical;
    device = vk->device;

    const auto getDeviceProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!getDeviceProc) {
        *error = "vkGetDeviceProcAddr is unavailable";
        return false;
    }
    GetMemoryProperties =
        Vk::instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!GetMemoryProperties) {
        *error = "vkGetPhysicalDeviceMemoryProperties is unavailable";
        return false;
    }

    bool complete = true;
    auto need = [&](auto& slot, const char* name) {
        if (!loadDevice(getDeviceProc, device, slot, name)) {
            std::printf("[Graphics] RADV does not provide %s\n", name);
            complete = false;
        }
    };
    need(CreateRenderPass, "vkCreateRenderPass");
    need(DestroyRenderPass, "vkDestroyRenderPass");
    need(CreateFramebuffer, "vkCreateFramebuffer");
    need(DestroyFramebuffer, "vkDestroyFramebuffer");
    need(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    need(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    need(CreatePipelineLayout, "vkCreatePipelineLayout");
    need(DestroyPipelineLayout, "vkDestroyPipelineLayout");
    need(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    need(DestroyPipeline, "vkDestroyPipeline");
    need(CreateShaderModule, "vkCreateShaderModule");
    need(DestroyShaderModule, "vkDestroyShaderModule");
    need(CreateSampler, "vkCreateSampler");
    need(DestroySampler, "vkDestroySampler");
    need(CreateDescriptorPool, "vkCreateDescriptorPool");
    need(DestroyDescriptorPool, "vkDestroyDescriptorPool");
    need(ResetDescriptorPool, "vkResetDescriptorPool");
    need(AllocateDescriptorSets, "vkAllocateDescriptorSets");
    need(UpdateDescriptorSets, "vkUpdateDescriptorSets");
    need(CreateBuffer, "vkCreateBuffer");
    need(DestroyBuffer, "vkDestroyBuffer");
    need(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    need(CreateImage, "vkCreateImage");
    need(DestroyImage, "vkDestroyImage");
    need(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    need(CreateImageView, "vkCreateImageView");
    need(DestroyImageView, "vkDestroyImageView");
    need(AllocateMemory, "vkAllocateMemory");
    need(FreeMemory, "vkFreeMemory");
    need(BindBufferMemory, "vkBindBufferMemory");
    need(BindImageMemory, "vkBindImageMemory");
    need(MapMemory, "vkMapMemory");
    need(UnmapMemory, "vkUnmapMemory");
    need(CmdBeginRenderPass, "vkCmdBeginRenderPass");
    need(CmdEndRenderPass, "vkCmdEndRenderPass");
    need(CmdBindPipeline, "vkCmdBindPipeline");
    need(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    need(CmdBindVertexBuffers, "vkCmdBindVertexBuffers");
    need(CmdBindIndexBuffer, "vkCmdBindIndexBuffer");
    need(CmdPushConstants, "vkCmdPushConstants");
    need(CmdSetViewport, "vkCmdSetViewport");
    need(CmdSetScissor, "vkCmdSetScissor");
    need(CmdDrawIndexed, "vkCmdDrawIndexed");
    need(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    need(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    need(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    need(CreateFence, "vkCreateFence");
    need(DestroyFence, "vkDestroyFence");
    need(WaitForFences, "vkWaitForFences");
    need(CreateCommandPool, "vkCreateCommandPool");
    need(DestroyCommandPool, "vkDestroyCommandPool");
    need(AllocateCommandBuffers, "vkAllocateCommandBuffers");
    need(FreeCommandBuffers, "vkFreeCommandBuffers");
    need(ResetCommandPool, "vkResetCommandPool");
    need(DeviceWaitIdle, "vkDeviceWaitIdle");
    if (!complete) {
        *error = "a device entry point RADV should provide is missing";
        return false;
    }

    // Each step names itself when it fails without a reason of its own, so a
    // console log never ends at "the graphics backend could not be created".
    auto fail = [&](const char* stage) {
        if (*error == nullptr) *error = stage;
        return false;
    };
    if (!createSwapchainObjects(error)) return fail("the swapchain objects could not be created");
    if (!createPipelines(error)) return fail("the pipelines could not be created");
    if (!createBuffers(error)) return fail("the backend's buffers could not be created");

    // The engine's own render targets. The layer owns what answers for a guest
    // surface, and needs the swapchain pass to copy a presented frontbuffer into
    // the image this renderer is drawing. Its failure is the renderer's: without
    // it the guest's scene commands have nowhere to go.
    if (!world.Create(instance, physical, device, Shaders::kWorldTransferVertex,
                      sizeof(Shaders::kWorldTransferVertex), Shaders::kWorldTransferFragment,
                      sizeof(Shaders::kWorldTransferFragment), Shaders::kWorldPresentFragment,
                      sizeof(Shaders::kWorldPresentFragment), error))
        return fail("the engine's render targets could not be created");
    if (!world.SetSwapchain(renderPassFirst, display->width(), display->height(), error))
        return fail("the engine's render targets could not take the swapchain");
    // The draws themselves: the programs, the constant arena and the caches. It
    // records into the same command buffer and renders into the surfaces above.
    if (!draws.Create(instance, physical, device, error))
        return fail("the world draw backend could not be created");

    // Uploads run on their own command buffer and fence, so a texture is ready
    // before the frame that samples it is recorded. The wait is one small copy,
    // and the guest reuses its atlases across frames.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = vk->queueFamily;
    if (CreateCommandPool(device, &poolInfo, nullptr, &uploadPool) != VK_SUCCESS) {
        *error = "vkCreateCommandPool (uploads) failed";
        return false;
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (CreateFence(device, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
        *error = "vkCreateFence failed";
        return false;
    }

    const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    whiteView = upload(kChannelColor, this, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, white, 4, error);
    if (whiteView == VK_NULL_HANDLE) return false;
    // Pinned: every descriptor set binds both planes, so the view a variant
    // does not sample must outlive the cache's eviction.
    textures[kChannelColor][this].used = UINT64_MAX;
    return true;
}

bool GraphicsBackend::Impl::createSwapchainObjects(const char** error) {
    // The swapchain's images have no maintained contents between frames. The
    // frame's first pass discards them and clears to the background; a later
    // pass -- after the engine's own world targets were cleared or resolved --
    // keeps what it drew. Both leave the image ready for another pass, and
    // finishImage() hands it to the display.
    VkAttachmentDescription colour{};
    colour.format = vk->format.format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &colour;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    // Two passes over the same framebuffers, which Vulkan treats as compatible:
    // they differ only in what they do with the contents they find.
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (CreateRenderPass(device, &passInfo, nullptr, &renderPassFirst) != VK_SUCCESS) {
        *error = "vkCreateRenderPass failed";
        return false;
    }
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (CreateRenderPass(device, &passInfo, nullptr, &renderPassResume) != VK_SUCCESS) {
        *error = "vkCreateRenderPass failed";
        return false;
    }

    framebuffers.assign(vk->views.size(), std::array<VkFramebuffer, 2>{});
    for (size_t i = 0; i < vk->views.size(); ++i) {
        const VkRenderPass passes[2] = {renderPassFirst, renderPassResume};
        for (uint32_t pass = 0; pass < 2; ++pass) {
            VkFramebufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = passes[pass];
            info.attachmentCount = 1;
            info.pAttachments = &vk->views[i];
            info.width = display->width();
            info.height = display->height();
            info.layers = 1;
            if (CreateFramebuffer(device, &info, nullptr, &framebuffers[i][pass]) != VK_SUCCESS) {
                *error = "vkCreateFramebuffer failed";
                return false;
            }
        }
    }
    return true;
}

bool GraphicsBackend::Impl::createPipelines(const char** error) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        *error = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    // The projection is one 64-byte matrix, like the D3D11 preview's
    // `cbuffer Transform`; push constants keep a UI frame free of buffer
    // management.
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    range.offset = 0;
    range.size = 16 * sizeof(float);
    VkPipelineLayoutCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &setLayout;
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &range;
    if (CreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        *error = "vkCreatePipelineLayout failed";
        return false;
    }

    vertexModule = shaderModule(device, CreateShaderModule, Shaders::kMeshVertex, sizeof(Shaders::kMeshVertex));
    fragmentModules[kPipelineAlpha] =
        shaderModule(device, CreateShaderModule, Shaders::kMeshAlpha, sizeof(Shaders::kMeshAlpha));
    fragmentModules[kPipelineColor] =
        shaderModule(device, CreateShaderModule, Shaders::kMeshColor, sizeof(Shaders::kMeshColor));
    fragmentModules[kPipelineVideo] =
        shaderModule(device, CreateShaderModule, Shaders::kMeshVideo, sizeof(Shaders::kMeshVideo));
    if (vertexModule == VK_NULL_HANDLE) {
        *error = "the simple-mesh vertex module is invalid";
        return false;
    }
    for (VkShaderModule fragment : fragmentModules) {
        if (fragment == VK_NULL_HANDLE) {
            *error = "a simple-mesh fragment module is invalid";
            return false;
        }
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (CreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        *error = "vkCreateSampler failed";
        return false;
    }

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(SimpleVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(SimpleVertex, position))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(SimpleVertex, uv))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(SimpleVertex, color))},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Both are recorded per draw: the movie path letterboxes by shrinking the
    // viewport, exactly as the D3D11 preview's fitDisplay does.
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

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blending{};
    blending.blendEnable = VK_TRUE;
    blending.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blending.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blending.colorBlendOp = VK_BLEND_OP_ADD;
    blending.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blending.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blending.alphaBlendOp = VK_BLEND_OP_ADD;
    blending.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    for (uint32_t variant = 0; variant < kPipelineCount; ++variant) {
        const VkPipelineShaderStageCreateInfo stages[2] = {
            shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule),
            shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModules[variant])};
        VkPipelineColorBlendAttachmentState state = blending;
        // The movie path writes opaque pixels and blends nothing: D3D11 binds
        // no blend state for it, so alpha must not enter the equation.
        state.blendEnable = variant == kPipelineVideo ? VK_FALSE : VK_TRUE;
        VkPipelineColorBlendStateCreateInfo colourBlend{};
        colourBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colourBlend.attachmentCount = 1;
        colourBlend.pAttachments = &state;

        VkGraphicsPipelineCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertexInput;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pDepthStencilState = &depth;
        create.pColorBlendState = &colourBlend;
        create.pDynamicState = &dynamic;
        create.layout = pipelineLayout;
        create.renderPass = renderPassFirst;
        create.subpass = 0;
        if (CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipelines[variant]) !=
            VK_SUCCESS) {
            *error = "vkCreateGraphicsPipelines failed";
            return false;
        }
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kDescriptorSets * 2;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kDescriptorSets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (CreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        *error = "vkCreateDescriptorPool failed";
        return false;
    }
    return true;
}

bool GraphicsBackend::Impl::createBuffers(const char** error) {
    vertexBuffers.assign(vk->images.size(), Buffer{});
    indexBuffers.assign(vk->images.size(), Buffer{});
    for (size_t i = 0; i < vk->images.size(); ++i) {
        if (!createBuffer(kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          vertexBuffers[i], error))
            return false;
        if (!createBuffer(kIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          indexBuffers[i], error))
            return false;
    }
    return true;
}

uint32_t GraphicsBackend::Impl::memoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties properties{};
    GetMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

bool GraphicsBackend::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags properties, Buffer& out,
                                         const char** error) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (CreateBuffer(device, &info, nullptr, &out.handle) != VK_SUCCESS) {
        *error = "vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    GetBufferMemoryRequirements(device, out.handle, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits, properties);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX || AllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) {
        *error = "vkAllocateMemory failed for a buffer";
        return false;
    }
    if (BindBufferMemory(device, out.handle, out.memory, 0) != VK_SUCCESS) {
        *error = "vkBindBufferMemory failed";
        return false;
    }
    out.size = size;
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        MapMemory(device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS) {
        *error = "vkMapMemory failed";
        return false;
    }
    return true;
}

// One image in the swapchain's own format: four bytes per pixel covers every
// 32-bit surface format the display can pick, and the copy leaves the texels
// as the console produced them.
bool GraphicsBackend::Impl::ensureDumpBuffer(const char** error) {
    const VkDeviceSize needed = VkDeviceSize(display->width()) * display->height() * 4u;
    if (dumpBuffer.handle && dumpBuffer.size >= needed) return true;
    if (dumpBuffer.mapped) {
        UnmapMemory(device, dumpBuffer.memory);
        dumpBuffer.mapped = nullptr;
    }
    if (dumpBuffer.handle) DestroyBuffer(device, dumpBuffer.handle, nullptr);
    if (dumpBuffer.memory) FreeMemory(device, dumpBuffer.memory, nullptr);
    dumpBuffer = {};
    if (!createBuffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      dumpBuffer, error))
        return false;
    dumpRgb.resize(size_t(display->width()) * display->height() * 3);
    return true;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

// The same staging shape the swapchain dump asks for, sized for the
// frontbuffer, which is usually the guest's own resolution.
bool GraphicsBackend::Impl::ensureWorldDumpBuffer(uint32_t width, uint32_t height, const char** error) {
    const VkDeviceSize needed = VkDeviceSize(width) * height * 4u;
    if (worldDumpBuffer.handle && worldDumpBuffer.size >= needed) return true;
    if (worldDumpBuffer.mapped) {
        UnmapMemory(device, worldDumpBuffer.memory);
        worldDumpBuffer.mapped = nullptr;
    }
    if (worldDumpBuffer.handle) DestroyBuffer(device, worldDumpBuffer.handle, nullptr);
    if (worldDumpBuffer.memory) FreeMemory(device, worldDumpBuffer.memory, nullptr);
    worldDumpBuffer = {};
    if (!createBuffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      worldDumpBuffer, error))
        return false;
    worldDumpRgb.resize(size_t(width) * height * 3);
    return true;
}

bool GraphicsBackend::Impl::copies(uint32_t width, uint32_t height, VkImage image, Buffer& staging,
                                   const char** error) {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = uploadPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (AllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS) {
        *error = "vkAllocateCommandBuffers (upload) failed";
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk->BeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        *error = "vkBeginCommandBuffer (upload) failed";
        return false;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    CmdCopyBufferToImage(command, staging.handle, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier toSample = toTransfer;
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &toSample);

    if (vk->EndCommandBuffer(command) != VK_SUCCESS) {
        *error = "vkEndCommandBuffer (upload) failed";
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (vk->QueueSubmit(vk->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        *error = "vkQueueSubmit (upload) failed";
        return false;
    }
    // The upload must be finished before the frame that samples it is recorded,
    // and the texture is about to be read by a later submission, so wait for
    // the queue rather than tracking one fence per upload.
    vk->QueueWaitIdle(vk->queue);
    FreeCommandBuffers(device, uploadPool, 1, &command);
    return true;
}

VkImageView GraphicsBackend::Impl::upload(Channel channel, const void* key, VkFormat format, uint32_t width,
                                          uint32_t height, const uint8_t* pixels, uint32_t rowPitch,
                                          const char** error, std::shared_ptr<const void> source) {
    if (!pixels || !width || !height || !rowPitch) {
        *error = "empty texture";
        return VK_NULL_HANDLE;
    }

    if (source) {
        if (auto found = textures[channel].find(key); found != textures[channel].end() &&
            found->second.source == source) {
            found->second.used = ++useCounter;
            return found->second.view;
        }
    }

    Buffer staging;
    if (!createBuffer(VkDeviceSize(rowPitch) * height, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
                      error)) {
        return VK_NULL_HANDLE;
    }
    std::memcpy(staging.mapped, pixels, size_t(rowPitch) * height);

    Texture texture;
    texture.source = std::move(source);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const auto abandon = [&] {
        release(texture);
        if (staging.mapped) UnmapMemory(device, staging.memory);
        DestroyBuffer(device, staging.handle, nullptr);
        FreeMemory(device, staging.memory, nullptr);
        return VK_NULL_HANDLE;
    };

    if (CreateImage(device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS) {
        *error = "vkCreateImage failed";
        return abandon();
    }
    VkMemoryRequirements requirements{};
    GetImageMemoryRequirements(device, texture.image, &requirements);
    const uint32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || AllocateMemory(device, &allocation, nullptr, &texture.memory) != VK_SUCCESS ||
        BindImageMemory(device, texture.image, texture.memory, 0) != VK_SUCCESS) {
        *error = "the texture's GPU memory could not be allocated";
        return abandon();
    }
    if (!copies(width, height, texture.image, staging, error)) return abandon();

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (CreateImageView(device, &viewInfo, nullptr, &texture.view) != VK_SUCCESS) {
        *error = "vkCreateImageView failed";
        return abandon();
    }

    if (staging.mapped) UnmapMemory(device, staging.memory);
    DestroyBuffer(device, staging.handle, nullptr);
    FreeMemory(device, staging.memory, nullptr);

    evictIfNeeded(channel);
    if (auto existing = textures[channel].find(key); existing != textures[channel].end()) {
        retiredTextures.push_back(existing->second);
        textures[channel].erase(existing);
    }
    texture.used = ++useCounter;
    textures[channel][key] = texture;
    ++uploads;
    return texture.view;
}

void GraphicsBackend::Impl::release(Texture& texture) {
    if (texture.view) {
        DestroyImageView(device, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image) {
        DestroyImage(device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory) {
        FreeMemory(device, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
}

void GraphicsBackend::Impl::evictIfNeeded(Channel channel) {
    auto& cache = textures[channel];
    if (cache.size() < kMaxTextures) return;
    auto oldest = cache.end();
    for (auto entry = cache.begin(); entry != cache.end(); ++entry) {
        if (entry->second.used == UINT64_MAX) continue; // pinned
        if (oldest == cache.end() || entry->second.used < oldest->second.used) oldest = entry;
    }
    if (oldest == cache.end()) return;
    // An earlier draw in the command buffer may still sample this texture.
    retiredTextures.push_back(oldest->second);
    cache.erase(oldest);
    ++evictions;
}

void GraphicsBackend::Impl::destroyTextures() {
    for (auto& texture : retiredTextures) release(texture);
    retiredTextures.clear();
    for (auto& cache : textures) {
        for (auto& entry : cache) release(entry.second);
        cache.clear();
    }
    whiteView = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void GraphicsBackend::Impl::ensurePass() {
    world.FlushDraws();
    if (passOpen) return;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    // The first pass of a frame discards the image: the swapchain does not keep
    // its contents between frames. A later one keeps them, because the engine's
    // world commands ran in between and the UI drawn before them must stay.
    begin.renderPass = passOpened ? renderPassResume : renderPassFirst;
    begin.framebuffer = renderOffscreen ? interpolationFramebuffers[passOpened?1:0] : framebuffers[image][passOpened ? 1 : 0];
    begin.renderArea = {{0, 0}, {frameWidth, frameHeight}};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    passOpen = true;
    passOpened = true;
    CmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void GraphicsBackend::Impl::closePass() {
    if (!passOpen) return;
    passOpen = false;
    CmdEndRenderPass(command);
}

void GraphicsBackend::Impl::finishImage() {
    // The pass leaves the image ready for another pass; the display needs it in
    // the present layout. A frame whose commands never opened a pass still
    // presents its image, so an untouched one is moved out of the undefined
    // layout here rather than handed over in a layout present does not accept.
    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.srcAccessMask = passOpened ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = passOpened ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = vk->images[image];
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(command,
                       passOpened ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
}

bool GraphicsBackend::Impl::draw(const SimpleMesh& mesh, const char** error) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        *error = "empty mesh";
        return false;
    }
    if (mesh.vertices.size() > kMaxVertices || mesh.indices.size() > kMaxIndices) {
        *error = "mesh exceeds the D3D11 preview's caps";
        return false;
    }
    for (uint16_t index : mesh.indices) {
        if (size_t(index) >= mesh.vertices.size()) {
            *error = "mesh index out of range";
            return false;
        }
    }
    const unsigned sources = unsigned(bool(mesh.texture)) + unsigned(bool(mesh.video)) +
                             unsigned(bool(mesh.colorTexture));
    if (sources != 1) {
        *error = "a mesh must sample exactly one image";
        return false;
    }

    // The sampled planes, then the geometry. A failed upload leaves the frame
    // running with this mesh skipped: one bad texture must not cost the frame.
    VkImageView planes[2] = {whiteView, whiteView};
    uint32_t variant = kPipelineAlpha;
    if (mesh.video) {
        variant = kPipelineVideo;
        const VideoFrame& video = *mesh.video;
        if (!video.width || !video.height || video.width > 2048 || video.height > 2048 || video.width % 2 ||
            video.luma.size() != size_t(video.width) * video.height ||
            video.chroma.size() != size_t(video.width) * video.height / 2) {
            *error = "invalid video frame";
            return false;
        }
        planes[0] = upload(kChannelLuma, mesh.video.get(), VK_FORMAT_R8_UNORM, video.width, video.height,
                           video.luma.data(), video.width, error, mesh.video);
        planes[1] = upload(kChannelChroma, mesh.video.get(), VK_FORMAT_R8G8_UNORM, video.width / 2,
                           video.height / 2, video.chroma.data(), video.width, error, mesh.video);
        if (planes[0] == VK_NULL_HANDLE || planes[1] == VK_NULL_HANDLE) return false;
    } else if (mesh.colorTexture) {
        variant = kPipelineColor;
        const ColorImage& image = *mesh.colorTexture;
        if (!image.width || !image.height ||
            image.pixels.size() != size_t(image.width) * image.height * 4) {
            *error = "invalid colour image";
            return false;
        }
        // Base level only for now: the D3D11 path uploads authored mips and a
        // resident first level, which this sampler does not need yet.
        planes[0] = upload(kChannelColor, mesh.colorTexture.get(), VK_FORMAT_R8G8B8A8_UNORM, image.width,
                           image.height, image.pixels.data(), image.width * 4, error, mesh.colorTexture);
        if (planes[0] == VK_NULL_HANDLE) return false;
    } else {
        variant = kPipelineAlpha;
        const AlphaImage& image = *mesh.texture;
        if (!image.width || !image.height ||
            image.pixels.size() != size_t(image.width) * image.height) {
            *error = "invalid alpha image";
            return false;
        }
        planes[0] = upload(kChannelAlpha, mesh.texture.get(), VK_FORMAT_R8_UNORM, image.width, image.height,
                           image.pixels.data(), image.width, error, mesh.texture);
        if (planes[0] == VK_NULL_HANDLE) return false;
    }

    Buffer& vertex = vertexBuffers[image];
    Buffer& index = indexBuffers[image];
    const VkDeviceSize vertexBytes = VkDeviceSize(mesh.vertices.size()) * sizeof(SimpleVertex);
    const VkDeviceSize indexBytes = VkDeviceSize(mesh.indices.size()) * sizeof(uint16_t);
    const VkDeviceSize vertexStart = (vertexOffset + 15) & ~VkDeviceSize(15);
    const VkDeviceSize indexStart = (indexOffset + 15) & ~VkDeviceSize(15);
    if (vertexStart + vertexBytes > vertex.size || indexStart + indexBytes > index.size) {
        *error = "the frame's upload buffer is exhausted";
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(vertex.mapped) + vertexStart, mesh.vertices.data(),
                size_t(vertexBytes));
    std::memcpy(static_cast<uint8_t*>(index.mapped) + indexStart, mesh.indices.data(), size_t(indexBytes));
    vertexOffset = vertexStart + vertexBytes;
    indexOffset = indexStart + indexBytes;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    if (AllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
        *error = "the frame's descriptor sets are exhausted";
        return false;
    }
    const VkDescriptorImageInfo images[2] = {
        {sampler, planes[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, planes[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    UpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkViewport viewport{0.0f, 0.0f, float(frameWidth), float(frameHeight), 0.0f, 1.0f};
    if (mesh.video && isFullscreenVideo(mesh)) {
        const DisplayRect fit =
            fitDisplay(mesh.video->width, mesh.video->height, frameWidth, frameHeight);
        viewport = {fit.x, fit.y, fit.width, fit.height, 0.0f, 1.0f};
    }
    CmdSetViewport(command, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {frameWidth, frameHeight}};
    CmdSetScissor(command, 0, 1, &scissor);

    CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[variant]);
    CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
    struct Push {
        float projection[16];
    } push{};
    std::memcpy(push.projection, mesh.projection.data(), sizeof(push.projection));
    CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    CmdBindVertexBuffers(command, 0, 1, &vertex.handle, &vertexStart);
    CmdBindIndexBuffer(command, index.handle, indexStart, VK_INDEX_TYPE_UINT16);
    CmdDrawIndexed(command, uint32_t(mesh.indices.size()), 1, 0, 0, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void GraphicsBackend::Impl::destroySwapchainObjects() {
    if(interpolation.created()) {
        DeviceWaitIdle(device);
        if(fgCaptureBuffer.mapped)UnmapMemory(device,fgCaptureBuffer.memory);
        if(fgCaptureBuffer.handle)DestroyBuffer(device,fgCaptureBuffer.handle,nullptr);
        if(fgCaptureBuffer.memory)FreeMemory(device,fgCaptureBuffer.memory,nullptr);
        fgCaptureBuffer={};fgCapturePending=false;
        for(auto& fb:interpolationFramebuffers) {if(fb)DestroyFramebuffer(device,fb,nullptr);fb=VK_NULL_HANDLE;}
        interpolation.destroy();
    }
    offscreenFrame=renderOffscreen=false;
    // The world layer's present pipeline was recorded against the pass below, so
    // it is released first.
    const char* released = nullptr;
    world.SetSwapchain(VK_NULL_HANDLE, 0, 0, &released);
    for (auto& set : framebuffers) {
        for (VkFramebuffer framebuffer : set) {
            if (framebuffer) DestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    framebuffers.clear();
    if (renderPassFirst) {
        DestroyRenderPass(device, renderPassFirst, nullptr);
        renderPassFirst = VK_NULL_HANDLE;
    }
    if (renderPassResume) {
        DestroyRenderPass(device, renderPassResume, nullptr);
        renderPassResume = VK_NULL_HANDLE;
    }
    passOpen = false;
    passOpened = false;
}

void GraphicsBackend::Impl::destroy() {
    if (device == VK_NULL_HANDLE) return;
    if (DeviceWaitIdle) DeviceWaitIdle(device);

    destroyTextures();
    draws.Destroy();
    world.Destroy();
    open = false;

    const auto drop = [&](std::vector<Buffer>& buffers) {
        for (Buffer& buffer : buffers) {
            if (buffer.mapped) {
                UnmapMemory(device, buffer.memory);
                buffer.mapped = nullptr;
            }
            if (buffer.handle) DestroyBuffer(device, buffer.handle, nullptr);
            if (buffer.memory) FreeMemory(device, buffer.memory, nullptr);
            buffer = {};
        }
        buffers.clear();
    };
    drop(vertexBuffers);
    drop(indexBuffers);
    if (dumpBuffer.mapped) {
        UnmapMemory(device, dumpBuffer.memory);
        dumpBuffer.mapped = nullptr;
    }
    if (dumpBuffer.handle) DestroyBuffer(device, dumpBuffer.handle, nullptr);
    if (dumpBuffer.memory) FreeMemory(device, dumpBuffer.memory, nullptr);
    dumpBuffer = {};
    dumping = false;
    dumpRemaining = 0;

    destroySwapchainObjects();
    for (VkPipeline& pipeline : pipelines) {
        if (pipeline) DestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    for (VkShaderModule& fragment : fragmentModules) {
        if (fragment) DestroyShaderModule(device, fragment, nullptr);
        fragment = VK_NULL_HANDLE;
    }
    if (vertexModule) DestroyShaderModule(device, vertexModule, nullptr);
    if (sampler) DestroySampler(device, sampler, nullptr);
    if (descriptorPool) DestroyDescriptorPool(device, descriptorPool, nullptr);
    if (setLayout) DestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (pipelineLayout) DestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (uploadPool) DestroyCommandPool(device, uploadPool, nullptr);
    if (uploadFence) DestroyFence(device, uploadFence, nullptr);

    device = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    vk = nullptr;
    display = nullptr;
}

// ---------------------------------------------------------------------------
// The title-facing interface
// ---------------------------------------------------------------------------

GraphicsBackend::~GraphicsBackend() { Shutdown(); }

bool GraphicsBackend::Initialize(Display& display) {
    Shutdown();
    impl_ = new Impl();
    const char* error = nullptr;
    if (!impl_->create(display, &error)) {
        error_ = error ? error : "the graphics backend could not be created";
        impl_->destroy();
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    error_ = nullptr;
    return true;
}

void GraphicsBackend::Shutdown() {
    if (!impl_) return;
    impl_->destroy();
    delete impl_;
    impl_ = nullptr;
    error_ = "the graphics backend is not initialised";
}

// Capture the resolved frontbuffer as well as the presented image, so scene
// output can be compared before and after the display ramp and scaling.
static void recordWorldDumpCopy(GraphicsBackend::Impl& impl) {
    const auto presented = impl.world.presentedFrame();
    if (!presented.image || !presented.width || !presented.height) return;
    const char* error = nullptr;
    if (!impl.ensureWorldDumpBuffer(presented.width, presented.height, &error)) {
        std::printf("[FrameDump] cannot copy the frontbuffer: %s\n", error ? error : "unknown error");
        std::fflush(stdout);
        return;
    }
    if (impl.worldDumpBuffer.mapped)
        std::memset(impl.worldDumpBuffer.mapped, 0x11, size_t(impl.worldDumpBuffer.size));
    VkImageMemoryBarrier toSource{};
    toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSource.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.image = presented.image;
    toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl.vk->PipelineBarrier(impl.command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {presented.width, presented.height, 1};
    impl.CmdCopyImageToBuffer(impl.command, presented.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              impl.worldDumpBuffer.handle, 1, &region);
    // The image is the next frames' present input too: put it back to sampled.
    VkImageMemoryBarrier back = toSource;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    impl.vk->PipelineBarrier(impl.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    impl.worldDumpWidth = presented.width;
    impl.worldDumpHeight = presented.height;
    impl.worldDumpFormat = presented.format;
    impl.worldDumpPending = true;
}

// A requested dump copies the finished image before the display takes it. The
// render pass leaves the image in PRESENT_SRC_KHR, so it is moved to
// TRANSFER_SRC_OPTIMAL for the copy and handed back for the present.
static void recordDumpCopy(GraphicsBackend::Impl& impl, bool* copied) {
    *copied = false;
    if (!impl.dumping || impl.dumpRemaining <= 0) return;
    // Fill the staging buffer with a value the image cannot contain (0x11),
    // then copy over it. A dump that still reads 0x11 afterwards never reached
    // the buffer; one that reads black with this fill in place says the copied
    // image itself was black. Without that split a black dump cannot tell a
    // broken copy from a black surface.
    if (impl.dumpBuffer.mapped)
        std::memset(impl.dumpBuffer.mapped, 0x11, size_t(impl.dumpBuffer.size));
    const VkImage image = impl.vk->images[impl.image];
    VkImageMemoryBarrier toSource{};
    toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSource.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.image = image;
    toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl.vk->PipelineBarrier(impl.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {impl.display->width(), impl.display->height(), 1};
    impl.CmdCopyImageToBuffer(impl.command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              impl.dumpBuffer.handle, 1, &region);
    VkImageMemoryBarrier toPresent = toSource;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    impl.vk->PipelineBarrier(impl.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
    *copied = true;
    recordWorldDumpCopy(impl);
}

bool GraphicsBackend::beginFrameDump(const std::string& directory, int frames) {
    // A dump is optional, so a refusal is reported here rather than through
    // error(): the title's rendering checks must not see a stale failure.
    auto refuse = [&](const char* reason) {
        std::printf("[FrameDump] Not available: %s\n", reason);
        std::fflush(stdout);
        return false;
    };
    if (!impl_) return refuse("the graphics backend is not initialised");
    if (frames <= 0) return false;
    if (!impl_->display->canReadBack()) return refuse("the display's swapchain cannot be read back on this surface");
    std::error_code failure;
    std::filesystem::create_directories(directory, failure);
    if (failure) return refuse("the frame dump directory could not be created");
    impl_->dumping = true;
    if (impl_->dumpDirectory != directory) {
        impl_->dumpDirectory = directory;
        impl_->dumpWritten = 0;
    }
    impl_->dumpRemaining = frames;
    std::printf("[FrameDump] Writing %d frame%s to %s\n", frames, frames == 1 ? "" : "s",
                directory.c_str());
    std::fflush(stdout);
    return true;
}

// A dumped frame is 25 MB that only a host can open, while a console session's
// readout is the kernel log. Summarise the frame there as well: a black or
// blank image then says so in the log, and the coarse map shows what kind of
// damage a broken frame has (flat colour, channel swap, stripes, noise).
static void reportFrameContent(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height) {
    if (rgb.size() < size_t(width) * height * 3 || !width || !height) return;
    std::array<uint32_t, 3> minimum{255, 255, 255}, maximum{};
    std::array<uint64_t, 3> total{};
    uint64_t dark = 0;
    // 15-bit colour bins: enough to separate a channel swap from noise, small
    // enough to sit on the stack and be counted over the whole image.
    std::vector<uint32_t> bins(32768, 0);
    const uint8_t* pixels = rgb.data();
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = pixels + (size_t(y) * width + x) * 3;
            uint32_t colour = 0;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                const uint8_t value = pixel[channel];
                minimum[channel] = (std::min)(minimum[channel], uint32_t(value));
                maximum[channel] = (std::max)(maximum[channel], uint32_t(value));
                total[channel] += value;
                colour |= (uint32_t(value) >> 3) << (channel * 5);
            }
            if (pixel[0] < 8 && pixel[1] < 8 && pixel[2] < 8) ++dark;
            ++bins[colour];
        }
    }
    const uint64_t count = uint64_t(width) * height;
    std::printf("[FrameDump] content %ux%u mean=%llu,%llu,%llu min=%u,%u,%u max=%u,%u,%u nearBlack=%llu.%02llu%%\n",
                width, height, (unsigned long long)(total[0] / count), (unsigned long long)(total[1] / count),
                (unsigned long long)(total[2] / count), minimum[0], minimum[1], minimum[2], maximum[0],
                maximum[1], maximum[2], (unsigned long long)(dark * 100 / count),
                (unsigned long long)(dark * 10000 / count % 100));

    // The dominant colours, most frequent first: a channel swap or a constant
    // fill is obvious here, and pure black shows up as one bin holding 100%.
    std::array<uint32_t, 6> best{};
    for (uint32_t colour = 0; colour < bins.size(); ++colour) {
        if (!bins[colour]) continue;
        for (uint32_t slot = 0; slot < best.size(); ++slot) {
            if (bins[colour] > bins[best[slot]]) {
                for (uint32_t move = uint32_t(best.size()) - 1; move > slot; --move) best[move] = best[move - 1];
                best[slot] = colour;
                break;
            }
        }
    }
    std::printf("[FrameDump] colours");
    for (uint32_t slot = 0; slot < best.size() && bins[best[slot]]; ++slot) {
        const uint32_t colour = best[slot];
        std::printf(" %02x%02x%02x=%llu.%01llu%%", ((colour >> 10) & 31) << 3 | 4,
                    ((colour >> 5) & 31) << 3 | 4, (colour & 31) << 3 | 4,
                    (unsigned long long)(bins[colour] * 100 / count),
                    (unsigned long long)(bins[colour] * 1000 / count % 10));
    }
    std::printf("\n");

    // A 16x9 tile map, each tile its own average: the whole frame's layout in
    // nine log lines, which is what tells a black border from a black frame.
    constexpr uint32_t kTilesX = 16, kTilesY = 9;
    for (uint32_t tileY = 0; tileY < kTilesY; ++tileY) {
        const uint32_t y0 = tileY * height / kTilesY, y1 = (std::max)(y0 + 1, (tileY + 1) * height / kTilesY);
        std::printf("[FrameDump] map");
        for (uint32_t tileX = 0; tileX < kTilesX; ++tileX) {
            const uint32_t x0 = tileX * width / kTilesX, x1 = (std::max)(x0 + 1, (tileX + 1) * width / kTilesX);
            uint64_t sum[3]{};
            uint64_t samples = 0;
            for (uint32_t y = y0; y < y1; y += 4) {
                for (uint32_t x = x0; x < x1; x += 4) {
                    const uint8_t* pixel = pixels + (size_t(y) * width + x) * 3;
                    for (uint32_t channel = 0; channel < 3; ++channel) sum[channel] += pixel[channel];
                    ++samples;
                }
            }
            if (!samples) samples = 1;
            std::printf(" %02llx%02llx%02llx",
                        (unsigned long long)(sum[0] / samples), (unsigned long long)(sum[1] / samples),
                        (unsigned long long)(sum[2] / samples));
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

// The frontbuffer's texels in the PNG writer's RGB form. Only the four-byte
// formats a resolved frontbuffer can have are packed; anything else is named in
// the log rather than written as a wrong picture.
static bool packPresentedRows(const uint8_t* source, uint32_t width, uint32_t height, VkFormat format,
                              std::vector<uint8_t>& rgb) {
    if (rgb.size() < size_t(width) * height * 3) return false;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* texel = source + (size_t(y) * width + x) * 4;
            uint8_t red = 0, green = 0, blue = 0;
            switch (format) {
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                    red = texel[0]; green = texel[1]; blue = texel[2];
                    break;
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    blue = texel[0]; green = texel[1]; red = texel[2];
                    break;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                    uint32_t packed = 0;
                    std::memcpy(&packed, texel, 4);
                    red = uint8_t((packed & 1023u) >> 2);
                    green = uint8_t(((packed >> 10) & 1023u) >> 2);
                    blue = uint8_t(((packed >> 20) & 1023u) >> 2);
                    break;
                }
                default:
                    return false;
            }
            uint8_t* out = rgb.data() + (size_t(y) * width + x) * 3;
            out[0] = red; out[1] = green; out[2] = blue;
        }
    }
    return true;
}

static void invalidateMappedRange(Display::Impl& vk, VkDeviceMemory memory) {
    if (const auto getDeviceProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(vk.instance, "vkGetDeviceProcAddr")) {
        if (const auto invalidate = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(
                getDeviceProc(vk.device, "vkInvalidateMappedMemoryRanges"))) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = memory;
            range.size = VK_WHOLE_SIZE;
            invalidate(vk.device, 1, &range);
        }
    }
}

// Reads the frontbuffer copy the frame recorded, beside the swapchain dump.
static void writeWorldFrameDump(GraphicsBackend::Impl& impl) {
    if (!impl.worldDumpPending || !impl.worldDumpBuffer.mapped) return;
    impl.worldDumpPending = false;
    invalidateMappedRange(*impl.vk, impl.worldDumpBuffer.memory);
    const uint32_t width = impl.worldDumpWidth, height = impl.worldDumpHeight;
    const auto* source = static_cast<const uint8_t*>(impl.worldDumpBuffer.mapped);
    if (!source || !width || !height) return;
    if (!packPresentedRows(source, width, height, impl.worldDumpFormat, impl.worldDumpRgb)) {
        std::printf("[FrameDump] frontbuffer format %d has no channel mapping for a dump\n",
                    int(impl.worldDumpFormat));
        std::fflush(stdout);
        return;
    }
    reportFrameContent(impl.worldDumpRgb, width, height);
    char path[1024];
    const unsigned long long number = static_cast<unsigned long long>(++impl.worldDumpWritten);
    std::snprintf(path, sizeof(path), "%s/world-%04llu.png", impl.dumpDirectory.c_str(), number);
    const char* writeError = nullptr;
    if (!writePngRgb(path, width, height, impl.worldDumpRgb.data(), size_t(width) * 3, &writeError)) {
        std::printf("[FrameDump] %s: %s\n", path, writeError ? writeError : "the frame could not be written");
        std::fflush(stdout);
        return;
    }
    std::printf("[FrameDump] %s (%ux%u)\n", path, width, height);
    std::fflush(stdout);
}

// Runs after the display has taken the frame: the copy in the command buffer
// has completed, so the staging buffer holds exactly what was presented.
void GraphicsBackend::writeFrameDump() {
    Impl& impl = *impl_;
    if (!impl.dumping || impl.dumpRemaining <= 0) return;
    impl.vk->QueueWaitIdle(impl.vk->queue);
    // The console's staging memory can be host-visible without the CPU's view
    // being invalidated when the GPU writes it. Ask for it explicitly; a driver
    // that reports the memory coherent ignores this.
    invalidateMappedRange(*impl.vk, impl.dumpBuffer.memory);
    writeWorldFrameDump(impl);
    const uint32_t width = impl.display->width();
    const uint32_t height = impl.display->height();
    const auto* source = static_cast<const uint8_t*>(impl.dumpBuffer.mapped);
    if (!source || impl.dumpRgb.size() < size_t(width) * height * 3) {
        std::printf("[FrameDump] the frame staging buffer is not readable\n");
        std::fflush(stdout);
        impl.dumping = false;
        return;
    }

    // The surface's format decides the channel order; PNG wants RGB.
    const VkFormat format = impl.vk->format.format;
    const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgb = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgr && !rgb) {
        std::printf("[FrameDump] swapchain format %d has no channel mapping for a dump\n", int(format));
        std::fflush(stdout);
        impl.dumping = false;
        return;
    }
    packRgbRows(source, width, height, bgr, impl.dumpRgb.data());
    reportFrameContent(impl.dumpRgb, width, height);

    char path[1024];
    const unsigned long long number = static_cast<unsigned long long>(++impl.dumpWritten);
    std::snprintf(path, sizeof(path), "%s/frame-%04llu.png", impl.dumpDirectory.c_str(), number);
    const char* writeError = nullptr;
    if (!writePngRgb(path, width, height, impl.dumpRgb.data(), size_t(width) * 3, &writeError)) {
        std::printf("[FrameDump] %s: %s\n", path, writeError ? writeError : "the frame could not be written");
        std::fflush(stdout);
        impl.dumping = false;
        return;
    }
    --impl.dumpRemaining;
    std::printf("[FrameDump] %s (%ux%u)\n", path, width, height);
    if (!impl.dumpRemaining) {
        std::printf("[FrameDump] wrote %llu frame%s\n",
                    static_cast<unsigned long long>(impl.dumpWritten), impl.dumpWritten == 1 ? "" : "s");
        impl.dumping = false;
    }
    std::fflush(stdout);
}

void GraphicsBackend::Impl::beginInterpolation() {
    const auto now=std::chrono::steady_clock::now();
    if(now>=fgNextCheck) {
        fgNextCheck=now+std::chrono::seconds(1);std::error_code ec;
        const bool requested=std::filesystem::exists("/download0/darkness-ps5/frame-generation.enabled",ec) && !ec;
        if(requested!=fgRequested) {
            fgRequested=requested;fgFailed=false;interpolation.resetHistory();
            fgReportStart=now;fgReportedReal=fgReal;fgReportedGenerated=fgGenerated;
            std::printf("[FrameGeneration] %s: experimental optical flow; real and generated rates are reported separately\n",requested?"enabled":"disabled");
        }
        const auto capture=std::filesystem::path("/download0/darkness-ps5/frame-generation-capture.request");
        ec.clear();if(std::filesystem::exists(capture,ec) && !ec) {
            fgCaptureRequested=true;std::filesystem::remove(capture,ec);
        }
    }
    interpolation.readMetrics();
    writeInterpolationCapture();
    fgScene=fgPresented=false;fgOverlaySafe=true;fgOverlays.clear();
    offscreenFrame=renderOffscreen=false;
    if(!fgRequested || fgFailed)return;
    const char* error=nullptr;
    auto fail=[&] {
        fgFailed=true;std::printf("[FrameGeneration] disabled for session: %s\n",error?error:"resource initialization failed");
    };
    if(!interpolation.created() && !interpolation.create(instance,physical,device,Vk::vk_icdGetInstanceProcAddr,vk->queueFamily,&error)) {fail();return;}
    if(!interpolation.realTarget(vk->format.format,frameWidth,frameHeight,&error)) {fail();return;}
    for(unsigned n=0;n<2;++n)if(!interpolationFramebuffers[n]) {
        const auto view=interpolation.realView();VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass=n?renderPassResume:renderPassFirst;info.attachmentCount=1;info.pAttachments=&view;
        info.width=frameWidth;info.height=frameHeight;info.layers=1;
        if(CreateFramebuffer(device,&info,nullptr,&interpolationFramebuffers[n])!=VK_SUCCESS) {error="offscreen framebuffer failed";fail();return;}
    }
    offscreenFrame=renderOffscreen=true;
}
bool GraphicsBackend::Impl::canReplayOverlays() const {
    if(!fgOverlaySafe || fgOverlays.size()>128)return false;
    VkDeviceSize v=vertexOffset,i=indexOffset;
    for(const auto& mesh:fgOverlays) {
        v=((v+15)&~VkDeviceSize(15))+mesh.vertices.size()*sizeof(SimpleVertex);
        i=((i+15)&~VkDeviceSize(15))+mesh.indices.size()*sizeof(uint16_t);
    }
    return v<=vertexBuffers[image].size && i<=indexBuffers[image].size;
}
void GraphicsBackend::Impl::reportInterpolation() {
    if(!fgRequested && fgReports==0 && fgGenerated==0)return;
    const auto now=std::chrono::steady_clock::now();const auto elapsed=std::chrono::duration<double>(now-fgReportStart).count();
    if(elapsed<1)return;
    char line[384];std::snprintf(line,sizeof(line),
        "[FrameGeneration] requested=%u active=%u realFps=%.2f generatedFps=%.2f displayFps=%.2f flowGpuMs=%.3f warmup=%llu cutFallback=%llu\n",
        unsigned(fgRequested),unsigned(fgRequested&&!fgFailed),(fgReal-fgReportedReal)/elapsed,(fgGenerated-fgReportedGenerated)/elapsed,
        (fgReal-fgReportedReal+fgGenerated-fgReportedGenerated)/elapsed,interpolation.gpuMilliseconds(),
        (unsigned long long)fgWarmup,(unsigned long long)fgCuts);
    std::printf("%s",line);
    if(fgReports<3600)if(FILE* f=std::fopen("/download0/darkness-ps5/fg.txt",fgReports++==0?"w":"a")) {std::fputs(line,f);std::fclose(f);}
    fgReportStart=now;fgReportedReal=fgReal;fgReportedGenerated=fgGenerated;
}
uint64_t GraphicsBackend::generatedFrames() const {return impl_?impl_->fgGenerated:0;}
void GraphicsBackend::Impl::captureInterpolation() {
    if(!fgCaptureRequested)return;fgCaptureRequested=false;
    const auto width=interpolation.generatedWidth(),height=interpolation.generatedHeight();
    const VkDeviceSize bytes=VkDeviceSize(width)*height*16;
    const char* error=nullptr;
    if(fgCaptureBuffer.size<bytes) {
        if(fgCaptureBuffer.mapped)UnmapMemory(device,fgCaptureBuffer.memory);
        if(fgCaptureBuffer.handle)DestroyBuffer(device,fgCaptureBuffer.handle,nullptr);
        if(fgCaptureBuffer.memory)FreeMemory(device,fgCaptureBuffer.memory,nullptr);
        fgCaptureBuffer={};
        if(!createBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,fgCaptureBuffer,&error)) {
            std::printf("[FrameGeneration] capture unavailable: %s\n",error?error:"allocation");
            if(fgCaptureBuffer.mapped)UnmapMemory(device,fgCaptureBuffer.memory);
            if(fgCaptureBuffer.handle)DestroyBuffer(device,fgCaptureBuffer.handle,nullptr);
            if(fgCaptureBuffer.memory)FreeMemory(device,fgCaptureBuffer.memory,nullptr);
            fgCaptureBuffer={};return;
        }
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=interpolation.generatedImage();
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    CmdCopyImageToBuffer(command,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fgCaptureBuffer.handle,1,&copy);
    std::swap(barrier.oldLayout,barrier.newLayout);std::swap(barrier.srcAccessMask,barrier.dstAccessMask);
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=fgCaptureBuffer.handle;host.size=bytes;
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    fgCapturePending=true;fgCaptureWidth=width;fgCaptureHeight=height;
}
void GraphicsBackend::Impl::writeInterpolationCapture() {
    if(!fgCapturePending)return;fgCapturePending=false;
    const size_t pixels=size_t(fgCaptureWidth)*fgCaptureHeight;
    std::vector<uint8_t> rgb(pixels*3);const auto* rgba=static_cast<const float*>(fgCaptureBuffer.mapped);
    for(size_t pixel=0;pixel<pixels;++pixel)for(unsigned channel=0;channel<3;++channel) {
        const float value=rgba[pixel*4+channel];
        rgb[pixel*3+channel]=uint8_t(std::isfinite(value)?std::clamp(value,0.0f,1.0f)*255.0f+0.5f:0.0f);
    }
    const char* error=nullptr;
    const bool saved=writePngRgb("/download0/darkness-ps5/fg-generated.png",fgCaptureWidth,fgCaptureHeight,rgb.data(),size_t(fgCaptureWidth)*3,&error);
    std::printf("[FrameGeneration] generated scene capture %s (%ux%u, HUD composed separately): %s\n",saved?"saved":"failed",fgCaptureWidth,fgCaptureHeight,error?error:"ok");
}

void GraphicsBackend::closeFrame() {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::frame);
    Impl& impl = *impl_;
    if (!impl.open) return;
    impl.open = false;
    impl.world.FlushDraws();
    impl.closePass();
    bool generated=false;
    if(impl.offscreenFrame) {
        if(!impl.passOpened) {impl.ensurePass();impl.closePass();}
        if(impl.fgScene && impl.fgPresented && impl.canReplayOverlays()) {
            const auto scene=impl.world.presentedFrame();const char* failure=nullptr;
            if(!impl.interpolation.prepare(impl.command,scene.image,scene.format,scene.width,scene.height,generated,&failure)) {
                impl.fgFailed=true;std::printf("[FrameGeneration] dispatch disabled: %s\n",failure?failure:"unknown error");
                impl.interpolation.resetHistory();
            } else if(!generated)++impl.fgWarmup;
        } else impl.interpolation.resetHistory();
        impl.renderOffscreen=false;
        if(generated) {
            impl.captureInterpolation();
            impl.passOpened=false;impl.ensurePass();const char* failure=nullptr;
            bool composed=impl.world.PresentGenerated(impl.interpolation.generatedView(),&failure);
            if(composed)for(const auto& overlay:impl.fgOverlays)if(!impl.draw(overlay,&failure)) {composed=false;break;}
            impl.closePass();
            if(composed)impl.finishImage();
            else {
                std::printf("[FrameGeneration] using real frame after composition refusal: %s\n",failure?failure:"unknown");
                generated=false;impl.interpolation.copyReal(impl.command,impl.vk->images[impl.image],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
        } else impl.interpolation.copyReal(impl.command,impl.vk->images[impl.image]);
    } else impl.finishImage();
    if (impl.dumping && impl.dumpRemaining > 0) {
        const char* captureError = nullptr;
        if (!impl.ensureDumpBuffer(&captureError)) {
            std::printf("[FrameDump] cannot copy frames: %s\n", captureError ? captureError : "unknown error");
            std::fflush(stdout);
            impl.dumping = false;
        }
    }
    bool captured = false;
    recordDumpCopy(impl, &captured);
    bool presented=impl.display->EndFrame(impl.image);
    if(presented && generated) {
        ++impl.fgGenerated;
        uint32_t image=0;void* command=nullptr;
        if(impl.display->BeginFrame(image,&command)) {
            impl.interpolation.readMetrics();if(impl.interpolation.sceneCut())++impl.fgCuts;
            impl.writeInterpolationCapture();
            impl.image=image;impl.command=reinterpret_cast<VkCommandBuffer>(command);
            impl.interpolation.copyReal(impl.command,impl.vk->images[image]);
            presented=impl.display->EndFrame(image);
        } else presented=false;
    }
    if (presented) {
        const auto presentedAt=std::chrono::steady_clock::now();
        ++frames_;++impl.fgReal;impl.reportInterpolation();
        const auto captureResult=impl.vertexPreparationCapture.frame(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                presentedAt.time_since_epoch()).count(),
            Native::compactVertexSourcesEnabled.load(std::memory_order_relaxed),
            {frames_,impl.fgGenerated,worldDraws_,worldRejected_+rejected_,Native::profileEngineCpu,
             Native::sampleEngineCpu.load(std::memory_order_relaxed)},
            [](bool enabled){Native::compactVertexSourcesEnabled.store(enabled,std::memory_order_relaxed);});
        if(captureResult==VertexPreparationCapture::Result::failed)
            std::puts("[VertexPreparationCapture] failed; inspect vertex-preparation-error.txt");
        if (captured) writeFrameDump();
        error_ = nullptr;
        return;
    }

    // Out-of-date or lost swapchain: rebuild it and everything that names its
    // images, then let the next frame acquire again. The guest keeps producing
    // while this happens, so the failure is reported and not fatal.
    error_ = impl.display->error();
    impl.destroySwapchainObjects();
    for (Buffer& buffer : impl.vertexBuffers) {
        if (buffer.mapped) impl.UnmapMemory(impl.device, buffer.memory);
        if (buffer.handle) impl.DestroyBuffer(impl.device, buffer.handle, nullptr);
        if (buffer.memory) impl.FreeMemory(impl.device, buffer.memory, nullptr);
        buffer = {};
    }
    for (Buffer& buffer : impl.indexBuffers) {
        if (buffer.mapped) impl.UnmapMemory(impl.device, buffer.memory);
        if (buffer.handle) impl.DestroyBuffer(impl.device, buffer.handle, nullptr);
        if (buffer.memory) impl.FreeMemory(impl.device, buffer.memory, nullptr);
        buffer = {};
    }
    impl.vertexBuffers.clear();
    impl.indexBuffers.clear();
    if (!impl.display->RebuildSwapchain()) {
        error_ = impl.display->error();
        return;
    }
    const char* rebuild = nullptr;
    if (!impl.createSwapchainObjects(&rebuild) || !impl.createBuffers(&rebuild)) {
        error_ = rebuild;
        return;
    }
    // The present pipeline was recorded against the old pass and went with it.
    if (!impl.world.SetSwapchain(impl.renderPassFirst, impl.display->width(), impl.display->height(),
                                 &rebuild)) {
        error_ = rebuild;
    }
}

void GraphicsBackend::render(const std::vector<SimpleMesh>& meshes, Native::PreviewFramePart part) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::frame);
    if (!impl_) return;
    Impl& impl = *impl_;
    if (part.first) {
        if (impl.open) closeFrame();
        uint32_t index = 0;
        void* raw = nullptr;
        if (!impl.display->BeginFrame(index, &raw)) {
            error_ = impl.display->error();
            return;
        }
        if (index >= impl.framebuffers.size() || index >= impl.vertexBuffers.size()) {
            error_ = "the display's swapchain changed without a rebuild";
            return;
        }
        impl.open = true;
        // BeginFrame waited for all previously submitted work.
        for (auto& texture : impl.retiredTextures) impl.release(texture);
        impl.retiredTextures.clear();
        impl.image = index;
        impl.command = reinterpret_cast<VkCommandBuffer>(raw);
        impl.vertexOffset = 0;
        impl.indexOffset = 0;
        impl.frameWidth = impl.display->width();
        impl.frameHeight = impl.display->height();
        impl.passOpen = false;
        impl.passOpened = false;
        impl.beginInterpolation();
        impl.ResetDescriptorPool(impl.device, impl.descriptorPool, 0);
        // The world layer records into the same command buffer and has its own
        // per-frame descriptor pool, which is reset here for the same reason: a
        // set an earlier frame's draw may still use is never rewritten.
        impl.world.BeginFrame();
        impl.world.SetCommandBuffer(impl.command);
        impl.draws.BeginFrame(impl.command);
    }
    if (!impl.open) return; // a frame always starts with its first part

    for (const SimpleMesh& mesh : meshes) {
        // The engine's own render targets cannot be touched while the swapchain
        // image is a render target, because clearing or resolving one ends the
        // pass. The image's pass is closed for the length of the command and
        // reopened -- keeping its contents -- when something draws again.
        if (mesh.worldClear || mesh.worldResolve) {
            impl.closePass();
            const char* error = nullptr;
            const bool done = mesh.worldClear ? impl.world.Clear(*mesh.worldClear, &error)
                                              : impl.world.Resolve(*mesh.worldResolve, &error);
            if (done) {
                (mesh.worldClear ? worldClears_ : worldResolves_)++;
            } else if (worldRejected_++ < 8 && error) {
                std::printf("[World] %s rejected: %s\n", mesh.worldClear ? "clear" : "resolve", error);
                std::fflush(stdout);
            }
            continue;
        }
        if (mesh.worldPresent) {
            impl.ensurePass();
            const char* error = nullptr;
            if (impl.world.Present(*mesh.worldPresent, &error)) {
                ++worldPresents_;
                if(impl.offscreenFrame) {impl.fgPresented=true;impl.fgOverlaySafe=true;impl.fgOverlays.clear();}
            } else if (worldRejected_++ < 8 && error) {
                std::printf("[World] present rejected: %s\n", error);
                std::fflush(stdout);
            }
            continue;
        }
        // A world draw renders into the engine's own targets, so the swapchain
        // image's pass is closed for its length and reopened by the next mesh.
        if (mesh.world) {
            impl.closePass();
            const char* error = nullptr;
            if (impl.draws.Draw(mesh.world, impl.world, &error)) {
                ++worldDraws_;
                if(impl.offscreenFrame && (mesh.world->material==Native::WorldMaterial::depth || mesh.world->material==Native::WorldMaterial::ndsp))impl.fgScene=true;
            } else if (worldRejected_++ < 8 && error) {
                std::printf("[World] draw rejected: %s\n", error);
                std::fflush(stdout);
            }
            continue;
        }
        // The display ramp is kept for the present; an occlusion bracket counts
        // the world draws it encloses (the exposure histogram and flares).
        if (mesh.displayGamma) impl.world.SetDisplayGamma(*mesh.displayGamma);
        if (mesh.worldQuery) impl.draws.Query(mesh.worldQuery, mesh.worldQueryBegin);
        if (mesh.displayGamma || mesh.worldQuery) {
            ++worldCommands_;
            continue;
        }
        impl.ensurePass();
        const char* error = nullptr;
        if (impl.draw(mesh, &error)) {
            ++meshes_;
            if(impl.offscreenFrame && impl.fgPresented) {
                try {impl.fgOverlays.push_back(mesh);}catch(...) {impl.fgOverlaySafe=false;}
            }
        } else if (rejected_++ < 8 && error) {
            std::printf("[Graphics] mesh skipped: %s\n", error);
            std::fflush(stdout);
        }
    }

    if (part.last) closeFrame();
}

void GraphicsBackend::printCounters() const {
    if (!impl_) {
        std::printf("[Graphics] not initialised\n");
        return;
    }
    std::printf(
        "[Graphics] frames=%llu meshes=%llu rejected=%llu unported=%llu uploads=%llu evictions=%llu\n",
        static_cast<unsigned long long>(frames_), static_cast<unsigned long long>(meshes_),
        static_cast<unsigned long long>(rejected_), static_cast<unsigned long long>(worldCommands_),
        static_cast<unsigned long long>(impl_->uploads), static_cast<unsigned long long>(impl_->evictions));
    std::printf("[Graphics] world clears=%llu resolves=%llu presents=%llu rejected=%llu\n",
                static_cast<unsigned long long>(worldClears_),
                static_cast<unsigned long long>(worldResolves_),
                static_cast<unsigned long long>(worldPresents_),
                static_cast<unsigned long long>(worldRejected_));
    impl_->draws.printCounters();
    impl_->world.printCounters();
    std::fflush(stdout);
}

} // namespace DarkRecomp::Ps5
