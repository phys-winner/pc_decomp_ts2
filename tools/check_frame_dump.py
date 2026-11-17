"""Validate production frame readback and reject failed/stale GPU captures."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_gl.c').read_text(encoding='utf-8')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLuint, GLenum;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
#define GL_FALSE 0
typedef struct { uint32_t width,height; } d3d_surface;
typedef struct { d3d_surface *backbuffer; uint32_t frame; int fbo_dirty; } d3d_device;
enum {
 GL_NO_ERROR, GL_READ_FRAMEBUFFER_BINDING, GL_DRAW_FRAMEBUFFER_BINDING,
 GL_READ_BUFFER, GL_PIXEL_PACK_BUFFER_BINDING, GL_PACK_ALIGNMENT,
 GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
 GL_PIXEL_PACK_BUFFER, GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER,
 GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_COMPLETE, GL_RGBA, GL_UNSIGNED_BYTE,
 GL_INVALID_OPERATION, GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT,
 GL_RASTERIZER_DISCARD, GL_TRANSFORM_FEEDBACK_ACTIVE, GL_TRANSFORM_FEEDBACK_PAUSED
};
static GLuint read_fbo,draw_fbo;
static GLint read_binding,draw_binding,pack_binding,pack_align,pack_row,pack_x,pack_y;
static GLenum read_buffer[256],pending_error;
static unsigned reads,logs,read_failure,incomplete,attach_error;
static uint32_t d3d_scale_of(d3d_surface *bb){(void)bb;return 100;}
static uint32_t d3d_scaled_size(uint32_t x,uint32_t scale){assert(scale==100);return x;}
static void void_log(const char *fmt,...){(void)fmt;++logs;}
static GLenum glGetError(void){GLenum r=pending_error;pending_error=0;return r;}
static GLboolean glIsEnabled(GLenum cap){assert(cap==GL_RASTERIZER_DISCARD);return GL_FALSE;}
static void glGetBooleanv(GLenum name,GLboolean *v){assert(name==GL_TRANSFORM_FEEDBACK_ACTIVE||name==GL_TRANSFORM_FEEDBACK_PAUSED);*v=GL_FALSE;}
static void glGetIntegerv(GLenum name,GLint *v){
 switch(name){
 case GL_READ_FRAMEBUFFER_BINDING:*v=read_binding;break;
 case GL_DRAW_FRAMEBUFFER_BINDING:*v=draw_binding;break;
 case GL_READ_BUFFER:*v=(GLint)read_buffer[read_binding];break;
 case GL_PIXEL_PACK_BUFFER_BINDING:*v=pack_binding;break;
 case GL_PACK_ALIGNMENT:*v=pack_align;break;
 case GL_PACK_ROW_LENGTH:*v=pack_row;break;
 case GL_PACK_SKIP_PIXELS:*v=pack_x;break;
 case GL_PACK_SKIP_ROWS:*v=pack_y;break;
 default:assert(0);
 }
}
static void glBindBuffer(GLenum target,GLuint name){assert(target==GL_PIXEL_PACK_BUFFER);pack_binding=(GLint)name;}
static void glPixelStorei(GLenum name,GLint v){
 switch(name){
 case GL_PACK_ALIGNMENT:pack_align=v;break;
 case GL_PACK_ROW_LENGTH:pack_row=v;break;
 case GL_PACK_SKIP_PIXELS:pack_x=v;break;
 case GL_PACK_SKIP_ROWS:pack_y=v;break;
 default:assert(0);
 }
}
static void glGenFramebuffers(int n,GLuint *p){static GLuint next=91;assert(n==1);*p=next++;}
static void glBindFramebuffer(GLenum target,GLuint name){
 assert(name<256);if(target==GL_READ_FRAMEBUFFER)read_binding=(GLint)name;
 else {assert(target==GL_DRAW_FRAMEBUFFER);draw_binding=(GLint)name;}
}
static void attach(GLenum target,GLenum attachment,d3d_surface *bb){
 assert(target==GL_READ_FRAMEBUFFER&&attachment==GL_COLOR_ATTACHMENT0&&bb);
 assert((GLuint)read_binding==read_fbo);if(attach_error)pending_error=GL_INVALID_OPERATION;
}
static void glReadBuffer(GLenum value){read_buffer[read_binding]=value;}
static GLenum glCheckFramebufferStatus(GLenum target){assert(target==GL_READ_FRAMEBUFFER);return incomplete?GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:GL_FRAMEBUFFER_COMPLETE;}
static void glReadPixels(int x,int y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *px){
 const uint8_t rgba[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
 assert(!x&&!y&&w==2&&h==2&&format==GL_RGBA&&type==GL_UNSIGNED_BYTE);
 assert(!pack_binding&&pack_align==1&&!pack_row&&!pack_x&&!pack_y);
 assert((GLuint)read_binding==read_fbo&&read_buffer[read_binding]==GL_COLOR_ATTACHMENT0);
 ++reads;if(read_failure)pending_error=GL_INVALID_OPERATION;else memcpy(px,rgba,sizeof(rgba));
}
static void initial_state(void){
 read_binding=31;draw_binding=32;read_buffer[31]=123;pack_binding=77;
 pack_align=8;pack_row=11;pack_x=1;pack_y=2;pending_error=0;
}
static void restored(void){
 assert(read_binding==31&&draw_binding==32&&read_buffer[31]==123&&pack_binding==77);
 assert(pack_align==8&&pack_row==11&&pack_x==1&&pack_y==2);
}
static void sentinel(const char *path){FILE *f=fopen(path,"wb");assert(f);assert(fwrite("stale",1,5,f)==5);assert(!fclose(f));}
static void missing(const char *path){FILE *f=fopen(path,"rb");assert(!f);}
'''
    checks = r'''
int main(void){
 const char *path="validated-frame.bmp";
 d3d_surface bb={2,2};d3d_device dev={&bb,17,0};
 initial_state();pending_error=GL_INVALID_OPERATION;
 d3d_gl_dump_frame(&dev,path);restored();assert(reads==1&&logs==3&&dev.fbo_dirty);
 FILE *f=fopen(path,"rb");assert(f);uint8_t out[100];size_t n=fread(out,1,sizeof(out),f);assert(!fclose(f));
 const uint8_t bgr[16]={11,10,9,15,14,13,0,0,3,2,1,7,6,5,0,0};
 assert(n==70&&out[0]=='B'&&out[1]=='M'&&out[2]==70&&out[10]==54&&out[14]==40);
 assert(out[18]==2&&out[22]==2&&out[26]==1&&out[28]==24&&!memcmp(out+54,bgr,sizeof(bgr)));
 read_failure=1;initial_state();dev.fbo_dirty=0;
 d3d_gl_dump_frame(&dev,path);restored();assert(reads==2&&dev.fbo_dirty);missing(path);
 read_failure=0;incomplete=1;sentinel(path);initial_state();
 d3d_gl_dump_frame(&dev,path);restored();assert(reads==2);missing(path);
 incomplete=0;attach_error=1;sentinel(path);initial_state();
 d3d_gl_dump_frame(&dev,path);restored();assert(reads==2);missing(path);
 attach_error=0;bb.width=0;sentinel(path);initial_state();dev.fbo_dirty=0;
 d3d_gl_dump_frame(&dev,path);restored();assert(reads==2&&!dev.fbo_dirty);missing(path);
 puts("PASS frame dump: validated pixels/BMP, PBO/pack/FBO restoration, failed reads and stale-output rejection");
 return 0;
}
'''
    compile_probe('frame-dump', prelude + function(source, 'void d3d_gl_dump_frame(') + checks,
                  {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
