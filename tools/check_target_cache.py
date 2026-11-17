"""Run the real target-binding cache with GL mocks on AArch64 Unicorn."""
import io
import json
from pathlib import Path
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/fbo-cache'
PRELUDE = r'''
#include "d3d9_internal.h"
#include "sims2_settings.h"
#include <EGL/egl.h>
#define FB_UNKNOWN 0xFFFFFFFFu
static GLuint fbo,unit_tex[32],draw_binding,read_binding;
static float target_scale;
static d3d_surface *trace_rt;
static uint32_t binds,attachments,bad;
static uint32_t msaa_colors,msaa_depths;
static GLenum framebuffer_status = GL_FRAMEBUFFER_COMPLETE;
static int tracing(d3d_device *d) { return 0; }
static void trace_dump(d3d_device *d,d3d_surface *s,const char *why) { }
void void_log(const char *fmt,...) { }
void *memcpy(void *p,const void *q,size_t n) { uint8_t *d=p;const uint8_t *s=q;while(n--)*d++=*s++;return p; }
void *memset(void *p,int c,size_t n) { uint8_t *d=p;while(n--)*d++=(uint8_t)c;return p; }
int memcmp(const void *p,const void *q,size_t n) { const uint8_t *a=p,*b=q;while(n--){if(*a!=*b)return *a-*b;++a;++b;}return 0; }
void glGenFramebuffers(GLsizei n,GLuint *names) { while(n--)*names++=77; }
void glBindFramebuffer(GLenum target,GLuint name) {
    ++binds;
    if(target==GL_FRAMEBUFFER || target==GL_DRAW_FRAMEBUFFER)draw_binding=name;
    if(target==GL_FRAMEBUFFER || target==GL_READ_FRAMEBUFFER)read_binding=name;
}
void glFramebufferTexture2D(GLenum target,GLenum point,GLenum textarget,GLuint texture,GLint level) {
    if(target!=GL_FRAMEBUFFER || draw_binding!=fbo)bad=1;++attachments;
}
void glFramebufferTextureLayer(GLenum target,GLenum point,GLuint texture,GLint level,GLint layer) {
    if(target!=GL_FRAMEBUFFER || draw_binding!=fbo)bad=2;++attachments;
}
void glDrawBuffers(GLsizei n,const GLenum *bufs) { if(draw_binding!=fbo)bad=3; }
GLenum glCheckFramebufferStatus(GLenum target) { if(draw_binding!=fbo)bad=4;return framebuffer_status; }
GLuint d3d_surface_gl_texture(d3d_surface *s,GLenum *target,GLint *level,GLint *layer) {
    *target=GL_TEXTURE_2D;*level=*layer=0;return s->gl;
}
GLenum glGetError(void) { return GL_NO_ERROR; }
void glGenRenderbuffers(GLsizei n,GLuint *names) { while(n--)*names++=123; }
void glDeleteRenderbuffers(GLsizei n,const GLuint *names) { }
void glBindRenderbuffer(GLenum target,GLuint name) { }
void glFramebufferRenderbuffer(GLenum target,GLenum point,GLenum kind,GLuint name) {
    if(target!=GL_FRAMEBUFFER || draw_binding!=fbo || name!=123)bad=5;
}
static void mock_msaa_texture(GLenum target,GLenum point,GLenum textarget,GLuint tex,GLint level,GLsizei samples) {
    if(samples!=4 || point!=GL_COLOR_ATTACHMENT0 || target!=GL_FRAMEBUFFER)bad=6;
    ++msaa_colors;++attachments;
}
static void mock_msaa_storage(GLenum target,GLsizei samples,GLenum format,GLsizei w,GLsizei h) {
    if(samples!=4 || target!=GL_RENDERBUFFER || w!=640 || h!=480)bad=7;
    ++msaa_depths;
}
float d3d_scale_of(const d3d_surface *s) { return s->scale; }
uint32_t d3d_scaled_size(uint32_t size,float scale) { return (uint32_t)((float)size*scale); }
'''
CHECK = r'''
#define REQUIRE(c) do { if(!(c))return __LINE__; } while(0)
uint32_t clean_binds,final_binds;
uint32_t selftest(void) {
    static d3d_format color_format,depth_format;
    static d3d_surface color,other,depth;
    static d3d_device d;
    color_format.internal=GL_RGBA8;depth_format.internal=GL_DEPTH_COMPONENT24;
    color.fmt=other.fmt=&color_format;depth.fmt=&depth_format;
    color.gl=1;other.gl=3;depth.gl=2;
    color.width=other.width=depth.width=640;color.height=other.height=depth.height=480;
    color.scale=other.scale=depth.scale=1;
    d.rt[0]=&color;d.ds=&depth;d.fbo_dirty=1;
    d3d_gl_bind_targets(&d);
    REQUIRE(draw_binding==fbo && read_binding==fbo && !d.fbo_dirty && !bad);
    uint32_t initial_attachments=attachments;
    for(int i=0;i<5000;++i)d3d_gl_bind_targets(&d);
    REQUIRE(attachments==initial_attachments && !bad);
    clean_binds=binds;
    REQUIRE(clean_binds==EXPECTED_CLEAN_BINDS);
    /* Present, blits, readback and frame dumps restore the main FBO and
     * mark it dirty; exercise each external-binding lifecycle independently. */
    for(uint32_t i=0;i<4;++i) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER,90+i);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,i ? 99+i : 0);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);d.fbo_dirty=1;
        uint32_t before=binds;
        d3d_gl_bind_targets(&d);
        REQUIRE(binds==before+1 && draw_binding==fbo && read_binding==fbo && !d.fbo_dirty);
    }
    d.rt[0]=&other;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_color[0].tex==3 && !d.fbo_dirty);
    d.ds=NULL;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_depth.tex==2);
    d.ds=&depth;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_depth.tex==2);
    d3d_gl_forget_texture(other.gl);d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_color[0].tex==3);
    fb_forget_all();d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_color[0].tex==3 && fb_depth.tex==2 && !bad);
    #ifdef TEST_MSAA
    /* A scene uses matching color/depth samples, retaining its depth renderbuffer across toggles. */
    color.screen_sized=depth.screen_sized=1;d.rt[0]=&color;d.ds=&depth;
    msaa_samples=4;msaa_texture=mock_msaa_texture;msaa_storage=mock_msaa_storage;
    d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(fb_samples==4 && msaa_colors==1 && msaa_depths==1 && fb_depth_rb==123 && !bad);
    for(int i=0;i<5000;++i)d3d_gl_bind_targets(&d);
    REQUIRE(msaa_colors==1 && msaa_depths==1);
    d.ds=NULL;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    d.ds=&depth;d.fbo_dirty=1;d3d_gl_bind_targets(&d);
    REQUIRE(msaa_colors==1 && msaa_depths==1 && fb_depth_rb==123 && !bad);
    /* Special/MRT targets stay single-sample and restore the scene cache afterward. */
    d.rt[1]=&other;d.fbo_dirty=1;d3d_gl_bind_targets(&d);REQUIRE(!fb_samples && !fb_depth_rb);
    d.rt[1]=NULL;d.fbo_dirty=1;d3d_gl_bind_targets(&d);REQUIRE(fb_samples==4 && msaa_depths==1);
    /* An unsupported sample/format combination retries safely without MSAA. */
    framebuffer_status=GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;fb_forget_all();d.fbo_dirty=1;
    d3d_gl_bind_targets(&d);REQUIRE(!msaa_samples && !fb_samples && !d.fbo_dirty);
    #endif
    final_binds=binds;
    return 0;
}
'''


