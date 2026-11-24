"""Production native widget selection against original EP9 RTTI and callback boundaries."""
import ctypes
import re
from pathlib import Path
import subprocess
import tempfile
import struct
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X
from build_android import CLANG, ROOT
from check_virtual_query import function
from build_overlay import dbpf, GAME

PRELUDE = r'''
int _fltused=0;
typedef unsigned char uint8_t;typedef unsigned int uint32_t;
typedef unsigned long long uint64_t,size_t;
#define NULL ((void *)0)
#define UINT32_MAX 0xFFFFFFFFu
#define INT32_MAX 2147483647
#define S2_TOUCH_CAS 4
#define S2_TOUCH_TOWN 64
#define S2_TOUCH_CHOOSER 8
#define S2_TOUCH_FOCUSED_TEXT 1024
#define S2_TOUCH_LIVE 128
#define S2_TOUCH_BUY 256
#define S2_TOUCH_BUILD 512
#define API __declspec(dllexport)
static uint8_t image[0x1210000],arena[0x20000];
API void *image_data(void){return image;}
static int void_vm_readable(uint32_t a,uint32_t n){return(a>=0x1000&&(uint64_t)a+n<=sizeof arena)||(a>=0xA0000000u&&(uint64_t)a+n<=0xA0000000ull+sizeof image);}
static void *pointer(uint32_t a){return a>=0xA0000000u?image+(a-0xA0000000u):arena+a;}
#define GPTR(a) pointer((uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
static void *memchr(const void *p,int c,size_t n){const uint8_t *s=p;while(n--){if(*s==(uint8_t)c)return(void *)s;++s;}return NULL;}
static int strncmp(const char *a,const char *b,size_t n){while(n--){if(*a!=*b)return(uint8_t)*a-(uint8_t)*b;if(!*a)return 0;++a;++b;}return 0;}
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return(uint8_t)*a-(uint8_t)*b;}
static unsigned context=128,epoch=3,owner=1,notified,reentry,picks,toggles;
static unsigned swipe_sequence,shown_swipe_context,shown_swipe_epoch,swipe_version;
static uint64_t shown_swipe;
#define atomic_store(p,v) (*(p)=(v))
#define atomic_load(p) (*(p))
#define atomic_fetch_add(p,v) ((*(p)+=(v))-(v))
static void void_log(const char *s,...){(void)s;}
static unsigned fixture_menu,fixture_chooser,fixture_hidden_vertical,fixture_rewards;
static uint32_t win_manager=0x11000,last_id,scene_owner,scene_window,town_camera,last_world_context,tool_handler;
static unsigned s2_touch_context(void){return context;}
static int s2_touch_rewards(void){return fixture_rewards!=0;}
static unsigned s2_touch_layout_epoch(void){return epoch;}
static uint32_t s2_touch_owner_manager(void){return owner?win_manager:0;}
static int touch_layout_matches(unsigned c,uint64_t size,unsigned e){return c==context&&size==((uint64_t)600<<32|1300)&&e==epoch;}
static uint32_t void_call_guest_this(uint32_t,uint32_t,int,const uint32_t *);
static double s2_ui_factor(void){return 1;}
static uint32_t s2_ui_pick(uint32_t root,int x,int y){uint32_t p[2]={(uint32_t)x,(uint32_t)y};return void_call_guest_this(MEM32(MEM32(root)+0xBC),root,2,p);}
static uint32_t touch_input_pick(uint32_t root,int x,int y){return s2_ui_pick(root,x,y);}
static uint32_t touch_town_catalog_proc(uint32_t w){(void)w;return 0;}
static uint32_t touch_bound_catalog_proc(uint32_t w){(void)w;return 0;}
static uint32_t touch_cas_grid_proc(uint32_t p,uint32_t w){(void)p;(void)w;return 0;}
static int touch_grid_proc(uint32_t p){(void)p;return 0;}
static int touch_grid_apply(uint32_t p,uint32_t w,int a){(void)p;(void)w;(void)a;return 0;}
static int touch_cas_grid_apply(uint32_t p,uint32_t w,int a,unsigned c,uint64_t s,unsigned e){(void)p;(void)w;(void)a;(void)c;(void)s;(void)e;return 0;}
static int s2_catalog_info(uint32_t p){(void)p;return 0;}
'''
CHECK = r'''
static uint32_t button(unsigned n){return 0x14004+n*0x400;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(fixture_hidden_vertical&&self==0x12000&&n==1&&args[0]==0xCB980E82u)return 0x1A000;
 if((self==0x12000||self==scene_window)&&n==1&&args[0]==MEM32(0x13010))return fixture_chooser?0x18000:0x13000;
 if(self==0x12000&&n==1&&args[0]==0x37D82A93u)return 0x17404;
 if(self==0x18000&&n==1&&args[0]==0x77D986A1u)return 0x13000;
 if(fixture_chooser&&self==0x18000&&n==1&&args[0]==0x1005)return button(1);
 if((self==0x13000||(fixture_menu&&self==0x13400))&&n==1){
  ++picks;if(reentry==1)context|=1;if(reentry==2)owner=0;if(reentry==3)MEM32(0x11044)=0;
  if(reentry==4)MEM32(button(0)+0x58)=0;
  for(unsigned i=0;i<(fixture_menu?9:6);++i)if(MEM32(button(i)+0x10)==args[0])return button(i);
  return 0;
 }
 if(va==0xA0936F94u&&n==2&&args[0]<=1&&args[1]==1){
  MEM32(self+0xFC)=args[0];++toggles;
  if(reentry==5)context|=1;if(reentry==6)owner=0;if(reentry==7)MEM32(0x11044)=0;
  if(reentry==8)MEM32(self+4+0x58)=0;
  if(reentry==9)MEM32(self+4)=0;
  return 1;
 }
 if(va!=0xA093550Au||n||args||(!fixture_menu&&(toggles!=1||!(MEM32(self+0xFC)&1))))return 0;
 for(unsigned i=0;i<(fixture_menu?9:6);++i)if(self+4==button(i)){
  for(unsigned j=0;j<6;++j)MEM32(button(j)+0xF8)=j==i;
  if(MEM32(button(0)+0x10)==touch_swipe_ids[0][0]){
   if(i==0)MEM32(0x1903C)^=0x10000;else if(i<4)MEM32(0x1903C)=i+1;
  }
  ++notified;last_id=MEM32(button(i)+0x10);return 1;
 }
 return 0;
}
API void reset(unsigned group,unsigned selected){
 shown_swipe=0;swipe_sequence=0;
 context=128;epoch=3;owner=1;notified=reentry=picks=toggles=0;last_id=0;
 fixture_menu=fixture_chooser=fixture_hidden_vertical=fixture_rewards=0;MEM32(0x11034)=0;MEM32(0x12000)=0xA0DE3704u;
 scene_owner=0x18100;scene_window=0x18000;town_camera=tool_handler=last_world_context=0;
 MEM32(0x18000)=0xA0E54498u;MEM32(0x18004)=win_manager;MEM32(0x18010)=0x480E8304u;MEM32(0x18058)=0x12000;MEM32(0x180E8)=3;
 MEM32(0x17404)=0xA0E53EECu;MEM32(0x17408)=win_manager;MEM32(0x17414)=0x37D82A93u;MEM32(0x1745C)=0x17000;MEM32(0x174EC)=1;
 MEM32(0xA10CC2ACu)=0x19000;MEM32(0x19000)=0xA0DCA99Cu;
 MEM32(0x1903C)=selected?selected+1:0x10002;
 MEM32(0x11044)=0x12000;MEM32(0x12004)=win_manager;MEM32(0x13000)=group?0xA0E54498u:0xA0DE3704u;MEM32(0x13004)=win_manager;MEM32(0x13010)=0x77D986A1u;MEM32(0x13058)=0x18000;MEM32(0x130E8)=3;
 MEM32(0x13400)=0xA0E54B18u;MEM32(0x13404)=win_manager;MEM32(0x13458)=0x13000;MEM32(0x134E8)=3;
 for(unsigned i=0;i<6;++i){
  uint32_t b=button(i);MEM32(b-4)=0xA0E53E14u;MEM32(b)=0xA0E53EECu;
  MEM32(b+4)=win_manager;MEM32(b+0x58)=!group&&i>0&&i<4?0x13400:0x13000;MEM32(b+0xE8)=3;
  MEM32(b+0x10)=touch_swipe_ids[group][i];MEM32(b+0xF8)=selected==i;
 }
 if(!group){MEM32(button(5)+0x10)=0x37D82A93u;MEM32(button(5)+0xE8)=1;}
}
API void navigation(unsigned c,unsigned last){
 context=c;last_world_context=last;scene_owner=0x18000;scene_window=0x18100;town_camera=0x18400;
 MEM32(scene_window)=0xA0DD89FCu;MEM32(scene_window+4)=win_manager;MEM32(town_camera)=0xA0DBAABCu;
}
API void town_tool(unsigned c,unsigned last,unsigned table){
 navigation(c,last);scene_owner=scene_window=0;tool_handler=0x18800;
 MEM32(tool_handler+0x20)=0x18900;MEM32(0x18900)=table;
}
API unsigned navigable(void){return touch_town_navigation();}
API unsigned classify(unsigned n){int i;return touch_swipe_group(button(n),&i);}
API unsigned time_state(unsigned n){return touch_time_state(button(n));}
API unsigned classify_window(unsigned w){int i;return touch_swipe_group(w,&i);}
API unsigned state_window(unsigned w){return touch_time_state(w);}
API int apply_window(unsigned w,int amount,unsigned c,uint64_t size,unsigned e){return touch_swipe_apply(w,amount,c,size,e);}
API int apply(unsigned n,int amount,unsigned c,uint64_t size,unsigned e){return touch_swipe_apply(button(n),amount,c,size,e);}
API void menu_setup(unsigned c){
 reset(0,0);context=c;fixture_menu=1;
 MEM32(0x13010)=c==512?0x17D986A3u:0x37D986A2u;MEM32(0x130E8)=3;
 const uint32_t buy[9]={0xCC607031u,0xCC607032u,0xCC607030u,0xAEu,0xCC607035u,0xCC6070AFu,0xCC607036u,0xCC607033u,0xCC607034u};
 const uint32_t build[9]={0xA5u,0xA4u,0xABu,0xAEu,0xAAu,0xAFu,0xA3u,0xA1u,0xA2u};
 for(unsigned i=0;i<9;++i){uint32_t b=button(i);MEM32(b-4)=0xA0E53E14u;MEM32(b)=0xA0E53EECu;MEM32(b+4)=win_manager;
  MEM32(b+0x58)=0x13000;MEM32(b+0x10)=c==512?build[i]:buy[i];MEM32(b+0xE8)=3;MEM32(b+0xF8)=0;}
}
API int menu_apply(int action,unsigned c,uint64_t size,unsigned e){return touch_catalog_apply(0,0,action+1,2,c,size,e);}
API void chooser_setup(unsigned table){
 menu_setup(8);fixture_chooser=1;MEM32(0x13000)=table;MEM32(0x13010)=0xCDA360CE;MEM32(0x13058)=0x18000;
 MEM32(0x18000)=0xA0E54B18;MEM32(0x18004)=win_manager;MEM32(0x18010)=0xCDA360CE;MEM32(0x18058)=0x12000;MEM32(0x180E8)=3;
 MEM32(button(0)+0x10)=0x1004;MEM32(button(1)+0x10)=0x1005;
}
API unsigned chooser_window(void){return touch_chooser_window(0x12000);}
API int chooser_scroll(int amount,unsigned c,uint64_t size,unsigned e){return touch_catalog_apply(916,278,amount,0,c,size,e);}
API unsigned classify_owner(void){int index;return touch_swipe_group(0x13000,&index);}
API unsigned vertical_band(int x,int y){return touch_vertical_band(0x12000,x,y);}
API void rewards(void){context=S2_TOUCH_BUY;fixture_rewards=1;}
API void primary_strip(unsigned id,unsigned selected){
 reset(1,selected);fixture_menu=1;
 MEM32(0x13000)=0xA0DE3D88u;MEM32(0x13010)=id;
 MEM32(0x13024)=400;MEM32(0x13028)=0;MEM32(0x1302C)=500;MEM32(0x13030)=250;
 for(unsigned n=0;n<9;++n){
  uint32_t b=button(n);
  if(n>=5 || (id==0x31D5BA3Au && n>=4))MEM32(b+0x10)=MEM32(b+0xE8)=0;
  MEM32(b+0x24)=410;MEM32(b+0x2C)=440;
  MEM32(b+0x28)=n*40;MEM32(b+0x30)=n*40+27;
 }
}
API unsigned band(int x,int y){return touch_time_band(0x12000,x,y);}
API unsigned root_clock(void){uint32_t id=0x37D82A93u;return void_call_guest_this(MEM32(MEM32(0x12000)+0x9C),0x12000,1,&id);}
API int band_scroll(int x,int y,int amount){return touch_catalog_apply(x,y,amount,0,context,((uint64_t)600<<32|1300),epoch);}
API void vertical_setup(unsigned row,unsigned selected){
 reset(1,0);context=row>=2?4:128;fixture_menu=1;
 MEM32(0x13010)=touch_vertical_ids[row][0];MEM32(0x130E8)=3;
 MEM32(0x13400)=0xA0E54B18;MEM32(0x13404)=win_manager;MEM32(0x13410)=0;MEM32(0x13458)=0x13000;MEM32(0x134E8)=3;
 MEM32(0x13024)=400;MEM32(0x13028)=0;MEM32(0x1302C)=500;MEM32(0x13030)=350;
 for(unsigned n=0;n<9;++n){
  uint32_t b=button(n),id=n<9?touch_vertical_ids[row][n+1]:0;
  MEM32(b-4)=0xA0E53E14;MEM32(b)=0xA0E53EEC;MEM32(b+4)=win_manager;MEM32(b+0x58)=row==4?0x13400:0x13000;
  MEM32(b+0x10)=id;MEM32(b+0xE8)=id?3:0;MEM32(b+0xF8)=n==selected;
  MEM32(b+0x24)=410;MEM32(b+0x2C)=440;
  MEM32(b+0x28)=(row==0?(n==4?2:n>=2?n+1:n):n)*35;MEM32(b+0x30)=MEM32(b+0x28)+30;
 }
}
API void hidden_vertical(void){fixture_hidden_vertical=1;MEM32(0x1A000)=0xA0E54B18;MEM32(0x1A004)=win_manager;MEM32(0x1A010)=0xCB980E82u;MEM32(0x1A024)=400;MEM32(0x1A028)=0;MEM32(0x1A02C)=500;MEM32(0x1A030)=350;MEM32(0x1A0E8)=0;}
API unsigned notices(void){return notified;}
API uint64_t feedback(void){return s2_touch_swipe_feedback();}
API unsigned toggle_count(void){return toggles;}
API unsigned last(void){return last_id;}
API void write32(unsigned a,unsigned v){MEM32(a)=v;}
API void change(unsigned c,unsigned e,unsigned r,unsigned o){context=c;epoch=e;reentry=r;owner=o;}
'''

