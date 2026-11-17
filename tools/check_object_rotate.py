"""Production original ToolObject rotation helper ABI oracle; no device operations.

The original full handler, capability, reference counting, message constructor,
setters/destructor and strict host guest-call helper execute. Preview/model and
global property/TLS-service endpoints are explicit boundaries, not game-model QA.
"""
import hashlib
import io
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_android as B
import check_touch_barrier as T
from check_virtual_query import function
from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A
from unicorn import x86_const as X

OUT = ROOT / "build/test/object-rotate-v15c"
PE = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
IMAGE = PE.get_memory_mapped_image()
BASE = PE.OPTIONAL_HEADER.ImageBase
def dword(va): return struct.unpack('<I', PE.get_data(va - BASE, 4))[0]
MODEL = {name: dword(0xA0DE83FC + offset) for name, offset in
         [('turn', 0x12C), ('point', 0x160), ('valid', 0x158), ('snap', 0x16C), ('destroy', 0x40)]}
ACTUAL = ['A067C356','A067BE89','A067B182','A06B5E7A','A067CB69','A0229B60',
          'A078D60A','A078D8E9','A01C033D','A01C0915','A01C0874','A01C0388',
          'A0145770','A0149CF0','A01CBD79','A078E56C','A08248B0']

EXTRA = r'''
static uint32_t win_manager=0x11000,tool_handler=0x11500,scene_window=0x16000;
static uint64_t token=0x100000002ull;
static unsigned cases,turns,stops,allocs,frees,destructs,mode,held_key,tracker_refs;
static _Atomic unsigned cas_pan_epoch;
static unsigned constructed,destroyed,handler_calls;
static int delta,expected_turn=-2;
static const char *touch_type(uint32_t object);
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx,g_icall_count;
void (*recomp_yield_hook)(void);
uint32_t s2_touch_owner_manager(void){return thread==1?win_manager:0;}
uint64_t s2_touch_tool_token(void){return token;}
uint32_t void_galloc(uint32_t size){++allocs;if(size!=44)bad=90;return mode==8?0:0x24000;}
void void_gfree(uint32_t p){if(p!=0x24000)bad=91;++frees;}
static void impossible(void){bad=92;g_eax=0;g_esp+=4;}
static void picker(void){
 if(g_ecx!=0x12000||ARG(0)!=700||ARG(1)!=500)bad=93;
 if(mode==4)context=S2_TOUCH_BUILD|S2_TOUCH_MODAL;
 if(mode==5)token+=0x100000000ull;
 if(mode==10){MEM32(tool_handler+0x20)=0x14000;token+=0x100000000ull;MEM32(0x13008)=1;}
 if(mode==11)held_key=1;
 if(mode==12)atomic_fetch_add(&cas_pan_epoch,1);
 if(mode==13)native_width=800;
 g_eax=mode==6?0:mode==7?0x16500:scene_window;g_esp+=12;
}
static void property(void){MEM32(g_ecx)=0;g_eax=g_ecx;g_esp+=4;}
static void tracker(void){g_eax=0x17400;g_esp+=4;}
static void track_add(void){++tracker_refs;g_eax=1;g_esp+=4;}
static void track_release(void){if(!tracker_refs)bad=94;else --tracker_refs;g_eax=0;g_esp+=4;}
static void model_turn(void){
 if(g_ecx!=0x13000||(int)ARG(0)!=expected_turn||MEM32(0x13008)!=2)bad=95;
 ++turns;delta=(int)ARG(0);
 if(mode==9){MEM32(tool_handler+0x20)=0x14000;token+=0x100000000ull;MEM32(0x13008)=1;}
 g_eax=1;g_esp+=8;
}
static void model_point(void){g_eax=1;g_esp+=8;}
static void model_valid(void){g_eax=1;g_esp+=8;}
static void model_snap(void){memcpy(GPTR(ARG(0)),GPTR(ARG(1)),12);g_eax=ARG(0);g_esp+=12;}
static void model_destroy(void){if(g_ecx!=0x13000||ARG(0)!=1||MEM32(0x13008))bad=96;++destructs;g_esp+=8;}
static void geometry(void){g_eax=0;g_esp+=4;} /* cdecl2; caller removes8 */
int _setjmp(jmp_buf env){(void)env;return 0;} /* normal returns only; no throw claim */
void void_lift_trap(uint32_t a,const char *s){(void)a;(void)s;bad=97;}
'''