def check(path, label, expected):
    text = path.read_text()
    snippet = text[(text.index('/* EXT color textures') if '/* EXT color textures' in text else text.index('typedef struct { GLuint tex; GLenum target; GLint level, layer; } fb_attachment;')):
                   text.index('/* --------------------------------------------------------------- state */')]
    source, binary = OUT / (label + '.c'), OUT / (label + '.elf')
    source.write_text(('#define TEST_MSAA\n' if '/* EXT color textures' in text else '') + PRELUDE + function(text, 'static GLenum depth_attachment(') + snippet +
                      '\n#define EXPECTED_CLEAN_BINDS ' + str(expected) + '\n' + CHECK)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Wl,--gc-sections,-e,selftest', '-Iandroid/host', '-Iandroid/runtime', str(source), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, stack, stop = 0x10000000, 0x20000000, 0x30000000
    uc.mem_map(base, 0x200000); uc.mem_map(stack, 0x10000); uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            uc.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    if relocations:
        for relocation in relocations.iter_relocations():
            assert relocation['r_info_type'] == 1027
            uc.mem_write(base + relocation['r_offset'], (base + relocation['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=5000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
    assert uc.reg_read(A.UC_ARM64_REG_X0) == 0, (label, 'C line', uc.reg_read(A.UC_ARM64_REG_X0))
    return {key: int.from_bytes(uc.mem_read(base + symbols[key], 4), 'little')
            for key in ('clean_binds', 'final_binds')}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    results = {}
    before = OUT / 'd3d9_gl.before.c'
    if before.exists():
        results['before'] = check(before, 'before', 5001)
    results['after'] = check(B.ROOT / 'android/host/d3d9_gl.c', 'after', 1)
    (OUT / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS actual ARM64 target cache: 5000 clean draws, present/blit/readback/dump bindings, '
          'target/depth changes, texture-name reuse and full cache invalidation:', results)


if __name__ == '__main__':
    main()
