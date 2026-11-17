"""Run the production shader window presenter with strict GL call spies."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_filter.c').read_text(encoding='utf-8')
    presenter = (ROOT / 'android/host/d3d9_gl.c').read_text(encoding='utf-8')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned GLuint,GLenum;
typedef int GLint;
typedef intptr_t GLintptr;
#define GL_TEXTURE_MIN_FILTER 1
#define GL_TEXTURE_MAG_FILTER 2
#define GL_TEXTURE_WRAP_S 3
#define GL_TEXTURE_WRAP_T 4
#define GL_LINEAR 5
#define GL_CLAMP_TO_EDGE 6
#define GL_DEPTH_TEST 7
#define GL_STENCIL_TEST 8
#define GL_SCISSOR_TEST 9
#define GL_BLEND 10
#define GL_CULL_FACE 11
#define GL_POLYGON_OFFSET_FILL 12
#define GL_TRUE 1
#define GL_FRAMEBUFFER 13
#define GL_TEXTURE31 14
#define GL_TEXTURE_2D 15
#define GL_TRIANGLE_STRIP 16
#define GL_NO_ERROR 0
#define GL_COPY_READ_BUFFER_BINDING 17
#define GL_COPY_WRITE_BUFFER_BINDING 18
#define GL_COPY_READ_BUFFER 19
#define GL_COPY_WRITE_BUFFER 20
#define GL_STREAM_READ 21
#define GL_MAP_READ_BIT 22
static GLuint sampler,filter_vao;
static unsigned calls,draws,allocations,fail,disabled,params;
static int expected_x,expected_y,expected_w,expected_h;
static GLuint read_binding=41,write_binding=42;
static uint32_t gpu_word;
static unsigned buffer_allocations,copies,map_failure,unmap_failure;
static void glGetIntegerv(GLenum name,GLint *value){assert(name==17||name==18);*value=name==17?(GLint)read_binding:(GLint)write_binding;}
static void glGenBuffers(int n,GLuint *p){assert(n==1);*p=99;++buffer_allocations;}
static void glBindBuffer(GLenum target,GLuint name){assert(target==19||target==20);if(target==19)read_binding=name;else write_binding=name;}
static void glBufferData(GLenum target,int size,const void *data,GLenum usage){assert(target==20&&write_binding==99&&size==4&&!data&&usage==21);}
static void glCopyBufferSubData(GLenum source,GLenum target,intptr_t at,intptr_t out,int size){
 assert(source==19&&target==20&&read_binding==77&&write_binding==99&&at==37&&out==0&&size==4);gpu_word=0x1234ABCD;++copies;
}
static void *glMapBufferRange(GLenum target,intptr_t at,int size,GLenum flags){
 assert(target==19&&read_binding==99&&at==0&&size==4&&flags==22);return map_failure?NULL:&gpu_word;
}
static int glUnmapBuffer(GLenum target){assert(target==19&&read_binding==99);return !unmap_failure;}
static GLenum glGetError(void){return GL_NO_ERROR;}
static void void_log(const char *fmt,...){(void)fmt;assert(0);}
static GLuint program(unsigned kind){assert(kind==5);++calls;return fail?0:77;}
static void glGenVertexArrays(int n,GLuint *p){assert(n==1);*p=31;++allocations;}
static void glGenSamplers(int n,GLuint *p){assert(n==1);*p=32;++allocations;}
static void glSamplerParameteri(GLuint s,GLenum name,GLint value){
 assert(s==32&&name>=1&&name<=4&&value==(name<=2?GL_LINEAR:GL_CLAMP_TO_EDGE));params|=1u<<name;
}
static void glBindVertexArray(GLuint v){assert(v==31);}
static void glDisable(GLenum cap){assert(cap>=7&&cap<=12);disabled|=1u<<cap;}
static void glColorMask(int a,int b,int c,int d){assert(a&&b&&c&&d);}
static void glBindFramebuffer(GLenum target,GLuint name){assert(target==GL_FRAMEBUFFER&&name==0);}
static void glViewport(int x,int y,int w,int h){assert(x==expected_x&&y==expected_y&&w==expected_w&&h==expected_h);}
static void glActiveTexture(GLenum unit){assert(unit==GL_TEXTURE31);}
static void glBindTexture(GLenum target,GLuint input){assert(target==GL_TEXTURE_2D&&input==123);}
static void glBindSampler(GLuint unit,GLuint s){assert(unit==31&&s==32);}
static void d3d_gl_use_program(GLuint p){assert(p==77);}
static GLint glGetUniformLocation(GLuint p,const char *name){assert(p==77);return !strcmp(name,"image")?0:!strcmp(name,"target")?1:!strcmp(name,"origin")?2:-1;}
static void glUniform1i(GLint location,GLint value){assert(location==0&&value==31);}
static void glUniform2f(GLint location,float x,float y){
 assert(location==1||location==2);
 if(location==1)assert(x==expected_w&&y==expected_h);else assert(x==expected_x&&y==expected_y);
}
static void glDrawArrays(GLenum mode,GLint start,int count){
 assert(mode==GL_TRIANGLE_STRIP&&start==0&&count==4);
 assert(disabled==0x1F80&&params==0x1E);++draws;
}
'''
    checks = r'''
int main(void){
 assert(old_mali_driver("Mali-G51","OpenGL ES 3.2 v1.r18p0-01rel0.caae"));
 assert(old_mali_driver("Mali-G51 MP4","OpenGL ES 3.2 v1.r18p0-01rel0"));
 assert(!old_mali_driver("Mali-G51","OpenGL ES 3.2 v1.r19p0"));
 assert(!old_mali_driver("Mali-G715","OpenGL ES 3.2 v1.r18p0"));
 assert(!old_mali_driver("Adreno 650","OpenGL ES 3.2 v1.r18p0"));
 assert(!old_mali_driver(NULL,"v1.r18p0")&&!old_mali_driver("Mali-G51",NULL));
 assert(shader_present_driver("ARM","Mali-G76 MC4"));
 assert(shader_present_driver("ARM","Mali-G51 MP4"));
 assert(shader_present_driver("ARM","Mali-G715"));
 assert(shader_present_driver("ARM","Immortalis-G720"));
 assert(shader_present_driver("ARM",NULL));
 assert(shader_present_driver(NULL,"Mali-T880"));
 assert(shader_present_driver("Google Inc.","ANGLE (ARM, Mali-G78, OpenGL ES 3.2)"));
 assert(shader_present_driver(NULL,"Immortalis-G925"));
 assert(!shader_present_driver("Qualcomm","Adreno 618"));
 assert(!shader_present_driver("Imagination Technologies","PowerVR D-Series"));
 assert(!shader_present_driver(NULL,NULL));
 assert(!old_mali_driver("Mali-G76 MC4","OpenGL ES 3.2 v1.r26p0"));
 assert(!d3d_filter_output(0,0,0,100,100)&&!calls);
 assert(!d3d_filter_output(123,0,0,0,100)&&!calls);
 assert(!d3d_filter_output(123,0,0,100,-1)&&!calls);
 assert(!d3d_filter_output(123,0,0,8193,100)&&!calls);
 fail=1;assert(!d3d_filter_output(123,0,0,100,100)&&calls==1&&!allocations&&!draws);fail=0;
 expected_x=13;expected_y=17;expected_w=1534;expected_h=707;
 assert(d3d_filter_output(123,13,17,1534,707)&&allocations==2&&draws==1);
 expected_x=expected_y=0;expected_w=1560;expected_h=720;disabled=0;
 assert(d3d_filter_output(123,0,0,1560,720)&&allocations==2&&draws==2);
 uint32_t value=0;
 assert(trace_buffer_word(77,37,&value)&&value==0x1234ABCD&&buffer_allocations==1&&copies==1);
 assert(read_binding==41&&write_binding==42);
 map_failure=1;value=0xDEADBEEF;
 assert(!trace_buffer_word(77,37,&value)&&value==0xDEADBEEF&&buffer_allocations==1&&copies==2);
 assert(read_binding==41&&write_binding==42);
 map_failure=0;unmap_failure=1;
 assert(!trace_buffer_word(77,37,&value)&&buffer_allocations==1&&copies==3);
 assert(read_binding==41&&write_binding==42);
 puts("PASS production window shader: ARM/Mali/Immortalis policy, default FBO, unblended state, sampler/VAO reuse, viewport+UV origin, size/compile guards; GPU word copied to independent scratch, binding restoration and map/unmap failures");
 return 0;
}
'''
    actual = '\n'.join(function(source, signature) for signature in (
        'static void filter_setup(', 'int d3d_filter_output('))
    actual += '\n' + function(presenter, 'static int old_mali_driver(const char *renderer, const char *version)\n')
    actual += '\n' + function(presenter, 'static int shader_present_driver(const char *vendor, const char *renderer)\n')
    actual += '\n' + function(presenter, 'static int trace_buffer_word(')
    compile_probe('window-present', prelude + actual + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
