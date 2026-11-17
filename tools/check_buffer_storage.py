"""Exercise production persistent stores with GL spies and optional real GLES pixels."""
import argparse
import subprocess

from audio_probe import CLANG, compile_probe
import build_android as B
from check_virtual_query import function


def production():
    source = (B.ROOT / 'android/host/d3d9_resource.c').read_text(encoding='utf-8')
    helpers = source[source.index('static _Atomic int buffer_subdata'):source.index('static void run_buffer_destroy(')]
    record = source[source.index('typedef struct { d3d_buffer *b; uint32_t lo, length, whole, unsynchronized; }'):
                    source.index('static void run_buffer_upload(')]
    return helpers + record + '\n'.join(function(source, signature) for signature in (
        'static void run_buffer_create(', 'static void run_buffer_upload(', 'static void run_buffer_destroy('))


HOST = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define memory_order_relaxed 0
#define atomic_load_explicit(p,order) __c11_atomic_load(p,order)
#define atomic_store_explicit(p,value,order) __c11_atomic_store(p,value,order)
typedef unsigned GLuint,GLenum,GLbitfield;
typedef void *GLsync;
typedef struct {unsigned size,usage,gl;struct d3d_buffer_storage *storage;} d3d_buffer;
#define GL_RENDERER 1u
#define GL_EXTENSIONS 2u
#define GL_COPY_WRITE_BUFFER 3u
#define GL_DYNAMIC_DRAW 4u
#define GL_STATIC_DRAW 5u
#define GL_STREAM_DRAW 6u
#define GL_MAP_WRITE_BIT 8u
#define GL_MAP_PERSISTENT_BIT_EXT 16u
#define GL_MAP_COHERENT_BIT_EXT 32u
#define GL_DYNAMIC_STORAGE_BIT_EXT 64u
#define GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT 256u
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 512u
#define GL_ELEMENT_ARRAY_BARRIER_BIT 1024u
#define GL_MAP_UNSYNCHRONIZED_BIT 128u
#define GL_SYNC_GPU_COMMANDS_COMPLETE 1u
#define GL_ALREADY_SIGNALED 2u
#define GL_CONDITION_SATISFIED 3u
#define GL_WAIT_FAILED 4u
#define D3DUSAGE_DYNAMIC 0x200u
typedef void (*PFNGLBUFFERSTORAGEEXTPROC)(GLenum,intptr_t,const void*,GLbitfield);
static uint64_t stat_buffers,stat_buffer_bytes;
static const char *renderer="PowerVR D-Series",*extensions="GL_EXT_buffer_storage";
static unsigned bound,names,stores,maps,subdata,waits,deleted,fail_map,missing_proc,fail_fence,barriers,ready=1;
static unsigned char bytes[256][128],dead[256],immutable[256];
static const unsigned char *glGetString(GLenum name){return (const unsigned char *)(name==GL_RENDERER?renderer:extensions);}
static void glGenBuffers(unsigned n,GLuint *name){assert(n==1&&names<255);*name=++names;}
static void glBindBuffer(GLenum target,GLuint name){assert(target==GL_COPY_WRITE_BUFFER&&!dead[name]);bound=name;}
static void storage(GLenum target,intptr_t size,const void *data,GLbitfield flags){
 assert(target==GL_COPY_WRITE_BUFFER&&size==128&&!data&&flags==120u);immutable[bound]=1;++stores;
}
static PFNGLBUFFERSTORAGEEXTPROC eglGetProcAddress(const char *name){assert(!strcmp(name,"glBufferStorageEXT"));return missing_proc?NULL:storage;}
static void *glMapBufferRange(GLenum target,uint32_t lo,uint32_t length,GLbitfield flags){
 assert(target==GL_COPY_WRITE_BUFFER&&lo+length<=128);++maps;
 if(fail_map){--fail_map;return NULL;}
 assert(flags==(immutable[bound]?56u:136u));return bytes[bound]+lo;
}
static int glUnmapBuffer(GLenum target){assert(target==GL_COPY_WRITE_BUFFER);return 1;}
static void glBufferData(GLenum target,unsigned size,const void *data,GLenum usage){
 assert(target==GL_COPY_WRITE_BUFFER&&size==128&&!immutable[bound]);
 assert(usage==GL_DYNAMIC_DRAW||usage==GL_STATIC_DRAW||usage==GL_STREAM_DRAW);
 if(data)memcpy(bytes[bound],data,size);
}
static void glBufferSubData(GLenum target,unsigned lo,unsigned length,const void *data){
 assert(target==GL_COPY_WRITE_BUFFER&&lo+length<=128);++subdata;memcpy(bytes[bound]+lo,data,length);
}
static void d3d_gl_forget_buffer(GLuint name){assert(name&&!dead[name]);}
static void glDeleteBuffers(unsigned n,const GLuint *name){assert(n==1&&*name&&!dead[*name]);dead[*name]=1;++deleted;}
static GLsync glFenceSync(GLenum condition,unsigned flags){assert(condition==1&&!flags);return fail_fence?NULL:(void *)1;}
static void glDeleteSync(GLsync fence){assert(fence);}
static GLenum glClientWaitSync(GLsync fence,unsigned flags,uint64_t timeout){assert(fence&&!flags&&!timeout);++waits;return ready?GL_CONDITION_SATISFIED:0;}
static GLenum glGetError(void){return 0;}
static void glMemoryBarrier(GLbitfield flags){assert(flags==1792u);++barriers;}
static void void_log(const char *format,...){(void)format;}
'''

CHECKS = r'''
static d3d_buffer *make(unsigned dynamic){
 d3d_buffer *b=calloc(1,sizeof *b);assert(b);b->size=128;b->usage=dynamic?D3DUSAGE_DYNAMIC:0;
 run_buffer_create(&b);return b;
}
static void upload(d3d_buffer *b,unsigned whole,unsigned unsynchronized,unsigned lo,unsigned length,unsigned byte){
 struct {buffer_upload_args a;unsigned char data[128];} r={{b,lo,length,whole,unsynchronized},{0}};
 memset(r.data,(int)byte,length);run_buffer_upload(&r);
}
static void reset(void){
 assert(!stat_buffers&&!stat_buffer_bytes);names=stores=maps=subdata=waits=deleted=fail_map=missing_proc=fail_fence=barriers=0;
 memset(dead,0,sizeof dead);memset(immutable,0,sizeof immutable);
 buffer_persistent=buffer_subdata=-1;buffer_storage=NULL;ready=1;
 renderer="PowerVR D-Series";extensions="GL_EXT_buffer_storage";
}
int main(void){
 reset();d3d_buffer *b=make(1);assert(b->storage&&stores==1&&maps==1);
 upload(b,1,0,0,128,0x11);unsigned old=b->gl;
 upload(b,0,1,17,23,0x22);assert(stores==2&&maps==2&&!subdata&&barriers==2);
 for(unsigned i=0;i<128;++i)assert(bytes[old][i]==(i>=17&&i<40?0x22:0x11));
 upload(b,1,0,0,128,0x33);assert(b->gl!=old&&bytes[old][17]==0x22);
 upload(b,1,0,0,128,0x44);assert(stores==3&&waits==1&&stat_buffers==3);
 upload(b,0,0,64,7,0x55);assert(subdata==1&&bytes[b->gl][64]==0x55&&bytes[b->gl][63]==0x44);
 ready=0;old=b->gl;upload(b,1,0,0,128,0x66);
 assert(deleted==1&&stat_buffers==3&&bytes[old][64]==0x55&&bytes[b->gl][0]==0x66);
 fail_fence=1;
 for(unsigned i=0;i<5;++i)upload(b,1,0,0,128,0x77);
 assert(stat_buffers==3);run_buffer_destroy(&b);assert(!stat_buffers&&!stat_buffer_bytes);
 reset();fail_map=1;b=make(1);assert(!b->storage&&deleted==1&&stat_buffers==1);
 upload(b,1,0,0,128,0x88);assert(bytes[b->gl][0]==0x88);run_buffer_destroy(&b);
 reset();b=make(1);fail_map=1;upload(b,1,0,0,128,0x99);
 assert(!b->storage&&stat_buffers==1&&bytes[b->gl][127]==0x99);run_buffer_destroy(&b);
 reset();b=make(0);assert(!b->storage&&!stores);run_buffer_destroy(&b);
 reset();renderer="Mali-G715";b=make(1);assert(!b->storage&&!stores&&buffer_persistent==0);run_buffer_destroy(&b);
 reset();extensions="";b=make(1);assert(!b->storage&&!stores&&buffer_subdata==1);run_buffer_destroy(&b);
 reset();missing_proc=1;b=make(1);assert(!b->storage&&!stores);run_buffer_destroy(&b);
 puts("PASS production persistent buffers: exact NOOVERWRITE bytes, three-store reuse, no blocking wait, busy/failed fences retired, synchronized partial writes, map-failure snapshots, destruction/accounting and capability fallbacks");
 return 0;
}
'''


GPU = r'''
#include "d3d9_internal.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdarg.h>
static uint64_t stat_buffers,stat_buffer_bytes;
void void_log(const char *format,...){va_list a;va_start(a,format);vprintf(format,a);puts("");va_end(a);}
void d3d_gl_forget_buffer(GLuint name){(void)name;}
'''

GPU_CHECKS = r'''
static GLuint shader(GLenum type,const char *source){
 GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;
}
static void upload(d3d_buffer *b,unsigned whole,unsigned nooverwrite,unsigned lo,unsigned n,const void *p){
 unsigned length=whole?b->size:n;buffer_upload_args *a=calloc(1,sizeof *a+length);assert(a);
 *a=(buffer_upload_args){b,lo,length,whole,nooverwrite};memcpy(a+1,p,n);run_buffer_upload(a);free(a);
}
static void draw(d3d_buffer *b,unsigned first){
 glBindBuffer(GL_ARRAY_BUFFER,b->gl);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,20,0);
 glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,20,(const void *)8);glDrawArrays(GL_POINTS,(GLint)first,1);
}
static void pixel(unsigned x,unsigned y,unsigned r,unsigned g,unsigned b){
 unsigned char p[4];glReadPixels((GLint)x,(GLint)y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
 if(p[0]!=r||p[1]!=g||p[2]!=b){fprintf(stderr,"pixel %u,%u = %u,%u,%u expected %u,%u,%u\n",x,y,p[0],p[1],p[2],r,g,b);abort();}
}
int main(void){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);assert(eglInitialize(d,NULL,NULL));
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
 EGLConfig config;EGLint n;assert(eglChooseConfig(d,attrs,&config,1,&n)&&n);
 EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
 EGLContext c=eglCreateContext(d,config,EGL_NO_CONTEXT,ctx);assert(c!=EGL_NO_CONTEXT);
 EGLint surface[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};EGLSurface s=eglCreatePbufferSurface(d,config,surface);
 assert(s!=EGL_NO_SURFACE&&eglMakeCurrent(d,s,s,c));
 GLuint p=glCreateProgram();glAttachShader(p,shader(GL_VERTEX_SHADER,"#version 300 es\nlayout(location=0) in vec2 a;layout(location=1) in vec3 b;out vec3 color;void main(){gl_Position=vec4(a,0.,1.);gl_PointSize=3.;color=b;}"));
 glAttachShader(p,shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;in vec3 color;out vec4 o;void main(){o=vec4(color,1.);}"));
 glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);
 GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glViewport(0,0,32,32);
 d3d_buffer *b=calloc(1,sizeof *b);assert(b);b->size=65536;b->usage=D3DUSAGE_DYNAMIC;run_buffer_create(&b);assert(b->storage);
 {
  float red[]={-.5f,0,1,0,0,.5f,0,0,1,0},yellow[]={-.5f,.5f,1,1,0},blue[]={.5f,0,0,0,1};
  glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  upload(b,1,0,0,40,red);draw(b,0);
  unsigned char bulk[20000]={0};memcpy(bulk,yellow,20);
  upload(b,0,0,0,sizeof bulk,bulk);draw(b,0);
  upload(b,0,1,20,20,blue);draw(b,1);
  pixel(8,16,255,0,0);pixel(8,24,255,255,0);pixel(24,16,0,0,255);
 }
 /* More renames than stores, with no readback between the queued draws. */
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 for(unsigned i=0;i<64;++i){
  float v[]={-.875f+(float)(i%8)*.25f,-.875f+(float)(i/8)*.25f,i%3==0,i%3==1,i%3==2,0,0,0,0,0};
  upload(b,1,0,0,40,v);draw(b,0);
 }
 for(unsigned i=0;i<64;++i)pixel(2+(i%8)*4,2+(i/8)*4,i%3==0?255:0,i%3==1?255:0,i%3==2?255:0);
 assert(glGetError()==GL_NO_ERROR);
 for(unsigned i=0;i<96;++i){
  float initial[]={-.5f,0,1,0,0,.5f,0,0,1,0},blue[]={.5f,0,0,0,1},yellow[]={-.5f,.5f,1,1,0,.5f,0,0,1,0},white[]={1,1,1};
  glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  upload(b,1,0,0,40,initial);draw(b,0);
  upload(b,0,1,20,20,blue);draw(b,1);
  upload(b,1,0,0,40,yellow);draw(b,0);
  pixel(8,16,255,0,0);pixel(24,16,0,0,255);pixel(8,24,255,255,0);
  upload(b,0,0,8,12,white);draw(b,0);pixel(8,24,255,255,255);
  assert(glGetError()==GL_NO_ERROR&&stat_buffers<=3&&stat_buffer_bytes==stat_buffers*b->size);
 }
 run_buffer_destroy(&b);assert(!stat_buffers&&!stat_buffer_bytes&&glGetError()==GL_NO_ERROR);
 puts("PASS real GLES: 451 exact pixel checks, 64 queued renames without readback, overlapping synchronized writes, NOOVERWRITE, DISCARD, store reuse and destruction");
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    actual = production()
    compile_probe('buffer-storage', HOST + actual + CHECKS, {}, compiler_override=CLANG)
    if args.serial:
        out = B.ROOT / 'build/test/buffer-storage-gpu'
        out.mkdir(parents=True, exist_ok=True)
        source, binary = out / 'probe.c', out / 'probe'
        source.write_text(GPU + actual + GPU_CHECKS, encoding='utf-8')
        subprocess.run([str(B.CLANG), '--target=aarch64-linux-android29', '-std=gnu11', '-O2',
                        '-Iandroid/host', '-Iandroid/runtime', str(source), '-o', str(binary),
                        '-Wl,-z,max-page-size=16384', '-lEGL', '-lGLESv3'], cwd=B.ROOT, check=True)
        adb = ['adb', '-s', args.serial]
        remote = '/data/local/tmp/sims2-buffer-storage-check'
        subprocess.run(adb + ['push', str(binary), remote], check=True, capture_output=True)
        result = subprocess.run(adb + ['shell', f'chmod 755 {remote} && {remote}'], capture_output=True, timeout=45)
        (out / 'device.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout.decode())


if __name__ == '__main__':
    main()
