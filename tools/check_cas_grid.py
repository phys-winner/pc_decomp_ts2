"""Compile actual native CAS grid classification and guarded page dispatch on the host."""
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import ROOT, CLANG
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/sims2_touch.c').read_text()
    prelude = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long long size_t;
#define UINT32_MAX 0xFFFFFFFFu
#define NULL ((void *)0)
#define S2_TOUCH_CAS 4u
#define S2_TOUCH_CHOOSER 8u
#define S2_TOUCH_TOWN 64u
#define S2_TOUCH_BUY 256u
#define S2_TOUCH_BUILD 512u
#define S2_TOUCH_FOCUSED_TEXT 1024u
static uint8_t memory[0x20000];
#define GPTR(a) (memory+(uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
static void *memchr(const void *s,int c,size_t n){const uint8_t *p=s;while(n--)if(*p++==(uint8_t)c)return(void *)(p-1);return 0;}
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return(uint8_t)*a-(uint8_t)*b;}
static int strncmp(const char *a,const char *b,size_t n){while(n--){if(*a!=*b)return(uint8_t)*a-(uint8_t)*b;if(!*a)return 0;a++;b++;}return 0;}
static const char *strstr(const char *s,const char *n){for(;*s;++s){const char *a=s,*b=n;while(*a&&*a==*b){a++;b++;}if(!*b)return s;}return 0;}
static void *memset(void *s,int c,size_t n){uint8_t *p=s;while(n--)*p++=(uint8_t)c;return s;}
static void text(uint32_t at,const char *s){do{memory[at++]=(uint8_t)*s;}while(*s++);}
static uint32_t win_manager=0x1000,context,epoch;
static unsigned dispatches,lookups,bad,reenter,registered;
static int void_vm_readable(uint32_t at,uint32_t n){return at>=0x1000 && (uint64_t)at+n<=sizeof memory;}
static int void_vm_writable(uint32_t at,uint32_t n){return void_vm_readable(at,n);}
static unsigned s2_touch_context(void){return context;}
static uint32_t s2_touch_owner_manager(void){return win_manager;}
static int s2_touch_text_editable(uint32_t w){(void)w;return 0;}
static int touch_layout_matches(unsigned c,uint64_t size,unsigned e){return c==context&&size==0x40000000300ull&&e==epoch;}
static int s2_catalog_info(uint32_t p){(void)p;bad=9;return 0;}
static void s2_catalog_track(uint32_t p){(void)p;bad=9;}
static int touch_swipe_group(uint32_t w,int *i){(void)w;(void)i;return 0;}
static unsigned touch_time_state(uint32_t w){(void)w;return 0;}
static int touch_swipe_apply(uint32_t w,int a,unsigned c,uint64_t s,unsigned e){(void)w;(void)a;(void)c;(void)s;(void)e;return 0;}
static uint32_t touch_town_catalog_proc(uint32_t w){(void)w;return 0;}
static uint32_t touch_chooser_window(uint32_t w){(void)w;return 0;}
static uint32_t touch_vertical_band(uint32_t w,int x,int y){(void)w;(void)x;(void)y;return 0;}
static uint32_t void_call_guest_this(uint32_t,uint32_t,int,const uint32_t *);
static uint32_t s2_ui_pick(uint32_t root,int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(MEM32(MEM32(root)+0xBC),root,2,p);}
static uint32_t touch_time_band(uint32_t w,int x,int y){(void)w;(void)x;(void)y;return 0;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int count,const uint32_t *args){
    if(va==0x11111111){if(self!=0x2000||count!=2||args[0]!=700||args[1]!=500)bad=1;
        if(reenter==1)context=0;return 0x4000;}
    if(va==0x22222222){if(self!=0x6000||count!=1||(args[0]!=0x120&&args[0]!=0x121))bad=2;
        ++lookups;if(reenter==2)context=0;if(reenter==3)MEM32(0x5458)=0;
        if(reenter==4)++epoch;if(reenter==6)MEM32(0x5218)=0;
        return args[0]==0x120?0x7000:0x7100;}
    if(va==0xA0755C58){if(self!=0x6000||count!=1||(args[0]!=0x120&&args[0]!=0x121))bad=3;
        if(!registered)return 0;++dispatches;if(reenter==5)context=0;
        return 1;}
    bad=4;return 0;
}
'''
    actual = ''.join(function(source, signature) for signature in (
        'static const char *touch_type(uint32_t object)\n{',
        'static int touch_ui_button(',
        'static int touch_grid_proc(', 'static uint32_t touch_bound_catalog_proc(',
        'static uint32_t touch_cas_grid_proc(',
        'static unsigned touch_widget(',
        'static int touch_cas_grid_apply(', 'static int touch_grid_apply(', 'static int touch_catalog_apply('))
    checks = r'''
