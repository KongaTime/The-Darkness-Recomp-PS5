#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace DarkRecomp::Ps5 {
// Experimental image-based interpolation. All resource reuse is bracketed by
// the display's existing render fence. No guest simulation or input ticks change.
class FrameInterpolation {
public:
    FrameInterpolation()=default;
    FrameInterpolation(const FrameInterpolation&)=delete;
    FrameInterpolation& operator=(const FrameInterpolation&)=delete;
    ~FrameInterpolation();
    bool create(VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,uint32_t queueFamily,const char** error);
    void destroy();
    bool created() const {return impl_!=nullptr;}
    bool realTarget(VkFormat,uint32_t width,uint32_t height,const char** error);
    VkImageView realView() const;
    // A first frame copies history but does not synthesize an image. The source
    // must be sampled/transfer-source capable and in SHADER_READ_ONLY_OPTIMAL.
    bool prepare(VkCommandBuffer,VkImage,VkFormat,uint32_t width,uint32_t height,bool& generated,const char** error);
    VkImageView generatedView() const;
    VkImage generatedImage() const;
    uint32_t generatedWidth() const;
    uint32_t generatedHeight() const;
    void resetHistory();
    // The real target was rendered in COLOR_ATTACHMENT_OPTIMAL. Copies it to
    // an acquired swapchain image and leaves that image ready for presentation.
    void copyReal(VkCommandBuffer,VkImage destination,VkImageLayout before=VK_IMAGE_LAYOUT_UNDEFINED,
                  VkImageLayout after=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    // Called only after the display fence has completed the recorded dispatch.
    void readMetrics();
    double gpuMilliseconds() const;
    bool sceneCut() const;
private:
    struct Impl;
    Impl* impl_=nullptr;
};
}
