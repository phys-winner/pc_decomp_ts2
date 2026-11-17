"""Actual ARM64 production Draw2D filter scope and guest-ABI checks.

The original sprite body is an explicit boundary; the separate generated/raw-x86
oracles run the sprite/filter/UV paths. Actual ARM64 TLS access instructions run
against two emulated TPIDR_EL0 contexts. No device action, benchmark, glyph
rasterizer or visual-quality acceptance occurs here.
"""
import hashlib,io,json,struct,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A
import build_android as B
import check_touch_barrier as T
from check_virtual_query import function

OUT=B.ROOT/'build/test/ui-quality/filter-production'
PRELUDE=T.PRELUDE[:T.PRELUDE.index('int pthread_mutex_lock')]
PRELUDE=PRELUDE.replace('static void drive(void);','static void drive(void){}')
EXTRA=r'''
static int phys_w=2424,fixture_width=808,fixture_height=360;
static double ui_applied_factor=3.0;
static unsigned fixture_readable=1,fixture_thread,fixture_mode,lifted_calls,filter_cases;
static void tls_select(void *p){__asm__ __volatile__("msr tpidr_el0,%0"::"r"(p):"memory");}
void s2_game_resolution(int *w,int *h){*w=fixture_width;*h=fixture_height;}
int void_vm_readable(uint32_t a,uint32_t n){return fixture_readable&&a>=0x10000&&(uint64_t)a+n<=0x30000;}
static int same_bank(const void_regs *a,const void_regs *b){return !memcmp(a,b,sizeof *a);}
static void seed(void){
 memset(&g_m,0,sizeof g_m);memset(&g_x87,0,sizeof g_x87);memset(g_mm,0,sizeof g_mm);
 g_stp=g_stbuf+X87_CENTRE;g_fp_bias=0;g_eax=0x10203040;g_ecx=0x11223344;g_edx=0x55667788;g_ebx=0x87654321;
 g_ebp=0x24680;g_esi=0x11112222;g_edi=0x33334444;g_fs_base=0x20000;g_cur_func=0xA08B8600;
 g_flag_k=4;g_flag_a=5;g_flag_b=6;g_flag_cf=1;void_fpu_cmp=2;g_fpu_cw=0x27F;g_mxcsr=0x1F80;
 g_xmm[0].u64[0]=0x123456789ABCDEF0ull;g_xmm[7].u64[1]=0xFEDCBA9876543210ull;g_mm[3]=0x87654321;
 g_eh_frames=(recomp_ehframe_t *)(uintptr_t)0x12345678;g_eh_obj=0xABCD;g_eh_info=0xEF01;
 for(unsigned i=0;i<8;i++){g_stp[i]=100+i;X87_SHADOW(g_stp)[i]=1000+i;}
 g_esp=0x24000;MEM32(g_esp)=0xA0123456;
 for(unsigned i=0;i<6;i++)MEM32(g_esp+4+4*i)=0xCAFE0000+i;
 MEM32(0x25000)=MEM32(0x25004)=0;MEM32(0x25008)=fixture_width;MEM32(0x2500C)=fixture_height;
}
static double query(uint32_t site,uint32_t rect){
 void_regs before={0},after={0};void_regs_save(&before);uint32_t sp=g_esp;
 g_esp=0x26000;MEM32(g_esp)=site;MEM32(g_esp+4)=rect;
 void_regs expected=before;expected.esp=0x26008;
 sub_A08B8390();double value=fp_pop();void_regs_save(&after);
 /* Pushing a float changes the unused shadow slot behind ST0, not the bank. */
 if(!same_bank(&expected,&after))bad=1;
 g_esp=sp;return value;
}
void sub_A08B8600_lifted(void){
 ++lifted_calls;uint32_t old_sp=g_esp;
 if(fixture_mode==0){if(query(0xA08B8751,0x25000)!=3.0)bad=2;}
 if(fixture_mode==1){
  if(query(0xA08B854B,0x25000)!=3.0||query(0xA08BA71C,0x25000)!=3.0||query(0xA08B8756,0x25000)!=3.0)bad=3;
  if(query(0xA08B8751,0x25000)!=3.0)bad=4;
 }
 if(fixture_mode==2){
  MEM32(0x25008)=MEM32(0x2500C)=128;if(query(0xA08B8751,0x25000)!=1.)bad=5;
  MEM32(0x25008)=MEM32(0x2500C)=256;if(query(0xA08B8751,0x25000)!=1.)bad=6;
 }
 if(fixture_mode==3){
  MEM32(0x25000)=1;if(query(0xA08B8751,0x25000)!=1.)bad=7;MEM32(0x25000)=0;
  MEM32(0x25004)=(uint32_t)-1;if(query(0xA08B8751,0x25000)!=1.)bad=8;MEM32(0x25004)=0;
  fixture_readable=0;if(query(0xA08B8751,0x25000)!=1.)bad=9;fixture_readable=1;
  if(query(0xA08B8751,0x2FFFF)!=1.||query(0xA08B8751,0xFFFFFFFF)!=1.)bad=10;
 }
 if(fixture_mode==4){
  if(query(0xA08B8751,0x25000)!=3.)bad=11;
  fixture_mode=0;sub_A08B8600();fixture_mode=4;
  if(query(0xA08B8751,0x25000)!=3.)bad=12;
  g_esp=old_sp;
 }
 if(fixture_mode==5){
  void *original_tp=__builtin_thread_pointer();tls_select((char *)original_tp+0x1000);
  if(query(0xA08B8751,0x25000)!=3.)bad=13;tls_select(original_tp);
  if(query(0xA08B8751,0x25000)!=3.)bad=14;
 }
 if(fixture_mode==6){if(query(0xA08B8751,0x25000)!=1.)bad=15;}
 if(fixture_mode==7){if(query(0xA08B8751,0x25000)!=1.25)bad=16;}
 if(fixture_mode==8){if(query(0xA08B8751,0x25000)!=1.)bad=17;}
 /* The hook must return this original body result/bank, not restore old state. */
 g_eax=0xDEADBEEF;g_ecx=0xA0E4EC84;g_edx=0x12560000;g_flag_k=7;g_flag_cf=0;
 g_xmm[2].u64[1]=0x1122334455667788ull;g_esp=old_sp+28;
}
'''
CHECK=r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
static void reset(void){
 fixture_thread=fixture_mode=bad=lifted_calls=0;ui_sampling_depth=0;fixture_readable=1;
 ui_applied_factor=3.;phys_w=2424;fixture_width=808;fixture_height=360;seed();
}
static int run(unsigned mode){
 fixture_mode=mode;void_regs before={0},after={0};void_regs_save(&before);
 sub_A08B8600();void_regs_save(&after);
 void_regs expected=before;expected.eax=0xDEADBEEF;expected.ecx=0xA0E4EC84;expected.edx=0x12560000;
 expected.flag_k=7;expected.flag_cf=0;expected.xmm[2].u64[1]=0x1122334455667788ull;expected.esp+=28;
 return same_bank(&expected,&after)&&!bad&&ui_sampling_depth==0;
}
uint32_t selftest(void){
 reset();REQUIRE(query(0xA08B8751,0x25000)==3.&&!bad);++filter_cases;
 reset();REQUIRE(run(0)&&lifted_calls==1);++filter_cases;
 reset();REQUIRE(run(1));++filter_cases;
 reset();REQUIRE(run(2));++filter_cases;
 reset();REQUIRE(run(3));++filter_cases;
 reset();REQUIRE(run(4)&&lifted_calls==2);++filter_cases;
 reset();REQUIRE(run(5));++filter_cases;
 reset();phys_w=808;ui_applied_factor=1.;REQUIRE(run(6));++filter_cases;
 reset();phys_w=400;ui_applied_factor=1.;REQUIRE(run(6));++filter_cases;
 reset();fixture_width=0;seed();REQUIRE(run(8));++filter_cases;
 reset();fixture_width=-808;seed();REQUIRE(run(8));++filter_cases;
 reset();phys_w=1010;ui_applied_factor=1.25;REQUIRE(run(7));++filter_cases;
 reset();REQUIRE(run(0));REQUIRE(query(0xA08B8751,0x25000)==3.&&!bad);++filter_cases;
 /* Mouse scale shim remains identity, with exact x87 and RET4 effect. */
 reset();MEM32(g_esp+4)=0x25000;void_regs before={0},after={0};void_regs_save(&before);
 sub_A09713BE();REQUIRE(fp_pop()==3.);void_regs_save(&after);before.esp+=8;REQUIRE(same_bank(&before,&after));++filter_cases;
 return 0;
}
'''

def main():
 OUT.mkdir(parents=True,exist_ok=True)
 settings=(B.ROOT/'android/host/sims2_settings.c').read_text(encoding='utf-8')
 core=(B.ROOT/'android/host/core.c').read_text(encoding='utf-8')
 selected=['void sub_A08B8390(', 'void sub_A08B8600(', 'void sub_A09713BE(']
 hooks='static _Thread_local unsigned ui_sampling_depth;\n'+''.join(function(settings,s) for s in selected)
 decls='void sub_A08B8390(void);void sub_A08B8600(void);void sub_A08B8600_lifted(void);\n'
 source=PRELUDE+''.join(function(core,s) for s in ['void void_regs_save(', 'void void_regs_load(', 'void void_x87_push('])+decls+EXTRA+hooks+CHECK
 cfile,binary=OUT/'check.c',OUT/'check.elf';cfile.write_text(source,encoding='utf-8')
 subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),'-o',str(binary)],cwd=B.ROOT,check=True)
 elf=ELFFile(io.BytesIO(binary.read_bytes()));sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);native,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
 u.mem_map(native,0x1000000);u.mem_map(guest+0x10000,0x20000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 for seg in elf.iter_segments():
  if seg['p_type']=='PT_LOAD':u.mem_write(native+seg['p_vaddr'],seg.data())
 reloc=elf.get_section_by_name('.rela.dyn')
 if reloc:
  for r in reloc.iter_relocations():assert r['r_info_type']==1027;u.mem_write(native+r['r_offset'],struct.pack('<Q',native+r['r_addend']))
 u.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop)
 u.emu_start(native+sym['selftest'],stop,count=10000000)
 assert u.reg_read(A.UC_ARM64_REG_PC)==stop and u.reg_read(A.UC_ARM64_REG_X0)==0,('C line',u.reg_read(A.UC_ARM64_REG_X0),'bad',int.from_bytes(u.mem_read(native+sym['bad'],4),'little'))
 assert 'sub_A08B8600' in B.NATIVE_OVERRIDES
 result={'pass':True,'actual_ARM64_cases':int.from_bytes(u.mem_read(native+sym['filter_cases'],4),'little'),
  'production_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_settings.c').read_bytes()).hexdigest(),
  'compiled_source_text_sha256':hashlib.sha256(settings.encode()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
  'hook_body_sha256':hashlib.sha256(hooks.encode()).hexdigest(),'exact_guest_return_site':'A08B8751',
  'scope':'Actual production scope/shim C and actual core x87/bank save/load compiled for ARM64. Checks outside scope, exact/wrong return sites, nested flush query, nested wrapper, two actual TLS-address contexts selected through emulated TPIDR_EL0, invalid/unreadable/full/offscreen rectangles, identity/fractional UI transforms, lifted-body return bank, normal TLS unwind and matching mouse transform.',
  'boundaries':'The original lifted A08B8600 body is an explicit boundary. Raw original x86 filter/quad/UV execution is covered by build/test/ui-quality-readonly/check_original_filter.py. The ARM64 ELF TLS access instructions run, but no host cross-thread scheduler, C/guest exception containment, actual GPU sampling, rasterizer or device visual acceptance is claimed.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
