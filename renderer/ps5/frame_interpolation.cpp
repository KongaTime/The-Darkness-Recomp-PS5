#include "frame_interpolation.h"
#include "ps5_shaders.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace DarkRecomp::Ps5 {
namespace {
struct Image {
    VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format=VK_FORMAT_UNDEFINED;uint32_t width=0,height=0;
};
struct Params {uint32_t width=0,height=0,kind=0,radius=0;float x=0,y=0,phase=0.5f,pad=0;};
struct Access {VkPipelineStageFlags stage;VkAccessFlags access;};
Access access(VkImageLayout layout) {
    switch(layout) {
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_GENERAL:return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0};
    default:return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0};
    }
}
}
struct FrameInterpolation::Impl {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDescriptorPool pool{};VkSampler sampler{};
    std::array<VkDescriptorSetLayout,4> sets{};
    std::array<VkPipelineLayout,4> layouts{};std::array<VkPipeline,4> pipelines{};
    std::array<Image,2> history;
    std::array<std::array<Image,4>,2> luma,flow;
    Image generated,real;
    VkBuffer cutBuffer{};VkDeviceMemory cutMemory{};uint32_t* cutMapped=nullptr;
    VkQueryPool queries{};float timestampPeriod=0;uint64_t timestampMask=~uint64_t(0);
    bool timingPending=false,cut=false,valid=false;double gpuMs=0;unsigned previous=0;
    uint32_t width=0,height=0;VkFormat format=VK_FORMAT_UNDEFINED;
#define FG_PROC(name) PFN_vk##name name=nullptr;
    FG_PROC(CreateImage) FG_PROC(DestroyImage) FG_PROC(GetImageMemoryRequirements) FG_PROC(AllocateMemory)
    FG_PROC(FreeMemory) FG_PROC(BindImageMemory) FG_PROC(CreateImageView) FG_PROC(DestroyImageView)
    FG_PROC(CreateSampler) FG_PROC(DestroySampler) FG_PROC(CreateDescriptorSetLayout) FG_PROC(DestroyDescriptorSetLayout)
    FG_PROC(CreatePipelineLayout) FG_PROC(DestroyPipelineLayout) FG_PROC(CreateComputePipelines) FG_PROC(DestroyPipeline)
    FG_PROC(CreateShaderModule) FG_PROC(DestroyShaderModule) FG_PROC(CreateDescriptorPool) FG_PROC(DestroyDescriptorPool)
    FG_PROC(ResetDescriptorPool) FG_PROC(AllocateDescriptorSets) FG_PROC(UpdateDescriptorSets)
    FG_PROC(CmdPipelineBarrier) FG_PROC(CmdCopyImage) FG_PROC(CmdBindPipeline) FG_PROC(CmdBindDescriptorSets)
    FG_PROC(CmdPushConstants) FG_PROC(CmdDispatch) FG_PROC(DeviceWaitIdle)
    FG_PROC(CreateBuffer) FG_PROC(DestroyBuffer) FG_PROC(GetBufferMemoryRequirements) FG_PROC(BindBufferMemory)
    FG_PROC(MapMemory) FG_PROC(UnmapMemory) FG_PROC(GetPhysicalDeviceFormatProperties)
    FG_PROC(CreateQueryPool) FG_PROC(DestroyQueryPool) FG_PROC(GetQueryPoolResults) FG_PROC(CmdWriteTimestamp) FG_PROC(CmdResetQueryPool)
#undef FG_PROC
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<memoryProperties.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memoryProperties.memoryTypes[i].propertyFlags&flags)==flags)return i;
        return UINT32_MAX;
    }
    void release(Image& i) {
        if(i.view)DestroyImageView(device,i.view,nullptr);
        if(i.image)DestroyImage(device,i.image,nullptr);
        if(i.memory)FreeMemory(device,i.memory,nullptr);
        i={};
    }
    void releaseHistory() {
        for(auto& i:history)release(i);
        for(auto& bank:luma)for(auto& i:bank)release(i);
        for(auto& bank:flow)for(auto& i:bank)release(i);
        release(generated);valid=false;width=height=0;format=VK_FORMAT_UNDEFINED;
    }
    bool makeImage(Image& i,VkFormat fmt,uint32_t w,uint32_t h,VkImageUsageFlags usage,const char** error) {
        VkFormatProperties props{};GetPhysicalDeviceFormatProperties(physical,fmt,&props);
        VkFormatFeatureFlags needed=0;
        if(usage&VK_IMAGE_USAGE_SAMPLED_BIT)needed|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if(usage&VK_IMAGE_USAGE_STORAGE_BIT)needed|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if(usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)needed|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if((props.optimalTilingFeatures&needed)!=needed) {*error="frame generation image format is unsupported";return false;}
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType=VK_IMAGE_TYPE_2D;image.format=fmt;image.extent={w,h,1};image.mipLevels=1;image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;
        image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(CreateImage(device,&image,nullptr,&i.image)!=VK_SUCCESS) {*error="frame generation image creation failed";return false;}
        VkMemoryRequirements requirements{};GetImageMemoryRequirements(device,i.image,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;
        allocate.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(allocate.memoryTypeIndex==UINT32_MAX || AllocateMemory(device,&allocate,nullptr,&i.memory)!=VK_SUCCESS ||
           BindImageMemory(device,i.image,i.memory,0)!=VK_SUCCESS) {*error="frame generation image memory failed";return false;}
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=i.image;
        view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=fmt;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(CreateImageView(device,&view,nullptr,&i.view)!=VK_SUCCESS) {*error="frame generation image view failed";return false;}
        i.format=fmt;i.width=w;i.height=h;return true;
    }
    void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after) {
        const auto src=access(before),dst=access(after);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src.access;b.dstAccessMask=dst.access;
        b.oldLayout=before;b.newLayout=after;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        CmdPipelineBarrier(cmd,src.stage,dst.stage,0,0,nullptr,0,nullptr,1,&b);
    }
    void transition(VkCommandBuffer cmd,Image& i,VkImageLayout after) {
        if(i.layout!=after) {barrier(cmd,i.image,i.layout,after);i.layout=after;}
    }
    void copy(VkCommandBuffer cmd,VkImage src,VkImage dst,uint32_t w,uint32_t h) {
        VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={w,h,1};
        CmdCopyImage(cmd,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    bool pipeline(unsigned n,const uint32_t* code,size_t bytes,const std::vector<VkDescriptorType>& bindings,const char** error) {
        std::vector<VkDescriptorSetLayoutBinding> entries;
        for(uint32_t i=0;i<bindings.size();++i)entries.push_back({i,bindings[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount=uint32_t(entries.size());set.pBindings=entries.data();
        if(CreateDescriptorSetLayout(device,&set,nullptr,&sets[n])!=VK_SUCCESS) {*error="frame generation descriptor layout failed";return false;}
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount=1;layout.pSetLayouts=&sets[n];layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;
        if(CreatePipelineLayout(device,&layout,nullptr,&layouts[n])!=VK_SUCCESS) {*error="frame generation pipeline layout failed";return false;}
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=bytes;shader.pCode=code;
        VkShaderModule module{};
        if(CreateShaderModule(device,&shader,nullptr,&module)!=VK_SUCCESS) {*error="frame generation shader module failed";return false;}
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module=module;info.stage.pName="main";info.layout=layouts[n];
        const auto result=CreateComputePipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&pipelines[n]);DestroyShaderModule(device,module,nullptr);
        if(result!=VK_SUCCESS) {*error="frame generation compute pipeline failed";return false;}return true;
    }
    bool dispatch(VkCommandBuffer cmd,unsigned pipeline,const std::vector<Image*>& sampled,Image* output,
                  bool buffer,const Params& params,uint32_t groupsX,uint32_t groupsY,const char** error) {
        VkDescriptorSet set{};VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&sets[pipeline];
        if(AllocateDescriptorSets(device,&allocate,&set)!=VK_SUCCESS) {*error="frame generation descriptors exhausted";return false;}
        std::array<VkDescriptorImageInfo,6> images{};std::array<VkWriteDescriptorSet,6> writes{};unsigned count=0;
        for(auto* image:sampled) {
            images[count]={sampler,image->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[count]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[count].dstSet=set;writes[count].dstBinding=count;
            writes[count].descriptorCount=1;writes[count].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].pImageInfo=&images[count];++count;
        }
        if(output) {
            images[count]={VK_NULL_HANDLE,output->view,VK_IMAGE_LAYOUT_GENERAL};
            writes[count]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[count].dstSet=set;writes[count].dstBinding=count;
            writes[count].descriptorCount=1;writes[count].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[count].pImageInfo=&images[count];++count;
        }
        VkDescriptorBufferInfo bufferInfo{cutBuffer,0,4};
        if(buffer) {
            writes[count]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[count].dstSet=set;writes[count].dstBinding=count;
            writes[count].descriptorCount=1;writes[count].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[count].pBufferInfo=&bufferInfo;++count;
        }
        UpdateDescriptorSets(device,count,writes.data(),0,nullptr);
        CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[pipeline]);
        CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[pipeline],0,1,&set,0,nullptr);
        CmdPushConstants(cmd,layouts[pipeline],VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        CmdDispatch(cmd,groupsX,groupsY,1);return true;
    }
};

FrameInterpolation::~FrameInterpolation() {destroy();}
bool FrameInterpolation::create(VkInstance instance,VkPhysicalDevice physical,VkDevice device,PFN_vkGetInstanceProcAddr resolver,
                                uint32_t family,const char** error) {
    destroy();impl_=new Impl;auto& i=*impl_;i.instance=instance;i.physical=physical;i.device=device;
#define FG_LOAD(name) i.name=reinterpret_cast<PFN_vk##name>(resolver(instance,"vk" #name));if(!i.name) {*error="frame generation Vulkan entry point missing";destroy();return false;}
    FG_LOAD(CreateImage) FG_LOAD(DestroyImage) FG_LOAD(GetImageMemoryRequirements) FG_LOAD(AllocateMemory) FG_LOAD(FreeMemory)
    FG_LOAD(BindImageMemory) FG_LOAD(CreateImageView) FG_LOAD(DestroyImageView) FG_LOAD(CreateSampler) FG_LOAD(DestroySampler)
    FG_LOAD(CreateDescriptorSetLayout) FG_LOAD(DestroyDescriptorSetLayout) FG_LOAD(CreatePipelineLayout) FG_LOAD(DestroyPipelineLayout)
    FG_LOAD(CreateComputePipelines) FG_LOAD(DestroyPipeline) FG_LOAD(CreateShaderModule) FG_LOAD(DestroyShaderModule)
    FG_LOAD(CreateDescriptorPool) FG_LOAD(DestroyDescriptorPool) FG_LOAD(ResetDescriptorPool) FG_LOAD(AllocateDescriptorSets)
    FG_LOAD(UpdateDescriptorSets) FG_LOAD(CmdPipelineBarrier) FG_LOAD(CmdCopyImage) FG_LOAD(CmdBindPipeline)
    FG_LOAD(CmdBindDescriptorSets) FG_LOAD(CmdPushConstants) FG_LOAD(CmdDispatch) FG_LOAD(DeviceWaitIdle)
    FG_LOAD(CreateBuffer) FG_LOAD(DestroyBuffer) FG_LOAD(GetBufferMemoryRequirements) FG_LOAD(BindBufferMemory)
    FG_LOAD(MapMemory) FG_LOAD(UnmapMemory) FG_LOAD(GetPhysicalDeviceFormatProperties)
    FG_LOAD(CreateQueryPool) FG_LOAD(DestroyQueryPool) FG_LOAD(GetQueryPoolResults) FG_LOAD(CmdWriteTimestamp) FG_LOAD(CmdResetQueryPool)
#undef FG_LOAD
    auto memory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(resolver(instance,"vkGetPhysicalDeviceMemoryProperties"));
    auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(resolver(instance,"vkGetPhysicalDeviceProperties"));
    auto queues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(resolver(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    if(!memory || !properties || !queues) {*error="frame generation physical-device query missing";destroy();return false;}
    memory(physical,&i.memoryProperties);VkPhysicalDeviceProperties props{};properties(physical,&props);
    uint32_t count=0;queues(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);queues(physical,&count,families.data());
    if(family>=count || !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) {*error="frame generation needs a compute-capable graphics queue";destroy();return false;}
    if(family<count && families[family].timestampValidBits) {
        i.timestampPeriod=props.limits.timestampPeriod;const auto bits=families[family].timestampValidBits;
        i.timestampMask=bits==64?~uint64_t(0):(uint64_t(1)<<bits)-1;
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=2;
        if(i.CreateQueryPool(device,&query,nullptr,&i.queries)!=VK_SUCCESS)i.timestampPeriod=0;
    }
    auto fail=[&](const char* why) {*error=why;destroy();return false;};
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(i.CreateSampler(device,&sampler,nullptr,&i.sampler)!=VK_SUCCESS)return fail("frame generation sampler failed");
    const VkDescriptorType sampled=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,storage=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,buffer=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if(!i.pipeline(0,Shaders::kFgLuma,sizeof(Shaders::kFgLuma),{sampled,storage},error) ||
       !i.pipeline(1,Shaders::kFgFlow,sizeof(Shaders::kFgFlow),{sampled,sampled,sampled,storage},error) ||
       !i.pipeline(2,Shaders::kFgCut,sizeof(Shaders::kFgCut),{sampled,sampled,buffer},error) ||
       !i.pipeline(3,Shaders::kFgWarp,sizeof(Shaders::kFgWarp),{sampled,sampled,sampled,sampled,storage,buffer},error)) {destroy();return false;}
    VkDescriptorPoolSize poolSizes[]{{sampled,128},{storage,64},{buffer,16}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=64;pool.poolSizeCount=3;pool.pPoolSizes=poolSizes;
    if(i.CreateDescriptorPool(device,&pool,nullptr,&i.pool)!=VK_SUCCESS)return fail("frame generation descriptor pool failed");
    VkBufferCreateInfo cut{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};cut.size=4;cut.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(i.CreateBuffer(device,&cut,nullptr,&i.cutBuffer)!=VK_SUCCESS)return fail("frame generation cut buffer failed");
    VkMemoryRequirements required{};i.GetBufferMemoryRequirements(device,i.cutBuffer,&required);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=required.size;
    allocate.memoryTypeIndex=i.memoryType(required.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(allocate.memoryTypeIndex==UINT32_MAX || i.AllocateMemory(device,&allocate,nullptr,&i.cutMemory)!=VK_SUCCESS ||
       i.BindBufferMemory(device,i.cutBuffer,i.cutMemory,0)!=VK_SUCCESS ||
       i.MapMemory(device,i.cutMemory,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&i.cutMapped))!=VK_SUCCESS)return fail("frame generation cut memory failed");
    *i.cutMapped=0;return true;
}
void FrameInterpolation::destroy() {
    if(!impl_)return;auto& i=*impl_;
    if(i.DeviceWaitIdle)i.DeviceWaitIdle(i.device);
    i.releaseHistory();i.release(i.real);
    if(i.cutMapped)i.UnmapMemory(i.device,i.cutMemory);
    if(i.cutBuffer)i.DestroyBuffer(i.device,i.cutBuffer,nullptr);
    if(i.cutMemory)i.FreeMemory(i.device,i.cutMemory,nullptr);
    if(i.queries)i.DestroyQueryPool(i.device,i.queries,nullptr);
    if(i.pool)i.DestroyDescriptorPool(i.device,i.pool,nullptr);
    if(i.sampler)i.DestroySampler(i.device,i.sampler,nullptr);
    for(unsigned n=0;n<4;++n) {
        if(i.pipelines[n])i.DestroyPipeline(i.device,i.pipelines[n],nullptr);
        if(i.layouts[n])i.DestroyPipelineLayout(i.device,i.layouts[n],nullptr);
        if(i.sets[n])i.DestroyDescriptorSetLayout(i.device,i.sets[n],nullptr);
    }
    delete impl_;impl_=nullptr;
}
bool FrameInterpolation::realTarget(VkFormat format,uint32_t width,uint32_t height,const char** error) {
    if(!impl_) {*error="frame generation is not initialized";return false;}auto& i=*impl_;
    if(i.real.image && i.real.format==format && i.real.width==width && i.real.height==height)return true;
    i.DeviceWaitIdle(i.device);i.release(i.real);
    return i.makeImage(i.real,format,width,height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,error);
}
VkImageView FrameInterpolation::realView() const {return impl_?impl_->real.view:VK_NULL_HANDLE;}
void FrameInterpolation::resetHistory() {if(impl_)impl_->valid=false;}
bool FrameInterpolation::prepare(VkCommandBuffer cmd,VkImage scene,VkFormat format,uint32_t width,uint32_t height,bool& result,const char** error) {
    result=false;if(!impl_ || !scene || !width || !height || width>2560 || height>1440) {*error="frame generation scene size is unsupported";return false;}
    auto& i=*impl_;readMetrics();
    if(i.ResetDescriptorPool(i.device,i.pool,0)!=VK_SUCCESS) {*error="frame generation pool reset failed";return false;}
    if(i.width!=width || i.height!=height || i.format!=format) {
        i.releaseHistory();
        for(unsigned bank=0;bank<2;++bank) {
            if(!i.makeImage(i.history[bank],format,width,height,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,error))return false;
            uint32_t w=width,h=height;
            for(unsigned level=0;level<4;++level) {
                w=(w+1)/2;h=(h+1)/2;
                if(!i.makeImage(i.luma[bank][level],VK_FORMAT_R32_SFLOAT,w,h,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,error) ||
                   !i.makeImage(i.flow[bank][level],VK_FORMAT_R32G32B32A32_SFLOAT,(w+3)/4,(h+3)/4,
                                VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,error))return false;
            }
        }
        if(!i.makeImage(i.generated,VK_FORMAT_R32G32B32A32_SFLOAT,width,height,
                        VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,error))return false;
        i.width=width;i.height=height;i.format=format;
    }
    const unsigned current=i.valid?1-i.previous:0;
    i.barrier(cmd,scene,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    i.transition(cmd,i.history[current],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    i.copy(cmd,scene,i.history[current].image,width,height);
    i.barrier(cmd,scene,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    i.transition(cmd,i.history[current],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if(i.queries) {i.CmdResetQueryPool(cmd,i.queries,0,2);i.CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,i.queries,0);}
    for(unsigned level=0;level<4;++level) {
        auto& out=i.luma[current][level];i.transition(cmd,out,VK_IMAGE_LAYOUT_GENERAL);
        Params p;p.kind=level==0?0:1;
        if(!i.dispatch(cmd,0,{level==0?&i.history[current]:&i.luma[current][level-1]},&out,false,p,(out.width+7)/8,(out.height+7)/8,error))return false;
        i.transition(cmd,out,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if(i.valid) {
        for(unsigned direction=0;direction<2;++direction)for(int level=3;level>=0;--level) {
            const unsigned from=direction==0?i.previous:current,to=1-from;
            auto& out=i.flow[direction][level];i.transition(cmd,out,VK_IMAGE_LAYOUT_GENERAL);
            Params p;p.kind=level==3?1:level==0?2:0;p.radius=level==3?8:2;
            if(level<3) {p.x=float(i.luma[from][level].width)/i.luma[from][level+1].width;p.y=float(i.luma[from][level].height)/i.luma[from][level+1].height;}
            if(!i.dispatch(cmd,1,{&i.luma[from][level],&i.luma[to][level],level==3?&i.luma[from][level]:&i.flow[direction][level+1]},
                &out,false,p,(out.width+7)/8,(out.height+7)/8,error))return false;
            i.transition(cmd,out,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        Params p;
        if(!i.dispatch(cmd,2,{&i.luma[i.previous][3],&i.luma[current][3]},nullptr,true,p,1,1,error))return false;
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=i.cutBuffer;barrier.size=4;
        i.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
        i.transition(cmd,i.generated,VK_IMAGE_LAYOUT_GENERAL);p.width=width;p.height=height;
        p.x=float(width)/i.luma[current][0].width;p.y=float(height)/i.luma[current][0].height;
        if(!i.dispatch(cmd,3,{&i.history[i.previous],&i.history[current],&i.flow[0][0],&i.flow[1][0]},&i.generated,true,p,(width+7)/8,(height+7)/8,error))return false;
        i.transition(cmd,i.generated,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);result=true;
    }
    if(i.queries) {i.CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,i.queries,1);i.timingPending=true;}
    i.previous=current;i.valid=true;return true;
}
void FrameInterpolation::copyReal(VkCommandBuffer cmd,VkImage dst,VkImageLayout before,VkImageLayout after) {
    auto& i=*impl_;
    i.barrier(cmd,i.real.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    i.barrier(cmd,dst,before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    i.copy(cmd,i.real.image,dst,i.real.width,i.real.height);
    i.barrier(cmd,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,after);
}
VkImageView FrameInterpolation::generatedView() const {return impl_?impl_->generated.view:VK_NULL_HANDLE;}
VkImage FrameInterpolation::generatedImage() const {return impl_?impl_->generated.image:VK_NULL_HANDLE;}
uint32_t FrameInterpolation::generatedWidth() const {return impl_?impl_->generated.width:0;}
uint32_t FrameInterpolation::generatedHeight() const {return impl_?impl_->generated.height:0;}
void FrameInterpolation::readMetrics() {
    if(!impl_)return;auto& i=*impl_;
    if(i.cutMapped)i.cut=*i.cutMapped!=0;
    if(i.timingPending) {
        uint64_t stamps[2]{};
        if(i.GetQueryPoolResults(i.device,i.queries,0,2,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            i.gpuMs=double((stamps[1]-stamps[0])&i.timestampMask)*i.timestampPeriod/1e6;i.timingPending=false;
        }
    }
}
double FrameInterpolation::gpuMilliseconds() const {return impl_?impl_->gpuMs:0;}
bool FrameInterpolation::sceneCut() const {return impl_ && impl_->cut;}
}
