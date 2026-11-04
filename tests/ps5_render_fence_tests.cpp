#include "renderer/ps5/display.h"
#include "renderer/ps5/display_vulkan.h"
#include <cassert>
#include <cstdio>

int order = 0;
bool timeOut = false;
VkResult VKAPI_PTR wait(VkDevice, uint32_t count, const VkFence*, VkBool32 all, uint64_t timeout) {
    assert(count == 1 && all && timeout == UINT64_MAX && order == 0);
    order = 1;
    return timeOut ? VK_TIMEOUT : VK_SUCCESS;
}
VkResult VKAPI_PTR reset(VkDevice, uint32_t count, const VkFence*) {
    assert(count == 1 && order == 1);
    order = 2;
    return VK_SUCCESS;
}
int main() {
    DarkRecomp::Ps5::Display::Impl renderer;
    renderer.WaitForFences = wait;
    renderer.ResetFences = reset;
    assert(renderer.waitForRender() == VK_SUCCESS && order == 0);
    renderer.renderPending = true;
    assert(renderer.waitForRender() == VK_SUCCESS && order == 2 && !renderer.renderPending);
    order = 0; timeOut = true; renderer.renderPending = true;
    assert(renderer.waitForRender() == VK_TIMEOUT && order == 1 && renderer.renderPending);
    std::puts("Render fence: reuse waits for completion; incomplete work is not reset or retired.");
}
