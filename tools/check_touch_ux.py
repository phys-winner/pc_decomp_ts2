"""Run production hit expansion and pinned inertia against explicit window/input boundaries."""
from pathlib import Path
from audio_probe import compile_probe
from check_virtual_query import function

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
static uint32_t memory[16384];
#define MEM32(p) memory[(p)/4]
enum { S2_TOUCH_MODAL=1,S2_TOUCH_OPTIONS=2,S2_TOUCH_LOT_INFO=32 };
static uint32_t scene_window=0x2900;
static unsigned fixture_context=128,fixture_epoch=3,fixture_flags=8,reenter;
static uint32_t fixture_manager=0x800,fixture_pick=0x2000;
static double fixture_factor=2;
static int wheels,warps,pages;
static unsigned ui_target_logs,fling_logs;
static void void_log(const char *s,...){(void)s;}
static int void_vm_readable(uint32_t p,size_t n){return p>0&&(uint64_t)p+n<sizeof memory;}
static unsigned s2_touch_context(void){return fixture_context;}
static uint32_t fixture_modal;
static uint32_t s2_touch_modal_window(void){return fixture_modal;}
static unsigned s2_touch_layout_epoch(void){return fixture_epoch;}
static uint32_t s2_touch_owner_manager(void){return fixture_manager;}
static double s2_ui_factor(void){return fixture_factor;}
static void void_input_screen_size(int *w,int *h){*w=2340;*h=1080;}
typedef void (*void_fn)(void);
static uint32_t g_esp=0x9000;
static int void_vm_writable(uint32_t p,uint32_t n){return void_vm_readable(p,n);}
static void stub_fn(void){}
static void_fn void_dispatch_find(uint32_t va){return va==0xA093282Au?stub_fn:NULL;}
/* Model of the menu: ids map to item windows; A05C929E hits when a visible,
 * mouse-visible descendant (A0931458) of the item window holds the point. */
