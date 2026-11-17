"""Check the actual VM on simulated 4/16 KiB hosts; build a real Android probe.

The probe is built only. Running it on a device belongs to the caller.
"""
import ctypes
import json
from pathlib import Path
import subprocess

import build_android as B

OUT = B.ROOT / 'build/test/vm-host-pages'
SOURCE = (B.ROOT / 'android/host/memory.c').read_text()
VM = SOURCE[SOURCE.index('#define PAGES'):SOURCE.index('/* ---------------------------------------------------------------- heaps */')]
COMMON = r'''
#define VOID_GUEST_BASE 0x100000000ull
#define VOID_GUEST_SPAN 0x100000000ull
#define GUEST_PAGE 4096u
#define GUEST_GRANULARITY 65536u
#define GUEST_THUNK_BASE 0x7FF00000u
#define GUEST_THUNK_LIMIT 0x7FFD0000u
#define GUEST_USER_TOP 0xFFFE0000u
#define ERROR_INVALID_PARAMETER_ 87
#define ERROR_INVALID_ADDRESS_ 487
#define ERROR_NOT_ENOUGH_MEMORY_ 8
#define ERROR_ACCESS_DENIED_ 5
static uint32_t error;
static void void_set_last_error(uint32_t e) { error=e; }
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)GPTR(a))
#define REQUIRE(c) do { if(!(c)) return __LINE__; } while(0)
'''
FIXTURE = r'''
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t,uintptr_t,size_t;
typedef long long off_t;
#define UINT8_MAX 255
#define NULL ((void *)0)
typedef uint32_t pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
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
#define _SC_PAGESIZE 2
static int errno;
static uint32_t fixture_page=4096, fail_protect, fail_advise, protect_calls, advise_calls, invalid_calls;
static uint64_t data_zero_bytes;
static uint8_t bytes[0x40000];
static uint32_t physical_prot[0x40000/4096];
#define GPTR(a) (bytes+(uint32_t)(a))
static long sysconf(int n) { return fixture_page; }
static void void_log(const char *s,...) { }
static const char *strerror(int e) { return "fixture"; }
static void pthread_mutex_lock(uint32_t *p) { }
static void pthread_mutex_unlock(uint32_t *p) { }
static void *memset(void *p,int c,size_t n) {
    if(!c && (uintptr_t)p>=(uintptr_t)bytes && (uintptr_t)p+n<=(uintptr_t)bytes+sizeof bytes)data_zero_bytes+=n;
    uint8_t *b=p;while(n--)*b++=(uint8_t)c;return p;
}
static int memcmp(const void *p,const void *q,size_t n) { const uint8_t *a=p,*b=q;while(n--){if(*a!=*b)return *a-*b;++a;++b;}return 0; }
static int mprotect(void *p,size_t n,int prot) {
    ++protect_calls;
    uint64_t at=(uintptr_t)p-(uintptr_t)bytes;
    if((at&(fixture_page-1)) || at+n>sizeof bytes) { ++invalid_calls;errno=22;return -1; }
    if(fail_protect && !--fail_protect) { errno=12;return -1; }
    for(uint32_t q=(uint32_t)at/fixture_page;q<(at+n+fixture_page-1)/fixture_page;++q)physical_prot[q]=prot;
    return 0;
}
static int madvise(void *p,size_t n,int advice) {
    ++advise_calls;uint64_t at=(uintptr_t)p-(uintptr_t)bytes;
    if((at&(fixture_page-1)) || (n&(fixture_page-1)) || at+n>sizeof bytes) { ++invalid_calls;return -1; }
    if(fail_advise && !--fail_advise) { errno=12;return -1; }
    memset(p,0,n);return 0;
}
static void *mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
    if(n==0x100000000ull)return p;
    uint64_t at=(uintptr_t)p-(uintptr_t)bytes,rounded=(n+fixture_page-1)&~(uint64_t)(fixture_page-1);
    if((at&(fixture_page-1)) || (off&(fixture_page-1)) || at+rounded>sizeof bytes) { ++invalid_calls;return MAP_FAILED; }
    memset(p,0,rounded);for(uint32_t q=(uint32_t)at/fixture_page;q<(at+rounded)/fixture_page;++q)physical_prot[q]=prot;
    return p;
}
static int munmap(void *p,size_t n) { return 0; }
'''
CASES = r'''
static uint32_t scenarios(void) {
    uint32_t old=0;
    REQUIRE(void_guest_reserve());
    REQUIRE(void_vm_alloc(0x10000,0x10000,0x2000,4)==0x10000);
    REQUIRE(void_vm_alloc(0x11000,4096,0x1000,4)==0x11000);
    REQUIRE(MEM8(0x11000)==0);MEM8(0x11000)=0x77;
    REQUIRE(void_vm_alloc(0x12000,4096,0x1000,2)==0x12000);
#ifdef VM_FIXTURE
    REQUIRE(data_zero_bytes==0);
#endif
    REQUIRE(MEM8(0x12000)==0 && MEM8(0x11000)==0x77);
    REQUIRE(void_vm_readable(0x11000,4096) && void_vm_writable(0x11000,4096));
    REQUIRE(void_vm_readable(0x12000,4096) && !void_vm_writable(0x12000,4096));
    REQUIRE(!void_vm_readable(0x10000,4096));
    REQUIRE(void_vm_protect(0x12000,4096,4,&old) && old==2);MEM8(0x12000)=0x88;
    REQUIRE(void_vm_protect(0x12000,4096,2,&old) && old==4);
    REQUIRE(void_vm_protect(0x11000,4096,2,&old) && old==4);
    REQUIRE(void_vm_protect(0x11000,4096,0x104,&old) && old==2);
    REQUIRE(!void_vm_readable(0x11000,1) && MEM8(0x12000)==0x88);
    REQUIRE(void_vm_protect(0x11000,4096,4,&old) && old==0x104);
    REQUIRE(MEM8(0x11000)==0x77);
    REQUIRE(void_vm_free(0x11000,4096,0x4000));
    REQUIRE(!void_vm_readable(0x11000,1) && MEM8(0x12000)==0x88);
    REQUIRE(void_vm_alloc(0x11000,4096,0x1000,4)==0x11000);
#ifdef VM_FIXTURE
    REQUIRE(data_zero_bytes==4096);
#endif
    REQUIRE(MEM8(0x11000)==0 && MEM8(0x12000)==0x88);
    REQUIRE(void_vm_protect(0x12000,4096,0x104,&old));
    REQUIRE(void_vm_free(0x11000,4096,0x4000));
    REQUIRE(void_vm_alloc(0x11000,4096,0x1000,4)==0x11000);
    REQUIRE(void_vm_protect(0x12000,4096,2,&old));
    REQUIRE(MEM8(0x12000)==0x88);
    REQUIRE(void_vm_query(0x11000,0x11010)==28);
    REQUIRE(MEM32(0x11010)==0x11000 && MEM32(0x11020)==0x1000 && MEM32(0x11024)==4);
    REQUIRE(void_vm_free(0x10000,0,0x8000));
    REQUIRE(void_vm_alloc(0x10000,0x4000,0x3000,4)==0x10000);
    REQUIRE(void_vm_free(0x10000,0x4000,0x4000));
    REQUIRE(void_vm_alloc(0x11000,4096,0x1000,2)==0x11000 && MEM8(0x11000)==0);
    REQUIRE(void_vm_free(0x10000,0,0x8000));
    return 0;
}
'''
EXTRA = r'''
__declspec(dllexport) uint32_t check(uint32_t page) {
    fixture_page=page;fail_protect=protect_calls=advise_calls=invalid_calls=0;
    memset(bytes,0,sizeof bytes);memset(physical_prot,0,sizeof physical_prot);
    memset(page_state,0,sizeof page_state);memset(page_protect,0,sizeof page_protect);
    memset(page_alloc,0,sizeof page_alloc);memset(alloc_protect,0,sizeof alloc_protect);
    memset(granule_used,0,sizeof granule_used);
    data_zero_bytes=0;fail_advise=0;
    uint32_t result=scenarios();if(result)return result;
    REQUIRE(!invalid_calls);
    REQUIRE(void_vm_alloc(0x20000,0x10000,0x2000,4)==0x20000);
    fail_protect=1;
    REQUIRE(!void_vm_alloc(0x21000,4096,0x1000,4));
    REQUIRE(page_state[0x21]==ST_RESERVE && !void_vm_readable(0x21000,1));
    REQUIRE(physical_prot[0x21000/page]==0);
    REQUIRE(void_vm_alloc(0x20000,0x10000,0x1000,4)==0x20000);
    if(page==16384) {
        uint32_t old;
        fail_protect=2;
        REQUIRE(!void_vm_protect(0x20000,0x7000,2,&old));
        for(uint32_t p=0x20;p<0x30;++p) REQUIRE(page_protect[p]==4);
        for(uint32_t p=0x20000/page;p<0x30000/page;++p) REQUIRE(physical_prot[p]==3);
        MEM8(0x21000)=0xAB;fail_advise=1;
        REQUIRE(void_vm_free(0x20000,0x4000,0x4000));
        REQUIRE(page_protect[0x21]==RESERVED_DIRTY);
        /* A failed final read-only protect leaves the zeroed page reserved
         * and restores the old physical permission for its whole group. */
        fail_protect=2;
        REQUIRE(!void_vm_alloc(0x21000,4096,0x1000,2));
        REQUIRE(page_state[0x21]==ST_RESERVE && page_protect[0x21]==RESERVED_DIRTY);
        REQUIRE(physical_prot[0x20000/page]==0);
        REQUIRE(void_vm_alloc(0x21000,4096,0x1000,2)==0x21000 && MEM8(0x21000)==0);
        REQUIRE(void_vm_protect(0x24000,4096,4,&old));
        REQUIRE(void_vm_query(0x22000,0x24000)==28);
        REQUIRE(MEM32(0x24000+12)==0x2000 && MEM32(0x24000+20)==0);
        REQUIRE(void_vm_alloc(0x30000,0x10000,0x2000,4)==0x30000);
        REQUIRE(void_vm_alloc(0x30000,4096,0x1000,4)==0x30000);
        REQUIRE(void_vm_alloc(0x33000,4096,0x1000,4)==0x33000);
        REQUIRE(void_vm_free(0x30000,4096,0x4000));
        /* Fresh reserved neighbors have no dirty bit but merge in Query. */
        REQUIRE(page_protect[0x30]==RESERVED_DIRTY && page_protect[0x31]==0);
        REQUIRE(void_vm_query(0x30000,0x24000)==28);
        REQUIRE(MEM32(0x24000+12)==0x3000 && MEM32(0x24000+20)==0);
        REQUIRE(void_vm_free(0x30000,0,0x8000));
        REQUIRE(void_vm_free(0x20000,0,0x8000));
        REQUIRE(void_vm_map_file(7,4096,4096,1,1)==0 && error==87);
        REQUIRE(void_vm_map_file(7,65536,4096,1,1)==0x10000);
        MEM8(0x10000)=0x99;
        REQUIRE(!void_vm_free(0x10000,4096,0x4000) && MEM8(0x10000)==0x99);
        REQUIRE(void_vm_unmap_file(0x10000));
    }
    REQUIRE(!invalid_calls);
    return 0;
}
'''
NATIVE = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#define GPTR(a) ((uint8_t *)(uintptr_t)(VOID_GUEST_BASE+(uint32_t)(a)))
static void void_log(const char *fmt,...) { va_list ap;va_start(ap,fmt);vprintf(fmt,ap);puts("");va_end(ap); }
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfile, obj, dll = (OUT / ('fixture' + suffix) for suffix in ('.c', '.obj', '.dll'))
    cfile.write_text('#define VM_FIXTURE 1\n' + FIXTURE + COMMON + VM + CASES + EXTRA)
    subprocess.run([str(B.CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                    '-fno-stack-protector', '-c', str(cfile), '-o', str(obj)], check=True)
    subprocess.run([str(B.CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                    '/out:' + str(dll), str(obj)], check=True)
    lib = ctypes.CDLL(str(dll))
    lib.check.argtypes = [ctypes.c_uint32]
    lib.check.restype = ctypes.c_uint32
    try:
        for page in (4096, 16384):
            result = lib.check(page)
            assert result == 0, (page, 'fixture C line', result)
    finally:
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(lib._handle)
    probe = OUT / 'probe.c'
    probe.write_text(NATIVE + COMMON + VM + CASES + r'''
int main(void) {
    uint32_t result=scenarios();
    printf("VM host-page probe: host=%ld guest=4096 result=%u\n",sysconf(_SC_PAGESIZE),result);
    return result ? 1 : 0;
}
''')
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-Wl,-z,max-page-size=16384',
                    '-Wl,-z,common-page-size=16384', str(probe), '-o', str(OUT / 'probe')], check=True)
    (OUT / 'results.json').write_text(json.dumps({'simulated_host_pages': [4096, 16384], 'pass': True,
        'checks': 'actual VM C: unaligned commit/protect/decommit, exact 4K metadata, sibling and guard data, '
                  'zero recommit, physical union, multi-run failure rollback, mapped-view preservation',
        'native_probe': str(OUT / 'probe'), 'native_probe_executed': False}, indent=2) + '\n')
    print('PASS actual C on simulated 4/16 KiB hosts; Android probe built:', OUT / 'probe')


if __name__ == '__main__':
    main()
