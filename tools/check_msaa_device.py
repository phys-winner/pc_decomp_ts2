"""Compile/run production scene MSAA binding on an isolated ES3 pbuffer."""
import argparse
import subprocess
import build_android as B
from build_game_apk import ADB
from check_virtual_query import function

PRELUDE = r'''
#include "d3d9_internal.h"
#include "sims2_settings.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define FB_UNKNOWN 0xFFFFFFFFu
static GLuint fbo,read_fbo,draw_fbo,unit_tex[32];
static d3d_device *test_device;
d3d_device *d3d_rt_device(void){return test_device;}
void d3d_gl_state_forget(void){}
static float target_scale;
static d3d_surface *trace_rt;
static int requested;
int s2_render_msaa(void){return requested;}
void void_log(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);puts("");}
static int tracing(d3d_device *d){(void)d;return 0;}
static void trace_dump(d3d_device *d,d3d_surface *s,const char *why){(void)d;(void)s;(void)why;}
GLuint d3d_surface_gl_texture(d3d_surface *s,GLenum *target,GLint *level,GLint *layer){
 *target=GL_TEXTURE_2D;*level=*layer=0;return s->gl;
}
float d3d_scale_of(const d3d_surface *s){return s->scale;}
uint32_t d3d_scaled_size(uint32_t size,float scale){return (uint32_t)((float)size*scale+.5f);}
'''
CHECK = r'''
static GLuint shader(GLenum kind,const char *text){
 GLuint s=glCreateShader(kind);glShaderSource(s,1,&text,NULL);glCompileShader(s);
 GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;
}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);
 EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);assert(eglInitialize(display,NULL,NULL));
 const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 EGLConfig config;EGLint count;assert(eglChooseConfig(display,attrs,&config,1,&count)&&count);
 const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
 EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);assert(context!=EGL_NO_CONTEXT);
 const EGLint pa[]={EGL_WIDTH,128,EGL_HEIGHT,128,EGL_NONE};
 EGLSurface surface=eglCreatePbufferSurface(display,config,pa);assert(surface!=EGL_NO_SURFACE);
 assert(eglMakeCurrent(display,surface,surface,context));
 printf("GPU %s / %s / %s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 300 es\nuniform float z;const vec2 p[3]=vec2[3](vec2(-.85,-.9),vec2(.83,-.75),vec2(-.1,.87));void main(){gl_Position=vec4(p[gl_VertexID],z,1.);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;uniform vec4 color;out vec4 c;void main(){c=color;}");
 GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
 GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(program);
 GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
 d3d_format cf={0},df={0};cf.internal=GL_RGBA8;df.internal=GL_DEPTH24_STENCIL8;df.depth=df.stencil=1;
 d3d_surface color={0},depth={0};color.fmt=&cf;depth.fmt=&df;
 color.width=color.height=depth.width=depth.height=128;color.scale=depth.scale=1;
 color.screen_sized=depth.screen_sized=1;
 glGenTextures(1,&color.gl);glBindTexture(GL_TEXTURE_2D,color.gl);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,128,128);
 glGenTextures(1,&depth.gl);glBindTexture(GL_TEXTURE_2D,depth.gl);glTexStorage2D(GL_TEXTURE_2D,1,GL_DEPTH24_STENCIL8,128,128);
 d3d_device d={0};d.rt[0]=&color;d.ds=&depth;test_device=&d;
 GLuint read;glGenFramebuffers(1,&read);unsigned char pixels[128*128*4];
 d3d_surface other=color;glGenTextures(1,&other.gl);glBindTexture(GL_TEXTURE_2D,other.gl);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,128,128);
 for(int pass=0;pass<3;pass++){
  requested=pass==0?0:pass==1?2:4;msaa_samples=0;init_msaa();
  if(requested&&!msaa_samples){printf("SKIP requested MSAA %d unsupported\n",requested);continue;}
  fb_forget_all();d.fbo_dirty=1;d3d_gl_bind_targets(&d);
  GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);assert(samples>=(requested?requested:0));
  glViewport(0,0,128,128);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
  glClearColor(0,0,0,1);glClearDepthf(1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
  glUniform1f(glGetUniformLocation(program,"z"),0);glUniform4f(glGetUniformLocation(program,"color"),1,1,1,1);glDrawArrays(GL_TRIANGLES,0,3);
  glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color.gl,0);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
  glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
  unsigned gray=0;for(int i=0;i<128*128;i++)if(pixels[4*i]>0&&pixels[4*i]<255)gray++;
  printf("draw requested=%d samples=%d gray=%u center=%u,%u,%u depthrb=%u\n",requested,samples,gray,pixels[4*(64*128+64)],pixels[4*(64*128+64)+1],pixels[4*(64*128+64)+2],depth.gl_msaa_depth);
  assert(requested?gray>50:gray==0);
  /* Reading color and temporarily detaching depth must preserve depth samples. */
  d.ds=NULL;d.fbo_dirty=1;d3d_gl_bind_targets(&d);d.ds=&depth;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
  d.rt[0]=&other;d.fbo_dirty=1;d3d_gl_bind_targets(&d);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
  d.rt[0]=&color;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
  glUniform1f(glGetUniformLocation(program,"z"),.8f);glUniform4f(glGetUniformLocation(program,"color"),1,0,0,1);glDrawArrays(GL_TRIANGLES,0,3);
  glBindFramebuffer(GL_FRAMEBUFFER,read);glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
  const unsigned char *center=pixels+4*(64*128+64);printf("after far center=%u,%u,%u samples=%d rb=%u\n",center[0],center[1],center[2],samples,fb_depth_rb);assert(center[0]==255&&center[1]==255&&center[2]==255);
  /* Run the production StretchRect path from a single-sample source to the scene. */
  const uint32_t rect[]={0,0,8,8};d3d_gl_blit(&other,rect,&color,rect,0);assert(!glGetError());
  glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color.gl,0);
  glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
  assert(pixels[4*(4*128+4)]==0&&pixels[4*(4*128+4)+2]==255);
  assert(pixels[4*(64*128+64)]==255&&pixels[4*(64*128+64)+1]==255);
  d.fbo_dirty=1;d3d_gl_bind_targets(&d);glDrawArrays(GL_TRIANGLES,0,3);
  glBindFramebuffer(GL_FRAMEBUFFER,read);glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());
  assert(pixels[4*(4*128+4)]==0&&pixels[4*(4*128+4)+2]==255);
  assert(pixels[4*(64*128+64)]==255&&pixels[4*(64*128+64)+1]==255);
  printf("PASS scene MSAA %d: actual %d samples, %u covered edge pixels, depth preserved after resolve/toggle\n",requested,samples,gray);
 }
 assert(!glGetError());return 0;
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args=parser.parse_args()
    out=B.ROOT/'build/test/msaa-device';out.mkdir(parents=True,exist_ok=True)
    source=(B.ROOT/'android/host/d3d9_gl.c').read_text(encoding='utf8')
    actual=source[source.index('/* EXT color textures'):source.index('/* --------------------------------------------------------------- state */')]
    cfile=out/'probe.c';binary=out/'probe'
    cfile.write_text(PRELUDE+function(source,'static GLenum depth_attachment(')+actual+
        next(line for line in source.splitlines() if line.startswith('static int scaled_px('))+'\n'+function(source,'static void attach(')+
        function(source,'void d3d_gl_blit(')+CHECK,encoding='utf8')
    subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Iandroid/host','-Iandroid/runtime',str(cfile),'-o',str(binary),'-lEGL','-lGLESv3'],check=True)
    print(binary)
    if args.serial:
        adb=[str(ADB),'-s',args.serial]
        remote='/data/local/tmp/sims2-msaa-check'
        subprocess.run(adb+['push',str(binary),remote],check=True)
        subprocess.run(adb+['shell','chmod','755',remote],check=True)
        run=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=30)
        (out/'device.log').write_text(run.stdout+run.stderr,encoding='utf8')
        subprocess.run(adb+['shell','rm',remote],check=True)
        print(run.stdout);assert run.returncode==0,run.stderr

if __name__=='__main__':main()
