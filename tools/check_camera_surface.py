"""Check the actual lot surface adapter; traversal is an explicit original-engine boundary."""
import argparse
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_camera_pan import original_camera
from check_virtual_query import function

PRELUDE = r'''
typedef unsigned char uint8_t;typedef unsigned int uint32_t;typedef unsigned long long size_t;
#define API __declspec(dllexport)
#define isfinite(x) __builtin_isfinite(x)
int _fltused;
enum {S2_TOUCH_LIVE=128,S2_TOUCH_BUY=256,S2_TOUCH_BUILD=512};
static unsigned context=S2_TOUCH_LIVE,invalid,bad,calls,changed,found=1;
static float distance=10;
static uint32_t g_esp=0x1F000;static uint8_t arena[0x20000];static uint32_t globals[3];
#define GPTR(a) (arena+(uint32_t)(a))
#define MEM8(a) (*GPTR(a))
#define MEM32(a) (*(uint32_t *)((uint32_t)(a)>=0xA10CC458u&&(uint32_t)(a)<0xA10CC464u?(uint8_t *)globals+(uint32_t)(a)-0xA10CC458u:GPTR(a)))
static void *memcpy(void *d,const void *s,size_t n){uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d;}
static unsigned s2_touch_context(void){return context;}
static int void_vm_writable(uint32_t a,uint32_t n){return a!=invalid&&a>=0x10000&&(size_t)a+n<=sizeof arena;}
static int void_vm_readable(uint32_t a,uint32_t n){return a!=invalid&&((a>=0xA10CC458u&&(size_t)a+n<=0xA10CC464u)||void_vm_writable(a,n));}
static float touch_float(uint32_t a){float v;memcpy(&v,GPTR(a),4);return v;}
static float (*rootf)(float);static float sqrtf(float v){return rootf(v);}
static void (*make_ray)(void *,const void *,const void *);
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 ++calls;
 if(va==0xA018E720u){if(calls!=1||self!=0x1EFC0||g_esp!=self||n!=2||args[0]!=self+32||args[1]!=self+44)bad=1;
   make_ray(GPTR(self),GPTR(args[0]),GPTR(args[1]));return self;}
 if(va==0xA0A4EFD0u){if(calls!=2||self!=0x13000||g_esp!=0x1EFC0||n!=3||args[0]!=0x1EFF8||args[1]!=0x15000||args[2]!=0x1EFC0||MEM32(args[0])!=0x7F7FFFFF)bad=1;
   memcpy(GPTR(args[0]),&distance,4);if(changed==1)context=S2_TOUCH_BUY;if(changed==2)globals[0]=0;
   if(changed==3)globals[2]=0;if(changed==4)MEM32(0x12008)=0;if(changed==5)MEM32(0x11098)=0;if(changed==6)MEM32(0x14024)=0;
   return found;}
 if(va==0xA018E860u){if(calls!=3||self!=0x1EFC0||g_esp!=self||n||args)bad=1;MEM32(self)=0xA0DA1304;return 0;}
 bad=1;return 0;
}
'''
CHECK = r'''
API void setup(float (*sqrtfn)(float),void (*rayfn)(void *,const void *,const void *)){
 rootf=sqrtfn;make_ray=rayfn;context=S2_TOUCH_LIVE;invalid=bad=calls=changed=0;found=1;distance=10;g_esp=0x1F000;
 globals[0]=0x11000;globals[2]=0x12000;MEM32(0x11000)=0xA0DAE264;MEM32(0x11098)=0x14000;
 MEM32(0x12000)=0xA0DB5344;MEM32(0x12008)=0x13000;MEM32(0x13000)=0xA0E6D0B4;
 MEM8(0x13098)=0;
 MEM32(0x14000)=0xA0DB56D0;MEM32(0x14024)=0x15000;MEM32(0x15000)=0xA0DB5654;
}
static float answer;
API int apply(const float *eye,const float *ray){answer=12345;int ok=touch_lot_surface(eye,ray,&answer);if(g_esp!=0x1F000||(!ok&&answer!=12345))bad=1;return bad?-1:ok;}
API float result(void){return answer;}
API unsigned count(void){return calls;}
API void word(unsigned a,unsigned v){MEM32(a)=v;}
API void unreadable(unsigned a){invalid=a;}
API void mode(unsigned v){context=v;}
API void response(unsigned yes,float value,unsigned mutation){found=yes;distance=value;changed=mutation;}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'android/host/sims2_touch.c')
    args=parser.parse_args()
    actual=function(args.source.read_text(),'static int touch_lot_surface(')
    oracle=original_camera();rng=random.Random(20261109)
    root_type=C.CFUNCTYPE(C.c_float,C.c_float)
    ray_type=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p,C.c_void_p)
    sqrtfn=root_type(math.sqrt);constructor_cases=0
    def original_ray(out,origin,direction):
        nonlocal constructor_cases
        eye=struct.unpack('<3f',C.string_at(origin,12));ray=struct.unpack('<3f',C.string_at(direction,12))
        assert abs(sum(v*v for v in ray)-1)<.000001
        data=oracle.ray(eye,ray)
        assert data==struct.pack('<I6fI',0xA0DA139C,*eye,*ray,0)
        C.memmove(out,data,32);constructor_cases+=1
    rayfn=ray_type(original_ray)
    with tempfile.TemporaryDirectory(prefix='s2-camera-surface-') as folder:
        base=Path(folder);c=base/'test.c';obj=base/'test.obj';dll=base/'test.dll'
        c.write_text(PRELUDE+actual+CHECK)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin','-fno-stack-protector','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll),str(obj)],check=True)
        lib=C.CDLL(str(dll));ptr=C.POINTER(C.c_float)
        lib.setup.argtypes=[root_type,ray_type];lib.apply.argtypes=[ptr,ptr];lib.result.restype=C.c_float
        lib.response.argtypes=[C.c_uint,C.c_float,C.c_uint]
        setup=lambda:lib.setup(sqrtfn,rayfn)
        def apply(eye=(10,20,30),ray=(.5,-.2,-2)):
            return lib.apply((C.c_float*3)(*eye),(C.c_float*3)(*ray))
        try:
            for _ in range(120):
                setup();eye=[rng.uniform(-100,100)for _ in range(3)];ray=[rng.uniform(-3,3)for _ in range(3)]
                dist=rng.uniform(.01,1000);lib.response(1,dist,0)
                assert apply(eye,ray)==1 and lib.count()==3
                expected=eye[2]+dist*ray[2]/math.sqrt(sum(v*v for v in ray))
                assert abs(lib.result()-expected)<.00025,(lib.result(),expected)
            negatives=0
            for address in [0xA10CC458,0xA10CC460,0x11000,0x12000,0x12008,0x11098,0x13000,0x14000,0x14024,0x15000]:
                setup();lib.word(address,0);assert apply()==0 and lib.count()==0;negatives+=1
            setup();lib.word(0x13098,1);assert apply()==0 and lib.count()==0;negatives+=1
            for address in [0xA10CC458,0x11000,0x12000,0x13000,0x14000,0x15000,0x1EFC0]:
                setup();lib.unreadable(address);assert apply()==0 and lib.count()==0;negatives+=1
            for mode in [0,1,2,64,1024]:
                setup();lib.mode(mode);assert apply()==0 and lib.count()==0;negatives+=1
            for eye,ray in [((float('nan'),0,0),(0,0,-1)),((0,0,0),(float('inf'),0,-1)),((0,0,0),(0,0,0))]:
                setup();assert apply(eye,ray)==0 and lib.count()==0;negatives+=1
            for found,dist,changed in [(0,10,0),(1,0,0),(1,-1,0),(1,1e7,0),(1,float('nan'),0)]+[(1,10,m)for m in range(1,7)]:
                setup();lib.response(found,dist,changed);assert apply()==0 and lib.count()==3;negatives+=1
            for mode in [128,256,512]:
                setup();lib.mode(mode);assert apply()==1
            receipt=dict(passed=True,adapter_cases=120,negative_cases=negatives,original_x86_ray_constructor_and_destructor_cases=constructor_cases,
                         source=str(args.source),source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
                         traversal_boundary='modeled root A0A4EFD0, real scene traversal requires physical calibration')
            out=ROOT/'build/20261109-poco/camera-surface-oracle.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(receipt,indent=2)+'\n')
            print(json.dumps(receipt,indent=2))
        finally:
            C.windll.kernel32.FreeLibrary.argtypes=[C.c_void_p];C.windll.kernel32.FreeLibrary(lib._handle)

if __name__=='__main__':main()