static void typed(uint32_t object,uint32_t table,uint32_t col,uint32_t td,const char *name){
    MEM32(object)=table;MEM32(table-4)=col;MEM32(col+12)=td;text(td+8,name);
}
static void setup(const char *family){
    memset(memory,0,sizeof memory);context=S2_TOUCH_CAS;epoch=7;dispatches=lookups=bad=reenter=0;registered=1;
    MEM32(0x1044)=0x2000;MEM32(0x2000)=0x3000;MEM32(0x30BC)=0x11111111;
    MEM32(0x4058)=0x5000;
    typed(0x5000,0x8000,0x8400,0x8500,".?AVcGZWinGrid@@");
    MEM32(0x50A0)=0x5200;MEM32(0x5200)=0x5210;MEM32(0x5210)=0x5200;MEM32(0x5218)=0x616C;
    typed(0x5400,0xA000,0xA400,0xA500,".?AVcGZWinFlatRect@@");
    MEM32(0x5058)=0x5400;MEM32(0x5458)=0x6000;
    typed(0x6000,0x9000,0x9400,0x9500,family);
    typed(0x616C,0x9800,0x9C00,0x9500,family);
    MEM32(0x980C)=0xA075739A;MEM32(0x92C8)=0xA075843E;MEM32(0x909C)=0x22222222;
    /* v20 hardware: grid -> FlatRect -> owner; +0x184 is SimEditor, not the grid. */
    MEM32(0x6184)=0x1069ADC0;MEM32(0x6188)=0x0BBE3548;
    MEM32(0x6174)=51;MEM32(0x617C)=6;MEM32(0x61B8)=0;MEM32(0x61BC)=6;MEM32(0x61C0)=51;
    MEM32(0x61C4)=0x120;MEM32(0x61C8)=0x121;
    MEM32(0x7010)=0x120;MEM32(0x70E8)=3;MEM32(0x7110)=0x121;MEM32(0x71E8)=3;
}
__declspec(dllexport) unsigned check(void){
    const char *families[]={".?AVcTSUICASComponentClothing@nTSUICAS@@",".?AVcTSUICASComponentHair@nTSUICAS@@",
        ".?AVcTSUICASComponentArchetypes@nTSUICAS@@",".?AVcTSUICASComponentOverlays@nTSUICAS@@"};
    for(unsigned f=0;f<4;f++){
        setup(families[f]);if(touch_cas_grid_proc(0x616C,0x5000)!=0x6000)return 1;
        for(unsigned offset=0;offset<0x1CC;offset+=4)if(MEM32(0x6000+offset)==0x5000)return 5;
        if(touch_widget(0x4000)!=(8|32))return 4;
        if(!touch_catalog_apply(700,500,-3,0,4,0x40000000300ull,7)||dispatches!=3||lookups!=3||bad)return 2;
        if(!touch_catalog_apply(700,500,2,0,4,0x40000000300ull,7)||dispatches!=5||lookups!=5||bad)return 3;
    }
    for(unsigned failure=0;failure<24;failure++){
        setup(families[0]);
        switch(failure){
        case 0:context=64;break;
        case 1:MEM32(0x980C)=0;break;
        case 2:MEM32(0x92C8)=0;break;
        case 3:MEM32(0x5458)=0;break;
        case 4:MEM32(0x61BC)=0;break;
        case 5:MEM32(0x61BC)=513;break;
        case 6:MEM32(0x6174)=1000001;break;
        case 7:MEM32(0x61B8)=52;break;
        case 8:text(0x9508,".?AVcDifferentClass@@");break;
        case 9:text(0x8508,".?AVcGZWinTextEdit@@");break;
        case 10:MEM32(0x61C4)=0;break;
        case 11:MEM32(0x7010)=0x121;break;
        case 12:MEM32(0x70E8)=1;break;
        case 13:MEM32(0x70E8)=2;break;
        case 14:MEM32(0x1034)=1;break;
        case 15:registered=0;break;
        case 16:MEM32(0x616C)=0;break;
        case 17:MEM32(0x6000)=0;break;
        case 18:MEM32(0x5218)=0x6168;break;
        case 19:MEM32(0x50A0)=0;break;
        case 20:MEM32(0x5458)=0x5400;break;
        case 21:MEM32(0x5458)=0xFFFFFFFF;break;
        case 22:MEM32(0x5210)=0x5210;MEM32(0x5218)=0;break;
        case 23:MEM32(0x5058)=0x4000;break;
        }
        if(touch_catalog_apply(700,500,-1,0,4,0x40000000300ull,7)||dispatches||bad)return 10+failure;
    }
    for(unsigned r=1;r<=6;r++){
        setup(families[0]);reenter=r;int result=touch_catalog_apply(700,500,-3,0,4,0x40000000300ull,7);
        if((r==5?(result!=1||dispatches!=1):(result||dispatches))||bad)return 40+r;
    }
    setup(families[0]);
    if(touch_cas_grid_apply(0x616C,0x5000,0,4,0x40000000300ull,7)||
       touch_cas_grid_apply(0x616C,0x5000,9,4,0x40000000300ull,7)||
       touch_cas_grid_apply(0x616C,0x5000,-9,4,0x40000000300ull,7)||dispatches)return 50;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-cas-grid-') as directory:
        path = Path(directory); cfile, obj, dll = path/'check.c', path/'check.obj', path/'check.dll'
        cfile.write_text(prelude + actual + checks)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:'+str(dll), str(obj)], check=True)
        library = ctypes.CDLL(str(dll))
        try:
            result = library.check()
            assert result == 0, result
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(library._handle)
    print('Native CAS grid actual C PASS: four families, shared original callbacks, v20 hardware fields, '
          'exact proc registration and grid/FlatRect/owner ancestry; 24 invalid/disabled/unregistered guards, '
          'six reentrant/context/layout/ownership changes and bounded commands')


if __name__ == '__main__':
    main()
