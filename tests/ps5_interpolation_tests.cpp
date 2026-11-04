#include "renderer/ps5/frame_interpolation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using DarkRecomp::Ps5::FrameInterpolation;
static std::atomic<unsigned> validationErrors{};
static void require(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
static void check(VkResult value,const char* text) {require(value==VK_SUCCESS,text);}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {++validationErrors;std::fprintf(stderr,"Vulkan: %s\n",data->pMessage);}
    return VK_FALSE;
}
struct Vulkan {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkCommandPool pool{};VkCommandBuffer cmd{};uint32_t family=0;VkDebugUtilsMessengerEXT messenger{};
    Vulkan() {
        uint32_t count=0;vkEnumerateInstanceLayerProperties(&count,nullptr);std::vector<VkLayerProperties> layers(count);vkEnumerateInstanceLayerProperties(&count,layers.data());
        const char* name="VK_LAYER_KHRONOS_validation";bool validation=false;
        for(const auto& layer:layers)if(!std::strcmp(layer.layerName,name))validation=true;
        require(validation,"Vulkan validation layer is required for this integration test");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
        const char* extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const VkValidationFeatureEnableEXT feature=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};features.enabledValidationFeatureCount=1;features.pEnabledValidationFeatures=&feature;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
        info.pNext=&features;info.enabledExtensionCount=2;info.ppEnabledExtensionNames=extensions;info.enabledLayerCount=1;info.ppEnabledLayerNames=&name;
        check(vkCreateInstance(&info,nullptr,&instance),"Create Vulkan instance");
        VkDebugUtilsMessengerCreateInfoEXT callback{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        callback.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;callback.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        callback.pfnUserCallback=debug;
        auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        check(create(instance,&callback,nullptr,&messenger),"Create validation messenger");
        vkEnumeratePhysicalDevices(instance,&count,nullptr);require(count!=0,"No Vulkan test device");
        std::vector<VkPhysicalDevice> devices(count);vkEnumeratePhysicalDevices(instance,&count,devices.data());physical=devices[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
        for(;family<count;++family)if((families[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))break;
        require(family<count,"No graphics/compute test queue");float priority=1;
        VkDeviceQueueCreateInfo queues{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queues.queueFamilyIndex=family;queues.queueCount=1;queues.pQueuePriorities=&priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queues;
        check(vkCreateDevice(physical,&deviceInfo,nullptr,&device),"Create test device");vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"Create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=pool;allocation.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&allocation,&cmd),"Allocate command buffer");
    }
    ~Vulkan() {
        if(device) {vkDeviceWaitIdle(device);vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);}
        if(messenger)reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if(instance)vkDestroyInstance(instance,nullptr);
    }
    uint32_t memory(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(physical,&props);
        for(uint32_t i=0;i<props.memoryTypeCount;++i)if((bits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("No test memory type");
    }
    void begin() {check(vkResetCommandPool(device,pool,0),"Reset command pool");VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(cmd,&info),"Begin command buffer");}
    void submit() {
        check(vkEndCommandBuffer(cmd),"End command buffer");VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&cmd;
        check(vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE),"Submit test work");check(vkQueueWaitIdle(queue),"Wait for test work");
    }
};
struct Fixture {
    Vulkan vk;FrameInterpolation fg;VkImage scene{};VkDeviceMemory sceneMemory{};
    VkBuffer staging{};VkDeviceMemory stagingMemory{};float* mapped=nullptr;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;uint32_t width=0,height=0;
    Fixture() {
        const char* error=nullptr;require(fg.create(vk.instance,vk.physical,vk.device,vkGetInstanceProcAddr,vk.family,&error),error?error:"Create frame interpolation");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=256*192*16;
        buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(vk.device,&buffer,nullptr,&staging),"Create staging buffer");
        VkMemoryRequirements required{};vkGetBufferMemoryRequirements(vk.device,staging,&required);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=required.size;
        allocate.memoryTypeIndex=vk.memory(required.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(vk.device,&allocate,nullptr,&stagingMemory),"Allocate staging memory");check(vkBindBufferMemory(vk.device,staging,stagingMemory,0),"Bind staging memory");
        check(vkMapMemory(vk.device,stagingMemory,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&mapped)),"Map staging memory");
    }
    ~Fixture() {
        vkDeviceWaitIdle(vk.device);fg.destroy();
        if(scene)vkDestroyImage(vk.device,scene,nullptr);if(sceneMemory)vkFreeMemory(vk.device,sceneMemory,nullptr);
        vkUnmapMemory(vk.device,stagingMemory);vkDestroyBuffer(vk.device,staging,nullptr);vkFreeMemory(vk.device,stagingMemory,nullptr);
    }
    void resize(uint32_t w,uint32_t h) {
        if(width==w && height==h)return;
        if(scene)vkDestroyImage(vk.device,scene,nullptr);if(sceneMemory)vkFreeMemory(vk.device,sceneMemory,nullptr);
        width=w;height=h;layout=VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R32G32B32A32_SFLOAT;
        info.extent={w,h,1};info.mipLevels=1;info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(vk.device,&info,nullptr,&scene),"Create scene image");
        VkMemoryRequirements required{};vkGetImageMemoryRequirements(vk.device,scene,&required);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=required.size;
        allocate.memoryTypeIndex=vk.memory(required.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(vk.device,&allocate,nullptr,&sceneMemory),"Allocate scene memory");check(vkBindImageMemory(vk.device,scene,sceneMemory,0),"Bind scene image");
    }
    void barrier(VkImage image,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=before;b.newLayout=after;
        b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
    bool frame(const std::vector<float>& color,uint32_t w,uint32_t h,std::vector<float>& output) {
        resize(w,h);std::memcpy(mapped,color.data(),color.size()*4);vk.begin();
        barrier(scene,layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
        vkCmdCopyBufferToImage(vk.cmd,staging,scene,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(scene,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bool generated=false;const char* error=nullptr;
        require(fg.prepare(vk.cmd,scene,VK_FORMAT_R32G32B32A32_SFLOAT,w,h,generated,&error),error?error:"Prepare interpolation");
        if(generated) {
            barrier(fg.generatedImage(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(vk.cmd,fg.generatedImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,1,&copy);
            barrier(fg.generatedImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT);
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&memory,0,nullptr,0,nullptr);
        }
        vk.submit();fg.readMetrics();
        if(generated)output.assign(mapped,mapped+color.size());else output.clear();
        return generated;
    }
    void realCopy() {
        const char* error=nullptr;
        require(fg.realTarget(VK_FORMAT_R32G32B32A32_SFLOAT,width,height,&error),error?error:"Create real-frame target");
        VkAttachmentDescription color{};color.format=VK_FORMAT_R32G32B32A32_SFLOAT;color.samples=VK_SAMPLE_COUNT_1_BIT;
        color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;color.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&ref;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&color;
        passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;VkRenderPass pass{};check(vkCreateRenderPass(vk.device,&passInfo,nullptr,&pass),"Create real-copy pass");
        const auto view=fg.realView();VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass;
        fb.attachmentCount=1;fb.pAttachments=&view;fb.width=width;fb.height=height;fb.layers=1;VkFramebuffer framebuffer{};
        check(vkCreateFramebuffer(vk.device,&fb,nullptr,&framebuffer),"Create real-copy framebuffer");
        vk.begin();VkClearValue clear{};clear.color={{0.2f,0.4f,0.6f,1.0f}};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=framebuffer;
        begin.renderArea={{0,0},{width,height}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(vk.cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(vk.cmd);
        fg.copyReal(vk.cmd,scene,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(vk.cmd,scene,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,1,&copy);
        barrier(scene,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT);
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&memory,0,nullptr,0,nullptr);
        vk.submit();
        for(size_t at=0;at<size_t(width)*height;++at)for(unsigned channel=0;channel<4;++channel)
            require(std::abs(mapped[at*4+channel]-clear.color.float32[channel])<1e-6,"Real-frame presentation copy altered color");
        vkDestroyFramebuffer(vk.device,framebuffer,nullptr);vkDestroyRenderPass(vk.device,pass,nullptr);
    }
};
static std::vector<float> picture(uint32_t w,uint32_t h,float dx=0,float dy=0) {
    std::vector<float> pixels(size_t(w)*h*4);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const float a=std::clamp(float(x)-dx,0.0f,float(w-1)),b=std::clamp(float(y)-dy,0.0f,float(h-1));
        const float value=0.5f+0.21f*std::sin(a*0.11f+b*0.07f)+0.17f*std::cos(a*0.037f-b*0.083f);
        auto* p=pixels.data()+(size_t(y)*w+x)*4;p[0]=value;p[1]=value*0.8f+0.05f;p[2]=value*0.6f+0.1f;p[3]=1;
    }
    return pixels;
}
static double error(const std::vector<float>& a,const std::vector<float>& b,uint32_t w,uint32_t h) {
    double sum=0;size_t count=0;
    for(uint32_t y=24;y<h-24;++y)for(uint32_t x=32;x<w-32;++x)for(unsigned c=0;c<3;++c) {
        const size_t i=(size_t(y)*w+x)*4+c;sum+=std::pow(double(a[i]-b[i]),2);++count;
    }
    return sum/std::max(size_t(1),count);
}
int main() {
    try {
        Fixture f;std::vector<float> output;const uint32_t w=128,h=96;auto a=picture(w,h);
        require(!f.frame(a,w,h,output),"First frame invented history");
        require(f.frame(a,w,h,output),"Second static frame did not generate");
        double maximum=0;for(size_t i=0;i<a.size();++i)maximum=std::max(maximum,std::abs(double(a[i]-output[i])));
        require(maximum<0.0002,"Static image changed during interpolation");
        for(const auto shift:std::array<std::array<float,2>,4>{{{16,0},{0,8},{12,8},{-12,0}}}) {
            f.fg.resetHistory();require(!f.frame(a,w,h,output),"Reset reused stale history");
            auto b=picture(w,h,shift[0],shift[1]),truth=picture(w,h,shift[0]*0.5f,shift[1]*0.5f),blend=a;
            for(size_t i=0;i<blend.size();++i)blend[i]=(a[i]+b[i])*0.5f;
            require(f.frame(b,w,h,output),"Motion frame failed to generate");
            const auto generatedError=error(output,truth,w,h),blendError=error(blend,truth,w,h);
            std::printf("Motion %.0f,%.0f: generated MSE %.8f; frame-blend MSE %.8f\n",shift[0],shift[1],generatedError,blendError);
            require(generatedError<blendError*0.7,"Motion interpolation did not beat simple frame blending");
        }
        f.fg.resetHistory();std::vector<float> objectA(size_t(w)*h*4),objectB;
        for(size_t i=0;i<objectA.size();i+=4) {objectA[i]=objectA[i+1]=objectA[i+2]=0.2f;objectA[i+3]=1;}
        objectB=objectA;
        for(unsigned y=32;y<64;++y)for(unsigned x=32;x<64;++x) {
            for(auto pair:std::array<std::pair<std::vector<float>*,unsigned>,2>{{{&objectA,x},{&objectB,x+16}}}) {
                auto* pixel=pair.first->data()+(size_t(y)*w+pair.second)*4;pixel[0]=0.95f;pixel[1]=pixel[2]=0.1f;
            }
        }
        f.frame(objectA,w,h,output);require(f.frame(objectB,w,h,output),"Moving-object generation failed");
        for(unsigned x=24;x<36;++x)require(output[(48*w+x)*4]<0.5f,"Moving object left a blended ghost trail");
        require(output[(48*w+64)*4]>0.8f && output[(48*w+64)*4+1]<0.2f,"Moving object became transparent");
        f.fg.resetHistory();auto dark=std::vector<float>(size_t(w)*h*4,0.05f),bright=dark;
        for(size_t i=0;i<bright.size();++i)bright[i]=0.95f;
        f.frame(dark,w,h,output);require(f.frame(bright,w,h,output) && f.fg.sceneCut(),"Scene cut was not detected");
        require(output==bright,"A scene cut mixed unrelated frames");
        auto resized=picture(160,101);require(!f.frame(resized,160,101,output),"Resolution change reused old history");
        require(f.frame(resized,160,101,output),"Odd-size static frame failed");
        f.realCopy();
        require(validationErrors==0,"Vulkan validation reported errors");
        std::puts("Interpolation: Vulkan/synchronization validation, static fidelity, bidirectional motion, opaque moving objects, cuts, reset, resize and real-frame copy pass.");
        return 0;
    }catch(const std::exception& e) {std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;}
}
