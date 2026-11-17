"""Actual C options category hit policy: native enabled buttons remain reachable on a lot."""
from audio_probe import ROOT, compile_probe
from check_virtual_query import function

source = (ROOT / 'android/host/sims2_touch.c').read_text()
harness = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "sims2_settings.h"
static uint32_t words[128];
#define MEM32(p) words[(p)/4]
static unsigned context;
unsigned s2_touch_context(void){return context;}
static int void_vm_readable(uint32_t p,uint32_t n){return p && (uint64_t)p+n<=sizeof words;}
static const char *type=".?AVcGZWinBtn@@";
static const char *touch_type(uint32_t p){assert(p==4);return type;}
'''
harness += function(source, 'static int touch_ui_button(')
harness += r'''
int main(void){
 MEM32(4)=0xA0E53EECu;MEM32(4+0xE8)=3;
 unsigned modes[]={S2_TOUCH_OPTIONS,S2_TOUCH_MODAL,S2_TOUCH_LIVE|S2_TOUCH_OPTIONS,0};
 for(unsigned i=0;i<4;i++){context=modes[i];assert(touch_ui_button(4));}
 context=S2_TOUCH_OPTIONS;MEM32(4+0xE8)=1;assert(!touch_ui_button(4));
 MEM32(4+0xE8)=3;MEM32(4)=1;assert(!touch_ui_button(4));
 MEM32(4)=0xA0E53EECu;type=".?AVcTSWinSceneGraph@@";assert(!touch_ui_button(4));
 assert(!touch_ui_button(0));
 puts("PASS actual options/rewards touch: visible enabled original buttons survive retired Live context; unknown/disabled/world rejected");
 return 0;
}
'''
header = (ROOT / 'android/host/sims2_settings.h').read_text()
compile_probe('options-touch', harness, {'sims2_settings.h':header})
