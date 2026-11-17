"""Verify production ground-ray pan with original x86 camera poses and independent reprojection."""
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as X
from build_android import CLANG, ROOT
from check_virtual_query import function

PRELUDE=r'''
typedef unsigned char uint8_t;typedef unsigned int uint32_t;typedef unsigned long long uint64_t,size_t;
#define API __declspec(dllexport)
#define UINT32_MAX 0xFFFFFFFFu
int _fltused;
#define isfinite(x) __builtin_isfinite(x)
static uint8_t arena[0x20000];static uint32_t g_esp=0x1F000;static unsigned bad,pose_calls,matrix_calls;
#define GPTR(a) (arena+(uint32_t)(a))
enum {S2_TOUCH_TOWN=64,S2_TOUCH_LIVE=128,S2_TOUCH_BUY=256,S2_TOUCH_BUILD=512};static unsigned fixture_context,fixture_epoch;static uint32_t fixture_system;
static unsigned s2_touch_context(void){return fixture_context;}
static unsigned s2_touch_layout_epoch(void){return fixture_epoch;}
static int lot_enabled;static float lot_height;static unsigned lot_calls;
static int touch_lot_surface(const float eye[3],const float ray[3],float *height){(void)eye;(void)ray;++lot_calls;if(!lot_enabled)return 0;*height=lot_height;return 1;}
#define MEM32(a) ((uint32_t)(a)==0xA10CC458u?fixture_system:*(uint32_t *)GPTR(a))
static void *memcpy(void *d,const void *s,size_t n){uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d;}
static int void_vm_writable(uint32_t a,uint32_t n){return a>=0x10000&&(uint64_t)a+n<=sizeof arena;}
static int void_vm_readable(uint32_t a,uint32_t n){return (a==0xA10CC458u&&n==4)||void_vm_writable(a,n);}
static float (*fixture_tanf)(float);
static float (*fixture_height)(float,float);static float height_result;static unsigned height_calls;
static double fp_pop(void){return height_result;}
static float tanf(float x){return fixture_tanf(x);}
static float original_eye[3],original_quaternion[4],original_matrix[9],expected_distance;
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0xA02A0660u){++height_calls;if(self!=0x13008||n!=2||!fixture_height){bad=1;return 0;}
  height_result=fixture_height(*(float *)args,*(float *)(args+1));return 0;}
 ++pose_calls;if(va!=0xA0B4CEC0u||self!=0x10000||n!=4||args[3]!=1||args[2]!=*(uint32_t *)&expected_distance)bad=1;
 if(!void_vm_writable(args[0],12)||!void_vm_writable(args[1],16)||args[0]==args[1]){bad=1;return 0;}
 memcpy(GPTR(args[0]),original_eye,12);memcpy(GPTR(args[1]),original_quaternion,16);return 1;
}
static uint32_t void_call_guest(uint32_t va,int n,const uint32_t *args,int pops){
 ++matrix_calls;if(va!=0xA01A77F0u||n!=2||pops)bad=1;
 if(!void_vm_writable(args[0],36)||!void_vm_writable(args[1],16)){bad=1;return 0;}
 for(unsigned i=0;i<16;++i)if(*(uint8_t *)GPTR(args[1]+i)!=((uint8_t *)original_quaternion)[i])bad=1;
 memcpy(GPTR(args[0]),original_matrix,36);return args[0];
}
'''
CHECK=r'''
static float movement[2];
API void setup(const float *eye,const float *q,const float *m,float fov,float z,float (*tangent)(float)){
 memcpy(original_eye,eye,12);memcpy(original_quaternion,q,16);memcpy(original_matrix,m,36);
 memcpy(GPTR(0x10134),&fov,4);memcpy(GPTR(0x10174),&z,4);fixture_tanf=tangent;g_esp=0x1F000;bad=pose_calls=matrix_calls=0;
 camera_anchor.valid=0;
}
API int apply(float distance,float dx,float dy,float x,float y,unsigned w,unsigned h){
 expected_distance=distance;pose_calls=matrix_calls=0;
 int result=touch_camera_translation(0x10000,distance,dx,dy,x,y,(uint64_t)h<<32|w,movement);
 if(g_esp!=0x1F000||pose_calls!=1||matrix_calls!=1)bad=1;return bad?-1:result;
}
API const float *result(void){return movement;}
API void terrain(unsigned enabled,float cell,unsigned width,unsigned rows,float (*height)(float,float)){
 fixture_context=enabled?S2_TOUCH_TOWN:0;fixture_system=0x12000;fixture_height=height;height_calls=0;
 *(uint32_t *)GPTR(0x12000)=0xA0DAE264;*(uint32_t *)GPTR(0x12084)=0x13000;
 *(uint32_t *)GPTR(0x13000)=0xA0DB2FC0;*(uint32_t *)GPTR(0x13008)=0xA0DB302C;
 memcpy(GPTR(0x13014),&cell,4);*(uint32_t *)GPTR(0x1301C)=width;*(uint32_t *)GPTR(0x13020)=rows;
 *(uint32_t *)GPTR(0x13070)=0x14000;
}
API unsigned ground_calls(void){return height_calls;}
API void terrain_word(unsigned address,unsigned value){*(uint32_t *)GPTR(address)=value;}
API int ground_height(float x,float y){float height=12345;int ok=touch_town_height(x,y,&height);if(!ok&&height!=12345)bad=1;return bad?-1:ok;}
API float anchor(const float *eye,const float *ray,float fallback){return touch_camera_ground_plane(eye,ray,fallback);}
API void lot(unsigned context,float height){fixture_context=context;lot_height=height;lot_enabled=1;lot_calls=0;camera_anchor.valid=0;}
API unsigned lot_count(void){return lot_calls;}
API void epoch(void){++fixture_epoch;}
API void new_pick(void){camera_anchor.valid=0;}
API void frame(const float *eye,float z){memcpy(original_eye,eye,12);memcpy(GPTR(0x10174),&z,4);}
'''

