"""Compare emitted ARM64 x87 stack operations with the pinned game's x86 bytes."""
import io
import math
from pathlib import Path
import re
import struct
import subprocess

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A, x86_const as X

import build_android as B
from liftlib import CheckedLifter, Lifter


def main():
    out = B.ROOT / 'build/test/x87-transcendentals'
    out.mkdir(parents=True, exist_ok=True)
    pe = pefile.PE(str(B.ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    sites = {}
    for path in (B.ROOT / 'generated').rglob('*.c'):
        for address, mnemonic in re.findall(r'/\* 0x([0-9A-F]+): (fsincos|fptan|fpatan)\s+\*/', path.read_text()):
            sites.setdefault(mnemonic, int(address, 16))
    assert set(sites) == {'fsincos', 'fptan', 'fpatan'}, sites
    source = ['#define RECOMP_GENERATED_CODE', '#include "recomp_types.h"',
              'recomp_machine_t g_m; x87_window_t g_x87; int g_fp_bias;',
              'double input[8], output[8]; int depth, condition;',
              'void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}',
              'double sin(double), cos(double), tan(double), atan2(double,double);']
    originals = {}
    for mnemonic, address in sites.items():
        original = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, 2)
        instruction = next(decoder.disasm(original, address))
        assert instruction.mnemonic == mnemonic
        originals[mnemonic] = original
        lifted = '\n'.join(CheckedLifter().lift_instruction(instruction))
        assert lifted == '\n'.join(Lifter().lift_instruction(instruction)), 'project bypasses fixed vendor instruction'
        source.append('void check_' + mnemonic + '(void){g_stp=g_stbuf+X87_CENTRE;g_fp_bias=1;'
                      'memcpy(g_stp,input,sizeof input);_fpu_cmp=0;\n' + lifted + '\n'
                      'memcpy(output,g_stp,sizeof output);depth=_fp_top;condition=_fpu_cmp;}')
    code = out / 'check.c'
    code.write_text('\n'.join(source) + '\n')
    math_code = out / 'libm-boundary.c'
    math_code.write_text('double sin(double x){return x;} double cos(double x){return x;} '
                         'double tan(double x){return x;} double atan2(double y,double x){return y+x;}\n')
    binary = out / 'check.elf'
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector',
                    '-nostdlib', '-Iandroid/runtime', '-Wl,-e,check_fsincos,-z,max-page-size=16384',
                    str(code), str(math_code), '-o', str(binary)], cwd=B.ROOT, check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base, stack, stop = 0x10000000, 0x50000000, 0x60000000
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    arm.mem_map(base, 0x200000)
    arm.mem_map(stack, 0x10000)
    arm.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            arm.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    for relocation in relocations.iter_relocations() if relocations else ():
        assert relocation['r_info_type'] == 1027
        arm.mem_write(base + relocation['r_offset'], struct.pack('<Q', base + relocation['r_addend']))
    callbacks = {base + symbols[n]: getattr(math, n) for n in ('sin', 'cos', 'tan', 'atan2')}

    def libm(uc, address, size, data):
        if address not in callbacks:
            return
        def argument(register):
            return struct.unpack('<d', struct.pack('<Q', uc.reg_read(register)))[0]
        args = [argument(A.UC_ARM64_REG_D0)]
        if address == base + symbols['atan2']:
            args.append(argument(A.UC_ARM64_REG_D1))
        value = callbacks[address](*args)
        uc.reg_write(A.UC_ARM64_REG_D0, struct.unpack('<Q', struct.pack('<d', value))[0])
        uc.reg_write(A.UC_ARM64_REG_PC, uc.reg_read(A.UC_ARM64_REG_LR))
    arm.hook_add(UC_HOOK_CODE, libm)
    angles = (0., 1e-6, -1e-6, .01, -.01, math.pi/180, -math.pi/180,
              math.pi/4, -math.pi/4, math.pi/2, math.pi, 2*math.pi, 10., -10.)
    cases = 0
    for mnemonic, original in originals.items():
        for angle in angles:
            values = (angle, 9.125, -3.25, 7.5, 2., 3., 4., 5.)
            x86 = Uc(UC_ARCH_X86, UC_MODE_32)
            x86.mem_map(0x10000, 4096)
            x86.mem_write(0x10300, struct.pack('<8d', *values))
            # FNINIT, load three lower sentinels and the angle, then execute the original instruction.
            instructions = bytes.fromhex('dbe3')
            for index in (3, 2, 1, 0):
                instructions += bytes.fromhex('dd05') + struct.pack('<I', 0x10300 + index*8)
            instructions += original
            count = 3 if mnemonic == 'fpatan' else 5
            for index in range(count):
                instructions += bytes.fromhex('dd1d') + struct.pack('<I', 0x10400 + index*8)
            x86.mem_write(0x10000, instructions)
            x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
            x86.emu_start(0x10000, 0x10000 + len(instructions), count=32)
            expected = struct.unpack('<' + 'd'*count, x86.mem_read(0x10400, count*8))
            assert x86.reg_read(X.UC_X86_REG_EFLAGS) == 0x202
            arm.mem_write(base + symbols['input'], struct.pack('<8d', *values))
            arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000)
            arm.reg_write(A.UC_ARM64_REG_LR, stop)
            arm.emu_start(base + symbols['check_' + mnemonic], stop, count=10000)
            actual = struct.unpack('<' + 'd'*count, arm.mem_read(base + symbols['output'], count*8))
            assert all(math.isclose(a, b, rel_tol=2e-12, abs_tol=2e-12)
                       for a, b in zip(actual, expected)), (mnemonic, angle, actual, expected)
            assert struct.unpack('<i', arm.mem_read(base + symbols['depth'], 4))[0] == (0 if mnemonic == 'fpatan' else 2)
            if mnemonic != 'fpatan':
                assert struct.unpack('<i', arm.mem_read(base + symbols['condition'], 4))[0] == 1
            cases += 1
    print(f'PASS {cases} original-x86/actual-ARM64 FSINCOS/FPTAN/FPATAN cases, stack, sentinels and EFLAGS; libm calls are explicit oracle boundaries')


if __name__ == '__main__':
    main()
