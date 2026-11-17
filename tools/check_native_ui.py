"""Actual owner FIFO/CAS pan C plus original lifted MoveTo/SetArea/tree geometry.

UI property dispatch and text commands are explicit boundaries here; the root's
separate resolution/persistence probe and original x86 property oracle cover them.
"""
import hashlib
import io
import json
import re
import struct
import subprocess

import pefile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_INVALID
from unicorn import arm64_const as A

import build_android as B
import check_cas_slider as S
from check_virtual_query import function

OUT = B.ROOT / 'build/test/native-ui-v15'

CHECK = r'''
unsigned ui_cases;
static void rect(uint32_t w,int l,int t,int r,int b){
 MEM32(w+0xC8)=l;MEM32(w+0xCC)=t;MEM32(w+0xD0)=r;MEM32(w+0xD4)=b;
 MEM32(w+0x24)=l;MEM32(w+0x28)=t;MEM32(w+0x2C)=r;MEM32(w+0x30)=b;
}
static void children(uint32_t w,uint32_t block,uint32_t child){
 MEM32(w+0x54)=block;MEM32(block)=block+0x10;MEM32(block+4)=child?1:0;MEM32(block+8)=1;
 uint32_t head=block+0x10,node=block+0x20;
 MEM32(head)=MEM32(head+4)=child?node:head;
 if(child){MEM32(node)=MEM32(node+4)=head;MEM32(node+8)=child;}
}
static void ui_setup(void){
 reset();context=S2_TOUCH_CAS;ui_command_kind=ui_command_started=0;
 ui_scale_fixture=300;scale_calls=0;scale_reentrant=0;mono_ns=0;text_owner_calls=0;
 cas_editor_window=0;cas_pan_y=0;cas_pan_selected=0;cas_pan_seen_epoch=atomic_load(&cas_pan_epoch);
 MEM32(win_manager)=0x1D100;MEM32(win_manager+4)=0x1D200;MEM32(0x1D20C)=0x80B;
 MEM32(0x12000)=0xA0E54498u;MEM32(0x12058)=0;MEM32(0x120E8)=0xC103;
 rect(0x12000,0,0,808,360);children(0x12000,0x1C000,0x13000);
 MEM32(0x13000)=0xA0DDB6E8u;MEM32(0x13058)=0x12000;MEM32(0x130E8)=0xC103;
 MEM32(0x13004)=win_manager+4;
 rect(0x13000,0,0,800,600);children(0x13000,0x1C100,0x18000);
 MEM32(0x1332C)=1;MEM32(0x1334C)=0x16000;
 MEM32(0x16000)=0xA0E54498u;rect(0x16000,0,0,800,600);
 MEM32(0x18000)=0xA0E52F54u;MEM32(0x18058)=0x13000;MEM32(0x180E8)=0xC103;
 rect(0x18000,10,400,100,420);children(0x18000,0x1C200,0x1A000);
 MEM32(0x1A000)=0xA0E52F54u;MEM32(0x1A058)=0x18000;MEM32(0x1A0E8)=0xC103;
 rect(0x1A000,5,2,30,10);children(0x1A000,0x1C300,0);
 MEM32(win_manager+0x38)=0x12000;MEM32(win_manager+0x34)=0;
}
static void paint(void){g_ecx=0x13000;sub_A05E01E5();}
static int pan(float dy){uint32_t v;memcpy(&v,&dy,4);return s2_touch_ui_command(S2_UI_CAS_PAN,v);}
static uint32_t pick_at(int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(0xA09315A7,0x12000,2,p);}
uint32_t selftest(void){
 ui_setup();paint();REQUIRE((int)MEM32(0x130CC)==-240 && (int)MEM32(0x18028)==160 && (int)MEM32(0x1A028)==162);
 REQUIRE(pick_at(20,165)==0x1A000 && !bad && !trap_calls);++ui_cases;
 void_regs bank_before={0},bank_after={0};void_regs_save(&bank_before);
 REQUIRE(pan(.25f));drive();void_regs_save(&bank_after);REQUIRE(!memcmp(&bank_before,&bank_after,sizeof bank_before));
 REQUIRE((int)MEM32(0x130CC)==-150 && (int)MEM32(0x1A028)==252);
 REQUIRE(pick_at(20,255)==0x1A000 && !bad && !trap_calls);++ui_cases;
 void_regs b={0};g_ecx=0x13000;sub_A05E01E5();void_regs_save(&b);
 REQUIRE(!memcmp(&paint_bank,&b,sizeof b) && (int)MEM32(0x130CC)==-150 && !bad && !trap_calls);++ui_cases;
 for(unsigned i=0;i<100;++i){REQUIRE(pan(.0001f));drive();}
 REQUIRE(cas_pan_y>-147 && cas_pan_y<-145 && (int)MEM32(0x130CC)==-146);++ui_cases;
 REQUIRE(pan(1));drive();REQUIRE(MEM32(0x130CC)==0);REQUIRE(pan(-1));drive();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 MEM32(win_manager+0x34)=0x18000;REQUIRE(pan(.25f));drive();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 MEM32(win_manager+0x34)=0;key_fixture=0x8000;REQUIRE(pan(.25f));drive();REQUIRE((int)MEM32(0x130CC)==-240);key_fixture=0;++ui_cases;
 context=S2_TOUCH_CAS|S2_TOUCH_MODAL;REQUIRE(!pan(.25f));paint();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 context=S2_TOUCH_CAS;REQUIRE(!pan(__builtin_nanf("")) && !pan(__builtin_inff()) && !pan(2) && !pan(0));++ui_cases;
 REQUIRE(pan(.25f));s2_touch_reset_cas_pan();drive();paint();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 REQUIRE(pan(.25f));mono_ns=500000000;drive();REQUIRE((int)MEM32(0x130CC)==-240);mono_ns=0;++ui_cases;
 MEM32(0x13058)=0x16000;paint();REQUIRE(pan(.25f));drive();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 ui_setup();MEM32(0x1332C)=0;paint();REQUIRE((int)MEM32(0x130CC)==-240);++ui_cases;
 ui_setup();rect(0x12000,0,0,1616,720);paint();REQUIRE((int)MEM32(0x130CC)==120);REQUIRE(pan(-.5f));drive();REQUIRE((int)MEM32(0x130CC)==120);++ui_cases;
 ui_setup();REQUIRE(!s2_touch_ui_command(S2_UI_SCALE,99) && !s2_touch_ui_command(S2_UI_SCALE,301));++ui_cases;
 memset(&bank_before,0,sizeof bank_before);memset(&bank_after,0,sizeof bank_after);void_regs_save(&bank_before);
 REQUIRE(s2_touch_ui_command(S2_UI_SCALE,200));REQUIRE(s2_touch_ui_command(S2_UI_SCALE,150));drive();void_regs_save(&bank_after);
 REQUIRE(!memcmp(&bank_before,&bank_after,sizeof bank_before));
 REQUIRE(ui_scale_fixture==150 && scale_calls==1 && !bad && !trap_calls);++ui_cases;
 ui_setup();scale_reentrant=1;REQUIRE(s2_touch_ui_command(S2_UI_SCALE,200));drive();REQUIRE(scale_calls==1 && !bad);++ui_cases;
 ui_setup();pending_count=512;REQUIRE(!s2_touch_ui_command(S2_UI_SCALE,200) && !ui_command_kind);pending_count=0;++ui_cases;
 ui_setup();REQUIRE(s2_touch_ui_command(S2_UI_SCALE,200));REQUIRE(!s2_touch_pick(700,500));
 REQUIRE(s2_touch_slider_step(700,500,1)==S2_SLIDER_BUSY);drive();REQUIRE(scale_calls==1 && !bad);++ui_cases;
 ui_setup();REQUIRE(pan(.25f));mono_ns=500000000;REQUIRE(s2_touch_pick(20,165)==1);REQUIRE(!ui_command_kind);++ui_cases;
 ui_setup();paint();REQUIRE(pan(.25f));mono_ns=400000000;drive();
 REQUIRE((int)MEM32(0x130CC)==-150 && !bad);drive();REQUIRE((int)MEM32(0x130CC)==-150);++ui_cases;
 for(unsigned delay=400000000;delay<=1900000000;delay+=1500000000){
  ui_setup();REQUIRE(s2_touch_ui_command(S2_UI_SCALE,200));mono_ns=delay;drive();
  REQUIRE(ui_scale_fixture==200 && scale_calls==1 && !bad);drive();REQUIRE(scale_calls==1);++ui_cases;
 }
 ui_setup();REQUIRE(s2_touch_ui_command(S2_UI_SCALE,200));mono_ns=2000000000;drive();
 REQUIRE(ui_scale_fixture==300 && !scale_calls && !ui_command_kind);++ui_cases;
 for(unsigned kind=S2_UI_TEXT_READ;kind<=S2_UI_TEXT_WRITE;++kind){
  ui_setup();REQUIRE(s2_touch_ui_command(kind,123));mono_ns=400000000;drive();
  REQUIRE(text_owner_calls==1 && text_owner_kind==kind && text_owner_serial==123 && !bad);
  drive();REQUIRE(text_owner_calls==1);++ui_cases;
  ui_setup();REQUIRE(s2_touch_ui_command(kind,123));mono_ns=500000000;drive();
  REQUIRE(!text_owner_calls && !ui_command_kind);++ui_cases;
 }
 for(unsigned info=0;info<=1;++info){
  ui_setup();int w,h;void_input_screen_size(&w,&h);uint64_t size=(uint64_t)(unsigned)h<<32|(unsigned)w;
  REQUIRE(info?s2_touch_catalog_info(700,500,context,size,s2_touch_layout_epoch()):s2_touch_scroll(700,500,1,context,size,s2_touch_layout_epoch()));
  mono_ns=400000000;ui_command_expire();REQUIRE(ui_command_kind==S2_UI_CATALOG && pick_pending);
  mono_ns=500000000;ui_command_expire();REQUIRE(!ui_command_kind && !pick_pending);++ui_cases;
 }
 ui_setup();int scroll_w,scroll_h;void_input_screen_size(&scroll_w,&scroll_h);uint64_t scroll_size=(uint64_t)(unsigned)scroll_h<<32|(unsigned)scroll_w,scroll_owner=(uint64_t)0x12000<<32|0x13000;
 REQUIRE(!s2_touch_fling(20,165,1,context,scroll_size,s2_touch_layout_epoch(),0));
 REQUIRE(s2_touch_fling(20,165,1,context,scroll_size,s2_touch_layout_epoch(),scroll_owner));
 REQUIRE(s2_touch_fling(20,165,2,context,scroll_size,s2_touch_layout_epoch(),scroll_owner));
 REQUIRE(catalog_request.amount==3&&catalog_request.owner==scroll_owner);
 s2_touch_cancel_fling();REQUIRE(!ui_command_kind&&!pick_pending);drive();REQUIRE(!bad);++ui_cases;
 ui_setup();REQUIRE(s2_touch_scroll(20,165,1,context,scroll_size,s2_touch_layout_epoch()));
 s2_touch_cancel_fling();REQUIRE(ui_command_kind==S2_UI_CATALOG&&pick_pending);++ui_cases;
 return 0;
}
'''


