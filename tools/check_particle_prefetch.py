"""Check the actual patched particle blocks against x86 and before-hint ARM64.

    python tools/check_particle_prefetch.py

No device is used. Also saves before/after generated copies and disassembly
under build/test/particle-prefetch for review and compiles both complete units.
"""
import io
from pathlib import Path
import random
import struct
import subprocess

import build_android as B
from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/particle-prefetch'
OUT.mkdir(parents=True, exist_ok=True)
SITES = (
    ('sims2_A0BB0000.c', 'A0BB6830', 0xA0BB7AA0, 0xA0BB7AB0, 0xA0BB7D18),
    ('sims2_A0BB8000.c', 'A0BBDD50', 0xA0BBE771, 0xA0BBE7AC, 0xA0BBECA1),
)
prefix = b'#include "recomp_types.h"\nrecomp_machine_t g_m;\nuint32_t branch;\n'
fixtures = []
helper = b''
for filename, function, start, fallthrough, taken in SITES:
    original = (ROOT / 'generated/sims2-x86' / filename).read_bytes()
    if b'goto _ljump; }' in original:
        original = B.direct_jump_tables(original, B.module_image('sims2'))
    patched = B.prefetch_particles(original)
    assert patched.count(b'__builtin_prefetch') - original.count(b'__builtin_prefetch') == 1
    if not helper:
        helper_start = patched.index(b'static inline __attribute__((always_inline)) void prefetch_particle(')
        helper_end = B.FUNCTION_START.search(patched, helper_start).start()
        helper = patched[helper_start:helper_end]
    for version, data in (('before', original), ('after', patched)):
        (OUT / f'{filename}.{version}.c').write_bytes(data)
        first = data.index(f'L_{start:08X}:'.encode())
        last = data.index(f'L_{fallthrough:08X}:'.encode(), first)
        block = data[first:last]
        fixture = (f'void sub_{function}_{version}(void) {{\n'
                   'uint32_t _cf=0, _flag_k=FK_NONE, _flag_a=0, _flag_b=0; int _df=1;\n'
                   f'RECOMP_REGS_LOCALS\nRECOMP_ENTER(0x{function}u);\n').encode() + block
        fixture += (f'branch=0; goto done;\nL_{taken:08X}: branch=1;\n'
                    'done: RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n').encode()
        fixtures.append(B.localize_registers(fixture))
    # A moved/missing measured loop must fail visibly instead of silently losing the hint.
    try:
        B.prefetch_particles(original.replace(f'L_{start:08X}:'.encode(), b'L_changed:'))
    except ValueError:
        pass
    else:
        raise AssertionError('missing prefetch label was accepted')

assert B.prefetch_particles(b'void sub_A0000000(void) {\n}\n') == b'void sub_A0000000(void) {\n}\n'
source = OUT / 'check.c'
source.write_bytes(prefix + helper + b'\n'.join(fixtures) + b'\n'
                   b'uint32_t flags(void) { return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1); }\n'
                   b'_Static_assert(offsetof(recomp_machine_t,m_xmm)==96,"machine layout");\n')
binary = OUT / 'check.elf'
entries = [f'sub_{function}_{version}' for _, function, *_ in SITES for version in ('before', 'after')]
flags = B.COMMON + ['-O2', '-DRECOMP_GENERATED_CODE', '-DRECOMP_LOCAL_REGS', '-DRECOMP_PLAIN_MEMORY',
                    '-Iandroid/runtime', '-fno-stack-protector', '-nostdlib', '-fdata-sections']
subprocess.run([str(B.CLANG)] + flags + ['-Wl,--gc-sections,-e,' + entries[0] +
                ''.join(',-u,' + entry for entry in entries[1:] + ['flags']), str(source), '-o', str(binary)],
               cwd=ROOT, check=True)
objdump = B.TOOLCHAIN / 'bin/llvm-objdump.exe'
assembly = subprocess.check_output([str(objdump), '-d', str(binary)], text=True)
(OUT / 'check.s').write_text(assembly)
assert assembly.count('\tprfm\t') == 2, 'expected one hardware prefetch in each changed block'
assert '<prefetch_particle' not in assembly, 'prefetch helper was not inlined'

elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000); arm.mem_map(native_stack, 0x10000); arm.mem_map(stop, 0x1000)
arm.mem_map(guest + 0x10000, 0x10000); arm.mem_map(guest + 0xFFFDF000, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type'] == 'PT_LOAD': arm.mem_write(base + segment['p_vaddr'], segment.data())
relocations = elf.get_section_by_name('.rela.dyn')
if relocations:
    for r in relocations.iter_relocations():
        assert r['r_info_type'] == 1027
        arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))

pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
image = pe.get_memory_mapped_image()
x86 = Uc(UC_ARCH_X86, UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image)+0xFFFF)&~0xFFFF)
x86.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
x86.mem_map(0x10000, 0x10000)
registers = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
             X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
rng = random.Random(0xA0BB7AA0)
targets = set()
def stop_at_boundary(machine, address, size, user):
    if address in targets: machine.emu_stop()
x86.hook_add(UC_HOOK_CODE, stop_at_boundary)

def run_arm(entry, memory, initial, xmm, high_payload):
    arm.mem_write(guest + 0x10000, memory)
    arm.mem_write(guest + 0xFFFDFFFC, struct.pack('<I', high_payload))
    arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(64) + xmm)
    arm.mem_write(base + symbols['branch'], bytes(4))
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.emu_start(base + symbols[entry], stop, count=10000)
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop
    state = bytes(arm.mem_read(base + symbols['g_m'], 224))
    branch = int.from_bytes(arm.mem_read(base + symbols['branch'], 4), 'little')
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.emu_start(base + symbols['flags'], stop, count=1000)
    return (state[:32], state[96:224], bytes(arm.mem_read(guest + 0x10000, 0x10000)),
            arm.reg_read(A.UC_ARM64_REG_X0) & 0x8D5, branch)

