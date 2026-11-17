"""Prove the guarded unchanged-pipeline path against original x86 and ARM64.

The original slow update body is untouched; guard failures are checked for exact
delegation without writes. Production-v7 ARM64 supplies the instruction baseline.
"""
import io
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A, x86_const as X

import build_android as B

OUT = B.ROOT / 'build/test/state-apply'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source, binary = OUT / 'check.c', OUT / 'check.elf'
    source.write_text('#include "host.h"\nrecomp_machine_t g_m;\nuint32_t delegated;\n'
        'void sub_A017FC00_lifted(void){++delegated;}\n'
        '#include "' + (B.ROOT / 'android/host/sims2_fast.c').as_posix() + '"\n'
        'uint32_t check(uint32_t initial_flags){g_flag_k=FK_EFLAGS;g_flag_a=initial_flags;'
        'g_flag_b=0;g_flag_cf=initial_flags&1;sub_A017FC00();'
        'return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}\n'
        '_Static_assert(offsetof(recomp_machine_t,m_xmm)==96,"XMM fixture offset");\n')
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,check', str(source), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    syms = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    arm.mem_map(base, 0x200000); arm.mem_map(guest + 0x10000, 0x20000)
    arm.mem_map(stack, 0x10000); arm.mem_map(stop, 4096)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            arm.mem_write(base + segment['p_vaddr'], segment.data())
    relocs = elf.get_section_by_name('.rela.dyn')
    if relocs:
        for r in relocs.iter_relocations():
            assert r['r_info_type'] == 1027
            arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))
    pe = pefile.PE(str(B.ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    image = pe.get_memory_mapped_image()
    x86 = Uc(UC_ARCH_X86, UC_MODE_32)
    x86.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image) + 65535) & ~65535)
    x86.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
    x86.mem_map(0x10000, 0x20000); x86.mem_map(stop, 4096)
    original_arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    prod_base = 0x70000000
    reference = json.loads((B.ROOT / 'tools/fixtures/state-apply-v7.json').read_text())
    func, old_machine = reference['function'], reference['machine']
    old_code = bytes.fromhex(reference['code_hex'])
    assert len(old_code) == func['st_size']
    assert hashlib.sha256(old_code).hexdigest() == reference['code_sha256']
    for symbol in (func, old_machine):
        lo = symbol['st_value'] & ~4095
        hi = (symbol['st_value'] + symbol['st_size'] + 4095) & ~4095
        original_arm.mem_map(prod_base + lo, hi - lo)
    original_arm.mem_write(prod_base + func['st_value'], old_code)
    original_arm.mem_map(guest + 0x10000, 0x20000); original_arm.mem_map(stack, 0x10000); original_arm.mem_map(stop, 4096)
    counts = {'native': 0, 'native_fast': 0, 'production_v7': 0}
    native = syms['sub_A017FC00']
    arm.hook_add(UC_HOOK_CODE, lambda uc, at, size, user: counts.__setitem__('native', counts['native'] + 1)
                 if base + native['st_value'] <= at < base + native['st_value'] + native['st_size'] else None)
    original_arm.hook_add(UC_HOOK_CODE, lambda uc, at, size, user: counts.__setitem__('production_v7', counts['production_v7'] + 1))
    regs = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
            X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
    rng = random.Random(0xA017FC00)
    total, fallbacks = 0, 0
    machine_bytes = syms['g_m']['st_size']

    def execute(memory, initial, initial_flags, fallback=False, label=''):
        nonlocal total, fallbacks
        data = bytearray(machine_bytes)
        struct.pack_into('<8I', data, 0, *initial)
        data[96:224] = rng.randbytes(128)
        arm.mem_write(guest + 0x10000, bytes(memory)); arm.mem_write(base + syms['g_m']['st_value'], bytes(data))
        arm.mem_write(base + syms['delegated']['st_value'], bytes(4))
        arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); arm.reg_write(A.UC_ARM64_REG_LR, stop)
        arm.reg_write(A.UC_ARM64_REG_X0, initial_flags)
        before_instructions = counts['native']
        arm.emu_start(base + syms['check']['st_value'], stop, count=10000)
        assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, label
        actual_data = bytes(arm.mem_read(base + syms['g_m']['st_value'], machine_bytes))
        actual = struct.unpack_from('<8I', actual_data)
        called = int.from_bytes(arm.mem_read(base + syms['delegated']['st_value'], 4), 'little')
        if fallback:
            assert called == 1 and actual == tuple(initial), label
            assert bytes(arm.mem_read(guest + 0x10000, len(memory))) == bytes(memory), label
            assert actual_data[96:224] == bytes(data[96:224]), label
            fallbacks += 1
            return
        assert called == 0, label
        counts['native_fast'] += counts['native'] - before_instructions
        x86.mem_write(0x10000, bytes(memory))
        for register, value in zip(regs, initial):
            x86.reg_write(register, value)
        x86.reg_write(X.UC_X86_REG_EFLAGS, initial_flags)
        x86.emu_start(0xA017FC00, stop, count=10000)
        assert x86.reg_read(X.UC_X86_REG_EIP) == stop, label
        assert actual == tuple(x86.reg_read(r) for r in regs), (label, actual, [x86.reg_read(r) for r in regs])
        assert bytes(arm.mem_read(guest + 0x10000, len(memory))) == bytes(x86.mem_read(0x10000, len(memory))), label
        assert (arm.reg_read(A.UC_ARM64_REG_X0) ^ x86.reg_read(X.UC_X86_REG_EFLAGS)) & 0x8D5 == 0, label
        assert actual_data[96:224] == bytes(data[96:224]), label
        original_arm.mem_write(guest + 0x10000, bytes(memory))
        original_arm.mem_write(prod_base + old_machine['st_value'], bytes(data))
        original_arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); original_arm.reg_write(A.UC_ARM64_REG_LR, stop)
        original_arm.emu_start(prod_base + func['st_value'], stop, count=10000)
        assert original_arm.reg_read(A.UC_ARM64_REG_PC) == stop, label
        assert bytes(original_arm.mem_read(prod_base + old_machine['st_value'], machine_bytes)) == actual_data, (label, 'complete machine bank')
        assert bytes(original_arm.mem_read(guest + 0x10000, len(memory))) == bytes(arm.mem_read(guest + 0x10000, len(memory))), label
        total += 1

    def fixture(flags, count):
        memory = bytearray(rng.randbytes(0x20000)); self, arg, descriptor, sp = 0x11000, 0x12000, 0x13000, 0x28000
        def put(address, value):
            struct.pack_into('<I', memory, address - 0x10000, value)
        put(arg + 12, descriptor); memory[descriptor + 2 - 0x10000] = flags
        for offset in (0x74, 0x78, 0x7C):
            put(self + offset, 0)
        if flags & 1:
            put(self + 0x98, arg)
            for offset in (0x1C, 0x20, 0x24, 0x28, 0x2C):
                put(self + offset, 0)
        if flags & 2:
            put(self + 0x9C, arg); put(self + 0x70, 0)
        put(self + 0x18, count); put(sp, stop); put(sp + 4, arg)
        initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
        return memory, initial, put

    for flags in range(256):
        for count in (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
            memory, initial, _ = fixture(flags, count)
            execute(memory, initial, 0x202 | (rng.getrandbits(12) & 0x8D5), label=f'flags{flags}/count{count:x}')
    for offset in (0x74, 0x78, 0x7C, 0x98, 0x9C, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x70):
        memory, initial, put = fixture(3, 0xFFFFFFFF)
        put(0x11000 + offset, 1)
        execute(memory, initial, 0x203, True, f'dirty{offset:x}')
    for which, offset in (('self', -8), ('arg', -8), ('descriptor', -4)):
        memory, initial, put = fixture(3, 5)
        if which == 'self':
            initial[1] = initial[3] + offset
        elif which == 'arg':
            put(initial[3] + 4, initial[3] + offset)
        else:
            put(0x12000 + 12, initial[3] + offset)
        execute(memory, initial, 0x203, True, which + '-stack-alias')
    for which in ('self', 'arg', 'descriptor', 'sp'):
        memory, initial, put = fixture(3, 5)
        if which == 'self': initial[1] = 0xFFFFFFF0
        elif which == 'arg': put(initial[3] + 4, 0xFFFFFFF8)
        elif which == 'descriptor': put(0x12000 + 12, 0xFFFFFFFF)
        else: initial[3] = 0xFFFFFFFC
        execute(memory, initial, 0x203, True, which + '-wrap')
    result = dict(pass_=True, original_x86_and_production_arm64_cases=total, exact_fallback_delegation_cases=fallbacks,
                  instructions=counts, native_mean_instructions=counts['native_fast'] / total,
                  original_arm64_mean_instructions=counts['production_v7'] / total,
                  checked='all GPRs, arithmetic flags, complete machine bank, guest buffers and stack scratch, unchanged XMM bank; '
                          'all 256 flags, counter wrap/sign edges, every dirty field/program mismatch, stack aliases and wrapping pointers',
                  limits='unchanged original slow body not reimplemented; rejected guards use a delegation stub')
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS guarded pipeline application:', result)


if __name__ == '__main__':
    main()
