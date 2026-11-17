"""Original x86 locale byte path versus actual native AArch64 bucket lookup; no device."""
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X
import build_android as B

OUT = B.ROOT / 'build/test/native-locale-lookup'
OUT.mkdir(parents=True, exist_ok=True)
source = OUT / 'check.c'
source.write_text(r'''#include "host.h"
recomp_machine_t g_m;
unsigned guard_calls,fallback_calls,fixture_mode,fixture_last_error;
void void_log(const char *format,...){ }
void *memcpy(void *d,const void *s,size_t n){for(size_t i=0;i<n;++i)((char *)d)[i]=((const char *)s)[i];return d;}
int void_vm_readable(uint32_t a,uint32_t n){
 if(fixture_mode==2 && a==0x32000)return 0;
 return a>=0x10000 && (uint64_t)a+n<=0x90000;
}
int void_vm_writable(uint32_t a,uint32_t n){
 if(fixture_mode==4)return 0;
 if(fixture_mode==9 && a==0x30350)return 0;
 return void_vm_readable(a,n);
}
void sub_A01D5B50_lifted(void){++fallback_calls;g_eax=0xFACADE;}
uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va!=0xA0831B2Au || n!=1 || args[0] || self!=g_esp+64) {g_eax=0xBAD;return 0;}
 ++guard_calls;
 fixture_last_error=0;
 MEM32(self)=0x30000;MEM32(self+4)=0x31000;MEM32(self+8)=0x33000;
 MEM8(self+12)=(MEM32(0x30350)&2)==0;
 if(fixture_mode==7 || fixture_mode==8){MEM32(self)=fixture_mode==7?0xFFFFFF00u:0;MEM8(self+12)=1;}
 else MEM32(0x30350)|=2;
 MEM32(0x31094)=fixture_mode==1?0:0x32000;
 if(fixture_mode==5)MEM8(0x32000)=7;
 if(fixture_mode==6)MEM8(0x32041)=0;
 g_eax=0x99999999;g_ecx=0x88888888;g_edx=0x77777777;g_esp+=16;
 g_ebx=1;g_esi=2;g_edi=3;g_ebp=4;g_flag_k=FK_TEST;g_flag_a=g_flag_b=1;
 return self;
}
''' + f'#include "{B.ROOT.as_posix()}/android/host/sims2_fast.c"\n' + r'''
uint32_t check(void){sub_A01D5B50();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}
''')
binary = OUT / 'check.elf'
subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections',
                '-fno-stack-protector', '-nostdlib', '-Iandroid/runtime', '-Iandroid/host',
                '-Wl,--gc-sections,-e,check', str(source), '-o', str(binary)], cwd=B.ROOT, check=True)
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_sp, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000); arm.mem_map(guest+0x10000, 0x80000)
arm.mem_map(guest+0xA10DF000, 0x1000); arm.mem_map(native_sp, 0x10000); arm.mem_map(stop, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD':arm.mem_write(base+segment['p_vaddr'],segment.data())
for r in (elf.get_section_by_name('.rela.dyn').iter_relocations() if elf.get_section_by_name('.rela.dyn') else []):
    assert r['r_info_type']==1027
    arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True)
image=pe.get_memory_mapped_image();x86=Uc(UC_ARCH_X86,UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase,(len(image)+65535)&~65535);x86.mem_write(pe.OPTIONAL_HEADER.ImageBase,image)
x86.mem_map(0x10000,0x80000);x86.mem_map(stop,0x1000)
regs=[X.UC_X86_REG_EAX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX,X.UC_X86_REG_ESP,
      X.UC_X86_REG_EBX,X.UC_X86_REG_ESI,X.UC_X86_REG_EDI,X.UC_X86_REG_EBP]
getptd_calls=0
def hook(machine,address,size,unused):
    global getptd_calls
    if address==0xA0845ECB:
        getptd_calls+=1
        sp=machine.reg_read(X.UC_X86_REG_ESP)
        machine.reg_write(X.UC_X86_REG_EAX,0x30000)
        machine.reg_write(X.UC_X86_REG_EIP,struct.unpack('<I',machine.mem_read(sp,4))[0])
        machine.reg_write(X.UC_X86_REG_ESP,sp+4)
