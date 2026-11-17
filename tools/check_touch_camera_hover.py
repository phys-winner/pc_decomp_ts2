"""Canonical camera guard matches the full original held-input handler path."""
import hashlib
import io
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/test/touch-camera-hover';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'tools'))
import build_android as B
from check_virtual_query import function
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A

original=function((ROOT/'generated/sims2-x86/sims2_A0688000.c').read_text(),'void sub_A068C624(void) {')
anchor='L_A068D4EA:\n'
assert original.count(anchor)==1
insertion='    if(s2_touch_camera_hover_active())goto L_A068D4F5;\n'
patched=B.touch_camera_hover_guard(original.encode()).decode()
assert patched.startswith('extern int s2_touch_camera_hover_active(void);\n')
patched=patched.split('\n',1)[1].replace('void sub_A068C624(void)','void sub_A068C624_native(void)',1)
crlf=B.touch_camera_hover_guard(original.replace('\n','\r\n').encode()).decode()
assert crlf.replace('\r\n','\n')==('extern int s2_touch_camera_hover_active(void);\n'+patched.replace('void sub_A068C624_native(void)','void sub_A068C624(void)',1))
assert B.touch_camera_hover_guard(b'void unrelated(void) {}')==b'void unrelated(void) {}'
assert patched.replace('void sub_A068C624_native(void)','void sub_A068C624(void)',1).replace(insertion,'',1)==original
header='#include "recomp_types.h"\n#include "sims2_funcs.h"\nint s2_touch_camera_hover_active(void);\n'
for name,text in (('original',original),('patched',patched)):
    (OUT/(name+'.c')).write_bytes(B.localize_registers((header+text).encode()))
seh=function((ROOT/'generated/sims2-x86/sims2_A0820000.c').read_text(),'void sub_A08248B0(void) {')
(OUT/'seh.c').write_bytes(B.localize_registers(('#include "recomp_types.h"\n'+seh).encode()))
model=r'''
#include "host.h"
recomp_machine_t g_m;
uint32_t g_icall_trace[128],g_icall_from[128],g_icall_trace_idx,g_icall_count;
uint32_t g_enter_trace[1024],g_enter_idx;
void (*recomp_yield_hook)(void);
unsigned active,guard_count,hover_count;
uint32_t hover_args[2];
int s2_touch_camera_hover_active(void){++guard_count;return active;}
void sub_A068B782(void){
 ++hover_count;hover_args[0]=MEM32(g_esp+4);hover_args[1]=MEM32(g_esp+8);
 MEM32(g_ecx+0x70)^=MEM32(hover_args[0])+MEM32(hover_args[0]+4)+hover_args[1];
 g_eax=0xA1145678;g_ecx=0xC1234567;g_edx=0xD1234567;
 g_flag_k=FK_CMP;g_flag_a=9;g_flag_b=15;g_flag_cf=1;g_esp+=12;
}
void void_lift_trap(uint32_t a,const char *b){(void)a;(void)b;__builtin_trap();}
void sub_A068C624(void),sub_A068C624_native(void);
void reset(void){guard_count=hover_count=hover_args[0]=hover_args[1]=g_enter_idx=0;}
uint32_t run_original(void){sub_A068C624();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}
uint32_t run_patched(void){sub_A068C624_native();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}
'''
(OUT/'model.c').write_text(model)
profile=ROOT/'build/pgo/abb24fae-20261022.profdata'
commands=[]
for name in ('original','patched','seh'):
    commands.append([str(B.CLANG),*B.GENERATED_FLAGS,'-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY',
        '-Igenerated/sims2-x86','-fprofile-instr-use='+profile.as_posix(),'-c',str(OUT/(name+'.c')),'-o',str(OUT/(name+'.o'))])
commands.append([str(B.CLANG),*B.HOST_FLAGS,'-fno-builtin','-fno-stack-protector','-mno-outline-atomics',
                 '-c',str(OUT/'model.c'),'-o',str(OUT/'model.o')])
for command in commands:subprocess.run(command,cwd=ROOT,check=True)
defined=set();undefined=set()
for name in ('original','patched','seh','model'):
    elf=ELFFile(io.BytesIO((OUT/(name+'.o')).read_bytes()))
    for sym in elf.get_section_by_name('.symtab').iter_symbols():
        if sym.name:(undefined if sym['st_shndx']=='SHN_UNDEF' else defined).add(sym.name)
