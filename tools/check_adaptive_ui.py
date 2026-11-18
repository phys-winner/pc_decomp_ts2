"""Execute production UI transforms and deferred layout with a guest-call boundary."""
from audio_probe import compile_probe
from check_virtual_query import function
from pathlib import Path

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
static int phys_w=2340,phys_h=1080,ui_scale=300,adaptive_ui=1;
static unsigned ui_fit_height_depth,ui_pie_depth;
static int ui_startup_fit,ui_fit_linger,ui_modal_floor_right,ui_modal_floor_bottom,ui_layout_pending;
static uint64_t ui_fit_linger_until,now;
static double ui_applied_factor=1;
static uint32_t memory[1024],fixture_manager=100;
static uint32_t g_ecx,g_esp,g_eax,pie_bounds[4];
#define MEM32(p) memory[(p)/4]
typedef struct {int bank;} void_regs;
static int calls,epochs,moves,bank;
static uint32_t ui_fit_lot_info;
static unsigned ui_lot_info_refresh_depth;
static int clamp(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}
static uint64_t void_monotonic_ns(void){return now;}
static uint32_t s2_touch_owner_manager(void){return fixture_manager;}
static int void_vm_readable(uint32_t p,size_t n){return p>0 && p+n<sizeof memory;}
static void void_regs_save(void_regs *r){r->bank=bank;}
static void void_regs_load(void_regs *r){bank=r->bank;}
static void s2_touch_reset_cas_pan(void){++epochs;}
static void void_log(const char *s,...){(void)s;}
static void sub_A05CA399_lifted(void){g_eax=123;g_esp+=8;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xBEEF){assert(self==2012 && !n && !args);return 1600;}
 if(va==0xA0932914u && self==2300){assert(n==4);memcpy(pie_bounds,args,sizeof pie_bounds);bank=999;return 1;}
 if(va==0xA0932914u){assert(self==1600 && n==4 && args[0]==650 && args[1]==50 && args[2]==1300 && args[3]==550);++moves;return 1;}
 if(va==0xA0971A55u){assert(self==fixture_manager && !n && !args);++calls;bank=999;return 1;}
 assert(va==0x12345678 && self==1000 && n==2 && args[0]==200 && args[1]==100);return 101;
}
'''
CHECK = r'''
int main(void){
 MEM32(fixture_manager)=0xA0E5B240u;MEM32(fixture_manager+0x44)=1000;
 MEM32(1000)=1100;MEM32(1100+0xBC)=0x12345678;
 int w,h;
 for(int scale=100;scale<=300;++scale){ui_scale=scale;s2_game_resolution(&w,&h);assert(w==2340&&h==1080);}
 ui_scale=300;assert(fabs(ui_desired_factor()-2.925)<1e-6);
 adaptive_ui=0;assert(ui_desired_factor()==1.8);
 ui_fit_height_depth=1;assert(ui_desired_factor()==1.8);ui_fit_height_depth=0;
 ui_modal_floor_bottom=900;assert(ui_desired_factor()==1.8);ui_modal_floor_bottom=0;adaptive_ui=1;
 bank=17;s2_fit_tick();assert(calls==1&&epochs==1&&bank==17);
 assert(s2_ui_pick(1000,585,293)==101);
 for(int i=0;i<1000;++i){ui_layout_pending=1;s2_fit_tick();}assert(calls==1);
 ui_fit_height_depth=1;s2_fit_tick();assert(ui_applied_factor==1.8&&calls==2);
 ui_fit_height_depth=0;ui_fit_linger=2;ui_fit_linger_until=400;now=399;s2_fit_tick();assert(calls==2);
 now=400;ui_pie_depth=1;s2_fit_tick();assert(calls==2&&ui_applied_factor==1.8&&!ui_fit_linger);
 ui_pie_depth=0;s2_fit_tick();assert(calls==3&&ui_applied_factor==2.925);
 ui_startup_fit=1;fixture_manager=0;s2_fit_tick();assert(calls==3);fixture_manager=100;s2_fit_tick();assert(calls==4&&ui_applied_factor==1.8);
 MEM32(1000+0x2C)=1300;MEM32(1000+0x30)=600;
 ui_fit_lot_info=2000;MEM32(2012)=2200;MEM32(2200+0x1C)=0xBEEF;
 MEM32(1600+4)=fixture_manager;MEM32(1600+0x58)=1000;MEM32(1600+0xE8)=3;
 MEM32(1600+0xC8)=1000;MEM32(1600+0xD0)=1650;MEM32(1600+0xCC)=400;MEM32(1600+0xD4)=900;
 ui_reposition_card(fixture_manager,1000);assert(moves==1);
 MEM32(1600+0xC8)=650;MEM32(1600+0xD0)=1300;MEM32(1600+0xCC)=50;MEM32(1600+0xD4)=550;
 ui_reposition_card(fixture_manager,1000);assert(moves==1);
 MEM32(1600+0x58)=0;ui_reposition_card(fixture_manager,1000);assert(moves==1);
 ui_fit_lot_info=0;
 MEM32(1000+0x2C)=808;MEM32(1000+0x30)=360;
 g_ecx=2300;MEM32(g_ecx+4)=fixture_manager;
 MEM32(2300+0x118)=MEM32(2300+0x11C)=1;
 for(int corner=0;corner<2;++corner){
  g_esp=2700;MEM32(2704)=2800;
  int left=corner?700:-30,top=corner?320:-50;
  MEM32(2800)=left;MEM32(2804)=top;MEM32(2808)=left+200;MEM32(2812)=top+220;
  MEM32(2300+0x108)=left+100;MEM32(2300+0x10C)=top+110;
  ui_applied_factor=3;sub_A05CA399();
  assert(g_esp==2708&&bank==17&&g_ecx==2300);
  assert(pie_bounds[0]==(corner?607u:1u)&&pie_bounds[1]==(corner?139u:1u));
  assert(pie_bounds[2]==pie_bounds[0]+200&&pie_bounds[3]==pie_bounds[1]+220);
  assert(!memcmp(pie_bounds,&MEM32(2800),sizeof pie_bounds));
  assert((int)MEM32(2300+0x110)==left+100&&(int)MEM32(2300+0x114)==top+110);
  assert(MEM32(2300+0x108)==pie_bounds[0]+100&&g_eax==pie_bounds[1]+110&&MEM32(2300+0x10C)==g_eax);
 }
 ui_applied_factor=1;g_esp=2700;sub_A05CA399();assert(g_eax==123&&g_esp==2708);
 for(phys_w=320;phys_w<=4096;phys_w+=127)for(phys_h=240;phys_h<=2160;phys_h+=113){
  s2_game_resolution(&w,&h);assert(w>=800&&h>=600&&!(w&1)&&!(h&1));
  for(ui_scale=100;ui_scale<=300;ui_scale+=25){
   ui_startup_fit=0;double gameplay=ui_desired_factor();assert(gameplay>=1&&w/gameplay>=799.999&&h/gameplay>=359.999);
   ui_startup_fit=1;double modal=ui_desired_factor();assert(modal>=1&&modal<=gameplay&&w/modal>=799.999&&h/modal>=599.999);
  }
 }
 puts("PASS production adaptive UI: fixed render canvas, bounded transforms, inverse hit-test, 1000 coalesced refreshes, deferred owner availability, ABI and 400ms restore; no device reset boundary");
 return 0;
}
'''

CARD = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t memory[8],g_ecx=0x200C,g_esp,g_eax,ui_fit_lot_info;
static unsigned ui_lot_info_refresh_depth;
static int fits,shows,show_ok=1,expected_fits;
#define MEM32(p) memory[(p)/4]
static void ui_owner_fit(uint32_t *owner,uint32_t self,int open){
 if(open)*owner=self;else if(*owner==self)*owner=0;
}
static void s2_fit_tick(void){assert(ui_fit_lot_info==0x2000);++fits;}
static void sub_A07B7B27_lifted(void){
 assert(fits==expected_fits && g_ecx==0x200C && g_esp==0);
 ++shows;g_eax=show_ok;g_esp+=16;
}
'''
CARD_CHECK = r'''
int main(void){
 expected_fits=1;sub_A07B7B27();assert(shows==1&&g_esp==16&&ui_fit_lot_info==0x2000&&g_eax==1);
 g_esp=0;sub_A07B7B27();assert(shows==2&&fits==1);
 ui_lot_info_refresh_depth=1;ui_fit_lot_info=0;g_esp=0;sub_A07B7B27();assert(!ui_fit_lot_info&&fits==1);
 ui_lot_info_refresh_depth=0;show_ok=0;
 for(int mode=0;mode<4;++mode){
  ui_fit_lot_info=0;g_esp=0;MEM32(12)=mode;expected_fits=fits+1;sub_A07B7B27();
  assert(g_esp==16&&!g_eax&&((mode==0||mode==3)?!ui_fit_lot_info:ui_fit_lot_info==0x2000));
 }
 puts("PASS production card Show: hidden prefit before portrait/layout creation, repeated/reentrant opens, failed modes and guest arguments");
 return 0;
}
'''

if __name__ == '__main__':
    source = Path(__file__).resolve().parents[1].joinpath('android/host/sims2_settings.c').read_text(encoding='utf-8')
    actual = '\n'.join(function(source, name) for name in ['void s2_game_resolution(', 'uint32_t s2_ui_pick(', 'static double ui_desired_factor(', 'static void ui_reposition_card(', 'void s2_fit_tick(', 'void sub_A05CA399(void)\n{'])
    compile_probe('adaptive-ui', PRELUDE + actual + CHECK, {})
    compile_probe('adaptive-card', CARD + function(source, 'void sub_A07B7B27(void)\n{') + CARD_CHECK, {})
