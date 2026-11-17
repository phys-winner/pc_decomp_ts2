"""Run actual topology/helper policy with NDK CPU sets on AArch64 Unicorn."""
import argparse
import io
import json
from pathlib import Path
import re
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A
import build_android as B

OUT=B.ROOT/'build/test/render-affinity'
OUT.mkdir(parents=True,exist_ok=True)

def function(text,signature):
    begin=text.index(signature)
    return text[begin:text.index('\n}\n',begin)+3]

current=(B.ROOT/'android/host/threads.c').read_text()
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',type=Path,default=OUT/'threads.before.c')
args=parser.parse_args()
before=args.baseline.read_text() if args.baseline.is_file() else None
declarations='''static cpu_set_t big_cores,next_cores;
static int big_cores_count,next_cores_count;
static pthread_once_t big_cores_once=PTHREAD_ONCE_INIT;
'''
parts=[]
versions=([('before',before)] if before is not None else [])+[('after',current)]
for label,text in versions:
    part=declarations+function(text,'static void big_cores_init(void)')+function(text,'int void_helper_cores(void *cpu_set_out)')
    for name in ('big_cores_count','next_cores_count','big_cores_once','big_cores_init','void_helper_cores','big_cores','next_cores'):
        part=re.sub(r'\b'+name+r'\b',label+'_'+name,part)
    parts.append(part)
source=OUT/'check.c'
source.write_text('''#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
long fixture_capacity[CPU_SETSIZE],fixture_frequency[CPU_SETSIZE];
uint32_t fixture_cpus,fixture_disabled;
cpu_set_t fixture_mask;
void *memset(void *p,int c,size_t n) { unsigned char *d=p;while(n--)*d++=(unsigned char)c;return p; }
void *memcpy(void *p,const void *q,size_t n) { unsigned char *d=p;const unsigned char *s=q;while(n--)*d++=*s++;return p; }
char *getenv(const char *name) { return fixture_disabled ? "0" : NULL; }
long sysconf(int name) { return fixture_cpus; }
int pthread_once(pthread_once_t *once,void (*init)(void)) { if(!*once){*once=1;init();}return 0; }
/* Stub diagnostics only; CPU-set operations remain the actual NDK macros. */
int snprintf(char *s,size_t n,const char *format,...) { if(n)*s=0;return 0; }
static void void_log(const char *format,...) { }
static long cpu_value(int cpu,const char *property) {
    return cpu<(int)fixture_cpus ? (property[3]=='_' ? fixture_capacity[cpu] : fixture_frequency[cpu]) : 0;
}
'''+ '\n'.join(parts)+'\nint check(int after) {\n'+''.join(
    f'CPU_ZERO(&{label}_big_cores);CPU_ZERO(&{label}_next_cores);\n'
    f'{label}_big_cores_count={label}_next_cores_count={label}_big_cores_once=0;\n' for label,_ in versions)+
    ('return after ? after_void_helper_cores(&fixture_mask) : before_void_helper_cores(&fixture_mask);\n'
     if before is not None else 'return after_void_helper_cores(&fixture_mask);\n')+'}\n')
elfpath=OUT/'check.elf'
command=[str(B.CLANG),*B.COMMON,'-O2','-fno-builtin','-fno-stack-protector','-nostdlib',
         '-Wl,--gc-sections,-e,check',str(source),'-o',str(elfpath)]
r=subprocess.run(command,cwd=B.ROOT,capture_output=True,text=True)
(OUT/'warnings.txt').write_text(r.stderr)
assert r.returncode==0 and not r.stderr.strip(),r.stderr
e=ELFFile(io.BytesIO(elfpath.read_bytes()))
symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
mask_size=e.get_section_by_name('.symtab').get_symbol_by_name('fixture_mask')[0]['st_size']
base,stack,stop=0x10000000,0x50000000,0x60000000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,0x200000);u.mem_map(stack,0x10000);u.mem_map(stop,0x1000)
for s in e.iter_segments():
    if s['p_type']=='PT_LOAD':u.mem_write(base+s['p_vaddr'],s.data())
rel=e.get_section_by_name('.rela.dyn')
if rel:
    for r in rel.iter_relocations():
        assert r['r_info_type']==1027
        u.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))

