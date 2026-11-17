"""Isolated exact-dot inlining proof. No production sources or device touched.

Checks the actual original x86 leaf and actual before/after AArch64
code; a complete PGO unit compile must retain all trained CFG hashes.
"""
import io
import json
from pathlib import Path
import random
import re
import struct
import subprocess

import build_android as B
from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/dot-inline'
OUT.mkdir(parents=True,exist_ok=True)


def function(text, name):
    start = text.index(f'void {name}(void) {{'.encode())
    end = text.index(b'\n}\n', start) + 3
    return text[start:end]


callee = function((ROOT/'generated/sims2-x86/sims2_A01A0000.c').read_text().encode(), 'sub_A01A6840')
original = (ROOT/'generated/sims2-x86/sims2_A0AF0000.c').read_text().encode()
if b'goto _ljump; }' in original:
    original=B.direct_jump_tables(original,B.module_image('sims2'))
patched = B.inline_dot(original)
assert B.inline_dot(original,callee)==patched
assert B.inline_dot(original.replace(b'\n',b'\r\n'),callee.replace(b'\n',b'\r\n')) == patched.replace(b'\n',b'\r\n')
assert B.inline_dot(b'void sub_A0000000(void) {\n}\n', callee) == b'void sub_A0000000(void) {\n}\n'
for site in B.DOT_INLINE_CALLS:
    try:
        B.inline_dot(original.replace(f'0x{site:08X}: call'.encode(), b'changed: call'), callee)
    except ValueError:
        pass
    else:
        raise AssertionError('changed dot site accepted')
assert patched.count(b'g_cur_func = 0xA01A6840u;') == 6
assert patched.count(b'    PUSH32(esp, RECOMP_RETADDR);\n    RECOMP_FLAGS_OUT();\n    g_cur_func = 0xA01A6840u;')==6
for bad_callee in (callee.replace(b'MEM32(ecx + 0x4)',b'MEM32(ecx + 0x8)',1),callee+b'\n'):
    try:B.inline_dot(original,bad_callee)
    except ValueError:pass
    else:raise AssertionError('changed original leaf accepted')
try:B.inline_dot(original+function(original,'sub_A0AF25A0'),callee)
except ValueError:pass
else:raise AssertionError('duplicate caller accepted')
assert patched.replace(B.DOT_INLINE_HELPER, b'').split(b'void sub_A0AF25A0(void) {')[0] == original.split(b'void sub_A0AF25A0(void) {')[0]
assert patched[patched.index(b'\n}\n', patched.index(b'void sub_A0AF25A0(void) {')):] == original[original.index(b'\n}\n', original.index(b'void sub_A0AF25A0(void) {')):]

new_start = patched.index(b'    /* Exact leaf inline:')
new_end = patched.index(b'    g_cur_func = 0xA0AF25A0u;', new_start) + len(b'    g_cur_func = 0xA0AF25A0u;')
new_call = patched[new_start:new_end]
old_call = b'    RECOMP_CALL(sub_A01A6840);\n    RECOMP_FLAGS_IN();'
declarations = b'uint32_t _cf=0, _flag_k=FK_NONE, _flag_a=0, _flag_b=0; int _df=1;\nRECOMP_REGS_LOCALS\nRECOMP_ENTER(0xA0AF25A0u);\n'
fixtures = []
for version, body in [('before', old_call), ('after', new_call)]:
    fixtures.append(B.localize_registers(b'void sub_A0AF25A0_'+version.encode()+b'(void) {\n'+declarations+body+b'\nRECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();\n}\n'))
prefix = (b'#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
          b'recomp_machine_t g_m; x87_window_t g_x87; int g_fp_bias;\n'
          b'uint32_t g_enter_trace[1024], g_enter_idx;\n'
          b'void *memcpy(void *dst,const void *src,size_t n) { unsigned char *d=dst; const unsigned char *s=src; for(size_t i=0;i<n;i++)d[i]=s[i];return dst;}\n'
          b'uint32_t flags(void) { return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1); }\n')
