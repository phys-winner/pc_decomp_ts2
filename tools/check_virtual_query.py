"""Compile actual VirtualQuery/GetSystemInfo C and check finite 32-bit walks.

The original psapi fallback advances a 32-bit pointer by RegionSize. The last
free region must end at the user top, not 4 GiB where that pointer wraps to0.
Also checks MEMORY_BASIC_INFORMATION32 state/protection/allocation fields.
Uses the installed NDK host compiler and ctypes, without adb or a GPU.
"""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile

from build_android import CLANG, ROOT


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    memory = (ROOT / 'android/host/memory.c').read_text(encoding='utf-8')
    kernel = (ROOT / 'android/host/kernel32.c').read_text(encoding='utf-8')
    header = (ROOT / 'android/host/host.h').read_text(encoding='utf-8')
    top_literal = re.search(r'^#define GUEST_USER_TOP (\S+)', header, re.M)[1]
    top = int(top_literal.rstrip('uU'), 16)
    prelude = r'''
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long size_t;
#define PAGES (1u << 20)
#define GRANULES (1u << 16)
enum { ST_FREE, ST_RESERVE, ST_COMMIT };
#define MEM_RESERVE_ 0x2000u
#define MEM_COMMIT_ 0x1000u
#define MEM_PRIVATE_ 0x20000u
#define ERROR_INVALID_PARAMETER_ 87u
static uint32_t words[32], error, vm_lock;
#define MEM32(a) (*(uint32_t *)((uint8_t *)words + (a)))
#define MEM16(a) (*(uint16_t *)((uint8_t *)words + (a)))
#define GPTR(a) ((uint8_t *)words + (a))
#define ARG(n) 0u
#define WINAPI_EXPORT(d,n,c) __declspec(dllexport) uint32_t test_##n(void)
#define _SC_NPROCESSORS_CONF 1
static long sysconf(int n) { return 8; }
static void pthread_mutex_lock(uint32_t *p) { (void)p; }
static void pthread_mutex_unlock(uint32_t *p) { (void)p; }
static void void_set_last_error(uint32_t e) { error=e; }
static void *memset(void *p, int c, size_t n) {
    volatile uint8_t *b=p; while(n--) *b++=(uint8_t)c; return p;
}
'''
    wrappers = r'''
__declspec(dllexport) uint32_t *test_words(void) { return words; }
__declspec(dllexport) uint32_t test_error(void) { return error; }
__declspec(dllexport) uint32_t test_query(uint32_t a) { return void_vm_query(a,64); }
__declspec(dllexport) void test_reset(void) {
    memset(page_state,0,sizeof page_state); memset(page_protect,0,sizeof page_protect);
    memset(page_alloc,0,sizeof page_alloc); memset(alloc_protect,0,sizeof alloc_protect); error=0;
}
__declspec(dllexport) void test_pages(uint32_t first,uint32_t count,uint32_t state,uint32_t protect,uint32_t base,uint32_t initial) {
    set_pages(first,count,(uint8_t)state,(uint16_t)protect,base);
    alloc_protect[base >> 4]=(uint16_t)initial;
}
/* Same unsigned pointer progression as original fallback A01BA010, bounded
 * here so the old wrap bug fails the check instead of hanging the test. */
__declspec(dllexport) uint32_t test_walk(uint32_t steps) {
    test_GetSystemInfo();uint32_t address=words[2],maximum=words[3],count=0,total=0;
    while(address<=maximum) {
        if(count++>=steps)return 0;
        if(!void_vm_query(address,64))break;
        if(words[20]==MEM_COMMIT_&&words[22]==MEM_PRIVATE_)total+=words[19];
        uint32_t next=address+words[19];
        if(next<=address)return 0;
        address=next;
    }
    words[24]=address;words[25]=count;words[26]=total;return 1;
}
'''
    declarations = memory[memory.index('static uint8_t page_state['):memory.index('static pthread_mutex_t vm_lock')]
    code = (prelude + declarations + f'\n#define GUEST_USER_TOP {top_literal}\n' +
            function(memory, 'static void set_pages(') +
            function(memory, 'uint32_t void_vm_query(') +
            function(kernel, 'WINAPI_EXPORT(K32, GetSystemInfo, 1)') + wrappers)
    with tempfile.TemporaryDirectory(prefix='sims2-virtual-query-') as directory:
        base = Path(directory)
        source, obj, library = base / 'check.c', base / 'check.obj', base / 'check.dll'
        source.write_text(code)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library))
        dll.test_words.restype = ctypes.POINTER(ctypes.c_uint32)
        dll.test_query.argtypes = [ctypes.c_uint32]
        dll.test_walk.argtypes = [ctypes.c_uint32]
        dll.test_pages.argtypes = [ctypes.c_uint32] * 6
        words = dll.test_words()
        try:
            dll.test_reset()
            dll.test_GetSystemInfo()
            assert (words[1], words[2], words[3], words[7]) == (4096, 65536, top - 1, 65536)
            assert dll.test_walk(32) == 1 and words[24] == top and words[25] == 1 and words[26] == 0
            assert dll.test_query(top - 1) == 28
            assert list(words[16:23]) == [top - 4096, 0, 0, 4096, 0x10000, 0, 0]
            old = list(words[16:23])
            for address in (top, top + 1, 0xFFFFFFFF):
                assert dll.test_query(address) == 0 and dll.test_error() == 87
                assert list(words[16:23]) == old, 'failed query changed the output'
            dll.test_pages(0x10, 2, 1, 0, 0x10, 4)
            dll.test_pages(0x12, 2, 2, 4, 0x10, 4)
            dll.test_pages(0x14, 1, 2, 2, 0x10, 4)
            dll.test_pages(0x20, 2, 2, 4, 0x20, 4)
            dll.test_pages(0x30, 1, 2, 4, 0x30, 4)
            cases = [
                (0x10001, [0x10000, 0x10000, 4, 0x2000, 0x2000, 0, 0x20000]),
                (0x12001, [0x12000, 0x10000, 4, 0x2000, 0x1000, 4, 0x20000]),
                (0x13FFF, [0x13000, 0x10000, 4, 0x1000, 0x1000, 4, 0x20000]),
                (0x14000, [0x14000, 0x10000, 4, 0x1000, 0x1000, 2, 0x20000]),
                (0x20000, [0x20000, 0x20000, 4, 0x2000, 0x1000, 4, 0x20000]),
                (0x30000, [0x30000, 0x30000, 4, 0x1000, 0x1000, 4, 0x20000]),
            ]
            for address, expected in cases:
                assert dll.test_query(address) == 28
                assert list(words[16:23]) == expected, (hex(address), list(words[16:23]), expected)
            assert dll.test_walk(32) == 1 and words[24] == top and words[26] == 6 * 4096
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('VirtualQuery/GetSystemInfo actual C: finite 32-bit walks, user-top boundaries, '
          'free/reserved/committed states, allocation/protection fields PASS')


if __name__ == '__main__':
    main()
