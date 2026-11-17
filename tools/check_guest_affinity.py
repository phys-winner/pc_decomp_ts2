"""Run guest pinning and own-main diagnostic masks with real NDK CPU sets."""
import io
import json
from pathlib import Path
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/guest-affinity'
PRELUDE = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
long fixture_capacity[CPU_SETSIZE],fixture_frequency[CPU_SETSIZE];
uint32_t fixture_cpus,fixture_disabled,fixture_calls,fixture_pid,fixture_fail,fixture_tid=1234;
cpu_set_t fixture_mask;
static cpu_set_t big_cores,next_cores;
static int big_cores_count,next_cores_count,t_main_thread;
static atomic_int main_pinned,main_guest_tid;
static pthread_once_t big_cores_once=PTHREAD_ONCE_INIT;
void *memset(void *p,int c,size_t n){uint8_t *d=p;while(n--)*d++=(uint8_t)c;return p;}
void *memcpy(void *p,const void *s,size_t n){uint8_t *d=p;const uint8_t *q=s;while(n--)*d++=*q++;return p;}
char *getenv(const char *name){return fixture_disabled?"0":NULL;}
long sysconf(int name){return fixture_cpus;}
pid_t gettid(void){return (pid_t)fixture_tid;}
int pthread_once(pthread_once_t *once,void (*init)(void)){if(!*once){*once=1;init();}return 0;}
int snprintf(char *s,size_t n,const char *fmt,...){if(n)*s=0;return 0;}
char *strerror(int error){return "fixture error";}
static void void_log(const char *fmt,...){ }
static void void_stat_main_thread(void){ }
int __sched_cpucount(size_t bytes,const cpu_set_t *mask){
    int n=0;const uint8_t *p=(const uint8_t *)mask;
    while(bytes--){uint8_t b=*p++;while(b){n+=b&1;b>>=1;}}return n;
}
static long cpu_value(int cpu,const char *name){
    return cpu<(int)fixture_cpus?(name[3]=='_'?fixture_capacity[cpu]:fixture_frequency[cpu]):0;
}
int sched_setaffinity(pid_t pid,size_t bytes,const cpu_set_t *mask){
    ++fixture_calls;fixture_pid=(uint32_t)pid;
    if(fixture_fail)return -1;
    fixture_mask=*mask;return 0;
}
'''
CHECK = r'''
int check(int action,int mode,int subsequent){
    CPU_ZERO(&big_cores);CPU_ZERO(&next_cores);
    big_cores_count=next_cores_count=big_cores_once=0;
    atomic_store(&main_pinned,subsequent);atomic_store(&main_guest_tid,0);
    fixture_calls=fixture_pid=0;fixture_tid=1234;
    if(action==2)return void_guest_affinity(mode);
    pin_guest_thread();
    if(!action)return (int)fixture_calls;
    if(action==3){fixture_tid=2345;pin_guest_thread();}
    fixture_calls=fixture_pid=0;
    memset(&fixture_mask,0xA5,sizeof fixture_mask);
    return void_guest_affinity(mode);
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = (B.ROOT / 'android/host/threads.c').read_text()
    functions = ''.join(function(source, signature) for signature in (
        'static void big_cores_init(void)', 'static void pin_guest_thread(void)\n{', 'int void_guest_affinity(int mode)'))
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(PRELUDE + functions + CHECK)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Wl,--gc-sections,-e,check', str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    mask_size = elf.get_section_by_name('.symtab').get_symbol_by_name('fixture_mask')[0]['st_size']
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
            uc.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))

    def run(caps, action=0, mode=0, subsequent=0, disabled=False, fail=False, frequency=None):
        n = len(caps)
        uc.mem_write(base + symbols['fixture_capacity'], struct.pack('<' + 'q' * n, *caps))
        uc.mem_write(base + symbols['fixture_frequency'], struct.pack('<' + 'q' * n, *(frequency or [0] * n)))
        for name, value in (('fixture_cpus', n), ('fixture_disabled', disabled), ('fixture_fail', fail)):
            uc.mem_write(base + symbols[name], struct.pack('<I', value))
        sentinel = bytes([0xA5]) * mask_size
        uc.mem_write(base + symbols['fixture_mask'], sentinel)
        uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
        for register, value in ((A.UC_ARM64_REG_X0, action), (A.UC_ARM64_REG_X1, mode & 0xFFFFFFFF),
                                (A.UC_ARM64_REG_X2, subsequent)):
            uc.reg_write(register, value)
        uc.emu_start(base + symbols['check'], stop, count=1000000)
        assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
        result = uc.reg_read(A.UC_ARM64_REG_X0)
        mask = bytes(uc.mem_read(base + symbols['fixture_mask'], mask_size))
        calls = int.from_bytes(uc.mem_read(base + symbols['fixture_calls'], 4), 'little')
        pid = int.from_bytes(uc.mem_read(base + symbols['fixture_pid'], 4), 'little')
        if not result:
            assert mask == sentinel
        return result, None if not result else [i for i in range(mask_size * 8) if mask[i // 8] & (1 << (i % 8))], calls, pid

    cases = [
        ('Tensor 4+3+1', [191] * 4 + [741] * 3 + [1024], [7], [4, 5, 6]),
        ('single prime capable next', [435] * 4 + [768] * 3 + [1024], [7], [4, 5, 6]),
        ('single prime low next', [435] * 7 + [1024], [7], []),
        ('two biggest', [435] * 6 + [1024] * 2, [6, 7], []),
        ('multi-big capable next', [700] * 6 + [1024] * 2, [6, 7], list(range(6))),
        ('60 percent threshold', [600] * 7 + [1000], [7], list(range(7))),
        ('below threshold', [599] * 7 + [1000], [7], []),
        ('high CPU bits', [191] * 63 + [741, 741, 1024], [65], [63, 64]),
    ]
    results = []
    for name, caps, big, next_ in cases:
        # Default policy stays big-only until the on-device A/B is complete.
        assert run(caps) == (1, big, 1, 0), name
        assert run(caps, subsequent=1) == (1, big, 1, 0), name
        for mode, wanted in enumerate((big, next_, sorted(big + next_))):
            expected = (1, wanted, 1, 1234) if wanted else (0, None, 0, 0)
            assert run(caps, action=1, mode=mode) == expected, (name, mode)
        assert run(caps, action=3, mode=2) == (1, sorted(big + next_), 1, 1234), name
        for mode in (-1, 3, 0x7FFFFFFF):
            assert run(caps, action=1, mode=mode) == (0, None, 0, 0)
        assert run(caps, action=2, mode=2) == (0, None, 0, 0)
        assert run(caps, action=1, mode=2, fail=True) == (0, None, 1, 1234)
        assert run(caps, action=1, mode=2, disabled=True) == (0, None, 0, 0)
        results.append({'topology': name, 'default': big, 'big': big, 'next': next_, 'union': sorted(big + next_)})
    for caps in ([1024] * 8, [0] * 8, [1024]):
        assert run(caps) == (0, None, 0, 0)
        assert run(caps, action=1, mode=2) == (0, None, 0, 0)
    assert run([0] * 8, frequency=[1000000] * 4 + [2600000] * 3 + [3105000]) == (1, [7], 1, 0)
    for filename in ('threads.c', 'script.c'):
        subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-fsyntax-only', str(B.ROOT / 'android/host' / filename)], check=True)
    (OUT / 'results.json').write_text(json.dumps({'pass': True, 'topologies': results,
        'checks': 'actual ARM64/NDK CPU sets; default and subsequent guests, diagnostic masks 0/1/2, own first-main TID, '
                  'invalid mode/no-main/no-policy/disabled/syscall failures, 60 percent boundary, frequency fallback, CPUs63–65',
        'policy': 'big-only default retained for pre-change device A/B'}, indent=2) + '\n')
    print('PASS actual ARM64 guest affinity:', len(cases), 'heterogeneous topologies, diagnostic guards and complete TU syntax')


if __name__ == '__main__':
    main()