def build_source(pe):
    source = S.build_source().rsplit(S.CHECK, 1)[0]
    source = source.replace('t->tv_sec=10;t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns:0;',
                            't->tv_sec=10+(c==CLOCK_MONOTONIC?mono_ns/1000000000:0);t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns%1000000000:0;')
    source = source.replace('void s2_touch_text_owner(unsigned kind,unsigned serial,uint32_t manager){(void)kind;(void)serial;(void)manager;}',
                            'static __thread unsigned message_depth;static unsigned text_owner_calls,text_owner_kind,text_owner_serial;void s2_touch_text_owner(unsigned kind,unsigned serial,uint32_t manager){++text_owner_calls;text_owner_kind=kind;text_owner_serial=serial;if(manager!=0x11000 || lock_depth || message_depth || handler_active)bad=204;}')
    source = source.replace('int16_t void_input_key_state(uint32_t key){(void)key;return 0;}',
                            'static int16_t key_fixture;int16_t void_input_key_state(uint32_t key){(void)key;return key_fixture;}')
    source = source.replace('int s2_ui_scale(void){return 200;}',
                            'static int ui_scale_fixture=300;static unsigned scale_calls,scale_reentrant;int s2_ui_scale(void){return ui_scale_fixture;}')
    source = source.replace('int s2_ui_scale_apply(int scale){(void)scale;return 0;}', 'int s2_ui_scale_apply(int scale);')
    scale_body = r'''
int s2_ui_scale_apply(int scale){
 ++scale_calls;if(message_depth || lock_depth || handler_active)bad=201;
 if(scale_reentrant){unsigned s=pick_serial;if(s2_touch_ui_command(S2_UI_SCALE,100) || s2_touch_pick(700,500) || s2_touch_slider_step(700,500,1)!=S2_SLIDER_BUSY || pick_serial!=s)bad=202;}
 ui_scale_fixture=scale;return 1;
}'''
    source = source.replace('return a>=0x10000 && (uint64_t)a+n<=0x30000;',
                            'return (a>=0x10000 && (uint64_t)a+n<=0x30000) || (a>=0xA0000000 && (uint64_t)a+n<=0xA2000000);')
    # Original tree methods are invoked through the same actual guest-call helper as production.
    image, image_base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    get = lambda a: struct.unpack_from('<I', image, a - image_base)[0]
    names = {0xA0930F79, 0xA0932914, 0xA0930463, 0xA0932A6D, 0xA0932A20, 0xA0933C35, 0xA0933FEA,
             0xA09315A7, 0xA05E01E5, 0xA0144910}
    for vt in (0xA0DDB6E8, 0xA0E54498, 0xA0E52F54):
        for offset in (0x28, 0x2C, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xE0, 0xEC, 0x134, 0x148, 0x218):
            names.add(get(vt + offset))
    bodies = []
    for address in sorted(names):
        path = B.ROOT / f'generated/sims2-x86/sims2_{address & ~0x7FFF:08X}.c'
        body = function(path.read_text(), f'void sub_{address:08X}(void) {{')
        if address == 0xA05E01E5:
            body = body.replace('void sub_A05E01E5(void)', 'void sub_A05E01E5_original(void)', 1)
        bodies.append(body)
    bodies = '\n'.join(bodies)
    dependencies = set(re.findall(r'RECOMP_CALL\(sub_([A-F0-9]+)\)', bodies)) - {f'{x:08X}' for x in names}
    declarations = '\n'.join(f'void sub_{x:08X}(void);' for x in names if x != 0xA05E01E5)
    declarations += '\nstatic void_regs paint_bank;void sub_A05E01E5_original(void);void sub_A05E01E5_lifted(void);\nstatic void ui_get_root(void){g_eax=MEM32(g_ecx+0x40);g_esp+=4;}\n'
    declarations += '\n'.join(f'void sub_{x}(void);' for x in dependencies)
    # Unreached allocator/list slow paths trap instead of compensating for unknown ABI.
    leaves = '\n'.join(f'void sub_{x}(void){{bad=203;g_esp+=4;}}' for x in dependencies)
    leaves = leaves.replace('void sub_A0933C9E(void){bad=203;g_esp+=4;}',
                            'void sub_A0933C9E(void){g_esp+=4;} /* clip cache leaf; geometry recursion remains original */')
    mappings = 'case 0x80B:return ui_get_root;\n' + '\n'.join(f'case 0x{x:08X}:return sub_{x:08X};' for x in names if x != 0xA05E01E5)
    source = source.replace('recomp_func_t void_resolve(uint32_t va){', declarations + '\nrecomp_func_t void_resolve(uint32_t va){')
    source = source.replace('recomp_func_t void_resolve(uint32_t va){', 'static volatile uint32_t ui_last_va,ui_unknown_va,ui_unknown_from;recomp_func_t void_resolve(uint32_t va){ui_last_va=va;')
    source = source.replace('default:return thunk_bad;', 'default:ui_unknown_va=va;ui_unknown_from=g_cur_func;return thunk_bad;')
    source = source.replace('    case 0xA0957FE3:return sub_A0957FE3;', mappings + '\n    case 0xA0957FE3:return sub_A0957FE3;')
    registers = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
    lifted = '\n'.join(f'#define {n} g_{n}' for n in registers) + '\n#define _xmm g_xmm\n' + bodies
    lifted += '\nvoid sub_A05E01E5_lifted(void){sub_A05E01E5_original();memset(&paint_bank,0,sizeof paint_bank);void_regs_save(&paint_bank);}'
    lifted += '\n' + '\n'.join(f'#undef {n}' for n in registers) + '\n#undef _xmm\n'
    return source + '\n' + leaves + '\n' + lifted + '\n' + scale_body + CHECK, sorted(names)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    pe = pefile.PE(str(B.ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    source, originals = build_source(pe)
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(source, encoding='utf-8')
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True, cwd=B.ROOT)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    uc.mem_map(base, 0x1000000); uc.mem_map(guest + 0x10000, 0x20000)
    uc.mem_map(guest + 0xA0000000, 0x2000000)
    uc.mem_write(guest + pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image())
    uc.mem_map(stack, 0x10000); uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD': uc.mem_write(base + segment['p_vaddr'], segment.data())
    for r in elf.get_section_by_name('.rela.dyn').iter_relocations():
        assert r['r_info_type'] == 1027
        uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, stack + 0x100)
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    def invalid(cpu, access, address, size, value, _):
        last = int.from_bytes(cpu.mem_read(base + symbols['ui_last_va'], 4), 'little')
        pc = cpu.reg_read(A.UC_ARM64_REG_PC) - base
        nearby = sorted((s['st_value'], s.name) for s in elf.get_section_by_name('.symtab').iter_symbols()
                        if s['st_info']['type'] == 'STT_FUNC' and s['st_value'] <= pc)[-1:]
        progress = int.from_bytes(cpu.mem_read(base + symbols['ui_cases'], 4), 'little')
        print('Unmapped fixture access', hex(address), size, 'guest call', hex(last), 'ARM PC', hex(pc), nearby, 'cases', progress)
        return False
    uc.hook_add(UC_HOOK_MEM_INVALID, invalid)
    uc.emu_start(base + symbols['selftest'], stop, count=100000000)
    failure = uc.reg_read(A.UC_ARM64_REG_X0)
    bad = int.from_bytes(uc.mem_read(base + symbols['bad'], 4), 'little')
    traps = int.from_bytes(uc.mem_read(base + symbols['trap_calls'], 4), 'little')
    unknown = int.from_bytes(uc.mem_read(base + symbols['ui_unknown_va'], 4), 'little')
    unknown_from = int.from_bytes(uc.mem_read(base + symbols['ui_unknown_from'], 4), 'little')
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop and not failure, ('C line', failure, 'bad', bad, 'traps', traps, 'unresolved original', hex(unknown), hex(unknown_from))
    count = int.from_bytes(uc.mem_read(base + symbols['ui_cases'], 4), 'little')
    result = dict(status='PASS', cases=count, originals=[f'{a:08X}' for a in originals],
                  production_touch_sha256=hashlib.sha256((B.ROOT / 'android/host/sims2_touch.c').read_bytes()).hexdigest(),
                  scope='Actual ARM64 owner FIFO/depth/serial and CAS paint policy with original MoveTo, SetArea, descendant geometry and GetChildAtPoint; property/text dispatch, WinMgr COM GetRoot accessor and clip-cache leaf are boundaries. No device clipping/model acceptance.')
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
