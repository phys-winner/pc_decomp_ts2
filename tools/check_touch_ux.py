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
static unsigned fixture_context=128,fixture_epoch=3,fixture_flags=8,reenter;
static uint32_t fixture_manager=0x800,fixture_pick=0x2000;
static double fixture_factor=2;
static int fixture_scale=300,wheels,warps,pages;
static unsigned ui_target_logs,fling_logs;
static void void_log(const char *s,...){(void)s;}
static int void_vm_readable(uint32_t p,size_t n){return p>0&&(uint64_t)p+n<sizeof memory;}
static unsigned s2_touch_context(void){return fixture_context;}
static unsigned s2_touch_layout_epoch(void){return fixture_epoch;}
static uint32_t s2_touch_owner_manager(void){return fixture_manager;}
static double s2_ui_factor(void){return fixture_factor;}
static int s2_ui_scale(void){return fixture_scale;}
static const char *touch_type(uint32_t w){return MEM32(w)==0xA0E53EECu?".?AVcGZWinBtn@@":"";}
static unsigned touch_widget(uint32_t w){return w==0x2800?fixture_flags:0;}
static uint32_t s2_ui_pick(uint32_t root,int x,int y){
 assert(root==0x1000);if(reenter==1)++fixture_epoch;if(reenter==2)++fixture_context;
 if(reenter==3)MEM32(0x844)=0;
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
 memset(memory,0,sizeof memory);fixture_context=128;fixture_epoch=3;fixture_manager=0x800;fixture_factor=2;fixture_scale=300;reenter=0;
 fixture_pick=0x2000;fixture_flags=8;wheels=warps=pages=0;MEM32(fixture_manager+0x44)=0x1000;
 window(0x1000,0,0,0,1300,600);MEM32(0x1000)=1;
 window(0x2000,0x1000,100,100,120,120);window(0x2400,0x1000,140,100,160,120);
 MEM32(0x1054)=0x4000;MEM32(0x4000)=0x4010;MEM32(0x4010)=0x4020;
 MEM32(0x4020)=0x4030;MEM32(0x4028)=0x2000;MEM32(0x4030)=0x4010;MEM32(0x4038)=0x2400;
 MEM32(0x1000)=0x5000;
}
int main(void){
 reset();int x=176,y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2000&&x==220&&y==220);
 reset();x=174;y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2000);
 reset();x=170;y=220;assert(!touch_ui_target(0x1000,0,&x,&y,96)&&x==170);
 reset();x=272;y=220;assert(touch_ui_target(0x1000,0,&x,&y,96)==0x2400&&x==300);
 reset();x=250;y=220;assert(!touch_ui_target(0x1000,0x2400,&x,&y,96)&&x==250);
 reset();MEM32(0x2C58)=0x2400;x=250;y=220;assert(!touch_ui_target(0x1000,0x2C00,&x,&y,96)&&x==250);
 for(int n=0;n<8;++n){
  reset();x=176;y=220;
  if(n==0)fixture_factor=1.4;if(n==1)fixture_scale=150;if(n==2)MEM32(0x844)=0;if(n==3)MEM32(0x834)=1;
  if(n==4)MEM32(0x20E8)=1;if(n==5)MEM32(0x10E8)=0;if(n==6)fixture_pick=0x2800;
  if(n==7)MEM32(0x2058)=0;
  assert(!touch_ui_target(0x1000,0,&x,&y,96));
 }
 for(unsigned n=1;n<=3;++n){reset();x=176;y=220;reenter=n;assert(!touch_ui_target(0x1000,0,&x,&y,96));}
 reset();x=176;y=220;assert(!touch_ui_target(0x1000,0x2800,&x,&y,96));
 reset();MEM32(0x4030)=0x4030;x=176;y=220;assert(!touch_ui_target(0x1000,0,&x,&y,96));
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
 puts("PASS production touch UX: expanded 48dp geometry, nearest/direct priority, scale/visibility/occlusion/tree bounds; pinned inertia rejects replacement windows, tabs, sliders, captures and stale/reentrant owners");
 return 0;
}
'''

if __name__ == '__main__':
    source=Path(__file__).resolve().parents[1].joinpath('android/host/sims2_touch.c').read_text(encoding='utf-8')
    actual='\n'.join(function(source,name) for name in ['static int touch_ui_button(', 'static uint32_t touch_ui_target(', 'static int touch_fling_apply('])
    compile_probe('touch-ux',PRELUDE+actual+CHECK,{})
