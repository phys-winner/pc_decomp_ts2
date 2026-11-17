"""Exercise production small-target selection against deterministic visible pick regions."""
import ctypes
import _ctypes
from contextlib import ExitStack
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function

PRELUDE = r'''
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned char uint8_t;
#define NULL ((void *)0)
#define S2_TOUCH_LIVE 128
#define isfinite(v) __builtin_isfinite(v)
#define API __declspec(dllexport)
int _fltused;
static uint32_t memory[0x10000/4],g_esp=0x9000,win_manager=0x1000,tool_handler=0x3000,scene_window=0x2100,lot_camera=0xa000;
#define GPTR(a) ((uint8_t *)memory+(uint32_t)(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
static unsigned context=128,epoch=3,calls,ui,mutation,writable=1;
static unsigned target_logs=32;
static void void_log(const char *s,...){(void)s;}
static int boxes[4][4],render_boxes[4][4],active[4],children[4],siblings[4],has_bounds[4],parents[4],object_node[4];
static void *memset(void *p,int c,uint64_t n){uint8_t *b=p;while(n--)*b++=(uint8_t)c;return p;}
static void *memcpy(void *p,const void *s,uint64_t n){uint8_t *b=p;const uint8_t *a=s;while(n--)*b++=*a++;return p;}
static float touch_float(uint32_t a){float v;memcpy(&v,GPTR(a),4);return v;}
static float tanf(float v){(void)v;return 1;}
static int touch_camera_basis(uint32_t c,float d,float eye[3],float matrix[9]){
 (void)c;(void)d;memset(eye,0,12);memset(matrix,0,36);matrix[0]=matrix[4]=matrix[8]=1;return 1;
}
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a-*b;}
static const char *touch_type(uint32_t p){(void)p;return "";}
static int void_vm_readable(uint32_t p,uint32_t n){return p>=0x1000&&(uint64_t)p+n<=sizeof memory;}
static int void_vm_writable(uint32_t p,uint32_t n){return writable&&void_vm_readable(p,n);}
static unsigned s2_touch_context(void){return context;}
static unsigned s2_touch_layout_epoch(void){return epoch;}
static uint32_t s2_touch_owner_manager(void){return win_manager;}
static void void_input_screen_size(int *w,int *h){*w=*h=200;}
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 ++calls;if(mutation&&calls==mutation)context=1;
 if(self==0x2000&&n==2)return ui&&args[0]>=103?0x2200:scene_window;
 if(self>=0x7000&&self<0x7400){
  unsigned i=(self-0x7000)/0x100;
  if(va==6)return parents[i]?0x7000+0x100*(parents[i]-1):0;
  if(va==7){if(n!=1||args[0]!=0xFA1C39EDu)__builtin_trap();return object_node[i];}
  if(va==1)return children[i]?0x7000+0x100*(children[i]-1):0;
  if(va==2)return siblings[i]?0x7000+0x100*(siblings[i]-1):0;
  if(va==3)return has_bounds[i];if(va==4)return 0;
  if(va!=5||n!=1)__builtin_trap();
  float box[6]={(float)render_boxes[i][0]-100,100,99-(float)render_boxes[i][3],(float)render_boxes[i][2]-99,100,100-(float)render_boxes[i][1]};
  memcpy(GPTR(args[0]),box,24);return args[0];
 }
 if(va!=0xA078D789u||self!=0x4000||n!=5||args[2]||args[4]||args[3]!=0x8fe0)__builtin_trap();
 int x=args[0],y=args[1];
 for(unsigned i=0;i<4;++i)if(active[i]&&x>=boxes[i][0]&&x<=boxes[i][2]&&y>=boxes[i][1]&&y<=boxes[i][3])return 0x7000+i*0x100;
 return 0;
}
'''
CHECK = r'''
API void reset(void){
 memset(memory,0,sizeof memory);memset(active,0,sizeof active);memset(children,0,sizeof children);memset(siblings,0,sizeof siblings);memset(parents,0,sizeof parents);context=128;epoch=3;calls=ui=mutation=0;writable=1;
 win_manager=0x1000;tool_handler=0x3000;g_esp=0x9000;
 MEM32(0x1044)=0x2000;MEM32(0x2000)=0x5000;MEM32(0x3020)=0x4000;MEM32(0x4000)=0xA0DE9374u;
 MEM32(0xa134)=0x3fc90fdb;MEM32(0xa0fc)=0x3f800000;MEM32(0xa0e0)=0x42c80000;
 for(unsigned i=0;i<4;++i){MEM32(0x7000+i*0x100)=0x6000;has_bounds[i]=object_node[i]=1;}
 MEM32(0x6064)=7;MEM32(0x6088)=6;
 MEM32(0x609c)=1;MEM32(0x6094)=2;MEM32(0x60c0)=3;MEM32(0x6118)=4;MEM32(0x6120)=5;
}
API void bounds(unsigned n,int x,int y,int xx,int yy){render_boxes[n][0]=x;render_boxes[n][1]=y;render_boxes[n][2]=xx;render_boxes[n][3]=yy;}
API void box(unsigned n,int x,int y,int xx,int yy){active[n]=1;boxes[n][0]=x;boxes[n][1]=y;boxes[n][2]=xx;boxes[n][3]=yy;bounds(n,x,y,xx,yy);}
API void branch(unsigned n,unsigned child,unsigned sibling){children[n]=child;siblings[n]=sibling;if(child)has_bounds[n]=0;}
API void attachment(unsigned n,unsigned parent,unsigned object){parents[n]=parent;object_node[n]=object;}
API void anchor(unsigned n,int x,int y){bounds(n,x,y,x-1,y-1);has_bounds[n]=1;}
API void change(unsigned c,unsigned u,unsigned w,unsigned m){context=c;ui=u;writable=w;mutation=m;}
API uint64_t resolve(int x,int y,int radius){
 touch_small_target(0x4000,0x2000,&x,&y,radius);
 if(g_esp!=0x9000)__builtin_trap();
 return(uint64_t)(uint32_t)y<<32|(uint32_t)x;
}
API unsigned queries(void){return calls;}
'''


