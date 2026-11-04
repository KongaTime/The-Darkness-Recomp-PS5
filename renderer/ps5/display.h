#pragma once
// The console's presentation path.
//
// The Windows build presents through D3D11 (renderer/d3d11/display_context_d3d11).
// The console has no D3D11: RADV speaks Vulkan to the console's video output
// through the VideoOut WSI, which is a VK_KHR_display surface. This class owns
// that surface, its swapchain and the frames the game presents.
//
// It is the base the graphics backend stands on: the renderer draws into the
// swapchain's images, and Present hands the finished image to the display. As
// of this step it clears each image to a colour, which is what makes "the title
// presents a frame" true before any of the game's drawing is ported.
#include <cstdint>

namespace DarkRecomp::Ps5 {

class Display {
public:
    Display() = default;
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Opens the console's display, creates the swapchain and prepares one
    // command buffer per swapchain image. Returns false and leaves `error()`
    // set if any step fails; a partial setup is torn down.
    bool Open(const char* title);
    void Close();
    bool opened() const { return swapchain_ != nullptr; }

    // Clears the next image to `colour` (linear RGB, 0..1) and presents it.
    // Returns false if the swapchain needs rebuilding; the caller rebuilds and
    // presents again, the way a desktop resize is handled.
    bool PresentClear(float red, float green, float blue);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t presentedFrames() const { return presentedFrames_; }
    // Time spent waiting for the GPU to finish earlier frames before a new one
    // is recorded (BeginFrame), in milliseconds since the display opened.
    double gpuWaitMilliseconds() const { return gpuWaitMilliseconds_; }
    // Time spent in vkAcquireNextImageKHR waiting for a free display image.
    double acquireWaitMilliseconds() const { return acquireWaitMilliseconds_; }
    // Time spent in vkQueuePresentKHR (EndFrame), which may wait for vblank.
    double presentWaitMilliseconds() const { return presentWaitMilliseconds_; }
    // Whether the swapchain's images were created with the transfer-source
    // usage the frame dump copies them with. A display surface that does not
    // support it still presents; dumps are reported as unavailable.
    bool canReadBack() const;
    const char* error() const { return error_; }

    // Recreates the swapchain after a present failure. Open() already does this
    // once, so a caller only needs it when PresentClear reports a failure.
    bool RebuildSwapchain();

    // Frame lifecycle for a renderer that records its own commands. BeginFrame
    // acquires the next image, resets that image's command buffer and returns
    // it; EndFrame submits and presents it, and reports whether the swapchain
    // needs rebuilding. PresentClear is these steps with a clear in between.
    bool BeginFrame(uint32_t& imageIndex, void** commandBuffer);
    bool EndFrame(uint32_t imageIndex);

    // The Vulkan objects, defined in display_vulkan.h. Public only so the
    // console renderer can record into this display's swapchain; nothing
    // outside renderer/ps5 should name it.
    struct Impl;
    Impl* impl() { return impl_; }
    const Impl* impl() const { return impl_; }

private:
    bool CreateSwapchain();
    void DestroySwapchain();

    Impl* impl_ = nullptr;
    void* swapchain_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t imageCount_ = 0;
    uint64_t presentedFrames_ = 0;
    double gpuWaitMilliseconds_ = 0.0;
    double acquireWaitMilliseconds_ = 0.0;
    double presentWaitMilliseconds_ = 0.0;
    const char* error_ = nullptr;
};

} // namespace DarkRecomp::Ps5
