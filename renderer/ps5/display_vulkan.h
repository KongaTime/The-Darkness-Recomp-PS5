#pragma once
// The Vulkan objects behind the console display, shared with its renderer.
//
// Display owns the instance, the physical device, the VK_KHR_display surface
// and the swapchain. The graphics backend draws into that swapchain, so it
// needs the same device and entry points. They live here rather than inside
// display.cpp because two translation units name them; the header is internal
// to renderer/ps5 and is not part of the title's interface.
#include "display.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace DarkRecomp::Ps5 {
namespace Vk {

// The console's RADV is an ICD: everything is reached through its dispatch
// table, and vkCreateInstance/vkGetInstanceProcAddr are the only globals it
// exports.
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

template <typename Fn>
Fn instanceProc(VkInstance instance, const char* name) {
    return reinterpret_cast<Fn>(vk_icdGetInstanceProcAddr(instance, name));
}

template <typename Fn>
Fn deviceProc(VkInstance instance, const char* name) {
    return instanceProc<Fn>(instance, name);
}

} // namespace Vk

struct Display::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{};
    // True when the surface accepted VK_IMAGE_USAGE_TRANSFER_SRC_BIT, which the
    // frame dump needs to copy a presented image out of the swapchain.
    bool readable = false;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> acquireDone;
    std::vector<VkSemaphore> rendered;
    VkFence renderFence = VK_NULL_HANDLE;
    bool renderPending = false;

    // Device-level entry points.
    PFN_vkCreateSwapchainKHR CreateSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImage = nullptr;
    PFN_vkQueuePresentKHR QueuePresent = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    VkResult waitForRender() {
        if (!renderPending) return VK_SUCCESS;
        VkResult result = WaitForFences(device, 1, &renderFence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) return result;
        result = ResetFences(device, 1, &renderFence);
        if (result == VK_SUCCESS) renderPending = false;
        return result;
    }
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier PipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage ClearColorImage = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;

    // Instance-level entry points.
    PFN_vkDestroySurfaceKHR DestroySurface = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR SurfaceCapabilities = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR SurfaceFormats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR SurfacePresentModes = nullptr;
};

} // namespace DarkRecomp::Ps5