source = OUT/'check.c'
source.write_bytes(prefix + B.DOT_INLINE_HELPER + B.localize_registers(callee) + b'\n'.join(fixtures))
binary = OUT/'check.elf'
subprocess.run([str(B.CLANG)] + B.COMMON + ['-O2','-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY',
    '-Iandroid/runtime','-fno-stack-protector','-fno-builtin','-nostdlib','-fdata-sections',
    '-Wl,--gc-sections,-e,sub_A0AF25A0_before,-u,sub_A0AF25A0_after,-u,flags',str(source),'-o',str(binary)], cwd=ROOT, check=True)
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0xDEAD0000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base,0x200000); arm.mem_map(native_stack,0x10000); arm.mem_map(stop,0x1000)
arm.mem_map(guest+0x10000,0x20000)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD': arm.mem_write(base+segment['p_vaddr'],segment.data())
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
x86.mem_map(0x10000,0x20000); x86.mem_map(stop,0x1000)
# Store/pop all eight x87 slots into a private test output, preserving EFLAGS.
dump=0x2F000
dump_code=stop+0x400
x86.mem_write(dump_code,b''.join(b'\xDD\x1D'+struct.pack('<I',dump+i*8) for i in range(8)))
registers=[X.UC_X86_REG_EAX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX,X.UC_X86_REG_ESP,
           X.UC_X86_REG_EBX,X.UC_X86_REG_ESI,X.UC_X86_REG_EDI,X.UC_X86_REG_EBP]
rng=random.Random(0xA01A6840)
counts={'before':0,'after':0}


def run_arm(version,memory,initial,xmm,eflags,cw,window,sp_index=28,measure=False):
    arm.mem_write(guest+0x10000,memory)
    state=bytearray(224)
    struct.pack_into('<8I',state,0,*initial)
    struct.pack_into('<4I',state,32,8,eflags,0,eflags&1) # FK_EFLAGS
    struct.pack_into('<I',state,48,0xA0AF25A0)
    struct.pack_into('<iIH',state,64,-1,0x1F80,cw)
    struct.pack_into('<Q',state,80,base+symbols['g_x87']+8*sp_index)
    state[96:224]=xmm
    arm.mem_write(base+symbols['g_m'],bytes(state))
    arm.mem_write(base+symbols['g_x87'],window+bytes(512))
    arm.mem_write(base+symbols['g_fp_bias'],struct.pack('<i',0))
    if 'g_enter_idx' in symbols:arm.mem_write(base+symbols['g_enter_idx'],bytes(4))
    arm.reg_write(A.UC_ARM64_REG_SP,native_stack+0xF000); arm.reg_write(A.UC_ARM64_REG_LR,stop)
    hook=None
    if measure:
        arm.ctl_remove_cache(base,base+0x200000)
        def tick(uc,address,size,user): counts[version]+=1
        hook=arm.hook_add(UC_HOOK_CODE,tick)
    arm.emu_start(base+symbols['sub_A0AF25A0_'+version],stop,count=100000)
    if hook: arm.hook_del(hook)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop
    machine=bytes(arm.mem_read(base+symbols['g_m'],224))
    output=bytes(arm.mem_read(guest+0x10000,0x20000))
    fpwindow=bytes(arm.mem_read(base+symbols['g_x87'],1024))
    bias=bytes(arm.mem_read(base+symbols['g_fp_bias'],4))
    arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols['flags'],stop,count=1000)
    return machine,output,fpwindow,bias,arm.reg_read(A.UC_ARM64_REG_X0)&0x8D5


