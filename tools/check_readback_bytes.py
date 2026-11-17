"""Compare actual surface-readback C with its pinned pre-optimization C.

    python tools/check_readback_bytes.py

Only the GLES/guest-memory adapters are mocked. The real read/resolve,
swizzle and format encoder execute on a Windows host, with GL call/state
traces and requested-allocation accounting. Artifacts go under build/test;
allocation savings are not a device PSS or frame-rate measurement.
"""
import ctypes
import hashlib
import io
import json
import re
import subprocess

import build_android as B
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as A

OUT = B.ROOT / 'build/test/readback-bytes'
REFERENCE = B.ROOT / 'tools/fixtures/readback_reference.c'


def function(source, name):
    match = re.search(r'^(?:static )?(?:int|float|void|uint8_t|uint16_t|uint32_t) ' +
                      re.escape(name) + r'\([^;\n]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start()
    pos = source.index('{', match.start())
    depth = 1
    # These small functions contain no braces in strings/comments.
    while depth:
        pos += 1
        depth += (source[pos] == '{') - (source[pos] == '}')
    return source[start:pos + 1] + '\n'


PRELUDE = r'''
typedef __SIZE_TYPE__ size_t;
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t, GLenum, GLuint;
typedef int int32_t, GLint, GLsizei;
typedef unsigned long long uint64_t;
typedef float GLfloat;
#define NULL ((void *)0)
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#define fixture_heap_reset() ((void)0)
#else
#define EXPORT
void fixture_heap_reset(void);
#endif
#define D3DUSAGE_RENDERTARGET 1
int _fltused;
void *malloc(size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
typedef struct { uint32_t d3d; int depth, is_float; GLenum internal; GLint swizzle[4]; } d3d_format;
typedef struct { uint32_t usage; float scale; } d3d_texture;
typedef struct {
    uint32_t width, height, format, usage; const d3d_format *fmt;
    float scale; d3d_texture *container;
} d3d_surface;
typedef struct { int fbo_dirty; } d3d_device;
static d3d_device dev;
static d3d_device *d3d_rt_device(void) { return &dev; }
static uint8_t guest[16*1024*1024];
#define GPTR(x) (guest+(uint32_t)(x))
#define MEM32(x) (*(uint32_t *)GPTR(x))
static size_t live, peak;
static uint32_t allocations, fail_allocation;
static void *tracked_malloc(size_t n) {
    if (++allocations == fail_allocation) return NULL;
    size_t *p = malloc(n+16);
    if (!p) return NULL;
    p[0]=n; live+=n; if (live>peak) peak=live;
    memset((uint8_t *)p+16,0xA5,n);
    return (uint8_t *)p+16;
}
static void tracked_free(void *v) {
    if (!v) return;
    size_t *p=(size_t *)((uint8_t *)v-16); live-=p[0]; free(p);
}
#define malloc tracked_malloc
#define free tracked_free
static uint32_t trace[4096], trace_n, next_id=100, fail_read, seed, stale_error;
static uint32_t bound_read, bound_draw, read_buffer, gl_error, state_valid, active_unit;
static uint32_t read_count, last_rect[4], last_type, log_count;
static GLuint fbo=77, read_fbo, draw_fbo;
static int void_verbose;
static void record(uint32_t op,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    if (trace_n+5>4096) __builtin_trap();
    trace[trace_n++]=op;trace[trace_n++]=a;trace[trace_n++]=b;
    trace[trace_n++]=c;trace[trace_n++]=d;
}
static void void_log(const char *f,...) { (void)f; ++log_count; }
#define VLOG(...) void_log(__VA_ARGS__)
static void glGenFramebuffers(GLsizei n,GLuint *p) { while(n--) *p++=++next_id; record(1,next_id,0,0,0); }
static void glGenTextures(GLsizei n,GLuint *p) { while(n--) *p++=++next_id; record(2,next_id,0,0,0); }
static void glDeleteTextures(GLsizei n,const GLuint *p) { record(3,n,*p,0,0); }
static void d3d_gl_forget_texture(GLuint p) { record(4,p,0,0,0); }
static void d3d_gl_upload_unit(void) { active_unit=31; record(5,31,0,0,0); }
static void glBindTexture(GLenum t,GLuint p) { record(6,t,p,0,0); }
static void glTexStorage2D(GLenum t,GLsizei n,GLenum f,GLsizei w,GLsizei h) {
    record(7,t,n,f,0);record(8,w,h,0,0);
}
static void glBindFramebuffer(GLenum t,GLuint p) {
    if(t==GL_FRAMEBUFFER||t==GL_READ_FRAMEBUFFER)bound_read=p;
    if(t==GL_FRAMEBUFFER||t==GL_DRAW_FRAMEBUFFER)bound_draw=p;
    record(9,t,p,0,0);
}
static GLuint d3d_surface_gl_texture(d3d_surface *s,GLenum *t,GLint *level,GLint *layer) {
    (void)s; *t=GL_TEXTURE_2D;*level=0;*layer=0; return 13;
}
static void glFramebufferTexture2D(GLenum f,GLenum a,GLenum t,GLuint p,GLint l) {
    record(10,f,a,t,p);record(11,l,0,0,0);
}
static void glFramebufferTextureLayer(GLenum f,GLenum a,GLuint p,GLint l,GLint k) {
    record(12,f,a,p,l);record(13,k,0,0,0);
}
static void d3d_gl_state_forget(void) { state_valid=0;record(14,0,0,0,0); }
static void glDisable(GLenum e) { record(15,e,0,0,0); }
static void glColorMask(int r,int g,int b,int a) { record(16,r,g,b,a); }
static void glBlitFramebuffer(GLint x0,GLint y0,GLint x1,GLint y1,
    GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLenum mask,GLenum filter) {
    record(17,x0,y0,x1,y1);record(18,dx0,dy0,dx1,dy1);record(19,mask,filter,0,0);
}
static void glReadBuffer(GLenum b) { read_buffer=b;record(20,b,0,0,0); }
static GLenum glCheckFramebufferStatus(GLenum t) {
    record(21,t,0,0,0);
    return fail_read==1?GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:GL_FRAMEBUFFER_COMPLETE;
}
static GLenum glGetError(void) {
    GLenum e=0;
    if(stale_error) {e=stale_error;stale_error=0;}
    else {e=gl_error;gl_error=0;}
    record(22,e,0,0,0);return e;
}
static uint8_t channel(uint32_t x,uint32_t y,uint32_t k) {
    if(k==0)return (uint8_t)(seed+x+y*13);
    if(k==1)return (uint8_t)(seed*3+x*7+y);
    if(k==2)return (uint8_t)((seed^0xA5)+x*11+y*19);
    return (uint8_t)(seed*5+x*17+y*23);
}
static void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum fmt,GLenum type,void *out) {
    ++read_count;last_rect[0]=x;last_rect[1]=y;last_rect[2]=w;last_rect[3]=h;last_type=type;
    record(23,x,y,w,h);record(24,fmt,type,0,0);
    uint32_t n=(uint32_t)w*h;
    if(fail_read==3)n/=2;
    if(fail_read==4)n=0;
    for(uint32_t i=0;i<n;++i)for(uint32_t k=0;k<4;++k) {
        uint8_t b=channel(x+i%(uint32_t)w,y+i/(uint32_t)w,k);
        if(type==GL_FLOAT)((float *)out)[4*i+k]=b/255.0f;
        else ((uint8_t *)out)[4*i+k]=b;
    }
    if(fail_read>=2)gl_error=0x502;
}
'''

WRAPPER = r'''
/* config: format, width,height,padding,owner,scale*100,seed,failure,
 * logical,rect x,y,w,h,allocation-failure-index,verbose,invalid-format. */
EXPORT uint32_t run(const uint32_t *p,uint8_t *out) {
    fixture_heap_reset();
    uint32_t fmt=p[0],w=p[1],h=p[2],pixel=fmt==23?2:fmt==116?16:4;
    d3d_format desc={fmt,0,fmt==116,fmt==116?GL_RGBA32F:GL_RGBA8,
        {GL_BLUE,GL_GREEN,GL_RED,fmt==22?GL_ONE:GL_ALPHA}};
    if(fmt==23||fmt==116) {desc.swizzle[0]=GL_RED;desc.swizzle[2]=GL_BLUE;}
    d3d_texture texture={p[4]==2?D3DUSAGE_RENDERTARGET:0,p[5]/100.0f};
    d3d_surface s={w,h,fmt,0,&desc,p[5]/100.0f,p[4]?&texture:NULL};
    if(p[15]==1)s.fmt=NULL;
    if(p[15]==2)desc.depth=1;
    if(p[15]==3)desc.internal=0;
    uint32_t rect[4]={p[9],p[10],p[9]+p[11],p[10]+p[12]};
    uint32_t n=p[8]?p[11]*p[12]*16+64:(w*pixel+p[3])*h+64;
    if(n+0x1000>sizeof guest)__builtin_trap();
    memset(guest+0x1000,0xCD,n);
    peak=live=allocations=0;fail_allocation=p[13];trace_n=0;
    fail_read=p[7];seed=p[6];stale_error=0x500;gl_error=0;read_count=0;
    read_fbo=draw_fbo=0;bound_read=41;bound_draw=42;read_buffer=0xDEAD;
    state_valid=1;active_unit=7;dev.fbo_dirty=0;void_verbose=p[14];log_count=0;
    for(int k=0;k<4;++k)last_rect[k]=0xFFFFFFFF;last_type=0;
    uint32_t ok=1;
    if(p[8])ok=d3d_gl_read_rgba(&s,rect,(float *)(guest+0x1020));
    else d3d_gl_read_surface(&s,0x1020,w*pixel+p[3]);
    memcpy(out,guest+0x1000,n);
    return ok;
}
EXPORT uint64_t metric(uint32_t n) {
    switch(n) {
    case 0:return peak;case 1:return live;case 2:return allocations;
    case 3:return dev.fbo_dirty;case 4:return bound_read;case 5:return bound_draw;
    case 6:return state_valid;case 7:return active_unit;case 8:return read_count;
    case 9:return last_type;case 10:return read_buffer;case 11:return log_count;
    case 12:return stale_error;case 13:return gl_error;
    default:return n<18?last_rect[n-14]:0;
    }
}
EXPORT uint32_t get_trace(uint32_t *out) { memcpy(out,trace,trace_n*4);return trace_n; }
'''

ARM_MEMORY = r'''
typedef __SIZE_TYPE__ size_t;
static unsigned char heap[1024*1024];
static size_t heap_at;
void fixture_heap_reset(void) { heap_at=0; }
void *malloc(size_t n) {
    n=(n+15)&~(size_t)15;
    if(n>sizeof heap-heap_at)return (void *)0;
    void *p=heap+heap_at;heap_at+=n;return p;
}
void free(void *p) { (void)p; }
void *memcpy(void *d,const void *s,size_t n) {
    volatile unsigned char *p=d;const volatile unsigned char *q=s;
    for(size_t i=0;i<n;++i)p[i]=q[i];return d;
}
void *memset(void *d,int c,size_t n) {
    volatile unsigned char *p=d;
    for(size_t i=0;i<n;++i)p[i]=(unsigned char)c;return d;
}
'''


def compile_fixture(name, code, library):
    path = OUT / (name + '.c')
    path.write_text(code)
    obj, dll = OUT / (name + '.obj'), OUT / (name + '.dll')
    subprocess.run([str(B.CLANG), '--target=x86_64-pc-windows-msvc', '-O2',
                    '-ffp-contract=off', '-fno-stack-protector', '-c', str(path), '-o', str(obj)], check=True)
    subprocess.run([str(B.CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                    '/out:' + str(dll), str(obj), str(library)], check=True)
    dll = ctypes.CDLL(str(dll))
    dll.run.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    dll.run.restype = ctypes.c_uint32
    dll.metric.argtypes = [ctypes.c_uint32]
    dll.metric.restype = ctypes.c_uint64
    dll.get_trace.argtypes = [ctypes.c_void_p]
    return dll


def execute(dll, p):
    pixel = 2 if p[0] == 23 else 16 if p[0] == 116 else 4
    n = (p[11]*p[12]*16 if p[8] else (p[1]*pixel+p[3])*p[2]) + 64
    out = ctypes.create_string_buffer(n)
    ok = dll.run((ctypes.c_uint32*16)(*p), out)
    metrics = [dll.metric(i) for i in range(18)]
    trace = (ctypes.c_uint32*4096)()
    count = dll.get_trace(trace)
    return ok, out.raw, metrics, list(trace[:count])


def expected_bytes(p):
    fmt,w,h,pad,owner,scale,seed,fail = p[:8]
    pitch=w*4+pad
    data=bytearray(b'\xCD'*(pitch*h+64))
    if fail or p[15]:
        data[32:32+pitch*h]=b'\0'*(pitch*h)
        return bytes(data)
    for y in range(h):
        for x in range(w):
            raw=[(seed+x+y*13)&255,(seed*3+x*7+y)&255,
                 ((seed^0xA5)+x*11+y*19)&255,(seed*5+x*17+y*23)&255]
            if owner!=1:raw[0],raw[2]=raw[2],raw[0]
            if fmt==22:raw[3]=255
            at=32+y*pitch+x*4
            data[at:at+4]=bytes(raw)
    return bytes(data)


class ArmFixture:
    """Same optimized C and GL adapters; only libc uses a resettable test arena."""
    def __init__(self,name):
        elf_path=OUT/(name+'.elf')
        subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-fno-stack-protector','-nostdlib',
            '-Wl,-e,run',str(OUT/(name+'.c')),str(OUT/'memory.c'),'-o',str(elf_path)],check=True)
        elf=ELFFile(io.BytesIO(elf_path.read_bytes()))
        self.symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        self.base,self.arena,self.stack,self.stop=0x10000000,0x20000000,0x30000000,0x40000000
        self.arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        segments=[s for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        end=max(s['p_vaddr']+s['p_memsz'] for s in segments)
        self.arm.mem_map(self.base,(end+4095)&~4095)
        for address,size in [(self.arena,0x10000),(self.stack,0x10000),(self.stop,0x1000)]:
            self.arm.mem_map(address,size)
        for s in segments:self.arm.mem_write(self.base+s['p_vaddr'],s.data())
        relocations=elf.get_section_by_name('.rela.dyn')
        if relocations:
            for r in relocations.iter_relocations():
                assert r['r_info_type']==1027,r['r_info_type']
                self.arm.mem_write(self.base+r['r_offset'],(self.base+r['r_addend']).to_bytes(8,'little'))

    def call(self,name,*args,count=False):
        for i,v in enumerate(args):self.arm.reg_write(A.UC_ARM64_REG_X0+i,v)
        self.arm.reg_write(A.UC_ARM64_REG_SP,self.stack+0xFFF0)
        self.arm.reg_write(A.UC_ARM64_REG_LR,self.stop)
        instructions=[0]
        hook=None
        if count:
            def tick(uc,address,size,data):instructions[0]+=1
            hook=self.arm.hook_add(UC_HOOK_CODE,tick)
        self.arm.emu_start(self.base+self.symbols[name],self.stop,timeout=10_000_000,count=20_000_000)
        if hook:self.arm.hook_del(hook)
        assert self.arm.reg_read(A.UC_ARM64_REG_PC)==self.stop,name
        return self.arm.reg_read(A.UC_ARM64_REG_X0),instructions[0]

    def execute(self,p,count=False):
        import struct
        self.arm.mem_write(self.arena,struct.pack('<16I',*p))
        ok,instructions=self.call('run',self.arena,self.arena+0x100,count=count)
        pixel=2 if p[0]==23 else 16 if p[0]==116 else 4
        n=(p[11]*p[12]*16 if p[8] else (p[1]*pixel+p[3])*p[2])+64
        out=bytes(self.arm.mem_read(self.arena+0x100,n))
        metrics=[self.call('metric',i)[0] for i in range(18)]
        length=self.call('get_trace',self.arena+0x8000)[0]
        trace=list(struct.unpack('<'+'I'*length,self.arm.mem_read(self.arena+0x8000,length*4)))
        return (ok,out,metrics,trace),instructions


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    gl=(B.ROOT/'android/host/d3d9_gl.c').read_text()
    texel=(B.ROOT/'android/host/d3d9_texel.c').read_text()
    ref=REFERENCE.read_text()
    common='\n'.join(function(texel,n) for n in ['d3d_half_to_float','d3d_float_to_half',
        'd3d_gl_layout_to_rgba','d3d_swizzle_rgba','d3d_storage_swizzle',
        'unorm','snorm8','d3d_format_encode'])
    common+='\n'.join(function(gl,n) for n in ['d3d_scale_of','scaled_px','attach','read_scaled'])
    candidate='\n'.join(function(gl,n) for n in ['read_rgba_pixels','d3d_gl_read_rgba','d3d_gl_read_surface'])
    # Use the NDK's real GL enum values, with a hard error for unrecognized names.
    enums={}
    for header in ['GLES3/gl32.h','GLES2/gl2ext.h']:
        text=(B.TOOLCHAIN/'sysroot/usr/include'/header).read_text()
        enums.update(re.findall(r'^#define\s+(GL_\w+)\s+(0x[0-9A-Fa-f]+|[01])\b',text,re.M))
    needed=set(re.findall(r'\bGL_\w+\b',PRELUDE+common+candidate+ref+WRAPPER))
    assert not needed-set(enums),needed-set(enums)
    definitions=''.join(f'#define {k} {enums[k]}\n' for k in sorted(needed))
    definition=OUT/'crt.def'
    definition.write_text('LIBRARY msvcrt.dll\nEXPORTS\nmalloc\nfree\nmemcpy\nmemset\n')
    library=OUT/'crt.lib'
    subprocess.run([str(B.CLANG.with_name('llvm-dlltool.exe')),'-m','i386:x86-64',
                    '-d',str(definition),'-l',str(library)],check=True)
    before=compile_fixture('baseline',definitions+PRELUDE+common+ref+WRAPPER,library)
    after=compile_fixture('candidate',definitions+PRELUDE+common+candidate+WRAPPER,library)
    cases=[]
    # All 256 values of every stored channel, both alpha contracts, all ownerships.
    for fmt in (21,22):
        for owner in (0,1,2):
            for seed in range(256):
                cases.append([fmt,7,5,12,owner,100,seed,0,0,0,0,0,0,0,1,0])
    for fmt in (21,22,23,116):
        for owner in (0,1,2):
            for scale in (50,100,200):
                for failure in range(5):
                    cases.append([fmt,17,9,12,owner,scale,83,failure,0,0,0,0,0,0,1,0])
                    cases.append([fmt,32,32,0,owner,scale,119,failure,1,3,5,7,9,0,0,0])
        for invalid in (1,2,3):
            cases.append([fmt,7,5,8,0,100,15,0,0,0,0,0,0,0,0,invalid])
            cases.append([fmt,32,32,0,1,100,15,0,1,3,5,7,9,0,0,invalid])
    # First picture-sized path and actual observed screen/readback size, padded.
    for fmt in (21,22):
        cases.append([fmt,128,128,4,1,50,171,0,0,0,0,0,0,0,1,0])
        cases.append([fmt,1334,600,12,0,100,71,0,0,0,0,0,0,0,1,0])
    peaks=[]
    compared=0
    for p in cases:
        a,b=execute(before,p),execute(after,p)
        assert a[:2]==b[:2],('output',p,a[0],b[0])
        assert a[2][3:]==b[2][3:],('state',p,a[2],b[2])
        assert a[3]==b[3],('GL calls',p,a[3],b[3])
        assert a[2][1]==b[2][1]==0,('leak',p,a[2],b[2])
        if p[0] in (21,22) and not p[8]:
            assert a[1]==expected_bytes(p),('independent byte reference',p)
        if p[1]==1334:
            assert a[2][0]==20*p[1]*p[2] and b[2][0]==4*p[1]*p[2]
            peaks.append({'format':p[0],'width':p[1],'height':p[2],
                'before_peak_requested_bytes':a[2][0],'after_peak_requested_bytes':b[2][0],
                'before_allocations':a[2][2],'after_allocations':b[2][2]})
        compared+=1
    # Allocation failure is newly handled cleanly; the old code dereferences NULL.
    oom_cases=0
    for fmt,logical,fail in [(21,0,1),(22,0,1),(21,1,1),(23,0,1),(23,0,2),(116,0,1)]:
        p=[fmt,17,9,12,0,100,35,0,logical,3,5,7,9,fail,0,0]
        ok,data,m,trace=execute(after,p)
        assert m[1]==0,('OOM leak',p,m)
        if logical:
            assert ok==0 and data==b'\xCD'*len(data),('logical OOM',p)
        else:
            assert data[:32]==data[-32:]==b'\xCD'*32
            assert data[32:-32]==bytes(len(data)-64),('surface OOM',p)
        oom_cases+=1
    # Independently execute the actual before/after C compiled for AArch64.
    # All channel values fit one 256-pixel row; representative rectangles cover
    # scaling, formats, ownerships and failed reads without large emulation runs.
    (OUT/'memory.c').write_text(ARM_MEMORY)
    arm_before,arm_after=ArmFixture('baseline'),ArmFixture('candidate')
    arm_cases=[]
    for fmt in (21,22):
        for owner in (0,1,2):
            arm_cases.append([fmt,256,1,12,owner,100,7,0,0,0,0,0,0,0,0,0])
    for fmt in (21,22,23,116):
        for owner in (0,1,2):
            for scale in (50,100):
                for fail in (0,1,3):
                    for logical in (0,1):
                        arm_cases.append([fmt,17,9,12,owner,scale,41,fail,logical,3,1,7,5,0,1,0])
    instruction_counts=None
    for i,p in enumerate(arm_cases):
        a,ai=arm_before.execute(p,count=i==0)
        b,bi=arm_after.execute(p,count=i==0)
        assert a[:2]==b[:2],('ARM output',p)
        assert a[2][3:]==b[2][3:] and a[3]==b[3],('ARM state/GL calls',p)
        assert a[2][1]==b[2][1]==0,('ARM leak',p)
        assert a[1]==execute(before,p)[1],('host/ARM baseline output',p)
        if p[0] in (21,22) and not p[8]:assert a[1]==expected_bytes(p),('ARM byte reference',p)
        if i==0:instruction_counts={'fixture':'256x1 format21 standalone, mock GL+libc included',
                                     'before':ai,'after':bi,'device_fps_implication':None}
    # Compile the real complete production TU for Android as an integration check.
    cmd=[str(B.CLANG),*B.HOST_FLAGS,'-DVOID_ANDROID_APP','-c','android/host/d3d9_gl.c',
         '-o',str(OUT/'d3d9_gl.o')]
    compiled=subprocess.run(cmd,cwd=B.ROOT,capture_output=True,text=True)
    (OUT/'compile.txt').write_text(compiled.stdout+compiled.stderr)
    assert compiled.returncode==0,compiled.stderr
    result={'status':'PASS','actual_c_differential_cases':compared,
        'candidate_allocation_failure_cases':oom_cases,
        'actual_arm64_differential_cases':len(arm_cases),
        'arm64_fixture_instructions':instruction_counts,
        'reference_sha256':hashlib.sha256(REFERENCE.read_bytes()).hexdigest(),
        'production_sha256':hashlib.sha256((B.ROOT/'android/host/d3d9_gl.c').read_bytes()).hexdigest(),
        'allocation_peaks':peaks,'allocation_metric':'simultaneously live requested malloc payload; excludes allocator headers',
        'coverage':['all256 channel values','A8/X8 alpha','ordinary texture/render target/standalone',
                    'padded pitch and canaries','odd rect and row origin','scaled resolve GL call trace',
                    'incomplete framebuffer/full/partial/no-write GL error','logical float and nonbyte fallbacks',
                    'GL FBO restore/dirty/cache/unit/error state','allocation failure cleanup'],
        'android_complete_tu_compile':'PASS','device_performance_measured':False}
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    line=f'PASS: {compared} actual-C host + {len(arm_cases)} ARM64 differential cases, {oom_cases} OOM cases; 1334x600 peak 16008000 -> 3201600 bytes (80% lower)'
    (OUT/'result.txt').write_text(line+'\n')
    print(line)


if __name__=='__main__':
    main()
