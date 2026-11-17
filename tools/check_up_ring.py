"""Execute the production UP ring allocator/fence logic with observable GL boundaries."""
from pathlib import Path

from audio_probe import compile_probe
from build_android import ROOT


source = (ROOT / 'android/host/d3d9_gl.c').read_text()
ring = source[source.index('#define RING_SEGMENTS'):source.index('static uint32_t enabled_attribs;')]
prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLuint, GLenum, GLbitfield;
typedef intptr_t GLsizeiptr;
typedef uintptr_t GLsync;
#define GL_EXTENSIONS 1
#define GL_MAP_WRITE_BIT 2
#define GL_MAP_PERSISTENT_BIT_EXT 4
#define GL_MAP_COHERENT_BIT_EXT 8
#define GL_COPY_WRITE_BUFFER 16
#define GL_SYNC_GPU_COMMANDS_COMPLETE 32
#define GL_SYNC_FLUSH_COMMANDS_BIT 64
#define GL_ALREADY_SIGNALED 128
#define GL_CONDITION_SATISFIED 256
#define GL_WAIT_FAILED 512
static uint8_t buffer[24u<<20];
static int disabled,missing_extension,missing_proc,map_failure,fence_failure,wait_failure;
static int stores,maps,deletes,wait_calls,sync_deletes,timeouts;
static uintptr_t next_sync;
static const char *test_getenv(const char *name){assert(!strcmp(name,"VOID_NO_RING"));return disabled?"1":NULL;}
#define getenv test_getenv
static const unsigned char *glGetString(GLenum name){assert(name==GL_EXTENSIONS);return (const unsigned char *)(missing_extension?"":"GL_EXT_buffer_storage");}
static void storage(GLenum t,GLsizeiptr n,const void *p,GLbitfield f){assert(t==GL_COPY_WRITE_BUFFER&&n==sizeof buffer&&!p&&f==14);++stores;}
typedef void (*PFNGLBUFFERSTORAGEEXTPROC)(GLenum,GLsizeiptr,const void*,GLbitfield);
static PFNGLBUFFERSTORAGEEXTPROC eglGetProcAddress(const char *name){assert(!strcmp(name,"glBufferStorageEXT"));return missing_proc?NULL:storage;}
static void glGenBuffers(int n,GLuint *p){assert(n==1);*p=77;}
static void glBindBuffer(GLenum t,GLuint n){assert(t==GL_COPY_WRITE_BUFFER&&n==77);}
static void *glMapBufferRange(GLenum t,intptr_t off,GLsizeiptr n,GLbitfield f){assert(t==GL_COPY_WRITE_BUFFER&&!off&&n==sizeof buffer&&f==14);++maps;return map_failure?NULL:buffer;}
static void glDeleteBuffers(int n,const GLuint *p){assert(n==1&&*p==77);++deletes;}
static GLsync glFenceSync(GLenum c,GLbitfield f){assert(c==GL_SYNC_GPU_COMMANDS_COMPLETE&&!f);return fence_failure?0:++next_sync;}
static void glDeleteSync(GLsync f){assert(f);++sync_deletes;}
static GLenum glClientWaitSync(GLsync f,GLbitfield flags,uint64_t ns){assert(f&&flags==GL_SYNC_FLUSH_COMMANDS_BIT&&ns==1000000000);++wait_calls;if(wait_failure)return GL_WAIT_FAILED;if(timeouts){--timeouts;return 0;}return GL_CONDITION_SATISFIED;}
static GLenum glGetError(void){return 0;}
static void void_log(const char *format,...){(void)format;}
'''
checks = r'''
static void reset(void){
 ring_buffer=0;ring_map=NULL;ring_segment=ring_used=0;ring_state=0;memset(ring_fence,0,sizeof ring_fence);
 disabled=missing_extension=missing_proc=map_failure=fence_failure=wait_failure=0;
 stores=maps=deletes=wait_calls=sync_deletes=timeouts=0;next_sync=0;
}
int main(void){
 uint32_t at=123;
 reset();assert(!ring_alloc(0,&at)&&!ring_state&&at==123);
 reset();missing_extension=1;assert(!ring_alloc(1,&at)&&ring_state<0&&!stores);
 reset();disabled=1;assert(!ring_alloc(1,&at)&&ring_state<0&&!stores);
 reset();missing_proc=1;assert(!ring_alloc(1,&at)&&ring_state<0&&!stores);
 reset();map_failure=1;assert(!ring_alloc(1,&at)&&ring_state<0&&maps==1&&deletes==1&&!ring_buffer);
 reset();assert(ring_alloc(1,&at)==buffer&&at==0&&stores==1);
 assert(ring_alloc(17,&at)==buffer+16&&at==16&&ring_used==33);
 uint32_t used=ring_used;assert(!ring_alloc(UINT32_MAX,&at)&&ring_used==used);
 ring_used=0;assert(ring_alloc(RING_SEGMENT_BYTES,&at)==buffer);assert(!ring_alloc(1,&at));
 ring_frame_end();assert(ring_segment==1&&!wait_calls&&!ring_used);
 assert(ring_alloc(16,&at)==buffer+RING_SEGMENT_BYTES&&at==RING_SEGMENT_BYTES);
 ring_frame_end();assert(ring_segment==2&&!wait_calls);
 timeouts=1;ring_frame_end();assert(ring_segment==0&&wait_calls==2&&!ring_fence[0]);
 assert(ring_alloc(4,&at)==buffer&&at==0);
 reset();assert(ring_alloc(4,&at));fence_failure=1;ring_frame_end();assert(ring_state<0&&!ring_alloc(4,&at));
 reset();assert(ring_alloc(4,&at));ring_frame_end();ring_frame_end();wait_failure=1;ring_frame_end();assert(ring_state<0&&!ring_alloc(4,&at));
 puts("PASS production UP ring: feature/proc/map fallback, alignment, segment capacity/overflow, fence-before-reuse, timeout retry and fail-closed fence errors; GL calls are observed boundaries");
 return 0;
}
'''
compile_probe('up-ring', prelude + ring + checks, {})
