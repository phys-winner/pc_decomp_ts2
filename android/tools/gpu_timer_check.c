/* Standalone EGL pbuffer evidence: core/EXT timer calls, result widths and pixels. */
#define _GNU_SOURCE
#include <assert.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TIME_ELAPSED 0x88BF
#define DISJOINT 0x8FBB
#define COUNTER_BITS 0x8864
#define TIMESTAMP 0x8E28
typedef void (*gen_fn)(GLsizei,GLuint *);
typedef void (*begin_fn)(GLenum,GLuint);
typedef void (*end_fn)(GLenum);
typedef void (*info_fn)(GLenum,GLenum,GLint *);
typedef void (*result_fn)(GLuint,GLenum,GLuint *);
typedef void (*result64_fn)(GLuint,GLenum,GLuint64 *);
static uint64_t now_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static GLuint shader(GLenum kind,const char *source){
 GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);
 GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"shader: %s\n",log);}assert(ok);return s;
}
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);
 EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);assert(display!=EGL_NO_DISPLAY);
 EGLint major,minor;assert(eglInitialize(display,&major,&minor));
 const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_NONE};
 EGLConfig config;EGLint count;assert(eglChooseConfig(display,attrs,&config,1,&count)&&count);
 const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
 EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);assert(context!=EGL_NO_CONTEXT);
 const EGLint pa[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
 EGLSurface surface=eglCreatePbufferSurface(display,config,pa);assert(surface!=EGL_NO_SURFACE&&eglMakeCurrent(display,surface,surface,context));
 printf("vendor=%s\nrenderer=%s\nversion=%s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
 const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
 assert(extensions&&strstr(extensions,"GL_EXT_disjoint_timer_query"));
 gen_fn gen=(gen_fn)eglGetProcAddress("glGenQueriesEXT");
 begin_fn begin=(begin_fn)eglGetProcAddress("glBeginQueryEXT");
 end_fn end=(end_fn)eglGetProcAddress("glEndQueryEXT");
 info_fn info=(info_fn)eglGetProcAddress("glGetQueryivEXT");
 result_fn result=(result_fn)eglGetProcAddress("glGetQueryObjectuivEXT");
 result64_fn result64=(result64_fn)eglGetProcAddress("glGetQueryObjectui64vEXT");
 assert(gen&&begin&&end&&info&&result&&result64);
 GLint bits_time=-1,bits_stamp=-1;info(TIME_ELAPSED,COUNTER_BITS,&bits_time);GLenum ext_bits_error=glGetError();
 info(TIMESTAMP,COUNTER_BITS,&bits_stamp);GLenum stamp_error=glGetError();
 GLint core_bits=-1;glGetQueryiv(TIME_ELAPSED,COUNTER_BITS,&core_bits);GLenum core_bits_error=glGetError();
 printf("bits elapsed_ext=%d timestamp_ext=%d elapsed_core=%d errors=%04X/%04X/%04X\n",bits_time,bits_stamp,core_bits,ext_bits_error,stamp_error,core_bits_error);
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;uniform vec4 shade;out vec4 color;void main(){color=shade;}");
 GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);assert(linked);
 glUseProgram(p);GLint shade=glGetUniformLocation(p,"shade");assert(shade>=0);glViewport(0,0,64,64);
 GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_SCISSOR_TEST);
 unsigned rendered=0,invalid=0;
 for(unsigned mode=0;mode<2;++mode)for(unsigned run=0;run<8;++run){
  GLuint query=0;if(mode)gen(1,&query);else glGenQueries(1,&query);GLenum created=glGetError();
  GLint disjoint_before=0;glGetIntegerv(DISJOINT,&disjoint_before);
  uint64_t start=now_ns();if(mode)begin(TIME_ELAPSED,query);else glBeginQuery(TIME_ELAPSED,query);GLenum begun=glGetError();
  glUniform4f(shade,run&1?1:0,run&1?0:1,.25f,1);for(unsigned draw=0;draw<16;++draw)glDrawArrays(GL_TRIANGLES,0,3);GLenum drawn=glGetError();
  if(mode)end(TIME_ELAPSED);else glEndQuery(TIME_ELAPSED);GLenum ended=glGetError();glFlush();
  GLuint available=0;GLenum polled=GL_NO_ERROR;
  while(!available&&now_ns()-start<2000000000ull){
   if(mode)result(query,GL_QUERY_RESULT_AVAILABLE,&available);else glGetQueryObjectuiv(query,GL_QUERY_RESULT_AVAILABLE,&available);
   polled=glGetError();if(polled)break;if(!available){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
  }
  GLuint core32=0,ext32=0;GLuint64 ext64=0;GLenum read_core=0,read_ext=0,read64=0;
  if(available&&!polled){glGetQueryObjectuiv(query,GL_QUERY_RESULT,&core32);read_core=glGetError();result(query,GL_QUERY_RESULT,&ext32);read_ext=glGetError();result64(query,GL_QUERY_RESULT,&ext64);read64=glGetError();}
  uint64_t wall=now_ns()-start;GLint disjoint_after=0;glGetIntegerv(DISJOINT,&disjoint_after);GLenum disjoint_error=glGetError();
  unsigned char pixel[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);GLenum pixels=glGetError();
  assert(!pixels&&pixel[0]==(run&1?255:0)&&pixel[1]==(run&1?0:255)&&pixel[2]>=63&&pixel[2]<=64&&pixel[3]==255);++rendered;
  int sane=bits_time>0&&available&&!created&&!begun&&!drawn&&!ended&&!polled&&!read_core&&!read_ext&&!read64&&!disjoint_error&&!disjoint_before&&!disjoint_after&&ext64<=wall+1000000ull;
  if(!sane)++invalid;
  printf("mode=%s run=%u host_ns=%llu available=%u core32=%u ext32=%u ext64=%llu disjoint=%d/%d sane=%d errors=%04X/%04X/%04X/%04X/%04X/%04X/%04X/%04X/%04X pixel=%u,%u,%u,%u\n",mode?"EXT":"core",run,(unsigned long long)wall,available,core32,ext32,(unsigned long long)ext64,disjoint_before,disjoint_after,sane,created,begun,drawn,ended,polled,read_core,read_ext,read64,disjoint_error,pixel[0],pixel[1],pixel[2],pixel[3]);
  glDeleteQueries(1,&query);while(glGetError()){}
 }
 printf("DONE pixel_checks=%u timer_invalid=%u\n",rendered,invalid);
 eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);return 0;
}