x86.hook_add(UC_HOOK_CODE,hook)
rng=random.Random(0xA0831B2A)
def run(memory,stack,initial,mode=0,original=True,budget=1000000):
    global getptd_calls
    getptd_calls=0
    if original:
        x86.mem_write(0x10000,bytes(memory));x86.mem_write(initial[3],stack)
        x86.mem_write(0xA10DFCAC,struct.pack('<I',1))
        x86.mem_write(0xA10E002C,struct.pack('<I',0x31000));x86.mem_write(0xA10E0348,struct.pack('<I',0x33000))
        for reg,value in zip(regs,initial):x86.reg_write(reg,value)
        x86.reg_write(X.UC_X86_REG_EFLAGS,0x202)
        x86.emu_start(0xA01D5B50,stop,count=budget)
        assert x86.reg_read(X.UC_X86_REG_EIP)==stop
    arm.mem_write(guest+0x10000,bytes(memory));arm.mem_write(guest+initial[3],stack)
    arm.mem_write(guest+0xA10DFCAC,struct.pack('<I',1))
    arm.mem_write(base+symbols['g_m'],struct.pack('<8I',*initial)+bytes(256))
    for symbol,value in [('guard_calls',0),('fallback_calls',0),('fixture_mode',mode),('fixture_last_error',0x1234)]:
        arm.mem_write(base+symbols[symbol],struct.pack('<I',value))
    arm.reg_write(A.UC_ARM64_REG_SP,native_sp+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols['check'],stop,count=budget)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop
    result=struct.unpack('<8I',arm.mem_read(base+symbols['g_m'],32))
    counts={name:struct.unpack('<I',arm.mem_read(base+symbols[name],4))[0] for name in ('guard_calls','fallback_calls')}
    assert struct.unpack('<I',arm.mem_read(base+symbols['fixture_last_error'],4))[0]==(0 if counts['guard_calls'] else 0x1234)
    return result,counts

def setup(names,query,table,flags=0,heap=False,out=0x17000):
    memory=bytearray(rng.randbytes(0x80000))
    def put(address,value):struct.pack_into('<I',memory,address-0x10000,value)
    def string(at,value,dest):
        value+=b'\0'
        if len(value)<=16 and not heap:
            memory[at-0x10000:at-0x10000+len(value)]=value;cap=15
        else:
            put(at,dest);memory[dest-0x10000:dest-0x10000+len(value)]=value;cap=127
        put(at+16,rng.getrandbits(32));put(at+20,cap)
    self,vector,head,key,sp=0x11000,0x12000,0x13000,0x16000,0x80000
    nodes=[0x14000+i*64 for i in range(len(names))]
    put(self+4,head);put(self+12,vector);put(self+24,0)
    put(vector,nodes[0] if nodes else head);put(vector+4,nodes[-1] if nodes else head)
    for i,node in enumerate(nodes):
        put(node,nodes[i+1] if i+1<len(nodes) else head);put(node+4,nodes[i-1] if i else head)
        string(node+12,names[i],0x18000+i*2048)
    string(key+4,query,0x1E000)
    put(0x3004C,0x31000);put(0x30048,0x33000);put(0x30350,flags);put(0x31094,0x32000)
    memory[0x32000-0x10000:0x32000-0x10000+128]=bytes(table)
    initial=[rng.getrandbits(32),self,rng.getrandbits(32),sp]+[rng.getrandbits(32) for _ in range(4)]
    stack=struct.pack('<4I',stop,out,key,0xBAD12345)
    return memory,stack,initial

