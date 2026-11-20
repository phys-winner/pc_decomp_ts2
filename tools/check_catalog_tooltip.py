"""Actual popup gate C and original x86 selection/button and popup Show ABI boundaries."""
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

from build_android import CLANG, ROOT

OUT = ROOT / 'build/20261028-touch'


def check_c():
    source = (ROOT / 'android/host/sims2_tooltip.c').read_text()
    source = re.sub(r'^#include .*\n', '', source, flags=re.M)
    prelude = r'''
typedef unsigned int uint32_t;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static void pthread_mutex_lock(pthread_mutex_t *lock){++*lock;}
static void pthread_mutex_unlock(pthread_mutex_t *lock){--*lock;}
#define NULL ((void *)0)
#define S2_TOUCH_FOCUSED_TEXT 1024
#define S2_TOUCH_BUY 256
#define S2_TOUCH_BUILD 512
static unsigned char memory[0x40000];
#define MEM32(a) (*(uint32_t *)(memory+(a)))
#define MEM8(a) memory[(a)]
static uint32_t g_ecx,g_eax,g_esp,context,owner=0x10000,calls,nested,preps,prepare_behavior,epoch;
static int width=1170,height=540,rewards;
static uint32_t s2_touch_context(void){return context;}
static int s2_touch_rewards(void){return rewards;}
static uint32_t s2_touch_owner_manager(void){return owner;}
static unsigned s2_touch_layout_epoch(void){return epoch;}
static void void_input_screen_size(int *w,int *h){*w=width;*h=height;}
static int void_vm_readable(uint32_t a,uint32_t n){return a>=0x10000&&(unsigned long long)a+n<=sizeof memory;}
static uint32_t void_call_guest_this(uint32_t,uint32_t,int,const uint32_t *);
'''
    check = r'''
void sub_A07495DA_lifted(void){
 ++calls;g_eax=77;g_esp+=4;
 if(nested){uint32_t self=g_ecx,sp=g_esp;unsigned before=calls;nested=0;
  g_ecx=0x17008;sub_A07495DA();if(calls!=before)calls=999;g_ecx=self;g_esp=sp;
 }
}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xA07522C6){
  ++preps;if(self!=0x14000||n!=1||args[0]||!preparing_catalog_popup){calls=998;return 0;}
  if(!prepare_behavior)return 0;
  if(prepare_behavior!=2){MEM32(0x1418C)=0x15090;MEM32(0x153A4)=prepare_behavior==3?0:0x1C000;}
  if(prepare_behavior==4)context|=1;
  if(prepare_behavior==5)owner=0;
  if(prepare_behavior==6)MEM32(0x1A010)=1;
  if(prepare_behavior==7)MEM32(0x11014)=0x1C004;
  if(prepare_behavior==8)++epoch;
  if(prepare_behavior==9)++width;
  if(prepare_behavior==10)MEM32(0x10044)=0;
  if(prepare_behavior==11)MEM32(0x1212C)=0;
  if(prepare_behavior==12&&s2_catalog_info(0x11000))calls=996;
  if(context==256||context==512){uint32_t old=g_esp,before=calls;g_ecx=0x15008;g_esp-=4;sub_A07495DA();
   if(g_esp!=old||calls!=before)calls=995;}
  return 1;
 }
 if(va!=0xA07495DA||n||args){calls=998;return 0;}
 uint32_t old=g_esp;g_ecx=self;g_esp-=4;sub_A07495DA();if(g_esp!=old)calls=997;return g_eax;
}
static void setup(void){
 for(unsigned i=0;i<sizeof memory;i++)memory[i]=0;
 MEM32(0x11000)=0xA0E023D0;MEM8(0x11032)=1;MEM32(0x11008)=0x12000;
 MEM32(0x11010)=0x1E000;MEM32(0x11014)=0x1C000;
 MEM32(0x10044)=0x20000;
 MEM32(0x12000)=0xA0E54498;MEM32(0x12004)=0x10000;MEM32(0x12058)=0x13000;
 MEM32(0x120F8)=0xA0E54754;MEM32(0x1212C)=0x11000;MEM32(0x120E8)=3;
 MEM32(0x13000)=0xA0DE3704;MEM32(0x13004)=0x10000;MEM32(0x130F8)=0xA0DE39C0;
 MEM32(0x1312C)=0x14000;MEM32(0x130E8)=3;
 MEM32(0x14000)=0xA0DF7438;MEM32(0x14054)=0x13000;MEM32(0x14160)=0xA0DF751C;
 MEM32(0x14010)=0x18000;MEM32(0x14024)=22;
 MEM32(0x18000)=0x12000;MEM32(0x18004)=0x11000;
 MEM32(0x140A0)=0x19000;MEM32(0x140A4)=1;MEM32(0x19000)=0x1A000;
 MEM32(0x1403C)=0;MEM32(0x1A010)=0;
 MEM32(0x1416C)=0x1B000;MEM32(0x14170)=0x1B09C;MEM32(0x1B000)=0x1C000;
 MEM32(0x1418C)=0x15090;
 MEM32(0x15000)=MEM32(0x17000)=0xA0DFCF28;
 MEM32(0x15008)=MEM32(0x17008)=0xA0DFCFA8;
 MEM32(0x15090)=0xA0DFD000;MEM32(0x15060)=0x13000;
 MEM32(0x15068)=0x1D000;MEM32(0x153A4)=0x1C000;
 owner=0x10000;calls=nested=preps=prepare_behavior=epoch=0;width=1170;height=540;
 preparing_catalog_popup=0;g_ecx=0x15008;g_esp=0x2F000;g_eax=1;
}
__declspec(dllexport) uint32_t check_all(void){
 setup();context=S2_TOUCH_BUY;rewards=1;sub_A07495DA();if(calls!=1||g_esp!=0x2F004)return 91;
 rewards=0;
 for(unsigned mode=256;mode<=512;mode+=256){
  setup();context=mode;sub_A07495DA();if(calls||g_esp!=0x2F004||g_eax)return 1;
  setup();context=mode;if(!s2_catalog_info(0x11000)||calls!=1||g_esp!=0x2F000||explicit_catalog_popup)return 2;
  g_ecx=0x15008;sub_A07495DA();if(calls!=1)return 3;
  setup();context=mode;nested=1;if(!s2_catalog_info(0x11000)||calls!=1||explicit_catalog_popup)return 4;
 }
 for(unsigned mode=0;mode<=128;mode=mode?mode*2:1){
  setup();context=mode;sub_A07495DA();if(calls!=1)return 5;
  if(s2_catalog_info(0x11000))return 6;
 }
 for(unsigned bad=0;bad<31;bad++){
  setup();context=256;
  if(bad==0)owner=0;
  if(bad==1)MEM8(0x11032)=0;
  if(bad==2)MEM32(0x11000)=0xA0E023D4;
  if(bad==3)MEM32(0x15008)=0xA0DFCFA4;
  if(bad==4)MEM32(0x15000)=0xA0DFCF24;
  if(bad==5)MEM32(0x15068)=0;
  if(bad==6)MEM32(0x153A4)=0;
  if(bad==7)MEM32(0x11008)=0;
  if(bad==8)MEM32(0x12000)=0xA0E5449C;
  if(bad==9)MEM32(0x12004)=2;
  if(bad==10)MEM32(0x120F8)=0;
  if(bad==11)MEM32(0x1212C)=0;
  if(bad==12)MEM32(0x120E8)=0;
  if(bad==13)MEM32(0x12058)=0;
  if(bad==14)MEM32(0x13000)=0xA0DE3708;
  if(bad==15)MEM32(0x13004)=2;
  if(bad==16)MEM32(0x130F8)=0;
  if(bad==17)MEM32(0x130E8)=0;
  if(bad==18)MEM32(0x1312C)=0;
  if(bad==19)MEM32(0x14000)=0;
  if(bad==20)MEM32(0x14054)=0;
  if(bad==21)MEM32(0x14160)=0;
  if(bad==22)MEM32(0x14024)=513;
  if(bad==23)MEM32(0x18004)=0;
  if(bad==24)MEM32(0x140A4)=0;
  if(bad==25)MEM32(0x19000)=0x19000;
  if(bad==26)MEM32(0x1A010)=1;
  if(bad==27)MEM32(0x1B000)=0;
  if(bad==28)MEM32(0x14170)=0x1B001;
  if(bad==29)MEM32(0x15090)=0;
  if(bad==30)MEM32(0x15060)=0;
  if(s2_catalog_info(0x11000)||calls||explicit_catalog_popup)return 20+bad;
 }
 setup();context=256;if(s2_catalog_info(0)||s2_catalog_info(0x3FFFF))return 14;
 MEM32(0x15008)=0xA0DDD000;sub_A07495DA();if(calls!=1)return 15;
 for(unsigned mode=256;mode<=512;mode+=256)for(unsigned mismatch=0;mismatch<2;mismatch++){
  setup();context=mode;prepare_behavior=1;
  if(mismatch)MEM32(0x153A4)=0x1C004;else MEM32(0x1418C)=0;
  if(!s2_catalog_info(0x11000)||preps!=1||calls!=1||g_esp!=0x2F000||preparing_catalog_popup||explicit_catalog_popup)return 60+mismatch;
 }
 for(unsigned behavior=2;behavior<=11;behavior++){
  setup();context=512;MEM32(0x1418C)=0;prepare_behavior=behavior;
  if(s2_catalog_info(0x11000)||preps!=1||calls||preparing_catalog_popup||explicit_catalog_popup)return 70+behavior;
 }
 setup();context=512;MEM32(0x1418C)=0;prepare_behavior=12;
 if(!s2_catalog_info(0x11000)||preps!=1||calls!=1||preparing_catalog_popup)return 90;
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-catalog-tooltip-') as folder:
        path = Path(folder)
        c, obj, dll = path/'check.c', path/'check.obj', path/'check.dll'
        c.write_text(prelude + source + check)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        loaded = ctypes.CDLL(str(dll))
        try:
            result = loaded.check_all()
            assert result == 0, result
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(loaded._handle)
    return 'Actual C automatic-show gate/explicit hold/ABI,31 guards, absent/stale-product original-info preparation,10 prepare failures/reentries and bounded nested hold PASS'


def check_original():
    image = pefile.PE(str(ROOT/'original/Sims2EP9.exe'))
    mapped = image.get_memory_mapped_image()
    cpu = Uc(UC_ARCH_X86, UC_MODE_32)
    cpu.mem_map(image.OPTIONAL_HEADER.ImageBase, (len(mapped)+4095)&~4095)
    cpu.mem_write(image.OPTIONAL_HEADER.ImageBase, mapped)
    cpu.mem_map(0x10000, 0x30000)
    cpu.mem_map(0x80000, 0x2000)
    thumb, popup, window, table, stack, stop = 0x11000, 0x15008, 0x18000, 0x19000, 0x2F000, 0x81000
    button, button_table = 0x1E000, 0x1F000
    put = lambda a, v: cpu.mem_write(a, struct.pack('<I', v))
    put(thumb, 0xA0E023D0); put(thumb+16, button)
    put(button, button_table); put(button_table+0x18, 0x80040)
    cpu.mem_write(thumb+49, b'\x01')
    put(popup-8, 0xA0DFCF28); put(popup, 0xA0DFCFA8)
    put(popup+96, window); put(window, table)
    for offset, target in [(344, 0x80000), (336, 0x80010), (332, 0x80020), (112, 0x80030)]:
        put(table+offset, target)
    seen = []

    def boundary(machine, address, size, user):
        sp = machine.reg_read(X.UC_X86_REG_ESP)
        this = machine.reg_read(X.UC_X86_REG_ECX)
        pops = None
        if address == 0xA07CE5A7:
            assert this == thumb
            assert struct.unpack('<I', machine.mem_read(sp+4, 4))[0] == 2
            seen.append('selected_sprite'); pops = 4
        elif address == 0xA01C9842:
            assert this == popup+48
            seen.append('timer_reset'); pops = 0
        elif address == 0xA07818D9:
            assert this == popup-8
            seen.append('description_update'); pops = 0
        elif address == 0x80000:
            assert this == window
            seen.append('visibility_query'); pops = 0
        elif address == 0x80010:
            assert this == window and struct.unpack('<I', machine.mem_read(sp+4, 4))[0] == 1
            seen.append('show'); pops = 4
        elif address == 0x80020:
            assert this == window
            args = struct.unpack('<II', machine.mem_read(sp+4, 8))
            assert args[0] == 2048 and args[1] in (0, 1)
            seen.append('window_flag'); pops = 8
        elif address == 0x80030:
            assert this == window
            seen.append('window_refresh'); pops = 0
        elif address == 0x80040:
            assert this == button
            seen.append('button_select'); pops = 0
        if pops is not None:
            ret = struct.unpack('<I', machine.mem_read(sp, 4))[0]
            machine.reg_write(X.UC_X86_REG_EAX, 1)
            machine.reg_write(X.UC_X86_REG_ESP, sp+4+pops)
            machine.reg_write(X.UC_X86_REG_EIP, ret)

    cpu.hook_add(UC_HOOK_CODE, boundary)
    cpu.mem_write(stack, struct.pack('<II', stop, 1))
    cpu.reg_write(X.UC_X86_REG_ESP, stack); cpu.reg_write(X.UC_X86_REG_ECX, thumb)
    cpu.reg_write(X.UC_X86_REG_EBX, 0x12345678); cpu.reg_write(X.UC_X86_REG_EDI, 0x22334455)
    cpu.reg_write(X.UC_X86_REG_ESI, 0x33445566)
    cpu.emu_start(0xA07CE56D, stop, count=1000)
    assert cpu.reg_read(X.UC_X86_REG_EIP) == stop and cpu.reg_read(X.UC_X86_REG_ESP) == stack+8
    assert cpu.mem_read(thumb+50, 1) == b'\x01'
    assert seen == ['selected_sprite', 'button_select'], seen
    seen.clear()
    cpu.mem_write(stack, struct.pack('<I', stop))
    cpu.reg_write(X.UC_X86_REG_ESP, stack); cpu.reg_write(X.UC_X86_REG_ECX, popup)
    cpu.emu_start(0xA07495DA, stop, count=1000)
    assert cpu.reg_read(X.UC_X86_REG_EIP) == stop and cpu.reg_read(X.UC_X86_REG_ESP) == stack+4
    assert seen == ['timer_reset', 'description_update', 'visibility_query',
                    'show', 'window_flag', 'window_refresh', 'window_flag'], seen
    assert cpu.reg_read(X.UC_X86_REG_EBX) == 0x12345678
    assert cpu.reg_read(X.UC_X86_REG_EDI) == 0x22334455
    assert cpu.reg_read(X.UC_X86_REG_ESI) == 0x33445566
    return 'Original x86 selection -> button interface (not popup); separate exact popup Show -> description/refresh, real stack/callee-saved ABI PASS'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    result = dict(c=check_c(), original_x86=check_original(),
                  source_sha256=hashlib.sha256((ROOT/'android/host/sims2_tooltip.c').read_bytes()).hexdigest(),
                  limits='Description rendering, timer internals and window callbacks are explicit boundaries; real-device card behavior remains separate.')
    (OUT/'catalog-tooltip-check.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
