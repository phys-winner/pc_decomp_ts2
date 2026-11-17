"""Exercise production EGL surface loss and same-serial recovery with EGL spies."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_gl.c').read_text(encoding='utf-8')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
typedef int EGLint,EGLBoolean;
typedef uintptr_t EGLDisplay,EGLSurface,EGLContext,EGLConfig,EGLNativeWindowType;
#define EGL_NO_SURFACE 0
#define EGL_TRUE 1
#define EGL_FALSE 0
static EGLDisplay egl_display=2;
static EGLContext egl_context=3;
static EGLSurface egl_surface=4,egl_pbuffer=5;
static EGLConfig egl_config=6;
static uint32_t egl_window_serial=7;
static int success=1,swap_calls,get_errors,current_calls,destroy_calls,create_calls,acks,waits,logs;
static void void_log(const char *text,...){assert(text);++logs;}
static EGLBoolean eglSwapBuffers(EGLDisplay d,EGLSurface s){assert(d==2&&s==4);++swap_calls;return success;}
static EGLint eglGetError(void){++get_errors;return 0x300D;}
static EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext c){
 assert(d==2&&c==3&&draw==read&&(draw==4||draw==5));++current_calls;return EGL_TRUE;
}
static EGLBoolean eglDestroySurface(EGLDisplay d,EGLSurface s){assert(d==2&&s==4);++destroy_calls;return EGL_TRUE;}
static void *void_android_window_current(uint32_t *serial,int wait){assert(wait==0);*serial=7;return (void *)9;}
static void void_android_window_ack(uint32_t serial){assert(serial==7);++acks;}
static void d3d_rt_wait_begin(void){++waits;}
static void d3d_rt_wait_end(void){++waits;}
static EGLSurface eglCreateWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType w,const EGLint *a){
 assert(d==2&&c==6&&w==9&&!a);++create_calls;return 4;
}
static EGLBoolean eglSwapInterval(EGLDisplay d,EGLint i){assert(d==2&&i==1);return EGL_TRUE;}
'''
    checks = r'''
int main(void){
 assert(swap_window_surface()&&swap_calls==1&&logs==1&&!get_errors&&!destroy_calls);
 assert(swap_window_surface()&&swap_calls==2&&logs==1&&!get_errors);
 follow_window();assert(!create_calls&&!acks&&!current_calls);
 success=0;assert(!swap_window_surface()&&swap_calls==3&&get_errors==1);
 assert(egl_surface==EGL_NO_SURFACE&&destroy_calls==1&&current_calls==1);
 follow_window();assert(egl_surface==4&&create_calls==1&&acks==2&&current_calls==3&&!waits);
 success=1;assert(swap_window_surface()&&logs==4&&get_errors==1);
 assert(destroy_calls==1&&create_calls==1);
 puts("PASS production EGL: successful swap leaves the surface alone, failed swap reads/logs error, releases failed surface onto pbuffer, same-serial next frame recreates, successful presentation resumes");
 return 0;
}
'''
    actual = '\n'.join(function(source, signature) for signature in (
        'static void follow_window(', 'static int swap_window_surface('))
    compile_probe('egl-swap', prelude + actual + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
