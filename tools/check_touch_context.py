"""Run the actual native touch lifecycle and read-only pick adapter on the host."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import pefile

from build_android import CLANG, ROOT, NATIVE_OVERRIDES
from check_virtual_query import function


def main():
    settings = (ROOT / 'android/host/sims2_settings.c').read_text()
    touch = (ROOT / 'android/host/sims2_touch.c').read_text()
    android = (ROOT / 'android/host/android_main.c').read_text()
    pairs = [('A0619442', 'A061A29D', 512), ('A06194E1', 'A061A2CB', 256),
             ('A0619A89', 'A061A67F', 128), ('A0619C09', 'A061A796', 64),
             ('A0619E26', 'A061A919', 2), ('A0619F86', 'A061A9F2', 256)]
    modes = [address for enter, leave, _ in pairs for address in (enter, leave)]
    assert all('sub_' + address in NATIVE_OVERRIDES for address in modes + ['A0973692', 'A0973E55', 'A05BDFD7', 'A05BE0F2'])
    pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    assert pe.get_data(0xA10A9960-base, len(b'.?AVcTSUIModeReward@@\0')) == b'.?AVcTSUIModeReward@@\0'
    assert pe.get_data(0xA0619FA4-base, 6) == b'\xc7\x07\xb4\x1f\xde\xa0'
    assert pe.get_data(0xA061A9F5-base, 6) == b'\xc7\x06\xb4\x1f\xde\xa0'
    prelude = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef short int16_t;
#define UINT32_MAX 0xFFFFFFFFu
#define NULL ((void *)0)
typedef unsigned long long size_t;
typedef unsigned long long uint64_t;
int _fltused;
static void *memchr(const void *s,int c,size_t n) { const uint8_t *p=s;while(n--){if(*p==(uint8_t)c)return(void *)p;++p;}return NULL; }
static int strcmp(const char *a,const char *b) { while(*a && *a==*b){++a;++b;}return (uint8_t)*a-(uint8_t)*b; }
static const char *strstr(const char *s,const char *n) { for(;*s;++s){const char *a=s,*b=n;while(*a && *a==*b){++a;++b;}if(!*b)return s;}return NULL; }
static size_t strlen(const char *s) { size_t n=0;while(s[n])++n;return n; }
static int strncmp(const char *a,const char *b,size_t n) { while(n--){if(*a!=*b)return(uint8_t)*a-(uint8_t)*b;if(!*a)return 0;++a;++b;}return 0; }
void *memcpy(void *d,const void *s,size_t n) { uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d; }
void *memset(void *d,int c,size_t n) { uint8_t *o=d;while(n--)*o++=(uint8_t)c;return d; }
static float tanf(float x) { return x; }
static float expf(float x) { return 1+x; }
static float sinf(float x) { return x; }
static float cosf(float x) { (void)x;return 1; }
static float sqrtf(float x) { (void)x;return 1; }
static double fmax(double x,double y) { return x>y?x:y; }
static double fabs(double x) { return x<0?-x:x; }
static long lround(double x) { return (long)(x<0?x-.5:x+.5); }
#define atomic_load(p) __c11_atomic_load(p,5)
#define atomic_store(p,v) __c11_atomic_store(p,v,5)
#define atomic_fetch_add(p,v) __c11_atomic_fetch_add(p,v,5)
#define atomic_exchange(p,v) __c11_atomic_exchange(p,v,5)
#define isfinite(v) __builtin_isfinite(v)
#define INFINITY __builtin_inff()
#define VOID_HOST_H
#include "sims2_settings.h"
typedef unsigned pthread_mutex_t;
typedef unsigned pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
struct timespec { long long tv_sec,tv_nsec; };
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
static int clock_gettime(int c,struct timespec *t) { (void)c;t->tv_sec=1;t->tv_nsec=990000000;return 0; }
static unsigned lock_depth;
static void pthread_mutex_lock(pthread_mutex_t *p) { (void)p; ++lock_depth; }
static void pthread_mutex_unlock(pthread_mutex_t *p) { (void)p; --lock_depth; }
static int simulate_pump;
static unsigned fixture_barrier;
static int void_input_touch_barrier(unsigned serial) { fixture_barrier=serial;return 1; }
static void tick(void);
static int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *p,const struct timespec *t) {
    (void)c;(void)t;
    if(!simulate_pump)return 1;
    pthread_mutex_unlock(p);tick();pthread_mutex_lock(p);return 0;
}
static void pthread_cond_broadcast(pthread_cond_t *c) { (void)c; }
static uint32_t g_eax,g_ecx,g_esp;
static uint32_t thread_id=1;
static uint32_t void_thread_id(void) { return thread_id; }
typedef struct { uint32_t eax,ecx,esp; } void_regs;
static uint32_t words[8192];
#define GPTR(a) ((uint8_t *)words+(uint32_t)(a))
#define MEM32(a) words[(uint32_t)(a)/4]
#define MEM8(a) (*(uint8_t *)GPTR(a))
static unsigned touch_modal_depth,touch_mode_flags,ui_pie_depth;
static uint32_t touch_modal_window;
static unsigned touch_input_flags;
static uint32_t touch_mode_owner;
static _Atomic unsigned touch_context;
static _Atomic int touch_rewards;
static uint32_t ui_fit_option,ui_fit_cas,ui_fit_chooser,ui_fit_loading,ui_fit_lot_info;
static uint32_t ui_fit_town_loading,ui_fit_lot_loading,ui_fit_loading_screen;
static unsigned fit_depth,nest_modal,modal_calls;
static int ui_modal_floor_right,ui_modal_floor_bottom;
static int readable=1,logs,calls,bad;
static double fp_pop(void) { bad=1;return 0; }
static void void_log(const char *f,...) { (void)f; ++logs; }
static int void_vm_readable(uint32_t p,uint32_t n) { return readable && p+n<=sizeof words; }
static int void_vm_writable(uint32_t p,uint32_t n) { return void_vm_readable(p,n); }
/* The benchmark camera log (camget) holds the machine; the fixture runs on one thread. */
static void void_machine_enter(void) {}
static void void_machine_leave(void) {}
static void void_regs_save(void_regs *r) { *r=(void_regs){g_eax,g_ecx,g_esp}; }
static void void_regs_load(const void_regs *r) { g_eax=r->eax;g_ecx=r->ecx;g_esp=r->esp; }
static uint32_t void_call_guest(uint32_t va,int nargs,const uint32_t *args,int pops){(void)va;(void)nargs;(void)args;(void)pops;return 0;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int nargs,const uint32_t *args) {
    ++calls;
    if(va==0xA067AD3Au)return 80;
    if(lock_depth || va!=0x12345678 || self!=0x200 || nargs!=2 || args[0]!=700 || args[1]!=500)bad=1;
    g_eax=9;g_ecx=10;g_esp=11;
    return 0x300;
}
static void ui_fit(int enter,int full_height) { (void)full_height;if(enter)++fit_depth;else --fit_depth; }
#define ui_fit_depth fit_depth
static int ui_fit_linger;
static const char *fit_owner_name(const uint32_t *owner) { (void)owner; return ""; }
int16_t void_input_key_state(uint32_t key){(void)key;return 0;}
static void void_input_mouse_warp(int x,int y) { (void)x;(void)y;bad=1; }
static void void_input_mouse_wheel(int delta) { (void)delta;bad=1; }
static uint32_t void_galloc(uint32_t size){(void)size;return 0;}
static void void_gfree(uint32_t pointer){(void)pointer;}
static int screen_w,screen_h,surface_w,surface_h;
static void void_input_screen_size(int *w,int *h) { *w=screen_w;*h=screen_h; }
void s2_game_resolution(int *w,int *h) { *w=screen_w;*h=screen_h; }
static void s2_ui_scale_requested(int scale){(void)scale;}
static double ui_applied_factor=1;
double s2_ui_factor(void) { return ui_applied_factor; }
static _Atomic int ui_scale=200;
int s2_ui_scale(void){return 200;}
int s2_ui_scale_apply(int scale){(void)scale;return 0;}
void s2_touch_cancel_text(void){}
void s2_cheats_owner(unsigned value,unsigned manager){(void)value;(void)manager;}
void sub_A0B4C1B0_lifted(void){}
void s2_touch_text_pump(uint32_t manager){(void)manager;}
void s2_touch_text_owner(unsigned kind,unsigned serial,uint32_t manager){(void)kind;(void)serial;(void)manager;}
int s2_touch_text_editable(uint32_t window){(void)window;return 0;}
int s2_catalog_info(uint32_t proc){(void)proc;return 0;}
void s2_catalog_track(uint32_t proc){(void)proc;}
uint32_t s2_catalog_clear_request(void){return 1;}
int s2_catalog_clear_owner(uint32_t serial){return serial==1;}
static void sub_A0972F48(void);
static unsigned modal_messages;
static unsigned void_begin_modal_messages(void){return modal_messages++;}
static void void_end_modal_messages(unsigned previous){if(modal_messages!=previous+1)bad=11;modal_messages=previous;}
static void sub_A0972F48_lifted(void) {
    ++modal_calls;
    if(!(s2_touch_context() & S2_TOUCH_MODAL) || modal_messages!=touch_modal_depth || touch_modal_window!=MEM32(g_esp+4) ||
        (ui_pie_depth ? fit_depth!=0 : fit_depth==0))bad=2;
    if(nest_modal){nest_modal=0;sub_A0972F48();}
    g_eax=0x1234;
}
static void sub_A0973692_lifted(void) { g_eax=1;g_esp+=4; }
static void sub_A0973E55_lifted(void) { g_eax=0;g_esp+=4; }
static void sub_A05BDFD7_lifted(void) { g_eax=1;g_esp+=12; }
static void s2_fit_scene_attached(void){}
static void sub_A05BE0F2_lifted(void) { g_eax=1;g_esp+=12; }
static void sub_A076C5EB_lifted(void) { g_eax=1;g_esp+=8; }
static void sub_A076CABE_lifted(void) { g_eax=1;g_esp+=4; }
static void sub_A03582A0_lifted(void) { g_eax=g_ecx;g_esp+=4; }
static void sub_A035A1B0_lifted(void) { g_eax=g_ecx;g_esp+=4; }
static void sub_A035AB10_lifted(void) { g_eax=g_ecx;g_esp+=4; }
static void sub_A0B46E90_lifted(void) { g_eax=1;g_esp+=8; }
static void sub_A05E01E5_lifted(void) { g_eax=1;g_esp+=4; }
#define JNIEXPORT __declspec(dllexport)
#define JNICALL
typedef long long jlong;
typedef void JNIEnv;
typedef void *jclass;
static uint32_t cursor_x,cursor_y;
static void *window=(void *)1;
static pthread_mutex_t window_lock;
static void void_input_cursor(uint32_t *x,uint32_t *y) { *x=cursor_x;*y=cursor_y; }
static int ANativeWindow_getWidth(void *p) { (void)p;return surface_w; }
static int ANativeWindow_getHeight(void *p) { (void)p;return surface_h; }
static float fminf(float a,float b) { return a<b?a:b; }
static float fmaxf(float a,float b) { return a>b?a:b; }
static void (*void_dispatch_find(uint32_t va))(void) { (void)va;return NULL; }
static long lroundf(float value) { return (long)(value+.5f); }
'''
    prelude = prelude.replace('#define MEM32(a) words[(uint32_t)(a)/4]', '''
static uint32_t unreadable_tool;
static unsigned unreadable_tool_reads;
static uint32_t *checked_word(uint32_t address) {
    if (address == unreadable_tool && unreadable_tool) ++unreadable_tool_reads;
    return &words[address / 4];
}
#define MEM32(a) (*checked_word((uint32_t)(a)))''')
    prelude = prelude.replace('return readable && p+n<=sizeof words;',
                              'return readable && p!=unreadable_tool && p+n<=sizeof words;')
    stubs = ''.join(f'void sub_{address}_lifted(void) {{ g_eax=g_ecx;g_esp+=4; }}\n' for address in modes)
    stubs += 'void sub_A06815D7_lifted(void){}void sub_A067D8A4_lifted(void){}void sub_A067D1BD_lifted(void){}\n'
    actual = (function(settings, 'static void touch_publish(') + function(settings, 'void s2_touch_input_context(') + function(settings, 'static void touch_mode(') +
              'unsigned s2_touch_context(void) { return atomic_load(&touch_context); }\n' + next(line for line in settings.splitlines() if line.startswith('int s2_touch_rewards(')) + '\n' +
              next(line for line in settings.splitlines() if line.startswith('uint32_t s2_touch_modal_window(')) + '\n' +
              '\n'.join(line for line in settings.splitlines() if any(line.startswith('void sub_' + a + '(void)') for a in modes)) + '\n' +
              function(settings, 'static void ui_owner_fit(uint32_t *owner, uint32_t self, int open)\n{') +
              function(settings, 'static int ui_modal_gen(') + function(settings, 'static int ui_story_window(') + function(settings, 'static int ui_modal_fits(') +
              function(settings, 'void sub_A0972F48(void)') +
              function(settings, 'uint32_t s2_ui_pick(') +
              '\n'.join(line for line in touch.splitlines() if not line.startswith('#include')) +
              function(android, 'JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchToolToken(') +
              function(android, 'JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchCursor(') +
              '\nstatic void tick(void) { s2_touch_pump(); if(fixture_barrier && win_manager && thread_id==win_manager_thread) '
              '{unsigned serial=fixture_barrier;fixture_barrier=0;s2_touch_barrier(serial);} }\n')
    checks = r'''
__declspec(dllexport) unsigned context(void) { return s2_touch_context(); }
__declspec(dllexport) int reused_live(void) {
    touch_mode_flags=touch_mode_owner=0;s2_touch_input_context(S2_TOUCH_LIVE);
    if(s2_touch_context()!=S2_TOUCH_LIVE)return 1;
    touch_mode(0x1200,S2_TOUCH_BUY);if(s2_touch_context()!=S2_TOUCH_BUY)return 2;
    touch_mode(0x1200,0);if(s2_touch_context()!=S2_TOUCH_LIVE)return 3;
    MEM32(0x1200)=0xA0DE1FB4u;touch_mode(0x1200,S2_TOUCH_BUY);if(!s2_touch_rewards())return 8;
    touch_mode(0x1300,0);if(!s2_touch_rewards())return 9;
    touch_mode(0x1200,0);if(s2_touch_rewards())return 10;
    MEM32(0x1200)=0;
    s2_touch_input_context(S2_TOUCH_LIVE|S2_TOUCH_FOCUSED_TEXT);
    if(s2_touch_context()!=(S2_TOUCH_LIVE|S2_TOUCH_FOCUSED_TEXT))return 4;
    touch_mode(0x1300,S2_TOUCH_BUILD);if(s2_touch_context()!=(S2_TOUCH_BUILD|S2_TOUCH_FOCUSED_TEXT))return 5;
    touch_mode(0x1300,0);s2_touch_input_context(0);
    for(unsigned i=0;i<2;++i){
        unsigned mode=i?S2_TOUCH_BUILD:S2_TOUCH_BUY;
        s2_touch_input_context(mode);touch_mode(0x1400,S2_TOUCH_OPTIONS);
        if(s2_touch_context()!=S2_TOUCH_OPTIONS)return 6;
        touch_mode(0x1400,0);if(s2_touch_context()!=mode)return 7;
        s2_touch_input_context(0);
    }
    return s2_touch_context()!=0;
}
__declspec(dllexport) int mode(unsigned enter,unsigned self) {
    g_ecx=self;g_esp=0x100;g_eax=0;
    switch(enter) {
'''
    for number, address in enumerate(modes):
        checks += f'case {number}: sub_{address}(); break;\n'
    checks += r'''
    }
    return g_eax==self && g_esp==0x104;
}
__declspec(dllexport) int owners(void) {
    uint32_t *owner[5]={&ui_fit_option,&ui_fit_cas,&ui_fit_chooser,&ui_fit_loading,&ui_fit_lot_info};
    unsigned flags[5]={2,4,8,16,32};
    for(unsigned i=0;i<5;++i) {
        ui_owner_fit(owner[i],100+i,1);
        if(s2_touch_context()!=flags[i] || fit_depth!=1)return 1;
        ui_owner_fit(owner[i],200+i,1);
        ui_owner_fit(owner[i],100+i,0);
        if(s2_touch_context()!=flags[i] || fit_depth!=1)return 2;
        nest_modal=1;sub_A0972F48();
        if(s2_touch_context()!=flags[i] || fit_depth!=1 || touch_modal_depth)return 3;
        ui_owner_fit(owner[i],200+i,0);
        ui_owner_fit(owner[i],200+i,0);
        if(s2_touch_context() || fit_depth)return 4;
    }
    g_esp=0x800;MEM32(g_esp+4)=0x900;MEM32(0x900)=0xA0DDA24Cu;
    ui_fit_linger=2;
    sub_A0972F48();
    if(ui_pie_depth || touch_modal_depth || s2_touch_context() || fit_depth || ui_fit_linger!=2)return 5;
    ui_fit_linger=0;MEM32(g_esp+4)=0;
    return bad || modal_calls!=11;
}
__declspec(dllexport) int pick(void) {
    logs=0;  /* owners() logged its [fit] transitions */
    s2_touch_pump(); if(calls || logs)return 1;
    s2_touch_request_pick(700,500);s2_touch_pump();
    if(calls || logs)return 2;
    int logged=logs;
    g_ecx=0x104;g_esp=0x800;sub_A0973692();
    touch_mode(0x70,S2_TOUCH_TOWN);
    MEM32(0x144)=0x200;MEM32(0x200)=0x400;MEM32(0x4BC)=0x12345678;
    MEM32(0x358)=0x200;
    g_eax=0xAB;g_ecx=0xCD;g_esp=0xEF;
    s2_touch_request_pick(700,500);thread_id=2;s2_touch_pump();
    if(calls || !pick_pending)return 14;
    thread_id=1;
    s2_touch_request_pick(1,2);s2_touch_request_pick(700,500);s2_touch_pump();
    if(calls!=1 || logs<=logged || bad || lock_depth)return 3;
    logged=logs;
    if(g_eax!=0xAB || g_ecx!=0xCD || g_esp!=0xEF)return 4;
    s2_touch_pump();if(calls!=1)return 5;
    simulate_pump=1;
    if(s2_touch_pick(700,500)!=1 || calls!=2 || logs!=logged)return 8;
    g_ecx=0x80;g_esp=0x800;MEM32(0x804)=0x300;sub_A05BDFD7();
    logged=logs;
    if(s2_touch_pick(700,500)!=2 || calls!=3 || logs!=logged)return 9;
    g_ecx=0x70;sub_A05BE0F2();
    if(s2_touch_pick(700,500)!=2 || calls!=4)return 10;
    g_ecx=0x80;sub_A05BE0F2();
    if(s2_touch_pick(700,500)!=1 || calls!=5)return 11;
    simulate_pump=0;
    if(s2_touch_pick(700,500) || calls!=5)return 12;
    s2_touch_pump();if(calls!=5)return 13;
    readable=0;s2_touch_request_pick(700,500);s2_touch_pump();
    if(calls!=5 || logs<=logged)return 6;
    logged=logs;
    readable=1;g_ecx=0x104;sub_A0973E55();
    s2_touch_request_pick(700,500);s2_touch_pump();
    if(calls!=5 || logs!=logged || win_manager)return 7;
    pick_pending=fixture_barrier=0;
    return 0;
}
static void typed(uint32_t object,uint32_t vtable,uint32_t locator,uint32_t descriptor,const char *name) {
    MEM32(object)=vtable;MEM32(vtable-4)=locator;MEM32(locator+12)=descriptor;
    uint8_t *out=GPTR(descriptor+8);unsigned i=0;do{out[i]=(uint8_t)name[i];}while(name[i++]);
}
__declspec(dllexport) int widgets(void) {
    static const char *const names[]={".?AVcGZWinSlider@@",".?AVcGZWinScrollbar2@@",".?AVcGZWinSpinner@@",
        ".?AVcGZWinGrid@@",".?AVcGZWinListBox@@",".?AVcGZWinTreeViewDrag@@",
        ".?AVcGZWinTextEdit@@",".?AVcGZWinLineInput@@",".?AVcGZWinBtn@@"};
    /* Synthetic RTTI alone no longer grants editable TEXT16; the native text fixture checks real classes/flags. */
    unsigned expected[]={4,4,4,8,8,8,0,0,0};
    MEM32(0x300+0x58)=0;
    for(unsigned i=0;i<9;++i) {
        typed(0x300,0x1000,0x1100,0x1200,names[i]);
        if(touch_widget(0x300)!=expected[i] || touch_text(0x300)!=(i==6 || i==7))return i+1;
    }
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSWinProcCatalog@@");
    MEM32(0x300+0xA0)=0x1500;MEM32(0x1500)=0x1510;MEM32(0x1510)=0x1500;MEM32(0x1518)=0x1600;
    if(touch_widget(0x300)!=8)return 10;
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSWinProcGrid2@@");
    MEM32(0x1768)=0xA07479F0u;
    if(touch_widget(0x300)!=(8|32))return 24;
    MEM32(0x1768)=0;
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSWinProcWantThumbnail@@");
    if(touch_widget(0x300)!=0)return 16;
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSWinProcSimThumbnail@@");
    if(touch_widget(0x300)!=0)return 17;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcGZWinTextEdit@@");
    MEM32(0x460)=8;if(touch_widget(0x300)!=8)return 25;
    MEM32(0x460)=9;if(touch_widget(0x300)!=8)return 26;
    MEM32(0x460)=0x99CB;if(touch_widget(0x300)!=8)return 28;
    MEM32(0x460)=1;if(touch_widget(0x300)!=0)return 29;
    MEM32(0x460)=0;if(touch_widget(0x300)!=0)return 27;
    win_manager=0x100;MEM32(0x138)=0x300;s2_touch_pump();
    if(!(s2_touch_context() & S2_TOUCH_FOCUSED_TEXT))return 11;
    MEM32(0x138)=0;s2_touch_pump();
    if(s2_touch_context() & S2_TOUCH_FOCUSED_TEXT)return 12;
    simulate_pump=1;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcTSWinSceneGraph@@");
    if(s2_touch_pick(700,500)!=2)return 13;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcGZWinBtn@@");
    typed(0x1A00,0x1B00,0x1C00,0x1D00,".?AVcTSWinSceneGraph@@");MEM32(0x358)=0x1A00;
    if((s2_touch_pick(700,500)&3)!=1)return 14;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcTSWinSceneGraphOverlay@@");
    if((s2_touch_pick(700,500)&3)!=1)return 15;
    MEM32(0x358)=0;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcGZWinFlatRect@@");MEM32(0x310)=0x4D097B6D;
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSUISimEditor@@");
    typed(0x1A00,0x1B00,0x1C00,0x1D00,".?AVcGZWinTextEdit@@");MEM32(0x138)=0x1A00;
    ui_owner_fit(&ui_fit_cas,0x55,1);
    if(s2_touch_pick(700,500)!=2 || !(s2_touch_context()&S2_TOUCH_FOCUSED_TEXT))return 18;
    MEM32(0x310)=0x4D097B6E;if((s2_touch_pick(700,500)&3)!=1)return 19;
    MEM32(0x310)=0x4D097B6D;typed(0x300,0x1000,0x1100,0x1200,".?AVcGZWinBtn@@");
    if((s2_touch_pick(700,500)&3)!=1)return 20;
    typed(0x300,0x1000,0x1100,0x1200,".?AVcGZWinFlatRect@@");
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSWinProcCatalog@@");
    if((s2_touch_pick(700,500)&3)!=1)return 21;
    typed(0x1600,0x1700,0x1800,0x1900,".?AVcTSUISimEditor@@");
    MEM32(0x3A0)=0;MEM32(0x358)=0x1A00;MEM32(0x1AA0)=0x1500;MEM32(0x1A10)=0x4D097B6D;
    typed(0x1A00,0x1B00,0x1C00,0x1D00,".?AVcGZWinFlatRect@@");
    if((s2_touch_pick(700,500)&3)!=1)return 22;
    MEM32(0x3A0)=0x1500;MEM32(0x358)=0;MEM32(0x138)=0;
    ui_owner_fit(&ui_fit_cas,0x55,0);
    if((s2_touch_pick(700,500)&3)!=1)return 23;
    MEM32(0x358)=0;simulate_pump=0;
    win_manager=0;
    return 0;
}
__declspec(dllexport) int tools(void) {
    const char *types[]={".?AVcTSUserToolStandardAction@@",".?AVcTSUserToolObject@@",
        ".?AVcTSUserToolWall@@",".?AVcTSUserToolTerrainPaint@@",".?AVcTSUserToolSledgeHammer@@",
        ".?AVcTSUserToolObjectDesign@@",".?AVcTSUserToolEyedropper@@",
        ".?AVcTSUserToolObjectDesignExtra@@",".?AVcTSUserToolEyedropperExtra@@",
        ".?AVcTSUserToolNHoodEyedropper@@",".?AVcTSUserToolUnknown@@","",
        ".?AVcTSUserToolFireplace@@",".?AVcTSUserToolElevator@@",".?AVcTSUserToolAutoRoof@@"};
    int expected[]={1,0,3,4,5,6,6,0,0,0,0,0,3,3,3}; /* Object needs the actual retained wrapper, tested separately. */
    win_manager=0x100;win_manager_thread=thread_id=1;
    g_ecx=0x2000;g_esp=0x800;sub_A076C5EB();MEM32(0x2020)=0x2100;
    for(unsigned i=0;i<sizeof expected/sizeof expected[0];++i) {
        typed(0x2100,0x2200,0x2300,0x2400,types[i]);s2_touch_pump();
        if(s2_touch_tool()!=expected[i])return i+1;
    }
    typed(0x2100,0x2200,0x2300,0x2400,types[1]);s2_touch_pump();
    g_ecx=0x2001;sub_A076CABE();if(s2_touch_tool()!=0)return 10;
    g_ecx=0x2000;sub_A076CABE();if(s2_touch_tool()!=0)return 11;
    s2_touch_pump();if(s2_touch_tool()!=0)return 12;
    const char *near[]={".?AVcTSUserToolObjectDesign@",".?AVcTSUserToolEyedropper@",
        ".?AVcTSUserToolObjectDesign@@Extra",".?AVcTSUserToolEyedropper@@Extra",
        ".?AVcTSUserToolUndoableObjectDesign@@",".?AVcTSWinProcEyedropper@@"};
    for(unsigned i=0;i<sizeof near/sizeof near[0];++i)
        if(touch_tool_kind(near[i]))return 20+i;
    return 0;
}
__declspec(dllexport) int tool_tokens(void) {
    win_manager=0x100;win_manager_thread=thread_id=1;
    g_ecx=0x2000;sub_A076C5EB();MEM32(0x2020)=0x2100;
    typed(0x2100,0x2200,0x2300,0x2400,".?AVcTSUserToolWall@@");s2_touch_pump();
    uint64_t wall=s2_touch_tool_token();if(!wall || (uint32_t)wall!=3)return 1;
    s2_touch_pump();if(s2_touch_tool_token()!=wall)return 2;
    typed(0x2500,0x2600,0x2700,0x2800,".?AVcTSUserToolFloorPattern@@");MEM32(0x2020)=0x2500;
    thread_id=2;s2_touch_pump();if(s2_touch_tool_token()!=wall)return 3;
    thread_id=1;s2_touch_pump();uint64_t floor=s2_touch_tool_token();
    if(floor==wall || (uint32_t)floor!=3)return 4;
    MEM32(0x2020)=0x2100;s2_touch_pump();if(s2_touch_tool_token()==wall)return 5;
    uint64_t before=s2_touch_tool_token();MEM32(0x2100)=0x2600;s2_touch_pump();
    if(s2_touch_tool_token()==before || (uint32_t)s2_touch_tool_token()!=3)return 6;
    typed(0x2100,0x2200,0x2300,0x2400,".?AVcTSUserToolAutoRoofExtra@@");s2_touch_pump();
    if(!s2_touch_tool_token() || s2_touch_tool())return 7;
    typed(0x2100,0x2200,0x2300,0x2400,".?AVcGZWinBtn@@");s2_touch_pump();
    if(s2_touch_tool_token())return 8;
    typed(0x2100,0x2200,0x2300,0x2400,".?AVcTSUserToolWall@@");s2_touch_pump();before=s2_touch_tool_token();
    readable=0;s2_touch_pump();if(s2_touch_tool_token())return 9;
    readable=1;s2_touch_pump();if(s2_touch_tool_token()==before)return 10;
    before=s2_touch_tool_token();g_ecx=0x2000;sub_A076CABE();
    if(s2_touch_tool_token())return 11;
    sub_A076C5EB();s2_touch_pump();if(s2_touch_tool_token()==before)return 12;
    tool_generation=0xFFFFFFFFu;MEM32(0x2020)=0x2500;s2_touch_pump();
    if((s2_touch_tool_token()>>32)!=1)return 13;
    if((uint64_t)Java_org_sims2_port_GameActivity_nativeTouchToolToken(NULL,NULL)!=s2_touch_tool_token())return 14;
    typed(0x2100,0x2200,0x2300,0x2400,".?AVcTSUserToolEyedropper@@");MEM32(0x2020)=0x2100;s2_touch_pump();
    uint64_t click=s2_touch_tool_token();if(!click||(uint32_t)click!=6)return 17;
    s2_touch_pump();if(s2_touch_tool_token()!=click)return 18;
    typed(0x2500,0x2600,0x2700,0x2800,".?AVcTSUserToolObjectDesign@@");MEM32(0x2020)=0x2500;
    thread_id=2;s2_touch_pump();if(s2_touch_tool_token()!=click)return 19;
    thread_id=1;s2_touch_pump();if((uint32_t)s2_touch_tool_token()!=6||s2_touch_tool_token()==click)return 20;
    g_ecx=0x104;sub_A0973E55();if(s2_touch_tool_token())return 15;
    s2_touch_pump();if(s2_touch_tool_token())return 16;
    return 0;
}
__declspec(dllexport) int unreadable_tool_recovery(void) {
    win_manager=0x100;win_manager_thread=thread_id=1;tool_handler=0x2000;
    MEM32(tool_handler+0x20)=0x2100;
    for(unsigned camera=0;camera<2;++camera) {
        lot_camera=camera?0x3000:0;unreadable_tool=0x2100;unreadable_tool_reads=0;
        s2_touch_pump();
        if(unreadable_tool_reads || s2_touch_tool_token())return camera+1;
        unreadable_tool=0;
    }
    lot_camera=0;return 0;
}
__declspec(dllexport) void cursor(int gw,int gh,int sw,int sh,int x,int y) {
    screen_w=gw;screen_h=gh;surface_w=sw;surface_h=sh;cursor_x=x;cursor_y=y;
}
__declspec(dllexport) int camera(void) {
    typed(0x3200,0x3300,0x3400,0x3500,".?AVcCameraNeighborhood@nTSSG@@");
    MEM32(0x3270)=0x3600;MEM32(0x3274)=0x360C;
    MEM32(0x3600)=0x44228000;MEM32(0x3608)=0x43660000;MEM32(0x32E0)=0x44228000;MEM32(0x32E4)=0x44228000;
    touch_mode(0x70,S2_TOUCH_TOWN);
    int before=logs;touch_log_camera();if(logs!=before)return 1;
    if(touch_float(0x32E0)!=650.0f || touch_float(0x3608)!=230.0f)return 2;
    g_ecx=0x3200;g_esp=0x800;sub_A0B46E90();
    if(town_camera!=0x3200 || g_esp!=0x808 || g_eax!=1)return 3;
    touch_log_camera();if(logs<=before)return 4;
    before=logs;MEM32(0x3274)=0x360D;touch_log_camera();if(logs!=before)return 5;
    MEM32(0x3274)=0x360C;touch_mode(0x70,S2_TOUCH_TOWN);
    if(town_camera!=0x3200)return 6;
    touch_mode(0x70,0);
    if(town_camera!=0x3200)return 7; /* mode transitions keep a constructed camera until scene teardown */
    s2_touch_reset_camera();if(town_camera || lot_camera || last_clamp_camera || last_clamp_vtable)return 9;
    touch_mode(0x71,S2_TOUCH_LIVE);before=logs;touch_log_camera();if(logs!=before)return 8;
    touch_mode(0x71,0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-touch-context-') as directory:
        base = Path(directory)
        cfile, obj, dllpath = base / 'check.c', base / 'check.obj', base / 'check.dll'
        cfile.write_text(prelude + stubs + actual + checks.replace('s2_touch_pump();', 'tick();'))
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11',
                        '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe',
                        '-I' + str(ROOT / 'android/host'), '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dllpath), str(obj)], check=True)
        dll = ctypes.CDLL(str(dllpath))
        dll.mode.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        try:
            assert dll.context() == 0
            assert dll.reused_live() == 0
            for i, (_, _, flag) in enumerate(pairs):
                assert dll.mode(i * 2, 100) and dll.context() == flag
                assert dll.mode(i * 2 + 1, 99) and dll.context() == flag
                assert dll.mode(i * 2 + 1, 100) and dll.context() == 0
            assert dll.owners() == 0
            result = dll.pick()
            assert result == 0, result
            assert dll.widgets() == 0
            assert dll.tools() == 0
            assert dll.unreadable_tool_recovery() == 0
            token_result=dll.tool_tokens()
            assert token_result == 0, ('tool token C',token_result)
            assert dll.camera() == 0
            dll.cursor.argtypes = [ctypes.c_int] * 6
            position = dll.Java_org_sims2_port_GameActivity_nativeTouchCursor
            position.argtypes = [ctypes.c_void_p] * 2
            position.restype = ctypes.c_uint64
            for gw, gh, sw, sh in ((1200, 540, 2400, 1080), (800, 600, 2400, 1080),
                                   (1600, 600, 1920, 1080), (1024, 768, 2048, 1536)):
                scale = min(sw / gw, sh / gh)
                for x, y in ((0, 0), (gw // 2, gh // 2), (gw - 1, gh - 1)):
                    dll.cursor(gw, gh, sw, sh, x, y)
                    packed = position(None, None)
                    actual = packed & 0xffffffff, packed >> 32
                    expected = ((x * scale + (sw - gw * scale) / 2) / sw * 1e6,
                                (y * scale + (sh - gh * scale) / 2) / sh * 1e6)
                    assert all(abs(a - e) <= 1 for a, e in zip(actual, expected)), (actual, expected)
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('Native touch actual C: mode/owner replacement, stale destructors, nested modals, '
          'ABI, main-thread read-only picks, RTTI widgets/tools/text focus, cursor letterbox projection, '
          'register preservation and shutdown PASS')


if __name__ == '__main__':
    main()
