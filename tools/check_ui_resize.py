"""Execute the actual native resize callback and message queue on ARM64."""
import io
import json
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
import check_touch_barrier as T
from check_virtual_query import function

OUT = B.ROOT / 'build/20261028-ui/resize-test'
CHECK = r'''
static int target_w=800,target_h=360,input_w,input_h;
static int callbacks;
static __thread unsigned message_depth,touch_barrier_deferred,modal_message_depth;
void s2_game_resolution(int *w,int *h){*w=target_w;*h=target_h;}
uint64_t void_monotonic_ns(void){return 0;}
void void_input_set_screen_size(int w,int h){input_w=w;input_h=h;}
void s2_touch_barrier(unsigned serial){(void)serial;bad=61;}
uint32_t void_call_guest(uint32_t va,int n,const uint32_t *args,int pops){
 if(va!=0x12340000 || n!=4 || !pops || lock_depth)bad=62;
 if(args[1]==WM_SIZE && !args[2]){
  if(!s2_ui_only_resize())bad=64;
  s2_ui_device_reset(0);if(s2_ui_take_material_resize())bad=65;
  s2_ui_device_reset(1);if(!s2_ui_take_material_resize()||s2_ui_take_material_resize())bad=66;
  if(++callbacks>4){bad=63;return 0;}
  int old_w=target_w,old_h=target_h;
  target_w=800;target_h=360;s2_ui_scale_requested(300);
  target_w=old_w;target_h=old_h;s2_ui_scale_requested(300);
 }
 return 0;
}
'''
TEST = r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
uint32_t selftest(void){
 window.w=800;window.h=360;
 queue_t *q=queue_for(1);
 s2_ui_scale_requested(300);REQUIRE(!q->count && input_w==800 && input_h==360);
 target_w=1334;target_h=600;s2_ui_scale_requested(300);
 REQUIRE(q->count==1 && window.w==1334 && window.h==600);
 for(unsigned i=0;i<1000;++i){target_w=i&1?1334:800;target_h=i&1?600:360;s2_ui_scale_requested(300);}
 REQUIRE(q->count==1 && !lock_depth);
 qmsg_t m;REQUIRE(queue_take(q,0,0,0,1,&m));
 REQUIRE(m.msg==WM_SIZE && m.lparam==(600u<<16|1334u) && !q->count);
 s2_ui_scale_requested(300);REQUIRE(!q->count);
 q->head=1023;target_w=800;target_h=360;s2_ui_scale_requested(300);
 queue_push(1,2,WM_SIZE,0,0xDEADBEEF);queue_push(1,1,0x202,0,0x1234);
 target_w=1600;target_h=720;s2_ui_scale_requested(150);
 REQUIRE(q->count==3);
 REQUIRE(queue_take(q,0,0,0,1,&m) && m.hwnd==1 && m.lparam==(720u<<16|1600u));
 REQUIRE(queue_take(q,0,0,0,1,&m) && m.hwnd==2 && m.lparam==0xDEADBEEF);
 REQUIRE(queue_take(q,0,0,0,1,&m) && m.msg==0x202 && m.lparam==0x1234);
 target_w=1334;target_h=600;s2_ui_scale_requested(300);
 for(unsigned i=0;q->count && i<10;++i){
  REQUIRE(queue_take(q,0,0,0,1,&m));send_message(m.hwnd,m.msg,m.wparam,m.lparam);
 }
 REQUIRE(!q->count && callbacks==1 && !bad && !message_depth && !s2_ui_only_resize());
 window.layout_size_valid=0;ui_scale_resize_size=0;
 ui_scale_resize_active=1;send_message(1,0x202,0,0);REQUIRE(s2_ui_only_resize());ui_scale_resize_active=0;
 s2_ui_device_reset(1);REQUIRE(!s2_ui_take_material_resize());
 return 0;
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = (B.ROOT / 'android/host/user32.c').read_text()
    state = source[source.index('typedef struct { uint32_t hwnd, msg, wparam, lparam, time, x, y; } qmsg_t;'):
                   source.index('static void timers_post_due(void);')]
    prelude = T.PRELUDE.replace('int layout_size_valid;} window_t;',
                                'int layout_size_valid;int w,h;} window_t;')
    actual = ''.join(function(source, name) for name in ('static queue_t *queue_for(',
        'static void queue_push(', 'void void_post_message(', 'static int matches(', 'static int queue_take(',
        'void s2_ui_scale_requested(', 'static void message_returned(', 'static uint32_t send_message('))
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(prelude + state + CHECK + actual + 'static void drive(void) {}\n' + TEST)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, stack, stop = 0x10000000, 0x50000000, 0x60000000
    uc.mem_map(base, 0x1000000); uc.mem_map(stack, 0x10000); uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD': uc.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    for r in relocations.iter_relocations() if relocations else ():
        assert r['r_info_type'] == 1027
        uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, stack + 0x100)
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=10000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
    assert uc.reg_read(A.UC_ARM64_REG_X0) == 0, ('C line', uc.reg_read(A.UC_ARM64_REG_X0))
    result = dict(pass_=True, scope='Actual native callback/send_message/queue;1000 transient fit transitions coalesce to one latest WM_SIZE; actual reentrant size dispatch opens+closes fit and settles after one guest callback; ring wrap and unrelated window/input ordering retained. Game layout callback and device freeze acceptance separate.')
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