def main():
    source = (ROOT / 'android/host/sims2_touch.c').read_text()
    actual = ''.join(function(source, s) for s in ('static uint32_t touch_target_node(', 'static int touch_target_bounds(', 'static void touch_small_target('))
    with tempfile.TemporaryDirectory(prefix='s2-small-targets-') as folder, ExitStack() as cleanup:
        base = Path(folder)
        c, obj, dll = base / 'check.c', base / 'check.obj', base / 'check.dll'
        c.write_text(PRELUDE + actual + CHECK)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin', '-fno-stack-protector', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry', '/out:' + str(dll), str(obj)], check=True)
        lib = ctypes.CDLL(str(dll))
        cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.resolve.restype = ctypes.c_uint64
        def resolve(x=100, y=100, radius=12):
            point = lib.resolve(x, y, radius)
            return point & 0xffffffff, point >> 32
        for width, height in [(1, 1), (3, 5), (7, 8)]:
            lib.reset(); lib.box(0, 106, 98, 105 + width, 97 + height)
            target = resolve()
            assert 106 <= target[0] <= 105 + width and 98 <= target[1] <= 97 + height, (width, height, target)
            if width >= 3 and height >= 3:
                assert 106 < target[0] < 105 + width and 98 < target[1] < 97 + height, 'avoid the original action handler\'s inactive rim'
            assert lib.queries() < 1000
        for width, height in [(50, 50), (60, 2), (2, 60), (18, 18)]:
            lib.reset(); lib.box(0, 106, 98, 105 + width, 97 + height)
            assert resolve() == (100, 100), (width, height)
        lib.reset(); lib.box(0, 99, 99, 160, 160)
        assert resolve() == (100, 100) and lib.queries() == 2, 'an exact hit always wins'
        lib.reset(); lib.box(0, 106, 98, 108, 102);lib.bounds(1,106,98,108,102);lib.bounds(2,140,70,180,130)
        lib.branch(0,2,0);lib.branch(1,0,3)
        assert resolve()==(100,100), 'all model pieces exclude the tiny mouse of a large computer'
        lib.reset(); lib.box(0,106,98,108,102);lib.bounds(3,140,70,180,130)
        lib.attachment(0,2,1);lib.attachment(1,3,0);lib.attachment(2,0,1)
        lib.branch(2,2,0);lib.branch(1,1,0);lib.branch(0,0,4)
        assert resolve()==(100,100), 'an animated part is measured with the outer object and its other pieces'
        lib.reset();lib.box(0,106,98,108,102);lib.bounds(1,106,98,108,102)
        lib.branch(0,2,0);lib.anchor(0,190,190)
        assert resolve()==(107,100), 'a distant invisible object origin does not enlarge the model'
        lib.reset();lib.box(0,106,98,108,102);lib.anchor(0,106,100)
        assert resolve()==(100,100), 'an origin without visible geometry is not a small target'
        lib.reset(); lib.box(0, 106, 98, 108, 102); lib.box(1, 94, 108, 96, 110)
        assert resolve()[0] >= 106, 'nearest small target wins'
        lib.reset(); lib.box(0, 106, 98, 108, 102); lib.change(128, 1, 1, 0)
        assert resolve() == (100, 100), 'UI retains input ownership'
        lib.reset(); lib.box(0, 0, 98, 2, 102)
        assert resolve(5, 100)[0] <= 2, 'screen boundary stays in range'
        for context, writable, radius in [(1, 1, 12), (256, 1, 12), (128, 0, 12), (128, 1, 0), (128, 1, 49)]:
            lib.reset(); lib.box(0, 106, 98, 108, 102); lib.change(context, 0, writable, 0)
            assert resolve(radius=radius) == (100, 100) and lib.queries() == 0
        lib.reset(); lib.box(0, 106, 98, 108, 102); lib.change(128, 0, 1, 3)
        assert resolve() == (100, 100), 'owner callbacks cannot retarget stale input'
    print('PASS small targets: projected whole-model bounds including disconnected pieces, tiny regions, exact-hit priority, nearest selection, large/long exclusions, UI/screen boundaries, owner/context/scratch validation and stack preservation')


if __name__ == '__main__':
    main()
