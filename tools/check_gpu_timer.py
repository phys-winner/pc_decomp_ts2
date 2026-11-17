"""Run the real asynchronous GPU timer with controlled driver query answers."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_gl.c').read_text(encoding='utf-8')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef unsigned GLuint,GLenum;
typedef int GLint;
typedef uint64_t GLuint64;
#define GL_TIME_ELAPSED_EXT 1
#define GL_GPU_DISJOINT_EXT 2
#define GL_QUERY_RESULT_AVAILABLE_EXT 3
#define GL_QUERY_RESULT 4
#define GL_EXTENSIONS 5
#define GL_RENDERER 6
#define GL_VERSION 7
static const char *renderer="Adreno 650",*version="OpenGL ES 3.2",*extensions="GL_EXT_disjoint_timer_query",*environment;
static unsigned starts,ends,generated,logs,available=1,disjoint;
static uint64_t time_ns=1000000,answer_ns=500000;
static const char *getenv(const char *name){assert(!strcmp(name,"S2_GPU_TIMER"));return environment;}
static const unsigned char *glGetString(GLenum name){assert(name>=5&&name<=7);return (const unsigned char *)(name==5?extensions:name==6?renderer:version);}
static void query_answer(GLuint id,GLenum name,GLuint64 *answer){assert(id>=100&&id<=105&&name==4);*answer=answer_ns;}
static void *eglGetProcAddress(const char *name){assert(!strcmp(name,"glGetQueryObjectui64vEXT"));return (void *)query_answer;}
static void glGenQueries(unsigned n,GLuint *queries){assert(n==6);for(unsigned i=0;i<n;++i)queries[i]=100+i;++generated;}
static void glGetIntegerv(GLenum name,GLint *value){assert(name==2);*value=(int)disjoint;}
static void glGetQueryObjectuiv(GLuint id,GLenum name,GLuint *value){assert(id>=100&&id<=105&&name==3);*value=available;}
static void glBeginQuery(GLenum name,GLuint id){assert(name==1&&id>=100&&id<=105);++starts;}
static void glEndQuery(GLenum name){assert(name==1);++ends;}
static uint64_t void_monotonic_ns(void){return time_ns;}
static void void_log(const char *format,...){assert(format);++logs;}
'''
    actual = source[source.index('#define GPU_QUERIES'):source.index('static volatile int dump_requested;')]
    policy = function(source, 'static int old_mali_driver(const char *renderer, const char *version)\n')
    checks = r'''
static void reset(void){gpu_timer_state=gpu_query_open=0;gpu_query_head=gpu_query_tail=0;gpu_ns=gpu_frames=0;starts=ends=generated=logs=0;time_ns=1000000;available=1;disjoint=0;environment=NULL;renderer="Adreno 650";version="OpenGL ES 3.2";}
int main(void){
 reset();gpu_timer_begin();assert(generated==1&&starts==1&&gpu_query_open);
 gpu_timer_end();time_ns=2000000;gpu_timer_begin();assert(gpu_ns==500000&&gpu_frames==1&&starts==2&&ends==1);
 gpu_timer_end();time_ns=3000000;answer_ns=UINT64_MAX;gpu_timer_begin();
 assert(gpu_timer_state==-1&&!gpu_query_open&&gpu_ns==500000&&gpu_frames==1&&starts==2&&logs==2);
 gpu_timer_end();gpu_timer_begin();assert(starts==2&&ends==2);
 reset();renderer="Mali-G51";version="OpenGL ES 3.2 v1.r18p0-01rel0";gpu_timer_begin();
 assert(gpu_timer_state==-1&&!generated&&!starts&&!gpu_query_open);
 reset();environment="0";gpu_timer_begin();assert(gpu_timer_state==-1&&!generated&&!starts);
 reset();gpu_timer_begin();gpu_timer_end();available=0;gpu_timer_begin();assert(!gpu_frames&&gpu_query_tail==0);
 disjoint=1;gpu_timer_end();gpu_timer_begin();assert(!gpu_frames&&gpu_query_tail==1);
 puts("PASS production GPU timer: asynchronous normal samples, no pending-query wait, disjoint drop, impossible duration excluded and timer disabled, known Mali r18 and explicit-off guards");
 return 0;
}
'''
    compile_probe('gpu-timer', prelude + policy + actual + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
