"""Actual C duplicate material reset scope and ABI contracts."""
from audio_probe import ROOT, compile_probe
from check_virtual_query import function

HARNESS = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[0x1000];
#define MEM32(p) (*(uint32_t *)(memory+(p)))
static uint32_t g_ecx,g_esp,g_eax;
static int active,original,recreated,invalid;
static void void_log(const char *format,...){(void)format;}
static int s2_ui_take_material_resize(void){int result=active;active=0;return result;}
static int void_vm_readable(uint32_t p,uint32_t n){return !invalid && p>0 && (uint64_t)p+n<=sizeof memory;}
static void sub_A0ACB780_lifted(void){++original;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 assert(va==0xA0ACBE90 && self==0x100 && n==0 && !args);++recreated;return 17;
}
#include "resize_material.h"
int main(void){
 for(unsigned mode=0;mode<7;mode++){
  memset(memory,0,sizeof memory);active=1;invalid=0;original=recreated=0;
  g_ecx=0x100;g_esp=0x400;g_eax=99;MEM32(0x100)=0xA0E78514;MEM32(0x404)=0x600;MEM32(0x604)=1;
  if(mode==1)active=0;
  if(mode==2)MEM32(0x100)=0xA0E78518;
  if(mode==3)MEM32(0x604)=0;
  if(mode==4)MEM32(0x404)=0;
  if(mode==5)MEM32(0x404)=0xFFFF;
  if(mode==6)invalid=1;
  sub_A0ACB780();
  assert(mode ? original==1 && !recreated && g_esp==0x400 : !original && recreated==1 && g_esp==0x408 && g_eax==17 && !active);
 }
 puts("PASS duplicate material resize: exact UI-only reset/type/event/memory/ABI; original buffers recreated and all other paths retained");
 return 0;
}
'''

if __name__ == "__main__":
 source=(ROOT/"android/host/sims2_patches.c").read_text(encoding="utf-8")
 compile_probe("material-resize",HARNESS,{"resize_material.h":function(source,"void sub_A0ACB780(void)")})
