"""Check Android's absent-property town-flythrough default against original x86.

    python tools/check_town_flythrough.py

Uses actual generated constructor/decision blocks and actual x86 instructions.
Explicit false/true properties bypass the changed default. No device or
production build is used; test binaries and PGO diagnostics live under build/test.
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
OUT = ROOT / 'build/test/town-flythrough'
OUT.mkdir(parents=True, exist_ok=True)
filename = 'sims2_A0360000.c'
original = (ROOT / 'generated/sims2-x86' / filename).read_bytes()
before = B.direct_jump_tables(original, B.module_image('sims2'))
after = B.default_neighborhood_flythrough_off(before)
old = b'MEM8(ebp + (-0x24)) = (uint8_t)(1); /* 0xA0361624'
new = b'MEM8(ebp + (-0x24)) = (uint8_t)(0); /* 0xA0361624'
assert after.count(new) == 1
assert after.replace(new, old, 1) == before, 'changed code outside the absent-property default'
renamed = before.replace(b'void sub_A0361370(void) {', b'void sub_A0000000(void) {')
assert B.default_neighborhood_flythrough_off(renamed) == renamed
assert B.default_neighborhood_flythrough_off(b'void sub_A0000000(void) {\n}\n') == b'void sub_A0000000(void) {\n}\n'
block_start = before.index(b'L_A036161D:')
block = before[block_start:before.index(b'    PUSH32(esp, 0xA0DBB314u);', block_start)]
for broken in (before.replace(b'L_A036161D:', b'L_missing:'),
               before.replace(old, new), before.replace(block, block + block)):
    try:
        B.default_neighborhood_flythrough_off(broken)
    except ValueError:
        pass
    else:
        raise AssertionError('missing/duplicated/changed default was accepted')
for ending in (b'\n', b'\r\n'):
    text = before.replace(b'\r\n', b'\n').replace(b'\n', ending)
    assert B.default_neighborhood_flythrough_off(text).replace(new, old, 1) == text

fixtures = []
for version, data in (('before', before), ('after', after)):
    (OUT / f'{filename}.{version}.c').write_bytes(data)
    a = data.index(b'L_A0361619:')
    b = data.index(b'    RECOMP_CALL(sub_A01773E0);', a)
    body = data[a:b]
    body += b'branch=1; goto done;\nL_A036166C: branch=0;\n'
    fixture = (f'void default_{version}(void) {{\n'
               'uint32_t _cf=0, _flag_k=FK_NONE, _flag_a=0, _flag_b=0;\n'
               'RECOMP_REGS_LOCALS\nRECOMP_ENTER(0xA0361370u);\n').encode() + body
    fixture += b'done: RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n'
    fixtures.append(B.localize_registers(fixture))

# Both decision blocks are unchanged production code. Their boundary labels
# name exactly the original start-flythrough/no-flythrough and UI-ready routes.
for name, a, b, yes, no in [
    ('property_branch', b'    /* cmp MEM8(ebp + (-0x10)), 0 */ /* 0xA036123C:', b'L_A0361246:', b'L_A0361246:', b'L_A036124D:'),
    ('camera_branch', b'L_A036125C:', b'L_A0361262:', b'L_A0361262:', b'L_A03612C2:'),
]:
    a = before.index(a); b = before.index(b, a)
    fixture = (f'void {name}(void) {{\n'
               'uint32_t _cf=0, _flag_k=FK_NONE, _flag_a=0, _flag_b=0;\n'
               'RECOMP_REGS_LOCALS\nRECOMP_ENTER(0xA0361180u);\n').encode() + before[a:b]
    fixture += yes + b' branch=1; goto done;\n' + no + b' branch=0;\n'
    fixture += b'done: RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n'
    fixtures.append(B.localize_registers(fixture))

source = OUT / 'check_default.c'
source.write_bytes(b'#include "recomp_types.h"\nrecomp_machine_t g_m;\nuint32_t branch;\n' + b'\n'.join(fixtures) +
    b'uint32_t flags(void) { return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1); }\n'
    b'_Static_assert(offsetof(recomp_machine_t,m_xmm)==96,"machine layout");\n')
binary = OUT / 'check_default.elf'
flags = B.COMMON + ['-O2', '-DRECOMP_GENERATED_CODE', '-DRECOMP_LOCAL_REGS', '-DRECOMP_PLAIN_MEMORY',
    '-Iandroid/runtime', '-fno-stack-protector', '-nostdlib', '-fdata-sections']
subprocess.run([str(B.CLANG)] + flags + ['-Wl,--gc-sections,-e,default_before,-u,default_after,-u,property_branch,-u,camera_branch,-u,flags',
    str(source), '-o', str(binary)], cwd=ROOT, check=True)
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
for address, size in [(base,0x200000),(native_stack,0x10000),(stop,0x1000),(guest+0x10000,0x10000)]:
    arm.mem_map(address,size)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD':arm.mem_write(base+segment['p_vaddr'],segment.data())
relocations=elf.get_section_by_name('.rela.dyn')
if relocations:
    for r in relocations.iter_relocations():
        assert r['r_info_type']==1027
        arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
pe=pefile.PE(str(ROOT/'generated/input/Sims2EP9.exe'),fast_load=True)
image=pe.get_memory_mapped_image()
x86=Uc(UC_ARCH_X86,UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase,(len(image)+0xFFFF)&~0xFFFF)
x86.mem_write(pe.OPTIONAL_HEADER.ImageBase,image)
x86.mem_map(0x10000,0x10000)
registers=[X.UC_X86_REG_EAX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX,X.UC_X86_REG_ESP,
    X.UC_X86_REG_EBX,X.UC_X86_REG_ESI,X.UC_X86_REG_EDI,X.UC_X86_REG_EBP]
boundaries=set()
def stop_at_boundary(machine,address,size,user):
    if address in boundaries:machine.emu_stop()
x86.hook_add(UC_HOOK_CODE,stop_at_boundary)

def run_arm(entry,memory,initial,xmm):
    arm.mem_write(guest+0x10000,memory)
    arm.mem_write(base+symbols['g_m'],struct.pack('<8I',*initial)+bytes(64)+xmm)
    arm.mem_write(base+symbols['branch'],bytes(4))
    arm.reg_write(A.UC_ARM64_REG_SP,native_stack+0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols[entry],stop,count=1000)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop
    state=bytes(arm.mem_read(base+symbols['g_m'],224))
    branch=int.from_bytes(arm.mem_read(base+symbols['branch'],4),'little')
    arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols['flags'],stop,count=1000)
    return state[:32],state[96:224],bytes(arm.mem_read(guest+0x10000,0x10000)),arm.reg_read(A.UC_ARM64_REG_X0)&0x8D5,branch

def run_x86(entry,targets,memory,initial,xmm):
    boundaries.clear();boundaries.update(targets)
    x86.mem_write(0x10000,memory)
    for reg,value in zip(registers,initial):x86.reg_write(reg,value)
    for i in range(8):x86.reg_write(getattr(X,f'UC_X86_REG_XMM{i}'),int.from_bytes(xmm[i*16:i*16+16],'little'))
    x86.reg_write(X.UC_X86_REG_EFLAGS,0x202)
    x86.emu_start(entry,0,count=1000)
    ip=x86.reg_read(X.UC_X86_REG_EIP);assert ip in targets
    return (struct.pack('<8I',*(x86.reg_read(reg) for reg in registers)),
        b''.join(x86.reg_read(getattr(X,f'UC_X86_REG_XMM{i}')).to_bytes(16,'little') for i in range(8)),
        bytes(x86.mem_read(0x10000,0x10000)),x86.reg_read(X.UC_X86_REG_EFLAGS)&0x8D5,int(ip==targets[1]))

rng=random.Random(0xA0361624);count=0
for case in range(256):
    memory=bytearray(rng.randbytes(0x10000));frame=0x18000
    presence=(0,1,0x7F,0x80,0xFF)[case%5]
    explicit=case&1;memory[0x1000]=explicit
    initial=[rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(32),0x1F000,
        (rng.getrandbits(24)<<8)|presence,0x11000,rng.getrandbits(32),frame]
    xmm=rng.randbytes(128)
    expected=run_x86(0xA0361619,(0xA036166C,0xA0361634),bytes(memory),initial,xmm)
    assert run_arm('default_before',bytes(memory),initial,xmm)==expected
    changed=bytearray(expected[2])
    if not presence:changed[frame-0x24-0x10000]=0
    expected=expected[:2]+(bytes(changed),)+expected[3:]
    assert run_arm('default_after',bytes(memory),initial,xmm)==expected,(case,'default')
    assert expected[2][0x1000]==explicit
    count+=2
    value=(0,1,0x7F,0x80,0xFF)[case%5]
    memory[frame-0x10-0x10000]=value
    expected=run_x86(0xA036123C,(0xA036124D,0xA0361246),bytes(memory),initial,xmm)
    assert run_arm('property_branch',bytes(memory),initial,xmm)==expected
    assert expected[-1]==bool(value)
    count+=1
    struct.pack_into('<I',memory,initial[5]+0x18-0x10000,0x13000 if value else 0)
    expected=run_x86(0xA036125C,(0xA03612C2,0xA0361262),bytes(memory),initial,xmm)
    assert run_arm('camera_branch',bytes(memory),initial,xmm)==expected
    assert expected[-1]==bool(value)
    count+=1
print(f'PASS: {count} original-x86/actual-ARM64 constructor/decision comparisons; GPRs/XMMs/flags/memory/branches exact')
print('PASS: one absent-property default byte only; explicit false/true bypass default; original property and camera branches retained')

profile=ROOT/'build/pgo/abb24fae-20261022.profdata'
for version,data in [('before',before),('after',after)]:
    localized=OUT/f'{filename}.{version}.localized.c';localized.write_bytes(B.localize_registers(data))
    obj=OUT/f'{filename}.{version}.o'
    command=[str(B.CLANG)]+[f for f in B.GENERATED_FLAGS if f not in ('-w','-DRECOMP_GENERATED_CODE')]+[
        '-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY','-Igenerated/sims2-x86','-c',str(localized),'-o',str(obj)]
    if profile.is_file():command.insert(1,'-fprofile-instr-use='+str(profile))
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (OUT/f'{filename}.{version}.warnings.txt').write_text(result.stderr)
    assert result.returncode==0,result.stderr
    assert '[-Wprofile-instr-out-of-date]' not in result.stderr,result.stderr
    assert '[-Wprofile-instr-unprofiled]' not in result.stderr,result.stderr
    print(f'PASS: complete {filename} {version} compile'+(' with production PGO' if profile.is_file() else ''))
    if result.stderr.strip():print(result.stderr.strip())
print(OUT)
