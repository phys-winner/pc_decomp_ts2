"""Production cheat queue and registry boundary checks; JVM grouping and original PE ABI oracle."""
from pathlib import Path
import json
import struct
import subprocess
import tempfile

import pefile
from audio_probe import ROOT, compile_probe
from check_touch import JDK

HEADER = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <time.h>
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static unsigned locks;
static int pthread_mutex_lock(pthread_mutex_t *p){(void)p;assert(!locks);++locks;return 0;}
static int pthread_mutex_unlock(pthread_mutex_t *p){(void)p;assert(locks==1);--locks;return 0;}
static unsigned fake_seconds=10;
#define CLOCK_MONOTONIC 1
static int test_clock_gettime(int clock,struct timespec *t){(void)clock;t->tv_sec=fake_seconds;t->tv_nsec=0;return 0;}
#define clock_gettime test_clock_gettime
static unsigned char guest[0x1000000];
#define GPTR(p) (guest+((uint32_t)(p)&0xFFFFFF))
#define MEM32(p) (*(uint32_t*)GPTR(p))
#define MEM8(p) (*GPTR(p))
static int void_vm_readable(uint32_t p,uint32_t n){return p && n<=0x1000000u-(p&0xFFFFFFu);}
static uint32_t void_call_guest_this(uint32_t f,uint32_t self,unsigned n,const uint32_t *args);
static uint32_t void_galloc(uint32_t n);
static void void_gfree(uint32_t p);
static void void_log(const char *f,...){(void)f;}
static uint16_t void_cp1252_to_u16(uint8_t c){return c;}
static size_t void_utf8_encode(uint32_t c,char *p){if(c<128){p[0]=(char)c;return 1;}if(c<2048){p[0]=(char)(0xc0|(c>>6));p[1]=(char)(0x80|(c&63));return 2;}if(c<65536){p[0]=(char)(0xe0|(c>>12));p[1]=(char)(0x80|((c>>6)&63));p[2]=(char)(0x80|(c&63));return 3;}p[0]=(char)(0xf0|(c>>18));p[1]=(char)(0x80|((c>>12)&63));p[2]=(char)(0x80|((c>>6)&63));p[3]=(char)(0x80|(c&63));return 4;}
'''

HARNESS = r'''
static unsigned ctx=S2_TOUCH_LIVE,epoch=3,queued,queue_ok=1,width=1280,height=720;
static uint32_t manager=0x2000;static unsigned owner_access;
static unsigned queries,releases,allocations,frees,executions,count_value=2,fail_qi,bad_iface,fail_alloc,mutate_context;
static uint64_t nested;
unsigned s2_touch_context(void){return ctx;}
unsigned s2_touch_layout_epoch(void){return epoch;}
uint32_t s2_touch_owner_manager(void){assert(owner_access);return manager;}
void void_input_screen_size(int *w,int *h){*w=(int)width;*h=(int)height;}
int s2_touch_ui_command(unsigned kind,uint32_t serial){assert(locks==1&&kind==S2_UI_CHEAT);queued=serial;return queue_ok;}
static uint32_t void_galloc(uint32_t n){assert(!locks&&n==517);++allocations;return fail_alloc?0:0x1F000;}
static void void_gfree(uint32_t p){assert(!locks&&p==0x1F000);++frees;}
static uint32_t void_call_guest_this(uint32_t f,uint32_t self,unsigned n,const uint32_t *args){
 assert(!locks);
 if(f==0xA0001000){assert(self==0x3000&&n==3&&args[0]==0x2A4915EA&&args[1]==0x4A490E00);++queries;if(fail_qi)return 0;MEM32(args[2])=0x4000;return 1;}
 if(f==0xA0001010){assert(self==0x4000&&n==0);++releases;return 0;}
 if(f==0xA0CBE190){assert(self==0x4000&&n==0);return 0x5000;}
 if(f==0xA0001020){assert(self==0x5000&&n==0);return count_value;}
 if(f==0xA0001030){assert(self==0x5000&&n==1&&args[0]<2);return 0x6000+args[0]*0x100;}
 if(f==0xA0001040){assert((self==0x6000||self==0x6100)&&n==0);return self==0x6000?0x8000:0x8100;}
 if(f==0xA0001050){assert((self==0x6000||self==0x6100)&&n==1&&!args[0]);if(mutate_context)ctx=S2_TOUCH_MODAL;return self==0x6000?0x8200:0;}
 if(f==0xA0CBE460){assert(self==0x4000&&n==1&&!strcmp((char*)GPTR(args[0]),"motherlode"));++executions;nested=cheat_request("kaching");return 1;}
 assert(0);return 0;
}
static unsigned status(uint64_t r){return(unsigned)r&255;}
static void deliver(unsigned serial,uint32_t target){owner_access=1;s2_cheats_owner(serial,target);owner_access=0;}
static void reset(void){
 cheat_cancel();cheat_clear_rows();cheat_state=CHEAT_IDLE;ctx=S2_TOUCH_LIVE;epoch=3;manager=0x2000;queue_ok=1;width=1280;height=720;
 queries=releases=allocations=frees=executions=fail_qi=bad_iface=fail_alloc=mutate_context=0;count_value=2;
 memset(guest,0,sizeof guest);MEM32(0xA10C9378)=0x3000;MEM32(0x3000)=0x3100;MEM32(0x3114)=0xA0001000;
 MEM32(0x4000)=0xA0EB8188;MEM32(0xA0EB8190)=0xA0001010;
 MEM32(0x5000)=0x5100;MEM32(0x51A0)=0xA0001020;MEM32(0x51A4)=0xA0001030;
 MEM32(0x6000)=0x7000;MEM32(0x6100)=0x7000;MEM32(0x7014)=0xA0001040;MEM32(0x7018)=0xA0001050;
 strcpy((char*)GPTR(0x8000),"motherlode");strcpy((char*)GPTR(0x8100),"help");strcpy((char*)GPTR(0x8200),"Add money");
}
int main(void){
 char encoded[513];uint16_t unicode[]={0x044F,0xD83D,0xDE00},broken[]={0xD800},zero[]={0};
 assert(cheat_encode(unicode,3,encoded)&&!strcmp(encoded,"\xD1\x8F\xF0\x9F\x98\x80"));
 assert(!cheat_encode(broken,1,encoded)&&!cheat_encode(unicode+2,1,encoded)&&!cheat_encode(zero,1,encoded)&&!cheat_encode(unicode,129,encoded));
 reset();uint64_t r=cheat_request(NULL);assert(status(r)==1&&status(cheat_status(queued))==1);deliver(queued,manager);
 assert(status(cheat_status(queued))==2&&cheat_count==2&&!strcmp(cheat_rows[0],"motherlode\tAdd money")&&!strcmp(cheat_rows[1],"help\t"));assert(queries==releases&&allocations==frees);
 deliver(queued,manager);assert(queries==1);
 reset();r=cheat_request("motherlode");deliver(queued,manager);deliver(queued,manager);assert(executions==1&&status(nested)==6&&status(cheat_status(queued))==2&&queries==releases);
 reset();cheat_request("motherlode");cheat_cancel();deliver(queued,manager);assert(!executions&&status(cheat_status(queued))==5);
 reset();cheat_request("motherlode");fake_seconds+=3;deliver(queued,manager);assert(!executions&&status(cheat_status(queued))==5);
 reset();cheat_request("motherlode");++epoch;deliver(queued,manager);assert(!queries&&status(cheat_status(queued))==5);
 reset();cheat_request("motherlode");++width;deliver(queued,manager);assert(!queries);
 reset();cheat_request("motherlode");manager=0x2500;deliver(queued,manager);assert(!queries);
 reset();cheat_request("motherlode");ctx=S2_TOUCH_MODAL;deliver(queued,manager);assert(!queries);
 reset();ctx=S2_TOUCH_OPTIONS;assert(status(cheat_request(NULL))==1);deliver(queued,manager);assert(status(cheat_status(queued))==2&&cheat_count==2);
 reset();ctx=S2_TOUCH_OPTIONS;cheat_request("motherlode");deliver(queued,manager);assert(executions==1&&status(cheat_status(queued))==2);ctx=S2_TOUCH_LIVE;
 for(unsigned mode=S2_TOUCH_TOWN;mode<=S2_TOUCH_BUILD;mode*=2){
  reset();ctx=S2_TOUCH_OPTIONS|mode;assert(status(cheat_request(NULL))==1);deliver(queued,manager);assert(status(cheat_status(queued))==2&&cheat_count==2);
  reset();ctx=S2_TOUCH_OPTIONS|mode|S2_TOUCH_MODAL;assert(status(cheat_request(NULL))==4);
 }
 assert(status(cheat_request(""))==4&&status(cheat_request("help\nquit"))==4);
 reset();queue_ok=0;assert(status(cheat_request(NULL))==6);
 reset();fail_qi=1;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==4&&!releases&&allocations==frees);
 reset();MEM32(0x4000)=0x7100;MEM32(0x7108)=0xA0001010;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==4&&releases==1);
 reset();count_value=8193;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==4&&queries==releases);
 reset();MEM32(0x7014)=0;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==4&&queries==releases);
 reset();fail_alloc=1;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==7&&!queries);
 reset();mutate_context=1;cheat_request(NULL);deliver(queued,manager);assert(status(cheat_status(queued))==4&&queries==releases);
 reset();assert(status(cheat_status(cheat_serial+1))==0);cheat_clear_rows();
 puts("PASS production cheat queue: complete registry boundary, undocumented help, original Execute once, BUSY while STARTED, QI/release, expiry/cancel, context/layout/size/manager and malformed registry guards");return 0;
}
'''

JAVA = r'''package org.sims2.port;
public class CheatsCheck{public static void main(String[] args){
 String[] names={"motherLode","aging","moveObjects","changeLotZoning","setHour","bloom","boolProp","dumpHappy"};
 for(int i=0;i<names.length;i++){CheatCommands.Command c=new CheatCommands.Command(names[i]+"\tOriginal help\nSecond line");assert c.name.equals(names[i]);assert c.group()==i:names[i];assert c.help.equals("Original help\nSecond line");}
 assert new CheatCommands.Command("motherLode\t").title(true).contains("50 000");
 assert new CheatCommands.Command("UnknownCommand\t").title(true).equals("UnknownCommand");
 assert CheatCommands.parse(new String[]{"z\tz",null,"","A\ta"}).get(0).name.equals("A");
 assert CheatCommands.groups(true).length==8&&CheatCommands.groups(false).length==8;
 System.out.println("PASS actual Java cheat catalog: all rows preserved, sorting, 8 groups, Russian annotations and original help preserved");
}}'''


def main():
    production=(ROOT/'android/host/sims2_cheats.c').read_text(encoding='utf-8')
    compile_probe('cheats', production+HARNESS, {'host.h': HEADER,'input.h':'void void_input_screen_size(int *w,int *h);',
        'sims2_cheats.h':(ROOT/'android/host/sims2_cheats.h').read_text(),
        'sims2_settings.h':(ROOT/'android/host/sims2_settings.h').read_text()},extra_flags=('/wd4505',))
    pe=pefile.PE(str(ROOT/'original/Sims2EP9.exe'))
    assert struct.unpack('<9I',pe.get_data(0xeb8188,36)) == (0xA0CBE67E,0xA01CC32D,0xA01CD7AD,0xA0CBE190,0xA0CBE690,0xA0CBE6F0,0xA0CBE6D0,0xA0CBE460,0xA0CBE250)
    source=(ROOT/'generated/sims2-x86/sims2_A0CA8000.c').read_text()
    assert 'RECOMP_ICALL(eax); /* 0xA0CAA70D: call eax */' in source
    assert 'eax = MEM32(edi + 0x1C); /* 0xA0CAA708' in source
    with tempfile.TemporaryDirectory(prefix='sims2-cheats-') as folder:
        out=Path(folder);harness=out/'CheatsCheck.java';harness.write_text(JAVA,encoding='utf-8')
        subprocess.run([str(JDK/'bin/javac.exe'),'--release','8','-encoding','UTF-8','-d',str(out),str(harness),str(ROOT/'android/app/src/org/sims2/port/CheatCommands.java')],check=True)
        subprocess.run([str(JDK/'bin/java.exe'),'-ea','-cp',str(out),'org.sims2.port.CheatsCheck'],check=True)
    print('PASS original PE cITSCheatSystem vtable and original console Execute call ABI')


if __name__=='__main__':main()
