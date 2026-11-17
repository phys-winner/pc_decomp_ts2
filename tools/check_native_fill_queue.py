"""Existing native FIFO/Dispatch/shared receipt for semantic Fill; no Android actions."""
import hashlib,io,json,struct,subprocess
import check_object_rotate_queue as Q
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A
OUT=Q.B.ROOT/'build/test/room-fill-v15d/transport'
EXTRA=r'''
static unsigned fill_down,fill_up,fill_restore,fill_effect,busy_checks;
static void fill_manager_add(void){++MEM32(g_ecx+0xC);g_esp+=4;}
static void fill_manager_release(void){--MEM32(g_ecx+0xC);g_esp+=4;}
static void fill_view_add(void){++MEM32(g_ecx+0xFC);g_esp+=4;}
static void fill_view_release(void){--MEM32(g_ecx+0xFC);g_esp+=4;}
static void fill_capture_clear(void){if(ARG(0)==MEM32(g_ecx+0x34))MEM32(g_ecx+0x34)=0;g_eax=1;g_esp+=8;}
static void fill_mode_guest(void){
 if(g_ecx!=0x13000 || (ARG(0)!=0x10003 && ARG(0)!=0x10001))bad=401;
 MEM32(g_ecx+0x8C)=MEM32(g_ecx+0x90)=(ARG(0)&0xFFFF)-1;
 if(ARG(0)==0x10001)++fill_restore;
 g_eax=1;g_esp+=8;
}
static void fill_event_guest(void){
 if(g_ecx!=0x13000 || ARG(0)!=1 || ARG(2)!=0x24000 || ARG(3) || rot_refs!=2)bad=402;
 if(MEM32(ARG(2))!=700 || MEM32(ARG(2)+4)!=500)bad=403;
 if(ARG(1)==4){
  ++fill_down;MEM8(0x1301C)=1;MEM8(0x13010)=1;MEM32(win_manager+0x34)=0x18000;
  if(mode==9){if(s2_touch_fill_room(700,500,s2_touch_tool_token(),S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,7)!=S2_SLIDER_BUSY)bad=404;++busy_checks;}
  if(mode==10)context|=S2_TOUCH_MODAL;
  if(mode==11){MEM32(tool_handler+0x20)=0x16000;MEM32(0x13064)=0;}
 }else if(ARG(1)==5){
  ++fill_up;if(MEM32(0x13064))++fill_effect;
  MEM8(0x1301C)=MEM8(0x13010)=0;MEM32(win_manager+0x34)=0;
 }else bad=405;
 g_eax=1;g_esp+=20;
}
'''
CHECK=r'''
unsigned fill_queue_cases;
static void fill_setup(void){
 rot_setup();context=S2_TOUCH_BUILD;fill_down=fill_up=fill_restore=fill_effect=busy_checks=0;
 MEM32(0x13000)=0xA0DE8DA8;MEM32(0x13020)=1;MEM32(0x13004)=0x180F8;MEM32(0x13064)=0x15000;
 MEM32(0x1308C)=MEM32(0x13090)=0;MEM32(0x15000)=0xA0DEBC7C;MEM32(tool_handler+0x28)=0x10001;
 MEM32(win_manager)=0xA0E5B240;MEM32(win_manager+0xC)=4;
 MEM32(0x18000)=0xA0DD89FC;MEM32(0x18004)=win_manager;MEM32(0x180F8)=0xA0DD8CB8;MEM32(0x180FC)=4;MEM32(0x180E8)=3;MEM8(win_manager+0x1E0)=1;
 s2_touch_pump();
}
static uint64_t fill(void){return s2_touch_fill_room(700,500,s2_touch_tool_token(),S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,7);}
uint32_t selftest(void){
 void_regs a={0},b={0};fill_setup();void_regs_save(&a);uint64_t r=fill();void_regs_save(&b);
 REQUIRE((r&255)==S2_SLIDER_CHANGED && r>>32 && fill_down==1 && fill_up==1 && fill_effect==1 && fill_restore==1 && rot_refs==1 && rot_allocs==rot_frees && MEM32(win_manager+0xC)==4 && MEM32(0x180FC)==4 && !bad && !memcmp(&a,&b,sizeof a));++fill_queue_cases;
 unsigned serial=r>>32;s2_touch_barrier(serial);REQUIRE(fill_down==1 && fill_up==1 && (s2_touch_slider_status(serial)&255)==S2_SLIDER_CHANGED);++fill_queue_cases;
 fill_setup();mode=0;r=fill();REQUIRE((r&255)==S2_SLIDER_IN_PROGRESS && !fill_down);mode=1;mono_ns=400000000;drive();REQUIRE(fill_down==1 && fill_up==1 && !bad);++fill_queue_cases;
 for(unsigned m=4;m<=7;++m){fill_setup();mode=m;r=fill();REQUIRE((r&255)==S2_SLIDER_REJECTED && !fill_down && !fill_up && !bad);++fill_queue_cases;}
 fill_setup();mode=8;r=fill();REQUIRE((r&255)==S2_SLIDER_CANCELLED && !fill_down && !bad);++fill_queue_cases;
 fill_setup();pending_count=512;r=fill();REQUIRE((r&255)==S2_SLIDER_CANCELLED && !fill_down && !bad);pending_count=0;++fill_queue_cases;
 fill_setup();ui_command_kind=S2_UI_SCALE;ui_command_expiry.tv_sec=11;r=fill();REQUIRE(r==S2_SLIDER_BUSY && !fill_down);++fill_queue_cases;
 fill_setup();slider_state=SLIDER_STARTED;r=fill();REQUIRE(r==S2_SLIDER_BUSY && !fill_down);++fill_queue_cases;
 fill_setup();thread=2;r=fill();REQUIRE((r&255)==S2_SLIDER_IN_PROGRESS && !fill_down);mono_ns=2000000000;REQUIRE((s2_touch_slider_status(r>>32)&255)==S2_SLIDER_CANCELLED);thread=1;drive();REQUIRE(!fill_down && !bad);++fill_queue_cases;
 fill_setup();mode=9;r=fill();REQUIRE((r&255)==S2_SLIDER_CHANGED && fill_down==1 && fill_up==1 && busy_checks==1 && !bad);++fill_queue_cases;
 fill_setup();mode=10;r=fill();REQUIRE((r&255)==S2_SLIDER_UNKNOWN && fill_down==1 && fill_up==1 && fill_effect==1 && fill_restore==1 && !bad);++fill_queue_cases;
 fill_setup();mode=11;r=fill();REQUIRE((r&255)==S2_SLIDER_UNKNOWN && fill_down==1 && fill_up==1 && !fill_effect && !fill_restore && !bad);++fill_queue_cases;
 fill_setup();r=s2_touch_fill_room(700,500,s2_touch_tool_token(),S2_TOUCH_BUY,((uint64_t)540<<32)|1212,7);REQUIRE(r==S2_SLIDER_REJECTED && !fill_down);++fill_queue_cases;
 return 0;
}
'''
def source():
 s=Q.source().rsplit(Q.CHECK,1)[0]
 s=s.replace('if(size!=44)bad=208;', 'if(size!=8)bad=208;')
 s=s.replace('recomp_func_t void_resolve(uint32_t va){',EXTRA+'\nrecomp_func_t void_resolve(uint32_t va){')
 mappings={0xA0685954:'fill_mode_guest',0xA078DC91:'fill_event_guest',0xA0145240:'fill_manager_add',0xA01A85B0:'fill_manager_release',0xA05B84E8:'fill_view_add',0xA05BBE91:'fill_view_release',0xA097340A:'fill_capture_clear'}
 s=s.replace('case 0xA0957FE3:return sub_A0957FE3;', ''.join(f'case 0x{va:X}:return {name};' for va,name in mappings.items())+'case 0xA0957FE3:return sub_A0957FE3;',1)
 # The common setup itself is retained; remove only its original selftest.
 setup=Q.CHECK[Q.CHECK.index('static void rot_setup(void){'):Q.CHECK.index('static volatile uint64_t last_rotation_receipt;')]
 return s+setup+CHECK
