// Drive the actual Vulkan clear path: stencil-only clears must retain depth.
#include "renderer/ps5/world_surfaces.cpp"
#include <cassert>

namespace {
float storedDepth = 0;
uint32_t storedStencil = 0;
bool scaledImageSeen = false, scaledRectangleSeen = false;
VkResult VKAPI_PTR createImage(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*, VkImage*) {
    assert(info->extent.width == 2560 && info->extent.height == 1440);
    scaledImageSeen = true;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY; // Stop before real allocation.
}
void VKAPI_PTR beginPass(VkCommandBuffer, const VkRenderPassBeginInfo* info, VkSubpassContents) {
    assert(info->renderArea.extent.width == 2560 && info->renderArea.extent.height == 1440);
}
void VKAPI_PTR endPass(VkCommandBuffer) {}
void VKAPI_PTR clearRectangle(VkCommandBuffer, uint32_t count, const VkClearAttachment* attachment,
                             uint32_t rectCount, const VkClearRect* rect) {
    assert(count == 1 && rectCount == 1 && attachment->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT);
    assert(rect->rect.offset.x == 20 && rect->rect.offset.y == 40);
    assert(rect->rect.extent.width == 80 && rect->rect.extent.height == 60);
    scaledRectangleSeen = true;
}
void VKAPI_PTR barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                      VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                      const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
void VKAPI_PTR clear(VkCommandBuffer, VkImage, VkImageLayout,
                    const VkClearDepthStencilValue* value, uint32_t count,
                    const VkImageSubresourceRange* ranges) {
    assert(count == 1 && ranges[0].baseArrayLayer == 0 && ranges[0].layerCount == 1);
    if (ranges[0].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) storedDepth = value->depth;
    if (ranges[0].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) storedStencil = value->stencil;
}
}

int main() {
    using namespace DarkRecomp::Ps5;
    WorldSurfaces::Impl renderer;
    renderer.CmdPipelineBarrier = barrier;
    renderer.CmdClearDepthStencilImage = clear;
    Surface surface;
    surface.depth = true;
    surface.format = kDepthFormat;
    surface.image = (VkImage)1;
    surface.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    Policy::ClearRegion region;
    region.valid = region.full = true;
    float color[4]{};
    const char* error = nullptr;
    assert(renderer.clearImage(surface, region, color, 0.25f, 127, 48, &error));
    assert(storedDepth == 0.25f && storedStencil == 127);
    assert(renderer.clearImage(surface, region, color, 1.0f, 128, 32, &error));
    assert(storedDepth == 0.25f && storedStencil == 128);
    assert(renderer.clearImage(surface, region, color, 0.5f, 0, 16, &error));
    assert(storedDepth == 0.5f && storedStencil == 128);
    renderer.scale = 2;
    renderer.CreateImage = createImage;
    Image image;
    assert(!renderer.createImage(image, kColorFormat, 1280, 720, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_IMAGE_VIEW_TYPE_2D, &error));
    assert(scaledImageSeen);
    surface.width = 1280; surface.height = 720;
    renderer.CmdBeginRenderPass = beginPass;
    renderer.CmdEndRenderPass = endPass;
    renderer.CmdClearAttachments = clearRectangle;
    region.full = false; region.rect = {10, 20, 50, 50};
    assert(renderer.clearImage(surface, region, color, 1.0f, 128, 32, &error));
    assert(scaledRectangleSeen);
    std::puts("World clears: stencil-only retains depth; depth-only retains stencil; combined clears both.");
}