CHECK = r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
static void reset(unsigned m){
 memset(GPTR(0x10000),0,0x20000);memset(&g_m,0,sizeof g_m);g_stp=g_stbuf+X87_CENTRE;
 win_manager=0x11000;tool_handler=0x11500;scene_window=0x16000;thread=1;context=S2_TOUCH_BUY;input_context=0;
 token=0x100000002ull;native_width=1212;native_height=540;atomic_store(&cas_pan_epoch,0);mode=m;held_key=turns=stops=allocs=frees=destructs=tracker_refs=bad=0;
 MEM32(win_manager+0x44)=0x12000;MEM32(0x12000)=0x17000;MEM32(0x170BC)=0x704;
 MEM32(tool_handler+0x20)=0x13000;MEM32(0x13000)=0xA0DE83FC;MEM32(0x13008)=1;MEM32(0x13064)=0x15000;
 MEM32(0x130A0)=2;MEM32(0x15054)=1;MEM32(0x1505C)=0x18000;
 MEM32(0x17400)=0x17600;MEM32(0x17604)=0x705;MEM32(0x17628)=0x706;
 MEM32(0x14000)=0xA0DE83FC;MEM32(0x14008)=1;MEM32(0x140A0)=3;
 g_eax=0xAA;g_ecx=0xBB;g_edx=0xCC;g_ebx=0xDD;g_esp=0x28000;g_ebp=0x29000;g_esi=0xEE;g_edi=0xFF;
 g_fs_base=0x25000;MEM32(0x25000)=0xCAFEBABE;g_cur_func=0xABCD;g_flag_k=FK_CMP;g_flag_a=123;g_flag_b=45;g_flag_cf=1;
 g_mxcsr=0x1F80;g_fpu_cw=0x27F;void_fpu_cmp=1;g_fp_bias=3;
 g_eh_frames=(void *)(uintptr_t)0x1111;g_eh_obj=222;g_eh_info=333;
}
static int checked_direction(uint64_t expected,int direction){
 void_regs before={0},after={0};void_regs_save(&before);
 int r=touch_rotate_object_apply(expected,700,500,context,((uint64_t)native_height<<32)|native_width,atomic_load(&cas_pan_epoch),direction);void_regs_save(&after);
 if(memcmp(&before,&after,sizeof before)||MEM32(0x25000)!=0xCAFEBABE)bad=98;
 return r;
}
static int checked(uint64_t expected){return checked_direction(expected,-1);}
int selftest(void){
 reset(0);REQUIRE(checked(token)==1&&!bad&&turns==1&&delta==-2&&MEM32(0x130A0)==0&&!MEM8(0x1313E));
 REQUIRE(MEM32(0x13008)==1&&!tracker_refs&&allocs==1&&frees==1&&!destructs);++cases;
 reset(0);expected_turn=2;REQUIRE(checked_direction(token,1)==1&&!bad&&turns==1&&delta==2&&MEM32(0x130A0)==4);++cases;expected_turn=-2;
 reset(0);REQUIRE(checked(token)==1&&!bad); /* restore the second-command case */
 /* A second EXPLICIT hold is another command, not a retry: STOP must unlock it. */
 REQUIRE(checked(token)==1&&!bad&&turns==2&&!MEM8(0x1313E));++cases;
 reset(0);MEM32(0x13064)=0;REQUIRE(!checked(token)&&!bad&&!turns&&!allocs);++cases;
 for(unsigned state=0;state<5;++state)if(state!=1&&state!=3){reset(0);MEM32(0x15054)=state;REQUIRE(!checked(token)&&!bad&&!turns&&!allocs&&MEM32(0x13008)==1);++cases;}
 reset(0);MEM32(0x1505C)=0;REQUIRE(!checked(token)&&!bad&&!turns&&!allocs);++cases;
 reset(0);MEM8(0x1313E)=1;REQUIRE(!checked(token)&&!bad&&!turns&&!allocs&&MEM8(0x1313E));++cases;
 reset(0);context=S2_TOUCH_LIVE;REQUIRE(!checked(token)&&!bad&&!turns&&!allocs);++cases;
 reset(0);context=S2_TOUCH_BUILD;REQUIRE(checked(token)&&!bad&&turns==1);++cases;
 reset(0);context=S2_TOUCH_CAS;REQUIRE(!checked(token)&&!bad&&!turns);++cases;
 reset(0);context|=S2_TOUCH_FOCUSED_TEXT;REQUIRE(!checked(token)&&!bad&&!turns);++cases;
 reset(0);MEM32(win_manager+0x34)=0x16000;REQUIRE(!checked(token)&&!bad&&!turns);++cases;
 const unsigned held[]={1,2,4,0x10,0x11,0x12,0xBC,0xBE};
 for(unsigned k=0;k<sizeof held/sizeof held[0];++k){reset(0);held_key=held[k];REQUIRE(!checked(token)&&!bad&&!turns);}++cases;
 reset(0);thread=2;REQUIRE(!checked(token)&&!bad&&!turns);++cases;
 reset(0);REQUIRE(!checked(token+0x100000000ull)&&!bad&&!turns);++cases;
 for(unsigned m=4;m<=8;++m){reset(m);REQUIRE(!checked(token)&&!bad&&!turns&&allocs==frees+(m==8));++cases;}
 reset(9);REQUIRE(checked(token)&&!bad&&turns==1&&destructs==1&&!tracker_refs&&!MEM8(0x1313E));
 REQUIRE(MEM32(tool_handler+0x20)==0x14000&&MEM32(0x140A0)==3&&!MEM8(0x1413E));++cases;
 reset(10);REQUIRE(!checked(token)&&!bad&&!turns&&!allocs&&destructs==1&&!tracker_refs);++cases;
 reset(11);REQUIRE(!checked(token)&&!bad&&!turns&&!allocs&&MEM32(0x13008)==1&&!tracker_refs);++cases;
 for(unsigned angle=0;angle<8;++angle)for(unsigned active=0;active<2;++active)for(unsigned state=1;state<=3;state+=2){
  reset(0);MEM32(0x130A0)=angle;MEM8(0x130FC)=active;MEM32(0x13068)=0x15000;MEM32(0x15054)=state;
  int next=((int)angle-2)%8;
  REQUIRE(checked(token)&&!bad&&turns==1&&(int)MEM32(0x130A0)==next&&!MEM8(0x1313E));++cases;
 }
 for(unsigned m=12;m<=13;++m){reset(m);REQUIRE(!checked(token)&&!bad&&!turns&&!allocs&&MEM32(0x13008)==1&&!tracker_refs);++cases;}
 reset(0);REQUIRE(!touch_rotate_object_apply(token,700,500,S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,0,-1)&&!turns&&!allocs);++cases;
 reset(0);REQUIRE(!touch_rotate_object_apply(token,700,500,S2_TOUCH_BUY,((uint64_t)600<<32)|1212,0,-1)&&!turns&&!allocs);++cases;
 reset(0);REQUIRE(!touch_rotate_object_apply(token,700,500,S2_TOUCH_BUY,((uint64_t)540<<32)|1212,1,-1)&&!turns&&!allocs);++cases;
 return 0;
}
'''

def original_oracle():
    """Original machine instructions, independently of the lifted/C implementation."""
    cpu=Uc(UC_ARCH_X86,UC_MODE_32)
    cpu.mem_map(BASE,(len(IMAGE)+65535)&~65535);cpu.mem_write(BASE,IMAGE)
    cpu.mem_map(0,0x30000);stop=0x60000000;cpu.mem_map(stop,0x1000)
    observed=[]; tracker_refs=0; destroyed=0
    def rd(a):return int.from_bytes(cpu.mem_read(a,4),'little')
    def wr(a,v):cpu.mem_write(a,struct.pack('<I',v&0xffffffff))
    def endpoint(c,address,size,user):
        nonlocal tracker_refs,destroyed
        counts={0xA01CBACE:0,0xA014491B:0,0x705:0,0x706:0,0xA07981F2:0,
                MODEL['turn']:1,MODEL['point']:1,MODEL['valid']:1,MODEL['snap']:2,MODEL['destroy']:1}
        if address not in counts:return
        sp=c.reg_read(X.UC_X86_REG_ESP);this=c.reg_read(X.UC_X86_REG_ECX);value=1
        if address==0xA01CBACE:wr(this,0);value=this
        elif address==0xA014491B:value=0x17400
        elif address==0x705:tracker_refs+=1
        elif address==0x706:tracker_refs-=1;assert tracker_refs>=0
        elif address==0xA07981F2:value=0  # cdecl, original caller pops its2 args
        elif address==MODEL['turn']:
            assert this==0x13000 and rd(0x13008)==2
            observed.append(struct.unpack('<i',cpu.mem_read(sp+4,4))[0])
        elif address==MODEL['snap']:cpu.mem_write(rd(sp+4),bytes(cpu.mem_read(rd(sp+8),12)))
        elif address==MODEL['destroy']:
            assert this==0x13000 and rd(sp+4)==1 and rd(0x13008)==0;destroyed+=1
        c.reg_write(X.UC_X86_REG_EAX,value)
        c.reg_write(X.UC_X86_REG_EIP,rd(sp));c.reg_write(X.UC_X86_REG_ESP,sp+4+4*counts[address])
    cpu.hook_add(UC_HOOK_CODE,endpoint)
    def call(va,this,args=()):
        sp=0x28000;cpu.mem_write(sp,struct.pack('<'+'I'*(len(args)+1),stop,*[a&0xffffffff for a in args]))
        cpu.reg_write(X.UC_X86_REG_ECX,this);cpu.reg_write(X.UC_X86_REG_ESP,sp)
        cpu.emu_start(va,stop,count=100000)
        assert cpu.reg_read(X.UC_X86_REG_EIP)==stop,(hex(va),'did not return')
        assert cpu.reg_read(X.UC_X86_REG_ESP)==sp+4+4*len(args),(hex(va),'wrong original arity')
        assert rd(0)==0xCAFEBABE,(hex(va),'FS chain not restored')
        return cpu.reg_read(X.UC_X86_REG_EAX)
    def reset(angle=2,active=0,state=1,object_=0x18000):
        nonlocal tracker_refs,destroyed
        cpu.mem_write(0,b'\0'*0x30000);wr(0,0xCAFEBABE);tracker_refs=destroyed=0;observed.clear()
        wr(0x13000,0xA0DE83FC);wr(0x13008,1);wr(0x13064,0x15000);wr(0x13068,0x15000)
        wr(0x130A0,angle);cpu.mem_write(0x130FC,bytes([active]));wr(0x15054,state);wr(0x1505C,object_)
        wr(0x17400,0x17600);wr(0x17604,0x705);wr(0x17628,0x706)
        cpu.reg_write(X.UC_X86_REG_EBP,0x29000)
    cases=0
    for angle in range(8):
      for active in (0,1):
       for state in (1,3):
        for direction in (-1,1):
          reset(angle,active,state);before=bytes(cpu.mem_read(0x13000,0x10000))
          assert call(0xA067BE89,0x13000)&255==1
          assert bytes(cpu.mem_read(0x13000,0x10000))==before,'capability mutated game memory'
          assert call(0xA078D60A,0x13000)==2 and tracker_refs==1
          call(0xA01C033D,0x24000);assert rd(0x24000)==0xA0DA4064
          call(0xA01C0915,0x24000,[0xACB3A629]);call(0xA01C0874,0x24000,[direction])
          assert call(0xA067C356,0x13000,[0x24000,0])&255==1
          delta=-2 if direction<0 else 2;raw=angle+delta;expected=raw%8 if raw>=0 else -((-raw)%8)
          assert rd(0x130A0)==expected&0xffffffff and observed==[delta]
          assert cpu.mem_read(0x1313E,1)==b'\1'
          assert call(0xA067C356,0x13000,[0x24000,0])&255==0 and observed==[delta],'START latch bypassed'
          call(0xA01C0915,0x24000,[0x2FE3C0CE]);call(0xA067C356,0x13000,[0x24000,0])
          assert cpu.mem_read(0x1313E,1)==b'\0'
          call(0xA01C0388,0x24000);call(0xA078D8E9,0x13000)
          assert rd(0x13008)==1 and tracker_refs==0 and destroyed==0;cases+=1
    for state,object_ in ((0,0x18000),(2,0x18000),(4,0x18000),(1,0)):
        reset(state=state,object_=object_);before=bytes(cpu.mem_read(0x13000,0x10000))
        assert call(0xA067BE89,0x13000)&255==0
        assert bytes(cpu.mem_read(0x13000,0x10000))==before
        call(0xA01C033D,0x24000);call(0xA01C0915,0x24000,[0xACB3A629]);call(0xA01C0874,0x24000,[-1])
        assert call(0xA067C356,0x13000,[0x24000,0])&255==0 and not observed
        call(0xA01C0915,0x24000,[0x2FE3C0CE]);call(0xA067C356,0x13000,[0x24000,0]);call(0xA01C0388,0x24000)
        assert not cpu.mem_read(0x1313E,1)[0];cases+=1
    return {'cases':cases,'actual_original_x86':True,
      'scope':'Both native signed commands, all8 starting angles, both selected slots and supported wrapper states; no-object capability is read-only and handler rejects. Actual message ctor/setters/nondeleting dtor, Tool AddRef/Release and START latch/STOP. Model, property/TLS service endpoints are explicit boundaries.'}

def build_source():
    pre = T.PRELUDE[:T.PRELUDE.index('typedef struct {uint32_t hwnd')]
    pre = pre.replace('static void drive(void);','static void drive(void){}')
    pre = pre.replace('void void_input_screen_size(int *w,int *h){*w=1212;*h=540;}', 'static unsigned native_width=1212,native_height=540;void void_input_screen_size(int *w,int *h){*w=native_width;*h=native_height;}')
    pre = pre.replace('uint32_t void_galloc(uint32_t size){(void)size;return 0;}\nvoid void_gfree(uint32_t pointer){(void)pointer;}','')
    pre = pre.replace('int16_t void_input_key_state(uint32_t key){(void)key;return 0;}',
                      'static unsigned held_key;\nint16_t void_input_key_state(uint32_t key){return held_key && key==held_key?(int16_t)0x8000:0;}')
    pre = pre.replace('return a>=0x10000 && (uint64_t)a+n<=0x30000;',
                      f'return (a>=0x10000 && (uint64_t)a+n<=0x30000) || (a>=0x{BASE:X}u && (uint64_t)a+n<=0x{BASE+len(IMAGE):X}ull);')
    extra = EXTRA.replace('mode,held_key,tracker_refs', 'mode,tracker_refs')
    core = (ROOT/'android/host/core.c').read_text(encoding='utf-8')
    threads = (ROOT/'android/host/threads.c').read_text(encoding='utf-8')
    touch = (ROOT/'android/host/sims2_touch.c').read_text(encoding='utf-8')
    actual = ''.join(function(core,s) for s in ['void void_regs_save(', 'void void_regs_load('])
    actual += ''.join(function(threads,s) for s in ['uint32_t void_call_guest(', 'uint32_t void_call_guest_this('])
    eh = (ROOT/'android/host/eh.c').read_text(encoding='utf-8')
    actual += function(eh, 'void recomp_eh_push(')  # also includes adjacent pop
    actual += function(touch,'static const char *touch_type(uint32_t object)\n{')
    lifted=''
    for name in ACTUAL:
        page=int(name,16)&~0x7fff
        source=(ROOT/f'generated/sims2-x86/sims2_{page:08X}.c').read_text(encoding='utf-8')
        lifted+=function(source,f'void sub_{name}(void) {{')
    refs=sorted(set(__import__('re').findall(r'RECOMP_CALL\(sub_(A[0-9A-F]+)\)',lifted)))
    forward=''.join(f'void sub_{n}(void);\n' for n in sorted(set(refs+ACTUAL)))
    fallback=''.join(f'void sub_{n}(void){{impossible();}}\n' for n in refs if n not in ACTUAL and n not in ['A01CBACE','A014491B','A07981F2'])
    fallback+='void sub_A01CBACE(void){property();}\nvoid sub_A014491B(void){tracker();}\nvoid sub_A07981F2(void){geometry();}\n'
    names=['eax','ecx','edx','ebx','esp','ebp','esi','edi']
    lifted=''.join(f'#define {n} g_{n}\n' for n in names)+'#define _xmm g_xmm\n#define _st g_stp\n'+lifted+''.join(f'\n#undef {n}' for n in names)+'\n#undef _xmm\n#undef _st\n'
    dispatch='recomp_func_t recomp_lookup(uint32_t va){switch(va){\n'
    dispatch+=''.join(f'case 0x{n}u:return sub_{n};\n' for n in ACTUAL)
    dispatch+='case 0x704:return picker;case 0x705:return track_add;case 0x706:return track_release;\n'
    dispatch+=''.join(f'case 0x{va:X}u:return model_{name};\n' for name,va in MODEL.items())
    dispatch+='default:return impossible;}}\nrecomp_func_t void_resolve(uint32_t va){return recomp_lookup(va);}\n'
    layout = __import__('re').search(r'unsigned s2_touch_layout_epoch\(void\) \{[^}]+\}',touch).group()
    layout += function(touch,'static int touch_layout_matches(')
    helper = function(touch,'static int touch_rotate_object_apply(')
    return pre+extra+forward+actual+lifted+fallback+dispatch+layout+helper+CHECK

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    cfile,binary=OUT/'check.c',OUT/'check.elf'
    cfile.write_text(build_source(),encoding='utf-8')
    subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-mno-outline-atomics','-fno-builtin','-fno-stack-protector','-nostdlib',
        '-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),'-o',str(binary)],cwd=ROOT,check=True)
    elf=ELFFile(io.BytesIO(binary.read_bytes()));symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);native,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
    u.mem_map(native,0x1000000);u.mem_map(guest+0x10000,0x20000);u.mem_map(guest+BASE,(len(IMAGE)+65535)&~65535);u.mem_write(guest+BASE,IMAGE)
    u.mem_map(stack,0x10000);u.mem_map(stop,0x1000)
    for seg in elf.iter_segments():
        if seg['p_type']=='PT_LOAD':u.mem_write(native+seg['p_vaddr'],seg.data())
    reloc=elf.get_section_by_name('.rela.dyn')
    if reloc:
        for r in reloc.iter_relocations():
            assert r['r_info_type']==1027;u.mem_write(native+r['r_offset'],struct.pack('<Q',native+r['r_addend']))
    u.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);u.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);u.reg_write(A.UC_ARM64_REG_LR,stop)
    u.emu_start(native+symbols['selftest'],stop,count=30000000)
    assert u.reg_read(A.UC_ARM64_REG_PC)==stop
    assert u.reg_read(A.UC_ARM64_REG_X0)==0,('C line',u.reg_read(A.UC_ARM64_REG_X0),'bad',int.from_bytes(u.mem_read(native+symbols['bad'],4),'little'))
    result={'pass':True,'ARM64_cases':int.from_bytes(u.mem_read(native+symbols['cases'],4),'little'),
      'original_x86':original_oracle(),
      'production_sha256':hashlib.sha256((ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),
      'actual':ACTUAL,'model_endpoints':{n:f'{v:08X}' for n,v in MODEL.items()},
      'original_PE_sha256':hashlib.sha256((ROOT/'generated/input/Sims2EP9.exe').read_bytes()).hexdigest(),
      'EH_fixture_boundary':'Actual EH push/pop and original guest FS chain run; _setjmp returns0. Only normal guest returns are tested; no exception containment/rollback claim.',
      'limits':'Actual production rotation helper and original lifted handler/message/ref/capability with strict guest-call/register code. Preview/model, property/TLS service and root picking are declared boundaries. Shared owner FIFO/receipt tested separately. No device or budget/save acceptance.'}
    (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
