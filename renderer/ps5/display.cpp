#include "display.h"
#include "display_vulkan.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::atomic<int32_t> videoOutHandle{-1};
}
extern "C" int32_t __real_sceVideoOutOpen(int32_t, int32_t, int32_t, const void*);
extern "C" int32_t __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* params) {
    const int32_t handle = __real_sceVideoOutOpen(user, bus, index, params);
    if (handle >= 0) videoOutHandle.store(handle, std::memory_order_relaxed);
    return handle;
}
extern "C" int sceVideoOutGetFlipStatus(int32_t, void*);

namespace {
void sampleVideoOutCadence() {
    const int32_t handle = videoOutHandle.load(std::memory_order_relaxed);
    uint64_t status[16]{};
    if (handle < 0 || sceVideoOutGetFlipStatus(handle, status) != 0) return;
    // Only word 3 (the shown marker) has a known layout in this SDK. Log the
    // raw status initially rather than guessing units for the other fields.
    static uint64_t previous = 0, observations = 0, repeats = 0, jumps = 0, rawReports = 0;
    if (status[3] != previous && rawReports < 32) {
        ++rawReports;
        std::fprintf(stderr, "[VideoOutRaw] %llu %llu %llu %llu %llu %llu %llu %llu\n",
            (unsigned long long)status[0], (unsigned long long)status[1],
            (unsigned long long)status[2], (unsigned long long)status[3],
            (unsigned long long)status[4], (unsigned long long)status[5],
            (unsigned long long)status[6], (unsigned long long)status[7]);
    }
    if (previous) {
        repeats += status[3] == previous;
        jumps += status[3] > previous + 1;
    }
    previous = status[3];
    if (++observations == 120) {
        std::fprintf(stderr, "[VideoOutCadence] samples=%llu sameMarker=%llu skippedMarkers=%llu shown=%llu\n",
            (unsigned long long)observations, (unsigned long long)repeats,
            (unsigned long long)jumps, (unsigned long long)status[3]);
        observations = repeats = jumps = 0;
    }
}
}

namespace DarkRecomp::Ps5 {
namespace {

constexpr uint32_t kFramesInFlight = 2;

VkResult createResult = VK_SUCCESS;

} // namespace

namespace {

bool loadDeviceFunctions(Display::Impl& impl, VkInstance instance, VkDevice device) {
    PFN_vkGetDeviceProcAddr getDeviceProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!getDeviceProc) return false;
    auto load = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::decay_t<decltype(slot)>>(getDeviceProc(device, name));
        return slot != nullptr;
    };
    return load(impl.CreateSwapchain, "vkCreateSwapchainKHR") &&
           load(impl.DestroySwapchain, "vkDestroySwapchainKHR") &&
           load(impl.GetSwapchainImages, "vkGetSwapchainImagesKHR") &&
           load(impl.AcquireNextImage, "vkAcquireNextImageKHR") &&
           load(impl.QueuePresent, "vkQueuePresentKHR") &&
           load(impl.QueueSubmit, "vkQueueSubmit") &&
           load(impl.QueueWaitIdle, "vkQueueWaitIdle") &&
           load(impl.CreateFence, "vkCreateFence") &&
           load(impl.DestroyFence, "vkDestroyFence") &&
           load(impl.WaitForFences, "vkWaitForFences") &&
           load(impl.ResetFences, "vkResetFences") &&
           load(impl.CreateCommandPool, "vkCreateCommandPool") &&
           load(impl.DestroyCommandPool, "vkDestroyCommandPool") &&
           load(impl.AllocateCommandBuffers, "vkAllocateCommandBuffers") &&
           load(impl.BeginCommandBuffer, "vkBeginCommandBuffer") &&
           load(impl.EndCommandBuffer, "vkEndCommandBuffer") &&
           load(impl.PipelineBarrier, "vkCmdPipelineBarrier") &&
           load(impl.ClearColorImage, "vkCmdClearColorImage") &&
           load(impl.CreateSemaphore, "vkCreateSemaphore") &&
           load(impl.DestroySemaphore, "vkDestroySemaphore") &&
           load(impl.CreateImageView, "vkCreateImageView") &&
           load(impl.DestroyImageView, "vkDestroyImageView") &&
           load(impl.DestroyDevice, "vkDestroyDevice");
}

} // namespace

