"""Production CreateQuery support/OOM contract, including an output aliasing its argument."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
from build_android import ROOT, CLANG
from check_virtual_query import function


def main():
    actual = function((ROOT/'android/host/d3d9_device.c').read_text(), 'COM_METHOD(dev, CreateQuery, 3)')
    prelude = r'''
typedef unsigned int uint32_t;
typedef struct {int unused;} d3d_device;
static d3d_device device;
static uint32_t words[16],dummy,bad,calls,answer;
static uint32_t *word(uint32_t a){if(!a||a%4||a>=sizeof words){bad=1;return &dummy;}return &words[a/4];}
#define ARG(n) words[n]
#define MEM32(a) (*word(a))
#define DEV (&device)
#define VLOG(...) ((void)0)
#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_OUTOFMEMORY_ 0x8007000Eu
#define COM_METHOD(p,n,c) static uint32_t p##_##n(void)
static uint32_t d3d_query_create(d3d_device *d,uint32_t t){if(d!=&device||t!=words[1])bad=1;++calls;return answer;}
'''
    checks = r'''
__declspec(dllexport) uint32_t check(uint32_t type,uint32_t out,uint32_t result){
    for(int i=0;i<16;++i)words[i]=0xA5A55A5A;
    words[1]=type;words[2]=out;bad=calls=0;answer=result;
    uint32_t r=dev_CreateQuery(),supported=type==4||type==8||type==9;
    uint32_t expected=!supported?D3DERR_NOTAVAILABLE:!out||result?D3D_OK:E_OUTOFMEMORY_;
    if(r!=expected||bad||calls!=(supported&&out))return 1;
    if(out&&supported&&words[out/4]!=result)return 2;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-query-create-') as d:
        p=Path(d);source=p/'check.c';obj=p/'check.obj';dll=p/'check.dll'
        source.write_text(prelude+actual+checks)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin',
                        '-fno-stack-protector','-mno-stack-arg-probe','-c',str(source),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry',
                        '/out:'+str(dll),str(obj)],check=True)
        library=ctypes.CDLL(str(dll));library.check.argtypes=[ctypes.c_uint32]*3
        for kind in (4,8,9,255):
            for out in (0,8,40):
                for value in (0,0x1234): assert library.check(kind,out,value)==0,(kind,out,value)
        ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(library._handle)
    print('PASS actual CreateQuery: 24 support/OOM/null/alias cases, original output pointer captured once')


if __name__=='__main__': main()
