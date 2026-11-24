"""Actual C stable canvas and modal ownership, including captured parentless Design geometry."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import struct

import pefile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/sims2_settings.c').read_text()
    assert '[modal-fit]' not in source and 'ui_modal_trace' not in source and 'ui_modal_probe' not in source
    pe = pefile.PE(str(ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    image, image_base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    contracts = {0xA0E5B298: 0xA09737BF, 0xA0E544C4: 0xA01DFF70,
                 0xA0E544C8: 0xA0932FF7, 0xA0E544E4: 0xA093061A,
                 0xA0E54578: 0xA0931190, 0xA0E54634: 0xA0930463}
    for address, target in contracts.items():
        assert struct.unpack_from('<I', image, address - image_base)[0] == target, hex(address)
    prelude = r'''
typedef unsigned int uint32_t;
typedef unsigned long long size_t;
#define atomic_store(p,v) __c11_atomic_store(p,v,5)
#define S2_TOUCH_MODAL 1
#define S2_TOUCH_OPTIONS 2
#define S2_TOUCH_CAS 4
#define S2_TOUCH_CHOOSER 8
#define S2_TOUCH_LOADING 16
#define S2_TOUCH_LOT_INFO 32
#define S2_TOUCH_LIVE 128
#define S2_TOUCH_TOWN 64
#define S2_TOUCH_BUY 256
#define S2_TOUCH_BUILD 512
static unsigned char memory[0x10000];
static unsigned char gen_table[0x1A0],manager_table[0x64];
#define GPTR(a) ((uint32_t)(a)>=0xA0E5B240u ? manager_table+((uint32_t)(a)-0xA0E5B240u) : (uint32_t)(a)>=0xA0E54498u ? gen_table+((uint32_t)(a)-0xA0E54498u) : memory+(uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
static _Atomic int ui_scale;
static int phys_w,phys_h,input_w,input_h,ui_startup_fit,render_scale=100;
static int ui_modal_floor_right,ui_modal_floor_bottom;
static unsigned ui_fit_depth,ui_fit_height_depth,touch_modal_depth,touch_mode_flags,touch_input_flags;
static int ui_fit_linger;
static unsigned long long ui_fit_linger_ns,ui_fit_linger_until,fake_ns;
static unsigned long long void_monotonic_ns(void){return fake_ns;}
static void void_log(const char *fmt,...){(void)fmt;}
static uint32_t ui_fit_option,ui_fit_cas,ui_fit_chooser,ui_fit_loading,ui_fit_town_loading,ui_fit_lot_loading,ui_fit_lot_info;
static _Atomic unsigned touch_context;
static uint32_t g_ecx,g_esp,g_eax,owner;
static unsigned requests,calls,nest,want_depth,failed,registry_bits,registry_calls,change_scale;
static unsigned nested_scale,expect_width,expect_height,nested_width,nested_height;
static uint32_t *during_owner;
typedef struct {uint32_t eax,ecx,esp;} void_regs;
static void void_regs_save(void_regs *r){*r=(void_regs){g_eax,g_ecx,g_esp};}
static void void_regs_load(const void_regs *r){g_eax=r->eax;g_ecx=r->ecx;g_esp=r->esp;}
static void clear(void){for(unsigned i=0;i<sizeof memory;++i)memory[i]=0;for(unsigned i=0;i<sizeof gen_table;++i)gen_table[i]=0;for(unsigned i=0;i<sizeof manager_table;++i)manager_table[i]=0;}
static int void_vm_readable(uint32_t a,size_t n){return (a>=0x1000&&(unsigned long long)a+n<=sizeof memory)||(a>=0xA0E54498u&&(unsigned long long)a+n<=0xA0E54638u)||(a>=0xA0E5B240u&&(unsigned long long)a+n<=0xA0E5B2A4u);}
static uint32_t s2_touch_owner_manager(void){return owner;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int nargs,const uint32_t *args){
 ++registry_calls;if(va!=0xA09737BFu||self!=0x1000||nargs!=1||(*args!=0x2000&&*args!=0x3000))failed=4;
 unsigned bit=*args==0x3000?1:2;g_eax=0xDEAD;g_ecx=0xDEAD;g_esp=0xDEAD;
 return (registry_bits&bit)?0xA5A50001u:0xA5A50000u;
}
static int minimal_requested=-1,minimal_resume,minimal_startup;
static int apply_minimal_graphics(int on,int resume){(void)on;(void)resume;return 1;}
static void void_input_screen_size(int *w,int *h){*w=input_w;*h=input_h;}
static void s2_ui_scale_requested(int);
static void sub_A0972F48_lifted(void);
'''
    actual = '\n'.join(function(source, signature) for signature in [
        'static void touch_publish(', 'void s2_game_resolution(', 'static void ui_fit(',
        'void s2_fit_tick(', 'static const char *fit_owner_name(', 'static void ui_owner_fit(', 'static int ui_modal_gen(', 'static int ui_story_window(', 'static int ui_modal_fits(',
        'void sub_A0972F48(void)'])
    actual += (ROOT / 'tools/fixtures/modal-fit-v26.c').read_text()
    checks = r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
static void rect(uint32_t w,int l,int t,int r,int b){MEM32(w+0x24)=MEM32(w+0xC8)=l;MEM32(w+0x28)=MEM32(w+0xCC)=t;MEM32(w+0x2C)=MEM32(w+0xD0)=r;MEM32(w+0x30)=MEM32(w+0xD4)=b;}
static void s2_ui_scale_requested(int scale){
 ++requests;if(scale!=ui_scale)failed=1;
 s2_game_resolution(&input_w,&input_h);rect(0x2000,0,0,input_w,input_h);
}
static void sub_A0972F48_lifted(void){
 ++calls;if(!(touch_context&S2_TOUCH_MODAL)||ui_fit_depth!=want_depth)failed=2;
 /* Original RunModal attaches this registered parentless Gen before its pump. */
 uint32_t window=MEM32(g_esp+4);if(window==0x3000&&!MEM32(window+0x58))MEM32(window+0x58)=0x2000;
 uint32_t *owner_scope=during_owner;during_owner=0;
 if(owner_scope)ui_owner_fit(owner_scope,0x6000,1);
 if(change_scale){
  ui_scale=change_scale;change_scale=0;s2_ui_scale_requested(ui_scale);
  if(input_w!=expect_width||input_h!=expect_height)failed=5;
 }
 if(owner_scope)ui_owner_fit(owner_scope,0x6000,0);
 if(nest){
  unsigned mode=nest;nest=0;uint32_t sp=g_esp,self=g_ecx;unsigned old_want=want_depth;
  unsigned before=ui_fit_depth,before_height=ui_fit_height_depth;
  int before_right=ui_modal_floor_right,before_bottom=ui_modal_floor_bottom;
  g_esp-=16;MEM32(g_esp+4)=mode==1?0x4000:mode==3?0x5000:0x3000;
  change_scale=nested_scale;nested_scale=0;
  want_depth=before+(mode==1);sub_A0972F48();
  if(ui_fit_depth!=before||ui_fit_height_depth!=before_height||!(touch_context&S2_TOUCH_MODAL))failed=3;
  if(ui_modal_floor_right!=before_right||ui_modal_floor_bottom!=before_bottom)failed=6;
  if(nested_width&&(input_w!=nested_width||input_h!=nested_height))failed=7;
  g_esp=sp;g_ecx=self;want_depth=old_want;
 }
 g_eax=0x12345678u;g_esp+=8;
}
static void setup(int scale){
 clear();ui_scale=scale;phys_w=2340;phys_h=1080;
 ui_modal_floor_right=ui_modal_floor_bottom=0;
 ui_fit_linger=0;ui_fit_linger_ns=0;ui_fit_linger_until=0;fake_ns=1000;
 ui_fit_depth=ui_fit_height_depth=touch_modal_depth=touch_mode_flags=touch_input_flags=0;
 ui_fit_option=ui_fit_cas=ui_fit_chooser=ui_fit_loading=ui_fit_town_loading=ui_fit_lot_loading=ui_fit_lot_info=0;
 requests=calls=nest=want_depth=failed=registry_calls=change_scale=0;registry_bits=3;touch_context=0;owner=0x1000;
 nested_scale=nested_width=nested_height=0;expect_width=1300;expect_height=600;during_owner=0;
 g_eax=0xCAFEBABEu;g_ecx=owner;g_esp=0xF000;MEM32(g_esp+4)=0x3000;
 MEM32(owner)=0xA0E5B240u;MEM32(0xA0E5B298u)=0xA09737BFu;
 MEM32(0xA0E544C4u)=0xA01DFF70u;MEM32(0xA0E544C8u)=0xA0932FF7u;
 MEM32(0xA0E544E4u)=0xA093061Au;MEM32(0xA0E54578u)=0xA0931190u;MEM32(0xA0E54634u)=0xA0930463u;
 MEM32(0x2000)=MEM32(0x3000)=0xA0E54498u;
 MEM32(0x1044)=0x2000;MEM32(0x2004)=MEM32(0x3004)=MEM32(0x4004)=owner;
 MEM32(0x4058)=0x2000;MEM32(0x30E8)=0x0007E107u;
 s2_game_resolution(&input_w,&input_h);rect(0x2000,0,0,input_w,input_h);
 /* Actual v27 RunModal ENTRY: Gen, same owner, parent0, local==global222,125,595,357. */
 rect(0x3000,222,125,595,357);rect(0x4000,0,0,1600,800);
}
__declspec(dllexport) unsigned check(void){
 /* The story remains attached after its modal closes. Never retain a fit owner. */
 for(unsigned kind=0;kind<2;++kind){
  setup(300);MEM32(0x30A0)=0x6000;MEM32(0x6000)=0x6100;MEM32(0x6100)=0x6000;
  MEM32(0x6108)=0x6200;MEM32(0x6200)=kind?0xA0DE1564u:0xA0DE145Cu;MEM32(0x6210)=0x3000;
  REQUIRE(ui_story_window(owner,0x3000));want_depth=1;sub_A0972F48();
  REQUIRE(!failed&&!requests&&!ui_fit_depth&&!ui_fit_height_depth&&input_h==600);
  REQUIRE(ui_story_window(owner,0x3000));
  MEM32(0x6210)=0x4000;REQUIRE(!ui_story_window(owner,0x3000));
  MEM32(0x6210)=0x3000;MEM32(0x6200)=0xA0DE1458u;REQUIRE(!ui_story_window(owner,0x3000));
  MEM32(0x6100)=0x6100;REQUIRE(!ui_story_window(owner,0x3000));
  MEM32(0x6108)=0xFFFC;REQUIRE(!ui_story_window(owner,0x3000));
  MEM32(0x30A0)=0xFFFC;REQUIRE(!ui_story_window(owner,0x3000));
  REQUIRE(!ui_story_window(owner+4,0x3000)&&!ui_story_window(owner,0));
 }
 for(unsigned scale=200;scale<=300;scale+=100){
  setup(scale);REQUIRE(ui_modal_fits(owner,0x3000));
  REQUIRE(!ui_modal_fits_v26(owner,0x3000));
  REQUIRE(registry_calls==2&&g_eax==0xCAFEBABEu&&g_ecx==owner&&g_esp==0xF000);
  sub_A0972F48();REQUIRE(!requests&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_modal_depth&&!touch_context&&!failed);
  REQUIRE(calls==1&&registry_calls==4&&g_eax==0x12345678u&&g_ecx==owner&&g_esp==0xF008&&MEM32(0x3058)==0x2000);
 }
 /* Unknown, detached, malformed and outside actual/target viewport all retain full fit. */
 for(unsigned bad=0;bad<40;++bad){
  setup(200);
  switch(bad){
   case 0:owner=0;break;
   case 1:g_ecx=0x20000;break;
   case 2:MEM32(g_esp+4)=0;break;
   case 3:MEM32(g_esp+4)=0xFF80;break;
   case 4:MEM32(0x3004)=0x1004;break;
   case 5:MEM32(0x1044)=0;break;
   case 6:MEM32(0x1044)=0x3000;break;
   case 7:MEM32(0x2004)=0;break;
   case 8:MEM32(0x2058)=0x3000;break;
   case 9:MEM32(0x2024)=1;break;
   case 10:MEM32(0x3024)=0xFFFFFFFFu;break;
   case 11:MEM32(0x3028)=0xFFFFFFFFu;break;
   case 12:MEM32(0x302C)=MEM32(0x3024);break;
   case 13:MEM32(0x302C)=1301;break;
   case 14:MEM32(0x3030)=601;break;
   case 15:input_w=500;break;
   case 16:MEM32(0x202C)=500;break;
   case 17:registry_bits=2;break;
   case 18:MEM32(0x3058)=0x3000;break;
   case 19:ui_scale=300;MEM32(0x3030)=601;break;
   case 20:phys_w=1000;break;
   case 21:input_h=300;break;
   case 22:MEM32(0x2030)=300;break;
   case 23:MEM32(0x3058)=0x5000;for(unsigned i=0;i<17;++i)MEM32(0x5058+i*0x100)=i==16?0x2000:0x5100+i*0x100;break;
   case 24:registry_bits=1;break;
   case 25:MEM32(0x30C8)++;break;
   case 26:MEM32(0x20C8)++;break;
   case 27:rect(0x3000,0,0,0,0);break;
   case 28:MEM32(0x3000)=0x5000;break;
   case 29:MEM32(0x2000)=0x5000;break;
   case 30:MEM32(0x1000)=0x5000;break;
   case 31:MEM32(0xA0E544C4u)=0;break;
   case 32:MEM32(0xA0E544C8u)=0;break;
   case 33:MEM32(0xA0E544E4u)=0;break;
   case 34:MEM32(0xA0E54578u)=0;break;
   case 35:MEM32(0xA0E54634u)=0;break;
   case 36:MEM32(0xA0E5B298u)=0;break;
   case 37:input_w++;break;
   case 38:MEM32(0x202C)++;MEM32(0x20D0)++;break;
   case 39:MEM32(0x3058)=0x5000;break;
  }
  REQUIRE(!ui_modal_fits(g_ecx,MEM32(g_esp+4)));
  want_depth=1;sub_A0972F48();
  REQUIRE(!failed&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_modal_depth&&!touch_context);
  REQUIRE(!requests&&calls==1);
 }
 /* Already attached windows retain the original bounded-ancestry decision. */
 setup(200);MEM32(0x3058)=0x2000;REQUIRE(ui_modal_fits_v26(owner,0x3000));
 sub_A0972F48();REQUIRE(!requests&&!registry_calls&&!failed);
 /* Bounded ancestry permits a native intermediate container. */
 setup(200);MEM32(0x3058)=0x5000;MEM32(0x5058)=0x2000;
 REQUIRE(ui_modal_fits(owner,0x3000));sub_A0972F48();REQUIRE(!requests&&!failed);
 /* Nested original modals retain ownership and never resize the renderer. */
 setup(200);nest=1;sub_A0972F48();
 REQUIRE(calls==2&&!requests&&!failed&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_context);
 setup(200);MEM32(g_esp+4)=0x4000;want_depth=1;nest=2;sub_A0972F48();
 REQUIRE(calls==2&&!requests&&!failed&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_context);
 uint32_t *owners[]={&ui_fit_option,&ui_fit_cas,&ui_fit_chooser,&ui_fit_loading,&ui_fit_town_loading,&ui_fit_lot_loading,&ui_fit_lot_info};
 for(unsigned i=0;i<sizeof owners/sizeof owners[0];++i){
  setup(300);ui_owner_fit(owners[i],0x6000,1);REQUIRE(!requests&&ui_fit_depth==1&&ui_fit_height_depth==1);
  want_depth=1;sub_A0972F48();REQUIRE(!requests&&ui_fit_depth==1&&ui_fit_height_depth==1&&!failed);
  ui_owner_fit(owners[i],0x6000,0);REQUIRE(!requests&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_context);
 }
 /* Explicit user changes may resize once; opening or closing a panel may not. */
 setup(200);rect(0x3000,0,0,900,500);change_scale=300;
 sub_A0972F48();REQUIRE(!failed&&requests==1&&ui_scale==300&&input_w==1300&&input_h==600);
 REQUIRE(!ui_fit_depth&&!ui_fit_height_depth&&!touch_context&&!ui_modal_floor_right&&!ui_modal_floor_bottom);
 setup(200);rect(0x3000,0,0,900,500);change_scale=100;expect_width=2340;expect_height=1080;
 sub_A0972F48();REQUIRE(!failed&&requests==1&&ui_scale==100&&input_w==2340&&input_h==1080);
 for(unsigned i=0;i<sizeof owners/sizeof owners[0];++i){
  setup(200);rect(0x3000,0,0,900,500);during_owner=owners[i];change_scale=300;
  sub_A0972F48();REQUIRE(!failed&&requests==1&&input_w==1300&&input_h==600&&ui_scale==300&&!ui_fit_depth&&!ui_fit_height_depth&&!touch_context);
 }
 /* The old linger/owner state cannot change the stable minimum at low render resolution. */
 for(unsigned i=0;i<sizeof owners/sizeof owners[0];++i){
  setup(300);render_scale=33;ui_fit_linger_ns=400;
  ui_owner_fit(owners[i],0x6000,1);REQUIRE(!requests&&input_w==1300&&input_h==600);
  ui_owner_fit(owners[i],0x6000,0);REQUIRE(!requests&&ui_fit_linger==2&&!ui_fit_depth);
  fake_ns+=399;s2_fit_tick();REQUIRE(!requests&&ui_fit_linger==2);
  want_depth=1;sub_A0972F48();want_depth=0;
  REQUIRE(!failed&&!requests&&calls==1&&registry_calls==0&&input_w==1300&&input_h==600);
  ui_owner_fit(owners[(i+1)%7],0x6100,1);REQUIRE(!requests&&!ui_fit_linger&&ui_fit_depth==1);
  ui_owner_fit(owners[(i+1)%7],0x6100,0);
  fake_ns+=400;s2_fit_tick();REQUIRE(!requests&&!ui_fit_linger&&input_w==1300&&input_h==600);
  REQUIRE(!ui_fit_depth&&!ui_fit_height_depth&&!touch_context);
  render_scale=100;
 }
 setup(300);ui_fit_linger_ns=400;ui_fit(1,0);ui_fit(0,0);fake_ns+=400;s2_fit_tick();
 REQUIRE(!requests&&input_w==1300&&input_h==600&&!ui_fit_linger);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-modal-fit-') as directory:
        base = Path(directory)
        c, obj, dll = base/'check.c', base/'check.obj', base/'check.dll'
        c.write_text(prelude + actual + checks)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        loaded = ctypes.CDLL(str(dll))
        try:
            result = loaded.check()
            assert result == 0, ('C line', result)
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(loaded._handle)
    print('PASS actual modal C: fixed canvas at render33..100; zero opening/closing/linger resizes; 40 guarded negatives, parentless registration ABI, nested contexts, 7 owners and explicit user scale changes retained.')


if __name__ == '__main__':
    main()
