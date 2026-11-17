"""Actual production native A09187BE versus the original x86, without adb."""
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X
import build_android as B

OUT=B.ROOT/'build/test/native-substring';OUT.mkdir(parents=True,exist_ok=True)
production=B.ROOT/'android/host/sims2_fast.c'
assert 'sub_A09187BE' in B.NATIVE_OVERRIDES
source=OUT/'check.c'
source.write_text(r'''#include "host.h"
recomp_machine_t g_m;
unsigned fallback_calls,fixture_mode;
int void_vm_readable(uint32_t a,uint32_t n){
 if(fixture_mode==1 && a==0x12000)return 0;
 return a>=0x10000 && (uint64_t)a+n<=0x90000;
}
int void_vm_writable(uint32_t a,uint32_t n){return fixture_mode!=2 && void_vm_readable(a,n);}
void *memchr(const void *p,int c,size_t n){
 const unsigned char *s=p;
 for(size_t i=0;i<n;++i)if(s[i]==(unsigned char)c)return (void *)(s+i);
 return 0;
}
void sub_A09187BE_lifted(void){++fallback_calls;g_eax=0xFACADE;}
''' + f'#include "{production.as_posix()}"\n' + r'''
uint32_t check(void){sub_A09187BE();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}
''')
binary=OUT/'check.elf'
subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-ffunction-sections','-fdata-sections','-fno-stack-protector',
                '-nostdlib','-Iandroid/runtime','-Iandroid/host','-Wl,--gc-sections,-e,check',str(source),'-o',str(binary)],
                cwd=B.ROOT,check=True)
elf=ELFFile(io.BytesIO(binary.read_bytes()))
symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
code,guest,native_sp,stop=0x10000000,0x2000000000,0x50000000,0x60000000
arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
arm.mem_map(code,0x200000);arm.mem_map(guest+0x10000,0x80000)
arm.mem_map(native_sp,0x10000);arm.mem_map(stop,0x1000)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD':arm.mem_write(code+segment['p_vaddr'],segment.data())
relocs=elf.get_section_by_name('.rela.dyn')
for reloc in (relocs.iter_relocations() if relocs else []):
    assert reloc['r_info_type']==1027
    arm.mem_write(code+reloc['r_offset'],struct.pack('<Q',code+reloc['r_addend']))
pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True)
image=pe.get_memory_mapped_image();x86=Uc(UC_ARCH_X86,UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase,(len(image)+65535)&~65535)
x86.mem_write(pe.OPTIONAL_HEADER.ImageBase,image);x86.mem_map(0x10000,0x80000);x86.mem_map(stop,0x1000)
regs=[X.UC_X86_REG_EAX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX,X.UC_X86_REG_ESP,
      X.UC_X86_REG_EBX,X.UC_X86_REG_ESI,X.UC_X86_REG_EDI,X.UC_X86_REG_EBP]
rng=random.Random(0xA09187BE)
def setup(hay,needle,start,end,base=0x18000,key=0x12000,self=0x11000):
    memory=bytearray(rng.randbytes(0x80000));sp=0x80000
    memory[base-0x10000:base-0x10000+len(hay)]=hay
    memory[key-0x10000:key-0x10000+len(needle)+1]=needle+b'\0'
    struct.pack_into('<I',memory,self+8-0x10000,base)
    initial=[rng.getrandbits(32) for _ in range(8)];initial[3]=sp
    stack=struct.pack('<5I',stop,self,key,start&0xFFFFFFFF,end&0xFFFFFFFF)
    memory[sp-0x10000:sp-0x10000+20]=stack
    return memory,initial