cases=0
for case in range(1024):
    memory=bytearray(rng.randbytes(0x20000))
    sp=0x2E000
    left,right=0x18000,0x19000
    # Alias each pointer with every argument/return slot and each other. The
    # actual callee reads both operands before overwriting its first argument.
    alias=case%8
    if alias==1:right=left
    if alias==2:left=sp
    if alias==3:right=sp+4
    if alias==4:left=sp-4
    if alias==5:right=sp-4
    if alias==6:right=left+4
    if alias==7:left=right+4
    values=[rng.uniform(-100,100) for _ in range(8)]
    if case%16==0:values=[0.,-0.,2**-126,2**-140,1.,-1.,0.5,-0.5]
    if case%16==1:values=[1e30,-1e30,1e-30,-1e-30,1e30,1e30,1e30,1e30]
    for ptr,vs in [(left,values[:4]),(right,values[4:])]: struct.pack_into('<4f',memory,ptr-0x10000,*vs)
    struct.pack_into('<3I',memory,sp-4-0x10000,stop,left,right)
    initial=[rng.getrandbits(32) for _ in range(8)]; initial[3]=sp
    xmm=rng.randbytes(128)
    eflags=0x202|(rng.getrandbits(12)&0x8D5)
    cw=(0x37F,0x77F,0xB7F,0xF7F)[case%4]
    old_values=[rng.uniform(-10,10) for _ in range(7)]
    window=bytearray(512)
    for i,v in enumerate(old_values):struct.pack_into('<d',window,(28+i)*8,v)
    before=run_arm('before',bytes(memory),initial,xmm,eflags,cw,bytes(window),measure=case==0)
    after=run_arm('after',bytes(memory),initial,xmm,eflags,cw,bytes(window),measure=case==0)
    assert after==before,('before/after mismatch',case)
    x86.mem_write(0x10000,bytes(memory))
    for reg,value in zip(registers,initial):x86.reg_write(reg,value)
    x86.reg_write(X.UC_X86_REG_ESP,sp-4)
    for i in range(8):x86.reg_write(getattr(X,f'UC_X86_REG_XMM{i}'),int.from_bytes(xmm[i*16:i*16+16],'little'))
    x86.reg_write(X.UC_X86_REG_EFLAGS,eflags);x86.reg_write(X.UC_X86_REG_MXCSR,0x1F80)
    x86.reg_write(X.UC_X86_REG_FPCW,cw)
    x86.reg_write(X.UC_X86_REG_FPSW,0);x86.reg_write(X.UC_X86_REG_FPTAG,0xFFFF)
    # Load old live slots in reverse order before running the original leaf.
    init_at=stop+0x200
    for i,v in enumerate(reversed(old_values)):struct.pack_into('<d',memory,dump+0x100+i*8-0x10000,v)
    x86.mem_write(dump+0x100,bytes(memory[dump+0x100-0x10000:dump+0x100-0x10000+56]))
    init_code=b''.join(b'\xDD\x05'+struct.pack('<I',dump+0x100+i*8) for i in range(7))
    x86.mem_write(init_at,init_code);x86.emu_start(init_at,init_at+len(init_code),count=100)
    x86.emu_start(0xA01A6840,stop,count=1000)
    expected_gpr=struct.pack('<8I',*(x86.reg_read(reg) for reg in registers))
    expected_xmm=b''.join(x86.reg_read(getattr(X,f'UC_X86_REG_XMM{i}')).to_bytes(16,'little') for i in range(8))
    assert after[0][:32]==expected_gpr,(case,'GPR')
    assert after[0][96:224]==expected_xmm,(case,'XMM')
    assert after[4]==x86.reg_read(X.UC_X86_REG_EFLAGS)&0x8D5,(case,'flags')
    xmem=bytes(x86.mem_read(0x10000,0x20000))
    # The private preloading area is test-only, not touched by either leaf.
    assert after[1][:dump+0x100-0x10000]==xmem[:dump+0x100-0x10000],(case,'memory')
    x86.emu_start(dump_code,dump_code+8*6,count=100)
    expected_fp=bytes(x86.mem_read(dump,64))
    actual_fp=after[2][27*8:35*8]
    assert actual_fp==expected_fp,(case,'live x87',actual_fp.hex(),expected_fp.hex())
    assert struct.unpack_from('<H',after[0],72)[0]==cw
    assert struct.unpack_from('<I',after[0],48)[0]==0xA0AF25A0
    cases+=1
print(f'PASS: {cases} original-x86/before-ARM64/after-ARM64 cases; all GPRs, XMMs, live x87, CW, flags and guest writes')
print('Instruction counts (same AArch64 ISA/O2 fixture):',counts)

# Existing cross-ISA NaN payload/FPCR differences belong to the unmodified
# runtime. Inlining must add none: compare every bit of the two ARM paths.
raw_cases=0
special=(0,0x80000000,1,0x80000001,0x7F7FFFFF,0xFF7FFFFF,0x7F800000,0xFF800000,
         0x7FC00001,0x7FC54321,0xFFC12345,0x7F800001,0xFF800001)
for case in range(512):
    memory=bytearray(rng.randbytes(0x20000))
    sp,left,right=0x2E000,0x18000,0x19000
    struct.pack_into('<3I',memory,sp-4-0x10000,stop,left,right)
    for ptr in (left,right):
        vals=[rng.choice(special) if case<256 else rng.getrandbits(32) for _ in range(4)]
        struct.pack_into('<4I',memory,ptr-0x10000,*vals)
    initial=[rng.getrandbits(32) for _ in range(8)];initial[3]=sp
    xmm=rng.randbytes(128)
    kwargs={'sp_index':(0,28,56)[case%3]}
    fpcr=((case%4)<<22)|(1<<24 if case%8>=4 else 0)
    arm.reg_write(A.UC_ARM64_REG_FPCR,fpcr)
    before=run_arm('before',bytes(memory),initial,xmm,0xAD7,0x37F,bytes(512),**kwargs)
    after=run_arm('after',bytes(memory),initial,xmm,0xAD7,0x37F,bytes(512),**kwargs)
    assert after==before,('raw bit/FPCR before-after mismatch',case)
    raw_cases+=1
arm.reg_write(A.UC_ARM64_REG_FPCR,0)
print(f'PASS: {raw_cases} additional ARM64 before/after NaN/Inf/subnormal/raw-bit cases, all host rounding modes/FZ and x87 recenter boundaries')