Display::~Display() { Close(); }

bool Display::Open(const char* title) {
    Close();
    impl_ = new Impl();
    Impl& impl = *impl_;

    // The instance. VK_KHR_display is how the console's WSI is reached: RADV
    // speaks to the video output through it, so there is no window system and
    // no surface extension beyond the display one.
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = title;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = extensions;

    auto createInstance = Vk::instanceProc<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
    if (!createInstance) {
        error_ = "RADV does not export vkCreateInstance through its ICD entry point";
        return false;
    }
    // What the driver reports before the instance exists: the extension the
    // console's WSI needs, and whether its dispatch answers at all. A driver
    // that cannot enumerate answers here too, and the two failures read
    // differently in the title's log.
    uint32_t instanceExtensions = 0;
    int extensionResult = -1;
    if (auto enumerateExtensions = Vk::instanceProc<PFN_vkEnumerateInstanceExtensionProperties>(
                nullptr, "vkEnumerateInstanceExtensionProperties")) {
        extensionResult = int(enumerateExtensions(nullptr, &instanceExtensions, nullptr));
        std::vector<VkExtensionProperties> properties(instanceExtensions);
        if (extensionResult == VK_SUCCESS && instanceExtensions > 0) {
            enumerateExtensions(nullptr, &instanceExtensions, properties.data());
            bool hasDisplay = false;
            for (const VkExtensionProperties& property : properties) {
                if (std::strcmp(property.extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0) hasDisplay = true;
            }
            if (!hasDisplay) {
                error_ = "RADV does not offer VK_KHR_display";
                return false;
            }
        }
    }
    if ((createResult = createInstance(&instanceInfo, nullptr, &impl.instance)) != VK_SUCCESS) {
        // The code is what tells the failures apart: -9 is no ICD/RADV at all,
        // -3 allocation, -8 a format the console cannot give. It reaches the
        // kernel log through title_main's trace, which is the only readout a
        // console launch has.
        static char message[160];
        std::snprintf(message, sizeof message,
                      "vkCreateInstance failed (VkResult %d); %u instance extensions, enumerate %d; entry %p",
                      int(createResult), instanceExtensions, extensionResult,
                      reinterpret_cast<const void*>(createInstance));
        error_ = message;
        return false;
    }
    impl.DestroyInstance = Vk::instanceProc<PFN_vkDestroyInstance>(impl.instance, "vkDestroyInstance");
    impl.DestroySurface = Vk::instanceProc<PFN_vkDestroySurfaceKHR>(impl.instance, "vkDestroySurfaceKHR");
    auto getPhysicals = Vk::instanceProc<PFN_vkEnumeratePhysicalDevices>(impl.instance, "vkEnumeratePhysicalDevices");
    auto getImageProc = Vk::instanceProc<PFN_vkGetDeviceProcAddr>(impl.instance, "vkGetDeviceProcAddr");
    (void)getImageProc;
    if (!getPhysicals) {
        error_ = "vkEnumeratePhysicalDevices is unavailable";
        return false;
    }

    uint32_t physicalCount = 0;
    getPhysicals(impl.instance, &physicalCount, nullptr);
    if (physicalCount == 0) {
        error_ = "RADV reports no physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    getPhysicals(impl.instance, &physicalCount, physicals.data());
    impl.physical = physicals[0];

    // The display: the console exposes one, and its first mode is the output.
    auto getDisplays = Vk::instanceProc<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
        impl.instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    auto getModes = Vk::instanceProc<PFN_vkGetDisplayModePropertiesKHR>(impl.instance, "vkGetDisplayModePropertiesKHR");
    auto getPlanes = Vk::instanceProc<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
        impl.instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    auto getPlaneDisplays = Vk::instanceProc<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
        impl.instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    auto createSurface = Vk::instanceProc<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        impl.instance, "vkCreateDisplayPlaneSurfaceKHR");
    if (!getDisplays || !getModes || !getPlanes || !getPlaneDisplays || !createSurface) {
        error_ = "the display extension entry points are missing";
        return false;
    }

    uint32_t displayCount = 0;
    getDisplays(impl.physical, &displayCount, nullptr);
    if (displayCount == 0) {
        error_ = "no display is attached to the console's output";
        return false;
    }
    std::vector<VkDisplayPropertiesKHR> displays(displayCount);
    getDisplays(impl.physical, &displayCount, displays.data());

    uint32_t modeCount = 0;
    getModes(impl.physical, displays[0].display, &modeCount, nullptr);
    if (modeCount == 0) {
        error_ = "the display reports no modes";
        return false;
    }
    std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
    getModes(impl.physical, displays[0].display, &modeCount, modes.data());

    // The largest mode is the one the console outputs (3840x2160 on a PS5).
    uint32_t mode = 0;
    for (uint32_t i = 1; i < modeCount; ++i) {
        const auto a = modes[i].parameters.visibleRegion;
        const auto b = modes[mode].parameters.visibleRegion;
        if (uint32_t(a.width) * uint32_t(a.height) > uint32_t(b.width) * uint32_t(b.height)) mode = i;
    }
    const VkDisplayModeParametersKHR parameters = modes[mode].parameters;
    width_ = parameters.visibleRegion.width;
    height_ = parameters.visibleRegion.height;

    uint32_t planeCount = 0;
    getPlanes(impl.physical, &planeCount, nullptr);
    if (planeCount == 0) {
        error_ = "the display reports no planes";
        return false;
    }
    std::vector<VkDisplayPlanePropertiesKHR> planes(planeCount);
    getPlanes(impl.physical, &planeCount, planes.data());
    uint32_t plane = 0;
    for (uint32_t i = 0; i < planeCount; ++i) {
        if (planes[i].currentDisplay == displays[0].display) {
            plane = i;
            break;
        }
    }

    VkDisplaySurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.displayMode = modes[mode].displayMode;
    surfaceInfo.planeIndex = plane;
    surfaceInfo.planeStackIndex = planes[plane].currentStackIndex;
    surfaceInfo.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surfaceInfo.globalAlpha = 1.0f;
    surfaceInfo.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surfaceInfo.imageExtent = parameters.visibleRegion;
    if ((createResult = createSurface(impl.instance, &surfaceInfo, nullptr, &impl.surface)) != VK_SUCCESS) {
        error_ = "vkCreateDisplayPlaneSurfaceKHR failed";
        return false;
    }

    // A queue family that can both draw and present.
    auto getQueueFamilies = Vk::instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        impl.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getSurfaceSupport = Vk::instanceProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        impl.instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    impl.SurfaceCapabilities = Vk::instanceProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        impl.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    impl.SurfaceFormats = Vk::instanceProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        impl.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    impl.SurfacePresentModes = Vk::instanceProc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        impl.instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!getQueueFamilies || !getSurfaceSupport || !impl.SurfaceCapabilities || !impl.SurfaceFormats ||
        !impl.SurfacePresentModes) {
        error_ = "the surface query entry points are missing";
        return false;
    }
    uint32_t familyCount = 0;
    getQueueFamilies(impl.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    getQueueFamilies(impl.physical, &familyCount, families.data());
    bool found = false;
    for (uint32_t i = 0; i < familyCount && !found; ++i) {
        VkBool32 present = VK_FALSE;
        getSurfaceSupport(impl.physical, i, impl.surface, &present);
        if (present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            impl.queueFamily = i;
            found = true;
        }
    }
    if (!found) {
        error_ = "no queue family can both draw and present";
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = impl.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                      VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    // The engine's exposure histogram counts passing samples with occlusion
    // queries, so the counts must be exact rather than any-passed. The world
    // draw layer asks the same question and uses the precise bit only when it
    // is available, which is when this enables it.
    VkPhysicalDeviceFeatures supported{}, enabled{};
    if (auto getFeatures = Vk::instanceProc<PFN_vkGetPhysicalDeviceFeatures>(impl.instance,
                                                                              "vkGetPhysicalDeviceFeatures")) {
        getFeatures(impl.physical, &supported);
        enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
        enabled.samplerAnisotropy = supported.samplerAnisotropy;
    }
    deviceInfo.pEnabledFeatures = &enabled;
    // The world stores D24-style depths in D32 storage on RADV. Its polygon
    // offsets must still use a fixed 24-bit UNORM step, rather than a float
    // ULP that shrinks with reversed-Z distance.
    VkPhysicalDeviceDepthBiasControlFeaturesEXT depthBiasFeatures{};
    depthBiasFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT;
    depthBiasFeatures.depthBiasControl = VK_TRUE;
    depthBiasFeatures.leastRepresentableValueForceUnormRepresentation = VK_TRUE;
    depthBiasFeatures.depthBiasExact = VK_TRUE;
    deviceInfo.pNext = &depthBiasFeatures;
    auto createDevice = Vk::instanceProc<PFN_vkCreateDevice>(impl.instance, "vkCreateDevice");
    if (!createDevice) {
        error_ = "vkCreateDevice is unavailable";
        return false;
    }
    if ((createResult = createDevice(impl.physical, &deviceInfo, nullptr, &impl.device)) != VK_SUCCESS) {
        error_ = "vkCreateDevice failed";
        return false;
    }
    if (!loadDeviceFunctions(impl, impl.instance, impl.device)) {
        error_ = "a device entry point RADV should provide is missing";
        return false;
    }
    auto getQueue = Vk::instanceProc<PFN_vkGetDeviceQueue>(impl.instance, "vkGetDeviceQueue");
    getQueue(impl.device, impl.queueFamily, 0, &impl.queue);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (impl.CreateFence(impl.device, &fenceInfo, nullptr, &impl.renderFence) != VK_SUCCESS) {
        error_ = "vkCreateFence failed";
        return false;
    }

    if (!CreateSwapchain()) return false;
    std::printf("[Display] %ux%u on RADV, %u swapchain images\n", width_, height_, imageCount_);
    std::fflush(stdout);
    return true;
}

bool Display::CreateSwapchain() {
    Impl& impl = *impl_;

    VkSurfaceCapabilitiesKHR capabilities{};
    if (impl.SurfaceCapabilities(impl.physical, impl.surface, &capabilities) != VK_SUCCESS) {
        error_ = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }
    if (capabilities.currentExtent.width != 0xffffffffu) {
        width_ = capabilities.currentExtent.width;
        height_ = capabilities.currentExtent.height;
    }

    uint32_t formatCount = 0;
    impl.SurfaceFormats(impl.physical, impl.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount ? formatCount : 1);
    if (formatCount == 0) {
        error_ = "the surface reports no formats";
        return false;
    }
    impl.SurfaceFormats(impl.physical, impl.surface, &formatCount, formats.data());
    // The frontbuffer already holds display-encoded values (the engine's ramp
    // ends in the panel's gamma, as the Xbox scan-out did), so the swapchain
    // must store them as they are. An _SRGB format would encode them a second
    // time and wash the picture out; a UNORM one in the sRGB colour space is
    // what the Windows swap chain uses.
    const auto isSrgbFormat = [](VkFormat format) {
        return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
               format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    };
    impl.format = formats[0];
    int rank = -1;
    for (const auto& candidate : formats) {
        std::printf("[Display] surface format %d colour space %d\n", int(candidate.format), int(candidate.colorSpace));
        int candidateRank = 0;
        if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) candidateRank += 2;
        if (!isSrgbFormat(candidate.format)) candidateRank += 4;
        if (candidateRank > rank) {
            rank = candidateRank;
            impl.format = candidate;
        }
    }
    std::printf("[Display] swapchain format %d colour space %d\n", int(impl.format.format),
                int(impl.format.colorSpace));

    const uint32_t wanted = capabilities.minImageCount + 1;
    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = impl.surface;
    swapchainInfo.minImageCount = wanted;
    swapchainInfo.imageFormat = impl.format.format;
    swapchainInfo.imageColorSpace = impl.format.colorSpace;
    swapchainInfo.imageExtent = {width_, height_};
    swapchainInfo.imageArrayLayers = 1;
    // Clearing needs the transfer usage; the renderer will draw into them.
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // The debug frame dump copies a presented image into a staging buffer, so
    // ask for the transfer-source usage where the surface allows it.
    impl.readable = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (impl.readable) swapchainInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if ((createResult = impl.CreateSwapchain(impl.device, &swapchainInfo, nullptr, &swapchain)) != VK_SUCCESS) {
        error_ = "vkCreateSwapchainKHR failed";
        return false;
    }
    swapchain_ = reinterpret_cast<void*>(swapchain);

    uint32_t images = 0;
    impl.GetSwapchainImages(impl.device, swapchain, &images, nullptr);
    impl.images.resize(images);
    impl.GetSwapchainImages(impl.device, swapchain, &images, impl.images.data());
    imageCount_ = images;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = impl.format.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl.views.resize(images);
    for (uint32_t i = 0; i < images; ++i) {
        viewInfo.image = impl.images[i];
        if (impl.CreateImageView(impl.device, &viewInfo, nullptr, &impl.views[i]) != VK_SUCCESS) {
            error_ = "vkCreateImageView failed";
            return false;
        }
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = impl.queueFamily;
    if (impl.CreateCommandPool(impl.device, &poolInfo, nullptr, &impl.commandPool) != VK_SUCCESS) {
        error_ = "vkCreateCommandPool failed";
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = impl.commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = images;
    impl.commandBuffers.resize(images);
    if (impl.AllocateCommandBuffers(impl.device, &allocate, impl.commandBuffers.data()) != VK_SUCCESS) {
        error_ = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    impl.acquireDone.resize(kFramesInFlight);
    impl.rendered.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (impl.CreateSemaphore(impl.device, &semaphoreInfo, nullptr, &impl.acquireDone[i]) != VK_SUCCESS ||
            impl.CreateSemaphore(impl.device, &semaphoreInfo, nullptr, &impl.rendered[i]) != VK_SUCCESS) {
            error_ = "vkCreateSemaphore failed";
            return false;
        }
    }
    return true;
}

void Display::DestroySwapchain() {
    if (!impl_ || impl_->device == VK_NULL_HANDLE) {
        swapchain_ = nullptr;
        return;
    }
    Impl& impl = *impl_;
    if (impl.device && impl.queue && impl.QueueWaitIdle) impl.QueueWaitIdle(impl.queue);
    for (auto semaphore : impl.acquireDone) impl.DestroySemaphore(impl.device, semaphore, nullptr);
    for (auto semaphore : impl.rendered) impl.DestroySemaphore(impl.device, semaphore, nullptr);
    impl.acquireDone.clear();
    impl.rendered.clear();
    if (impl.commandPool) {
        impl.DestroyCommandPool(impl.device, impl.commandPool, nullptr);
        impl.commandPool = VK_NULL_HANDLE;
    }
    impl.commandBuffers.clear();
    for (auto view : impl.views) impl.DestroyImageView(impl.device, view, nullptr);
    impl.views.clear();
    impl.images.clear();
    if (swapchain_) {
        impl.DestroySwapchain(impl.device, reinterpret_cast<VkSwapchainKHR>(swapchain_), nullptr);
        swapchain_ = nullptr;
    }
    imageCount_ = 0;
    impl.readable = false;
}

bool Display::RebuildSwapchain() {
    if (!impl_ || impl_->device == VK_NULL_HANDLE) return false;
    DestroySwapchain();
    return CreateSwapchain();
}

bool Display::BeginFrame(uint32_t& imageIndex, void** commandBuffer) {
    if (!impl_ || !swapchain_) {
        error_ = "the display is not open";
        return false;
    }
    Impl& impl = *impl_;
    const uint32_t slot = uint32_t(presentedFrames_ % kFramesInFlight);
    // Shared arenas/caches still require the previous render to finish before
    // reuse. Wait for that submission, rather than stalling the entire queue
    // (including presentation). Resource retirement remains after this wait.
    const auto waitStart = std::chrono::steady_clock::now();
    const VkResult idle = impl.waitForRender();
    gpuWaitMilliseconds_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    if (idle != VK_SUCCESS) {
        error_ = "render fence failed before frame reuse";
        return false;
    }

    uint32_t index = 0;
    const auto acquireStart = std::chrono::steady_clock::now();
    const VkResult acquired = impl.AcquireNextImage(impl.device, reinterpret_cast<VkSwapchainKHR>(swapchain_),
                                                    UINT64_MAX, impl.acquireDone[slot], VK_NULL_HANDLE, &index);
    acquireWaitMilliseconds_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart).count();
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        error_ = "the swapchain needs rebuilding";
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        error_ = "vkAcquireNextImageKHR failed";
        return false;
    }

    VkCommandBuffer command = impl.commandBuffers[index];
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (impl.BeginCommandBuffer(command, &beginInfo) != VK_SUCCESS) {
        error_ = "vkBeginCommandBuffer failed";
        return false;
    }
    imageIndex = index;
    if (commandBuffer) *commandBuffer = command;
    return true;
}

bool Display::EndFrame(uint32_t index) {
    Impl& impl = *impl_;
    const uint32_t slot = uint32_t(presentedFrames_ % kFramesInFlight);
    VkCommandBuffer command = impl.commandBuffers[index];
    if (impl.EndCommandBuffer(command) != VK_SUCCESS) {
        error_ = "vkEndCommandBuffer failed";
        return false;
    }

    // The acquire semaphore is waited on in the submit; the render-finished
    // semaphore tells the display when the image is ready. Both the clear path
    // (transfer) and the renderer's render pass (colour attachment) wait here,
    // so an image is never written before the display has handed it over.
    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &impl.acquireDone[slot];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &impl.rendered[slot];
    if ((createResult = impl.QueueSubmit(impl.queue, 1, &submit, impl.renderFence)) != VK_SUCCESS) {
        error_ = "vkQueueSubmit failed";
        return false;
    }
    impl.renderPending = true;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &impl.rendered[slot];
    present.swapchainCount = 1;
    const VkSwapchainKHR chain = reinterpret_cast<VkSwapchainKHR>(swapchain_);
    present.pSwapchains = &chain;
    present.pImageIndices = &index;
    const auto presentStart = std::chrono::steady_clock::now();
    const VkResult presented = impl.QueuePresent(impl.queue, &present);
    presentWaitMilliseconds_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        error_ = "the swapchain needs rebuilding";
        return false;
    }
    if (presented != VK_SUCCESS) {
        error_ = "vkQueuePresentKHR failed";
        return false;
    }
    ++presentedFrames_;
    sampleVideoOutCadence();
    return true;
}