def original_camera():
    pe=pefile.PE(str(ROOT/'generated/input/Sims2EP9.exe'));image=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
    u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(base,(len(image)+4095)&~4095);u.mem_write(base,image)
    u.mem_map(0x10000,0x40000);u.mem_map(0x90000,0x1000)
    u.mem_write(0xA10FB36C,struct.pack('<3f',0,0,1))
    def call(va,args,self=0):
        u.reg_write(X.UC_X86_REG_ESP,0x40000);u.reg_write(X.UC_X86_REG_ECX,self)
        u.reg_write(X.UC_X86_REG_MXCSR,0x1F80);u.reg_write(X.UC_X86_REG_FPCW,0x37F)
        u.mem_write(0x40000,struct.pack('<'+'I'*(len(args)+1),0x90000,*args));u.emu_start(va,0x90000,count=500000)
        assert u.reg_read(X.UC_X86_REG_EIP)==0x90000
    def pose(yaw,slope,distance,roll):
        u.mem_write(0x10000+0xAC,struct.pack('<f',slope));u.mem_write(0x10000+196,struct.pack('<f',yaw))
        u.mem_write(0x10000+0x16C,struct.pack('<3f',17,23,4))
        u.mem_write(0x10000+0x1EC,struct.pack('<4f',0,math.sin(roll/2),0,math.cos(roll/2)))
        call(0xA0B4CEC0,[0x18000,0x18020,struct.unpack('<I',struct.pack('<f',distance))[0],1],0x10000)
        eye=struct.unpack('<3f',u.mem_read(0x18000,12));q=struct.unpack('<4f',u.mem_read(0x18020,16))
        call(0xA01A77F0,[0x18100,0x18020]);m=struct.unpack('<9f',u.mem_read(0x18100,36))
        return eye,q,m
    def heights(width,rows,cell,values):
        u.mem_write(0x1A000+12,struct.pack('<f',cell));u.mem_write(0x1A000+20,struct.pack('<II',width,rows))
        u.mem_write(0x1A000+104,struct.pack('<I',0x20000))
        u.mem_write(0x20000,b''.join(struct.pack('<3f',x*cell,y*cell,values[x+y*width])for y in range(rows)for x in range(width)))
    def height(x,y):
        u.reg_write(X.UC_X86_REG_ESP,0x40000);u.reg_write(X.UC_X86_REG_ECX,0x1A000)
        u.reg_write(X.UC_X86_REG_MXCSR,0x1F80);u.reg_write(X.UC_X86_REG_FPCW,0x27F)
        u.mem_write(0x40000,struct.pack('<IffI',0x90200,x,y,0x90000))
        # Store/pop the real x87 return to a float, as the production caller does.
        u.mem_write(0x90200,b'\xD9\x1D\x00\x82\x01\x00\xC3')
        u.emu_start(0xA02A0660,0x90000,count=100000)
        assert u.reg_read(X.UC_X86_REG_ESP)==0x40010
        return struct.unpack('<f',u.mem_read(0x18200,4))[0]
    pose.heights=heights;pose.height=height
    def ray(origin, direction):
        u.mem_write(0x18400,struct.pack('<3f',*origin));u.mem_write(0x18420,struct.pack('<3f',*direction))
        call(0xA018E720,[0x18400,0x18420],0x18300)
        assert u.reg_read(X.UC_X86_REG_ESP)==0x4000C
        result=bytes(u.mem_read(0x18300,32))
        call(0xA018E860,[],0x18300)
        assert u.reg_read(X.UC_X86_REG_ESP)==0x40004 and bytes(u.mem_read(0x18300,4))==struct.pack('<I',0xA0DA1304)
        return result
    pose.ray=ray
    return pose

