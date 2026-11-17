"""Actual native purchase-facing guard preserves the selected orientation and the ABI."""
from audio_probe import ROOT, compile_probe
from check_virtual_query import function
source = (ROOT / 'android/host/sims2_patches.c').read_text()
touch = (ROOT / 'android/host/sims2_touch.c').read_text()
harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "sims2_settings.h"
static uint32_t words[256];
#define MEM32(p) words[(p)/4]
static uint32_t g_ecx,g_esp,g_eax;
static unsigned fixture_context,original;
unsigned s2_touch_context(void){return fixture_context;}
static int void_vm_readable(uint32_t p,uint32_t n){return p && (uint64_t)p+n<=sizeof words;}
static void sub_A0680D8B_lifted(void){++original;g_eax=7;g_esp+=4;}
static void sub_A067D1BD_lifted(void){++original;g_eax=9;g_esp+=8;}
'''
harness += function(source, 'void sub_A0680D8B(void)')
harness += function(touch, 'void sub_A067D1BD(void)')
harness += r'''
int main(void){
 unsigned modes[]={S2_TOUCH_BUY,S2_TOUCH_BUILD,S2_TOUCH_LIVE,S2_TOUCH_BUY|S2_TOUCH_MODAL,0};
 for(unsigned i=0;i<5;i++){
  fixture_context=modes[i];original=0;g_ecx=4;g_esp=500;g_eax=99;MEM32(4)=0xA0DE83FCu;MEM32(4+0xA0)=5;
  sub_A0680D8B();assert(g_esp==504&&MEM32(4+0xA0)==5);
  assert(original==(i<2?0u:1u)&&g_eax==(i<2?1u:7u));
 }
 fixture_context=S2_TOUCH_BUY;original=0;g_ecx=4;MEM32(4)=1;sub_A0680D8B();assert(original==1);
 original=0;g_ecx=1000;sub_A0680D8B();assert(original==1);
 for(unsigned i=0;i<5;i++){
  fixture_context=modes[i];original=0;g_ecx=4;g_esp=500;g_eax=99;MEM32(4)=0xA0DE83FCu;MEM32(4+0xA0)=5;
  sub_A067D1BD();assert(g_esp==508&&MEM32(4+0xA0)==5);
  assert(original==(i<2?0u:1u)&&g_eax==(i<2?1u:9u));
 }
 fixture_context=S2_TOUCH_BUY;original=0;g_ecx=4;MEM32(4)=1;sub_A067D1BD();assert(original==1);
 original=0;g_ecx=1000;sub_A067D1BD();assert(original==1);
 puts("PASS purchase/pickup facing: Buy/Build exact object keeps orientation; mouse-drag facing cannot rotate stationary pickup; other modes/types and unreadable objects keep original behavior/ABI");
 return 0;
}
'''
compile_probe('object-facing', harness, {'sims2_settings.h':(ROOT/'android/host/sims2_settings.h').read_text()})
