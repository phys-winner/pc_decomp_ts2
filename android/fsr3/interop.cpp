/* The optional Vulkan SDK imports the GLES buffers; no pixel readback or frame generation. */
#include "fsr3_api.h"
#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>
#include <array>
#include <vector>
#include <memory>
#include <cstring>
#include "prepare_spv.h"

namespace {
enum {Color,PackedDepth,Opaque,Output,Depth,Motion,Reactive,Flow,SceneChange,DilatedDepth,DilatedMotion,PreviousDepth,ImageCount};
struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format{};uint32_t width{},height{};};
struct State {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family{};
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    VkDescriptorSetLayout layout{};VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};
    VkDescriptorPool descriptors{};VkDescriptorSet descriptor{};VkSampler sampler{};
    std::array<Image,ImageCount> images{};const S2Fsr3Api *api{};void *sdk{};bool initialized{};
    ~State(){
        if(device)vkDeviceWaitIdle(device);
        if(sdk)api->destroy(sdk);
        if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(layout)vkDestroyDescriptorSetLayout(device,layout,nullptr);
        if(sampler)vkDestroySampler(device,sampler,nullptr);
        for(auto& i:images){if(i.view)vkDestroyImageView(device,i.view,nullptr);if(i.image)vkDestroyImage(device,i.image,nullptr);if(i.memory)vkFreeMemory(device,i.memory,nullptr);}
        if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);
        if(device)vkDestroyDevice(device,nullptr);if(instance)vkDestroyInstance(instance,nullptr);
    }
    uint32_t memoryType(uint32_t bits){
        VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
        for(uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return i;
        for(uint32_t i=0;i<p.memoryTypeCount;++i)if(bits&(1u<<i))return i;throw VkResult(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    void check(VkResult result){if(result!=VK_SUCCESS)throw result;}
    void image(unsigned slot,uint32_t w,uint32_t h,VkFormat format,AHardwareBuffer *buffer=nullptr){
        auto& image=images[slot];image.width=w;image.height=h;image.format=format;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=buffer?&external:nullptr;
        info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={w,h,1};info.mipLevels=info.arrayLayers=1;
        info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;check(vkCreateImage(device,&info,nullptr,&image.image));
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image.image;
        VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
        if(buffer){
            VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};properties.pNext=&formatProperties;
            auto get=reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(vkGetDeviceProcAddr(device,"vkGetAndroidHardwareBufferPropertiesANDROID"));
            if(!get)throw VkResult(VK_ERROR_EXTENSION_NOT_PRESENT);check(get(device,buffer,&properties));
            if(formatProperties.format!=format)throw VkResult(VK_ERROR_FORMAT_NOT_SUPPORTED);
            allocation.allocationSize=properties.allocationSize;allocation.memoryTypeIndex=memoryType(properties.memoryTypeBits);
            import.buffer=buffer;import.pNext=&dedicated;allocation.pNext=&import;
        }else{
            VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,image.image,&requirements);
            allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits);allocation.pNext=&dedicated;
        }
        check(vkAllocateMemory(device,&allocation,nullptr,&image.memory));check(vkBindImageMemory(device,image.image,image.memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(device,&view,nullptr,&image.view));
    }
    S2Fsr3Image image(unsigned slot)const{const auto& i=images[slot];return {i.image,i.format,i.width,i.height};}
    void barriers(bool acquire){
        std::vector<VkImageMemoryBarrier> barriers;
        for(unsigned n=0;n<ImageCount;++n){
            if(n>Output&&(initialized||!acquire))continue;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[n].image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.oldLayout=n<=Output?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask=acquire?0:VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask=acquire?VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT:0;
            b.srcQueueFamilyIndex=n<=Output?(acquire?VK_QUEUE_FAMILY_FOREIGN_EXT:family):VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex=n<=Output?(acquire?family:VK_QUEUE_FAMILY_FOREIGN_EXT):VK_QUEUE_FAMILY_IGNORED;
            barriers.push_back(b);
        }
        vkCmdPipelineBarrier(command,acquire?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            acquire?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,barriers.size(),barriers.data());
    }
    void computeBarrier(){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);}
};
}
extern "C" __attribute__((visibility("default"))) void *sims2_fsr3_create(AHardwareBuffer **buffers,uint32_t w,uint32_t h,uint32_t ow,uint32_t oh,uint32_t *error){
    if(!error)return nullptr;*error=uint32_t(VK_ERROR_INITIALIZATION_FAILED);
    std::unique_ptr<State> s(new(std::nothrow) State);if(!s)return nullptr;
    try{
        if(!buffers||!w||!h||ow<w||oh<h||ow>8192||oh>8192)throw VkResult(VK_ERROR_INITIALIZATION_FAILED);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="The Sims 2 FSR upscaler";application.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance.pApplicationInfo=&application;s->check(vkCreateInstance(&instance,nullptr,&s->instance));
        uint32_t count=0;s->check(vkEnumeratePhysicalDevices(s->instance,&count,nullptr));if(!count)throw VkResult(VK_ERROR_FEATURE_NOT_PRESENT);
        std::vector<VkPhysicalDevice> physical(count);s->check(vkEnumeratePhysicalDevices(s->instance,&count,physical.data()));s->physical=physical[0];
        vkGetPhysicalDeviceQueueFamilyProperties(s->physical,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(s->physical,&count,queues.data());
        s->family=UINT32_MAX;for(uint32_t n=0;n<count;++n)if(queues[n].queueFlags&VK_QUEUE_COMPUTE_BIT){s->family=n;break;}if(s->family==UINT32_MAX)throw VkResult(VK_ERROR_FEATURE_NOT_PRESENT);
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&features12;vkGetPhysicalDeviceFeatures2(s->physical,&features);
        if(!features12.vulkanMemoryModel||!features12.vulkanMemoryModelDeviceScope)throw VkResult(VK_ERROR_FEATURE_NOT_PRESENT);
        float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=s->family;queue.queueCount=1;queue.pQueuePriorities=&priority;
        const char *extensions[]={VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.pNext=&features12;device.pEnabledFeatures=&features.features;
        device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;device.enabledExtensionCount=2;device.ppEnabledExtensionNames=extensions;
        s->check(vkCreateDevice(s->physical,&device,nullptr,&s->device));vkGetDeviceQueue(s->device,s->family,0,&s->queue);
        for(unsigned n=0;n<4;++n){if(!buffers[n])throw VkResult(VK_ERROR_INITIALIZATION_FAILED);s->image(n,n==Output?ow:w,n==Output?oh:h,n==Output?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM,buffers[n]);}
        const VkFormat formats[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R8_UNORM,VK_FORMAT_R16G16_SINT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R32_UINT};
        for(unsigned n=Depth;n<ImageCount;++n)s->image(n,n==Flow?(w+7)/8:n==SceneChange?3:w,n==Flow?(h+7)/8:n==SceneChange?1:h,formats[n-Depth]);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=s->family;s->check(vkCreateCommandPool(s->device,&pool,nullptr,&s->pool));
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command.commandPool=s->pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;s->check(vkAllocateCommandBuffers(s->device,&command,&s->command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};s->check(vkCreateFence(s->device,&fence,nullptr,&s->fence));
        std::array<VkDescriptorSetLayoutBinding,8> bindings{};for(unsigned n=0;n<8;++n)bindings[n]={n,n<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=bindings.size();layout.pBindings=bindings.data();s->check(vkCreateDescriptorSetLayout(s->device,&layout,nullptr,&s->layout));
        VkPushConstantRange phase{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=1;pipelineLayout.pSetLayouts=&s->layout;
        pipelineLayout.pushConstantRangeCount=1;pipelineLayout.pPushConstantRanges=&phase;s->check(vkCreatePipelineLayout(s->device,&pipelineLayout,nullptr,&s->pipelineLayout));
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shaderInfo.codeSize=sizeof(s2Prepare);shaderInfo.pCode=s2Prepare;
        VkShaderModule shader{};s->check(vkCreateShaderModule(s->device,&shaderInfo,nullptr,&shader));
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};pipeline.layout=s->pipelineLayout;
        VkResult status=vkCreateComputePipelines(s->device,VK_NULL_HANDLE,1,&pipeline,nullptr,&s->pipeline);vkDestroyShaderModule(s->device,shader,nullptr);s->check(status);
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4}};
        VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptors.maxSets=1;descriptors.poolSizeCount=2;descriptors.pPoolSizes=sizes;s->check(vkCreateDescriptorPool(s->device,&descriptors,nullptr,&s->descriptors));
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set.descriptorPool=s->descriptors;set.descriptorSetCount=1;set.pSetLayouts=&s->layout;s->check(vkAllocateDescriptorSets(s->device,&set,&s->descriptor));
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;s->check(vkCreateSampler(s->device,&sampler,nullptr,&s->sampler));
        const unsigned inputs[]={PackedDepth,Flow,Color,Opaque,Depth,Motion,Reactive,Output};std::array<VkDescriptorImageInfo,8> imageInfo{};std::array<VkWriteDescriptorSet,8> writes{};
        for(unsigned n=0;n<writes.size();++n){imageInfo[n]={n<4?s->sampler:VK_NULL_HANDLE,s->images[inputs[n]].view,VK_IMAGE_LAYOUT_GENERAL};writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,s->descriptor,n,0,1,bindings[n].descriptorType,&imageInfo[n],nullptr,nullptr};}
        vkUpdateDescriptorSets(s->device,writes.size(),writes.data(),0,nullptr);
        s->api=sims2_fsr3_api();s->sdk=s->api->create(s->device,s->physical,vkGetDeviceProcAddr,w,h,ow,oh,0,error);if(!s->sdk)return nullptr;
        *error=0;return s.release();
    }catch(VkResult result){*error=uint32_t(result);return nullptr;}catch(...){*error=uint32_t(VK_ERROR_OUT_OF_HOST_MEMORY);return nullptr;}
}
extern "C" __attribute__((visibility("default"))) void sims2_fsr3_destroy(void *state){delete static_cast<State*>(state);}
extern "C" __attribute__((visibility("default"))) uint32_t sims2_fsr3_dispatch(void *state,float jitterX,float jitterY,float milliseconds,float nearPlane,float farPlane,float fov,int reset){
    auto *s=static_cast<State*>(state);if(!s||!s->sdk)return uint32_t(VK_ERROR_INITIALIZATION_FAILED);
    try{
        s->check(vkResetCommandBuffer(s->command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;s->check(vkBeginCommandBuffer(s->command,&begin));s->barriers(true);
        S2Fsr3Frame f{};f.color=s->image(Opaque);f.depth=s->image(Depth);f.motion=s->image(Motion);f.reactive=s->image(Reactive);f.output=s->image(Output);
        f.flow=s->image(Flow);f.sceneChange=s->image(SceneChange);f.dilatedDepth=s->image(DilatedDepth);f.dilatedMotion=s->image(DilatedMotion);f.previousDepth=s->image(PreviousDepth);
        f.jitterX=jitterX;f.jitterY=jitterY;f.milliseconds=milliseconds;f.nearPlane=nearPlane;f.farPlane=farPlane;f.verticalFov=fov;f.reset=reset||!s->initialized;
        uint32_t status=s->api->flow(s->sdk,s->command,&f);if(status)return status;s->computeBarrier();
        vkCmdBindPipeline(s->command,VK_PIPELINE_BIND_POINT_COMPUTE,s->pipeline);vkCmdBindDescriptorSets(s->command,VK_PIPELINE_BIND_POINT_COMPUTE,s->pipelineLayout,0,1,&s->descriptor,0,nullptr);
        uint32_t phase=0;vkCmdPushConstants(s->command,s->pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof phase,&phase);
        vkCmdDispatch(s->command,(f.color.width+7)/8,(f.color.height+7)/8,1);s->computeBarrier();
        status=s->api->upscale(s->sdk,s->command,&f);if(status)return status;s->computeBarrier();
        vkCmdBindPipeline(s->command,VK_PIPELINE_BIND_POINT_COMPUTE,s->pipeline);vkCmdBindDescriptorSets(s->command,VK_PIPELINE_BIND_POINT_COMPUTE,s->pipelineLayout,0,1,&s->descriptor,0,nullptr);
        phase=1;vkCmdPushConstants(s->command,s->pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof phase,&phase);
        vkCmdDispatch(s->command,(f.output.width+7)/8,(f.output.height+7)/8,1);s->barriers(false);s->check(vkEndCommandBuffer(s->command));
        s->check(vkResetFences(s->device,1,&s->fence));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&s->command;s->check(vkQueueSubmit(s->queue,1,&submit,s->fence));
        s->check(vkWaitForFences(s->device,1,&s->fence,VK_TRUE,1000000000ull));s->initialized=true;return 0;
    }catch(VkResult result){return uint32_t(result);}
}