def main():
 OUT.mkdir(parents=True,exist_ok=True);cfile,binary=OUT/'check.c',OUT/'check.elf';cfile.write_text(source())
 subprocess.run([str(Q.B.CLANG),*Q.B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),'-o',str(binary)],cwd=Q.B.ROOT,check=True)
 elf=ELFFile(io.BytesIO(binary.read_bytes()));symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);native,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
 pe=Q.pefile.PE(str(Q.B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);image=pe.get_memory_mapped_image();imagebase=pe.OPTIONAL_HEADER.ImageBase
 u.mem_map(native,0x1000000);u.mem_map(guest+0x10000,0x20000);u.mem_map(guest+imagebase,(len(image)+65535)&~65535);u.mem_write(guest+imagebase,image);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 for seg in elf.iter_segments():
  if seg['p_type']=='PT_LOAD':u.mem_write(native+seg['p_vaddr'],seg.data())
 reloc=elf.get_section_by_name('.rela.dyn')
 if reloc:
  for r in reloc.iter_relocations():assert r['r_info_type']==1027;u.mem_write(native+r['r_offset'],struct.pack('<Q',native+r['r_addend']))
 u.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop)
 u.emu_start(native+symbols['selftest'],stop,count=30000000)
 assert u.reg_read(A.UC_ARM64_REG_PC)==stop and u.reg_read(A.UC_ARM64_REG_X0)==0,('C line',u.reg_read(A.UC_ARM64_REG_X0),'bad',int.from_bytes(u.mem_read(native+symbols['bad'],4),'little'))
 result={'pass':True,'actual_ARM64_transport_cases':int.from_bytes(u.mem_read(native+symbols['fill_queue_cases'],4),'little'),'production_sha256':hashlib.sha256((Q.B.ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),'scope':'Actual input pending FIFO, user32 owner dispatch/depth/fence, shared QUEUED/STARTED/deadline/serial receipt and semantic Fill helper. Original event/mode/ref/pick are strict boundary mocks, independently exercised by check_native_fill.py. No real threads/device.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