def original_button_notifications(image):
    u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0xA0000000,(len(image)+4095)&~4095);u.mem_write(0xA0000000,image)
    u.mem_map(0x10000,0x40000);u.mem_map(0x90000,0x1000)
    def put(address,*values):u.mem_write(address,struct.pack('<'+'I'*len(values),*values))
    put(0x10000,0x12000,0x13000);put(0x10060,0x12345678)
    for offset,target in [(0xC0,0x90010)]:put(0x12000+offset,target)
    for offset,target in [(0x2A0,0x90020),(0xC8,0x90030),(0xC4,0x90040)]:put(0x13000+offset,target)
    notifications=[];properties=[]
    def hook(uc,va,size,data):
        counts={0x90010:1,0x90020:4,0x90030:0,0x90040:1,0xA0936AA8:1}
        if va not in counts:return
        sp=uc.reg_read(X.UC_X86_REG_ESP);n=counts[va];stack=struct.unpack('<'+'I'*(n+1),uc.mem_read(sp,4*(n+1)))
        if va==0x90010:properties.append(stack[1]);result=0xABCDEF01
        elif va==0x90020:notifications.append(stack[1:]);result=1
        else:result=0
        uc.reg_write(X.UC_X86_REG_EAX,result);uc.reg_write(X.UC_X86_REG_ESP,sp+4*(n+1));uc.reg_write(X.UC_X86_REG_EIP,stack[0])
    u.hook_add(UC_HOOK_CODE,hook)
    def call(va,args):
        put(0x40000,0x90000,*args);u.reg_write(X.UC_X86_REG_ESP,0x40000);u.reg_write(X.UC_X86_REG_ECX,0x10000)
        u.emu_start(va,0x90000,count=10000);assert u.reg_read(X.UC_X86_REG_EIP)==0x90000
    call(0xA0936F94,[1,1]);call(0xA093550A,[])
    assert struct.unpack('<I',u.mem_read(0x100FC,4))[0]&1
    assert properties==[1,1] and notifications==[(0x12345678,3,0x287259F7,0xABCDEF01),(0x12345678,3,0x287259F6,0xABCDEF01)]
    call(0xA0935541,[]);assert properties[-1]==0 and notifications[-1][2]==0x287259F0
    for speed in [2,3,4]:
        for paused in [0,1,0x100]:
            put(0x1003C,speed|(paused<<16))
            call(0xA0480CE0,[]);assert u.reg_read(X.UC_X86_REG_EAX)==speed
            call(0xA0480FE0,[]);assert u.reg_read(X.UC_X86_REG_EAX)&255==bool(paused)

