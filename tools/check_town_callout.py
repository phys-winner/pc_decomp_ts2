"""Actual ARM64 native pick classification with original PE RTTI/vtables.

Reproduces the old WORLD->passive-callout UI transition, then checks the narrow
new exception and negative ownership cases. Actual x86 no-op handlers verify
their return arities and lack of object writes. No Android/device actions.
"""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess

import pefile
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

import build_android as B
import check_touch_barrier as T
from check_virtual_query import function

OUT = B.ROOT / 'build/test/town-callout'
CHECK = r'''
#define BOX 0x19000u
#define PARENT 0x1B000u
#define CHILD 0x1D000u
uint32_t cases,queries;
static void empty(uint32_t window,uint32_t head){
 MEM32(window+0xA0)=head;MEM32(window+0xA4)=0;MEM32(head)=head;MEM32(head+4)=head;
}
static void setup(void){
 reset();context=S2_TOUCH_TOWN;slider_state=SLIDER_IDLE;scene_window=scene_owner=0;
 memset(GPTR(BOX),0,0x200);memset(GPTR(PARENT),0,0x200);memset(GPTR(CHILD),0,0x200);
 MEM32(BOX)=0xA0E89208;MEM32(BOX+0x10)=0x2AAB8CC1;MEM32(BOX+0x58)=PARENT;
 MEM32(PARENT)=0xA0DE3704;MEM32(PARENT+0x10)=0xC9615A3F;MEM32(PARENT+0x58)=0x12000;
 empty(BOX,0x1F000);empty(PARENT,0x1F010);picked=BOX;
}
static int pick(void){++queries;return s2_touch_pick(700,500);}
static uint32_t transition(void){setup();picked=0x13000;unsigned first=pick();picked=BOX;return(first<<16)|(unsigned)pick();}
uint32_t selftest(void){
#if !HAS_TOWN_FIX
 REQUIRE(transition()==0x00020001u && !bad && !guest_calls);return 0;
#else
 REQUIRE(transition()==0x00020002u && !bad && !guest_calls);++cases;
 REQUIRE(MEM32(0xA0DE3704+0x26C)==0xA0938167); /* Parent is not a no-op input window. */
 setup();picked=PARENT;REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();typed(CHILD,0x22000,0x22200,0x22400,".?AVcGZWinBtn@@");MEM32(CHILD+0x58)=BOX;picked=CHILD;
 REQUIRE(pick()==1 && !bad);++cases;
 setup();typed(CHILD,0x22000,0x22200,0x22400,".?AVcGZWinSlider@@");MEM32(CHILD+0x58)=BOX;picked=CHILD;
 REQUIRE(pick()==5 && !bad);++cases;
 setup();typed(CHILD,0x22000,0x22200,0x22400,".?AVcGZWinListBox@@");MEM32(CHILD+0x58)=BOX;picked=CHILD;
 REQUIRE(pick()==9 && !bad);++cases;
 setup();MEM32(BOX+0x10)^=1;REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();MEM32(PARENT+0x10)^=1;REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();MEM32(PARENT+0x58)=0x21000;MEM32(0x21058)=0x12000;
 REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();MEM32(BOX+0x58)=0;REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();MEM32(BOX+0x58)=0x40000;REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();MEM32(BOX)=0x22000;typed(BOX,0x22000,0x22200,0x22400,".?AVcTSWinMultiIconCalloutBox@@");
 REQUIRE((pick()&3)==1 && !bad);++cases;
 setup();typed(PARENT,0x22000,0x22200,0x22400,".?AVcTSWinGenTransp@@");
 REQUIRE((pick()&3)==1 && !bad);++cases;
 for(unsigned parent=0;parent<2;++parent){
  setup();uint32_t window=parent?PARENT:BOX,head=MEM32(window+0xA0);
  MEM32(window+0xA4)=1;MEM32(head)=MEM32(head+4)=0x1F100;
  MEM32(0x1F100)=head;MEM32(0x1F104)=head;MEM32(0x1F108)=0x25000;
  typed(0x25000,0x25200,0x25400,0x25600,".?AVcTSWinProcUnknown@@");
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;MEM32(window+0xA4)=1;
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;MEM32(window+0xA0)=0;
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;MEM32(window+0xA0)=0x40000;
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;MEM32(window+0xA0)=0xFFFFFFFC;
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;head=MEM32(window+0xA0);MEM32(head+4)=0x1F100;
  REQUIRE((pick()&3)==1 && !bad);++cases;
  setup();window=parent?PARENT:BOX;head=MEM32(window+0xA0);MEM32(head)=0x1F100;
  REQUIRE((pick()&3)==1 && !bad);++cases;
 }
 unsigned contexts[]={S2_TOUCH_CAS,S2_TOUCH_LIVE,S2_TOUCH_BUY,S2_TOUCH_BUILD,S2_TOUCH_TOWN|S2_TOUCH_FOCUSED_TEXT,
  S2_TOUCH_TOWN|S2_TOUCH_MODAL,S2_TOUCH_TOWN|S2_TOUCH_OPTIONS,S2_TOUCH_TOWN|S2_TOUCH_LOADING,S2_TOUCH_TOWN|S2_TOUCH_LOT_INFO,
  S2_TOUCH_TOWN|S2_TOUCH_CAS,S2_TOUCH_TOWN|S2_TOUCH_BUY};
 for(unsigned n=0;n<sizeof contexts/sizeof contexts[0];++n){setup();context=contexts[n];REQUIRE((pick()&3)==((context&S2_TOUCH_LOADING)?0:1) && !bad);++cases;}
 for(unsigned off=0x26C;off<=0x280;off+=4){
  setup();uint32_t old=MEM32(0xA0E89208+off);MEM32(0xA0E89208+off)=0xA0938167;
  REQUIRE((pick()&3)==1 && !bad);MEM32(0xA0E89208+off)=old;++cases;
 }
 for(unsigned parent=0;parent<2;++parent){
  setup();uint32_t table=parent?0xA0DE3704:0xA0E89208,old=MEM32(table-4);
  MEM32(table-4)=0x22000;MEM32(0x2200C)=0x22200;memcpy(GPTR(0x22208),".?AVcGZWinBtn@@",15);
  REQUIRE((pick()&3)==1 && !bad);MEM32(table-4)=old;++cases;
 }
 setup();REQUIRE(!touch_town_callout(BOX,0x21000));++cases;
 setup();win_manager=0;REQUIRE(!touch_town_callout(BOX,0x12000));++cases;
 return 0;
#endif
}
'''