cases=0
for index in range(512):
    table=list(range(128))
    for c in range(65,91):table[c]=c+32
    if index%3==1:table[ord('I')]=253
    if index%3==2:
        for c in range(32,127):table[c]=rng.randrange(1,256)
    alphabet=b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-'
    names=[bytes(rng.choice(alphabet) for _ in range(rng.randrange(30))) for _ in range(rng.randrange(1,9))]
    query=rng.choice(names) if index%2 else bytes(rng.choice(alphabet) for _ in range(rng.randrange(30)))
    if index%3:query=query.swapcase()
    if index%7==0:query=query.split(b'.')[0]+b'.package'
    out=0x17000 if index%5 else rng.choice([0x16000,0x12000,0x14000])
    memory,stack,initial=setup(names,query,table,index&3,index&4,out)
    actual,counts=run(memory,stack,initial)
    expected=tuple(x86.reg_read(r) for r in regs)
    assert counts==dict(guard_calls=int(getptd_calls>0),fallback_calls=0),(index,counts,getptd_calls)
    assert actual==expected,(index,actual,expected)
    assert (arm.reg_read(A.UC_ARM64_REG_X0)^x86.reg_read(X.UC_X86_REG_EFLAGS))&0x8D5==0,('flags',index)
    assert bytes(arm.mem_read(guest+0x10000,0x60000))==bytes(x86.mem_read(0x10000,0x60000)),('persistent memory',index)
    assert struct.unpack('<I',arm.mem_read(guest+0x30350,4))[0]==(index&3),('PTD owned bit',index)
    cases+=1

# Every ASCII input, custom I mapping and original guard reuse, including EOF-style NUL termination.
for c in range(1,128):
    table=list(range(128));table[c]=rng.randrange(1,256)
    memory,stack,initial=setup([bytes([c])+b'Z'],b'Iq',table,2)
    actual,counts=run(memory,stack,initial)
    assert actual==tuple(x86.reg_read(r) for r in regs),(c,actual)
    assert counts['fallback_calls']==0
    cases+=1

no_fold_cases=0
for names,query in (([],b'abc'),([b'abc'],b'abc'),([b'different',b'abc'],b'abc'),
                    ([b''],b''),([b'.package'],b''),([b''],b'.package'),
                    ([b'abc.package'],b'abc.saved')):
    memory,stack,initial=setup(names,query,list(range(128)))
    struct.pack_into('<II',memory,0x30048-0x10000,0xBAD00,0xBAD00)
    actual,counts=run(memory,stack,initial)
    assert actual==tuple(x86.reg_read(r) for r in regs)
    assert counts==dict(guard_calls=0,fallback_calls=0) and getptd_calls==0
    assert bytes(arm.mem_read(guest+0x10000,0x60000))==bytes(x86.mem_read(0x10000,0x60000))
    assert (arm.reg_read(A.UC_ARM64_REG_X0)^x86.reg_read(X.UC_X86_REG_EFLAGS))&0x8D5==0
    no_fold_cases+=1

collision_cases=0
for count in (129,512,2048,8192,8193):
    lower=list(range(128))
    for c in range(65,91):lower[c]=c+32
    memory,stack,initial=setup([],b'a',lower)
    def put(at,value):struct.pack_into('<I',memory,at-0x10000,value)
    nodes=[0x34000+i*36 for i in range(count)]
    put(0x12000,nodes[0]);put(0x12004,nodes[-1])
    for i,node in enumerate(nodes):
        put(node,nodes[i+1] if i+1<count else 0x13000)
        put(node+4,nodes[i-1] if i else 0x13000)
        memory[node+12-0x10000:node+14-0x10000]=b'B\0'
        put(node+28,1);put(node+32,15)
    actual,counts=run(memory,stack,initial,original=count<=8192,budget=10000000)
    if count<=8192:
        assert counts==dict(guard_calls=1,fallback_calls=0),(count,counts)
        assert actual==tuple(x86.reg_read(r) for r in regs),(count,actual)
        assert (arm.reg_read(A.UC_ARM64_REG_X0)^x86.reg_read(X.UC_X86_REG_EFLAGS))&0x8D5==0
        assert bytes(arm.mem_read(guest+0x10000,0x6F000))==bytes(x86.mem_read(0x10000,0x6F000))
    else:
        assert counts==dict(guard_calls=0,fallback_calls=1)
        assert actual[0]==0xFACADE and actual[1:]==tuple(initial[1:])
        assert bytes(arm.mem_read(guest+initial[3],16))==stack
    collision_cases+=1

