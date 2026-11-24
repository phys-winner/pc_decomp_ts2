"""Check Android's main-intro latch against the original x86 state 6.

    python tools/check_startup_intro.py

No device or production build is used. Saves before/after generated copies,
targeted ARM64 fixtures and complete-unit PGO diagnostics under build/test.
"""
import io
from pathlib import Path
import random
import struct
import subprocess

import build_android as B
from audio_probe import compile_probe
from check_virtual_query import function
from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

ROOT = Path(__file__).resolve().parents[1]
wrapper = function((ROOT / 'android/host/sims2_patches.c').read_text(), 'void sub_A05C089B(void)')
compile_probe('startup-movie-choice', r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
static uint32_t g_eax,g_esp,calls;
static const char *choice;
static int extender;
static int s2_extender_skips_intro(void){return extender;}
static const char *test_getenv(const char *key){assert(!strcmp(key,"PORT_SKIP_INTRO"));return choice;}
#define getenv test_getenv
static void sub_A05C089B_lifted(void){++calls;g_eax=42;g_esp+=12;}
''' + wrapper + r'''
int main(void){
 const char *choices[]={NULL,"0","false","1","true","invalid"};
 for(unsigned i=0;i<6;++i){
  choice=choices[i];calls=0;g_esp=0x10000;g_eax=99;sub_A05C089B();
  assert(g_esp==0x1000C);
  assert(i==3||i==4 ? (!calls&&!g_eax) : (calls==1&&g_eax==42));
 }
 extender=1;choice="0";calls=0;g_esp=0x10000;g_eax=99;sub_A05C089B();
 assert(!calls&&!g_eax&&g_esp==0x1000C);
 puts("PASS actual movie-open wrapper: skip both clips only when requested; original path and stack preserved otherwise");
 return 0;
}
''', {})
OUT = ROOT / 'build/test/startup-intro'
OUT.mkdir(parents=True, exist_ok=True)
filename = 'sims2_A05C0000.c'
original = (ROOT / 'generated/sims2-x86' / filename).read_bytes()
before = B.direct_jump_tables(original, B.module_image('sims2'))
after = B.skip_startup_intro(before)
insertion = (b'    /* Android: request the original main-intro key-skip path. */\r\n'
             b'    s2_intro_latch(ebx);\r\n')
assert after.count(insertion) == 1
assert after.removeprefix(b'extern void s2_intro_latch(unsigned int owner);\n').replace(insertion, b'', 1) == before, 'changed code outside the state-6 latch'
assert B.skip_startup_intro(before.replace(b'void sub_A05C0D83(void) {', b'void sub_A0000000(void) {')) == before.replace(
    b'void sub_A05C0D83(void) {', b'void sub_A0000000(void) {'), 'changed another function with the same label'
assert B.skip_startup_intro(b'void sub_A0000000(void) {\n}\n') == b'void sub_A0000000(void) {\n}\n'
for broken in (before.replace(b'L_A05C11F9:', b'L_missing:'),
               before.replace(b'L_A05C11F9:', b'L_A05C11F9:\r\nL_A05C11F9:')):
    try:
        B.skip_startup_intro(broken)
    except ValueError:
        pass
    else:
        raise AssertionError('missing/duplicated startup site was accepted')

fixtures = []
for version, data in (('before', before), ('after', after)):
    (OUT / f'{filename}.{version}.c').write_bytes(data)
    start = data.index(b'L_A05C11F9:')
    end = data.index(b'L_A05C120E:', start)
    fixture = (f'void sub_A05C0D83_{version}(void) {{\n'
               'uint32_t _cf=0, _flag_k=FK_NONE, _flag_a=0, _flag_b=0;\n'
               'RECOMP_REGS_LOCALS\nRECOMP_ENTER(0xA05C0D83u);\n').encode() + data[start:end]
    fixture += (b'L_A05C120E: branch=0; goto done;\nL_A05C1EFC: branch=1;\n'
                b'done: RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n')
    fixtures.append(B.localize_registers(fixture))
source = OUT / 'check.c'
source.write_bytes(b'#include "recomp_types.h"\nrecomp_machine_t g_m;\nuint32_t branch, skip_intro;\nstatic void s2_intro_latch(unsigned int owner) { if (skip_intro) MEM8(owner+0x11)=1; }\n' + b'\n'.join(fixtures) +
                   b'uint32_t flags(void) { return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1); }\n'
                   b'_Static_assert(offsetof(recomp_machine_t,m_xmm)==96,"machine layout");\n')
binary = OUT / 'check.elf'
flags = B.COMMON + ['-O2', '-DRECOMP_GENERATED_CODE', '-DRECOMP_LOCAL_REGS', '-DRECOMP_PLAIN_MEMORY',
                    '-Iandroid/runtime', '-fno-stack-protector', '-nostdlib', '-fdata-sections']
subprocess.run([str(B.CLANG)] + flags + ['-Wl,--gc-sections,-e,sub_A05C0D83_before,-u,sub_A05C0D83_after,-u,flags',
               str(source), '-o', str(binary)], cwd=ROOT, check=True)
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000); arm.mem_map(native_stack, 0x10000); arm.mem_map(stop, 0x1000)
arm.mem_map(guest + 0x10000, 0x10000)
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
targets = (0xA05C120E, 0xA05C1EFC)
def stop_at_boundary(machine, address, size, user):
    if address in targets: machine.emu_stop()
x86.hook_add(UC_HOOK_CODE, stop_at_boundary)
def run_arm(entry, memory, initial, xmm, skip=1):
    arm.mem_write(base + symbols['skip_intro'], struct.pack('<I', skip))
    arm.mem_write(guest + 0x10000, memory)
    arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(64) + xmm)
    arm.mem_write(base + symbols['branch'], bytes(4))
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.emu_start(base + symbols[entry], stop, count=1000)
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop
    state = bytes(arm.mem_read(base + symbols['g_m'], 224))
    branch = int.from_bytes(arm.mem_read(base + symbols['branch'], 4), 'little')
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.emu_start(base + symbols['flags'], stop, count=1000)
    return (state[:32], state[96:224], bytes(arm.mem_read(guest + 0x10000, 0x10000)),
            arm.reg_read(A.UC_ARM64_REG_X0) & 0x8D5, branch)
def run_x86(memory, initial, xmm):
    x86.mem_write(0x10000, memory)
    for reg, value in zip(registers, initial): x86.reg_write(reg, value)
    for i in range(8): x86.reg_write(getattr(X, f'UC_X86_REG_XMM{i}'), int.from_bytes(xmm[i*16:i*16+16], 'little'))
    x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    x86.emu_start(0xA05C11F9, 0, count=1000)
    ip = x86.reg_read(X.UC_X86_REG_EIP)
    assert ip in targets
    return (struct.pack('<8I', *(x86.reg_read(reg) for reg in registers)),
            b''.join(x86.reg_read(getattr(X, f'UC_X86_REG_XMM{i}')).to_bytes(16,'little') for i in range(8)),
            bytes(x86.mem_read(0x10000,0x10000)), x86.reg_read(X.UC_X86_REG_EFLAGS)&0x8D5, int(ip==targets[1]))
rng = random.Random(0xA05C11F9)
for case in range(256):
    memory = bytearray(rng.randbytes(0x10000))
    self = 0x11000 + (case % 16) * 0x100
    latch = self + 0x11 - 0x10000
    memory[latch] = (0,1,0x7F,0x80,0xFF)[case%5]
    struct.pack_into('<I', memory, self+0xBC-0x10000, 6)
    initial = [rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(32),0x1F000,
               self,self+0xBC,rng.getrandbits(32),rng.getrandbits(32)]
    xmm = rng.randbytes(128)
    assert run_arm('sub_A05C0D83_before',bytes(memory),initial,xmm) == run_x86(bytes(memory),initial,xmm)
    assert run_arm('sub_A05C0D83_after',bytes(memory),initial,xmm,skip=0) == run_x86(bytes(memory),initial,xmm)
    requested = bytearray(memory); requested[latch] = 1
    expected = run_x86(bytes(requested),initial,xmm)
    assert expected[-1] == 1, 'original did not choose the native skip branch'
    assert run_arm('sub_A05C0D83_after',bytes(memory),initial,xmm) == expected, (case,'patched state disagrees with original input-skip')
    assert expected[2][latch] == 0
    assert struct.unpack_from('<I',expected[2],self+0xBC-0x10000)[0] == 8
print('PASS: 768 original-x86/actual-ARM64 state-6 comparisons, all GPRs/XMMs/flags/guest writes/branches exact')
print('PASS: runtime main-intro choice preserves playback or requests the original skip to state 8')
objdump = B.TOOLCHAIN / 'bin/llvm-objdump.exe'
(OUT/'check.s').write_text(subprocess.check_output([str(objdump),'-d',str(binary)],text=True))
profile = ROOT / 'build/pgo/abb24fae-20261022.profdata'
for version, data in (('before', before), ('after', after)):
    localized = OUT / f'{filename}.{version}.localized.c'
    localized.write_bytes(B.localize_registers(data))
    obj = OUT / f'{filename}.{version}.o'
    command = [str(B.CLANG)] + [f for f in B.GENERATED_FLAGS if f not in ('-w','-DRECOMP_GENERATED_CODE')] + [
        '-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY','-Igenerated/sims2-x86','-c',str(localized),'-o',str(obj)]
    if profile.is_file():command.insert(1,'-fprofile-instr-use='+str(profile))
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (OUT/f'{filename}.{version}.warnings.txt').write_text(result.stderr)
    assert result.returncode==0,result.stderr
    assert '[-Wprofile-instr-out-of-date]' not in result.stderr,result.stderr
    assert '[-Wprofile-instr-unprofiled]' not in result.stderr,result.stderr
    (OUT/f'A05C0D83.{version}.s').write_text(subprocess.check_output([
        str(objdump),'--disassemble-symbols=sub_A05C0D83',str(obj)],text=True))
    print(f'PASS: complete {filename} {version} compile' + (' with production PGO' if profile.is_file() else ''))
    if result.stderr.strip():print(result.stderr.strip())
print(OUT)