zero=sorted(undefined-defined)
command=[str(B.CLANG),'--target='+B.TARGET,'-nostdlib','-no-pie',
    *[str(OUT/(name+'.o')) for name in ('original','patched','seh','model')],
    '-Wl,-e,run_original',*['-Wl,--defsym='+name+'=0' for name in zero],'-o',str(OUT/'proof.elf')]
subprocess.run(command,cwd=ROOT,check=True);commands.append(command)
elf=ELFFile(io.BytesIO((OUT/'proof.elf').read_bytes()))
syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
sizes={s.name:s['st_size'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM);stack=0x50000000;stop=0x60000000;guest=0x2000000000
arm.mem_map(0x200000,0x100000);arm.mem_map(stack,65536);arm.mem_map(stop,4096)
arm.mem_map(guest+0x10000,0x20000)
for seg in elf.iter_segments():
    if seg['p_type']=='PT_LOAD':arm.mem_write(seg['p_vaddr'],seg.data())
def word(name,value=None):
    if value is not None:arm.mem_write(syms[name],struct.pack('<I',value))
    return struct.unpack('<I',arm.mem_read(syms[name],4))[0]
def call(name):
    arm.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(syms[name],stop,count=1000000)
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop,name+' runaway/unmodeled path'
    return arm.reg_read(A.UC_ARM64_REG_X0)
rng=random.Random(0xA068D4E5);counts={'inactive':0,'held_reference':0,'already_held':0,'other_events':0,'hover_calls':0}
for event in (3,10,0,1,2,5,6,7,8,9,11,12):
    for flags in (0,1,2,4,0x8000,0x10000,0x2000000,0x2010000):
        for enabled in (0,1):
            for case in range(4):
                memory=bytearray(rng.randbytes(0x20000));state=bytearray(rng.randbytes(sizes['g_m']))
                sp,self,point=0x2F000,0x12000,0x13000
                if case==1:point=self+0x28
                gpr=[rng.getrandbits(32) for _ in range(8)];gpr[1]=self;gpr[3]=sp
                struct.pack_into('<8I',state,0,*gpr)
                struct.pack_into('<I',state,60,0x11000)
                def put(at,value):struct.pack_into('<I',memory,at-0x10000,value&0xFFFFFFFF)
                for i,v in enumerate((stop,flags,event,point,rng.getrandbits(32))):put(sp+i*4,v)
                put(point,rng.getrandbits(24));put(point+4,rng.getrandbits(24))
                reference_flags=flags
                held_reference=bool(enabled and event in (3,10) and not(flags&0xFFFF))
                if held_reference:reference_flags|=1
                results=[]
                for name,f in (('run_original',reference_flags),('run_patched',flags)):
                    image=bytearray(memory);struct.pack_into('<I',image,sp+4-0x10000,f)
                    arm.mem_write(guest+0x10000,bytes(image));arm.mem_write(syms['g_m'],bytes(state))
                    call('reset');word('active',enabled)
                    eflags=call(name)
                    result=(bytes(arm.mem_read(syms['g_m'],sizes['g_m'])),eflags,
                            bytes(arm.mem_read(guest+0x10000,len(memory))),word('hover_count'),
                            bytes(arm.mem_read(syms['hover_args'],8)))
                    results.append(result)
                assert results[0]==results[1],(event,hex(flags),enabled,case,'state mismatch')
                expected_hover=int(event in (3,10) and not(flags&0xFFFF) and not enabled)
                assert results[1][3]==expected_hover,(event,flags,enabled,'hover count')
                assert word('guard_count')==int(event in (3,10) and not(flags&0xFFFF)),(event,'unexpected guard reach')
                key='other_events' if event not in (3,10) else 'held_reference' if held_reference else 'already_held' if flags&0xFFFF else 'inactive'
                counts[key]+=1;counts['hover_calls']+=expected_hover
receipt=dict(status='PASS',counts=counts,commands=commands,unreached_symbols_zero=zero,
    limits='Actual full original/patched handler and actual SEH prologue; hover callback modeled. Full machine/XMM/flags/guest memory compared. Held-reference changes only input flag bit0, which original prefix overwrites before return. Complex event4 click paths remain unchanged by the single source insertion but are not dynamically executed. Unreached external targets link to zero and trap if accidentally reached. Canonical generator hook; no game performance claim.',
    sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (OUT/'original.c',OUT/'patched.c',OUT/'seh.c',OUT/'model.c',OUT/'proof.elf')})
(OUT/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'status':'PASS','counts':counts},indent=2))
