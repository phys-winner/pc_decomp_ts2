"""Real host threads exercise production text request/owner/cancel synchronization.

Guest calls and FIFO delivery are controlled boundaries; real clocks and atomic
locks/waits execute the actual sims2_text.c state machine. ARM64 ABI/getters are
covered by check_native_text.py. No device or production source changes.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from build_android import CLANG, ROOT

OUT=ROOT/'build/test/native-text-v15'
PRELUDE=r'''
#include <stdint.h>
#include "sims2_settings.h"
typedef unsigned long long size_t;
#define NULL ((void *)0)
#define atomic_load(p) __c11_atomic_load(p,5)
#define atomic_store(p,v) __c11_atomic_store(p,v,5)
#define atomic_fetch_add(p,v) __c11_atomic_fetch_add(p,v,5)
typedef _Atomic unsigned pthread_mutex_t;
typedef _Atomic unsigned pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
struct timespec {long long tv_sec,tv_nsec;};
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
static unsigned long long (*fixture_clock)(int);
static int clock_gettime(int c,struct timespec *t){unsigned long long n=fixture_clock(c);t->tv_sec=n/1000000000;t->tv_nsec=n%1000000000;return 0;}
static void pthread_mutex_lock(pthread_mutex_t *p){while(__c11_atomic_exchange(p,1,5)){} }
static void pthread_mutex_unlock(pthread_mutex_t *p){atomic_store(p,0);}
static void pthread_cond_broadcast(pthread_cond_t *c){atomic_fetch_add(c,1);}
static int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *p,const struct timespec *t){
 unsigned sequence=atomic_load(c);pthread_mutex_unlock(p);int expired=0;
 while(atomic_load(c)==sequence){struct timespec n;clock_gettime(CLOCK_REALTIME,&n);if(n.tv_sec>t->tv_sec||(n.tv_sec==t->tv_sec&&n.tv_nsec>=t->tv_nsec)){expired=1;break;}}
 pthread_mutex_lock(p);return expired;
}
static void *memcpy(void *d,const void *s,size_t n){uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d;}
static void *memset(void *d,int c,size_t n){uint8_t *o=d;while(n--)*o++=c;return d;}
static int memcmp(const void *a,const void *b,size_t n){const uint8_t *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;++x;++y;}return 0;}
static uint8_t memory[0x40000];
#define GPTR(a) (memory+((uint32_t)(a)&0x3FFFFu))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*GPTR(a))
static int void_vm_readable(uint32_t a,uint32_t n){return(a>=0x10000&&(uint64_t)a+n<=0x30000)||(a>=0xA0E50000u&&(uint64_t)a+n<=0xA0E60000u);}
static int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}
static uint32_t g_eax,g_ecx,g_esp;
typedef struct {uint32_t eax,ecx,esp;} void_regs;
static void void_regs_save(void_regs *r){*r=(void_regs){g_eax,g_ecx,g_esp};}
static void void_regs_load(const void_regs *r){g_eax=r->eax;g_ecx=r->ecx;g_esp=r->esp;}
static unsigned s2_context=S2_TOUCH_CAS|S2_TOUCH_FOCUSED_TEXT;
unsigned s2_touch_context(void){return s2_context;}
int s2_ui_scale(void){return 200;}
uint32_t s2_touch_owner_manager(void){return 0x11000;}
static int16_t void_input_key_state(uint32_t key){(void)key;return 0;}
static void void_input_screen_size(int *w,int *h){*w=1212;*h=540;}
static uint32_t void_galloc(uint32_t n){(void)n;return 0x25000;}
static void void_gfree(uint32_t a){(void)a;}
static _Atomic unsigned queued,kind,block_set,block_get,blocked,release_callback,refs,setters,releases,bad;
static unsigned value_length;static uint8_t value[513];
int s2_touch_ui_command(unsigned k,uint32_t serial){atomic_store(&kind,k);atomic_store(&queued,serial);return 1;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 g_ecx=self;g_eax=0xFEED;
 if(va==0x703){if(n!=2||self!=0x12000||args[0]!=700||args[1]!=500)atomic_store(&bad,1);return 0x13000;}
 if(va==0xA095079A){if(n!=2||self!=0x13000||args[0]!=0x231A1C57u)atomic_store(&bad,2);MEM32(args[1])=self+0xF8;atomic_fetch_add(&refs,1);return 1;}
 if(va==0xA01C1C80){if(n||self!=0x130F8)atomic_store(&bad,3);if(atomic_load(&block_get)){atomic_store(&blocked,1);while(!atomic_load(&release_callback)){} }memcpy(GPTR(0x23000),value,value_length+1);return 0x23000;}
 if(va==0xA0148BA0){if(n||self!=0x130F8)atomic_store(&bad,4);return value_length;}
 if(va==0xA09520ED){
  if(n!=3||self!=0x130F8||args[2]!=1||atomic_load(&refs)!=2)atomic_store(&bad,5);
  atomic_fetch_add(&setters,1);if(atomic_load(&block_set)){atomic_store(&blocked,1);while(!atomic_load(&release_callback)){} }
  value_length=args[1];memcpy(value,GPTR(args[0]),value_length);value[value_length]=0;return 1;
 }
 if(va==0x702){if(n||self!=0x13000||atomic_load(&refs)!=2)atomic_store(&bad,6);atomic_fetch_add(&releases,1);atomic_fetch_add(&refs,(unsigned)-1);return 1;}
 atomic_store(&bad,7);return 0;
}
'''
CHECK=r'''
__declspec(dllexport) void prepare(void *clock){
 fixture_clock=clock;memset(memory,0,sizeof memory);atomic_store(&queued,0);atomic_store(&kind,0);atomic_store(&block_set,0);atomic_store(&block_get,0);atomic_store(&blocked,0);atomic_store(&release_callback,0);
 atomic_store(&refs,1);atomic_store(&setters,0);atomic_store(&releases,0);atomic_store(&bad,0);value_length=3;memcpy(value,"old",4);
 text_serial=text_state=text_kind=text_result=text_epoch_request=text_abandoned=0;atomic_store(&text_epoch,0);memset(&text_session,0,sizeof text_session);
 atomic_store(&text_lock,0);atomic_store(&text_ready,0);g_eax=0xAA;g_ecx=0xBB;g_esp=0x1F00;
 MEM32(0x11044)=0x12000;MEM32(0x11038)=0x13000;MEM32(0x12000)=0x12200;MEM32(0x122BC)=0x703;MEM32(0x120E8)=3;
 MEM32(0x13000)=0xA0E56138u;MEM32(0x13058)=0x12000;MEM32(0x130E8)=0x8003;MEM32(0x13160)=1;
 MEM32(0x130F8)=0xA0E56404u;MEM32(0xA0E56140u)=0x702;
}
__declspec(dllexport) unsigned read(void){uint8_t bytes[513];unsigned length=0;return s2_touch_text_read(700,500,bytes,&length);}
__declspec(dllexport) unsigned long long replace(void){return s2_touch_text_replace((const uint8_t *)"new value",9);}
__declspec(dllexport) unsigned long long receipt(unsigned s){return s2_touch_text_status(s);}
__declspec(dllexport) void owner(void){unsigned serial=__c11_atomic_exchange(&queued,0,5),k=atomic_load(&kind);if(serial){void_regs save;void_regs_save(&save);s2_touch_text_owner(k,serial,0x11000);void_regs_load(&save);}}
__declspec(dllexport) void cancel(void){s2_touch_cancel_text();s2_touch_text_pump(0x11000);}
__declspec(dllexport) void configure(unsigned k){atomic_store(k?&block_get:&block_set,1);}
__declspec(dllexport) void release(void){atomic_store(&release_callback,1);}
__declspec(dllexport) unsigned state(unsigned k){
 if(k==0)return atomic_load(&queued);if(k==1)return atomic_load(&blocked);if(k==2)return atomic_load(&refs);
 if(k==3)return atomic_load(&setters);if(k==4)return atomic_load(&releases);if(k==5)return atomic_load(&bad);
 if(k==6)return value_length==9&&!memcmp(value,"new value",9);return g_eax==0xAA&&g_ecx==0xBB&&g_esp==0x1F00;
}
'''


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    actual=(ROOT/'android/host/sims2_text.c').read_text(encoding='utf-8')
    helper='uint32_t s2_ui_pick(uint32_t root,int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(MEM32(MEM32(root)+0xBC),root,2,p);}\nuint32_t s2_ui_mouse_pick(uint32_t root,int x,int y){return s2_ui_pick(root,x,y);}\n'
    source=PRELUDE+helper+'\n'.join(line for line in actual.splitlines() if not line.startswith('#include'))+CHECK
    cfile=OUT/'threads.c';cfile.write_text(source,encoding='utf-8')
    cases=[]
    with tempfile.TemporaryDirectory(prefix='sims2-native-text-') as folder:
        obj,library=Path(folder)/'check.obj',Path(folder)/'check.dll'
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-I'+str(ROOT/'android/host'),'-c',str(cfile),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(library),str(obj)],check=True)
        dll=ctypes.CDLL(str(library));workers=[];results={}
        clock_type=ctypes.CFUNCTYPE(ctypes.c_uint64,ctypes.c_int)
        now=clock_type(lambda k:time.monotonic_ns() if k==1 else time.time_ns())
        dll.prepare.argtypes=[ctypes.c_void_p];dll.replace.restype=dll.receipt.restype=ctypes.c_uint64
        def run(name,fn):
            t=threading.Thread(target=lambda:results.__setitem__(name,fn()),daemon=True);workers.append(t);t.start();return t
        def join(t):t.join(3);assert not t.is_alive(),'native worker stuck'
        def wait(k):
            end=time.monotonic()+3
            while not dll.state(k):
                assert time.monotonic()<end,('boundary not reached',k)
                time.sleep(.001)
        def read_session():
            user=run('read',dll.read);wait(0);dll.owner();join(user);assert results['read']==1 and dll.state(2)==2
        try:
            dll.prepare(now);user=run('slow_read',dll.read);wait(0);time.sleep(.4);dll.owner();join(user)
            assert results['slow_read']==1 and dll.state(2)==2 and not dll.state(5)
            user=run('slow_replace',dll.replace);wait(0);time.sleep(.4);dll.owner();join(user)
            receipt=results['slow_replace'];assert receipt>>32 and receipt&255==2 and dll.state(3)==1 and dll.state(2)==1 and dll.state(4)==1 and dll.state(6)==1 and not dll.state(5)
            dll.owner();assert dll.state(3)==1
            cases.append('READ and WRITE queued behind 400 ms game frames complete once with matching receipt and balanced reference')

            dll.prepare(now);read_session();dll.configure(0);user=run('started',dll.replace);wait(0);owner=run('owner',dll.owner);wait(1);join(user)
            receipt=results['started'];serial=receipt>>32;assert serial and receipt&255==1 and dll.receipt(serial)&255==1
            assert dll.replace()&255==6 and dll.state(2)==2 and dll.state(3)==1
            dll.release();join(owner);assert dll.receipt(serial)&255==2 and dll.state(2)==1 and dll.state(4)==1 and dll.state(6)==1 and dll.state(7)==1 and not dll.state(5)
            dll.owner();assert dll.state(3)==1;cases.append('STARTED setter beyond 500 ms returns IN_PROGRESS; same-serial poll/BUSY; exactly one eventual change/release')

            dll.prepare(now);read_session();dll.configure(0);user=run('cancel_started',dll.replace);wait(0);owner=run('owner2',dll.owner);wait(1);join(user)
            serial=results['cancel_started']>>32;dll.cancel();assert dll.state(2)==2 and dll.state(4)==0
            dll.release();join(owner);assert dll.receipt(serial)&255==7 and dll.state(2)==1 and dll.state(3)==1 and dll.state(4)==1 and dll.state(6)==1 and not dll.state(5)
            cases.append('Cancel during STARTED preserves reference until callback completes; one effect with unconfirmed/FAILED receipt, no replay')

            dll.prepare(now);read_session();receipt=dll.replace();assert receipt&255==5 and dll.state(3)==0;dll.owner();assert dll.state(3)==0 and dll.state(2)==2;dll.cancel();assert dll.state(2)==1
            cases.append('QUEUED replace expires at 500 ms; late owner has zero setters; owner cleanup releases session once')

            dll.prepare(now);assert dll.read()==0;dll.owner();assert dll.state(2)==1 and dll.state(4)==0 and dll.state(3)==0
            cases.append('QUEUED read expires at 500 ms; late owner has zero QI/reference or setter effects')

            dll.prepare(now);dll.configure(1);user=run('abandoned_read',dll.read);wait(0);owner=run('read_owner',dll.owner);wait(1);join(user);assert results['abandoned_read']==0 and dll.state(2)==2
            dll.release();join(owner);assert dll.state(2)==1 and dll.state(4)==1 and dll.state(3)==0 and not dll.state(5) and dll.state(7)==1
            cases.append('STARTED read getter beyond 500 ms is abandoned; callback completion releases retained QI exactly once')
        finally:
            dll.release()
            for t in workers:t.join(3)
            if all(not t.is_alive() for t in workers):
                ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.FreeLibrary(dll._handle)
    result={'pass':True,'real_host_thread_cases':cases,'production_sha256':hashlib.sha256((ROOT/'android/host/sims2_text.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(cfile.read_bytes()).hexdigest(),
            'limits':'Actual C text state machine with Windows real threads/clocks/atomic spin locks. Guest calls, native command queue delivery and3-register bank are boundary fixtures; full ARM64 ABI/bank/getter tests separate. No device acceptance.'}
    (OUT/'threads-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS native full-field real threads:',len(cases),'cases')


if __name__=='__main__':main()
