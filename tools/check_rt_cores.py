"""Execute the pure render-core selector as AArch64 with the real NDK CPU-set macros."""
import io
import json
import struct
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A
import build_android as B


def main():
    out = B.ROOT / 'build/test/rt-cores'
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'check.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stddef.h>
void *memset(void *p,int c,size_t n){unsigned char *s=p;while(n--)*s++=(unsigned char)c;return p;}
#include "../../../android/host/d3d9_rt_cores.inc"
long capacities[CPU_SETSIZE];
int count, policy, null_input;
cpu_set_t result;
int check(void){return d3d_rt_select_cores(null_input?NULL:capacities,count,policy,&result);}
''', encoding='utf-8')
    binary = out / 'check.elf'
    r = subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector',
                        '-nostdlib', '-Wl,--gc-sections,-e,check', str(source), '-o', str(binary)],
                       capture_output=True, text=True, cwd=B.ROOT)
    assert r.returncode == 0, r.stderr
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symtab = elf.get_section_by_name('.symtab')
    symbols = {s.name: s['st_value'] for s in symtab.iter_symbols()}
    mask_size = symtab.get_symbol_by_name('result')[0]['st_size']
    base, stack, stop = 0x10000000, 0x50000000, 0x60000000
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(base, 0x200000); u.mem_map(stack, 0x10000); u.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD': u.mem_write(base + segment['p_vaddr'], segment.data())
    rel = elf.get_section_by_name('.rela.dyn')
    if rel:
        for entry in rel.iter_relocations():
            assert entry['r_info_type'] == 1027
            u.mem_write(base + entry['r_offset'], struct.pack('<Q', base + entry['r_addend']))

    def run(caps, mode, expected, count=None, null=False):
        if caps: u.mem_write(base + symbols['capacities'], struct.pack('<' + 'q' * len(caps), *caps))
        for name, value in (('count', len(caps) if count is None else count), ('policy', mode), ('null_input', null)):
            u.mem_write(base + symbols[name], struct.pack('<i', value))
        sentinel = bytes([0xA5]) * mask_size
        u.mem_write(base + symbols['result'], sentinel)
        u.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); u.reg_write(A.UC_ARM64_REG_LR, stop)
        u.emu_start(base + symbols['check'], stop, count=100000)
        assert u.reg_read(A.UC_ARM64_REG_PC) == stop
        good = u.reg_read(A.UC_ARM64_REG_X0)
        mask = bytes(u.mem_read(base + symbols['result'], mask_size))
        if expected is None: assert not good and mask == sentinel
        else:
            actual = [i for i in range(mask_size * 8) if mask[i // 8] & (1 << (i % 8))]
            assert good and actual == expected, (caps, mode, actual, expected)

    cases = [
        ('1+4+3', [1024] + [768]*4 + [435]*3, list(range(1,5)), [0], list(range(1,5))),
        ('4+4', [435]*4 + [1024]*4, list(range(4,8)), list(range(4,8)), list(range(4))),
        ('2+6', [435]*6 + [1024]*2, [6,7], [6,7], list(range(6))),
        ('1+3+4', [435]*4 + [768]*3 + [1024], [4,5,6], [7], [4,5,6]),
        ('two tiers, one prime', [435]*7 + [1024], list(range(7)), [7], list(range(7))),
        ('two tiers above old60percent', [700]*6 + [1024]*2, [6,7], [6,7], list(range(6))),
        ('four tiers', [435]*3 + [600]*2 + [768]*2 + [1024], [5,6], [7], [5,6]),
        ('homogeneous', [1024]*8, None, None, None),
        ('unknown', [0]*8, None, None, None),
        ('partly unknown', [1024,0,768], None, None, None),
        ('single CPU', [1024], None, None, None),
        ('empty', [], None, None, None),
        ('bits above63', [435]*64 + [1024]*2, [64,65], [64,65], list(range(64))),
    ]
    for name, caps, auto, big, nxt in cases:
        for mode, expected in enumerate((auto, big, nxt, list(range(len(caps))) if caps else None)):
            run(caps, mode, expected)
    for mode in range(3): run([1024,435], mode, None, null=True)
    run([1024,435], 3, [0,1], null=True)
    for count in (-1, 0, mask_size*8+1): run([1024,435], 0, None, count=count)
    for mode in (-1,4): run([1024,435], mode, None)
    receipt = {'topologies': [c[0] for c in cases], 'policies': ['auto','big','next','any'],
               'cases': len(cases)*4+9, 'ndk_cpu_set_bytes': mask_size, 'all_pass': True,
               'no_selection_keeps_output_unchanged': True}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print(f"PASS actual AArch64 RT core policy: {receipt['cases']} cases, {len(cases)} topologies, four policies")


if __name__ == '__main__': main()