def run(after,capacity,frequency=None,disabled=False):
    n=len(capacity);frequency=frequency or [0]*n
    u.mem_write(base+symbols['fixture_capacity'],struct.pack('<'+'q'*n,*capacity))
    u.mem_write(base+symbols['fixture_frequency'],struct.pack('<'+'q'*n,*frequency))
    u.mem_write(base+symbols['fixture_cpus'],struct.pack('<I',n))
    u.mem_write(base+symbols['fixture_disabled'],struct.pack('<I',disabled))
    sentinel=bytes([0xA5])*mask_size;u.mem_write(base+symbols['fixture_mask'],sentinel)
    u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop);u.reg_write(A.UC_ARM64_REG_X0,after)
    u.emu_start(base+symbols['check'],stop,count=1_000_000)
    assert u.reg_read(A.UC_ARM64_REG_PC)==stop
    result=u.reg_read(A.UC_ARM64_REG_X0);mask=bytes(u.mem_read(base+symbols['fixture_mask'],mask_size))
    if not result:assert mask==sentinel,'no-policy result changed output'
    return result,None if not result else [i for i in range(mask_size*8) if mask[i//8]&(1<<(i%8))]

cases=[
 ('mt6785 capacity',[435]*6+[1024]*2,None,False,(1,list(range(6))),(1,[6,7])),
 ('four biggest',[435]*4+[1024]*4,None,False,(1,list(range(4))),(1,list(range(4,8)))),
 ('single prime three tiers',[435]*4+[768]*3+[1024],None,False,(1,[4,5,6]),(1,[4,5,6])),
 ('single prime two tiers',[435]*7+[1024],None,False,(1,list(range(7))),(1,list(range(7)))),
 ('capable next tier',[700]*6+[1024]*2,None,False,(1,list(range(6))),(1,list(range(6)))),
 ('exact60percent next tier',[600]*6+[1000]*2,None,False,(1,list(range(6))),(1,list(range(6)))),
 ('below60percent next tier',[599]*6+[1000]*2,None,False,(1,list(range(6))),(1,[6,7])),
 ('homogeneous',[1024]*8,None,False,(0,None),(0,None)),
 ('unknown',[0]*8,None,False,(0,None),(0,None)),
 ('single CPU',[1024],None,False,(0,None),(0,None)),
 ('disabled',[435]*6+[1024]*2,None,True,(0,None),(0,None)),
 ('frequency fallback',[0]*8,[1000000]*6+[2050000]*2,False,(1,list(range(6))),(1,[6,7])),
 ('frequency next tier',[0]*8,[2000000]*6+[2050000]*2,False,(1,list(range(6))),(1,list(range(6)))),
 ('big bits above63',[435]*64+[1024]*2,None,False,(1,list(range(64))),(1,[64,65])),
]
results=[]
for name,caps,freq,disabled,old_expected,new_expected in cases:
    old=run(0,caps,freq,disabled) if before is not None else None
    new=run(1,caps,freq,disabled)
    if before is not None:assert old==old_expected,(name,'baseline',old,old_expected)
    assert new==new_expected,(name,'current',new,new_expected)
    results.append(dict(topology=name,before=old,after=new))
# The full changed production TUs must compile with their real declarations.
for name in ('threads','d3d9_thread'):
    r=subprocess.run([str(B.CLANG),*B.HOST_FLAGS,'-fsyntax-only',str(B.ROOT/'android/host'/f'{name}.c')],
                     cwd=B.ROOT,capture_output=True,text=True)
    (OUT/(name+'.warnings.txt')).write_text(r.stderr)
    assert r.returncode==0 and not r.stderr.strip(),r.stderr
(OUT/'results.json').write_text(json.dumps(dict(actual_arm64_topologies=results,
    baseline_compared=before is not None,ndk_cpu_set_bytes=mask_size,full_production_tu_syntax_warnings=0),indent=2))
scope='before/after' if before is not None else 'current expected'
print(f'PASS actual AArch64 policy with NDK CPU sets: {len(cases)} {scope} topologies; no-policy output unchanged')
print('PASS complete threads.c/d3d9_thread.c syntax: zero warnings; no adb/full build')
