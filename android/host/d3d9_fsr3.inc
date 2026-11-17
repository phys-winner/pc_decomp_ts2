/* AHardwareBuffer keeps color/depth/opaque snapshots and the FSR output on the GPU. */
#include <android/hardware_buffer.h>
#include <android/api-level.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <time.h>
#include "../fsr3/fsr3_api.h"
#include "d3d9_thread.inc"

static struct {
    void *library,*state;
    const S2Fsr3Api *api;
    void *(*create)(AHardwareBuffer **,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *);
    void (*destroy)(void *);
    uint32_t (*dispatch)(void *,float,float,float,float,float,float,int);
    AHardwareBuffer *buffers[4];EGLImageKHR images[4];GLuint textures[4],copy_fbo;
    int width,height,ow,oh,ready,reset;unsigned frame;
    GLuint depth;int depth_width,depth_height;
    float projection[3],jitter_x,jitter_y;uint64_t previous_time;
} fsr3;
static _Atomic unsigned fsr3_status;
unsigned d3d_fsr3_status(void) { return atomic_load(&fsr3_status); }

static void fsr3_release(void)
{
    if(fsr3.state)fsr3.destroy(fsr3.state);fsr3.state=NULL;
    PFNEGLDESTROYIMAGEKHRPROC destroy=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    for(unsigned i=0;i<4;++i){
        if(fsr3.textures[i]){d3d_gl_forget_texture(fsr3.textures[i]);glDeleteTextures(1,fsr3.textures+i);fsr3.textures[i]=0;}
        if(fsr3.images[i]&&destroy)destroy(eglGetCurrentDisplay(),fsr3.images[i]);fsr3.images[i]=EGL_NO_IMAGE_KHR;
        if(fsr3.buffers[i])AHardwareBuffer_release(fsr3.buffers[i]);fsr3.buffers[i]=NULL;
    }
    fsr3.ready=0;fsr3.reset=1;fsr3.frame=0;fsr3.jitter_x=fsr3.jitter_y=0;fsr3.previous_time=0;
}
static int fsr3_failure(unsigned error,const char *stage)
{
    if(!error)error=1;atomic_store(&fsr3_status,error);
    void_log("[fsr3] %s failed status=%u; using FSR1",stage,error);fsr3_release();return 0;
}
static int fsr3_size(int width,int height,int ow,int oh)
{
    if(atomic_load(&fsr3_status))return 0;
    if(fsr3.state&&fsr3.width==width&&fsr3.height==height&&fsr3.ow==ow&&fsr3.oh==oh)return 1;
    if(android_get_device_api_level()<35)return fsr3_failure(7,"Android API 35 required");
    fsr3_release();fsr3.width=width;fsr3.height=height;fsr3.ow=ow;fsr3.oh=oh;
    if(width<64||height<64||ow<width||oh<height)return fsr3_failure(2,"upscale dimensions");
    if(!fsr3.library){
        fsr3.library=dlopen("libs2fsr3.so",RTLD_NOW|RTLD_LOCAL);
        if(!fsr3.library){void_log("[fsr3] load: %s",dlerror());return fsr3_failure(3,"library");}
        const S2Fsr3Api *(*get)(void)=(const S2Fsr3Api *(*)(void))dlsym(fsr3.library,"sims2_fsr3_api");
        fsr3.api=get?get():NULL;
        fsr3.create=(void *(*)(AHardwareBuffer **,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *))dlsym(fsr3.library,"sims2_fsr3_create");
        fsr3.destroy=(void (*)(void *))dlsym(fsr3.library,"sims2_fsr3_destroy");
        fsr3.dispatch=(uint32_t (*)(void *,float,float,float,float,float,float,int))dlsym(fsr3.library,"sims2_fsr3_dispatch");
        if(!fsr3.api||fsr3.api->version!=1||fsr3.api->bytes!=sizeof(S2Fsr3Api)||!fsr3.create||!fsr3.destroy||!fsr3.dispatch)return fsr3_failure(4,"ABI");
    }
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client=(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    PFNEGLCREATEIMAGEKHRPROC create=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if(!client||!create||!bind)return fsr3_failure(5,"EGL interop");
    for(unsigned i=0;i<4;++i){
        AHardwareBuffer_Desc desc={0};desc.width=i==3?ow:width;desc.height=i==3?oh:height;desc.layers=1;
        desc.format=i==3?AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
        int status=AHardwareBuffer_allocate(&desc,fsr3.buffers+i);if(status)return fsr3_failure((unsigned)status,"AHardwareBuffer");
        const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        fsr3.images[i]=create(eglGetCurrentDisplay(),EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,client(fsr3.buffers[i]),attrs);
        if(fsr3.images[i]==EGL_NO_IMAGE_KHR)return fsr3_failure((unsigned)eglGetError(),"EGL image");
        glGenTextures(1,fsr3.textures+i);glActiveTexture(GL_TEXTURE31);glBindTexture(GL_TEXTURE_2D,fsr3.textures[i]);bind(GL_TEXTURE_2D,fsr3.images[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    unsigned error=0;fsr3.state=fsr3.create(fsr3.buffers,width,height,ow,oh,&error);
    if(!fsr3.state)return fsr3_failure(error,"Vulkan context");
    fsr3.reset=1;void_log("[fsr3] ready %dx%d -> %dx%d, optical flow, no frame generation",width,height,ow,oh);return 1;
}
static GLuint surface_image(d3d_surface *s) { return s->container?s->container->gl:s->gl; }
static void fsr3_copy(GLuint source,GLuint target,int width,int height)
{
    if(!fsr3.copy_fbo)glGenFramebuffers(1,&fsr3.copy_fbo);
    if(!framebuffer)glGenFramebuffers(1,&framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fsr3.copy_fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    glDisable(GL_SCISSOR_TEST);glBlitFramebuffer(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
}
int d3d_filter_scene(d3d_device *d)
{
    if(s2_render_upscaler()!=3||atomic_load(&fsr3_status))return 0;
    if(!d->ui_draw&&d->ds&&d->st.rs[7]&&d->rt[0]&&d->rt[0]->width==d->pp[0]&&d->rt[0]->height==d->pp[1]&&d->projection[0]>0){
        fsr3.depth=surface_image(d->ds);fsr3.depth_width=d3d_scaled_size(d->ds->width,d3d_scale_of(d->ds));
        fsr3.depth_height=d3d_scaled_size(d->ds->height,d3d_scale_of(d->ds));memcpy(fsr3.projection,d->projection,sizeof fsr3.projection);
    }
    if(!d->ui_draw||fsr3.ready||!fsr3.depth||d->rt[0]!=d->backbuffer)return 0;
    int width=d3d_scaled_size(d->backbuffer->width,d3d_scale_of(d->backbuffer));
    int height=d3d_scaled_size(d->backbuffer->height,d3d_scale_of(d->backbuffer)),ow,oh;s2_physical_size(&ow,&oh);
    float fit=fminf((float)ow/d->backbuffer->width,(float)oh/d->backbuffer->height);
    int fitted_w=(int)(d->backbuffer->width*fit),fitted_h=(int)(d->backbuffer->height*fit);
    if(ow-fitted_w>2||oh-fitted_h>2){ow=fitted_w;oh=fitted_h;}
    if(fsr3.depth_width!=width||fsr3.depth_height!=height)return 0;
    if(!fsr3_size(width,height,ow,oh))return 1;
    fsr3_copy(surface_image(d->backbuffer),fsr3.textures[2],width,height);
    GLuint p=program(4);if(!p)return fsr3_failure(6,"depth pack")+1;
    if(!filter_vao)glGenVertexArrays(1,&filter_vao);if(!sampler){glGenSamplers(1,&sampler);glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,GL_NEAREST);}
    glBindVertexArray(filter_vao);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,fsr3.textures[1],0);
    glActiveTexture(GL_TEXTURE31);glBindTexture(GL_TEXTURE_2D,fsr3.depth);glBindSampler(31,sampler);
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    GLboolean dither=glIsEnabled(GL_DITHER);glDisable(GL_DITHER);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glViewport(0,0,width,height);
    d3d_gl_use_program(p);glUniform1i(glGetUniformLocation(p,"image"),31);glUniform2f(glGetUniformLocation(p,"target"),width,height);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    if(dither)glEnable(GL_DITHER);fsr3.ready=1;return 1;
}
void d3d_filter_jitter(d3d_device *d,float *x,float *y)
{
    if(s2_render_upscaler()!=3||!fsr3.state||atomic_load(&fsr3_status)||d->ui_draw||!d->ds||!d->st.rs[7]||!d->rt[0]||
        d->rt[0]->width!=d->pp[0]||d->rt[0]->height!=d->pp[1])return;
    *x+=2*fsr3.jitter_x/fsr3.width;*y+=2*fsr3.jitter_y/fsr3.height;
}
static GLuint fsr3_present(GLuint input,int width,int height,int ow,int oh)
{
    GLuint result=0;
    if(fsr3.ready&&fsr3.state&&fsr3.width==width&&fsr3.height==height&&fsr3.ow==ow&&fsr3.oh==oh){
        fsr3_copy(input,fsr3.textures[0],width,height);
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+now.tv_nsec;
        float ms=fsr3.previous_time?(float)(ns-fsr3.previous_time)/1000000.f:16.6667f;
        if(ms>250)fsr3.reset=1;if(ms>1000)ms=1000;if(ms<.1f)ms=.1f;
        d3d_rt_wait_begin();glFinish();
        unsigned status=fsr3.dispatch(fsr3.state,fsr3.jitter_x,fsr3.jitter_y,ms,fsr3.projection[0],fsr3.projection[1],fsr3.projection[2],fsr3.reset);
        d3d_rt_wait_end();
        if(status)fsr3_failure(status,"dispatch");
        else{
            if(!fsr3.frame)void_log("[fsr3] dispatch PASS near=%g far=%g fov=%g",fsr3.projection[0],fsr3.projection[1],fsr3.projection[2]);
            result=fsr3.textures[3];fsr3.previous_time=ns;fsr3.reset=0;
            fsr3.api->jitter(++fsr3.frame,width,ow,&fsr3.jitter_x,&fsr3.jitter_y);
        }
    }else fsr3.reset=1;
    fsr3.ready=0;fsr3.depth=0;return result;
}
