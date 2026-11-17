"""Normal-build full enumeration boundary with shared modeled vector callbacks."""
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import argparse
import hashlib

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--trace',action='store_true',help='Verify generated tracing retains every original call and bypasses all batches')
parser.add_argument('--build-device',action='store_true',help='Build a self-contained physical probe; never invokes adb')
args=parser.parse_args()
if args.trace and args.build_device:parser.error('--build-device measures the normal generated build; omit --trace')
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/test/native-resource-enumeration'/('trace' if args.trace else 'normal');OUT.mkdir(parents=True,exist_ok=True)
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function
helper=ROOT/'android/host/sims2_resource.c'
assert helper.is_file(), 'canonical resource helper is not available yet'
assert hasattr(B,'resource_enumeration_source'), 'canonical clone generator is not available yet'
old_out=B.OUT
try:
 B.OUT=OUT
 clone=B.resource_enumeration_source()
finally:B.OUT=old_out
body=function((ROOT/'generated/sims2-x86/sims2_A09A8000.c').read_text(),'void sub_A09AC040(void) {')
declarations='#include "recomp_types.h"\nvoid sub_A09A5500(void),sub_A09A37D0(void),sub_A09A5E20(void),sub_A09B2FB0(void),sub_A0824624(void),sub_A01443B0(void),sub_A082A797(void);\nvoid void_lift_trap(uint32_t,const char *);\n'
(OUT/'original.c').write_bytes(B.localize_registers((declarations+body).encode()))
pred=function((ROOT/'generated/sims2-x86/sims2_A01F0000.c').read_text(),'void sub_A01F3D30(void) {')
(OUT/'predicate.c').write_bytes(B.localize_registers(('#include "recomp_types.h"\n'+pred).encode()))
guard=function((ROOT/'android/host/core.c').read_text(),'int void_native_call_allowed(')
(OUT/'guard.c').write_text('#include "host.h"\nextern int fixture_count_thunks,fixture_trace_n;\n#define count_thunks fixture_count_thunks\n#define trace_n fixture_trace_n\n'+guard)
profile=ROOT/'build/pgo/abb24fae-20261022.profdata'
commands=[]
for name,source in (('original',OUT/'original.c'),('patched',clone),('predicate',OUT/'predicate.c')):
 commands.append([str(B.CLANG),*B.GENERATED_FLAGS,*(['-DRECOMP_TRACE'] if args.trace else []),'-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY','-fprofile-instr-use='+profile.as_posix(),'-Wno-profile-instr-unprofiled','-c',str(source),'-o',str(OUT/(name+'.o'))])
for name,source in (('skip',helper),('guard',OUT/'guard.c'),('model',ROOT/'android/tools/resource_enum_fixture.c')):
 commands.append([str(B.CLANG),*B.HOST_FLAGS,'-fno-builtin','-fno-stack-protector','-mno-outline-atomics','-c',str(source),'-o',str(OUT/(name+'.o'))])
