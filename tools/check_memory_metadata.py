"""Check actual VM C metadata operations and ARM64 BSS without adb/full builds.

    python tools/check_memory_metadata.py
    python tools/check_memory_metadata.py --baseline path/to/old/memory.c

The host fixture stubs only OS mapping/protection calls, not VM bookkeeping.
Optional old-source comparison checks identical results, queries and syscalls.
Touched-page estimates exclude test resets and are not device RSS measurements.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/memory-metadata'
PRELUDE = r'''
typedef unsigned char uint8_t;
#define UINT8_MAX 255
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t, uintptr_t, size_t;
typedef long long off_t;
typedef unsigned int pthread_mutex_t;
#define NULL ((void *)0)
#define PTHREAD_MUTEX_INITIALIZER 0
#define VOID_GUEST_BASE 0x100000000ull
#define VOID_GUEST_SPAN 0x100000000ull
#define GUEST_PAGE 0x1000u
#define GUEST_GRANULARITY 0x10000u
#define GUEST_THUNK_BASE 0x7FF00000u
#define GUEST_THUNK_LIMIT 0x7FFD0000u
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 32
#define MAP_NORESERVE 16384
#define MAP_FIXED 16
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4
#define ERROR_INVALID_PARAMETER_ 87
#define ERROR_INVALID_ADDRESS_ 487
#define ERROR_NOT_ENOUGH_MEMORY_ 8
#define ERROR_ACCESS_DENIED_ 5
static uint32_t words[64], calls[8], error, fail_map;
static int errno;
#define GPTR(a) ((uint8_t *)((uintptr_t)words + (uint32_t)(a)))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM16(a) (*(uint16_t *)GPTR(a))
#define ARG(n) 0u
#define WINAPI_EXPORT(d,n,c) __declspec(dllexport) uint32_t test_##n(void)
#define _SC_NPROCESSORS_CONF 1
#define _SC_PAGESIZE 2
static long sysconf(int n) { return n == _SC_PAGESIZE ? 4096 : 8; }
static void void_log(const char *s, ...) { (void)s; }
static const char *strerror(int e) { return "fixture error"; }
static void void_set_last_error(uint32_t e) { error=e; }
static void pthread_mutex_lock(pthread_mutex_t *p) { (void)p; }
static void pthread_mutex_unlock(pthread_mutex_t *p) { (void)p; }
static void *memset(void *p, int c, size_t n) {
    volatile uint8_t *b=p; while(n--) *b++=(uint8_t)c; return p;
}
static int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x=a,*y=b; while(n--) { if(*x!=*y) return *x-*y; ++x;++y; } return 0;
}
static void *mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
    ++calls[0];calls[1]=(uint32_t)n;calls[2]=(uint32_t)flags;
    if(fail_map) { fail_map=0;return MAP_FAILED; } return p;
}
static int munmap(void *p,size_t n) { return 0; }
static int mprotect(void *p,size_t n,int prot) {
    ++calls[3];calls[4]=(uint32_t)n;calls[5]=(uint32_t)prot;return 0;
}
static int madvise(void *p,size_t n,int advice) {
    ++calls[6];calls[7]=(uint32_t)n;return 0;
}
'''
WRAPPERS = r'''
__declspec(dllexport) uint32_t *test_words(void) { return words; }
__declspec(dllexport) void test_reset(void) {
    memset(page_state,0,sizeof page_state);memset(page_protect,0,sizeof page_protect);
    memset(page_alloc,0,sizeof page_alloc);memset(alloc_protect,0,sizeof alloc_protect);
    memset(granule_used,0,sizeof granule_used);memset(words,0,sizeof words);
    error=fail_map=0;void_guest_reserve();memset(calls,0,sizeof calls);
}
__declspec(dllexport) uint32_t test_alloc(uint32_t a,uint32_t n,uint32_t t,uint32_t p) { return void_vm_alloc(a,n,t,p); }
__declspec(dllexport) uint32_t test_claim(uint32_t a,uint32_t n,uint32_t p) { return void_vm_claim(a,n,p); }
__declspec(dllexport) uint32_t test_free(uint32_t a,uint32_t n,uint32_t t) { return void_vm_free(a,n,t); }
__declspec(dllexport) uint32_t test_protect(uint32_t a,uint32_t n,uint32_t p) { return void_vm_protect(a,n,p,words+30); }
__declspec(dllexport) uint32_t test_query(uint32_t a) { return void_vm_query(a,64); }
__declspec(dllexport) uint32_t test_map(uint32_t n,uint32_t writable,uint32_t fail) { fail_map=fail;return void_vm_map_file(7,0,n,writable,0); }
__declspec(dllexport) uint32_t test_unmap(uint32_t a) { return void_vm_unmap_file(a); }
__declspec(dllexport) uint32_t test_error(void) { return error; }
__declspec(dllexport) void test_calls(void) { for(int i=0;i<8;++i) words[32+i]=calls[i]; }
__declspec(dllexport) uint32_t test_bytes(void) {
    return sizeof page_state+sizeof page_protect+sizeof page_alloc+sizeof alloc_protect+sizeof granule_used;
}
__declspec(dllexport) uint32_t test_walk(void) {
    test_GetSystemInfo();uint32_t a=words[2],maximum=words[3],count=0;
    while(a<=maximum) {
        if(count++>4096||!void_vm_query(a,64))return 0;
        uint32_t next=a+words[19];if(next<=a)return 0;a=next;
    }
    return a==GUEST_USER_TOP;
}
'''


def build_fixture(path, label):
    text = path.read_text(encoding='utf-8')
    header = (B.ROOT/'android/host/host.h').read_text(encoding='utf-8')
    top = re.search(r'^#define GUEST_USER_TOP (\S+)', header, re.M)[1]
    vm = text[text.index('#define PAGES'):text.index('/* ---------------------------------------------------------------- heaps */')]
    kernel = (B.ROOT/'android/host/kernel32.c').read_text(encoding='utf-8')
    source, obj, dll = (OUT/(label+s) for s in ('.c', '.obj', '.dll'))
    source.write_text(PRELUDE+'\n#define GUEST_USER_TOP '+top+'\n'+vm+
                      function(kernel,'WINAPI_EXPORT(K32, GetSystemInfo, 1)')+WRAPPERS)
    subprocess.run([str(B.CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin',
                    '-fno-stack-protector','-c',str(source),'-o',str(obj)],check=True)
    subprocess.run([str(B.CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry',
                    '/out:'+str(dll),str(obj)],check=True)
    lib=ctypes.CDLL(str(dll));lib.test_words.restype=ctypes.POINTER(ctypes.c_uint32)
    for name,n in [('alloc',4),('claim',3),('free',3),('protect',3),('query',1),('map',3),('unmap',1)]:
        getattr(lib,'test_'+name).argtypes=[ctypes.c_uint32]*n
        getattr(lib,'test_'+name).restype=ctypes.c_uint32
    return lib


def exercise(lib):
    rows=[];words=lib.test_words()
    def op(name,*args,expected=None,query=()):
        value=getattr(lib,'test_'+name)(*args)
        if expected is not None:assert value==expected,(name,args,hex(value),expected)
        row=[name,args,value,lib.test_error()]
        for a in query:
            got=lib.test_query(a);row.append((a,got,list(words[16:23])))
        lib.test_calls();row.append(list(words[32:40]));rows.append(row)
    lib.test_reset()
    op('claim',0x11000,0x1000,4,expected=0)  # rejects non-granular bases
    op('claim',0,0x1000,4,expected=0)       # null sentinel stays unavailable
    op('claim',0x10000,0x23000,4,expected=1,query=[0x10001,0x32fff])
    op('free',0x11000,0,0x8000,expected=0) # interior page must NOT release a granule
    op('protect',0x12001,0x2fff,2,expected=1,query=[0x12001,0x15000])
    assert words[30]==4
    op('free',0x13001,100,0x4000,expected=1,query=[0x13000,0x14000])
    assert list(words[32+6:32+8])==[1,4096]
    op('alloc',0x13000,4096,0x1000,4,expected=0x13000,query=[0x13000])
    op('free',0x22000,0,0x4000,expected=1,query=[0x10000,0x32000])
    assert words[19]==4096 and words[20]==0x2000 and words[21]==0
    op('alloc',0x12000,4096,0x1000,4,expected=0x12000)
    op('free',0x10000,0,0x8000,expected=1,query=[0x10000])
    op('alloc',0,0x10000,0x3000,4,expected=0x10000)
    op('alloc',0,0x10000,0x3000,4,expected=0x20000,query=[0x10000,0x20000])
    assert words[17]==0x20000
    op('free',0x10000,0,0x8000,expected=1)
    op('free',0x20000,0,0x8000,expected=1)
    op('alloc',0,4096,0x103000,0x102,expected=0xfffd0000,query=[0xfffd0000])
    assert list(words[16:23])==[0xfffd0000,0xfffd0000,0x102,4096,0x1000,0x102,0x20000]
    op('free',0xfffd1000,0,0x8000,expected=0)
    op('free',0xfffd0000,0,0x4000,expected=1,query=[0xfffd0000])
    op('free',0xfffd0000,0,0x8000,expected=1)
    op('map',0x5000,0,0,expected=0x10000,query=[0x10000])
    assert words[18]==2 and words[21]==2
    op('map',0x11000,1,0,expected=0x20000,query=[0x20000])
    op('unmap',0x10000,expected=1)
    op('unmap',0x20000,expected=1)
    op('map',4096,0,1,expected=0,query=[0x10000])
    op('alloc',0,4096,0x3000,4,expected=0x10000) # failed map released metadata
    rng=random.Random(0x64c0)
    for i in range(128):
        base=(i*3+8)<<16;n=rng.randrange(1,33)*4096;protect=rng.choice([2,4,0x104])
        op('alloc',base,n,0x3000,protect,expected=base,query=[base,base+n-1])
        op('free',base+rng.randrange(n//4096)*4096,0,0x4000,expected=1,query=[base])
        op('free',base,0,0x8000,expected=1,query=[base])
    op('walk',expected=1)
    return rows


def touched_pages(ranges, compact):
    # Unique 4 KiB metadata pages written by set_pages/reserve_exact, with each
    # array considered separately. Anonymous BSS pages stay demand allocated.
    arrays=[set() for _ in range(5)]
    for address,size in ranges:
        first=address>>12;last=(address+size+4095)>>12
        for target,width in zip(arrays[:3],(1,2,2 if compact else 4)):
            target.update(range(first*width//4096,((last*width-1)//4096)+1))
        index=address>>16 if compact else first
        arrays[3].add(index*2//4096)
        arrays[4].update(range((address>>16)//4096,(((address+size+65535)>>16)-1)//4096+1))
    return [len(x) for x in arrays]


def arm_bss(path,label):
    # Keep the production TU intact; only relocate its relative dlmalloc include.
    source=OUT/(label+'.arm.c');obj=OUT/(label+'.arm.o')
    text=path.read_text(encoding='utf-8').replace('"../../reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c"',
                                 '"'+(B.ROOT/'reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c').as_posix()+'"')
    source.write_text(text)
    subprocess.run([str(B.CLANG)]+B.HOST_FLAGS+['-c',str(source),'-o',str(obj)],cwd=B.ROOT,check=True)
    data=subprocess.check_output([str(B.CLANG.with_name('llvm-size.exe')),'-A',str(obj)],text=True)
    (OUT/(label+'.sections.txt')).write_text(data)
    return sum(int(n) for n in re.findall(r'^\.bss\S*\s+(\d+)\s',data,re.M))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,default=OUT/'memory.before.c')
    args=parser.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    current=B.ROOT/'android/host/memory.c';paths=[('current',current)]
    if args.baseline.is_file():paths.insert(0,('before',args.baseline.resolve()))
    libs=[];results={};baseline=None
    try:
        for label,path in paths:
            lib=build_fixture(path,label);libs.append(lib);rows=exercise(lib)
            if baseline is None:baseline=rows
            else:assert rows==baseline,'actual-C result/query/syscall differential mismatch'
            results[label]={'metadata_bytes':lib.test_bytes(),'arm_tu_bss_bytes':arm_bss(path,label),
                            'operations':len(rows),'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        # One contiguous 1 GiB claim and128 sparse64KiB claims. These are explicit
        # hypothetical write-coverage models, not estimates of Sims2's live heap.
        for label,ranges in [('contiguous_1gib',[(0x1000000,1<<30)]),
                             ('sparse_128',[(0x10000+i*0x100000,0x10000) for i in range(128)])]:
            results[label]={key:touched_pages(ranges,compact) for key,compact in [('before',False),('current',True)]}
        results['limits']='OS mapping calls are stubbed; BSS is actual ARM64 object size. Touch models exclude reset/OS residency and are not device RSS or FPS measurements.'
        (OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps(results,indent=2))
        print('PASS actual-C VM/query/syscall checks, finite user-top walk and ARM64 TU compilation; no adb/full build')
    finally:
        ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p]
        for lib in libs:ctypes.windll.kernel32.FreeLibrary(lib._handle)


if __name__=='__main__':main()