static uint32_t model_ids[4]={0xFFFFFFFEu,0x3E8u,0x3E9u},model_windows[4]={0x2600,0x2000,0x2A00};
static int convert_dx;static unsigned direct_calls;
static int model_hit(uint32_t w,int x,int y){
 uint32_t list=MEM32(w+0x54);if(!list)return 0;
 uint32_t head=MEM32(list);
 for(uint32_t node=MEM32(head);node&&node!=head;node=MEM32(node)){
  uint32_t c=MEM32(node+8);
  if(!(MEM32(c+0xE8)&1)||(MEM32(c+0xE8)&0x200000))continue;
  if(model_hit(c,x,y))return 1;
  if(x>=(int)MEM32(c+0x24)&&x<(int)MEM32(c+0x2C)&&y>=(int)MEM32(c+0x28)&&y<(int)MEM32(c+0x30))return 1;
 }
 return 0;
}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xA093282Au){assert(n==2&&self==0x1000&&args[0]>=g_esp);MEM32(args[0])-=(uint32_t)convert_dx;return 0;}
 assert(va==0xA05C929Eu&&n==3&&self==0x3000);++direct_calls;
 for(unsigned i=0;i<3;++i)if(model_ids[i]==args[0])return model_hit(model_windows[i],(int)args[1],(int)args[2]);
 assert(!"unknown item id");return 0;
}
static const char *touch_type(uint32_t w){return MEM32(w)==0xA0E53EECu?".?AVcGZWinBtn@@":"";}
static unsigned touch_widget(uint32_t w){return w==0x2800?fixture_flags:0;}
static int fixture_world_x;
static uint32_t s2_ui_pick(uint32_t root,int x,int y){
 assert(root==0x1000);if(fixture_world_x&&x>=fixture_world_x)return scene_window;if(reenter==1)++fixture_epoch;if(reenter==2)++fixture_context;
 if(reenter==3)MEM32(0x844)=0;
 if(reenter==4)MEM32(0x834)=0;
 if(fixture_pick==0x2000&&x>260)return 0x2400;
 (void)y;return fixture_pick;
}
static const uint32_t *touch_vertical_set(uint32_t w,uint32_t *p){(void)w;(void)p;return NULL;}
static uint32_t touch_vertical_band(uint32_t r,int x,int y){(void)r;(void)x;(void)y;return 0;}
static int touch_layout_matches(unsigned c,uint64_t s,unsigned e){return c==fixture_context&&e==fixture_epoch&&s==((uint64_t)1080<<32|2340);}
static void void_input_mouse_warp(int x,int y){assert(x==400&&y==500);++warps;}
static void void_input_mouse_wheel(int delta){wheels+=delta;}
static int touch_catalog_apply(int x,int y,int amount,int info,unsigned c,uint64_t s,unsigned e){
 assert(x==400&&y==500&&amount==2&&!info&&touch_layout_matches(c,s,e));++pages;return 1;
}
'''
CHECK = r'''
static void window(uint32_t w,uint32_t parent,int l,int t,int r,int b){
 MEM32(w)=0xA0E53EECu;MEM32(w+4)=fixture_manager;MEM32(w+0x58)=parent;MEM32(w+0xE8)=3;
 MEM32(w+0x24)=l;MEM32(w+0x28)=t;MEM32(w+0x2C)=r;MEM32(w+0x30)=b;
}
static void reset(void){
 memset(memory,0,sizeof memory);fixture_context=128;fixture_epoch=3;fixture_manager=0x800;fixture_factor=2;reenter=0;
 fixture_pick=0x2000;fixture_flags=8;wheels=warps=pages=0;MEM32(fixture_manager+0x44)=0x1000;
 window(0x1000,0,0,0,1300,600);MEM32(0x1000)=1;
 window(0x2000,0x1000,100,100,120,120);window(0x2400,0x1000,140,100,160,120);
 MEM32(0x1054)=0x4000;MEM32(0x4000)=0x4010;MEM32(0x4010)=0x4020;
 MEM32(0x4020)=0x4030;MEM32(0x4028)=0x2000;MEM32(0x4030)=0x4010;MEM32(0x4038)=0x2400;
 MEM32(0x1000)=0x5000;
}
static void pie(void){
 window(0x3000,0x1000,0,0,500,500);MEM32(0x3000)=0xA0DDA24Cu;
 MEM32(0x3054)=MEM32(0x1054);MEM32(0x1054)=0;
 MEM32(0x2000)=MEM32(0x2400)=0xA0E54498u;
 MEM32(0x2058)=MEM32(0x2458)=0x3000;
 MEM32(0x20A0)=0x4100;MEM32(0x4100)=0x4120;MEM32(0x4120)=0x4100;MEM32(0x4128)=0x4200;
 MEM32(0x4200)=0xA0DDA548u;
 window(0x2C00,0x2000,101,105,119,115);MEM32(0x2C00)=1;MEM32(0x2C10)=0xCB93E6FCu;
 MEM32(0x2054)=0x4300;MEM32(0x4300)=0x4310;MEM32(0x4310)=0x4320;
 MEM32(0x4320)=0x4310;MEM32(0x4328)=0x2C00;fixture_pick=0x2C00;
}
int main(void){
 reset();window(0x3000,0x1000,0,0,400,400);window(0x2C00,0x3000,0,0,300,300);
 MEM32(0x3000)=0xA0DE3704u;MEM32(0x3010)=0x77D986A1u;MEM32(0x2C00)=0xA0E54B18u;
 assert(touch_live_hud_background(0x2C00));
 MEM32(0x2C10)=0xAAAA;assert(!touch_live_hud_background(0x2C00));
 MEM32(0x2C10)=0;MEM32(0x3010)=0;assert(!touch_live_hud_background(0x2C00));
 reset();int x=176,y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2000&&x==220&&y==220);
 reset();x=174;y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2000);
 reset();x=170;y=220;assert(!touch_ui_target(0x1000,0,&x,&y,96)&&x==170);
 reset();x=272;y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2400&&x==300);
 reset();x=250;y=220;assert(!touch_ui_target(0x1000,0x2400,&x,&y,96)&&x==250);
 reset();MEM32(0x2C58)=0x2400;x=250;y=220;assert(!touch_ui_target(0x1000,0x2C00,&x,&y,96)&&x==250);
 reset();fixture_factor=1;x=66;y=110;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2000&&x==110&&y==110);
 for(int n=0;n<7;++n){
  reset();x=176;y=220;
  if(n==0)fixture_factor=.9;if(n==1)MEM32(0x844)=0;if(n==2)MEM32(0x834)=1;
  if(n==3)MEM32(0x20E8)=1;if(n==4)MEM32(0x10E8)=0;if(n==5)fixture_pick=0x2800;
  if(n==6)MEM32(0x2058)=0;
  assert(!touch_ui_target(0x1000,0,&x,&y,96));
 }
 for(unsigned n=1;n<=3;++n){reset();x=176;y=220;reenter=n;assert(!touch_ui_target(0x1000,0,&x,&y,96));}
 reset();x=176;y=220;assert(!touch_ui_target(0x1000,0x2800,&x,&y,96));
 reset();MEM32(0x4030)=0x4030;x=176;y=220;assert(!touch_ui_target(0x1000,0,&x,&y,96));
 reset();pie();fixture_factor=1.25;x=112;y=138;
 assert(touch_ui_target(0x1000,0x3000,&x,&y,60)==0x2000&&x==138&&y==138);
 MEM32(0x1054)=0x4400;MEM32(0x4400)=0x4410;MEM32(0x4410)=0x4420;
 MEM32(0x4420)=0x4410;MEM32(0x4428)=0x3000;
 fixture_factor=1.75;x=150;y=193;
 assert(touch_ui_target(0x1000,scene_window,&x,&y,80)==0x2000&&x==193&&y==193);
 MEM32(0x834)=0x3000;fixture_factor=1.6;x=144;y=176;
 assert(touch_ui_target(0x1000,0,&x,&y,69)==0x2000&&x==176&&y==176);
 x=144;y=176;assert(!touch_ui_target(0x1000,0x2C00,&x,&y,69)&&x==144);
 assert(touch_ui_target(0x1000,0x2000,&x,&y,69)==0x2000);
 x=144;y=176;
 MEM32(0x20E8)=1;assert(!touch_ui_target(0x1000,0x3000,&x,&y,69));
 MEM32(0x20E8)=3;fixture_pick=0x2400;assert(!touch_ui_target(0x1000,0x3000,&x,&y,69));
 fixture_pick=0x2000;assert(!touch_ui_target(0x1000,0x3000,&x,&y,69));
 fixture_pick=0x2C00;reenter=4;assert(!touch_ui_target(0x1000,0x3000,&x,&y,69));
 /* An open pie: the menu's own direct hit (A05C929E, head first, then each item id) decides. */
 reset();pie();assert(!touch_open_pie(0x800,0x1000));
 MEM32(0x834)=0x3000;assert(touch_open_pie(0x800,0x1000)==0x3000);
 MEM32(0x3058)=0x1000;MEM32(0x5000+0x128)=0xA093282Au;MEM32(0x31A0)=0xFFFFFFFEu;
 MEM32(0x3254)=0x4600;MEM32(0x4600)=0x4700;MEM32(0x4700)=0x4600;MEM32(0x4768)=0x3E8;
 MEM32(0x4030)=0x4060;MEM32(0x4060)=0x4010;MEM32(0x4068)=0x2600;
 window(0x2600,0x3000,0,0,500,500);MEM32(0x26E8)=3|0x200000;MEM32(0x2654)=0x4800;
 MEM32(0x4800)=0x4810;MEM32(0x4810)=0x4820;MEM32(0x4820)=0x4810;MEM32(0x4828)=0x2700;window(0x2700,0x2600,200,200,260,260);
 {int px=220,py=220;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==1&&px==220&&py==220&&direct_calls);
 px=460;py=460;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==1&&px==460&&py==460);
 px=201;py=202;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==2&&px==220&&py==220);
 px=300;py=220;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96)&&px==300&&py==220);
 px=260;py=270;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==2&&px==220&&py==220);
 px=600;py=600;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96)&&px==600&&py==600);
 MEM32(0x20E8)=1;px=220;py=220;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==1);
 px=260;py=270;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96));MEM32(0x20E8)=3;
 MEM32(0x2CE8)=3|0x200000;px=220;py=220;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96));MEM32(0x2CE8)=3;
 convert_dx=10;px=220;py=220;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96));convert_dx=0;
 px=220;py=220;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==1);
 MEM32(0x5128)=0x1234;direct_calls=0;px=220;py=220;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96)&&!direct_calls);MEM32(0x5128)=0xA093282Au;
 fixture_factor=.5;px=220;py=220;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96));fixture_factor=2;
 /* Overlapping rows listed first lose to the nearer label; an unhittable label falls back to the next. */
 MEM32(0x4010)=0x4080;MEM32(0x4080)=0x4020;MEM32(0x4088)=0x2A00;window(0x2A00,0x3000,90,90,130,130);
 MEM32(0x2A54)=0x4900;MEM32(0x4900)=0x4910;MEM32(0x4910)=0x4920;MEM32(0x4920)=0x4910;MEM32(0x4928)=0x2B00;
 window(0x2B00,0x2A00,125,125,130,130);MEM32(0x2B10)=0xCB93E6FCu;
 MEM32(0x4700)=0x4740;MEM32(0x4740)=0x4600;MEM32(0x47A8)=0x3E9;
 px=201;py=202;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==2&&px==220&&py==220);
 MEM32(0x2CE8)=3|0x200000;px=201;py=202;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==2&&px==254&&py==254);
 MEM32(0x2BE8)=3|0x200000;px=201;py=202;assert(!touch_pie_pick(0x3000,0x800,&px,&py,96)&&px==201);MEM32(0x2BE8)=3;MEM32(0x2CE8)=3;
 MEM32(0x2A04)=0x900;MEM32(0x2B04)=0x900;px=201;py=202;assert(touch_pie_pick(0x3000,0x800,&px,&py,96)==2&&px==220);}
 /* Live mode: the open pie holds no capture and is found among the visible windows. */
 reset();pie();MEM32(0x1054)=0x4400;MEM32(0x4400)=0x4410;MEM32(0x4410)=0x4420;MEM32(0x4420)=0x4410;MEM32(0x4428)=0x3000;
 assert(!MEM32(0x834)&&touch_open_pie(0x800,0x1000)==0x3000);
 window(0x5600,0x1000,180,200,590,450);fixture_modal=0x5600;
 assert(!touch_open_pie(0x800,0x1000));
 fixture_modal=0x3000;assert(touch_open_pie(0x800,0x1000)==0x3000);
 fixture_modal=0;
 MEM32(0x30E8)=1;assert(!touch_open_pie(0x800,0x1000));MEM32(0x30E8)=3;
 MEM32(0x3004)=0x900;assert(!touch_open_pie(0x800,0x1000));MEM32(0x3004)=0x800;
 MEM32(0x10E8)=0;assert(!touch_open_pie(0x800,0x1000));MEM32(0x10E8)=3;
 /* The dismissing click lands outside the menu grown by kCancelBoundary, nearest the finger, on screen. */
 reset();pie();MEM32(0x3100)=30;{int px=300,py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1069&&py==300&&px/2.0>530);
 px=1200;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1200&&py==300);
 window(0x3000,0x1000,0,0,1300,600);MEM32(0x3000)=0xA0DDA24Cu;MEM32(0x3100)=30;px=300;py=300;assert(!touch_pie_outside(0x3000,0x1000,&px,&py));
 window(0x3000,0x1000,400,0,1300,600);MEM32(0x3000)=0xA0DDA24Cu;MEM32(0x3100)=30;px=900;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px<=2*366&&py==300);
 MEM32(0x3100)=9999;px=900;assert(!touch_pie_outside(0x3000,0x1000,&px,&py));
 /* The release lands under the cursor: prefer the nearest point over the lot to HUD controls. */
 reset();pie();MEM32(0x3100)=30;fixture_world_x=1100;px=300;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1149&&py==300);
 px=1200;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1200&&py==300);
 fixture_world_x=2000;px=1200;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1200&&py==300);
 px=300;py=300;assert(touch_pie_outside(0x3000,0x1000,&px,&py)&&px==1069&&py==300);fixture_world_x=0;}
 uint64_t size=(uint64_t)1080<<32|2340,owner=(uint64_t)0x1000<<32|0x2800;
 reset();fixture_pick=0x2800;assert(touch_fling_apply(400,500,2,128,size,3,owner)&&wheels==240&&warps==1);
 reset();fixture_pick=0x2800;fixture_flags=8|32;assert(touch_fling_apply(400,500,2,128,size,3,owner)&&pages==1&&!wheels);
 for(unsigned n=0;n<8;++n){
  reset();fixture_pick=0x2800;
  if(n==0)fixture_pick=0x2400;if(n==1)++fixture_epoch;if(n==2)++fixture_context;if(n==3)MEM32(0x834)=1;
  if(n==4)fixture_flags|=512;if(n==5)fixture_flags|=256;if(n==6)fixture_flags|=64;if(n==7)fixture_flags|=4;
  assert(!touch_fling_apply(400,500,2,128,size,3,owner)&&!wheels&&!pages);
 }
 for(unsigned n=1;n<=3;++n){reset();fixture_pick=0x2800;reenter=n;assert(!touch_fling_apply(400,500,2,128,size,3,owner)&&!wheels);}
 puts("PASS production touch UX: expanded 48dp geometry, nearest/direct priority, scale/visibility/occlusion/tree bounds; open-pie item hits by the menu's own A05C929E test (part, row, near miss, overlapping rows, unhittable labels, parent conversion) and dismissal outside the cancel rectangle, over the lot when possible; pinned inertia rejects replacement windows, tabs, sliders, captures and stale/reentrant owners");
 return 0;
}
'''

if __name__ == '__main__':
    source=Path(__file__).resolve().parents[1].joinpath('android/host/sims2_touch.c').read_text(encoding='utf-8')
    actual='\n'.join(function(source,name) for name in ['static int touch_ui_button(', 'static int touch_live_hud_background(', 'static uint32_t touch_pie_owner(',
        'static int touch_pie_item(', 'static uint32_t touch_pie_label(', 'static uint32_t touch_open_pie(', 'static int touch_pie_rect(', 'static int touch_pie_direct(', 'static int touch_pie_hits(', 'static void touch_pie_center(', 'static int touch_pie_pick(', 'static int touch_pie_outside(',
        'static uint32_t touch_ui_target(', 'static int touch_fling_apply('])
    compile_probe('touch-ux',PRELUDE+actual+CHECK,{})