cases = 0
for filename, function, start, fallthrough, taken in SITES:
    targets.clear(); targets.update((fallthrough, taken))
    for case in range(512):
        memory = bytearray(rng.randbytes(0x10000))
        def put(address, value): struct.pack_into('<I', memory, address-0x10000, value)
        node, head, payload, self, geometry, frame, sp = 0x11000, 0x13000, 0x14000, 0x16000, 0x17000, 0x1F000, 0x1E000
        # The sentinel and invalid successors are deliberately unmapped. Their
        # payload pointer must not be read; a hinted payload may also be unmapped.
        head = 0x40000000 if case % 8 == 1 else head
        successor = (0x12000, head, 0, 0xFFFF, 0xFFFDFFF5, 0xFFFFFFFC, 0xFFFDFFF4, 0x12000)[case % 8]
        next_payload = 0x70000000 if case % 8 == 7 else 0x15000
        put(node, successor); put(node+8, payload); put(0x12000+8, next_payload)
        put(self+0x14, head); put(geometry+0x74, self); put(frame-0x30, geometry)
        memory[payload-0x10000] = case % 2
        memory[frame-0x4D-0x10000] = (0, 1, 0x7F, 0x80, 0xFF)[case % 5]
        for offset in (4, 8):
            value = rng.choice((0.125, -2.0, 1e-20, 1e20, rng.uniform(-1000,1000)))
            struct.pack_into('<f', memory, payload+offset-0x10000, value)
        initial = [rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), sp,
                   rng.getrandbits(32), node, self if function == 'A0BB6830' else 0x18000, frame]
        xmm = rng.randbytes(128)
        x86.mem_write(0x10000, bytes(memory))
        for reg, value in zip(registers, initial): x86.reg_write(reg, value)
        for i in range(8): x86.reg_write(getattr(X, f'UC_X86_REG_XMM{i}'), int.from_bytes(xmm[i*16:i*16+16], 'little'))
        x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202); x86.reg_write(X.UC_X86_REG_MXCSR, 0x1F80)
        x86.emu_start(start, 0, count=1000)
        ip = x86.reg_read(X.UC_X86_REG_EIP)
        assert ip in targets
        expected = (struct.pack('<8I', *(x86.reg_read(reg) for reg in registers)),
                    b''.join(x86.reg_read(getattr(X, f'UC_X86_REG_XMM{i}')).to_bytes(16,'little') for i in range(8)),
                    bytes(x86.mem_read(0x10000,0x10000)), x86.reg_read(X.UC_X86_REG_EFLAGS)&0x8D5, int(ip==taken))
        before = run_arm(f'sub_{function}_before', bytes(memory), initial, xmm, next_payload)
        after = run_arm(f'sub_{function}_after', bytes(memory), initial, xmm, next_payload)
        assert before == expected, (function, case, 'existing block disagrees with x86')
        assert after == before, (function, case, 'prefetch changed guest state')
        cases += 1
print(f'PASS: {cases} actual x86/ARM64 block comparisons, all GPRs/XMMs/flags/guest writes/branches exact')
print('PASS: sentinel, lower/upper/wrapping bounds and unmapped hinted payload; two PRFM instructions')

# Compile both complete units before/after with production options. Keep PGO
# diagnostics visible here even though normal generated builds use -w.
profile = ROOT / 'build/pgo/abb24fae-20261022.profdata'
for filename, function, *_ in SITES:
    sizes = []
    for version in ('before', 'after'):
        data = (OUT / f'{filename}.{version}.c').read_bytes()
        localized = OUT / f'{filename}.{version}.localized.c'
        localized.write_bytes(B.localize_registers(data))
        obj = OUT / f'{filename}.{version}.o'
        command = [str(B.CLANG)] + [flag for flag in B.GENERATED_FLAGS if flag not in ('-w', '-DRECOMP_GENERATED_CODE')] + [
            '-DRECOMP_LOCAL_REGS', '-DRECOMP_PLAIN_MEMORY', '-Igenerated/sims2-x86',
            '-c', str(localized), '-o', str(obj)]
        if profile.is_file(): command.insert(1, '-fprofile-instr-use=' + str(profile))
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (OUT / f'{filename}.{version}.warnings.txt').write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        if profile.is_file():
            assert '[-Wprofile-instr-out-of-date]' not in result.stderr, result.stderr
            assert '[-Wprofile-instr-unprofiled]' not in result.stderr, result.stderr
        disassembly = subprocess.check_output([str(objdump), '--disassemble-symbols=sub_' + function, str(obj)], text=True)
        (OUT / f'{function}.{version}.s').write_text(disassembly)
        assert disassembly.count('\tprfm\t') == int(version == 'after')
        assert '<prefetch_particle' not in disassembly
        object_elf = ELFFile(io.BytesIO(obj.read_bytes()))
        sizes.append(object_elf.get_section_by_name('.symtab').get_symbol_by_name('sub_' + function)[0]['st_size'])
        print(f'PASS: complete {filename} {version} compile' + (' with production PGO' if profile.is_file() else ''))
        if result.stderr.strip(): print(result.stderr.strip())
    print(f'{function}: {sizes[0]} -> {sizes[1]} native bytes; one added PRFM')
print(OUT)
