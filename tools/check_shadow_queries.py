"""Exercise the actual Apply diagnostic and guest-call ABI without a device."""
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/sims2_patches.c').read_text()
    threads = (ROOT / 'android/host/threads.c').read_text()
    prelude = r'''
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef unsigned long long size_t;
typedef void (*recomp_func_t)(void);
#define NULL ((void *)0)
#define RECOMP_RETADDR 0xDEAD0000u
static uint8_t memory[65536], height_cache, portal_cache;
static uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_cur_func;
static uint32_t mode,missing,bits,queries,releases,frees,bad,logs,apply_calls;
static int recorded[7];
#define GPTR(a) (memory+(uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)((uint32_t)(a)==0xA0FFDE4Cu ? &height_cache : (uint32_t)(a)==0xA0FFDE40u ? &portal_cache : GPTR(a)))
#define PUSH32(sp,v) do{(sp)-=4;MEM32(sp)=(v);}while(0)
static size_t strlen(const char *s){size_t n=0;while(s[n])++n;return n;}
static void *memset(void *p,int v,size_t n){uint8_t *q=p;while(n--)*q++=(uint8_t)v;return p;}
static void *memcpy(void *p,const void *s,size_t n){uint8_t *q=p;const uint8_t *r=s;while(n--)*q++=*r++;return p;}
static void void_lift_trap(uint32_t va,const char *s){(void)va;(void)s;bad=1;}
static uint32_t void_galloc(uint32_t n){if(n!=68)bad=2;return mode==4?0:0x6000;}
static void void_gfree(uint32_t p){if(p!=0x6000)bad=3;++frees;}
#include <stdarg.h>
static void void_log(const char *fmt,...){
    ++logs;if(mode==4)return;
    va_list ap;va_start(ap,fmt);for(int i=0;i<7;++i)recorded[i]=va_arg(ap,int);va_end(ap);
}
static void clobber(void){g_ebx=11;g_esi=12;g_edi=13;g_ebp=14;g_eax=0x12340000;g_edx=15;}
static void get_com(void){clobber();g_eax=mode==1?0:0x2000;g_esp+=4;}
static void get_set(void){
    if(g_ecx!=0x2000 || MEM32(g_esp+4)!=0x0AC1887E || MEM32(g_esp+8)!=0x4BB0D269)bad=4;
    MEM32(MEM32(g_esp+12))=mode==2?0:0x2100;clobber();g_eax=1;g_esp+=16;
}
static void get_property(void){
    static const char *names[4]={"simShadows","objectShadows","guob","heightMapShadows"};
    uint32_t key=MEM32(g_esp+4),any=MEM32(g_esp+8),i=queries++,len=(uint32_t)strlen(names[i]);
    if(g_ecx!=0x2100 || i>=4 || MEM32(key+16)!=len || MEM32(key+20)!=31 || MEM32(any)!=0xA0D9C670)bad=5;
    for(uint32_t j=0;j<=len;++j)if(MEM8(MEM32(key)+j)!=(uint8_t)names[i][j])bad=6;
    MEM8(any+4)=(bits&(1u<<i))?1:0;clobber();g_eax=0x12340000u+((missing&(1u<<i))?0:1);g_esp+=12;
}
static void release_set(void){if(g_ecx!=0x6000 || MEM32(g_ecx)!=0x2100)bad=7;++releases;clobber();g_esp+=4;}
static void get_scene(void){clobber();g_eax=mode==3?0:0x2200;g_esp+=4;}
static void get_manager(void){if(g_ecx!=0x2200)bad=8;clobber();g_eax=mode==5?0:0x2300;g_esp+=4;}
static void get_params(void){if(g_ecx!=0x2300)bad=9;clobber();g_eax=mode==6?0:0x2400;g_esp+=4;}
static recomp_func_t void_resolve(uint32_t va){
    if(va==0xA014323F)return get_com;
    if(va==0xA020DEFB)return get_scene;
    if(va==0xA01CBD79)return release_set;
    if(va==0x4000)return get_set;
    if(va==0x4001)return get_property;
    if(va==0x4002)return get_manager;
    if(va==0x4003)return get_params;
    bad=10;return get_com;
}
static void sub_A064ADC6_lifted(void){++apply_calls;g_eax=0xA1B2C3D4;g_esp+=4;}
'''
    actual = (function(threads, 'uint32_t void_call_guest(') +
              function(threads, 'uint32_t void_call_guest_this(') +
              function(source, 'static void log_shadow_settings(') +
              function(source, 'void sub_A064ADC6(void)'))
    check = r'''
__declspec(dllexport) uint32_t check(uint32_t m,uint32_t miss,uint32_t flags){
    mode=m;missing=miss;bits=flags;queries=releases=frees=bad=logs=apply_calls=0;
    memset(memory,0,sizeof memory);memset(recorded,0,sizeof recorded);
    height_cache=1;portal_cache=0;
    MEM32(0x2000)=0x3000;MEM32(0x3014)=0x4000;
    MEM32(0x2100)=0x3100;MEM32(0x3124)=0x4001;
    MEM32(0x2200)=0x3200;MEM32(0x32C8)=0x4002;
    MEM32(0x2300)=0x3300;MEM32(0x3330)=0x4003;
    MEM32(0x242C)=0x353;
    g_esp=0xF000;g_ebx=0xBB;g_esi=0x55;g_edi=0xDD;g_ebp=0xEE;g_cur_func=0xABC;
    sub_A064ADC6();
    if(bad)return bad;
    if(g_eax!=0xA1B2C3D4 || g_esp!=0xF004 || g_ebx!=0xBB || g_esi!=0x55 || g_edi!=0xDD || g_ebp!=0xEE || g_cur_func!=0xABC)return 20;
    if(apply_calls!=1 || logs!=1)return 21;
    if(mode==4)return queries||releases||frees?22:0;
    uint32_t present=mode!=1 && mode!=2;
    if(queries!=(present?4u:0u) || releases!=present || frees!=1)return 23;
    for(uint32_t i=0;i<4;++i)
        if(recorded[i]!=(present && !(missing&(1u<<i)) ? ((bits>>i)&1u) : -1))return 24;
    if((uint32_t)recorded[4]!=(mode==3 || mode==5 || mode==6 ? 0xFFFFFFFFu : 0x353u))return 25;
    if(recorded[5]!=1 || recorded[6]!=0)return 26;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-shadow-queries-') as directory:
        base = Path(directory)
        cfile, obj, library = base / 'check.c', base / 'check.obj', base / 'check.dll'
        cfile.write_text(prelude + actual + check)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library))
        dll.check.argtypes = [ctypes.c_uint32] * 3
        try:
            for mode in range(7):
                for missing in range(16):
                    for bits in range(16):
                        result = dll.check(mode, missing, bits)
                        assert result == 0, (mode, missing, bits, result)
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('Shadow query actual C + guest-call helpers: 1792 cases; exact COM/key/AnyBoolean ABI, '
          'all boolean/missing combinations, absent services/allocation failure, mask/caches, '
          'reference release, once-per-Apply logging and EAX/nonvolatile/ESP preservation PASS')


if __name__ == '__main__':
    main()