fallbacks=0
for kind in ('high-key','high-node','stack-output','stack-key','unreadable-stack','null-table','unreadable-table',
             'nonzero-nul','mapped-to-nul','long-key','too-many-nodes','ptd-output','locale-output','table-output',
             'ptd-key','ptd-node','ptd-key-descriptor','ptd-bucket','ptd-self','invalid-ptd','null-ptd','readonly-ptd'):
    names=[b'ABC'];query=b'abc';mode=0;out=0x17000
    if kind=='high-key':query=b'\x80name'
    if kind=='high-node':names=[b'\x80name']
    if kind=='stack-output':out=0x7FFF0
    if kind=='unreadable-stack':mode=4
    if kind=='null-table':mode=1
    if kind=='unreadable-table':mode=2
    if kind=='nonzero-nul':mode=5
    if kind=='mapped-to-nul':mode=6
    if kind=='invalid-ptd':mode=7
    if kind=='null-ptd':mode=8
    if kind=='readonly-ptd':mode=9
    if kind=='long-key':query=b'a'*1100
    if kind=='ptd-output':out=0x30350
    if kind=='locale-output':out=0x31000
    if kind=='table-output':out=0x32000
    memory,stack,initial=setup(names,query,list(range(128)),2 if kind=='readonly-ptd' else 0,True,out)
    def put(at,value):struct.pack_into('<I',memory,at-0x10000,value)
    if kind=='too-many-nodes':put(0x12000,0x14100);put(0x14004,0x14000)
    if kind=='ptd-key':put(0x16004,0x30340);memory[0x20340:0x20344]=b'abc\0'
    if kind=='ptd-node':put(0x1400C,0x30340);memory[0x20340:0x20344]=b'ABC\0'
    if kind=='ptd-key-descriptor':
        memory[0x20000:0x20018]=memory[0x6004:0x601C]
        stack=stack[:8]+struct.pack('<I',0x2FFFC)+stack[12:]
    if kind=='ptd-bucket':
        memory[0x20000:0x20008]=memory[0x2000:0x2008];put(0x1100C,0x30000)
    if kind=='ptd-self':
        memory[0x20000:0x2001C]=memory[0x1000:0x101C];initial[1]=0x30000
    if kind=='stack-key':
        q=bytearray(stack);struct.pack_into('<I',q,8,0x7FF00);stack=bytes(q)
    before=bytes(memory)
    actual,counts=run(memory,stack,initial,mode,False)
    assert counts['fallback_calls']==1,(kind,counts)
    assert counts['guard_calls']==(0 if kind in ('high-key','high-node','stack-output','stack-key','unreadable-stack','long-key','too-many-nodes') else 1),(kind,counts)
    assert actual[0]==0xFACADE and actual[1:]==tuple(initial[1:]),(kind,actual,initial)
    assert bytes(arm.mem_read(guest+initial[3],16))==stack,('fallback arguments',kind)
    assert struct.unpack('<I',arm.mem_read(guest+0x30350,4))[0]==(2 if kind=='readonly-ptd' else 0),('fallback PTD bit',kind)
    fallbacks+=1
result=dict(result='PASS',locale_cases=cases,no_fold_cases=no_fold_cases,fallback_cases=fallbacks,collision_cases=collision_cases,
    original='Actual x86 A01D5B50/A0837529/A083732D/A0831B2A/A0846240/A084629E; only initialized CRT PTD getter is a boundary',
    native='Actual production C compiled AArch64; locale constructor and VM queries are explicit boundaries',
    source_sha256=hashlib.sha256((B.ROOT/'android/host/sims2_fast.c').read_bytes()).hexdigest(),
    limits='All GPRs, arithmetic flags and persistent guest memory compared; dead stack scratch differs because locale guard is acquired once, with stack-alias inputs delegated before mutations. No device speedup measured.')
(OUT/'result.json').write_text(json.dumps(result,indent=2))
print(f'PASS locale lookup: {cases} original-x86/AArch64 cases, {no_fold_cases} empty/exact/dot no-constructor cases, {fallbacks} preflight/table/alias fallbacks, {collision_cases} collision boundary cases; PTD ownership, custom locale maps, ABI and persistent memory')
