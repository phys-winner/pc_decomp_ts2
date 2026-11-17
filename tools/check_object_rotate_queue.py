"""Actual owner FIFO/DispatchMessage + shared rotate/slider command receipts.

Rotation guest methods are strict arity/behavior boundaries here; actual original
handler/ref/message bodies are covered by check_object_rotate.py independently.
"""
import hashlib,io,json,struct,subprocess
from pathlib import Path
import pefile
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn import arm64_const as A
import build_android as B
import check_cas_slider as S
OUT=B.ROOT/'build/test/object-rotate-v15c/transport'
EXTRA=r'''
static unsigned rot_refs,rot_turns,rot_stops,rot_picker_calls;
static void rot_cap(void){g_eax=1;g_esp+=4;}
static void rot_add(void){++rot_refs;g_eax=rot_refs;g_esp+=4;}
static void rot_release(void){if(!rot_refs)bad=201;else --rot_refs;g_eax=rot_refs;g_esp+=4;}
static void rot_construct(void){if(g_ecx!=0x24000)bad=202;g_esp+=4;}
static void rot_setid(void){MEM32(g_ecx)=ARG(0);g_esp+=8;}
static void rot_param(void){if(ARG(0)!=0xffffffffu)bad=203;g_esp+=8;}
static void rot_destroy(void){if(g_ecx!=0x24000)bad=204;g_esp+=4;}
static void rot_handler(void){
 if(g_ecx!=0x13000 || ARG(0)!=0x24000 || ARG(1) || rot_refs!=2)bad=205;
 if(MEM32(0x24000)==0xACB3A629u){++rot_turns;MEM8(0x1313E)=1;}else if(MEM32(0x24000)==0x2FE3C0CEu){++rot_stops;MEM8(0x1313E)=0;}else bad=206;
 g_eax=1;g_esp+=12;
}
static void rot_picker(void){if(g_ecx!=0x12000 || ARG(0)!=700 || ARG(1)!=500 || rot_refs!=2)bad=207;++rot_picker_calls;g_eax=0x18000;g_esp+=12;}
'''
CHECK=r'''
unsigned rotation_queue_cases;
static void rot_setup(void){
 reset();mode=1;context=S2_TOUCH_BUY;slider_state=SLIDER_IDLE;ui_command_kind=ui_command_started=0;
 atomic_store(&cas_pan_epoch,7);rot_width=1212;rot_height=540;rot_refs=1;rot_turns=rot_stops=rot_allocs=rot_frees=rot_picker_calls=0;mono_ns=0;
 tool_handler=0x14000;MEM32(tool_handler+0x20)=0x13000;MEM32(0x13000)=0xA0DE83FC;MEM32(0x13064)=0x15000;
 MEM32(0x15000)=0xA0DEBB54;MEM32(0x15054)=1;MEM32(0x1505C)=0x19000;MEM8(0x1313E)=0;MEM32(win_manager+0x34)=0;
 MEM32(0x120BC)=0;MEM32(MEM32(0x12000)+0xBC)=0x80C;
 typed(0x18000,0x18100,0x18200,0x18300,".?AVcTSWinSceneGraph@@");s2_touch_pump();
}
static volatile uint64_t last_rotation_receipt;static uint64_t queue_rotate(void){return last_rotation_receipt=s2_touch_rotate_object(700,500,s2_touch_tool_token(),S2_TOUCH_BUY,((uint64_t)540<<32)|1212,7,-1);}
uint32_t selftest(void){
 void_regs a={0},b={0};rot_setup();void_regs_save(&a);uint64_t r=queue_rotate();void_regs_save(&b);
 REQUIRE((r&255)==S2_SLIDER_CHANGED && r>>32 && rot_turns==1 && rot_stops==1 && rot_picker_calls==1 && rot_refs==1 && rot_allocs==1 && rot_frees==1 && !bad && !trap_calls && !memcmp(&a,&b,sizeof a));++rotation_queue_cases;
 unsigned oldserial=(unsigned)(r>>32);s2_touch_barrier(oldserial);REQUIRE(rot_turns==1 && (s2_touch_slider_status(oldserial)&255)==S2_SLIDER_CHANGED);++rotation_queue_cases;
 rot_setup();mode=0;r=queue_rotate();REQUIRE((r&255)==S2_SLIDER_IN_PROGRESS && !rot_turns && !rot_allocs);mode=1;mono_ns=400000000;drive();REQUIRE(rot_turns==1 && rot_stops==1 && !bad);++rotation_queue_cases;
 for(unsigned m=4;m<=7;++m){rot_setup();mode=m;r=queue_rotate();REQUIRE((r&255)==S2_SLIDER_REJECTED && !rot_turns && !rot_allocs && !bad);++rotation_queue_cases;}
 rot_setup();mode=8;r=queue_rotate();REQUIRE((r&255)==S2_SLIDER_CANCELLED && !rot_picker_calls && !rot_turns && !bad);++rotation_queue_cases;
 rot_setup();pending_count=512;r=queue_rotate();REQUIRE((r&255)==S2_SLIDER_CANCELLED && !rot_turns && !rot_allocs && !bad);pending_count=0;++rotation_queue_cases;
 rot_setup();ui_command_kind=S2_UI_SCALE;ui_command_expiry.tv_sec=11;r=queue_rotate();REQUIRE(r==S2_SLIDER_BUSY);
 REQUIRE(!rot_turns);
 REQUIRE(!rot_allocs);++rotation_queue_cases;
 rot_setup();slider_state=SLIDER_STARTED;r=queue_rotate();REQUIRE(r==S2_SLIDER_BUSY && !rot_turns);++rotation_queue_cases;
 rot_setup();thread=2;r=queue_rotate();REQUIRE((r&255)==S2_SLIDER_IN_PROGRESS && !rot_turns && !rot_allocs);mono_ns=2000000000;REQUIRE((s2_touch_slider_status(r>>32)&255)==S2_SLIDER_CANCELLED);thread=1;drive();REQUIRE(!rot_turns && !bad);++rotation_queue_cases;
 return 0;
}
'''
def source():
 s=S.build_source().rsplit(S.CHECK,1)[0]
 s=s.replace('t->tv_sec=10;t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns:0;', 't->tv_sec=10+(c==CLOCK_MONOTONIC?mono_ns/1000000000:0);t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns%1000000000:0;')
 s=s.replace('void void_input_screen_size(int *w,int *h){*w=1212;*h=540;}','static int rot_width=1212,rot_height=540;void void_input_screen_size(int *w,int *h){*w=rot_width;*h=rot_height;}')
 s=s.replace('uint32_t void_galloc(uint32_t size){(void)size;return 0;}','static unsigned rot_allocs;uint32_t void_galloc(uint32_t size){if(size!=44)bad=208;++rot_allocs;return 0x24000;}')
 s=s.replace('void void_gfree(uint32_t pointer){(void)pointer;}','static unsigned rot_frees;void void_gfree(uint32_t pointer){if(pointer!=0x24000)bad=209;++rot_frees;}')
 s=s.replace('return a>=0x10000 && (uint64_t)a+n<=0x30000;','return (a>=0x10000 && (uint64_t)a+n<=0x30000) || (a>=0xA0000000u && (uint64_t)a+n<=0xA2000000ull);')
 s=s.replace('pthread_mutex_unlock(p);if(step_mode==9)mono_ns=80000000;drive();pthread_mutex_lock(p);return 1;', '''pthread_mutex_unlock(p);
 if(mode==4)context=S2_TOUCH_BUY|S2_TOUCH_MODAL;
 if(mode==5)atomic_fetch_add(&cas_pan_epoch,1);
 if(mode==6)rot_width=800;
 if(mode==7)MEM32(tool_handler+0x20)=0x16000;
 if(mode==8)mono_ns=2000000000;
 drive();pthread_mutex_lock(p);return 1;''')
 # Timedwait appears before native private state declarations; helper callback is forward declared.
 begin=s.index(' if(mode==4)context=S2_TOUCH_BUY|S2_TOUCH_MODAL;');end=s.index(' drive();pthread_mutex_lock(p);return 1;',begin)
 callbacks=s[begin:end];s=s[:begin]+' rotation_wait_update();\n'+s[end:]
 s=s.replace('static void drive(void);','static void drive(void);static void rotation_wait_update(void);')
 extra=EXTRA+'\nstatic void rot_cost(void){g_eax=80;g_esp+=4;}\n'+'\nstatic void rotation_wait_update(void){'+callbacks+'}\n'
 s=s.replace('recomp_func_t void_resolve(uint32_t va){',extra+'\nrecomp_func_t void_resolve(uint32_t va){')
 mappings={0xA067AD3A:'rot_cost',0xA067BE89:'rot_cap',0xA078D60A:'rot_add',0xA078D8E9:'rot_release',0xA01C033D:'rot_construct',0xA01C0915:'rot_setid',0xA01C0874:'rot_param',0xA01C0388:'rot_destroy',0xA067C356:'rot_handler',0x80C:'rot_picker'}
 s=s.replace('case 0xA0957FE3:return sub_A0957FE3;',''.join(f'case 0x{k:X}:return {v};\n' for k,v in mappings.items())+'case 0xA0957FE3:return sub_A0957FE3;',1)
 return s+CHECK