def source(touch_source, current):
    paths = {name: (B.ROOT / f'android/host/{name}.c').read_text(encoding='utf-8')
             for name in ('input', 'user32', 'core')}
    i, u, core = (paths[name] for name in ('input', 'user32', 'core'))
    touch = touch_source.read_text(encoding='utf-8')
    prelude = T.PRELUDE.replace('static void drive(void);', 'static uint32_t picked;\nstatic void drive(void);')
    prelude += 'uint64_t void_monotonic_ns(void){return 1000000000ull;}\n'
    prelude = prelude.replace('return a>=0x10000 && (uint64_t)a+n<=0x30000;',
        'return (a>=0x10000 && (uint64_t)a+n<=0x30000) || (a>=0xA0000000 && (uint64_t)a+n<=0xA1210000);')
    state = u[u.index('typedef struct { uint32_t hwnd, msg, wparam, lparam, time, x, y; } qmsg_t;'):
              u.index('static void timers_post_due(void);')]
    state += i[i.index('typedef struct { uint32_t msg, wparam, lparam; } pending_t;'):i.index('void void_input_screen_size(')]
    state += 'static __thread unsigned message_depth,touch_barrier_deferred;\n'
    actual = ''.join(function(core, sig) for sig in ('void void_regs_save(', 'void void_regs_load('))
    actual += '\n'.join(line for line in touch.splitlines() if not line.startswith('#include'))
    actual += ''.join(function(u, sig) for sig in ('static queue_t *queue_for(', 'static void queue_push(',
        'void void_post_message(', 'static int matches(', 'static int queue_take(', 'static void store_msg(',
        'static void message_returned(', 'static uint32_t send_message(', 'static uint32_t dispatch_message('))
    actual += ''.join(function(i, sig) for sig in ('static void pend(', 'int void_input_touch_barrier(', 'void void_input_pump('))
    utility = T.CHECK[:T.CHECK.index('uint32_t void_call_guest(')]
    callbacks = function(T.CHECK, 'uint32_t void_call_guest(')
    callbacks += function(T.CHECK, 'uint32_t void_call_guest_this(').replace('return 0x13000;', 'return picked;')
    forward = 'static void queue_push(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n'
    forward += 'static void message_returned(void);\nstatic uint32_t send_message(uint32_t,uint32_t,uint32_t,uint32_t);\n'
    return f'#define HAS_TOWN_FIX {int(current)}\n' + prelude + state + forward + actual + utility + callbacks + function(T.CHECK, 'static void reset(') + CHECK


