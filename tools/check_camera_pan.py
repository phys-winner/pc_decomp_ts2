"""Verify uniform production pan with original x86 camera poses."""
import ctypes as C
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
#define MEM32(a) (*(uint32_t *)GPTR(a))
static void *memcpy(void *d,const void *s,size_t n){uint8_t *o=d;const uint8_t *i=s;while(n--)*o++=*i++;return d;}
static int void_vm_writable(uint32_t a,uint32_t n){return a>=0x10000&&(uint64_t)a+n<=sizeof arena;}
static float (*fixture_tanf)(float);
static float tanf(float x){return fixture_tanf(x);}
static float original_eye[3],original_quaternion[4],original_matrix[9],expected_distance;
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
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
}
API int apply(float distance,float dx,float dy,unsigned w,unsigned h){
 expected_distance=distance;pose_calls=matrix_calls=0;
 int result=touch_camera_translation(0x10000,distance,dx,dy,(uint64_t)h<<32|w,movement);
 if(g_esp!=0x1F000||pose_calls!=1||matrix_calls!=1)bad=1;return bad?-1:result;
}
API const float *result(void){return movement;}
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
    actual=''.join(function(source,sig) for sig in ('static float touch_float(',
                   'static int touch_camera_basis(', 'static int touch_camera_translation('))
    oracle=original_camera();rng=random.Random(20261109);count=0;worst=0
    tan_type=C.CFUNCTYPE(C.c_float,C.c_float);tangent=tan_type(lambda x:math.tan(x) if math.isfinite(x) else math.nan)
    with tempfile.TemporaryDirectory(prefix='s2-camera-pan-') as folder:
        base=Path(folder);c=base/'test.c';obj=base/'test.obj';dll=base/'test.dll';c.write_text(PRELUDE+actual+CHECK)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin','-fno-stack-protector','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll),str(obj)],check=True)
        lib=C.CDLL(str(dll));ptr=C.POINTER(C.c_float);lib.setup.argtypes=[ptr,ptr,ptr,C.c_float,C.c_float,tan_type]
        lib.apply.argtypes=[C.c_float]*3+[C.c_uint,C.c_uint];lib.result.restype=ptr
        try:
            for n in range(240):
                yaw=rng.random()*2*math.pi;slope=rng.uniform(.35,3);distance=rng.choice([8,35,650])
                fov=rng.choice([.575958653,.785398163,1.047197551]);w,h=rng.choice([(800,600),(1300,600),(1346,600),(2400,1080)])
                eye,q,m=oracle(yaw,slope,distance,rng.uniform(-.08,.08))
                lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),fov,4,tangent)
                dx,dy=rng.uniform(-.06,.06),rng.uniform(-.06,.06)
                assert lib.apply(distance,dx,dy,w,h)==1
                delta=list(lib.result()[:2])
                assert lib.apply(distance,dx*.5,dy*.5,w,h)==1
                assert max(abs(a-2*b) for a,b in zip(delta,lib.result()[:2]))<.0001
                right=m[0::3];forward=m[1::3];up=m[2::3]
                span=2*math.tan(fov/2)
                def project(step):
                    ray=[forward[k]+step*span*(dx*right[k]-dy*h/w*up[k]) for k in range(3)]
                    depth=distance*forward[2]/ray[2]
                    return [depth*ray[k] for k in range(2)]
                epsilon=.0001
                expected=[(a-b)/(2*epsilon) for a,b in zip(project(-epsilon),project(epsilon))]
                error=max(abs(a-b) for a,b in zip(delta,expected));worst=max(worst,error)
                assert error<.001,(n,error)
                count+=1
            for slope in (0,.01,1,3):
                eye,q,m=oracle(.96,slope,35,0)
                lib.setup((C.c_float*3)(*eye),(C.c_float*4)(*q),(C.c_float*9)(*m),.6,4,tangent)
                assert lib.apply(35,.04,.04,1334,600)==1
                assert all(math.isfinite(x) and abs(x)<35 for x in lib.result()[:2])
            receipt=dict(passed=True,original_x86_poses=count,uniform_split_drags=count,
                         max_center_projection_error=worst,device_calibration='separate')
            out=ROOT/'build/test/camera-pan/results.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(receipt,indent=2)+'\n')
            print(json.dumps(receipt,indent=2))
        finally:
            C.windll.kernel32.FreeLibrary.argtypes=[C.c_void_p];C.windll.kernel32.FreeLibrary(lib._handle)

if __name__=='__main__':main()
