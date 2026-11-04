#include "renderer/ps5/world_surfaces.cpp"
#include <cassert>

unsigned begun = 0, ended = 0;
VkResult VKAPI_PTR framebuffer(VkDevice, const VkFramebufferCreateInfo* info,
                              const VkAllocationCallbacks*, VkFramebuffer* output) {
    assert(info->width == 1280 && info->height == 720 && info->attachmentCount == 2);
    *output = (VkFramebuffer)(uintptr_t(begun) + 1);
    return VK_SUCCESS;
}
void VKAPI_PTR begin(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents) { ++begun; }
void VKAPI_PTR end(VkCommandBuffer) { ++ended; }

int main() {
    using namespace DarkRecomp::Ps5;
    WorldSurfaces::Impl renderer;
    renderer.command = (VkCommandBuffer)1;
    renderer.CreateFramebuffer = framebuffer;
    renderer.CmdBeginRenderPass = begin;
    renderer.CmdEndRenderPass = end;
    DarkRecomp::Native::WorldDraw draw;
    draw.material = DarkRecomp::Native::WorldMaterial::post;
    draw.viewport = {0, 0, 1280, 720};
    draw.targets[0] = 10;
    draw.targets[4] = 20;
    for (unsigned slot : {0u, 4u}) {
        Surface surface;
        surface.depth = slot == 4;
        surface.width = 1280; surface.height = 720;
        surface.attachment = (VkImageView)(uintptr_t(slot) + 1);
        renderer.surfaces.emplace(draw.surfaceKey(slot), surface);
    }
    WorldSurfaces::DrawTarget target;
    const char* error = nullptr;
    assert(renderer.beginDraw(draw, target, &error));
    assert(target.serial == 1);
    assert(renderer.beginDraw(draw, target, &error));
    assert(begun == 1 && ended == 0);
    assert(target.serial == 1);
    draw.targets[0] = 30;
    renderer.surfaces.emplace(draw.surfaceKey(0), renderer.surfaces.begin()->second);
    renderer.surfaces.at(draw.surfaceKey(0)).depth = false;
    assert(renderer.beginDraw(draw, target, &error));
    assert(begun == 2 && ended == 1);
    assert(target.serial == 2);
    renderer.flushDraws();
    renderer.flushDraws();
    assert(ended == 2 && !renderer.drawPassOpen && renderer.renderPasses == 2);
    assert(renderer.beginDraw(draw, target, &error));
    assert(begun == 3 && ended == 2);
    renderer.flushDraws();
    std::puts("World passes: consecutive draws batch; target switches and transfers close the pass.");
}
