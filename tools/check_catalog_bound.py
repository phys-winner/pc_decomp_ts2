"""Replay captured Buy delegates through the production C using the original PE RTTI."""
import ctypes
import json
from pathlib import Path
import subprocess
import tempfile

import pefile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    fixture = json.loads((ROOT / 'tools/fixtures/catalog-buy-runtime.json').read_text())
    touch = (ROOT / 'android/host/sims2_touch.c').read_text()
    tooltip = (ROOT / 'android/host/sims2_tooltip.c').read_text()
    image = pefile.PE(str(ROOT / 'original/Sims2EP9.exe')).get_memory_mapped_image()
    prelude = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t,size_t;
typedef long long int64_t;
#define NULL ((void *)0)
#define UINT32_MAX 0xFFFFFFFFu
#define INT32_MAX 2147483647
#define S2_TOUCH_LIVE 128
#define S2_TOUCH_TOWN 64
#define S2_TOUCH_BUY 256
#define S2_TOUCH_BUILD 512
#define S2_TOUCH_CAS 4
#define S2_TOUCH_CHOOSER 8
#define S2_TOUCH_FOCUSED_TEXT 1024
#define API __declspec(dllexport)
typedef unsigned pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static void pthread_mutex_lock(pthread_mutex_t *p){*p=1;}
static void pthread_mutex_unlock(pthread_mutex_t *p){*p=0;}
static uint8_t image[0x1210000], memory[64][0x800], empty[0x800];
static uint32_t starts[64], lengths[64], used;
API void *image_data(void){return image;}
API void *region(uint32_t address,uint32_t length){
 if(used>=64||length>0x800)return NULL;
 starts[used]=address;lengths[used]=length;return memory[used++];
}
static void *pointer(uint32_t a){
 if(a>=0xA0000000u&&(uint64_t)a<0xA0000000ull+sizeof image)return image+(a-0xA0000000u);
 for(unsigned n=0;n<used;n++)if(a>=starts[n]&&(uint64_t)a<starts[n]+(uint64_t)lengths[n])return memory[n]+a-starts[n];
 return empty;
}
#define GPTR(a) pointer((uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)GPTR(a))
static int void_vm_readable(uint32_t a,uint32_t length){
 if(a>=0xA0000000u&&(uint64_t)a+length<=0xA0000000ull+sizeof image)return 1;
 for(unsigned n=0;n<used;n++)if(a>=starts[n]&&(uint64_t)a+length<=starts[n]+(uint64_t)lengths[n])return 1;
 return 0;
}
static int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n)&&a<0xA0000000u;}
static void *memchr(const void *p,int c,size_t n){const uint8_t *s=p;while(n--){if(*s==(uint8_t)c)return(void *)s;++s;}return NULL;}
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return(uint8_t)*a-(uint8_t)*b;}
static int strncmp(const char *a,const char *b,size_t n){while(n--){if(*a!=*b)return(uint8_t)*a-(uint8_t)*b;if(!*a)return 0;++a;++b;}return 0;}
static const char *strstr(const char *s,const char *n){for(;*s;++s){const char *a=s,*b=n;while(*a&&*a==*b){++a;++b;}if(!*b)return s;}return NULL;}
static unsigned context=256,owner_thread=1,shows,pages,picks,reenter,epoch=7,prepares,prepare_result,clears;
static unsigned swipe_sequence,shown_swipe_context,shown_swipe_epoch,swipe_version;
static uint64_t shown_swipe;
#define atomic_store(p,v) (*(p)=(v))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
static void void_log(const char *s,...){(void)s;}
static int width=1170,height=540;
static uint32_t win_manager=0x014E03E8u,g_ecx,g_eax,g_esp;
static uint32_t scene_owner,scene_window;
static unsigned s2_touch_context(void){return context;}
static uint32_t s2_touch_owner_manager(void){return owner_thread?win_manager:0;}
static unsigned s2_touch_layout_epoch(void){return epoch;}
static void void_input_screen_size(int *w,int *h){*w=width;*h=height;}
static int touch_layout_matches(unsigned c,uint64_t size,unsigned epoch){return context==c&&size==((uint64_t)540<<32|1170)&&epoch==7;}
static int s2_touch_text_editable(uint32_t window){(void)window;return 0;}
static uint32_t touch_cas_grid_proc(uint32_t p,uint32_t w){(void)p;(void)w;return 0;}
static int touch_cas_grid_apply(uint32_t p,uint32_t w,int a,unsigned c,uint64_t s,unsigned e){(void)p;(void)w;(void)a;(void)c;(void)s;(void)e;return 0;}
int _fltused=0;
static int touch_ui_button(uint32_t);
static double s2_ui_factor(void){return 1;}
static uint32_t void_call_guest_this(uint32_t,uint32_t,int,const uint32_t *);
static uint32_t s2_ui_pick(uint32_t root,int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(MEM32(MEM32(root)+0xBC),root,2,p);}
int s2_catalog_info(uint32_t);
void s2_catalog_track(uint32_t);
'''
    actual = touch[touch.index('static const uint32_t touch_swipe_ids'):touch.index('static int touch_swipe_group')] + '\n'.join(function(touch, signature) for signature in [
        'static const char *touch_type(uint32_t object)\n{', 'static int touch_ui_button(', 'static int touch_swipe_group(', 'static int touch_swipe_owner(', 'static uint32_t touch_vertical_band(', 'static int touch_time_index(', 'static unsigned touch_time_state(', 'static uint32_t touch_time_band(', 'static int touch_swipe_apply(', 'static int touch_grid_proc(',
        'static uint32_t touch_bound_catalog_proc(', 'static uint32_t touch_town_catalog_proc(', 'static unsigned touch_widget(',
        'static int touch_grid_apply(', 'static int touch_catalog_apply('])
    actual += '\n' + '\n'.join(line for line in tooltip.splitlines() if not line.startswith('#include'))
    check = r'''
