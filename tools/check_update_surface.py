"""Execute actual UpdateSurface/row-snapshot C with controlled queue/GL boundaries."""
import ctypes
import hashlib
import io
import json
import random
import subprocess
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/update-surface'
OUT.mkdir(parents=True, exist_ok=True)
PRELUDE = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t, size_t;
#define UINT32_MAX 0xFFFFFFFFu
#define NULL ((void*)0)
#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DPOOL_DEFAULT 0
#define D3DPOOL_SYSTEMMEM 2
#define D3DPOOL_SCRATCH 3
#define D3DUSAGE_RENDERTARGET 1
void *memcpy(void*,const void*,size_t);
void *memset(void*,int,size_t);
int memcmp(const void*,const void*,size_t);
typedef struct { uint32_t bits,block_bytes,internal,depth; } d3d_format;
typedef struct { uint32_t levels,*sysmem;int has_storage;const d3d_format *fmt; } d3d_texture;
typedef struct { void *dev;uint32_t width,height,format,usage,pool,multisample;const d3d_format *fmt;
    d3d_texture *container;uint32_t face,level,sysmem,pitch;int locked;uint32_t lock_mem,lock_rect[4];int lock_whole; } d3d_surface;
static uint8_t guest[1048576],pending[131072];
#define GPTR(a) (guest+(uint32_t)(a))
#define MEM32(a) (*(uint32_t*)GPTR(a))
typedef struct { d3d_surface *s;uint32_t x0,y0,x1,y1,pitch; } surface_upload_args;
static uint32_t metrics[20];
static const uint8_t *recorded;
static int deny_memory;
static void void_log(const char *f,...) { (void)f; }
static int void_vm_readable(uint32_t a,uint32_t n) { return !deny_memory&&a<sizeof guest&&n<=sizeof guest-a; }
static void run_surface_upload(void *p) { (void)p; }
static void *d3d_rt_begin(void (*f)(void*),uint32_t n) { (void)f;if(n>sizeof pending)__builtin_trap();memset(pending,0xCC,sizeof pending);return pending; }
static void d3d_rt_commit(void) {
    surface_upload_args *a=(surface_upload_args*)pending;
    metrics[1]=a->x0;metrics[2]=a->y0;metrics[3]=a->x1-a->x0;metrics[4]=a->y1-a->y0;metrics[5]=a->pitch;
    metrics[6]=a->s->fmt->block_bytes?(metrics[4]+3)/4:metrics[4];
    metrics[7]=a->pitch*metrics[6];++metrics[8];metrics[9]=1;recorded=(uint8_t*)(a+1);
}
static void d3d_texture_upload_rect(d3d_texture *t,uint32_t face,uint32_t level,uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint8_t *data,uint32_t pitch) {
    uint32_t row=t->fmt->block_bytes?((w+3)/4)*t->fmt->block_bytes:(w*t->fmt->bits+7)/8;
    uint32_t rows=t->fmt->block_bytes?(h+3)/4:h;
    metrics[1]=x;metrics[2]=y;metrics[3]=w;metrics[4]=h;metrics[5]=row;metrics[6]=rows;metrics[7]=row*rows;
    ++metrics[8];metrics[9]=2;metrics[10]=face;metrics[11]=level;
    for(uint32_t i=0;i<rows;++i)memcpy(pending+i*row,data+i*pitch,row);recorded=pending;
}
'''
WRAPPER = r'''
__declspec(dllexport) uint32_t metric(uint32_t n) { return metrics[n]; }
__declspec(dllexport) void take(uint8_t *p) { if(metrics[7])memcpy(p,recorded,metrics[7]); }
__declspec(dllexport) uint32_t run(const uint32_t *p) {
    memset(metrics,0,sizeof metrics);memset(guest+0x1000,0xAC,32);recorded=NULL;deny_memory=0;
    static const uint32_t formats[8][3]={{21,32,0},{22,32,0},{20,24,0},{23,16,0},{111,16,0},{0x31545844,4,8},{0x35545844,8,16},{0x31495441,4,8}};
    d3d_format f={formats[p[0]][1],formats[p[0]][2],1,0};
    uint32_t pitch=d3d_format_pitch(&f,p[1])+p[5],mem=0x10000;
    uint32_t memory[4]={0,mem,0,0};
    d3d_texture st={2,memory,0,&f},dt={2,NULL,1,&f};
    d3d_surface s={(void*)0x1234,p[1],p[2],formats[p[0]][0],0,2,0,&f,NULL,0,0,mem,pitch,0,0,{0},0};
    d3d_surface d={(void*)0x1234,p[3],p[4],formats[p[0]][0],0,0,0,&f,NULL,0,1,0,d3d_format_pitch(&f,p[3])+p[6],0,0xFADE,{0},0};
    if(p[13]&4) { s.container=&st;s.sysmem=0;s.level=1; }
    if(p[13]&8) { d.container=&dt;d.face=3; }
    if(p[13]&16) { d.lock_whole=0;d.lock_rect[0]=4;d.lock_rect[1]=4;d.lock_rect[2]=8;d.lock_rect[3]=8; }
    for(uint32_t i=0;i<pitch*d3d_format_rows(&f,p[2]);++i)guest[mem+i]=(uint8_t)(i*37+13);
    for(unsigned i=0;i<4;++i)MEM32(0x1000+i*4)=p[7+i%2]+(i>=2?p[9+i%2]:0);
    MEM32(0x1100)=p[11];MEM32(0x1104)=p[12];
    uint32_t rect=p[13]&1?0:0x1000,point=p[13]&2?0:0x1100;
    switch(p[14]) {
        case 1:s.pool=0;break;case 2:d.pool=2;break;case 3:s.locked=1;break;case 4:d.locked=1;break;
        case 5:s.multisample=1;break;case 6:d.multisample=1;break;case 7:d.format=22^s.format;break;
        case 8:f.depth=1;break;case 9:d.dev=(void*)0x5678;break;case 10:s.sysmem=0;s.container=NULL;break;
        case 11:f.internal=0;break;case 12:dt.has_storage=0;d.container=&dt;break;
        case 13:deny_memory=1;break;case 14:s.pitch=1;break;case 15:s.sysmem=0xFFFFFFF0u;break;
        case 16:s.fmt=NULL;break;case 17:d.fmt=NULL;break;case 18:rect=sizeof guest-8;break;
        case 19:point=sizeof guest-4;break;case 20:s.usage=1;break;
    }
    d3d_surface old_s=s,old_d=d;
#ifdef BASELINE
    d3d_update_surface(&s,rect,&d,point);metrics[0]=0;
#else
    metrics[0]=d3d_update_surface(&s,rect,p[14]==21?&s:&d,point);
#endif
    metrics[12]=!memcmp(&s,&old_s,sizeof s)&&!memcmp(&d,&old_d,sizeof d);
    memset(guest+mem,0xDD,pitch*d3d_format_rows(&f,p[2]));
    return metrics[0];
}
'''

def build(name, code, library):
    src, obj, dll = (OUT / (name + suffix) for suffix in ('.c', '.obj', '.dll'))
    src.write_text(code)
    subprocess.run([str(B.CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-stack-protector',
                    '-c', str(src), '-o', str(obj)], check=True)
    subprocess.run([str(B.CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                    '/out:' + str(dll), str(obj), str(library)], check=True)
    loaded = ctypes.CDLL(str(dll))
    loaded.run.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
    loaded.run.restype = loaded.metric.restype = ctypes.c_uint32
    loaded.take.argtypes = [ctypes.c_void_p]
    return loaded

resource = (B.ROOT / 'android/host/d3d9_resource.c').read_text()
fmt = (B.ROOT / 'android/host/d3d9_format.c').read_text()
common = PRELUDE + function(fmt, 'uint32_t d3d_format_pitch(') + function(fmt, 'uint32_t d3d_format_rows(')
reference = (B.ROOT / 'tools/fixtures/update_surface_reference.c').read_text()
candidate = ''.join(function(resource, n) for n in ('static void surface_upload_data(',
    'static void surface_upload_rect(', 'void d3d_surface_upload(', 'uint32_t d3d_update_surface('))
definition, library = OUT / 'crt.def', OUT / 'crt.lib'
definition.write_text('LIBRARY msvcrt.dll\nEXPORTS\nmemcpy\nmemset\nmemcmp\n')
subprocess.run([str(B.CLANG.with_name('llvm-dlltool.exe')), '-m', 'i386:x86-64', '-d', str(definition), '-l', str(library)], check=True)
before = build('before', '#define BASELINE\n' + common + reference + WRAPPER, library)
after = build('after', common + candidate + WRAPPER, library)

# The same actual C and boundary stubs also execute as AArch64 machine code.
memory = r'''
void *memcpy(void *d,const void *s,size_t n) { volatile uint8_t *p=d;const volatile uint8_t *q=s;for(size_t i=0;i<n;++i)p[i]=q[i];return d; }
void *memset(void *d,int c,size_t n) { volatile uint8_t *p=d;for(size_t i=0;i<n;++i)p[i]=(uint8_t)c;return d; }
int memcmp(const void *a,const void *b,size_t n) { const uint8_t *p=a,*q=b;for(size_t i=0;i<n;++i)if(p[i]!=q[i])return p[i]-q[i];return 0; }
'''
arm_source, arm_binary = OUT / 'after-arm.c', OUT / 'after.elf'
arm_source.write_text((common + candidate + WRAPPER + memory).replace('__declspec(dllexport)', ''))
subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                '-Wl,--gc-sections,-e,run,-u,metric,-u,take', str(arm_source), '-o', str(arm_binary)], check=True)
elf = ELFFile(io.BytesIO(arm_binary.read_bytes()))
symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, arena, stack, stop = 0x10000000, 0x20000000, 0x30000000, 0x40000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
for at,size in ((base,0x400000),(arena,0x20000),(stack,0x10000),(stop,0x1000)): arm.mem_map(at,size)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD': arm.mem_write(base+segment['p_vaddr'],segment.data())
relocs = elf.get_section_by_name('.rela.dyn')
if relocs:
    for r in relocs.iter_relocations():
        assert r['r_info_type']==1027
        arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))

def arm_call(name,arg):
    arm.reg_write(A.UC_ARM64_REG_X0,arg)
    arm.reg_write(A.UC_ARM64_REG_SP,stack+0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols[name],stop,count=2000000)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop,name
    return arm.reg_read(A.UC_ARM64_REG_X0)&0xFFFFFFFF

def execute(dll, p):
    result = dll.run((ctypes.c_uint32 * 15)(*p))
    metrics = [dll.metric(i) for i in range(13)]
    out = ctypes.create_string_buffer(metrics[7])
    dll.take(out)
    if dll is after:
        arm.mem_write(arena,struct.pack('<15I',*p))
        actual_result=arm_call('run',arena)
        actual_metrics=[arm_call('metric',i) for i in range(13)]
        arm_call('take',arena+0x1000)
        actual_data=bytes(arm.mem_read(arena+0x1000,metrics[7]))
        assert (actual_result,actual_metrics,actual_data)==(result,metrics,out.raw),(p,'ARM versus host')
    return result, metrics, out.raw

basic = [0,8,8,8,8,8,4,0,0,8,8,0,0,3,0]
old = execute(before, basic)
assert old[0] == 0 and old[1][8] == 0, old  # False success, zero uploads on a fresh destination.
new = execute(after, basic)
assert new[0] == 0 and new[1][8] == 1 and new[1][12], new
rng = random.Random(0xD3D90020)
cases = 0
for fmt_id in range(8):
    bits, block = ((32,0),(32,0),(24,0),(16,0),(16,0),(4,8),(8,16),(4,8))[fmt_id]
    for index in range(80):
        w,h = rng.randrange(1,7)*4,rng.randrange(1,7)*4
        sx,sy = rng.randrange(w//4)*4,rng.randrange(h//4)*4
        sw,sh = rng.randrange(1,(w-sx)//4+1)*4,rng.randrange(1,(h-sy)//4+1)*4
        dw,dh = w+8,h+8;dx,dy=rng.randrange(3)*4,rng.randrange(3)*4
        flags = (4 if index&1 else 0)|(8 if index&2 else 0)|(16 if index&4 else 0)
        if index % 7 == 0: flags|=1;sx=sy=0;sw=w;sh=h
        if index % 11 == 0: flags|=2;dx=dy=0
        p=[fmt_id,w,h,dw,dh,rng.randrange(4)*8,rng.randrange(4)*8,sx,sy,sw,sh,dx,dy,flags,0]
        result,m,data=execute(after,p)
        row=((sw+3)//4)*block if block else sw*bits//8
        pitch=((w+3)//4)*block+p[5] if block else w*bits//8+p[5]
        rows=(sh+3)//4 if block else sh
        offset=(sy//4)*pitch+(sx//4)*block if block else sy*pitch+sx*bits//8
        expected=bytes(((offset+r*pitch+i)*37+13)&255 for r in range(rows) for i in range(row))
        assert result==0 and m[1:7]==[dx,dy,sw,sh,row,rows] and m[8]==1 and m[12],(p,m)
        assert data==expected,(p,'snapshot bytes')
        if flags&8: assert m[9:12]==[2,3,1],m
        else: assert m[9]==1,m
        cases+=1

# Small compressed mip tails, bounded at both source and destination edges.
for fmt_id in (5,6,7):
    for n in (1,2,3,5,7):
        p=[fmt_id,n,n,n,n,8,4,0,0,n,n,0,0,3,0]
        result,m,data=execute(after,p)
        assert result==0 and m[8]==1 and m[3:5]==[n,n] and m[12],(p,m)
        cases+=1

for failure in range(1,22):
    p=basic.copy();p[14]=failure
    result,m,data=execute(after,p)
    assert result==0x8876086C and m[8]==0 and m[12],(p,m)
    cases+=1
for field,value in ((7,0xFFFFFFFF),(8,0xFFFFFFFF),(9,0),(10,0),(9,9),(10,9),(11,0xFFFFFFFF),(12,0xFFFFFFFF),(11,1),(12,1)):
    p=basic.copy();p[13]=0;p[field]=value
    result,m,data=execute(after,p)
    assert result==0x8876086C and m[8]==0 and m[12],(p,m)
    cases+=1
for field in (7,8,11,12):
    p=[5,8,8,8,8,0,0,0,0,4,4,0,0,0,0];p[field]=1
    result,m,data=execute(after,p)
    assert result==0x8876086C and m[8]==0 and m[12],(p,m)
    cases+=1
receipt={'result':'PASS','cases':cases,'architectures':['Windows-x64','AArch64'],'old_fresh_surface_uploads':old[1][8],'new_fresh_surface_uploads':new[1][8],
    'candidate_c_sha256':hashlib.sha256(candidate.encode()).hexdigest(),
    'scope':'Actual C rectangle validation and queued row snapshot; GL conversion/upload and actual device visuals remain separate checks'}
(OUT/'result.json').write_text(json.dumps(receipt,indent=2))
print(f'PASS: original fresh-destination no-op reproduced; {cases} host/AArch64 copy/pitch/ROI/block/mip/validation/state/snapshot cases')
