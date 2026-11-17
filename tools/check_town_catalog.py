"""Replay the captured town subpanel COM indirection through production C."""
import ctypes
import json
from pathlib import Path
import subprocess
import tempfile
import pefile
from build_android import CLANG, ROOT
from check_virtual_query import function
from check_widget_swipe import PRELUDE

CHECK=r'''
static unsigned changes;
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va!=0xA07479F0u||self!=0x17000||n!=1)return 0;
 MEM32(self+0x20)=args[0];++changes;return 1;
}
API void write32(unsigned address,unsigned value){MEM32(address)=value;}
API unsigned read32(unsigned address){return MEM32(address);}
API void state(unsigned c,unsigned o){context=c;owner=o;changes=0;}
API unsigned bound(void){return touch_town_catalog_proc(0x13000);}
API unsigned scroll(int amount){uint32_t p=touch_town_catalog_proc(0x13000);return p&&touch_grid_apply(p,MEM32(p+0x54),amount);}
API unsigned changed(void){return changes;}
'''

def main():
    source=(ROOT/'android/host/sims2_touch.c').read_text()
    actual='\n'.join(function(source,s) for s in ('static const char *touch_type(uint32_t object)\n{','static int touch_grid_proc(','static uint32_t touch_town_catalog_proc(','static int touch_grid_apply('))
    pe=pefile.PE(str(ROOT/'generated/input/Sims2EP9.exe'));image=pe.get_memory_mapped_image()
    cases=json.loads((ROOT/'tools/fixtures/town-catalogues.json').read_text())['cases']
    with tempfile.TemporaryDirectory(prefix='s2-town-catalog-') as folder:
        base=Path(folder);c=base/'test.c';obj=base/'test.obj';dll=base/'test.dll'
        c.write_text(PRELUDE+'\ntypedef long long int64_t;\n#define S2_TOUCH_CAS 4\n#define void_vm_writable void_vm_readable\n'+actual+CHECK)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin','-fno-stack-protector','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll),str(obj)],check=True)
        lib=ctypes.CDLL(str(dll));lib.image_data.restype=ctypes.c_void_p;lib.read32.restype=ctypes.c_uint
        try:
            ctypes.memmove(lib.image_data(),image,len(image))
            for captured in cases:
                name=captured['name'];records={int(k,16):v for k,v in captured['records'].items()}
                parent_address=int(captured['window'],16);delegate_address=int(captured['delegate'],16)
                catalogue=int(captured['catalogue'],16);slot=int(captured['slot'],16)
                owner=int(records[catalogue]['0x54'],16)
                mapping={parent_address:0x13000,delegate_address:0x15000,catalogue:0x17000,catalogue+0x160:0x17160,
                         owner:0x16000,int(captured['manager'],16):0x11000}
                for address in records:
                    if address not in mapping:mapping[address]=0x18000+0x200*len(mapping)
                def setup():
                    lib.state(64,1)
                    for address,record in records.items():
                        for offset,word in record.items():
                            value=int(word,16);lib.write32(mapping[address]+int(offset,16),mapping.get(value,value))
                setup();assert lib.bound()==0x17000,name
                assert lib.scroll(-1) and lib.read32(0x17020)==1 and lib.read32(0x17028)==1 and lib.changed()==1
                assert lib.scroll(1) and lib.read32(0x17020)==0 and lib.changed()==2
                for address,value in [(0x13000,0),(0x13004,0),(0x1312C,0),(0x130E8,0),(0x15000,0),(0x1500C,0),
                                      (0x15000+slot,0),(0x17160,0),(0x17000,0),(0x17054,0),(0x16004,0),(0x16058,0),
                                      (0x16058,0x16000)]:
                    setup();lib.write32(address,value);assert not lib.bound() and not lib.scroll(-1) and not lib.changed(),(name,hex(address))
                for context,owner in [(0,1),(4,1),(65,1),(66,1),(128,1),(256,1),(512,1),(64,0)]:
                    setup();lib.state(context,owner);assert not lib.bound() and not lib.scroll(-1)
                setup();assert not lib.scroll(9) and not lib.scroll(0) and not lib.changed()
                print('PASS captured town',name,': real outer delegate, +160 catalog interface and complete catalog owner chain; original Grid2 setter, one-item scroll and ownership/type/context/loop guards')
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.FreeLibrary(lib._handle)

if __name__=='__main__':main()
