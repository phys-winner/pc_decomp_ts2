"""Check original query selection/synchronization and actual host EVENT fence status."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

from build_android import ROOT, CLANG
from check_virtual_query import function


def original_paths():
    pe = pefile.PE(str(ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(image) + 65535) & ~65535); uc.mem_write(base, image)
    uc.mem_map(0x10000, 0x30000); uc.mem_map(0x60000000, 0x1000)
    obj, enum, dev, query, surface, state = 0x10000, 0x11000, 0x12000, 0x13000, 0x14000, 0x18000
    table, frame, stack, stop = 0x19000, 0x20000, 0x2F000, 0x60000000
    rd = lambda a: int.from_bytes(uc.mem_read(a, 4), 'little')
    wr = lambda a, v: uc.mem_write(a, struct.pack('<I', v))
    calls = []
    supported = True
    statuses = []

    def callback(c, address, size, user):
        nonlocal statuses
        if address not in (stop + 0x100, stop + 0x110, stop + 0x120, stop + 0x130,
                            stop + 0x140, 0xA017CDF0):
            return
        sp = c.reg_read(X.UC_X86_REG_ESP)
        if address == stop + 0x100:  # CreateQuery(EVENT)
            assert rd(sp + 4) == dev and rd(sp + 8) == 8
            calls.append('create_event'); wr(rd(sp + 12), query if supported else 0)
            result, pop = (0 if supported else 0x8876086A), 16
        elif address == stop + 0x110:  # fallback CreateRenderTarget(16,16,...,lockable=1)
            assert rd(sp + 4) == dev and rd(sp + 8) == rd(sp + 12) == 16
            assert rd(sp + 28) == 1
            calls.append('create_pixels'); wr(rd(sp + 32), surface); result, pop = 0, 40
        elif address == stop + 0x120:  # device IsInitialized
            result, pop = 1, 4
        elif address == stop + 0x130:  # EVENT Issue(END)
            assert rd(sp + 4) == query and rd(sp + 8) == 1
            calls.append('issue_end'); result, pop = 0, 12
        elif address == stop + 0x140:  # EVENT GetData(BOOL*,4,FLUSH)
            assert rd(sp + 4) == query and rd(sp + 12) == 4 and rd(sp + 16) == 1
            calls.append('poll'); result, pop = statuses.pop(0), 20
        else:
            calls.append('flush_state'); result, pop = 0, 8
        c.reg_write(X.UC_X86_REG_EAX, result)
        c.reg_write(X.UC_X86_REG_ESP, sp + pop); c.reg_write(X.UC_X86_REG_EIP, rd(sp))

    uc.hook_add(UC_HOOK_CODE, callback)
    wr(obj, table); wr(table + 0x8C, stop + 0x120)
    wr(obj + 0x38, dev); wr(obj + 0x3C, enum); wr(obj + 0x44, state)
    wr(enum, 0xA0D9F758); wr(dev, table + 0x400)
    wr(table + 0x400 + 0x1D8, stop + 0x100); wr(table + 0x400 + 0x70, stop + 0x110)
    wr(query, table + 0x800); wr(table + 0x800 + 0x18, stop + 0x130)
    wr(table + 0x800 + 0x1C, stop + 0x140)
    wr(frame - 0x7C, enum); wr(0xA0D9F758 + 0xC, stop + 0x120)
    cases = 0
    for enabled, supported in ((False, True), (True, True), (True, False)):
        calls.clear(); wr(obj + 0x60, 0); wr(obj + 0x64, 0)
        uc.mem_write(enum + 0x770, bytes([enabled]))
        uc.reg_write(X.UC_X86_REG_EDI, obj); uc.reg_write(X.UC_X86_REG_EBP, frame)
        uc.reg_write(X.UC_X86_REG_ESP, stack)
        uc.emu_start(0xA01690C8, 0xA0169196, count=2000)
        expected = ['create_event'] if enabled and supported else (['create_event', 'create_pixels'] if enabled else ['create_pixels'])
        assert calls == expected, (enabled, supported, calls)
        assert rd(obj + 0x60) == (query if enabled and supported else 0)
        assert rd(obj + 0x64) == (0 if enabled and supported else surface)
        cases += 1
    for results in ([1, 1, 0], [0], [0x8876086A]):
        calls.clear(); statuses = list(results); wr(obj + 0x60, query); wr(obj + 0x64, 0)
        uc.mem_write(obj + 0x71, b'\0'); wr(stack, stop)
        uc.reg_write(X.UC_X86_REG_ESP, stack); uc.reg_write(X.UC_X86_REG_ECX, obj)
        uc.emu_start(0xA0169740, stop, count=2000)
        assert calls == ['flush_state', 'issue_end'] + ['poll'] * len(results), calls
        assert not statuses and uc.reg_read(X.UC_X86_REG_ESP) == stack + 4
        cases += 1
    print(f'PASS {cases} original x86 query/fallback and synchronous wait paths')


def host_status():
    source = (ROOT / 'android/host/d3d9_resource.c').read_text()
    prelude = r'''
typedef unsigned int uint32_t;
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef struct {uint32_t type,ended;void *fence;} d3d_query;
typedef struct {d3d_query *q;int wait,ready;GLuint any;} result_args;
#define GL_SYNC_FLUSH_COMMANDS_BIT 1
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_QUERY_RESULT_AVAILABLE 1
#define GL_QUERY_RESULT 2
#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define S_FALSE_D3D 1u
#define FOURCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
static uint32_t words[16],args[4],status,waits,syncs,bad,pending_ns;
static d3d_query query;
#define ARG(n) args[n]
#define MEM32(a) words[(a)/4]
#define QRY (&query)
#define COM_METHOD(p,n,c) static uint32_t p##_##n(void)
static GLenum glClientWaitSync(void *f,uint32_t flags,unsigned long long timeout){
    uint32_t flush=args[3]&1u;
    ++waits;if(f!=(void *)1 || flags!=flush || timeout!=(flush?1000000ull:0))bad=1;
    return status==GL_TIMEOUT_EXPIRED&&pending_ns&&timeout>=pending_ns?GL_CONDITION_SATISFIED:status;
}
static void glGetQueryObjectuiv(uint32_t q,GLenum key,GLuint *out){(void)q;(void)key;*out=1;}
static void d3d_rt_sync(void (*f)(void *),void *p){++syncs;f(p);}
'''
    # Unused occlusion branches still need their exact field names to compile.
    prelude = prelude.replace('uint32_t type,ended;', 'uint32_t type,ended,gl;struct {struct {uint32_t viewport[4];} st;} *dev;')
    actual = function(source, 'static void run_query_result(') + function(source, 'static uint32_t query_getdata_sync(')
    check = r'''
__declspec(dllexport) uint32_t check(uint32_t s,uint32_t fence,uint32_t ended,uint32_t out,uint32_t size,uint32_t flags,uint32_t pending){
    query.type=8;query.fence=fence?(void *)1:0;query.ended=ended;status=s;waits=syncs=bad=0;
    pending_ns=pending;
    for(int i=0;i<16;++i)words[i]=0xA5A5A5A5u;
    args[1]=out;args[2]=size;args[3]=flags;uint32_t result=query_getdata_sync(&query,out,size,flags);
    if(s==GL_TIMEOUT_EXPIRED&&pending&&(flags&1u)&&pending<=1000000)s=GL_CONDITION_SATISFIED;
    uint32_t expected=!ended?0:!fence||s==GL_WAIT_FAILED?D3DERR_NOTAVAILABLE:s==GL_TIMEOUT_EXPIRED?1:0;
    if(result!=expected || bad || waits!=(ended&&fence) || syncs!=ended)return 1;
    for(int i=0;i<16;++i){uint32_t value=!result&&out&&size>=4&&(uint32_t)i==out/4?1:0xA5A5A5A5u;
        if(words[i]!=value)return 2;}
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-event-query-') as directory:
        path = Path(directory); cfile, obj, dll = path/'check.c', path/'check.obj', path/'check.dll'
        cfile.write_text(prelude + actual + check)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:'+str(dll), str(obj)], check=True)
        library = ctypes.CDLL(str(dll)); library.check.argtypes = [ctypes.c_uint32]*7
        cases = 0
        try:
            for status in (0x911A, 0x911B, 0x911C, 0x911D):
                for fence in (0, 1):
                    for ended in (0, 1):
                        for out, size in ((0,0), (4,0), (4,3), (4,4), (4,16)):
                            for flags in (0, 1):
                                assert library.check(status,fence,ended,out,size,flags,0) == 0, (status,fence,ended,out,size,flags)
                                cases += 1
            for pending in (500000, 1000000, 2000000):
                for flags in (0, 1):
                    assert library.check(0x911B,1,1,4,4,flags,pending) == 0, (flags,pending)
                    cases += 1
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(library._handle)
    print(f'PASS {cases} actual-C EVENT fence timeout/signal/failure/unissued/output cases; '
          'FLUSH requests1ms wait, zero flags remain nonblocking, pending fence may signal during the bounded wait')


if __name__ == '__main__':
    original_paths()
    host_status()