void sub_A07495DA_lifted(void){++shows;g_eax=1;g_esp+=4;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xA09315A7u){
  ++picks;
  if(self!=0x01B56E08u||n!=2||args[0]!=610||args[1]!=436)return 0;
  if(reenter==1)context|=1;
  if(reenter==2)win_manager=0;
  if(reenter==3)MEM32(win_manager+0x44)=0;
  if(reenter==4)MEM32(win_manager+0x34)=1;
  return 0x1227268Cu;
 }
 if(va==0xA07522C6u){
  if(self!=0x13E28488u||n!=1||args[0]||!preparing_catalog_popup)return 0;
  ++prepares;
  if(prepare_result){MEM32(self+0x18C)=0x0F843348u;MEM32(0x0F84365C)=0x06EAC2A4u;}
  return 1;
 }
 if(va==0xA07479F0u){if(self!=0x13E28488u||n!=1)return 0;++pages;MEM32(self+0x20)=args[0];return 1;}
 if(va==0xA07486C3u){if(self!=0x13E28488u||n||args)return 0;++clears;return 1;}
 if(va!=0xA07495DAu||self!=0x0F8432C0u||n||args)return 0;
 g_ecx=self;g_esp-=4;sub_A07495DA();return g_eax;
}
API uint32_t read32(uint32_t a){return MEM32(a);}
API void write32(uint32_t a,uint32_t v){MEM32(a)=v;}
API void reset(unsigned c,unsigned r){
 context=c;reenter=r;owner_thread=1;win_manager=0x014E03E8u;
 MEM32(win_manager+0x44)=0x01B56E08u;MEM32(win_manager+0x34)=0;
 g_esp=0x2F000;shows=pages=picks=prepares=prepare_result=clears=0;explicit_catalog_popup=0;preparing_catalog_popup=0;tracked_catalog.thumbnail=0;
 epoch=7;width=1170;height=540;
}
API void prepare(unsigned value){prepare_result=value;}
API unsigned prepared(void){return prepares;}
API void owner(unsigned value){owner_thread=value;}
API uint32_t classify(uint32_t w){return touch_widget(w);}
API uint32_t bound(uint32_t w){return touch_bound_catalog_proc(w);}
API int info(void){return s2_catalog_info(0x0BD3CEB8u);}
API int apply(int amount,int info,unsigned epoch,uint64_t size){return touch_catalog_apply(610,436,amount,info,context,size,epoch);}
API unsigned shown(void){return shows;}
API unsigned paged(void){return pages;}
API unsigned stack(void){return g_esp;}
API void track(void){s2_catalog_track(0x0BD3CEB8u);}
API unsigned clear_ticket(void){return s2_catalog_clear_request();}
API int clear(unsigned serial){return s2_catalog_clear_owner(serial);}
API unsigned cleared(void){return clears;}
API void change_guard(unsigned which){
 if(which==1)context|=1;if(which==2)++epoch;if(which==3)++width;if(which==4)++height;
 if(which==5)owner_thread=0;if(which==6)MEM32(win_manager+0x44)=0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-bound-catalog-') as folder:
        base = Path(folder)
        c, obj, dll = base/'check.c', base/'check.obj', base/'check.dll'
        c.write_text(prelude + actual + check)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        lib = ctypes.CDLL(str(dll))
        try:
            lib.image_data.restype = ctypes.c_void_p
            lib.region.restype = ctypes.c_void_p
            lib.region.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
            lib.read32.argtypes = [ctypes.c_uint32]
            lib.read32.restype = ctypes.c_uint32
            lib.write32.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
            lib.classify.argtypes = lib.bound.argtypes = [ctypes.c_uint32]
            lib.bound.restype = ctypes.c_uint32
            lib.apply.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint64]
            ctypes.memmove(lib.image_data(), image, len(image))
            # Merge overlapping bounded captures without inventing bytes between regions.
            words = {}
            for record in fixture['regions']:
                address = int(record['address'], 16)
                for offset, value in record['words'].items():
                    words[address+int(offset, 16)] = int(value, 16)
            sorted_words = sorted(words)
            for start in sorted_words:
                if start-4 in words:
                    continue
                end = start
                while end in words:
                    end += 4
                raw = b''.join(words[a].to_bytes(4, 'little') for a in range(start, end, 4))
                destination = lib.region(start, len(raw))
                assert destination
                ctypes.memmove(destination, raw, len(raw))
            # The picker and owner lifecycle are explicit boundaries, not captured callbacks.
            for address, length in [(0x014E03E8, 0x48), (0x01B56E08, 0xEC)]:
                assert lib.region(address, length)
            lib.write32(0x01B56E08, 0xA0E54498)
            size = 540 << 32 | 1170
            for mode in [256, 512]:
                lib.reset(mode, 0)
                assert lib.bound(0x15212BE8) == 0x0BD3CEB8
                assert lib.bound(0x1377B4C8) == 0x13E28488
                assert lib.classify(0x1227268C) == 8 | 32 | 64
                assert lib.apply(0, 1, 7, size) == 1 and lib.shown() == 1
                assert lib.stack() == 0x2F000
                assert lib.apply(-1, 0, 7, size) == 1 and lib.paged() == 1
                assert lib.read32(0x13E284A8) == 1
                assert lib.apply(8, 0, 7, size) == 1 and lib.paged() == 2
                assert lib.read32(0x13E284A8) == 0
                assert lib.apply(0, 1, 8, size) == 0
                assert lib.apply(0, 1, 7, size+1) == 0
            # A clear consumes the exact cached hit snapshot once; newer selection tickets survive stale requests.
            for mode in [256,512]:
                lib.reset(mode,0);assert lib.clear_ticket()==0
                lib.track();ticket=lib.clear_ticket();assert ticket and lib.clear(ticket)==1 and lib.cleared()==1
                assert not lib.clear(ticket) and not lib.clear_ticket() and lib.cleared()==1
                lib.reset(mode,0);lib.track();stale=lib.clear_ticket();lib.track();fresh=lib.clear_ticket()
                assert fresh!=stale and not lib.clear(stale) and not lib.cleared()
                assert lib.clear(fresh)==1 and lib.cleared()==1
            for guard in range(1,7):
                lib.reset(256,0);lib.track();ticket=lib.clear_ticket();lib.change_guard(guard)
                assert ticket and not lib.clear(ticket) and not lib.cleared(),guard
            for address,value in [(0x15212BE8,0),(0x15212BEC,0),(0x15212D14,0),
                                  (0x0BD3CEC0,0),(0x1377B4CC,0),(0x13E284DC,0),
                                  (0x13E285E8,0),(0x13E284AC,513),(0x13E2852C,0),
                                  (0x051E7AD8,1),(0x0BD3CECC,0),(0x0BD3CEF4,1)]:
                lib.reset(256,0);lib.track();ticket=lib.clear_ticket();previous=lib.read32(address)
                lib.write32(address,value);assert not lib.clear(ticket) and not lib.cleared(),hex(address)
                lib.write32(address,previous)
            for mismatch in [False, True]:
                lib.reset(512, 0)
                if mismatch:
                    lib.write32(0x0F84365C, 0x06EAC2A8)
                else:
                    lib.write32(0x13E28614, 0)
                lib.prepare(1)
                assert lib.info() == 1 and lib.prepared() == 1 and lib.shown() == 1
                assert lib.stack() == 0x2F000
            for address, value in [
                (0x15212BE8, 0xA0E5449C), (0x15212BEC, 1), (0x15212CE0, 0),
                (0x15212D14, 0), (0x0BD3CEC0, 0), (0x1377B4CC, 1),
                (0x1377B5F4, 0), (0x13E284DC, 0), (0x13E285E8, 0),
                (0x13E284AC, 513), (0x0BD3CEE8, 0), (0x0BE2091C, 0),
                (0x13E2852C, 0), (0x051E7AD8, 1), (0x0F8432B8, 0),
                (0x0F843348, 0), (0x0F843318, 0), (0x0F84365C, 0),
            ]:
                previous = lib.read32(address)
                lib.write32(address, value)
                lib.reset(256, 0)
                assert lib.info() == 0 and not lib.shown(), hex(address)
                lib.write32(address, previous)
            for reentry in [1, 2, 3, 4]:
                lib.reset(256, reentry)
                assert not lib.apply(0, 1, 7, size) and not lib.shown()
                lib.reset(256, reentry)
                assert not lib.apply(-1, 0, 7, size) and not lib.paged()
            lib.reset(256, 0)
            lib.owner(0)
            assert not lib.bound(0x15212BE8) and not lib.info()
            for mode in [0, 4, 128, 257, 513]:
                lib.reset(mode, 0)
                assert not lib.bound(0x15212BE8) and not lib.info()
            print('PASS captured Buy C: original PE RTTI/delegates, actual button flags104, selected membership/product popup, original page setter; absent/mismatched product original-info preparation,18 negatives,8 picker reentries; cached-selection clear dispatch once, stale/new ticket, product/owner/context/epoch/size rejection and ABI. Build runs the same captured object-catalog shape, not physical Build evidence.')
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(lib._handle)


if __name__ == '__main__':
    main()
