"""Run the production ES3.0 window-copy shader on an isolated EGL pbuffer."""
import argparse
import json
import subprocess
import build_android as B
from check_virtual_query import function


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', action='append', required=True)
    a = p.parse_args()
    out = B.ROOT / 'build/test/window-present-device'
    out.mkdir(parents=True, exist_ok=True)
    source = (B.ROOT / 'android/host/d3d9_filter.c').read_text()
    declarations = source[source.index('static const char vertex[]'):source.index('static GLuint shader(')]
    actual = '\n'.join(function(source, s) for s in (
        'static GLuint shader(', 'static GLuint program(', 'static void filter_setup(', 'int d3d_filter_output('))
    prelude = r'''
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static GLuint programs[6],sampler,filter_vao;
static unsigned failed;
static const char fsr1_easu_glsl[]="",fsr1_rcas_glsl[]="";
static void void_log(const char *format,...){va_list a;va_start(a,format);vprintf(format,a);va_end(a);puts("");}
static void d3d_gl_use_program(GLuint p){glUseProgram(p);}
static void d3d_gl_delete_program(GLuint p){glDeleteProgram(p);}
'''
    checks = r'''
int main(void){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);assert(eglInitialize(d,NULL,NULL));
 assert(eglBindAPI(EGL_OPENGL_ES_API));
 const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 EGLConfig config;EGLint count;assert(eglChooseConfig(d,attrs,&config,1,&count)&&count==1);
 const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
 EGLContext c=eglCreateContext(d,config,EGL_NO_CONTEXT,ca);assert(c!=EGL_NO_CONTEXT);
 EGLSurface surface=EGL_NO_SURFACE;GLuint input=0;unsigned pixels=0;
 unsigned char image[16*16*4];
 const unsigned char colors[4][4]={{255,0,0,17},{0,255,0,41},{0,0,255,71},{255,255,255,101}};
 for(int y=0;y<16;++y)for(int x=0;x<16;++x)memcpy(image+4*(y*16+x),colors[2*(y>=8)+(x>=8)],4);
 for(int round=0;round<3;++round){
  int sw=round==1?96:round==2?130:128,sh=round==1?128:round==2?94:96;
  const EGLint pa[]={EGL_WIDTH,sw,EGL_HEIGHT,sh,EGL_NONE};
  EGLSurface next=eglCreatePbufferSurface(d,config,pa);assert(next!=EGL_NO_SURFACE);
  assert(eglMakeCurrent(d,next,next,c));if(surface!=EGL_NO_SURFACE)assert(eglDestroySurface(d,surface));surface=next;
  if(!input){
   printf("GPU %s / %s / %s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
   glGenTextures(1,&input);glBindTexture(GL_TEXTURE_2D,input);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,image);
   assert(!glGetError());
  }
  for(int partial=0;partial<2;++partial){
   int x=partial?13:0,y=partial?17:0,w=partial?sw-31:sw,h=partial?sh-39:sh;
   glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
   glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_BLEND);glBlendFunc(GL_ZERO,GL_ZERO);glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,1);
   glEnable(GL_DEPTH_TEST);glDepthFunc(GL_NEVER);glEnable(GL_CULL_FACE);glColorMask(0,0,0,0);
   assert(d3d_filter_output(input,x,y,w,h));assert(!glGetError());
   GLuint shaders[2];GLsizei attached;glGetAttachedShaders(programs[5],2,&attached,shaders);assert(attached==2);
   for(int i=0;i<2;++i){char text[64];glGetShaderSource(shaders[i],sizeof text,NULL,text);assert(!strncmp(text,"#version 300 es",15));}
   unsigned char *result=malloc((size_t)sw*sh*4);assert(result);glReadPixels(0,0,sw,sh,GL_RGBA,GL_UNSIGNED_BYTE,result);assert(!glGetError());
   for(int py=0;py<sh;++py)for(int px=0;px<sw;++px){
    int inside=px>=x&&px<x+w&&py>=y&&py<y+h;
    if(inside&&(abs(2*(px-x)+1-w)<w/3||abs(2*(py-y)+1-h)<h/3))continue;
    unsigned char expected[4]={0,0,0,255};
    if(inside){int key=2*(2*(py-y)+1<h)+(2*(px-x)+1>=w);memcpy(expected,colors[key],3);}
    const unsigned char *got=result+4*(py*sw+px);
    for(int k=0;k<4;++k)assert(abs((int)got[k]-expected[k])<=1);
    ++pixels;
   }
   free(result);
  }
 }
 printf("PASS ES300 production presenter: %u pixels, flipped color quadrants, opaque alpha, hostile GL state, letterbox origins and three EGL surface sizes\n",pixels);
 glDeleteTextures(1,&input);glDeleteSamplers(1,&sampler);glDeleteVertexArrays(1,&filter_vao);glDeleteProgram(programs[5]);
 assert(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));assert(eglDestroySurface(d,surface));assert(eglDestroyContext(d,c));assert(eglTerminate(d));
 return 0;
}
'''
    cfile, binary = out / 'probe.c', out / 'probe'
    cfile.write_text(prelude + declarations + actual + checks)
    subprocess.run([str(B.CLANG), '--target=aarch64-linux-android29', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wl,-z,max-page-size=16384', str(cfile), '-o', str(binary), '-lEGL', '-lGLESv3'], check=True)
    results = []
    for index, serial in enumerate(a.serial):
        adb = ['adb', '-s', serial]
        remote = '/data/local/tmp/sims2-present-check-20261113'
        subprocess.run(adb + ['push', str(binary), remote], check=True)
        subprocess.run(adb + ['shell', f'chmod 755 {remote}'], check=True)
        run = subprocess.run(adb + ['shell', remote], text=True, capture_output=True, timeout=30)
        (out / f'device-{index + 1}.log').write_text(run.stdout + run.stderr)
        subprocess.run(adb + ['shell', f'rm {remote}'], check=True)
        assert run.returncode == 0 and 'PASS ES300 production presenter:' in run.stdout, run.stdout + run.stderr
        results.append({'serial': serial, 'log': f'device-{index + 1}.log', 'passed': True})
        print(run.stdout)
    (out / 'result.json').write_text(json.dumps({'source_sha256': B.sha_file(cfile), 'binary_sha256': B.sha_file(binary),
        'devices': results, 'scope': 'Production shader/setup/output on isolated pbuffers; Android window acceptance is separate.'}, indent=2) + '\n')


if __name__ == '__main__':
    main()