commands.append([str(B.CLANG),'--target='+B.TARGET,'-nostdlib',*[str(OUT/(name+'.o')) for name in ('original','patched','predicate','skip','guard','model')],'-Wl,--gc-sections,-e,run_original,-u,run_patched,-u,reset,-u,s2_resource_skip_enable,-u,s2_resource_skip,-u,get_resource_stats','-o',str(OUT/'proof.elf')])
for command in commands:subprocess.run(command,cwd=ROOT,check=True)
(OUT/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
elf=ELFFile(io.BytesIO((OUT/'proof.elf').read_bytes()))
syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
sizes={s.name:s['st_size'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
regions=((0x10000,0x90000),(0,4096),(0xFFFFF000,4096))
arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
for at,length in ((base,0x200000),(stack,65536),(stop,4096)):arm.mem_map(at,length)
for at,length in regions:arm.mem_map(guest+at,length)
for seg in elf.iter_segments():
 if seg['p_type']=='PT_LOAD':arm.mem_write(base+seg['p_vaddr'],seg.data())
relocations=elf.get_section_by_name('.rela.dyn')
if relocations:
 for r in relocations.iter_relocations():
  assert r['r_info_type']==1027
  arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
def put_global(name,value):arm.mem_write(base+syms[name],struct.pack('<I',value))
def global_word(name):return struct.unpack('<I',arm.mem_read(base+syms[name],4))[0]
def stats():
 call('get_resource_stats');return struct.unpack('<2Q',arm.mem_read(base+syms['resource_stats'],16))
def call(name,*args):
 for register,value in zip((A.UC_ARM64_REG_X0,A.UC_ARM64_REG_X1,A.UC_ARM64_REG_X2,A.UC_ARM64_REG_X3),args):arm.reg_write(register,value)
 arm.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
 arm.emu_start(base+syms[name],stop,count=2000000)
 assert arm.reg_read(A.UC_ARM64_REG_PC)==stop,name+' runaway'
 return arm.reg_read(A.UC_ARM64_REG_X0)
rng=random.Random(0xA09AC040)
counts={'cases':0,'callback_observations':0,'yield_observations':0,'off_cases':0,'diagnostic_cases':0,'retarget_cases':0,
        'stack_alias_cases':0,'wrap_cases':0}
fixture_records=bytearray()
for mode,total in enumerate((256,64,64,32)):
 for case in range(total):
  memory={at:bytearray(rng.randbytes(length)) for at,length in regions}
  writes=[]
  def put(at,value):
   at&=0xFFFFFFFF
   for start,data in memory.items():
    if start<=at and at+4<=start+len(data):
     struct.pack_into('<I',data,at-start,value&0xFFFFFFFF);writes.append((at,value&0xFFFFFFFF));return
   raise AssertionError(hex(at))
  self,table,rows,predicate,vtable,out,sp=0x11000,0x12000,0x16000,0x17000,0x18000,0x19000,0x80800
  bucket_count=rng.randrange(1,9);lengths=[rng.randrange(0,12) for _ in range(bucket_count)]
  groups=[rng.randrange(5) for _ in lengths];types=[rng.getrandbits(32) for _ in lengths]
  selected=rng.randrange(5)
  unknown=mode==0 and case%13==0
  pred_null=mode==0 and case%17==0
  out_null=case%7==0
  mutation=(1+case%4) if mode==1 else 0
  enabled=not(mode==0 and case%11==0)
  diagnostic=mode==0 and case%19==0
  retarget=mode==0 and case%23==0
  backedges=(0x10000-1-case%9) if mode==1 else rng.getrandbits(32)
  if mode==2:
   bucket_count=1;lengths=[1];groups=[1];types=[0x12345678];selected=9;out_null=True
   frame=sp-0x54
   if case%3==0:rows=frame+0x44
   if case%3==1:table=frame+0x30
   if case%3==2:predicate=sp+16
   counts['stack_alias_cases']+=1
  if mode==3:
   bucket_count=1;lengths=[1];groups=[1];types=[0x12345678];selected=9;out_null=True
   if case%2==0:rows=0xFFFFFFF0
   else:sp=4
   counts['wrap_cases']+=1
  total_rows=sum(lengths)
  put(self+4,rows);put(self+8,rows+16*total_rows);put(self+0x1C,table);put(self+0x28,bucket_count)
  row=0
  for i,n in enumerate(lengths):
   put(table+16*i,0xE86B1EEF if mode==0 and (case+i)%29==0 else types[i]);put(table+16*i+4,groups[i]);put(table+16*i+8,row)
   for j in range(n):
    for word in range(4):put(rows+row*16+word*4,rng.getrandbits(32))
    row+=1
  put(table+16*bucket_count+8,total_rows)
  put(predicate,vtable);put(predicate+4,1);put(predicate+8,selected);put(vtable+12,0xA01F3D31 if unknown else 0xA01F3D30)
  put(out,0x1A000);put(out+4,0x1A010);put(out+8,0x1A020)
  put(sp,stop);put(sp+4,0 if out_null else out);put(sp+8,0 if pred_null else predicate)
  state=bytearray(rng.randbytes(sizes['g_m']));gpr=[rng.getrandbits(32) for _ in range(8)];gpr[1]=self;gpr[3]=sp
  struct.pack_into('<8I',state,0,*gpr);struct.pack_into('<4I',state,32,8,0x202|(rng.getrandbits(12)&0x8D5),0,0)
  struct.pack_into('<3I',state,48,0x12345678,backedges,case+mode*1000+101)
  fixture_records+=struct.pack('<8I',32+len(state)+8*len(writes),mutation,enabled,diagnostic,retarget,len(writes),mode,case)+state
  fixture_records+=b''.join(struct.pack('<2I',*write) for write in writes)
  results=[]
  for name in ('run_original','run_patched'):
   for at,data in memory.items():arm.mem_write(guest+at,bytes(data))
   arm.mem_write(base+syms['g_m'],bytes(state))
   for field,value in (('mutation',mutation),('diag_active',diagnostic),('retarget',retarget)):put_global(field,value)
   call('reset');call('s2_resource_skip_enable',enabled)
   try:flags=call(name)&0x8D5
   except Exception as exc:raise AssertionError(f'mode={mode} case={case} {name}') from exc
   observations=global_word('event_count')
   result=(bytes(arm.mem_read(base+syms['g_m'],sizes['g_m'])),flags,
           tuple(bytes(arm.mem_read(guest+at,length)) for at,length in regions),
           bytes(arm.mem_read(base+syms['events'],sizes['events'])),observations,global_word('yield_count'),
           bytes(arm.mem_read(base+syms['g_icall_trace'],sizes['g_icall_trace'])),
           bytes(arm.mem_read(base+syms['g_icall_from'],sizes['g_icall_from'])),global_word('g_icall_trace_idx'),
           bytes(arm.mem_read(base+syms['g_enter_trace'],sizes['g_enter_trace'])),global_word('g_enter_idx'))
   results.append(result)
  for label,a,b in zip(('machine','flags','memory','events','event count','yield count','icall','icall from','icall idx','entries','entry idx'),*results):
   if a!=b:
    if isinstance(a,bytes):diff=[(i,x,y) for i,(x,y) in enumerate(zip(a,b)) if x!=y][:24]
    else:diff=label
    raise AssertionError(f'mode={mode} case={case} {label}: {diff}')
  counts['cases']+=1;counts['callback_observations']+=results[0][4];counts['yield_observations']+=results[0][5]
  counts['off_cases']+=not enabled;counts['diagnostic_cases']+=bool(diagnostic);counts['retarget_cases']+=bool(retarget)
counts['skipped_rows']=stats()[0];counts['skip_batches']=stats()[1]
if args.trace:assert counts['skipped_rows']==counts['skip_batches']==0
else:assert counts['skipped_rows']>0 and counts['skip_batches']>0
direct_cases=('underflow','overflow','unreadable_bucket','unreadable_predicate','unreadable_vtable','predicate_stack',
              'vtable_stack','bucket_stack','rows_stack','unaligned','reverse','partial','null_predicate','matching',
              'unknown','disabled','diagnostic','call_trace','retarget','unreadable_rows','below_threshold','yield_boundary')
for kind in direct_cases:
 for at,length in regions:arm.mem_write(guest+at,bytes(length))
 frame,bucket,at,end,predicate,vtable=0x80800,0x12000,0x16000,0x16040,0x17000,0x18000
 if kind=='underflow':frame=7
 if kind=='overflow':frame=0xFFFFFFA0
 if kind=='unreadable_bucket':bucket=0xD0000
 if kind=='unreadable_predicate':predicate=0xD0000
 if kind=='unreadable_vtable':vtable=0xD0000
 if kind=='predicate_stack':predicate=frame+0x44
 if kind=='vtable_stack':vtable=frame+0x40
 if kind=='bucket_stack':bucket=frame+0x44
 if kind=='rows_stack':at=frame+0x40;end=at+64
 if kind=='below_threshold':end=at+48
 if kind=='unaligned':at+=1;end+=1
 if kind=='reverse':end=at-16
 if kind=='partial':end-=1
 if kind=='null_predicate':predicate=0
 if kind=='unreadable_rows':at=0x9FFF0;end=0xA0030
 def direct_put(address,value):
  if any(start<=address and address+4<=start+length for start,length in regions):arm.mem_write(guest+address,struct.pack('<I',value))
 direct_put(frame+0x5C,predicate)
 direct_put(bucket,0x12345678);direct_put(bucket+4,9 if kind=='matching' else 1)
 if predicate:direct_put(predicate,vtable);direct_put(predicate+8,9)
 direct_put(vtable+12,0xA01F3D31 if kind=='unknown' else 0xA01F3D30)
 state=bytearray(rng.randbytes(sizes['g_m']));struct.pack_into('<I',state,52,0xFFFF if kind=='yield_boundary' else 20)
 arm.mem_write(base+syms['g_m'],bytes(state))
 for field,value in (('mutation',4 if kind=='yield_boundary' else 0),('diag_active',2 if kind=='call_trace' else kind=='diagnostic'),('retarget',kind=='retarget')):put_global(field,value)
 call('reset');call('s2_resource_skip_enable',kind!='disabled')
 before=tuple(bytes(arm.mem_read(guest+start,length)) for start,length in regions)
 stats_before=(stats()[0],stats()[1])
 assert call('s2_resource_skip',frame,bucket,at,end)==0,kind
 assert bytes(arm.mem_read(base+syms['g_m'],sizes['g_m']))==bytes(state),kind+' machine'
 assert tuple(bytes(arm.mem_read(guest+start,length)) for start,length in regions)==before,kind+' memory'
 assert (stats()[0],stats()[1])==stats_before,kind+' stats'
counts['direct_invalid_guards']=len(direct_cases)
fixtures=struct.pack('<4I',0x31525053,counts['cases'],sizes['g_m'],0)+fixture_records
(OUT/'fixtures.bin').write_bytes(fixtures)
receipt={'status':'PASS','counts':counts,
 'scope':'Complete actual original/clone and actual predicate; shared modeled allocation/grow/permutation/output-vector/free callbacks. Full machine/XMM/flags/guest memory, callback/yield snapshots and indirect diagnostic records.',
 'limits':'Actual canonical helper, mode wrapper, core guard body and clone generator; shared vector/dispatch models, no game performance claim.',
 'generated_trace':args.trace,'sha256':{str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
  (helper,ROOT/'tools/build_android.py',ROOT/'android/host/core.c',ROOT/'android/tools/resource_enum_fixture.c',clone,OUT/'proof.elf')}}
(OUT/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if args.build_device:
 (OUT/'fixtures.S').write_text('.section .rodata\n.balign 16\n.global fixtures_begin\n.global fixtures_end\nfixtures_begin:\n.incbin "'+(OUT/'fixtures.bin').as_posix()+'"\nfixtures_end:\n')
 command=[str(B.CLANG),'--target='+B.TARGET,'-O2','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-fno-math-errno',
  '-Iandroid/runtime','-Iandroid/host','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function',
  '-DS2_RESOURCE_DEVICE_PROBE',str(ROOT/'android/tools/resource_enum_fixture.c'),str(OUT/'fixtures.S'),
  *[str(OUT/(name+'.o')) for name in ('original','patched','predicate','skip','guard')],'-Wl,--gc-sections','-o',str(OUT/'device-probe')]
 subprocess.run(command,cwd=ROOT,check=True)
 built={'status':'BUILT_ONLY','api':B.API,'command':command,
  'sha256':{path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in (OUT/'device-probe',OUT/'skip.o',OUT/'patched.o',OUT/'guard.o',OUT/'fixtures.bin')},
  'limits':'Never invokes adb; full original extraction/shared vector+dispatch models are an isolated kernel, not byte-identical full-TU production baseline or game performance.'}
 (OUT/'device-build.json').write_text(json.dumps(built,indent=2)+'\n');print(json.dumps(built,indent=2))