bool Display::PresentClear(float red, float green, float blue) {
    uint32_t index = 0;
    void* raw = nullptr;
    if (!BeginFrame(index, &raw)) return false;
    Impl& impl = *impl_;
    VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(raw);

    // The presented image is in an undefined layout: move it to a transfer
    // destination, clear it, and hand it to the display.
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = impl.images[index];
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl.PipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toTransfer);

    VkClearColorValue colour{};
    colour.float32[0] = red;
    colour.float32[1] = green;
    colour.float32[2] = blue;
    colour.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    impl.ClearColorImage(command, impl.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    impl.PipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toPresent);
    return EndFrame(index);
}

bool Display::canReadBack() const {
    return impl_ && impl_->readable;
}

void Display::Close() {
    if (!impl_) return;
    Impl& impl = *impl_;
    DestroySwapchain();
    if (impl.device) {
        if (impl.queue && impl.QueueWaitIdle) impl.QueueWaitIdle(impl.queue);
        if (impl.renderFence && impl.DestroyFence) impl.DestroyFence(impl.device, impl.renderFence, nullptr);
        if (impl.DestroyDevice) impl.DestroyDevice(impl.device, nullptr);
        impl.device = VK_NULL_HANDLE;
    }
    if (impl.surface && impl.DestroySurface) impl.DestroySurface(impl.instance, impl.surface, nullptr);
    impl.surface = VK_NULL_HANDLE;
    if (impl.instance && impl.DestroyInstance) impl.DestroyInstance(impl.instance, nullptr);
    impl.instance = VK_NULL_HANDLE;
    delete impl_;
    impl_ = nullptr;
    swapchain_ = nullptr;
}

} // namespace DarkRecomp::Ps5
