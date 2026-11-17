"""Build the actual-C FPS parser/binder probe and check its original-game ABI contracts."""
import json
from pathlib import Path
import struct
import subprocess

import pefile
import build_android as B
from check_virtual_query import function


def main():
    pe = pefile.PE(str(B.ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    for table, setter in ((0xA0E54498, 0xA0939083), (0xA0E53EEC, 0xA0628F0E)):
        assert struct.unpack_from('<I', image, table-base+0x168)[0] == setter
    assert struct.unpack_from('<I', image, 0xA0E54498-base+0x98)[0] == 0xA09314F0
    assert pe.get_data(0xA0208280-base, 4) == bytes.fromhex('8d4160c3')  # original GetCaption: this+0x60
    assert pe.get_data(0xA0932BDD-base, 8) == bytes.fromhex('83c1608b01ff6040')  # original caption assignment
    # The referenced entry includes push ebp/sub esp, preceding the internal SEH label.
    assert pe.get_data(0xA0649404-base, 4) == bytes.fromhex('5583ec50')
    assert struct.pack('<I', 0xA0649404) in image
    assert struct.pack('<I', 0xA0649408) not in image
    assert struct.unpack_from('<I', image, 0xA0E53EEC-base+0x14C)[0] == 0xA093621D
    assert pe.get_data(0xA0628F0E-base, 8) == bytes.fromhex('83e904e926d23000')
    assert pe.get_data(0xA0932C95-base, 6) == bytes.fromhex('8b96e8000000')
    source = (B.ROOT / 'android/host/sims2_settings.c').read_text()
    functions = '\n'.join(function(source, signature) for signature in (
        'static void read_display_rates(', 'static int options_caption(',
        'static void options_display_rates(', 'void sub_A0649404(void)'))
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int display_rates[10]={0,30,60},display_rate_count=3;
static uint8_t memory[0x20000],tables[0x3000];
#define GPTR(a) ((uint32_t)(a)>=0xA0E53000u?tables+(uint32_t)(a)-0xA0E53000u:memory+(uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)GPTR(a))
static uint32_t g_ecx=123,g_esp=0x800,alloc_cursor=0x10000;
typedef struct {uint32_t ecx,esp;} void_regs;
static void void_regs_save(void_regs *r){*r=(void_regs){g_ecx,g_esp};}
static void void_regs_load(const void_regs *r){g_ecx=r->ecx;g_esp=r->esp;}
static int setter_calls,allocations,frees,fail_alloc,missing_child,bound,visibility_calls;
static int visible[10];
static char captions[11][160];
static int void_vm_readable(uint32_t a,size_t n){return (a>=0x800&&(uint64_t)a+n<=sizeof memory)||
 (a>=0xA0E53000u&&(uint64_t)a+n<=0xA0E56000ull);}
static uint32_t void_galloc(uint32_t n){if(fail_alloc)return 0;uint32_t p=alloc_cursor;alloc_cursor=(p+n+7)&~7u;assert(alloc_cursor<sizeof memory);++allocations;return p;}
static void void_gfree(uint32_t p){assert(p>=0x10000&&p<sizeof memory);++frees;}
static void void_log(const char *f,...){(void)f;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 assert(args);g_ecx=987;g_esp=0x900;
 if(va==0xA093621Du){assert(n==2&&self>=0x2000&&self<=0x2900&&args[0]==1);visible[(self-0x2000)/0x100]=args[1];++visibility_calls;return 1;}
 assert(n==1);
 if(va==0xA09314F0u){assert(self==0x1000&&*args>=1&&*args<=10);return (int)*args==missing_child?0:0x2000+(*args-1)*0x100;}
 assert((self==0x1000&&va==0xA0939083u)||(self>=0x2000&&self<=0x2900&&va==0xA0628F0Eu));
 uint32_t text=*args;assert(MEM32(text)==0xA0D9B470u&&MEM32(text+24)>15);
 uint32_t chars=MEM32(text+4),length=MEM32(text+20);assert(length<160&&MEM8(chars+length)==0);
 int slot=self==0x1000?0:1+(self-0x2000)/0x100;
 memcpy(captions[slot],GPTR(chars),length+1);++setter_calls;return 1;
}
static void sub_A0649404_lifted(void){assert(g_ecx==123&&g_esp==0x800);++bound;}
static void setup(void){
 memset(memory,0,sizeof memory);memset(tables,0,sizeof tables);memset(captions,0,sizeof captions);
 MEM32(0x1000)=0xA0E54498u;MEM32(0x1060)=0xA0D9B470u;MEM32(0x1064)=0x8000;MEM32(0x1078)=159;
 const char *s="radio:s2FrameRateLimit:int:0:30:60";strcpy((char *)GPTR(0x8000),s);MEM32(0x1074)=strlen(s);
 for(int i=0;i<10;++i)MEM32(0x2000+i*0x100)=0xA0E53EECu;
 MEM32(0xA0E54498u+0x168)=0xA0939083u;MEM32(0xA0E53EECu+0x168)=0xA0628F0Eu;MEM32(0xA0E53EECu+0x14C)=0xA093621Du;
 MEM32(0x804)=0x1000;alloc_cursor=0x10000;setter_calls=allocations=frees=fail_alloc=missing_child=bound=visibility_calls=0;memset(visible,0,sizeof visible);g_ecx=123;g_esp=0x800;
}
'''
    checks = r'''
int main(void){
 setup();setenv("S2_DISPLAY_RATES","120,60,40,60,30",1);read_display_rates();
 assert(display_rate_count==5&&display_rates[0]==0&&display_rates[1]==30&&display_rates[2]==40&&display_rates[3]==60&&display_rates[4]==120);
 sub_A0649404();assert(bound==1&&setter_calls==5&&allocations==frees&&visibility_calls==10);
 for(int i=0;i<10;++i)assert(visible[i]==(i<5));
 assert(!strcmp(captions[0],"radio:s2FrameRateLimit:int:0:30:40:60:120"));
 assert(!*captions[1]&&!strcmp(captions[3],"40")&&!strcmp(captions[5],"120"));
 const char *invalid[]={"0","29","241","60x","60,","60,,120","9999999999999999999999","60:120"};
 for(unsigned i=0;i<sizeof invalid/sizeof *invalid;++i){setenv("S2_DISPLAY_RATES",invalid[i],1);read_display_rates();assert(display_rate_count==5&&display_rates[2]==40);}
 setup();missing_child=7;options_display_rates(0x1000);assert(!setter_calls&&!allocations);
 setup();fail_alloc=1;options_display_rates(0x1000);assert(!setter_calls&&!allocations&&!visibility_calls);
 setup();MEM32(0xA0E53EECu+0x14C)=0;options_display_rates(0x1000);assert(!setter_calls&&!allocations&&!visibility_calls);
 setup();MEM32(0x1000)=0;options_display_rates(0x1000);assert(!setter_calls);
 setup();MEM32(0x1074)=160;options_display_rates(0x1000);assert(!setter_calls);
 setup();MEM8(0x8000)='x';options_display_rates(0x1000);assert(!setter_calls);
 setup();MEM32(0xA0E54498u+0x168)=0;options_display_rates(0x1000);assert(!setter_calls&&allocations==frees);
 setenv("S2_DISPLAY_RATES","40",1);read_display_rates();assert(display_rate_count==3&&display_rates[2]==40);
 setenv("S2_DISPLAY_RATES","40,48,50,60,90,120,144,240",1);read_display_rates();assert(display_rate_count==10&&display_rates[9]==240);
 setenv("S2_DISPLAY_RATES","40,48,50,60,90,120,144,165,240",1);read_display_rates();assert(display_rate_count==10&&display_rates[9]==240);
 puts("PASS actual-C display rates: complete binding values/labels, 40Hz, duplicates, malformed data, original ABI contracts, stale trees, allocation failure and full register restoration");return 0;
}
'''
    out = B.ROOT / 'build/test/display-rates'
    out.mkdir(parents=True, exist_ok=True)
    code, binary = out/'check.c', out/'check-display-rates'
    code.write_text(prelude + functions + checks)
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-Wl,-z,max-page-size=16384',
                    str(code), '-o', str(binary)], cwd=B.ROOT, check=True)
    print('PASS original PE contracts and probe compilation; execute', binary, 'on the selected device')


if __name__ == '__main__':
    main()