def main():
    source=(ROOT/'android/host/sims2_touch.c').read_text()
    actual=source[source.index('static const uint32_t touch_swipe_ids'):source.index('static const uint32_t *touch_vertical_set')]
    actual+='\n'.join(function(source,s) for s in ('static const char *touch_type(uint32_t object)\n{','static int touch_ui_button(','static int touch_live_hud_context(','static const uint32_t *touch_vertical_set(','static int touch_swipe_group(','static int touch_swipe_owner(','static uint32_t touch_vertical_band(','static int touch_time_index(','static unsigned touch_time_state(','static uint32_t touch_time_band(','static int touch_swipe_apply(','static int touch_town_navigation(','static int touch_catalog_apply(', 'uint64_t s2_touch_swipe_feedback('))
    pe=pefile.PE(str(ROOT/'generated/input/Sims2EP9.exe'));image=pe.get_memory_mapped_image()
    for table,name in [(0xA0E54B18,b'.?AVcGZWinFlatRect@@'),(0xA0DE3D88,b'.?AVcTSWinFlatRectTransp@@')]:
        locator=struct.unpack_from('<I',image,table-pe.OPTIONAL_HEADER.ImageBase-4)[0]
        descriptor=struct.unpack_from('<I',image,locator-pe.OPTIONAL_HEADER.ImageBase+12)[0]
        assert image[descriptor-pe.OPTIONAL_HEADER.ImageBase+8:].split(b'\0',1)[0]==name
    original_button_notifications(image)
    size=600<<32|1300
    with tempfile.TemporaryDirectory(prefix='s2-widget-') as folder:
        base=Path(folder);c=base/'test.c';obj=base/'test.obj';dll=base/'test.dll'
        c.write_text(PRELUDE+actual+CHECK)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin','-fno-stack-protector','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll),str(obj)],check=True)
        lib=ctypes.CDLL(str(dll));lib.image_data.restype=ctypes.c_void_p;lib.last.restype=ctypes.c_uint;lib.feedback.restype=ctypes.c_uint64
        lib.apply.argtypes=[ctypes.c_uint,ctypes.c_int,ctypes.c_uint,ctypes.c_uint64,ctypes.c_uint]
        lib.apply_window.argtypes=lib.apply.argtypes
        lib.menu_apply.argtypes=[ctypes.c_int,ctypes.c_uint,ctypes.c_uint64,ctypes.c_uint]
        lib.chooser_scroll.argtypes=lib.menu_apply.argtypes
        try:
            ctypes.memmove(lib.image_data(),image,len(image))
            ids=[[0x27D7258F,0x57E01CC1,0x37E01CC3,0x47E01CC5],[0x0D8AB291,0x17DEFBF5,0x57DEFBF4,0x47DEFBF2,0x8B85BA4E,0x8B85BADD]]
            resources=dbpf.Package(GAME/'EP9/TSData/Res/UI/ui.package')
            descriptions='\n'.join(resources.read(e).decode('latin1') for e in resources.entries if e['type']==0)
            # IDs are checked against original UI records, not only copied constants in this fixture.
            for group in ids:
                for identifier in group:
                    assert re.search(r'clsid=GZWinBtn[^\n]*id=0x'+f'{identifier:08x}'+r'\b',descriptions,re.I),hex(identifier)
            for group,count in [(0,4),(1,6)]:
                for selected in range(count):
                    for amount in [-8,-1,1,8]:
                        lib.reset(group,selected);assert lib.classify(selected)==group+1
                        assert lib.apply(0,amount,128,size,3)==1
                        target=max(0,min(count-1,selected+amount))
                        assert lib.notices()==(target!=selected)
                        if target!=selected:
                            assert lib.last()==ids[group][target]
                            receipt=lib.feedback();assert receipt & 0xffffffff == ids[group][target]
                            assert (receipt>>32)&15 == target+1 and (receipt>>36)&15 == count
                            assert (receipt>>40)&15 == group
                            lib.change(128|1024,3,0,1);assert lib.feedback()==receipt
                            lib.change(128,4,0,1);assert not lib.feedback()
                        else: assert not lib.feedback(), 'a clamped swipe has no success notice'
            for row,count in [(0,5),(1,4),(2,7),(3,7),(4,7)]:
                for selected in range(count):
                    lib.vertical_setup(row,selected)
                    context=4 if row>=2 else 128
                    assert lib.classify(selected)==4
                    assert not lib.classify_owner(), "catalog contents outside the narrow strip keep their own gestures"
                    assert lib.vertical_band(420,10)!=0, (row,selected)
                    assert not lib.vertical_band(450,10)
                    assert not lib.vertical_band(420,349)
                    assert lib.apply(selected,1,context,size,3)==1
                    assert lib.toggle_count()<=1
            lib.vertical_setup(4,0);lib.hidden_vertical()
            assert lib.vertical_band(420,10), "a hidden genetics component cannot cover the active cosmetics strip"
            for mutate in [(0x130E8,0),(0x180E8,0),(0x13004,0),(0x13058,0)]:
                lib.vertical_setup(0,0);lib.write32(*mutate)
                assert not lib.vertical_band(420,10), mutate
            for reenter in [1,2,3,4]:
                lib.vertical_setup(0,0);lib.change(128,3,reenter,1)
                assert not lib.vertical_band(420,10), reenter
            lib.vertical_setup(0,1)
            assert lib.apply(1,1,128,size,3)==1 and lib.last()==0x80000000, 'sort the actual tabs by Y, including Hobbies'
            lib.vertical_setup(0,1);lib.change(256,3,0,1)
            assert not lib.vertical_band(420,65), 'ordinary Buy leaves Live tabs inactive'
            lib.rewards()
            assert lib.vertical_band(420,65), 'the gap in the Live tab strip remains active in Rewards'
            assert lib.band_scroll(420,65,1) and lib.last()==0x80000000
            lib.change(256|1,3,0,1)
            assert not lib.vertical_band(420,65), 'a modal owns touches even in Rewards'
            lib.vertical_setup(2,0);lib.change(4|1024,3,0,1)
            assert lib.classify(0)==4 and lib.apply(0,1,4|1024,size,3)==1, 'CAS tabs stay usable while the name field owns focus'
            lib.vertical_setup(0,0);lib.change(128|1024,3,0,1)
            assert lib.classify(0)==0, 'text focus does not enable arbitrary LIVE controls'
            lib.vertical_setup(0,0);lib.write32(0x13010,0xDEADBEEF)
            assert lib.classify(0)==0 and not lib.apply(0,1,128,size,3), 'a matching ID in the wrong panel must not activate'
            for selected in range(4):
                lib.reset(0,selected);assert lib.time_state(5)==1024|(selected<<11)
            lib.reset(0,2);lib.write32(0x14004+0xF8,1);assert lib.time_state(5)==1024|(2<<11), 'a stale pause toggle cannot change actual speed'
            lib.reset(0,2);lib.write32(0x1903C,0x10003);lib.write32(0x14004+0xF8,0)
            assert lib.time_state(5)==1024 and lib.apply(5,-1,128,size,3)==1 and not lib.notices(), 'already paused with a stale Normal toggle must stay paused'
            for address,value in [(0xA10CC2AC,0),(0x19000,0),(0x1903C,1)]:
                lib.reset(0,1);lib.write32(address,value)
                assert not lib.time_state(5) and not lib.apply(5,1,128,size,3) and not lib.notices()
            # Disabled/hidden variants are skipped; the clock uses selected speed, not its own toggle.
            lib.reset(0,1);lib.write32(0x14004+0x400+0x10,0x37D82A93);assert lib.classify(1)==1
            for selected in range(4):
                for amount in [-1,1]:
                    lib.reset(0,selected);assert lib.classify(5)==1 and lib.apply(5,amount,128,size,3)==1
                    target=max(0,min(3,selected+amount))
                    assert lib.notices()==(target!=selected)
                    if target!=selected:assert lib.last()==ids[0][target]
            # Captured native Pause and last Normal/High/Ultra can be toggled simultaneously.
            for previous_speed in range(1,4):
                for origin in [0,previous_speed,5]:
                    for amount in [-1,1]:
                        lib.reset(0,0);lib.write32(0x14004+previous_speed*0x400+0xF8,1)
                        assert lib.apply(origin,amount,128,size,3)==1
                        assert lib.notices()==(amount>0)
                        if amount>0:assert lib.last()==ids[0][1]
            for address,value in [(0x13000,0),(0x13010,0),(0x13400,0),(0x13404,0),(0x13458,0),(0x134E8,0)]:
                lib.reset(0,0);lib.write32(address,value);assert not lib.apply(1,1,128,size,3) and not lib.notices()
            lib.reset(1,0);lib.write32(0x14004+0x400+0xE8,1);assert lib.apply(0,1,128,size,3)==1
            assert lib.last()==0x57DEFBF4
            for reentry in range(1,10):
                lib.reset(0,0);lib.change(128,3,reentry,1);assert lib.apply(0,1,128,size,3)==0 and not lib.notices()
                assert lib.toggle_count()==(reentry>=5)
            for ctx,e,o in [(129,3,1),(128,4,1),(128,3,0)]:
                lib.reset(0,0);lib.change(ctx,e,0,o);assert not lib.apply(0,1,128,size,3) and not lib.notices()
            for field,value in [(0,0),(4,0),(0x58,0),(0xE8,1),(0x10,0)]:
                lib.reset(0,0);lib.write32(0x14004+field,value);assert not lib.apply(0,1,128,size,3) and not lib.notices()
            # Every point in the native vertical holder, including inactive tabs and gaps, owns the same selected group.
            for holder in [0xA94E97E8,0xA94E97F4]:
                for table in [0xA0E54B18,0xA0DE3D88]:
                    lib.reset(1,2);lib.write32(0x13000,table);lib.write32(0x13010,holder);lib.write32(0x130E8,3)
                    assert lib.classify_window(0x13000)==2 and lib.apply_window(0x13000,1,128,size,3)
                    assert lib.last()==ids[1][3]
            for holder,count in [(0xA94E97E8,5),(0xA94E97F4,5),(0x31D5BA3A,4)]:
                lib.primary_strip(holder,count-1)
                if count==5:lib.rewards()
                for selected in reversed(range(1,count)):
                    assert lib.vertical_band(420,selected*40-5), (holder,selected)
                    assert lib.band_scroll(420,selected*40-5,-1)
                    assert lib.last()==ids[1][selected-1]
                assert lib.notices()==count-1 and lib.band_scroll(420,12,-1) and lib.notices()==count-1
                assert not lib.vertical_band(450,52)
                lib.write32(0x130E8,0)
                assert not lib.vertical_band(420,52)
            for age in [0x2E9AF844,0x2E9AF849]:
                for table in [0xA0E54B18,0xA0DE3D88]:
                    lib.reset(0,1);lib.write32(0x15404,table);lib.write32(0x15414,age);lib.write32(0x1545C,0x13400)
                    assert lib.classify_window(0x15404)==1 and lib.state_window(0x15404)==1024|(1<<11)
                    assert lib.apply_window(0x15404,1,128,size,3) and lib.last()==ids[0][2]
            # Global native extents cover age/date/speed plus gaps, without fixed display coordinates.
            for scale,offset in [(1,0),(2,17),(3,101)]:
                lib.reset(0,1)
                rects=[(170,257,190,277),(192,257,212,277),(211,257,241,277),(237,257,277,277),(185,191,295,231),(180,237,287,257)]
                lib.write32(0x15004,0xA0DE3D88);lib.write32(0x15014,0x2E9AF844);lib.write32(0x1505C,0x13400)
                for n,rect in enumerate(rects):
                    for field,value in zip([0x24,0x28,0x2C,0x30],rect):lib.write32(0x14004+n*0x400+field,value*scale+offset)
                for x in range(170,295,17):
                    for y in range(191,277,11):assert lib.band(x*scale+offset,y*scale+offset)==0x15404,(scale,x,y)
                for x,y in [(169,200),(295,200),(180,190),(180,277),(500,250)]:assert not lib.band(x*scale+offset,y*scale+offset)
                assert lib.band_scroll(180*scale+offset,233*scale+offset,1) and lib.last()==ids[0][2]
                lib.write32(0x150EC,0);assert not lib.band(190*scale+offset,200*scale+offset)
                lib.write32(0x154EC,0);assert not lib.band(190*scale+offset,240*scale+offset)
            # Exact physical Redmi age/time extents: the age holder uses TSWinFlatRectTransp.
            lib.reset(0,1);lib.write32(0x15004,0xA0DE3D88);lib.write32(0x15014,0x2E9AF844);lib.write32(0x1505C,0x13400)
            for n,rect in [(4,(190,505,300,545)),(5,(185,551,292,571)),(2,(216,571,246,591))]:
                for field,value in zip([0x24,0x28,0x2C,0x30],rect):lib.write32(0x14004+n*0x400+field,value)
            assert lib.root_clock()==0x17404, 'the unrestricted root namespace contains a conflicting clock'
            assert lib.band(222,528)==0x15404 and lib.band(222,548)==0x15404
            for address,wrong,original in [(0x18010,0,0x480E8304),(0x18058,0,0x12000),(0x180E8,0,3),
                                           (0x18004,0,0x11000),(0x13058,0,0x18000),(0x1545C,0,0x13000)]:
                lib.write32(address,wrong);assert not lib.band(222,548)
                lib.write32(address,original);assert lib.band(222,548)==0x15404
            assert lib.band_scroll(222,528,-1) and lib.last()==ids[0][0]
            # Chooser swipes invoke original Previous/Next arrows, never thumbnails or a keyboard guess.
            for direction,target in [(-1,0x1005),(1,0x1004)]:
                lib.menu_setup(8);lib.write32(0x13010,0xCDA360CE);lib.write32(0x14014,0x1004);lib.write32(0x14414,0x1005)
                assert lib.classify_window(0x13000)==3 and lib.apply_window(0x13000,direction,8,size,3)
                assert lib.last()==target and not lib.toggle_count()
                for table in [0xA0DE3704,0xA0E54498]:
                    lib.chooser_setup(table);assert lib.chooser_window()==0x13000
                    assert lib.chooser_scroll(direction,8,size,3) and lib.last()==target
            for address,value in [(0x13058,0),(0x18058,0),(0x13000,0),(0x180E8,0),(0x130E8,0),(0x1445C,0),(0x14414,0),(0x14408,0)]:
                lib.chooser_setup(0xA0E54498);lib.write32(address,value)
                assert not lib.chooser_window()
            menu_ids=[[0xCC607031,0xCC607032,0xCC607030,0xAE,0xCC607035,0xCC6070AF,0xCC607036,0xCC607033,0xCC607034],
                      [0xA5,0xA4,0xAB,0xAE,0xAA,0xAF,0xA3,0xA1,0xA2]]
            for build,mode in enumerate([256,512]):
                for action,identifier in enumerate(menu_ids[build]):
                    assert re.search(r'clsid=GZWinBtn[^\n]*id=0x'+f'{identifier:08x}'+r'\b',descriptions,re.I),hex(identifier)
                    lib.menu_setup(mode);assert lib.menu_apply(action,mode,size,3)
                    assert lib.last()==identifier and lib.toggle_count()==(action<7 and not(build and action==4))
                    lib.menu_setup(mode);lib.write32(0x13000,0xA0E54498);assert lib.menu_apply(action,mode,size,3) and lib.last()==identifier
                for action in [-1,9]:
                    lib.menu_setup(mode);assert not lib.menu_apply(action,mode,size,3) and not lib.notices()
                for reentry in range(1,10):
                    lib.menu_setup(mode);lib.change(mode,3,reentry,1)
                    assert not lib.menu_apply(0,mode,size,3) and not lib.notices()
                for address,value in [(0x11034,1),(0x130E8,0),(0x13004,0),(0x140EC,1),(0x1405C,0),(0x14014,0)]:
                    lib.menu_setup(mode);lib.write32(address,value)
                    assert not lib.menu_apply(0,mode,size,3) and not lib.notices()
                lib.menu_setup(mode);assert not lib.menu_apply(0,mode,size,4)
            lib.reset(0,0);assert not lib.apply(0,1,128,size+1,3)
            assert not lib.apply(0,9,128,size,3) and not lib.apply(0,0,128,size,3)
            for c in [0,64,1024,1088]:
                lib.reset(0,0);lib.navigation(c,64);assert lib.navigable()
            for c in [1,2,4,8,16,32,128,256,512,66]:
                lib.reset(0,0);lib.navigation(c,64);assert not lib.navigable()
            for last in [0,128,256,512]:
                lib.reset(0,0);lib.navigation(0,last);assert not lib.navigable()
            for address in [0x18100,0x18104,0x18400]:
                lib.reset(0,0);lib.navigation(0,64);lib.write32(address,0);assert not lib.navigable()
            # Runtime Town has no attached scene; its active original Neighborhood tool owns navigation.
            for c in [0,64,1024,1088]:
                lib.reset(0,0);lib.town_tool(c,64,0xA0DE9084);assert lib.navigable()
            for c in [1,2,4,8,16,32,128,256,512,66]:
                lib.reset(0,0);lib.town_tool(c,64,0xA0DE9084);assert not lib.navigable()
            for table in [0,0xA0E53EEC,0xA0DE9374]:
                lib.reset(0,0);lib.town_tool(0,64,table);assert not lib.navigable()
            print('PASS native widget swipes: original EP9 button RTTI/IDs, time+vertical tabs, selected base, clamp/reversal, disabled variants, exact original notification, owner/layout/context and callback reentry guards')
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.FreeLibrary(lib._handle)

if __name__=='__main__':main()
