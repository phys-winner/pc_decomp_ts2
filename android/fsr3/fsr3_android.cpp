#include "fsr3_api.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <cmath>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include "optical_flow_wave16.h"

namespace {
struct State {
    VkDeviceContext vk;
    std::vector<uint8_t> scratch;
    FfxFsr3UpscalerContext upscaler{};
    FfxOpticalflowContext optical{};
    bool upscalerCreated=false, opticalCreated=false;
    uint32_t width, height, outputWidth, outputHeight;
};
static std::atomic<FfxGetDeviceCapabilitiesFunc> nativeCapabilities;
static std::atomic<FfxGetPermutationBlobByIndexFunc> nativeBlob;
static std::atomic<PFN_vkGetDeviceProcAddr> nativeResolve;
static PFN_vkVoidFunction resolveDevice(VkDevice device,const char *name){
    auto resolve=nativeResolve.load(std::memory_order_relaxed);auto result=resolve(device,name);
    size_t length=std::strlen(name);
    if(!result&&length>3&&length<120&&!std::strcmp(name+length-3,"KHR")){
        char core[120];std::memcpy(core,name,length-3);core[length-3]=0;result=resolve(device,core);
    }
    return result;
}
static FfxErrorCode shaderBlob(FfxEffect effect,FfxPass pass,FfxBindStage stage,uint32_t options,FfxShaderBlob *blob){
    FfxErrorCode status=nativeBlob.load(std::memory_order_relaxed)(effect,pass,stage,options,blob);
    if(status==FFX_OK&&effect==FFX_EFFECT_OPTICALFLOW&&pass==FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5){
        // The SDK combines two Wave32 groups; Mali requires four Wave16 groups.
        FfxShaderBlob replacement={reinterpret_cast<const uint8_t*>(fpOpticalFlowWave16),sizeof(fpOpticalFlowWave16),
            blob->cbvCount,blob->srvTextureCount,blob->uavTextureCount,blob->srvBufferCount,blob->uavBufferCount,blob->samplerCount,blob->rtAccelStructCount,
            blob->boundConstantBufferNames,blob->boundConstantBuffers,blob->boundConstantBufferCounts,blob->boundConstantBufferSpaces,
            blob->boundSRVTextureNames,blob->boundSRVTextures,blob->boundSRVTextureCounts,blob->boundSRVTextureSpaces,
            blob->boundUAVTextureNames,blob->boundUAVTextures,blob->boundUAVTextureCounts,blob->boundUAVTextureSpaces,
            blob->boundSRVBufferNames,blob->boundSRVBuffers,blob->boundSRVBufferCounts,blob->boundSRVBufferSpaces,
            blob->boundUAVBufferNames,blob->boundUAVBuffers,blob->boundUAVBufferCounts,blob->boundUAVBufferSpaces,
            blob->boundSamplerNames,blob->boundSamplers,blob->boundSamplerCounts,blob->boundSamplerSpaces,
            blob->boundRTAccelerationStructureNames,blob->boundRTAccelerationStructures,blob->boundRTAccelerationStructureCounts,blob->boundRTAccelerationStructureSpaces};
        new(blob) FfxShaderBlob(replacement);
    }
    return status;
}
static FfxErrorCode capabilities(FfxInterface *backend,FfxDeviceCapabilities *caps) {
    FfxErrorCode status=nativeCapabilities.load(std::memory_order_relaxed)(backend,caps);
    // DXVK does not enable shaderFloat16; choose the SDK's genuine FP32 permutations.
    if(status==FFX_OK)caps->fp16Supported=false;
    return status;
}
static void destroy(void *handle) {
    auto *state=static_cast<State*>(handle);
    if(!state)return;
    if(state->opticalCreated)ffxOpticalflowContextDestroy(&state->optical);
    if(state->upscalerCreated)ffxFsr3UpscalerContextDestroy(&state->upscaler);
    delete state;
}
static void *create(VkDevice device,VkPhysicalDevice physical,PFN_vkGetDeviceProcAddr resolve,
                    uint32_t width,uint32_t height,uint32_t outWidth,uint32_t outHeight,int inverted,uint32_t *error) {
    if(!error)return nullptr;
    *error=uint32_t(FFX_ERROR_INVALID_ARGUMENT);
    if(!device||!physical||!resolve||width<64||height<64||outWidth<width||outHeight<height||outWidth>16384||outHeight>16384)
        return nullptr;
    std::unique_ptr<State,decltype(&destroy)> state(nullptr,destroy);
    try {
        state.reset(new State);
        nativeResolve.store(resolve,std::memory_order_relaxed);state->vk={device,physical,resolveDevice};
        state->width=width;state->height=height;state->outputWidth=outWidth;state->outputHeight=outHeight;
        state->scratch.resize(ffxGetScratchMemorySizeVK(physical,2));
        FfxInterface backend{};
        *error=ffxGetInterfaceVK(&backend,ffxGetDeviceVK(&state->vk),state->scratch.data(),state->scratch.size(),2);
        if(*error)return nullptr;
        nativeCapabilities.store(backend.fpGetDeviceCapabilities,std::memory_order_relaxed);backend.fpGetDeviceCapabilities=capabilities;
        nativeBlob.store(backend.fpGetPermutationBlobByIndex,std::memory_order_relaxed);backend.fpGetPermutationBlobByIndex=shaderBlob;
        FfxFsr3UpscalerContextDescription desc{};
        desc.backendInterface=backend;desc.maxRenderSize={width,height};desc.maxUpscaleSize={outWidth,outHeight};
        desc.flags=FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE|FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
        if(inverted)desc.flags|=FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
        *error=ffxFsr3UpscalerContextCreate(&state->upscaler,&desc);
        if(*error)return nullptr;
        state->upscalerCreated=true;
        FfxOpticalflowContextDescription optical{};optical.backendInterface=backend;optical.resolution={width,height};
        *error=ffxOpticalflowContextCreate(&state->optical,&optical);
        if(*error)return nullptr;
        state->opticalCreated=true;
        return state.release();
    } catch(const std::bad_alloc&) {*error=uint32_t(FFX_ERROR_OUT_OF_MEMORY);return nullptr;}
}
static FfxResource resource(const S2Fsr3Image& image) {
    VkImageCreateInfo description{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    description.imageType=VK_IMAGE_TYPE_2D;description.format=image.format;
    description.extent={image.width,image.height,1};description.mipLevels=description.arrayLayers=1;
    description.samples=VK_SAMPLE_COUNT_1_BIT;
    description.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    auto ffxDescription=ffxGetImageResourceDescriptionVK(image.image,description);
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image),ffxDescription,nullptr,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}
static bool valid(const S2Fsr3Image& image,uint32_t w,uint32_t h,VkFormat format) {
    return image.image&&image.width==w&&image.height==h&&(format==VK_FORMAT_UNDEFINED||image.format==format);
}
static uint32_t flow(void *handle,VkCommandBuffer command,const S2Fsr3Frame *frame) {
    auto *state=static_cast<State*>(handle);
    if(!state||!command||!frame||!valid(frame->color,state->width,state->height,VK_FORMAT_UNDEFINED)
       ||!valid(frame->flow,(state->width+7)/8,(state->height+7)/8,VK_FORMAT_R16G16_SINT)
       ||!valid(frame->sceneChange,3,1,VK_FORMAT_R32_UINT))return uint32_t(FFX_ERROR_INVALID_ARGUMENT);
    FfxOpticalflowDispatchDescription desc{};desc.commandList=ffxGetCommandListVK(command);
    desc.color=resource(frame->color);desc.opticalFlowVector=resource(frame->flow);desc.opticalFlowSCD=resource(frame->sceneChange);
    desc.reset=frame->reset!=0;desc.backbufferTransferFunction=0;desc.minMaxLuminance={0,1};
    return ffxOpticalflowContextDispatch(&state->optical,&desc);
}
static uint32_t upscale(void *handle,VkCommandBuffer command,const S2Fsr3Frame *frame) {
    auto *state=static_cast<State*>(handle);
    if(!state||!command||!frame||!valid(frame->color,state->width,state->height,VK_FORMAT_UNDEFINED)
       ||!valid(frame->depth,state->width,state->height,VK_FORMAT_R32_SFLOAT)
       ||!valid(frame->motion,state->width,state->height,VK_FORMAT_R16G16_SFLOAT)
       ||!valid(frame->reactive,state->width,state->height,VK_FORMAT_R8_UNORM)
       ||!valid(frame->output,state->outputWidth,state->outputHeight,VK_FORMAT_R16G16B16A16_SFLOAT)
       ||!valid(frame->dilatedDepth,state->width,state->height,VK_FORMAT_R32_SFLOAT)
       ||!valid(frame->dilatedMotion,state->width,state->height,VK_FORMAT_R16G16_SFLOAT)
       ||!valid(frame->previousDepth,state->width,state->height,VK_FORMAT_R32_UINT)
       ||!std::isfinite(frame->jitterX)||!std::isfinite(frame->jitterY)||!std::isfinite(frame->milliseconds)
       ||frame->milliseconds<=0||frame->milliseconds>1000||!std::isfinite(frame->nearPlane)||frame->nearPlane<=0
       ||!std::isfinite(frame->farPlane)||frame->farPlane<=frame->nearPlane
       ||!std::isfinite(frame->verticalFov)||frame->verticalFov<=0||frame->verticalFov>=3.141592654f)
        return uint32_t(FFX_ERROR_INVALID_ARGUMENT);
    FfxFsr3UpscalerDispatchDescription desc{};desc.commandList=ffxGetCommandListVK(command);
    desc.color=resource(frame->color);desc.depth=resource(frame->depth);desc.motionVectors=resource(frame->motion);
    desc.reactive=resource(frame->reactive);desc.output=resource(frame->output);
    desc.dilatedDepth=resource(frame->dilatedDepth);desc.dilatedMotionVectors=resource(frame->dilatedMotion);
    desc.reconstructedPrevNearestDepth=resource(frame->previousDepth);
    desc.renderSize={state->width,state->height};desc.upscaleSize={state->outputWidth,state->outputHeight};
    desc.jitterOffset={frame->jitterX,frame->jitterY};desc.motionVectorScale={1,1};
    desc.frameTimeDelta=frame->milliseconds;desc.preExposure=1;desc.reset=frame->reset!=0;
    desc.cameraNear=frame->nearPlane;desc.cameraFar=frame->farPlane;desc.cameraFovAngleVertical=frame->verticalFov;
    desc.viewSpaceToMetersFactor=1;desc.enableSharpening=true;desc.sharpness=.2f;
    return ffxFsr3UpscalerContextDispatch(&state->upscaler,&desc);
}
static void jitter(uint32_t index,uint32_t width,uint32_t outputWidth,float *x,float *y) {
    if(!x||!y||!width||outputWidth<width)return;
    int32_t phases=ffxFsr3UpscalerGetJitterPhaseCount(width,outputWidth);
    ffxFsr3UpscalerGetJitterOffset(x,y,index%uint32_t(phases),phases);
}
}
extern "C" __attribute__((visibility("default"))) const S2Fsr3Api *sims2_fsr3_api() {
    static const S2Fsr3Api api={1,sizeof(S2Fsr3Api),create,destroy,flow,upscale,jitter};return &api;
}
