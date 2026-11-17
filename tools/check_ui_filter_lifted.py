"""Actual patched/localized Draw2D generated body -> production filter shim.

The nested flush runs the actual generated viewport math. Immutable image/target
geometry, texture QI/bind and hardware state consumers are strict boundaries.
"""
import hashlib,io,json,struct,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_INVALID
from unicorn import arm64_const as A
import build_android as B
import check_ui_filter as F
import pefile
from check_virtual_query import function

OUT=B.ROOT/'build/test/ui-quality/filter-production/lifted'
EXTRA=r'''
static int phys_w=2424,fixture_width=808,fixture_height=360;
static double ui_applied_factor=3.0;
static unsigned cases,flush_count,viewport_count,fixture_target_width=808,fixture_target_height=360;
static uint32_t viewport_seen[4];
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
void (*recomp_yield_hook)(void);
void s2_game_resolution(int *w,int *h){*w=fixture_width;*h=fixture_height;}
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x10000&&(uint64_t)a+n<=0x30000;}
void void_lift_trap(uint32_t a,const char *why){(void)a;(void)why;bad=1;}
static void getrect(void){
 uint32_t out=ARG(0);if(g_ecx==0x15000){MEM32(out)=MEM32(out+4)=0;MEM32(out+8)=MEM32(out+12)=256;}
 else if(g_ecx==0x16000||g_ecx==0x12000){MEM32(out)=MEM32(out+4)=0;MEM32(out+8)=fixture_target_width;MEM32(out+12)=fixture_target_height;}
 else bad=2;
 g_eax=out;g_esp+=8;
}
static void qi(void){
 if(g_ecx!=0x15000||ARG(0)!=0x20732180)bad=3;
 MEM32(ARG(1))=g_ecx;g_eax=1;g_esp+=12;
}
static void bind(void){if(g_ecx!=0x12000||ARG(0)!=0x15000)bad=4;g_eax=1;g_esp+=8;}
static void tableinfo(void){if(g_ecx!=0x12000)bad=5;g_eax=1;g_esp+=12;}
static void viewport(void){
 if(g_ecx!=0x12000)bad=6;
 memcpy(viewport_seen,GPTR(ARG(0)),16);++viewport_count;g_eax=1;g_esp+=8;
}
static void matrix(void){if(g_ecx!=0x12000)bad=7;g_eax=1;g_esp+=8;}
recomp_func_t recomp_lookup(uint32_t va){switch(va){
 case 0x701:return getrect;case 0x702:return qi;case 0x703:return bind;
 case 0x704:return tableinfo;case 0x705:return viewport;case 0x706:return matrix;
 default:bad=8;return getrect;
}}
recomp_func_t void_resolve(uint32_t va){return recomp_lookup(va);}
void sub_A08BA660(void);
void sub_A08BAA50(void){
 if(g_ecx!=0x12000||MEM32(g_esp)!=0xDEAD0000u)bad=9;
 ++flush_count;uint32_t sp=g_esp;PUSH32(g_esp,0xDEAD0000u);sub_A08BA660();
 if(g_esp!=sp)bad=10;g_esp+=4;
}
'''
CHECK=r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
static void setup(int fresh){
 memset(&g_m,0,sizeof g_m);memset(&g_x87,0,sizeof g_x87);memset(GPTR(0x10000),0,0x20000);
 g_stp=g_stbuf+X87_CENTRE;g_fp_bias=0;g_dispatch_gen=1;g_flag_k=FK_NONE;g_cur_func=0xA0880000;
 g_eax=0xABCD;g_ebx=0x11112222;g_esi=0x33334444;g_edi=0x55556666;g_ebp=0x77778888;
 g_mxcsr=0x1F80;g_fpu_cw=0x27F;g_eh_frames=(recomp_ehframe_t *)(uintptr_t)0x23456789;g_eh_obj=12;g_eh_info=34;
 bad=flush_count=viewport_count=0;
 MEM32(0x12000)=0x12100;MEM32(0x12114)=0x701;MEM32(0x12170)=0x705;MEM32(0x12174)=0x706;MEM32(0x12178)=0x703;MEM32(0x1217C)=0x704;
 MEM32(0x12008)=0x16000;MEM32(0x12024)=fresh?0:0x15000;MEM32(0x12028)=fresh?0:0x15000;
 MEM32(0x12014)=MEM32(0x12018)=0;MEM32(0x1201C)=(uint32_t)(808/ui_applied_factor);MEM32(0x12020)=(uint32_t)(360/ui_applied_factor);
 MEM32(0x15000)=0x15100;MEM32(0x15100)=0x702;MEM32(0x1512C)=0x701;
 MEM32(0x16000)=0x16100;MEM32(0x1612C)=0x701;
 g_ecx=0x12000;g_esp=0x28000;MEM32(g_esp)=0xDEAD0000u;
 MEM32(g_esp+4)=0x15000;MEM32(g_esp+8)=MEM32(g_esp+12)=0;
 MEM32(g_esp+16)=MEM32(g_esp+20)=0x3F800000;MEM32(g_esp+24)=0x25000;
}
uint32_t selftest(void){
 fixture_target_width=808;fixture_target_height=360;setup(0);sub_A08B8600();
 REQUIRE(!bad&&MEM8(0x12044)==1&&g_esp==0x2801C&&!ui_sampling_depth&&!flush_count);
 REQUIRE(MEM32(0x25000)==0&&MEM32(0x25004)==0&&MEM32(0x25008)==256&&MEM32(0x2500C)==256);++cases;
 setup(1);sub_A08B8600();
 REQUIRE(!bad&&MEM8(0x12044)==1&&g_esp==0x2801C&&!ui_sampling_depth&&flush_count==1&&viewport_count==1);
 REQUIRE(viewport_seen[0]==0&&viewport_seen[1]==0&&viewport_seen[2]==807&&viewport_seen[3]==360);++cases;
 for(unsigned n=0;n<2;n++){
  fixture_target_width=fixture_target_height=n?256:128;setup(0);sub_A08B8600();
  REQUIRE(!bad&&!MEM8(0x12044)&&g_esp==0x2801C&&!ui_sampling_depth);++cases;
 }
 fixture_target_width=808;fixture_target_height=360;setup(0);phys_w=808;ui_applied_factor=1.;sub_A08B8600();
 REQUIRE(!bad&&!MEM8(0x12044)&&g_esp==0x2801C&&!ui_sampling_depth);++cases;
 phys_w=2424;ui_applied_factor=3.;setup(0);sub_A08B8600();
 REQUIRE(g_ebx==0x11112222&&g_esi==0x33334444&&g_edi==0x55556666&&g_ebp==0x77778888);
 REQUIRE(g_eh_frames==(recomp_ehframe_t *)(uintptr_t)0x23456789&&g_eh_obj==12&&g_eh_info==34);++cases;
 return 0;
}
'''

def main():
 OUT.mkdir(parents=True,exist_ok=True)
 settings=(B.ROOT/'android/host/sims2_settings.c').read_text(encoding='utf-8')
 core=(B.ROOT/'android/host/core.c').read_text(encoding='utf-8')
 raw=(B.ROOT/'generated/sims2-x86/sims2_A08B8000.c').read_text(encoding='utf-8')
 body=(function(raw,'void sub_A08B8600(void) {')+function(raw,'void sub_A08BA660(void) {')).encode()
 assert body.count(b'/* 0xA08B874C: call 0xa08b8390 */')==1
 patched=B.ui_sampling_return_sites(body)
 assert patched.count(b'#define RECOMP_RETADDR 0xA08B8751u')==1
 assert B.ui_sampling_return_sites(b'unrelated file')==b'unrelated file'
 try:B.ui_sampling_return_sites(body+body)
 except SystemExit:pass
 else:raise AssertionError('duplicate target was not rejected')
 actual=B.localize_registers(patched.replace(b'void sub_A08B8600(void) {',b'void sub_A08B8600_lifted(void) {',1))
 declarations='void sub_A08B8600(void);void sub_A08B8600_lifted(void);void sub_A08B8390(void);void sub_A08BAA50(void);\n'
 gen=OUT/'generated.c';gen.write_text('#define RECOMP_GENERATED_CODE\n#define RECOMP_LOCAL_REGS\n#include "recomp_types.h"\nvoid void_lift_trap(uint32_t,const char *);\n'+declarations+actual.decode()+'\nunsigned generated_default_marker(void){return RECOMP_RETADDR;}\n',encoding='utf-8')
 hooks='static _Thread_local unsigned ui_sampling_depth;\n'+''.join(function(settings,s) for s in ['void sub_A08B8390(', 'void sub_A08B8600('])
 source=F.PRELUDE+''.join(function(core,s) for s in ['void void_x87_push('])+declarations+EXTRA+hooks+CHECK
 cfile,binary=OUT/'check.c',OUT/'check.elf';cfile.write_text(source,encoding='utf-8')
 command=[str(B.CLANG),*B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),str(gen),'-o',str(binary)]
 subprocess.run(command,cwd=B.ROOT,check=True)
 elf=ELFFile(io.BytesIO(binary.read_bytes()));sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);native,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
 u.mem_map(native,0x1000000);u.mem_map(guest+0x10000,0x20000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);image=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
 u.mem_map(guest+base,(len(image)+65535)&~65535);u.mem_write(guest+base,image)
 for seg in elf.iter_segments():
  if seg['p_type']=='PT_LOAD':u.mem_write(native+seg['p_vaddr'],seg.data())
 reloc=elf.get_section_by_name('.rela.dyn')
 if reloc:
  for r in reloc.iter_relocations():assert r['r_info_type']==1027;u.mem_write(native+r['r_offset'],struct.pack('<Q',native+r['r_addend']))
 observed=[]
 def trace(cpu,pc,size,user):
  if pc!=native+sym['sub_A08B8390']:return
  esp=int.from_bytes(cpu.mem_read(native+sym['g_m']+12,4),'little')
  observed.append(int.from_bytes(cpu.mem_read(guest+esp,4),'little'))
 u.hook_add(UC_HOOK_CODE,trace)
 def invalid(cpu,access,address,size,value,user):
  print('FAULT',hex(address),'nativePC',hex(cpu.reg_read(A.UC_ARM64_REG_PC)-native),'queries',[hex(v) for v in observed]);return False
 u.hook_add(UC_HOOK_MEM_INVALID,invalid)
 u.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop)
 u.emu_start(native+sym['selftest'],stop,count=30000000)
 assert u.reg_read(A.UC_ARM64_REG_PC)==stop and u.reg_read(A.UC_ARM64_REG_X0)==0,('C line',u.reg_read(A.UC_ARM64_REG_X0),'bad',int.from_bytes(u.mem_read(native+sym['bad'],4),'little'))
 assert observed.count(0xA08B8751)==6 and observed.count(0xDEAD0000)==1,observed
 result={'pass':True,'actual_ARM64_generated_cases':int.from_bytes(u.mem_read(native+sym['cases'],4),'little'),
  'observed_factor_query_guest_return_addresses':[f'{a:08X}' for a in observed],
  'production_settings_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_settings.c').read_bytes()).hexdigest(),
  'compiled_settings_text_sha256':hashlib.sha256(settings.encode()).hexdigest(),'builder_sha256':hashlib.sha256((B.ROOT/'tools/build_android.py').read_bytes()).hexdigest(),
  'localized_generated_body_sha256':hashlib.sha256(actual).hexdigest(),
  'scope':'Actual generated original A08B8600 and nested viewport A08BA660, exact production builder return-site patch followed by register localization, real production filter shim/TLS wrapper and core x87. Original prebound/new-image path, nested default-marker query, 128/256 offscreen targets, identity output, strict RET18 and preserved callee/EH bank pass.',
  'boundaries':'Image/target GetRect, texture QI/bind and hardware viewport/matrix consumers are strict endpoints; A08BAA50 boundary invokes the actual generated A08BA660. No actual D3D sampler/GPU, guest exception containment or device visual-quality acceptance is claimed.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
