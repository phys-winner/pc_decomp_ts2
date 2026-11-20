"""Run actual input FIFO, user32 dispatch and touch-query C on ARM64."""
import io
import json
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/touch-barrier'
PRELUDE = r'''
#include "host.h"
#include "input.h"
#include "sims2_settings.h"
#include <stdatomic.h>
#include <time.h>
#include <math.h>
#define WM_SIZE 5u
#define WM_QUIT 0x12u
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias,void_verbose;
uint64_t g_mm[8];
recomp_ehframe_t *g_eh_frames;
uint32_t g_eh_obj,g_eh_info;
static uint32_t thread=1,context=S2_TOUCH_LIVE,input_context;
static pthread_mutex_t input_lock,pump_lock,user_lock;
static uint32_t lock_depth,waits,picker_calls,guest_calls,timer_calls,bad,mode,filter,nested,handler_active;
static uint32_t main_window=1;
static unsigned slider_state;
static pthread_cond_t pick_ready;
/* Camera math is outside this FIFO fixture; reaching it is a test failure. */
float tanf(float x){bad=209;return x;}
float expf(float x){bad=209;return x;}
float sinf(float x){bad=209;return x;}
float cosf(float x){bad=209;return x;}
float sqrtf(float x){bad=209;return x;}
float fminf(float x,float y){return x<y?x:y;}
float fmaxf(float x,float y){return x>y?x:y;}
void_fn void_dispatch_find(uint32_t va){(void)va;bad=209;return NULL;}
double fmax(double x,double y){return x>y?x:y;}
double fabs(double x){return x<0?-x:x;}
long lround(double x){return (long)(x<0?x-.5:x+.5);}
static void drive(void);
double s2_ui_factor(void){return 1;}
uint32_t s2_ui_pick(uint32_t root,int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(MEM32(MEM32(root)+0xBC),root,2,p);}
uint32_t s2_ui_mouse_pick(uint32_t root,int x,int y){return s2_ui_pick(root,x,y);}
void s2_cheats_owner(unsigned serial,uint32_t manager){(void)serial;(void)manager;}
void sub_A0B4C1B0_lifted(void){bad=209;}
void *memcpy(void *d,const void *s,size_t n){uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d;}
void *memset(void *d,int c,size_t n){uint8_t *o=d;while(n--)*o++=(uint8_t)c;return d;}
int memcmp(const void *a,const void *b,size_t n){const uint8_t *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;++x;++y;}return 0;}
void *memchr(const void *p,int c,size_t n){const uint8_t *s=p;while(n--){if(*s==(uint8_t)c)return(void *)s;++s;}return NULL;}
size_t strlen(const char *s){size_t n=0;while(s[n])++n;return n;}
int strcmp(const char *a,const char *b){while(*a && *a==*b){++a;++b;}return(uint8_t)*a-(uint8_t)*b;}
int strncmp(const char *a,const char *b,size_t n){while(n--){if(*a!=*b)return(uint8_t)*a-(uint8_t)*b;if(!*a)return 0;++a;++b;}return 0;}
char *strstr(const char *s,const char *n){for(;*s;++s){const char *a=s,*b=n;while(*a && *a==*b){++a;++b;}if(!*b)return(char *)s;}return NULL;}
int pthread_mutex_lock(pthread_mutex_t *p){++lock_depth;return 0;}
int pthread_mutex_unlock(pthread_mutex_t *p){--lock_depth;return 0;}
int pthread_cond_broadcast(pthread_cond_t *c){return 0;}
int clock_gettime(clockid_t c,struct timespec *t){t->tv_sec=10;t->tv_nsec=0;return 0;}
int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *p,const struct timespec *t){
    /* Queries may wait two slow frames; mutating slider commands retain their 80ms expiry. */
    long budget=(c==&pick_ready && slider_state!=1 && slider_state!=2)?500000000:80000000;
    ++waits;if(t->tv_sec!=10 || t->tv_nsec!=budget)bad=1;
    if(!mode)return 1;
    pthread_mutex_unlock(p);drive();pthread_mutex_lock(p);return 1;
}
void void_log(const char *s,...){ }
uint32_t void_tick_ms(void){return 100;}
uint32_t void_thread_id(void){return thread;}
void void_wait_broadcast(void){}
void void_input_cursor(uint32_t *x,uint32_t *y){*x=700;*y=500;}
uint32_t void_window_main(void){return main_window;}
unsigned s2_touch_context(void){return context|input_context;}
uint32_t s2_touch_modal_window(void){return 0;}
int s2_touch_rewards(void){return 0;}
void s2_touch_input_context(unsigned f){input_context=f;}
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x10000 && (uint64_t)a+n<=0x30000;}
int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}
void s2_ui_resized(void){}
int16_t void_input_key_state(uint32_t key){(void)key;return 0;}
void void_input_mouse_warp(int x,int y){(void)x;(void)y;}
void void_input_mouse_wheel(int delta){(void)delta;}
void void_input_screen_size(int *w,int *h){*w=1212;*h=540;}
uint32_t void_galloc(uint32_t size){(void)size;return 0;}
void void_gfree(uint32_t pointer){(void)pointer;}
int s2_ui_scale(void){return 200;}
int s2_ui_scale_apply(int scale){(void)scale;return 0;}
void s2_touch_cancel_text(void){}
void s2_touch_text_pump(uint32_t manager){(void)manager;}
void s2_touch_text_owner(unsigned kind,unsigned serial,uint32_t manager){(void)kind;(void)serial;(void)manager;}
int s2_touch_text_editable(uint32_t window){(void)window;return 0;}
int s2_catalog_info(uint32_t proc){(void)proc;return 0;}
void s2_catalog_track(uint32_t proc){(void)proc;}
uint32_t s2_catalog_clear_request(void){return 1;}
int s2_catalog_clear_owner(uint32_t serial){return serial==1;}
typedef struct {uint32_t hwnd,wndproc,thread;struct {uint32_t style;} *cls;uint32_t layout_size;int layout_size_valid;} window_t;
static window_t window={1,0x12340000,1,NULL};
static window_t *find_window(uint32_t hwnd){return hwnd==1?&window:NULL;}
static uint32_t dblclk_translate(window_t *w,uint32_t msg,uint32_t p){return msg;}
static void timer_delivered(uint32_t w,uint32_t id){++timer_calls;}
'''
CHECK = r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
static void typed(uint32_t o,uint32_t v,uint32_t l,uint32_t d,const char *name){
    MEM32(o)=v;MEM32(v-4)=l;MEM32(l+12)=d;memcpy(GPTR(d+8),name,strlen(name)+1);
}
static void one(void){
    qmsg_t message;
    pthread_mutex_lock(&user_lock);queue_t *q=queue_for(thread);
    int got=queue_take(q,0,filter==2?VOID_TOUCH_QUERY_MESSAGE:filter?0x200:0,filter==2?VOID_TOUCH_QUERY_MESSAGE:filter?0x20E:0,1,&message);
    pthread_mutex_unlock(&user_lock);if(!got)return;
    store_msg(0x24000,&message);
    void_regs before={0},after={0};void_regs_save(&before);
    uint32_t result=dispatch_message(0x24000);
    if(message.msg==VOID_TOUCH_QUERY_MESSAGE){
        void_regs_save(&after);if(memcmp(&before,&after,sizeof before))bad=2;
        if(result)bad=3;
    }else if(message.msg==0x202 && result!=0xCAFEBABE)bad=4;
}
static void drive(void){
    void_input_pump();
    for(unsigned i=0;i<1100;++i){
        queue_t *q=queue_for(thread);int before=q->count;one();if(q->count==before)break;
    }
}
uint32_t void_call_guest(uint32_t va,int n,const uint32_t *args,int pops){
    ++guest_calls;if(va!=0x12340000 || n!=4 || !pops || lock_depth)bad=5;
    if(args[1]==0x202 || args[1]==0x113){
        handler_active=1;
        if(nested){
            nested=0;
            if(mode==3)s2_touch_request_pick(700,500);
            if(mode==5){
                unsigned previous=void_begin_modal_messages();
                context=S2_TOUCH_MODAL|S2_TOUCH_LIVE;
                drive();
                if(picker_calls!=1 || touch_barrier_deferred)bad=16;
                void_end_modal_messages(previous);
            }else{
                drive();
                if(picker_calls || !touch_barrier_deferred || !message_depth)bad=6;
            }
        }
        MEM32(tool_handler+0x20)=0x16000;
        if(mode==4)context=S2_TOUCH_MODAL|S2_TOUCH_LIVE;
        handler_active=0;
    }
    return 0xCAFEBABE;
}
uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
    if(va==0xA067AD3Au){if(n||args)bad=210;return 80;}
    ++picker_calls;
    if(va!=0x13572468 || self!=0x12000 || n!=2 || args[0]!=700 || args[1]!=500 ||
        message_depth!=modal_message_depth || (handler_active && mode!=5) || lock_depth)bad=7;
    g_eax=1;g_ecx=2;g_edx=3;g_ebx=4;g_esp=5;g_ebp=6;g_esi=7;g_edi=8;
    g_fs_base=9;g_cur_func=10;g_flag_k=FK_TEST;g_flag_a=11;g_flag_b=12;g_flag_cf=1;
    g_mxcsr=0;g_fpu_cw=0x7F;void_fpu_cmp=3;g_fp_bias=0;
    for(unsigned i=0;i<8;++i){g_stp[i]=i+7;X87_SHADOW(g_stp)[i]=i+8;g_mm[i]=i+9;memset(&g_xmm[i],0xA5,sizeof g_xmm[i]);}
    g_eh_frames=NULL;g_eh_obj=g_eh_info=0;
    return 0x13000;
}
static void reset(void){
    memset(queues,0,sizeof queues);queue_count=0;pending_count=0;
    thread=window.thread=1;context=S2_TOUCH_LIVE;input_context=0;mode=1;filter=nested=handler_active=0;
    message_depth=touch_barrier_deferred=modal_message_depth=0;pick_pending=pick_done=pick_serial=0;
    waits=picker_calls=guest_calls=timer_calls=bad=0;
    memset(&g_m,0,sizeof g_m);g_stp=g_stbuf+X87_CENTRE;g_fp_bias=3;
    g_eax=0xAA;g_ecx=0xBB;g_edx=0xCC;g_ebx=0xDD;g_esp=0x28000;g_ebp=0x29000;g_esi=0xEE;g_edi=0xFF;
    g_fs_base=0x1234;g_cur_func=0xABCD;g_flag_k=FK_CMP;g_flag_a=123;g_flag_b=45;g_flag_cf=1;
    g_mxcsr=0x1F80;g_fpu_cw=0x27F;void_fpu_cmp=1;
    g_eh_frames=(void *)(uintptr_t)0x1111;g_eh_obj=222;g_eh_info=333;
    for(unsigned i=0;i<8;++i){g_stp[i]=i*.25;X87_SHADOW(g_stp)[i]=i+400;g_mm[i]=i+500;memset(&g_xmm[i],(int)i,sizeof g_xmm[i]);}
    win_manager=0x11000;win_manager_thread=1;tool_handler=0x14000;
    MEM32(win_manager+0x44)=0x12000;MEM32(0x12000)=0x12200;MEM32(0x122BC)=0x13572468;
    MEM32(tool_handler+0x20)=0x15000;
    typed(0x13000,0x13200,0x13400,0x13600,".?AVcTSWinSceneGraph@@");
    typed(0x15000,0x15200,0x15400,0x15600,".?AVcTSUserToolStandardAction@@");
    typed(0x16000,0x16200,0x16400,0x16600,".?AVcTSUserToolInventoryObject@@");
}
uint32_t cases;
uint32_t selftest(void){
    reset();pend(0x202,0,0);REQUIRE(s2_touch_pick(700,500)==2);
    REQUIRE(guest_calls==1 && picker_calls==1 && s2_touch_tool()==2 && !bad);++cases;
    reset();nested=1;pend(0x202,0,0);REQUIRE(s2_touch_pick(700,500)==2);
    REQUIRE(guest_calls==1 && picker_calls==1 && !touch_barrier_deferred && s2_touch_tool()==2 && !bad);++cases;
    reset();mode=0;REQUIRE(!s2_touch_pick(700,500) && !pick_pending);
    mode=1;drive();REQUIRE(!picker_calls && !guest_calls && !bad);++cases;
    reset();s2_touch_request_pick(1,2);unsigned old=pick_serial;s2_touch_request_pick(700,500);
    void_input_pump();one();REQUIRE(!picker_calls && pick_done!=old);one();
    REQUIRE(picker_calls==1 && pick_done==pick_serial && !bad);++cases;
    reset();for(unsigned i=0;i<512;++i)pend(0,0,0);
    REQUIRE(!s2_touch_pick(700,500) && !waits && pending_count==512 && !picker_calls && !bad);++cases;
    reset();for(unsigned i=0;i<1024;++i)queue_push(1,1,0,0,0);
    REQUIRE(!s2_touch_pick(700,500) && !picker_calls && !bad);++cases;
    reset();filter=1;pend(0x202,0,0);REQUIRE(!s2_touch_pick(700,500) && guest_calls==1 && !picker_calls);
    filter=0;drive();REQUIRE(!picker_calls && !bad);++cases;
    reset();filter=2;pend(0x202,0,0);REQUIRE(!s2_touch_pick(700,500) && !guest_calls && !picker_calls);
    filter=0;drive();REQUIRE(guest_calls==1 && !picker_calls && !bad);++cases;
    reset();pend(0x202,0,0);s2_touch_request_pick(700,500);void_input_pump();
    qmsg_t copy;REQUIRE(queue_take(queue_for(1),0,VOID_TOUCH_QUERY_MESSAGE,VOID_TOUCH_QUERY_MESSAGE,0,&copy));
    REQUIRE(!copy.lparam && queue_for(1)->count==2);store_msg(0x24000,&copy);dispatch_message(0x24000);
    REQUIRE(!picker_calls && !guest_calls);drive();REQUIRE(picker_calls==1 && guest_calls==1 && !bad);++cases;
    reset();thread=2;REQUIRE(!s2_touch_pick(700,500) && !picker_calls);
    thread=1;drive();REQUIRE(!picker_calls && !bad);++cases;
    reset();mode=3;nested=1;REQUIRE(send_message(1,0x202,0,0)==0xCAFEBABE);
    REQUIRE(picker_calls==1 && guest_calls==1 && !message_depth && !bad);++cases;
    reset();mode=3;nested=1;
    MEM32(0x24000)=1;MEM32(0x24004)=0x113;MEM32(0x24008)=7;MEM32(0x2400C)=0x12340000;
    REQUIRE(!dispatch_message(0x24000));REQUIRE(timer_calls==1 && picker_calls==1 && !bad);++cases;
    reset();mode=4;pend(0x202,0,0);REQUIRE(!s2_touch_pick(700,500) && picker_calls==1 && !bad);++cases;
    reset();mode=5;nested=1;typed(0x13000,0x13100,0x13200,0x13300,".?AVcGZWinBtn@@");
    pend(0x202,0,0);REQUIRE(s2_touch_pick(700,500)==1 && picker_calls==1 && !modal_message_depth && !bad);++cases;
    reset();context=0;REQUIRE(!s2_touch_pick(700,500) && picker_calls==1 && !bad);++cases;
    reset();context=0;typed(0x13000,0x13100,0x13200,0x13300,".?AVcGZWinBtn@@");
    REQUIRE(s2_touch_pick(700,500)==1 && picker_calls==1 && !bad);++cases;
    reset();context=0;typed(0x13000,0x13100,0x13200,0x13300,".?AVcGZWinBtn@@");
    typed(0x18000,0x18100,0x18200,0x18300,".?AVcTSWinSceneGraph@@");MEM32(0x13058)=0x18000;
    REQUIRE(!s2_touch_pick(700,500) && picker_calls==1 && !bad);++cases;
    reset();typed(0x15000,0x15200,0x15400,0x15600,".?AVcTSUserToolWall@@");
    typed(0x16000,0x16200,0x16400,0x16600,".?AVcTSUserToolFloorPattern@@");s2_touch_pump();
    uint64_t old_tool=s2_touch_tool_token();REQUIRE((uint32_t)old_tool==3);
    pend(0x202,0,0);REQUIRE(s2_touch_pick(700,500)==2);
    REQUIRE((uint32_t)s2_touch_tool_token()==3 && s2_touch_tool_token()!=old_tool && !bad);++cases;
    return 0;
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    paths = {n: (B.ROOT / ('android/host/' + n + '.c')).read_text(encoding='utf-8')
             for n in ('input', 'user32', 'sims2_touch', 'core')}
    u, i, touch, core = (paths[n] for n in ('user32', 'input', 'sims2_touch', 'core'))
    state = u[u.index('typedef struct { uint32_t hwnd, msg, wparam, lparam, time, x, y; } qmsg_t;'):
              u.index('static void timers_post_due(void);')]
    state += i[i.index('typedef struct { uint32_t msg, wparam, lparam; } pending_t;'):
               i.index('void void_input_screen_size(')]
    state += 'static __thread unsigned message_depth, touch_barrier_deferred, modal_message_depth;\n'
    actual = ''.join(function(core, s) for s in ('void void_regs_save(', 'void void_regs_load('))
    actual += '\n'.join(line for line in touch.splitlines() if not line.startswith('#include'))
    actual += ''.join(function(u, s) for s in ('static queue_t *queue_for(', 'static void queue_push(',
        'void void_post_message(', 'static int matches(', 'static int queue_take(', 'static void store_msg(',
        'unsigned void_begin_modal_messages(', 'void void_end_modal_messages(',
        'static void message_returned(', 'static uint32_t send_message(', 'static uint32_t dispatch_message('))
    actual += ''.join(function(i, s) for s in ('static void pend(', 'int void_input_touch_barrier(', 'void void_input_pump('))
    # Bodies appear later than callbacks in the original files; retain declarations.
    forward = 'static void queue_push(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n'
    forward += 'static void message_returned(void);\nstatic uint32_t send_message(uint32_t,uint32_t,uint32_t,uint32_t);\n'
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(PRELUDE + 'uint64_t void_monotonic_ns(void){return 1000000000ull;}\n' + state + forward + actual + CHECK)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
        '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    uc.mem_map(base, 0x1000000); uc.mem_map(guest + 0x10000, 0x20000)
    uc.mem_map(stack, 0x100000); uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD': uc.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    if relocations:
        for r in relocations.iter_relocations():
            assert r['r_info_type'] == 1027
            uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, stack + 0x100); uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xFF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=10000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
    assert uc.reg_read(A.UC_ARM64_REG_X0) == 0, ('C line', uc.reg_read(A.UC_ARM64_REG_X0))
    count = int.from_bytes(uc.mem_read(base + symbols['cases'], 4), 'little')
    (OUT / 'results.json').write_text(json.dumps(dict(pass_=True, cases=count, actual='input FIFO, user32 dispatch, touch serial/context/tool and full core register save/restore',
        limits='condition waits and guest window/picker calls are boundary mocks; no device actions'), indent=2) + '\n')
    print('PASS actual ARM64 touch barrier:', count, 'cases: ordered catalog UP, nested/slow callback, timeout/stale, both queue saturations, filtering, owner, SendMessage/timer depth, modal/unknown, Wall/Floor identity and full ABI register bank')


if __name__ == '__main__': main()