def run_arm(path, output, current, image, image_base):
    output.mkdir(parents=True, exist_ok=True); cfile, binary = output / 'check.c', output / 'check.elf'
    cfile.write_text(source(path, current), encoding='utf-8')
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
        '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes())); symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM);base,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
    uc.mem_map(base,0x1000000);uc.mem_map(guest+0x10000,0x20000);uc.mem_map(guest+image_base,0x1210000)
    uc.mem_write(guest+image_base,image);uc.mem_map(stack,0x10000);uc.mem_map(stop,4096)
    for segment in elf.iter_segments():
        if segment['p_type']=='PT_LOAD':uc.mem_write(base+segment['p_vaddr'],segment.data())
    relocations=elf.get_section_by_name('.rela.dyn')
    if relocations:
        for r in relocations.iter_relocations():
            assert r['r_info_type']==1027;uc.mem_write(base+r['r_offset'],(base+r['r_addend']).to_bytes(8,'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);uc.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);uc.reg_write(A.UC_ARM64_REG_LR,stop)
    uc.emu_start(base+symbols['selftest'],stop,count=30000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC)==stop
    assert uc.reg_read(A.UC_ARM64_REG_X0)==0,('C assertion',uc.reg_read(A.UC_ARM64_REG_X0),str(cfile))
    return {name:int.from_bytes(uc.mem_read(base+symbols[name],4),'little') for name in ('cases','queries')}


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    pe=pefile.PE(str(B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);image=pe.get_memory_mapped_image();image_base=pe.OPTIONAL_HEADER.ImageBase
    old=run_arm(OUT/'before-v14e/android/host/sims2_touch.c',OUT/'before-repro',False,image,image_base)
    counts=run_arm(B.ROOT/'android/host/sims2_touch.c',OUT,True,image,image_base)
    original=Uc(UC_ARCH_X86,UC_MODE_32);original.mem_map(image_base,(len(image)+65535)&~65535);original.mem_write(image_base,image)
    original.mem_map(0x10000,0x20000);stop=0x60000000;original.mem_map(stop,4096)
    handlers=[]
    for offset in range(0x26C,0x284,4):
        address=struct.unpack_from('<I',image,0xA0E89208-image_base+offset)[0];argc=4 if offset==0x280 else 3
        memory=bytes((n*37)&255 for n in range(0x20000));original.mem_write(0x10000,memory)
        original.mem_write(0x28000,struct.pack('<6I',stop,11,22,33,44,55));before=bytes(original.mem_read(0x10000,0x20000))
        original.reg_write(X.UC_X86_REG_ESP,0x28000);original.reg_write(X.UC_X86_REG_ECX,0x19000);original.reg_write(X.UC_X86_REG_EAX,0x88020001)
        original.emu_start(address,stop,count=100)
        assert original.reg_read(X.UC_X86_REG_ESP)==0x28004+4*argc and original.reg_read(X.UC_X86_REG_EAX)&255==0
        assert bytes(original.mem_read(0x10000,0x20000))==before
        handlers.append({'slot':hex(offset),'address':hex(address),'args':argc,'AL':0,'guest_memory_unchanged':True})
    result={'pass_':True,**counts,'before_transition':'WORLD2 -> UI1','after_transition':'WORLD2 -> WORLD2 only for exact passive Town callout',
        'before_reproduction_queries':old['queries'],'original_x86_noop_handler_cases':handlers,
        'source_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),
        'limits':'Actual native C/real PE RTTI-vtables under ARM64; original x86 leaf handlers executed. Picker/OS waits are controlled boundaries; no device or new native activation.'}
    (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS actual ARM64 Town callout:',counts,'plus original x86 handlers',len(handlers),'and frozen v14e WORLD2->UI1 reproduction')


if __name__=='__main__':main()
