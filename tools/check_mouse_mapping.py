"""Run actual Android cursor mapping C as ARM64, without JNI/device access.

Window/input/picker boundaries are stubs. All production mapping bodies run;
the lroundf boundary uses ARM64's matching finite ties-away instruction.
"""
import hashlib
import io
import json
from pathlib import Path
import subprocess

from build_android import CLANG, COMMON, ROOT
from check_virtual_query import function
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A


def main():
    source = (ROOT / 'android/host/android_main.c').read_text(encoding='utf-8')
    signatures = ('static int view_to_game(',
                  'JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchHitKind(',
                  'JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchGameSize(',
                  'JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchCursor(',
                  'JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseMove(',
                  'JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseWarp(')
    bodies = '\n'.join(function(source, signature) for signature in signatures)
    prelude = r'''
#include <stdint.h>
#include "sims2_settings.h"
#define JNIEXPORT
#define JNICALL
#define isfinite(x) __builtin_isfinite(x)
typedef int jint,jboolean;
typedef float jfloat;
typedef int64_t jlong;
typedef void JNIEnv;
typedef void *jclass,*jobject;
static float fminf(float a,float b){return a<b?a:b;}
static float fmaxf(float a,float b){return a>b?a:b;}
static long lroundf(float v){long r;__asm__("fcvtas %x0, %s1":"=r"(r):"w"(v));return r;}
static int screen_w,screen_h,surface_w,surface_h,cursor_x,cursor_y,window_lock;
static unsigned context=S2_TOUCH_BUILD,picks,moves,warps;
static int picked_x,picked_y,mock_area=2;
static void *window=(void *)1;
static void pthread_mutex_lock(int *p){}
static void pthread_mutex_unlock(int *p){}
static void void_input_screen_size(int *w,int *h){*w=screen_w;*h=screen_h;}
static void void_input_cursor(uint32_t *x,uint32_t *y){*x=cursor_x;*y=cursor_y;}
static int ANativeWindow_getWidth(void *p){return surface_w;}
static int ANativeWindow_getHeight(void *p){return surface_h;}
static void clamp(void){
 if(cursor_x<0)cursor_x=0;if(cursor_x>=screen_w)cursor_x=screen_w-1;
 if(cursor_y<0)cursor_y=0;if(cursor_y>=screen_h)cursor_y=screen_h-1;
}
static void void_input_mouse_move(int dx,int dy){++moves;cursor_x+=dx;cursor_y+=dy;clamp();}
static void void_input_mouse_warp(int x,int y){++warps;cursor_x=x;cursor_y=y;clamp();}
void s2_touch_camera_pointer_intent(void){}
unsigned s2_touch_context(void){return context;}
int s2_touch_pick(int x,int y){++picks;picked_x=x;picked_y=y;return mock_area;}
static unsigned query_prectx_logs;
static void void_log(const char *format,...){(void)format;++query_prectx_logs;}
'''
    checks = r'''
uint32_t checks,roundtrips,geometries;
#define CHECK(x) do{++checks;if(!(x))return __LINE__;}while(0)
static void setup(int w,int h,int sw,int sh){screen_w=w;screen_h=h;surface_w=sw;surface_h=sh;window=(void *)1;context=S2_TOUCH_BUILD;cursor_x=w/2;cursor_y=h/2;}
static int roundtrip(int x,int y){
 cursor_x=x;cursor_y=y;
 uint64_t p=(uint64_t)Java_org_sims2_port_GameActivity_nativeTouchCursor(NULL,NULL);
 Java_org_sims2_port_GameActivity_nativeMouseMove(NULL,NULL,(float)(uint32_t)p/1000000.f,(float)(uint32_t)(p>>32)/1000000.f,0);
 ++roundtrips;return cursor_x==x && cursor_y==y;
}
static float normalized_x(float x){float s=fminf((float)surface_w/screen_w,(float)surface_h/screen_h);return(x*s+(surface_w-screen_w*s)*.5f)/surface_w;}
static float normalized_y(float y){float s=fminf((float)surface_w/screen_w,(float)surface_h/screen_h);return(y*s+(surface_h-screen_h*s)*.5f)/surface_h;}
uint32_t selftest(void){
 static const int sizes[][4]={{1212,540,2424,1080},{1600,900,2424,1080},{800,600,2424,1080},{2424,1080,2424,1080},{4096,2160,2424,1080},{1080,2424,1080,2424},{1,1,2424,1080}};
 for(unsigned i=0;i<sizeof sizes/sizeof sizes[0];++i){
  int w=sizes[i][0],h=sizes[i][1];setup(w,h,sizes[i][2],sizes[i][3]);++geometries;
  CHECK((uint64_t)Java_org_sims2_port_GameActivity_nativeTouchGameSize(NULL,NULL)==((uint64_t)(uint32_t)h<<32|(uint32_t)w));
  for(int x=0;x<w;++x)CHECK(roundtrip(x,h/2));
  for(int y=0;y<h;++y)CHECK(roundtrip(w/2,y));
  CHECK(roundtrip(0,0));CHECK(roundtrip(w-1,h-1));CHECK(roundtrip(0,h-1));CHECK(roundtrip(w-1,0));
  unsigned before=picks;
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,normalized_x(w-.125f),normalized_y(h-.125f))==2);
  CHECK(picks==before+1 && picked_x==w-1 && picked_y==h-1);
  before=picks;
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,normalized_x(-.25f),normalized_y(h/2))==1);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,normalized_x(w+.25f),normalized_y(h/2))==1);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,normalized_x(w/2),normalized_y(-.25f))==1);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,normalized_x(w/2),normalized_y(h+.25f))==1);
  CHECK(picks==before);
  Java_org_sims2_port_GameActivity_nativeMouseMove(NULL,NULL,-10,-10,0);CHECK(cursor_x==0 && cursor_y==0);
  Java_org_sims2_port_GameActivity_nativeMouseWarp(NULL,NULL,10,10);CHECK(cursor_x==w-1 && cursor_y==h-1);
  // Game-originated warp remains the new authoritative starting position.
  cursor_x=0;cursor_y=h-1;CHECK(roundtrip(0,h-1));
 }
 setup(1212,540,2424,1080);
 unsigned before=picks;
 CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,1,.5f)==1);
 CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,1)==1 && picks==before);
 float nan=__builtin_nanf(""),inf=__builtin_inff();
 for(unsigned i=0;i<4;++i){
  float x=i==0?nan:i==1?inf:i==2?-inf:.5f,y=i==3?nan:.5f;
  unsigned oldmoves=moves,oldwarps=warps;int gx=2,gy=2;
  CHECK(!view_to_game(x,y,&gx,&gy) && gx==-1 && gy==-1);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,x,y)==1 && picks==before);
  Java_org_sims2_port_GameActivity_nativeMouseMove(NULL,NULL,x,y,0);
  Java_org_sims2_port_GameActivity_nativeMouseMove(NULL,NULL,x,y,1);
  Java_org_sims2_port_GameActivity_nativeMouseWarp(NULL,NULL,x,y);
  CHECK(moves==oldmoves && warps==oldwarps);
 }
 for(unsigned i=0;i<4;++i){
  setup(1212,540,2424,1080);if(i==0)screen_w=0;if(i==1)screen_h=-1;if(i==2)surface_w=0;if(i==3)surface_h=-1;
  int gx=2,gy=2;CHECK(!view_to_game(.5f,.5f,&gx,&gy) && gx==-1 && gy==-1);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchCursor(NULL,NULL)==0);
  CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,.5f)==1);
  if(i<2)CHECK(Java_org_sims2_port_GameActivity_nativeTouchGameSize(NULL,NULL)==0);
 }
 setup(1212,540,0,0);window=NULL;CHECK(roundtrip(190,416));
 context=0;mock_area=1;unsigned oldpicks=picks;
 CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,.5f)==1 && picks==oldpicks+1);
 mock_area=0;CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,.5f)==0);
 context=S2_TOUCH_OPTIONS;mock_area=17;CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,.5f)==17);
 context=S2_TOUCH_LOADING;oldpicks=picks;CHECK(Java_org_sims2_port_GameActivity_nativeTouchHitKind(NULL,NULL,.5f,.5f)==1 && picks==oldpicks);
 return 0;
}
'''
    output = ROOT / 'build/test/cas-slider-diagnostics/mapping'
    output.mkdir(parents=True, exist_ok=True)
    cfile, binary = output / 'check.c', output / 'check.elf'
    cfile.write_text(prelude + bodies + checks, encoding='utf-8')
    subprocess.run([str(CLANG), *COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True, cwd=ROOT)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, stack, stop = 0x10000000, 0x50000000, 0x60000000
    uc.mem_map(base, 0x1000000)
    uc.mem_map(stack, 0x10000)
    uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            uc.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    if relocations:
        for r in relocations.iter_relocations():
            assert r['r_info_type'] == 1027
            uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000)
    uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=100000000)
    line = uc.reg_read(A.UC_ARM64_REG_X0)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop, 'ARM64 instruction budget exhausted'
    assert line == 0, f'actual-C assertion failed at generated check.c:{line}'
    counts = {name: int.from_bytes(uc.mem_read(base + symbols[name], 4), 'little') for name in ('checks', 'roundtrips', 'geometries')}
    result = dict(status='PASS', **counts, actual_C_functions_sha256=hashlib.sha256(bodies.encode()).hexdigest(),
                  scope='Actual production JNI C bodies, NDK ARM64 under Unicorn; window/input/picker/finite-lroundf boundary stubs; no device operations.')
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
