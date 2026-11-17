"""Run actual owner UIScale apply C diagnostics against explicit property-service boundaries.

This executes the observed original notification wrapper and proves apply
guards/ABI/failure preservation. Generic setter/notification bodies remain
explicit boundaries; original x86 property ABI and runtime probe are separate.
"""
import hashlib,io,json,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn import arm64_const as A
from unicorn import x86_const as X
import pefile,struct
import build_android as B
from check_virtual_query import function
import check_touch_barrier as T
OUT=B.ROOT/'build/test/native-scale-v15c'
PRELUDE=T.PRELUDE[:T.PRELUDE.index('int pthread_mutex_lock')]
PRELUDE=PRELUDE.replace('#include <math.h>','#include <math.h>\n#include <stdarg.h>')
EXTRA=r'''
static unsigned fixture_mode,current_scale=200,allocations,frees,sets,gets,releases,persists,diag_lines,notifications;
static int env_override,disk_ok=1;
static const char *last_stage;
char *getenv(const char *key){return env_override?(char *)"200":NULL;}
int s2_ui_scale(void){return current_scale;}
int void_vm_readable(uint32_t a,uint32_t n){
 if(fixture_mode==3 && a==0x11000)return 0;
 if(fixture_mode==4 && a==0x12000)return 0;
 if(fixture_mode==7 && a==0x13000)return 0;
 return (a>=0x10000 && (uint64_t)a+n<=0x30000) || (a>=0xA0E98000u && (uint64_t)a+n<=0xA0E99000ull);
}
uint32_t void_galloc(uint32_t n){if(n!=72)bad=1;++allocations;return fixture_mode==1?0:0x18000;}
void void_gfree(uint32_t p){if(p!=0x18000)bad=2;++frees;}
static int persist_ui_scale(int scale){if(scale!=(int)current_scale)bad=3;++persists;return disk_ok;}
void void_log(const char *format,...){
 if(!strncmp(format,"[native-ui-scale]",17)){va_list v;va_start(v,format);last_stage=va_arg(v,const char *);va_end(v);++diag_lines;}
}
static uint32_t boundary(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xA01CBAF3u){if(self!=0x18000 || n || args)bad=4;MEM32(self)=fixture_mode==2?0:0x11000;return self;}
 if(va==0xA01CBD79u){if(self!=0x18000 || n || args)bad=5;++releases;return 0;}
 if(va==0xA01BF515u){
  if(self!=0x11000 || n!=2 || args[0]!=0x18004 || args[1]!=0x1803C)bad=6;
  uint32_t k=args[0],a=args[1];
  if(MEM32(k)!=0x1801C || MEM32(k+16)!=9 || MEM32(k+20)!=31 || strcmp((char *)GPTR(MEM32(k)),"s2UIScale") || MEM32(a)!=0xA0D9F534u)bad=7;
  if(fixture_mode==15){++sets;return 0;}
  ++sets;MEM32(0x13000)=fixture_mode==8?0:0xA0D9F534u;MEM32(0x13004)=fixture_mode==9?200:MEM32(a+4);
  if(fixture_mode!=10)current_scale=MEM32(a+4);return 1;
 }
 if(va==0xA01BF670u){if(self!=0x11000 || n!=1 || args[0]!=0x18004)bad=8;++gets;return fixture_mode==6?0:0x13000;}
 bad=9;return 0;
}
static void setup(unsigned mode){
 fixture_mode=mode;current_scale=200;allocations=frees=sets=gets=releases=persists=bad=env_override=notifications=0;disk_ok=1;
 memset(&g_m,0,sizeof g_m);g_esp=0x28000;g_ebp=0x29000;g_fs_base=0x25000;
 MEM32(0x13000)=0xA0D9F534u;MEM32(0x13004)=200;
 MEM32(0x11000)=0x12000;MEM32(0x1200C)=mode==5?0:0xA01BF515u;MEM32(0x12020)=0xA01BF670u;
 if(mode>=11){uint32_t table=mode==12?0x12000:0xA0E98C60u;
  MEM32(0x11000)=table;MEM32(table+0xC)=0xA0C299E2u;MEM32(table+0x20)=mode==14?0:0xA01BF670u;
 }
}
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx,g_icall_count;
void (*recomp_yield_hook)(void);
void void_lift_trap(uint32_t a,const char *reason){(void)a;(void)reason;bad=10;}
static void service(void){g_eax=boundary(0xA01CBAF3u,g_ecx,0,NULL);g_esp+=4;}
static void release(void){g_eax=boundary(0xA01CBD79u,g_ecx,0,NULL);g_esp+=4;}
void sub_A01BF515(void){uint32_t args[]={ARG(0),ARG(1)};g_eax=boundary(0xA01BF515u,g_ecx,2,args);g_esp+=12;}
static void getter(void){uint32_t key=ARG(0);g_eax=boundary(0xA01BF670u,g_ecx,1,&key);g_esp+=8;}
void sub_A0C28D0F(void){if(g_ecx!=0x11000 || ARG(0)!=0x18004)bad=11;++notifications;g_eax=0xBADC0D00u;g_esp+=8;}
void sub_A0C299E2(void);
recomp_func_t recomp_lookup(uint32_t va){switch(va){
 case 0xA01CBAF3u:return service;case 0xA01CBD79u:return release;case 0xA01BF515u:return sub_A01BF515;
 case 0xA01BF670u:return getter;case 0xA0C299E2u:return sub_A0C299E2;case 0xA0C28D0Fu:return sub_A0C28D0F;
 default:bad=12;return service;
}}
recomp_func_t void_resolve(uint32_t va){return recomp_lookup(va);}
'''
CHECK=r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
unsigned scale_cases;
uint32_t selftest(void){
 setup(0);REQUIRE(s2_ui_scale_apply(300) && current_scale==300 && sets==1 && gets==1 && releases==1 && frees==1 && persists==1 && !bad && !strcmp(last_stage,"done"));++scale_cases;
 setup(0);REQUIRE(!s2_ui_scale_apply(99) && !allocations && current_scale==200 && !strcmp(last_stage,"bounds"));++scale_cases;
 setup(0);env_override=1;REQUIRE(!s2_ui_scale_apply(300) && !allocations);++scale_cases;
 const char *stages[]={"allocation","service","service","table","methods","readback","readback","valueType","value","live"};
 for(unsigned m=1;m<=10;++m){setup(m);REQUIRE(!s2_ui_scale_apply(300) && !persists && !bad && !strcmp(last_stage,stages[m-1]));
  REQUIRE(frees==(m!=1) && releases==(m!=1 && m!=2));if(m<6)REQUIRE(!sets && !gets);++scale_cases;}
 setup(0);disk_ok=0;REQUIRE(s2_ui_scale_apply(175) && current_scale==175 && persists==1 && !bad);++scale_cases;
 for(unsigned i=0;i<30;++i){setup(5);REQUIRE(!s2_ui_scale_apply(300) && !sets && !bad);}REQUIRE(diag_lines==16);++scale_cases;
 setup(11);REQUIRE(s2_ui_scale_apply(300) && current_scale==300 && sets==1 && gets==1 && notifications==1 && releases==1 && frees==1 && persists==1 && !bad);++scale_cases;
 setup(12);REQUIRE(!s2_ui_scale_apply(300) && !sets && !notifications && !bad);++scale_cases;
 setup(14);REQUIRE(!s2_ui_scale_apply(300) && !sets && !notifications && !bad);++scale_cases;
 setup(15);REQUIRE(!s2_ui_scale_apply(300) && sets==1 && !notifications && current_scale==200 && !persists && !bad);++scale_cases;
 return 0;
}
'''
def original_wrapper():
 pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);image=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
 cpu=Uc(UC_ARCH_X86,UC_MODE_32);cpu.mem_map(base,(len(image)+65535)&~65535);cpu.mem_write(base,image)
 stop=0x60000000;cpu.mem_map(0x10000,0x20000);cpu.mem_map(stop,0x1000);trace=[];success=0
 def rd(a):return int.from_bytes(cpu.mem_read(a,4),'little')
 def hook(c,va,size,user):
  if va not in (0xA01BF515,0xA0C28D0F):return
  sp=c.reg_read(X.UC_X86_REG_ESP);self=c.reg_read(X.UC_X86_REG_ECX)
  if va==0xA01BF515:
   assert self==0x11000 and rd(sp+4)==0x18004 and rd(sp+8)==0x1803C
   trace.append(('set',self,rd(sp+4),rd(sp+8)));n=2;value=0xCAFE5500|success
  else:
   assert self==0x11000 and rd(sp+4)==0x18004
   trace.append(('notify',self,rd(sp+4)));n=1;value=0xBADC0D00
  c.reg_write(X.UC_X86_REG_EAX,value);c.reg_write(X.UC_X86_REG_EIP,rd(sp));c.reg_write(X.UC_X86_REG_ESP,sp+4+4*n)
 cpu.hook_add(UC_HOOK_CODE,hook)
 for success in (0,1):
  trace.clear();cpu.mem_write(0x28000,struct.pack('<3I',stop,0x18004,0x1803C))
  for reg,value in [(X.UC_X86_REG_ESP,0x28000),(X.UC_X86_REG_ECX,0x11000),(X.UC_X86_REG_EBX,0x11223344),(X.UC_X86_REG_ESI,0x55667788)]:cpu.reg_write(reg,value)
  cpu.emu_start(0xA0C299E2,stop,count=1000)
  assert cpu.reg_read(X.UC_X86_REG_EIP)==stop and cpu.reg_read(X.UC_X86_REG_ESP)==0x2800C
  assert cpu.reg_read(X.UC_X86_REG_EAX)&255==success
  assert cpu.reg_read(X.UC_X86_REG_EBX)==0x11223344 and cpu.reg_read(X.UC_X86_REG_ESI)==0x55667788
  assert trace==[('set',0x11000,0x18004,0x1803C)]+([('notify',0x11000,0x18004)] if success else [])
 return 2

def main():
 OUT.mkdir(parents=True,exist_ok=True)
 threads=(B.ROOT/'android/host/threads.c').read_text()
 calls=''.join(function(threads,s) for s in ['uint32_t void_call_guest(', 'uint32_t void_call_guest_this('])
 lifted=function((B.ROOT/'generated/sims2-x86/sims2_A0C28000.c').read_text(),'void sub_A0C299E2(void) {')
 names=['eax','ecx','edx','ebx','esp','ebp','esi','edi']
 lifted=''.join(f'#define {n} g_{n}\n' for n in names)+lifted+''.join(f'\n#undef {n}' for n in names)+'\n'
 source=PRELUDE+EXTRA+calls+lifted+function((B.ROOT/'android/host/sims2_settings.c').read_text(),'int s2_ui_scale_apply(int scale)')+CHECK
 c=OUT/'check.c';elfpath=OUT/'check.elf';c.write_text(source)
 subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(c),'-o',str(elfpath)],check=True,cwd=B.ROOT)
 elf=ELFFile(io.BytesIO(elfpath.read_bytes()));sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);base=0x10000000;guest=0x2000000000;stack=0x50000000;stop=0x60000000
 uc.mem_map(base,0x1000000);uc.mem_map(guest+0x10000,0x20000);uc.mem_map(guest+0xA0E98000,0x1000);uc.mem_map(stack,0x10000);uc.mem_map(stop,0x1000)
 for s in elf.iter_segments():
  if s['p_type']=='PT_LOAD':uc.mem_write(base+s['p_vaddr'],s.data())
 for r in elf.get_section_by_name('.rela.dyn').iter_relocations():
  assert r['r_info_type']==1027;uc.mem_write(base+r['r_offset'],(base+r['r_addend']).to_bytes(8,'little'))
 uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);uc.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);uc.reg_write(A.UC_ARM64_REG_LR,stop)
 uc.emu_start(base+sym['selftest'],stop,count=10000000);failure=uc.reg_read(A.UC_ARM64_REG_X0);assert not failure,('C line',failure)
 count=int.from_bytes(uc.mem_read(base+sym['scale_cases'],4),'little')
 result={'pass':True,'ARM64_cases':count,'original_x86_wrapper_cases':original_wrapper(), 'production_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_settings.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'scope':'Actual scale apply C, actual lifted A0C299E2 wrapper and strict guest-call ABI; original x86 wrapper independently executes. Generic setter/notification, other service methods/allocator/persistence are explicit boundaries. No actual full runtime service or device proof.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