def run(memory,initial,original=True,mode=0):
    if original:
        x86.mem_write(0x10000,bytes(memory))
        for reg,value in zip(regs,initial):x86.reg_write(reg,value)
        x86.reg_write(X.UC_X86_REG_EFLAGS,0x202)
        x86.emu_start(0xA09187BE,stop,count=5000000)
        assert x86.reg_read(X.UC_X86_REG_EIP)==stop
    arm.mem_write(guest+0x10000,bytes(memory))
    arm.mem_write(code+symbols['g_m'],struct.pack('<8I',*initial)+bytes(256))
    arm.mem_write(code+symbols['fallback_calls'],bytes(4));arm.mem_write(code+symbols['fixture_mode'],struct.pack('<I',mode))
    arm.reg_write(A.UC_ARM64_REG_SP,native_sp+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(code+symbols['check'],stop,count=5000000)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop
    actual=struct.unpack('<8I',arm.mem_read(code+symbols['g_m'],32))
    fallback=struct.unpack('<I',arm.mem_read(code+symbols['fallback_calls'],4))[0]
    if original:
        assert not fallback
        expected=tuple(x86.reg_read(reg) for reg in regs)
        assert actual==expected,(actual,expected)
        assert (arm.reg_read(A.UC_ARM64_REG_X0)^x86.reg_read(X.UC_X86_REG_EFLAGS))&0x8D5==0,('flags',actual,expected)
        assert bytes(arm.mem_read(guest+0x10000,0x80000))==bytes(x86.mem_read(0x10000,0x80000)),('guest memory',actual)
    return actual,fallback

cases=0
corners=[(b'xabc',b'abc',0,2),(b'a\0',b'',0,2),(b'xxxx',b'abc',0,4),
         (b'abX',b'abc',0,1),(b'abXzzz',b'abc',0,6),(b'aaaaaab',b'aaab',0,4),
         (b'abc',b'abc',1,1),(b'abc',b'abc',2,1),(b'abc',b'abc',-1,-2),
         (b'abc',b'abc',0x7FFFFFFF,-1),(b'\0',b'',0,1),(b'abc',b'\0ignored',0,3)]
for hay,needle,start,end in corners:
    memory,initial=setup(hay,needle,start,end);run(memory,initial);cases+=1
for index in range(1536):
    alphabet=bytes(range(256)) if index%4 else b'aaaaab'
    hay=bytes(rng.choice(alphabet) for _ in range(rng.randrange(1,385)))
    needle=bytes(rng.choice(alphabet[1:] or b'x') for _ in range(rng.randrange(48)))
    if index%3==0:
        pos=rng.randrange(len(hay));needle=hay[pos:pos+rng.randrange(1,24)].split(b'\0')[0]
    start=rng.randrange(len(hay)+1);end=rng.randrange(len(hay)+1)
    memory,initial=setup(hay,needle,start,end)
    if index%11==0:
        # Alias both read-only inputs; the context itself remains separate.
        key=0x18000+rng.randrange(len(hay));struct.pack_into('<I',memory,0x80008-0x10000,key)
        memory[0x18000-0x10000+len(hay)]=0
    run(memory,initial);cases+=1

fallback_cases=0
for kind in ('unreadable-needle','readonly-stack','long-needle','negative-start','large-work','overflow-base',
             'scratch-needle','scratch-hay','scratch-context','bad-context'):
    memory,initial=setup(b'abcabcabc\0',b'abc',0,9);mode=0
    def put(at,value):struct.pack_into('<I',memory,at-0x10000,value)
    if kind=='unreadable-needle':mode=1
    if kind=='readonly-stack':mode=2
    if kind=='long-needle':memory[0x2000:0x3000]=b'a'*4096
    if kind=='negative-start':put(0x8000C,0xFFFFFFFF)
    if kind=='large-work':put(0x80010,0x1000000)
    if kind=='overflow-base':put(0x11008,0xFFFFFFFE)
    if kind=='scratch-needle':put(0x80008,0x7FFF0)
    if kind=='scratch-hay':put(0x11008,0x7FFF0);put(0x80010,32)
    if kind=='scratch-context':put(0x80004,0x7FFF0)
    if kind=='bad-context':put(0x80004,0xFFFFFFFF)
    actual,fallback=run(memory,initial,False,mode)
    assert fallback==1 and actual[0]==0xFACADE and actual[1:]==tuple(initial[1:]),(kind,actual)
    assert bytes(arm.mem_read(guest+0x10000,0x80000))==bytes(memory),('fallback mutated memory',kind)
    fallback_cases+=1
result=dict(result='PASS',production_source=True,differential_cases=cases,fallback_cases=fallback_cases,
            original='Actual x86 A09187BE without API stubs',native='Actual production sims2_fast.c compiled AArch64; VM queries and memchr explicit boundaries',
            source_sha256=hashlib.sha256(production.read_bytes()).hexdigest(),
            compared='All 8 GPRs, arithmetic flags and all mapped guest memory including exact scratch/argument writes',
            limits='Worst-case compared-byte work <=16MiB; needle readable window4096. No device or performance measurement.')
(OUT/'result.json').write_text(json.dumps(result,indent=2))
print(f'PASS native substring: {cases} original-x86/production-AArch64 cases, {fallback_cases} untouched-state fallback cases; all GPRs, flags, exact guest memory')