def main():
 OUT.mkdir(parents=True,exist_ok=True);c=OUT/'check.c';binary=OUT/'check.elf';c.write_text(source())
 subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib','-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(c),'-o',str(binary)],check=True,cwd=B.ROOT)
 elf=ELFFile(io.BytesIO(binary.read_bytes()));sym={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()};uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 base,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
 uc.mem_map(base,0x1000000);uc.mem_map(guest+0x10000,0x20000);uc.mem_map(guest+0xA0000000,0x2000000)
 pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);uc.mem_write(guest+pe.OPTIONAL_HEADER.ImageBase,pe.get_memory_mapped_image())
 uc.mem_map(stack,0x10000);uc.mem_map(stop,0x1000)
 for p in elf.iter_segments():
  if p['p_type']=='PT_LOAD':uc.mem_write(base+p['p_vaddr'],p.data())
 for r in elf.get_section_by_name('.rela.dyn').iter_relocations():
  assert r['r_info_type']==1027;uc.mem_write(base+r['r_offset'],(base+r['r_addend']).to_bytes(8,'little'))
 uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);uc.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);uc.reg_write(A.UC_ARM64_REG_LR,stop)
 uc.emu_start(base+sym['selftest'],stop,count=30000000);fail=uc.reg_read(A.UC_ARM64_REG_X0);bad=int.from_bytes(uc.mem_read(base+sym['bad'],4),'little');assert not fail,('C line',fail,'bad',bad,'receipt',hex(int.from_bytes(uc.mem_read(base+sym['last_rotation_receipt'],8),'little')),'count',int.from_bytes(uc.mem_read(base+sym['rotation_queue_cases'],4),'little'))
 result={'pass':True,'actual_ARM64_transport_cases':int.from_bytes(uc.mem_read(base+sym['rotation_queue_cases'],4),'little'),'production_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),'scope':'Actual owner FIFO/input/DispatchMessage, shared command state/deadline/receipt, production rotation wrapper/helper and full bank helpers. Rotation guest methods/picker are strict arity boundary mocks; original bodies tested separately. No device actions.'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
