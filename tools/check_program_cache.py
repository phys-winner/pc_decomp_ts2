"""Exercise actual FF/shader bind C and the shared program cache on ARM64."""
import io
import json
from pathlib import Path
import re
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/program-cache'
PRELUDE = r'''
#include "d3d9_internal.h"
static GLuint bound,next_name=10;
static uint32_t uses,deletes,bad,forgot,link_calls,link_ok=1;
uint32_t clean_uses,total_uses,draw_hash,draws;
static unsigned char pool[4096] __attribute__((aligned(16)));
static size_t pool_at;
void *memset(void *p,int c,size_t n){uint8_t *d=p;while(n--)*d++=(uint8_t)c;return p;}
void *calloc(size_t n,size_t s){size_t bytes=(n*s+15)&~15u;void *p=pool+pool_at;pool_at+=bytes;return p;}
void free(void *p){}
void void_log(const char *fmt,...){}
void glUseProgram(GLuint p){bound=p;++uses;}
void glDeleteProgram(GLuint p){++deletes;}
void glDeleteShader(GLuint p){}
void glDeleteTextures(GLsizei n,const GLuint *p){}
void MOJOSHADER_freeParseData(const MOJOSHADER_parseData *p){}
GLuint glCreateProgram(void){return next_name++;}
void glAttachShader(GLuint p,GLuint s){}
void glLinkProgram(GLuint p){}
void glGetProgramiv(GLuint p,GLenum name,GLint *value){*value=(GLint)link_ok;}
void glGetProgramInfoLog(GLuint p,GLsizei n,GLsizei *length,GLchar *out){if(n)*out=0;}
void d3d_ff_forget_program(GLuint p){++forgot;}
static void record(void){draw_hash=draw_hash*33u+bound;++draws;}
/* The program binary cache and statistics (d3d9_shader.c), off here: every
 * program links as before, so both runs see the same draws and names. */
static uint64_t stat_programs;
int d3d_program_cache_on(void){return 0;}
d3d_program_key d3d_program_key_of(const char *v,const char *f,const MOJOSHADER_parseData *a){d3d_program_key k={0,0};return k;}
GLuint d3d_program_cache_load(d3d_program_key k){return 0;}
void d3d_program_cache_store(d3d_program_key k,GLuint p){}
char *d3d_shader_text(GLuint s){return 0;}
void d3d_shader_count(int programs,uint64_t link_ns,int compiles,uint64_t compile_ns){}
uint64_t void_monotonic_ns(void){return 0;}
void glProgramParameteri(GLuint p,GLenum name,GLint value){}
'''
STUBS = r'''
static program_t *lookup,*linked;
static int shader_type_ok(d3d_shader *s,int pixel){return 1;}
static program_t *find_program(d3d_shader *v,d3d_shader *p,uint32_t bgra,uint32_t ff,uint32_t *h){*h=0;return lookup;}
static program_t *link_program(d3d_shader *v,d3d_shader *p,uint32_t bgra,GLuint ff,uint32_t id,uint32_t sm){
    ++link_calls;d3d_gl_use_program(linked->gl);return linked;
}
GLuint d3d_ff_vertex_stage(d3d_device *d,d3d_shader *s,uint32_t *id){*id=1;return 2;}
GLuint d3d_ff_pixel_stage(d3d_device *d,d3d_shader *s,uint32_t *id,uint32_t *sm){*id=1;*sm=1;return 3;}
void d3d_ff_uniforms(d3d_device *d,GLuint p,int v,int f){if(bound!=p)bad=1;}
static ff_vs vertex={.gl=2,.id=2};
static ff_fs pixel={.gl=3,.id=3};
static ff_locs loc;
static void pixel_key(d3d_device *d,ff_fkey *k,uint32_t available,int mixed){k->stage_count=1;}
static int vertex_key(d3d_device *d,ff_vkey *k,const uint32_t *o,uint32_t n){return 1;}
static ff_vs *vertex_half(const ff_vkey *k){return &vertex;}
static ff_fs *pixel_half(const ff_fkey *k){return &pixel;}
static ff_locs *locations(GLuint p){if(bound!=p)bad=2;loc.program=p;return &loc;}
static void vertex_uniforms(d3d_device *d,ff_locs *l){if(bound!=l->program)bad=3;}
static void pixel_uniforms(d3d_device *d,ff_locs *l,int a){if(bound!=l->program)bad=4;}
static int bgra(d3d_device *d,int u,int i){return 0;}
'''
CHECK = r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
uint32_t selftest(void){
    static d3d_device d;
    static MOJOSHADER_parseData pd;
    static d3d_shader vs,ps;
    static program_t programmable,new_program,mixed,reused;
    int count,usage[16],index[16];uint32_t sm,vm;
    const int *u,*i,*pt,*vt;
    vs.parse=ps.parse=&pd;
    for(int n=0;n<5000;++n){REQUIRE(d3d_ff_bind(&d,&count,usage,index,&sm));REQUIRE(bound==10 && count==12);record();}
    clean_uses=uses;REQUIRE(clean_uses==EXPECTED_CLEAN_USES && !bad);
    programmable.gl=20;lookup=&programmable;
    REQUIRE(d3d_shader_bind2(&d,&vs,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));record();
    REQUIRE(bound==20);uint32_t before=uses;
    for(int n=0;n<100;++n)REQUIRE(d3d_shader_bind2(&d,&vs,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));
    REQUIRE(uses==before);record();
    if(CACHED){
        /* A fullscreen filter changed the real GL program between identical guest draws. */
        glUseProgram(99);current_gl_program_valid=0;before=uses;
        REQUIRE(d3d_shader_bind2(&d,&vs,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));
        REQUIRE(bound==20&&uses==before+1);
    }
    for(int n=0;n<100;++n){REQUIRE(d3d_ff_bind(&d,&count,usage,index,&sm));REQUIRE(bound==10);record();}
    new_program.gl=30;lookup=NULL;linked=&new_program;
    REQUIRE(d3d_shader_bind2(&d,&vs,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));
    REQUIRE(bound==30 && link_calls==1);record();
    mixed.gl=40;lookup=&mixed;
    REQUIRE(d3d_shader_bind2(&d,NULL,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));REQUIRE(bound==40);record();
    /* ProcessVertices uses a private program then restores GL_CURRENT_PROGRAM. */
    GLuint saved=bound;d3d_gl_use_program(90);REQUIRE(bound==90);d3d_gl_use_program(saved);
    before=uses;
    REQUIRE(d3d_shader_bind2(&d,NULL,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));
    REQUIRE(bound==40 && uses==before);record();
    program_destroy(&mixed);REQUIRE(!current_program && forgot==1);
    REQUIRE(!CACHED || !current_gl_program_valid);
    reused.gl=40;lookup=&reused;
    REQUIRE(d3d_shader_bind2(&d,&vs,&ps,bgra,&count,&u,&i,&sm,&vm,&pt,&vt));REQUIRE(bound==40);record();
    before=uses;d3d_gl_delete_program(77);d3d_gl_use_program(40);
    REQUIRE(uses==before+!CACHED);record();
    d3d_gl_use_program(0);REQUIRE(bound==0);before=uses;d3d_gl_use_program(0);
    REQUIRE(uses==before+!CACHED);record();
    /* Initial context binding invalidates the cached program; window/pbuffer
     * surface replacement retains this same context and needs no reset. */
    current_gl_program_valid=0;bound=0;before=uses;d3d_gl_use_program(40);
    REQUIRE(bound==40 && uses==before+1);record();
    static process_program pv1,pv2;static d3d_shader process_shader;
    pv1.gl=90;pv2.gl=91;pv1.next=&pv2;process_shader.process=&pv1;
    d3d_gl_use_program(90);process_destroy(&process_shader);
    REQUIRE(!process_shader.process && deletes==4 && (!CACHED || !current_gl_program_valid));
    before=uses;vertex.id=4;link_ok=0;
    REQUIRE(!d3d_ff_bind(&d,&count,usage,index,&sm));REQUIRE(uses==before && deletes==5);
    REQUIRE(!bad);total_uses=uses;return 0;
}
'''


def check(label, cached):
    texts = {name: ((OUT / (name + '.before.c')) if not cached else B.ROOT / ('android/host/' + name + '.c')).read_text()
             for name in ('d3d9_gl', 'd3d9_ff', 'd3d9_shader')}
    gl, ff, shader = (texts[n] for n in ('d3d9_gl', 'd3d9_ff', 'd3d9_shader'))
    types = shader[shader.index('typedef struct { int type, index, count; } umap_t;'):
                   shader.index('uint32_t d3d_constant_generation = 1;')]
    types += shader[shader.index('typedef struct process_program {'):shader.index('static void process_destroy(d3d_shader *s)\n{')]
    types += ff[ff.index('typedef struct {\n    uint32_t inputs, bgra;'):ff.index('static uint32_t next_id = 1;')]
    helpers = 'static GLuint current_gl_program; static int current_gl_program_valid;\nstatic struct {GLuint program;GLint location,value;} ui_filters[64];\n'
    if cached:
        helpers += function(gl, 'void d3d_gl_use_program(') + function(gl, 'void d3d_gl_delete_program(')
    else:
        helpers += 'void d3d_gl_use_program(GLuint p){glUseProgram(p);}\nvoid d3d_gl_delete_program(GLuint p){glDeleteProgram(p);}\n'
    actual = (next(line + '\n' for line in shader.splitlines() if line.startswith('void d3d_shader_forget_current(void) {')) + function(ff, 'void d3d_ff_attributes(') +
              function(ff, 'int d3d_ff_bind(') + function(shader, 'int d3d_shader_bind2(') +
              function(shader, 'static void program_destroy(') + function(shader, 'static void process_destroy(d3d_shader *s)\n{'))
    cfile, binary = OUT / (label + '.c'), OUT / (label + '.elf')
    cfile.write_text(PRELUDE + types + helpers + STUBS + actual +
                     f'\n#define CACHED {int(cached)}\n#define EXPECTED_CLEAN_USES {1 if cached else 5000}\n' + CHECK)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Wl,--gc-sections,-e,selftest', '-Iandroid/host', '-Iandroid/runtime', str(cfile), '-o', str(binary)], check=True)
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
        for r in relocations.iter_relocations():
            assert r['r_info_type'] == 1027
            uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=5000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
    assert uc.reg_read(A.UC_ARM64_REG_X0) == 0, (label, 'C line', uc.reg_read(A.UC_ARM64_REG_X0))
    return {key: int.from_bytes(uc.mem_read(base + symbols[key], 4), 'little')
            for key in ('clean_uses', 'total_uses', 'draw_hash', 'draws')}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    # Guard every real program mutation, not just the fixture's selected paths.
    hosts = list((B.ROOT / 'android/host').glob('*.c'))
    calls = {name: [(p.name, m.start()) for p in hosts for m in re.finditer(r'\b' + name + r'\(', p.read_text(encoding='utf-8'))]
             for name in ('glUseProgram', 'glDeleteProgram')}
    assert all(len(v) == 1 and v[0][0] == 'd3d9_gl.c' for v in calls.values()), calls
    gl = (B.ROOT / 'android/host/d3d9_gl.c').read_text()
    assert 'current_gl_program_valid = 0;' in function(gl, 'int d3d_gl_init(void)')
    shader = (B.ROOT / 'android/host/d3d9_shader.c').read_text()
    assert shader.count('d3d_gl_use_program(') == 4 and shader.count('d3d_gl_delete_program(') == 3
    result = {label: check(label, cached) for label, cached in (('before', False), ('after', True))}
    assert result['before']['draw_hash'] == result['after']['draw_hash']
    assert result['before']['draws'] == result['after']['draws']
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-DVOID_ANDROID_APP', '-fsyntax-only',
                    *('android/host/' + name + '.c' for name in ('d3d9_gl', 'd3d9_ff', 'd3d9_shader'))], check=True)
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS actual ARM64 FF/shader binds: 5000 FF draws, shader/mixed transitions, link boundary, '
          'ProcessVertices save/restore, program destruction/name reuse, failed link and context invalidation:', result)


if __name__ == '__main__':
    main()