def main():
    source=(ROOT/'android/host/sims2_touch.c').read_text()
    cache=next(line for line in source.splitlines()if line.startswith('static struct {')and line.endswith(' camera_anchor;'))+'\n'
    actual=cache+''.join(function(source,sig)for sig in ('static float touch_float(','static int touch_town_height(',
                    'static float touch_camera_ground_plane(','static int touch_camera_translation('))
    oracle=original_camera();rng=random.Random(20261109);count=0;worst=0
    tan_type=C.CFUNCTYPE(C.c_float,C.c_float);tangent=tan_type(lambda x:math.tan(x) if math.isfinite(x) else math.nan)
    with tempfile.TemporaryDirectory(prefix='s2-camera-pan-') as folder:
        base=Path(folder);c=base/'test.c';obj=base/'test.obj';dll=base/'test.dll';c.write_text(PRELUDE+actual+CHECK)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin','-fno-stack-protector','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll),str(obj)],check=True)
        lib=C.CDLL(str(dll));ptr=C.POINTER(C.c_float);lib.setup.argtypes=[ptr,ptr,ptr,C.c_float,C.c_float,tan_type]
        lib.apply.argtypes=[C.c_float]*5+[C.c_uint,C.c_uint];lib.result.restype=ptr
        height_type=C.CFUNCTYPE(C.c_float,C.c_float,C.c_float);height_callback=height_type(oracle.height)
        lib.terrain.argtypes=[C.c_uint,C.c_float,C.c_uint,C.c_uint,height_type]
        lib.ground_height.argtypes=[C.c_float,C.c_float]
        lib.anchor.argtypes=[ptr,ptr,C.c_float];lib.anchor.restype=C.c_float
        lib.lot.argtypes=[C.c_uint,C.c_float]
        lib.frame.argtypes=[ptr,C.c_float]
        try:
            for n in range(240):
                yaw=rng.random()*2*math.pi;slope=rng.uniform(.35,3);distance=rng.choice([8,35,650])
                fov=rng.choice([.575958653,.785398163,1.047197551]);w,h=rng.choice([(800,600),(1300,600),(1346,600),(2400,1080)])
                eye,q,m=oracle(yaw,slope,distance,rng.uniform(-.08,.08))
                lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),fov,4,tangent)
                ax,ay=rng.uniform(.2,.8),rng.uniform(.3,.85);dx,dy=rng.uniform(-.06,.06),rng.uniform(-.06,.06)
                assert lib.apply(distance,dx,dy,ax,ay,w,h)==1
                delta=list(lib.result()[:2]);right=m[0::3];forward=m[1::3];up=m[2::3];th=math.tan(fov/2);tv=th*h/w
                ray=[forward[k]+(2*ax-1)*th*right[k]+(1-2*ay)*tv*up[k] for k in range(3)]
                ground=[eye[k]+(4-eye[2])/ray[2]*ray[k] for k in range(3)]
                v=[ground[k]-eye[k]-(delta[k] if k<2 else 0) for k in range(3)]
                dot=lambda a,b:sum(x*y for x,y in zip(a,b))
                sx=(1+dot(v,right)/dot(v,forward)/th)/2;sy=(1-dot(v,up)/dot(v,forward)/tv)/2
                error=max(abs(sx-(ax+dx))*w,abs(sy-(ay+dy))*h);worst=max(worst,error)
                assert error<.004,(n,error,yaw,slope,distance,delta)
                count+=1
            # A raised/sloping town is above the camera's fixed target plane.
            # Read actual production C heights through the original x86 getter.
            terrain_count=0;terrain_worst=0
            for surface in range(3):
                values=[6.0 if surface==0 else 5+.01*x+.02*y if surface==1
                        else 5+.002*((x-20)**2+(y-20)**2)
                        for y in range(0,200,10)for x in range(0,200,10)]
                oracle.heights(20,20,10,values)
                for n in range(20):
                    yaw=rng.random()*2*math.pi;distance=35;fov=.575958653;w,h=1334,600
                    eye,q,m=oracle(yaw,1,distance,0)
                    lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),fov,4,tangent)
                    lib.terrain(1,10,20,20,height_callback)
                    ax,ay=rng.uniform(.45,.55),rng.uniform(.45,.55);dx,dy=rng.uniform(-.04,.04),rng.uniform(-.04,.04)
                    assert lib.apply(distance,dx,dy,ax,ay,w,h)==1
                    delta=list(lib.result()[:2]);right=m[0::3];forward=m[1::3];up=m[2::3];th=math.tan(fov/2);tv=th*h/w
                    ray=[forward[k]+(2*ax-1)*th*right[k]+(1-2*ay)*tv*up[k]for k in range(3)]
                    lo,hi=0,distance*1.3
                    for k in range(2):
                        if abs(ray[k])>1e-8:
                            bounds=sorted((-eye[k]/ray[k],(190-eye[k])/ray[k]))
                            lo=max(lo,bounds[0]+.0001);hi=min(hi,bounds[1]-.0001)
                    def difference(t):
                        return eye[2]+t*ray[2]-oracle.height(eye[0]+t*ray[0],eye[1]+t*ray[1])
                    assert difference(lo)>0 and difference(hi)<0,(surface,n,eye,ray,difference(lo),difference(hi))
                    for _ in range(45):
                        mid=(lo+hi)/2
                        if difference(mid)>0:lo=mid
                        else:hi=mid
                    ground=[eye[k]+(lo+hi)/2*ray[k]for k in range(3)]
                    v=[ground[k]-eye[k]-(delta[k]if k<2 else 0)for k in range(3)]
                    dot=lambda a,b:sum(x*y for x,y in zip(a,b))
                    sx=(1+dot(v,right)/dot(v,forward)/th)/2;sy=(1-dot(v,up)/dot(v,forward)/tv)/2
                    error=max(abs(sx-(ax+dx))*w,abs(sy-(ay+dy))*h);terrain_worst=max(terrain_worst,error)
                    assert error<.15,(surface,n,error,lib.ground_calls())
                    assert 1<=lib.ground_calls()<=12
                    terrain_count+=1
            # Invalid ownership, dimensions, coordinates and wrapped vertex storage cannot call the getter.
            invalid_terrain=0
            for address,value,x,y in [(0x12000,0,20,20),(0x12084,0,20,20),(0x13000,0,20,20),
                    (0x13008,0,20,20),(0x13014,0,20,20),(0x1301C,1,20,20),
                    (0x13020,4097,20,20),(0x13070,0xFFFFFFF0,20,20),(0x13070,0x14000,-1,20),
                    (0x13070,0x14000,190,20),(0x13070,0x14000,20,float('nan'))]:
                lib.terrain(1,10,20,20,height_callback);lib.terrain_word(address,value)
                assert lib.ground_height(x,y)==0 and lib.ground_calls()==0
                invalid_terrain+=1
            def changed_owner(x,y):
                lib.terrain_word(0x12084,0)
                return 6
            changed_callback=height_type(changed_owner)
            lib.terrain(1,10,20,20,changed_callback)
            assert lib.ground_height(20,20)==0 and lib.ground_calls()==1
            nan_callback=height_type(lambda x,y:float('nan'))
            lib.terrain(1,10,20,20,nan_callback)
            assert lib.ground_height(20,20)==0 and lib.ground_calls()==1
            alternating=[0]
            def changing_height(x,y):
                alternating[0]+=1
                return 6+alternating[0]%2
            changing_callback=height_type(changing_height)
            lib.terrain(1,10,20,20,changing_callback)
            assert lib.anchor((C.c_float*3)(20,20,20),(C.c_float*3)(0,0,-1),4)==4
            assert lib.ground_calls()==12
            invalid_terrain+=3
            lib.terrain(0,10,20,20,height_callback)
            # One scene query anchors each lot gesture, including coalesced/multi-frame moves.
            lot_count=0;lot_worst=0
            for context in [128,256,512]:
                for height in [-6,0,3.1,9.3]:
                    eye,q,m=oracle(.96,1,35,0);fov=.575958653;w,h=1334,600
                    lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),fov,4,tangent)
                    lib.lot(context,height);ax,ay=.45,.52
                    initial_eye=eye
                    for step in range(10):
                        eye=initial_eye[:2]+(initial_eye[2]+step*.02,)
                        lib.frame((C.c_float*3)(*eye),4+step*.02)
                        assert lib.apply(35,.005,0,ax,ay,w,h)==1
                        assert lib.lot_count()==1
                        delta=list(lib.result()[:2]);right=m[0::3];forward=m[1::3];up=m[2::3]
                        th=math.tan(fov/2);tv=th*h/w
                        ray=[forward[k]+(2*ax-1)*th*right[k]+(1-2*ay)*tv*up[k]for k in range(3)]
                        ground=[eye[k]+(height-eye[2])/ray[2]*ray[k]for k in range(3)]
                        v=[ground[k]-eye[k]-(delta[k]if k<2 else 0)for k in range(3)]
                        dot=lambda a,b:sum(x*y for x,y in zip(a,b))
                        sx=(1+dot(v,right)/dot(v,forward)/th)/2;sy=(1-dot(v,up)/dot(v,forward)/tv)/2
                        error=max(abs(sx-(ax+.005))*w,abs(sy-ay)*h);lot_worst=max(lot_worst,error)
                        assert error<.004,(context,height,step,error)
                        ax+=.005;lot_count+=1
                    # Discontinuous origin, layout epoch and a fresh owner pick each re-anchor.
                    assert lib.apply(35,.005,0,ax+.1,ay,w,h)==1 and lib.lot_count()==2
                    ax+=.105;lib.epoch()
                    assert lib.apply(35,.005,0,ax,ay,w,h)==1 and lib.lot_count()==3
                    ax+=.005;lib.new_pick()
                    assert lib.apply(35,.005,0,ax,ay,w,h)==1 and lib.lot_count()==4
            lib.terrain(0,10,20,20,height_callback)
            # A parallel/away-facing ray cannot anchor terrain; reject it instead of jumping the camera.
            eye,q,m=oracle(0,0,10,0);lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),.6,4,tangent)
            assert lib.apply(10,.1,0,.5,.5,1300,600)==0
            eye,q,m=oracle(0,1,10,0)
            for payload in [(float('nan'),eye,m),(float('inf'),eye,m),(.6,(float('nan'),0,0),m),(.6,eye,(float('nan'),)+m[1:])]:
                ff,ee,mm=payload;lib.setup((C.c_float*3)(*ee),(C.c_float*4)(*q),(C.c_float*9)(*mm),ff,4,tangent)
                assert lib.apply(10,.1,0,.5,.5,1300,600)==0
            receipt=dict(passed=True,production_touch_sha256=hashlib.sha256((ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),input_pe_sha256=hashlib.sha256((ROOT/'generated/input/Sims2EP9.exe').read_bytes()).hexdigest(),original_x86_poses=count,production_translation_cases=count,worst_reprojection_error_pixels=worst,
                         original_functions=['A0B4CEC0','A01A77F0','A02A0660'],town_terrain_cases=terrain_count,town_terrain_invalid_cases=invalid_terrain,
                         worst_terrain_reprojection_error_pixels=terrain_worst,horizontal_fov_projection='A0A656F0',sky_ray='rejected',device_calibration='separate')
            receipt['lot_cached_moves']=lot_count
            receipt['worst_lot_surface_reprojection_error_pixels']=lot_worst
            out=ROOT/'build/20261109-request/camera-pan-oracle.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(receipt,indent=2)+'\n')
            print(json.dumps(receipt,indent=2))
        finally:
            C.windll.kernel32.FreeLibrary.argtypes=[C.c_void_p];C.windll.kernel32.FreeLibrary(lib._handle)

if __name__=='__main__':main()