trace_binary=OUT/'check-trace.elf'
subprocess.run([str(B.CLANG)] + B.COMMON + ['-O2','-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY','-DRECOMP_TRACE',
    '-Iandroid/runtime','-fno-stack-protector','-fno-builtin','-nostdlib','-fdata-sections',
    '-Wl,--gc-sections,-e,sub_A0AF25A0_before,-u,sub_A0AF25A0_after,-u,flags',str(source),'-o',str(trace_binary)], cwd=ROOT, check=True)
trace_elf=ELFFile(io.BytesIO(trace_binary.read_bytes()))
symbols={s.name:s['st_value'] for s in trace_elf.get_section_by_name('.symtab').iter_symbols()}
arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
arm.mem_map(base,0x200000);arm.mem_map(native_stack,0x10000);arm.mem_map(stop,0x1000);arm.mem_map(guest+0x10000,0x20000)
for segment in trace_elf.iter_segments():
    if segment['p_type']=='PT_LOAD':arm.mem_write(base+segment['p_vaddr'],segment.data())
rel=trace_elf.get_section_by_name('.rela.dyn')
if rel:
    for r in rel.iter_relocations():arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
for version in ('before','after'):
    run_arm(version,bytes(memory),initial,xmm,0xAD7,0x37F,bytes(512))
    assert struct.unpack('<I',arm.mem_read(base+symbols['g_enter_idx'],4))[0]==2
    assert struct.unpack('<2I',arm.mem_read(base+symbols['g_enter_trace'],8))==(0xA0AF25A0,0xA01A6840)
print('PASS: optional trace caller/callee entries and caller VA restore')

B.OUT=OUT/'pipeline'
production_path,_=B.override_source(ROOT/'generated/sims2-x86/sims2_A0AF0000.c',True)
assert production_path.read_bytes().replace(b'\r\n',b'\n')==B.localize_registers(patched).replace(b'\r\n',b'\n')
print('PASS: real override_source pipeline emits the checked source; generated input remains unchanged')

objdump=B.TOOLCHAIN/'bin/llvm-objdump.exe'
(OUT/'check.s').write_text(subprocess.check_output([str(objdump),'-d',str(binary)],text=True))
profile=ROOT/'build/pgo/abb24fae-20261022.profdata'
sizes={}
for version,data in [('before',original),('after',patched)]:
    path=OUT/f'sims2_A0AF0000.{version}.c'
    path.write_bytes(B.localize_registers(data))
    obj=OUT/f'sims2_A0AF0000.{version}.o'
    command=[str(B.CLANG)]+[f for f in B.GENERATED_FLAGS if f not in ('-w','-DRECOMP_GENERATED_CODE')]+['-Wno-empty-body','-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY',
        '-Igenerated/sims2-x86','-c',str(path),'-o',str(obj)]
    if profile.is_file():command.insert(1,'-fprofile-instr-use='+str(profile))
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (OUT/f'{version}.warnings.txt').write_text(result.stderr)
    assert result.returncode==0,result.stderr
    assert '[-Wprofile-instr-out-of-date]' not in result.stderr,result.stderr
    assert '[-Wprofile-instr-unprofiled]' not in result.stderr,result.stderr
    assert not result.stderr.strip(),result.stderr
    assembly=subprocess.check_output([str(objdump),'--disassemble-symbols=sub_A0AF25A0',str(obj)],text=True)
    (OUT/f'A0AF25A0.{version}.s').write_text(assembly)
    e=ELFFile(io.BytesIO(obj.read_bytes()))
    sizes[version]=e.get_section_by_name('.symtab').get_symbol_by_name('sub_A0AF25A0')[0]['st_size']
    relocations=[s for s in e.iter_sections() if s.name.startswith('.rela.text.') and s.name.endswith('.sub_A0AF25A0')]
    dot_calls=sum(e.get_section_by_name('.symtab').get_symbol(r['r_info_sym']).name=='sub_A01A6840'
                  for section in relocations for r in section.iter_relocations())
    assert dot_calls==(6 if version=='before' else 0),(version,'remaining dot calls',dot_calls)
    assert not re.search(r'\t(?:fmla|fmls|fmadd|fmsub)\t',assembly), 'unexpected fused operation'
    print('PASS: complete unit',version,'production PGO compile; no profile mismatch/unprofiled warnings')
(OUT/'result.json').write_text(json.dumps({'x86Cases':cases,'additionalArmCases':raw_cases,'fixtureInstructions':counts,'callerNativeBytes':sizes},indent=2))
print('Complete caller native bytes:',sizes)
print(OUT)
