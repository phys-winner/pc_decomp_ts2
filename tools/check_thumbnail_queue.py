"""Exercise the production thumbnail queue against the original callbacks' guest ABI."""
from pathlib import Path

from audio_probe import compile_probe

HOST = r"""
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static uint32_t mem[16384],g_ecx,g_eax,g_edi,g_ebx,g_esi,g_esp;
static uint32_t *word(uint32_t address) {
 if(address>=0xA0E54460u)address=0x600+(address-0xA0E54460u);
 return &mem[address/4];
}
#define MEM32(a) (*word(a))
static int main_thread=1, refs, draws, clears, cancels, reentry, synchronous;
static uint32_t last_key;
static int void_is_main_guest_thread(void) { return main_thread; }
static int void_vm_readable(uint32_t a,uint32_t n) {return a && (uint64_t)a+n<=sizeof mem;}
typedef struct { uint32_t eax,ecx,esp; } void_regs;
static void void_regs_save(void_regs *r) {*r=(void_regs){g_eax,g_ecx,g_esp};}
static void void_regs_load(const void_regs *r) {g_eax=r->eax;g_ecx=r->ecx;g_esp=r->esp;}
static void void_log(const char *fmt,...) {(void)fmt;}
static uint64_t void_monotonic_ns(void) {return 0;}
static void s2_preview_tick(void);
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int nargs,const uint32_t *args) {
 switch (va) {
 case 0xA0146A55u: assert(nargs==0);++refs;break;
 case 0xA05BDFC7u: assert(nargs==0&&refs>0);--refs;break;
 case 0xA05D8015u: assert(nargs==1);last_key=*args;++draws;break;
 case 0xA05DA671u: assert(nargs==0&&self==0x240);++draws;
      if(reentry)s2_preview_tick();break;
 default: assert(va==5&&nargs==1&&!*args);++clears;break;
 }
 g_eax=g_ecx=g_esp=0xDEADu;
 return 1;
}
static void synchronous_family(void){++synchronous;g_esp+=24;}
static void (*void_resolve(uint32_t va))(void) {assert(va==14);return synchronous_family;}
void sub_A07444D5(void) { ++synchronous;g_esp+=4; }
void sub_A05D8ED0_lifted(void) { ++cancels;g_esp+=12; }
void sub_A05DBD1C_lifted(void) { ++cancels;g_esp+=12; }
"""

CASES = r"""
int main(void)
{
 uint32_t family=0x140,lot=0x240,bitmap=0x300;
 MEM32(family)=0xA0DDB040u;MEM32(lot)=0xA0DDB444u;
 MEM32(family+12)=MEM32(lot+12)=0x500;
 MEM32(family+20)=MEM32(lot+20)=bitmap;
 MEM32(bitmap)=0xA0E54460u;MEM32(0x600+16)=5;
 MEM32(0x400+24)=14;g_esi=0x400;g_edi=family;g_ebx=lot;
 MEM32(family+4)=8;MEM32(lot+36)=9;

 g_esp=64;g_ecx=11;s2_defer_family_preview();
 assert(refs==1&&job_count==1&&g_eax==0&&g_esp==88&&g_ecx==11);
 s2_preview_tick();assert(!draws);
 s2_defer_family_preview();assert(refs==1&&job_count==1);
 MEM32(family+4)=16;s2_defer_family_preview();assert(refs==1&&job_count==1);
 s2_defer_lot_preview();assert(refs==2&&job_count==2&&g_esp==140);
 s2_preview_frame_submitted();s2_preview_tick();s2_preview_tick();
 assert(draws==1&&last_key==16&&refs==1&&job_count==1);
 reentry=1;s2_preview_frame_submitted();s2_preview_tick();assert(draws==2&&!refs&&!job_count);
 s2_defer_lot_preview();g_ecx=lot-8;sub_A05DBD1C();
 s2_preview_frame_submitted();s2_preview_tick();assert(draws==2&&!refs&&!job_count&&cancels==1);
 s2_defer_family_preview();MEM32(family+4)=80;s2_preview_frame_submitted();s2_preview_tick();
 assert(draws==2&&!refs&&!job_count);
 main_thread=0;s2_defer_family_preview();s2_defer_lot_preview();
 assert(synchronous==2&&refs==0&&job_count==0);
 puts("PASS production preview queue: owner thread, frame pacing, coalescing, detach, changed selection, ABI");
 return 0;
}
"""

if __name__ == "__main__":
    original = Path(__file__).parents[1].joinpath("android/host/sims2_previews.c").read_text()
    source = original.split('#include "host.h"', 1)[1]
    compile_probe("preview-queue", HOST + source + CASES, {})
