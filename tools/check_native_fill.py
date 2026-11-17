"""Actual semantic Fill C, original mode/event/ref/capture ABI; no device actions."""
import hashlib, io, json, re, struct, subprocess
from pathlib import Path
import check_object_rotate as R
from check_virtual_query import function
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

ROOT=R.ROOT
OUT=ROOT/'build/test/room-fill-v15d'
ACTUAL=['A078DC91','A078D769','A01474A0','A02279F0','A0685954','A0693287',
 'A078D60A','A078D8E9','A078DB29','A078DB5C','A0147A30','A05B8509',
 'A097340A','A0973442','A0685938','A069326B','A0142080',
 'A0145240','A01A85B0','A01BDBAD','A05B84E8','A01A9A25','A05BBE91']
EXTRA=r'''
static unsigned fills,down_calls,up_calls,previews,restores,painter_releases,fill_kind;
static uint32_t painter_slot,fill_setter;
static void fill_preview(void){
 if(g_ecx!=0x13000)bad=301;++previews;
 unsigned m=MEM32(g_ecx+(fill_kind==1?0x8C:0x58));
 if(m==2){
  if(mode==20)context|=S2_TOUCH_MODAL;
  if(mode==21)atomic_fetch_add(&cas_pan_epoch,1);
  if(mode==22){MEM32(tool_handler+0x20)=0x14000;token+=0x100000000ull;}
 }else ++restores;
 g_eax=1;g_esp+=4;
}
static void fill_project(void){
 if(g_ecx!=0x13000 || (ARG(0)!=4 && ARG(0)!=5) || ARG(1)!=0x24000)bad=302;
 if(MEM32(ARG(1))!=700 || MEM32(ARG(1)+4)!=500)bad=303;
 g_eax=1;g_esp+=12;
}
static void fill_begin(void){
 if(g_ecx!=0x13000 || ARG(0)!=0x24000 || !MEM8(0x1301C) || MEM32(0x13008)!=2)bad=304;
 if(MEM32(0x13000+(fill_kind==1?0x8C:0x58))!=2)bad=305;
 ++down_calls;
 if(mode==23)context|=S2_TOUCH_MODAL;
 if(mode==24)atomic_fetch_add(&cas_pan_epoch,1);
 if(mode==25){
  MEM32(tool_handler+0x20)=0x14000;token+=0x100000000ull;
  void_call_guest_this(fill_kind==1?0xA0685938u:0xA069326Bu,g_ecx,0,NULL);
 }
 if(mode==26)MEM32(win_manager+0x34)=0x19500;
 g_eax=1;g_esp+=8;
}
static void fill_end(void){
 if(g_ecx!=0x13000 || ARG(0)!=0x24000 || !MEM8(0x1301C))bad=306;
 ++up_calls;
 if(MEM32(0x13000+painter_slot)){
  ++fills;
  if(mode==27)MEM32(0x13000+painter_slot)=0x15800;
 }
 g_eax=1;g_esp+=8;
}
static void fill_dispose(void){if(g_ecx!=0x15000 && g_ecx!=0x15800)bad=307;g_eax=1;g_esp+=4;}
static void fill_painter_release(void){if(g_ecx!=0x15000)bad=308;++painter_releases;g_eax=1;g_esp+=4;}
static void fill_capture_event(void){
 if(g_ecx!=win_manager || ARG(1)!=0x12)bad=309;
 if(mode==28 && MEM32(win_manager+0x34))MEM32(0x13004)=0;
 g_eax=1;g_esp+=24;
}
static void fill_refresh(void){if(g_ecx!=win_manager)bad=310;g_eax=1;g_esp+=20;}
static void fill_window_add(void){++MEM32(0x160FC);g_eax=MEM32(0x160FC);g_esp+=4;}
static void fill_window_release(void){--MEM32(0x160FC);g_eax=MEM32(0x160FC);g_esp+=4;}
static void fill_picker(void){
 if(g_ecx!=0x12000 || ARG(0)!=700 || ARG(1)!=500 || MEM32(0x13008)!=2)bad=311;
 ++picker_calls;
 if(mode==4)context|=S2_TOUCH_MODAL;
 if(mode==5)token+=0x100000000ull;
 if(mode==10)MEM32(tool_handler+0x20)=0x14000;
 if(mode==11)held_key=1;
 if(mode==12)atomic_fetch_add(&cas_pan_epoch,1);
 if(mode==13)native_width=800;
 g_eax=mode==6?0:mode==7?0x16500:0x16000;g_esp+=12;
}
'''
CHECK=r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
unsigned fill_cases;
static void fill_reset(unsigned kind,unsigned m){
 memset(GPTR(0x10000),0,0x20000);memset(&g_m,0,sizeof g_m);g_stp=g_stbuf+X87_CENTRE;
 win_manager=0x11000;tool_handler=0x11500;scene_window=0x16000;thread=1;context=S2_TOUCH_BUILD;input_context=0;
 token=0x100000003ull;native_width=1212;native_height=540;atomic_store(&cas_pan_epoch,0);mode=m;
 held_key=bad=tracker_refs=allocs=frees=picker_calls=0;
 fills=down_calls=up_calls=previews=restores=painter_releases=0;fill_kind=kind;
 painter_slot=kind==1?0x64:0x50;fill_setter=kind==1?0xA0685954u:0xA0693287u;
 MEM32(win_manager)=0xA0E5B240;MEM32(win_manager+0xC)=4;MEM32(win_manager+0x44)=0x12000;MEM8(win_manager+0x1E0)=1;
 MEM32(0x12000)=0x17800;MEM32(0x178BC)=0x718;
 MEM32(tool_handler+0x20)=0x13000;MEM32(tool_handler+0x28)=0x10001;
 MEM32(0x13000)=kind==1?0xA0DE8DA8:0xA0DE9DCC;MEM32(0x13008)=1;MEM32(0x13020)=1;
 MEM32(0x13004)=0x160F8;MEM32(0x13000+painter_slot)=0x15000;
 if(kind==2)MEM8(0x1305C)=1;
 MEM32(0x15000)=MEM32(0x15800)=kind==1?0xA0DEBC7C:0xA0DEBFEC;
 MEM32(0x16000)=0xA0DD89FC;MEM32(0x16004)=win_manager;MEM32(0x160F8)=0xA0DD8CB8;
 MEM32(0x160FC)=4;MEM32(0x160E8)=3;
 MEM32(0x17400)=0x17600;MEM32(0x17604)=0x705;MEM32(0x17628)=0x706;
 g_eax=0xAA;g_ecx=0xBB;g_edx=0xCC;g_ebx=0xDD;g_esp=0x28000;g_ebp=0x29000;g_esi=0xEE;g_edi=0xFF;
 g_fs_base=0x25000;MEM32(0x25000)=0xCAFEBABE;g_cur_func=0xABCD;g_flag_k=FK_CMP;g_flag_a=123;g_flag_b=45;g_flag_cf=1;
 g_mxcsr=0x1F80;g_fpu_cw=0x27F;void_fpu_cmp=1;g_fp_bias=3;g_eh_frames=(void *)(uintptr_t)0x1111;g_eh_obj=222;g_eh_info=333;
}
static int fill_checked(uint64_t expected){
 void_regs a={0},b={0};void_regs_save(&a);
 int result=touch_fill_room_apply(expected,700,500,S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,0);
 void_regs_save(&b);if(memcmp(&a,&b,sizeof a)||MEM32(0x25000)!=0xCAFEBABE)bad=312;
 return result;
}
int selftest(void){
 for(unsigned kind=1;kind<=2;++kind){
  fill_reset(kind,0);REQUIRE(fill_checked(token)==S2_SLIDER_CHANGED && !bad && fills==1 && down_calls==1 && up_calls==1 && restores==1 && previews==2);++fill_cases;
  REQUIRE(MEM32(0x13008)==1 && !tracker_refs && allocs==frees && allocs==1 && MEM32(0x160FC)==4 && MEM32(win_manager+0xC)==4 && !MEM32(win_manager+0x34) && !MEM8(0x13010) && !MEM8(0x1301C));++fill_cases;
  REQUIRE(MEM32(0x13000+(kind==1?0x8C:0x58))==0 && (kind!=1 || MEM32(0x13090)==0));++fill_cases;
  for(unsigned old=0;old<4;++old){fill_reset(kind,0);MEM32(tool_handler+0x28)=0x10001+old;MEM32(0x13000+(kind==1?0x8C:0x58))=old;if(kind==1)MEM32(0x13090)=old;
   REQUIRE(fill_checked(token)==S2_SLIDER_CHANGED && !bad && fills==1 && MEM32(0x13000+(kind==1?0x8C:0x58))==old);++fill_cases;}
  for(unsigned m=4;m<=13;++m){if(m==8||m==9)continue;fill_reset(kind,m);REQUIRE(fill_checked(token)==S2_SLIDER_REJECTED && !bad && !down_calls && !up_calls && MEM32(0x13008)==1 && !tracker_refs);++fill_cases;}
  for(unsigned m=20;m<=22;++m){fill_reset(kind,m);REQUIRE(fill_checked(token)==S2_SLIDER_REJECTED && !bad && !down_calls && !up_calls && MEM32(0x13008)==1);++fill_cases;}
  for(unsigned m=23;m<=24;++m){fill_reset(kind,m);REQUIRE(fill_checked(token)==S2_SLIDER_UNKNOWN && !bad && fills==1 && down_calls==1 && up_calls==1 && restores==1 && !MEM8(0x1301C) && !MEM8(0x13010));++fill_cases;}
  fill_reset(kind,25);REQUIRE(fill_checked(token)==S2_SLIDER_UNKNOWN && !bad && !fills && down_calls==1 && up_calls==1 && !restores && painter_releases==1 && !MEM8(0x1301C) && !MEM8(0x13010) && MEM32(0x13008)==1);++fill_cases;
  fill_reset(kind,27);REQUIRE(fill_checked(token)==S2_SLIDER_CHANGED && !bad && fills==1 && MEM32(0x13000+painter_slot)==0x15800 && restores==1);++fill_cases;
  fill_reset(kind,28);REQUIRE(fill_checked(token)==S2_SLIDER_UNKNOWN && !bad && !fills && down_calls==1 && !up_calls && !restores && !MEM32(win_manager+0x34) && MEM32(0x13008)==1 && MEM32(win_manager+0xC)==4);++fill_cases;
  for(unsigned k=0;k<10;++k){fill_reset(kind,0);
   if(k==0)MEM32(0x13000+painter_slot)=0;
   if(k==1)MEM32(0x13000)=0xA0DE83FC;
   if(k==2)MEM32(win_manager+0x34)=0x19500;
   if(k==3)MEM8(0x13010)=1;
   if(k==4)MEM8(0x1301C)=1;
   if(k==5)MEM8(win_manager+0x1E0)=0;
   if(k==6)MEM32(0x16004)=0x19000;
   if(k==7)MEM32(tool_handler+0x28)=0;
   if(k==8)held_key=0x10;
   if(k==9)context|=S2_TOUCH_MODAL;
   REQUIRE(fill_checked(token)==S2_SLIDER_REJECTED && !bad && !down_calls && !up_calls && !allocs && !tracker_refs);++fill_cases;
  }
  fill_reset(kind,8);REQUIRE(fill_checked(token)==S2_SLIDER_REJECTED && !bad && !down_calls && !up_calls && !frees && MEM32(0x13008)==1);++fill_cases;
 }
 return 0;
}
'''

def source():
 R.ACTUAL=ACTUAL
 s=R.build_source().rsplit(R.CHECK,1)[0]
 s=s.replace('(void)a;(void)s;bad=97;', '(void)s;bad=a;')
 s=s.replace('if(size!=44)bad=90;return mode==8?0:0x24000;','if(size!=8)bad=90;return mode==8?0:0x24000;')
 # Original calls retain strict stack arity. Only paint/world/model endpoints below are mocked.
 custom={0xA0685BAF:'fill_preview',0xA06932DB:'fill_preview',0xA068546E:'fill_project',0xA0693104:'fill_project',
  0xA078EC25:'fill_begin',0xA00300A0:'fill_begin',0xA078EC49:'fill_end',0xA0693191:'fill_end',
  0xA078F377:'fill_dispose',0xA078F27F:'fill_dispose',0x716:'fill_capture_event',0x717:'fill_refresh',0x718:'fill_picker',
  R.dword(0xA0E5B240+0x28):'fill_capture_event',R.dword(0xA0E5B240+0x20):'fill_refresh',
  R.dword(0xA0DEBC7C+8):'fill_painter_release',R.dword(0xA0DEBFEC+8):'fill_painter_release'}
 # Remove fallback definitions that are deliberately explicit world boundaries.
 for va,name in custom.items():
  label=f'A{va&0xFFFFFFF:07X}'
  s=s.replace(f'void sub_{label}(void){{impossible();}}',f'void sub_{label}(void){{{name}();}}')
 declarations=''.join(f'static void {n}(void);\n' for n in set(custom.values()))
 s=s.replace('static uint32_t win_manager=',declarations+'static uint32_t win_manager=',1)
 s=s.replace('recomp_func_t recomp_lookup(uint32_t va){switch(va){',EXTRA+'\nrecomp_func_t recomp_lookup(uint32_t va){switch(va){\n'+''.join(f'case 0x{va:X}u:return {name};\n' for va,name in custom.items()))
 touch=(ROOT/'android/host/sims2_touch.c').read_text()
 helpers=''.join(function(touch,n) for n in ['static int touch_fill_type(', 'static uint32_t touch_fill_painter(', 'static int touch_fill_idle(', 'static int touch_fill_current(', 'static int touch_fill_owner(', 'static int touch_fill_room_apply('])
 return s+helpers+CHECK

def main():
 OUT.mkdir(parents=True,exist_ok=True)
 cfile,binary=OUT/'check.c',OUT/'check.elf';cfile.write_text(source())
 subprocess.run([str(R.B.CLANG),*R.B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),'-o',str(binary)],cwd=ROOT,check=True)
 elf=ELFFile(io.BytesIO(binary.read_bytes()));symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);native,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
 u.mem_map(native,0x1000000);u.mem_map(guest+0x10000,0x20000);u.mem_map(guest+R.BASE,(len(R.IMAGE)+65535)&~65535);u.mem_write(guest+R.BASE,R.IMAGE)
 u.mem_map(stack,0x10000);u.mem_map(stop,0x1000)
 for seg in elf.iter_segments():
  if seg['p_type']=='PT_LOAD':u.mem_write(native+seg['p_vaddr'],seg.data())
 reloc=elf.get_section_by_name('.rela.dyn')
 if reloc:
  for r in reloc.iter_relocations():assert r['r_info_type']==1027;u.mem_write(native+r['r_offset'],struct.pack('<Q',native+r['r_addend']))
 u.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop)
 try:u.emu_start(native+symbols['selftest'],stop,count=30000000)
 except Exception:
  print('ARM fault',hex(u.reg_read(A.UC_ARM64_REG_PC)),{n:int.from_bytes(u.mem_read(native+symbols[n],4),'little') for n in ('bad','fill_cases','mode')}, {hex(a):hex(int.from_bytes(u.mem_read(guest+a,4),'little')) for a in (0x13004,0x13008,0x160F8,0x160FC,0x1100C)});raise
 assert u.reg_read(A.UC_ARM64_REG_PC)==stop
 assert u.reg_read(A.UC_ARM64_REG_X0)==0,('C line',u.reg_read(A.UC_ARM64_REG_X0),{n:int.from_bytes(u.mem_read(native+symbols[n],4),'little') for n in ('bad','fill_cases','mode','tracker_refs','allocs','frees')}, {hex(a):int.from_bytes(u.mem_read(guest+a,4),'little') for a in (0x13008,0x160FC,0x11034,0x13010,0x1301C)})
 result={'pass':True,'actual_ARM64_cases':int.from_bytes(u.mem_read(native+symbols['fill_cases'],4),'little'),'production_sha256':hashlib.sha256((ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),'actual_original':ACTUAL,'boundaries':'Native preview/world projection/Begin/End, painter rollback/release, root picker and manager notifications are explicit boundaries. Actual original mode/event/ref/capture methods, strict guest ABI and full normal-return bank are exercised. No topology/budget/device/exception rollback claim.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
