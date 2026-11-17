"""Original x86 versus actual AArch64 RGBX/A8 writers; no adb/build/install."""
import io
from pathlib import Path
import random
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A, x86_const as X
import build_android as B

ROOT=Path(__file__).resolve().parents[1]
SMALL_YIELD='--small-yield' in sys.argv
OUT=ROOT/'build/test/quantize-native'
if SMALL_YIELD:OUT=OUT/'small-yield'
OUT.mkdir(parents=True,exist_ok=True)
generated=(ROOT/'generated/sims2-x86/sims2_A0038000.c').read_text()
bodies=[]
for name in ('A0039752','A003AA9F'):
    begin=generated.index('void sub_'+name+'(void) {')
    end=generated.index('\n}\n',begin)+3
    bodies.append(generated[begin:end].replace('sub_'+name+'(void)', 'reference_'+name+'(void)',1))
source=OUT/'check.c'
reference=OUT/'reference.c'
reference.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                     'void sub_A00377E3(void), sub_A0038D18(void), sub_A0039034(void);\n'+'\n'.join(bodies))
source.write_text('''#include "host.h"
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias;
void (*recomp_yield_hook)(void);
uint32_t fallback_calls, helper_calls;
uint32_t g_enter_trace[RECOMP_ENTER_SIZE], g_enter_idx;
double nearbyint(double v) { double r; __asm__("frintn %d0, %d1":"=w"(r):"w"(v)); return r; }
double floor(double v) { double r; __asm__("frintm %d0, %d1":"=w"(r):"w"(v)); return r; }
double ceil(double v) { double r; __asm__("frintp %d0, %d1":"=w"(r):"w"(v)); return r; }
double trunc(double v) { double r; __asm__("frintz %d0, %d1":"=w"(r):"w"(v)); return r; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; for(size_t i=0;i<n;++i)p[i]=q[i]; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; if(p<q)for(size_t i=0;i<n;++i)p[i]=q[i]; else while(n){--n;p[n]=q[n];} return d; }
void sub_A00377E3(void) { ++helper_calls; g_esp+=8; }
void sub_A0038D18(void) { ++helper_calls; g_esp+=8; }
void sub_A0039034(void) { ++helper_calls; g_esp+=8; }
void reference_A0039752(void), reference_A003AA9F(void);
void sub_A0039752_lifted(void) { ++fallback_calls; reference_A0039752(); }
void sub_A003AA9F_lifted(void) { ++fallback_calls; reference_A003AA9F(); }
'''+f'#include "{ROOT.as_posix()}/android/host/sims2_fast.c"\n'+'''
uint32_t check(uint32_t alpha, uint32_t mode) {
    if(mode==1) return quant_row(alpha);
    if(mode==2) { if(alpha)reference_A003AA9F();else reference_A0039752(); }
    else { if(alpha)sub_A003AA9F();else sub_A0039752(); }
    return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);
}
''')
binary=OUT/'check.elf'
command=[str(B.CLANG),'--target=aarch64-linux-android30','-O2','-ffp-contract=off','-fno-builtin',
         '-ffunction-sections','-fdata-sections','-fno-stack-protector','-nostdlib','-DRECOMP_TRACE',
         '-Iandroid/runtime','-Iandroid/host','-Wl,--gc-sections,-e,check',str(source),str(reference),'-o',str(binary)]
if SMALL_YIELD:command.insert(1,'-DRECOMP_YIELD_EVERY=1')
build=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
(OUT/'warnings.txt').write_text(build.stderr)
assert build.returncode==0,build.stderr
assert not build.stderr.strip(),build.stderr
elf=ELFFile(io.BytesIO(binary.read_bytes()))
symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base,guest,native_stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
arm.mem_map(base,0x200000)
arm.mem_map(guest+0x10000,0x2F0000)
arm.mem_map(native_stack,0x10000)
arm.mem_map(stop,0x1000)
for segment in elf.iter_segments():
    if segment['p_type']=='PT_LOAD':arm.mem_write(base+segment['p_vaddr'],segment.data())
rel=elf.get_section_by_name('.rela.dyn')
if rel:
    for r in rel.iter_relocations():
        assert r['r_info_type']==1027
        arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
pe=pefile.PE(str(ROOT/'generated/input/Sims2EP9.exe'),fast_load=True)
image=pe.get_memory_mapped_image();imagebase=pe.OPTIONAL_HEADER.ImageBase
x86=Uc(UC_ARCH_X86,UC_MODE_32)
for uc,offset in ((arm,guest),(x86,0)):
    uc.mem_map(imagebase+offset,(len(image)+0xFFFF)&~0xFFFF)
    uc.mem_write(imagebase+offset,image)
x86.mem_map(0x10000,0x2F0000);x86.mem_map(stop,0x1000)
regs=[X.UC_X86_REG_EAX,X.UC_X86_REG_ECX,X.UC_X86_REG_EDX,X.UC_X86_REG_ESP,
      X.UC_X86_REG_EBX,X.UC_X86_REG_ESI,X.UC_X86_REG_EDI,X.UC_X86_REG_EBP]
rng=random.Random(0xA0039752)
def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def raw(v):return struct.pack('<I',v)
cases=0
pixels_checked=0
rejections=0
operations=[]
def compare(alpha,values,*,x=0,y=0,cw=0x37F,table=0xA10518B0,fpcr=0,expected_fast=True,
            alias=None,measure=False,label='',try_reject=False,field=None,strides=(0,0),live=0,
            backedges=0,hook=False,bias_override=None):
    global cases,pixels_checked,rejections
    n=len(values);self=0x11000;inp=0x20000;out=0x100000;sp=0x2A0000
    if alias=='input_output':out=inp
    if alias=='stack_output':out=sp-32
    if alias=='self_output':out=self
    if alias=='bias_output':out=table
    if alias=='scale_output':out=0xA0D9C79C
    if alias=='input_stack':inp=sp-64
    initial=[rng.getrandbits(32),self,rng.getrandbits(32),sp]+[rng.getrandbits(32) for _ in range(4)]
    obj=bytearray(rng.randbytes(128));struct.pack_into('<I',obj,4,28 if alpha else 22)
    for at,v in ((0x10,0),(0x20,out),(0x34,table),(0x54,0),(0x5C,0),(0x60,strides[0]),(0x64,strides[1]),(0x68,n)):
        struct.pack_into('<I',obj,at,v)
    if field:struct.pack_into('<I',obj,*field)
    payload=b''.join(struct.pack('<4I',*v) for v in values)
    stack=rng.randbytes(128)+struct.pack('<4I',stop,x,y,inp)+rng.randbytes(16)
    output=rng.randbytes(strides[0]*x+strides[1]*y+max(n*4,1)+64)
    xmm=rng.randbytes(128)
    for uc,offset in ((x86,0),(arm,guest)):
        for t in (0xA10518B0,0xA1051930):uc.mem_write(t+offset,image[t-imagebase:t-imagebase+128])
        uc.mem_write(0xA0D9C79C+offset,image[0xA0D9C79C-imagebase:0xA0D9C7A0-imagebase])
        uc.mem_write(out-32+offset,output);uc.mem_write(self+offset,bytes(obj))
        uc.mem_write(sp-128+offset,stack);uc.mem_write(inp+offset,payload)
        if alias=='scale_output':uc.mem_write(0xA0D9C79C+offset,image[0xA0D9C79C-imagebase:0xA0D9C7A0-imagebase])
        if bias_override is not None:uc.mem_write(table+4*((y&3)+8*(x&3))+offset,raw(bias_override))
    for r,v in zip(regs,initial):x86.reg_write(r,v)
    x86.reg_write(X.UC_X86_REG_EFLAGS,0x202)
    x86.reg_write(X.UC_X86_REG_FPCW,cw)
    x86.reg_write(X.UC_X86_REG_FPSW,0)
    x86.reg_write(X.UC_X86_REG_FPTAG,0xFFFF & ~((1<<(2*live))-1))
    for i in range(live):x86.reg_write(getattr(X,'UC_X86_REG_FP'+str(i)),((8+i)<<60,0x3FFF))
    for i in range(8):x86.reg_write(getattr(X,'UC_X86_REG_XMM'+str(i)),int.from_bytes(xmm[16*i:16*i+16],'little'))
    machine=bytearray(224)
    struct.pack_into('<8I',machine,0,*initial)
    struct.pack_into('<4I',machine,32,0,0,0,0)
    struct.pack_into('<I',machine,52,backedges)
    struct.pack_into('<i',machine,64,2)
    struct.pack_into('<H',machine,72,cw)
    struct.pack_into('<Q',machine,80,base+symbols['g_x87']+28*8)
    machine[96:224]=xmm
    arm.mem_write(base+symbols['g_m'],bytes(machine))
    fp_window=bytearray(rng.randbytes(1024))
    for i in range(live):struct.pack_into('<d',fp_window,(28+i)*8,1+i/8)
    arm.mem_write(base+symbols['g_x87'],bytes(fp_window))
    arm.mem_write(base+symbols['g_fp_bias'],raw(live))
    arm.mem_write(base+symbols['fallback_calls'],raw(0));arm.mem_write(base+symbols['helper_calls'],raw(0))
    arm.mem_write(base+symbols['g_enter_idx'],raw(0));arm.mem_write(base+symbols['g_enter_trace'],bytes(4096))
    arm.mem_write(base+symbols['recomp_yield_hook'],struct.pack('<Q',stop if hook else 0))
    arm.reg_write(A.UC_ARM64_REG_FPCR,fpcr);arm.reg_write(A.UC_ARM64_REG_FPSR,0x08000000)
    arm.reg_write(A.UC_ARM64_REG_SP,native_stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.reg_write(A.UC_ARM64_REG_X0,alpha);arm.reg_write(A.UC_ARM64_REG_X1,0)
    before={at:bytes(arm.mem_read(guest+at,size)) for at,size in
            ((self,128),(inp,len(payload)),(out-32,len(output)),(sp-128,len(stack)))} if measure else None
    if try_reject:
        arm.reg_write(A.UC_ARM64_REG_X1,1)
        arm.emu_start(base+symbols['check'],stop,count=1_000_000)
        assert arm.reg_read(A.UC_ARM64_REG_X0)==0,(label,'guard accepted')
        assert bytes(arm.mem_read(base+symbols['g_m'],224))==machine,(label,'rejected machine mutation')
        assert bytes(arm.mem_read(base+symbols['g_x87'],1024))==fp_window,(label,'rejected FPU mutation')
        for at,size in ((self,128),(inp,len(payload)),(out-32,len(output)),(sp-128,len(stack))):
            assert arm.mem_read(guest+at,size)==x86.mem_read(at,size),(label,'rejected guest mutation')
        assert arm.reg_read(A.UC_ARM64_REG_FPCR)==fpcr,(label,'rejected FPCR mutation')
        assert arm.reg_read(A.UC_ARM64_REG_FPSR)==0x08000000,(label,'rejected FPSR mutation')
        assert not struct.unpack('<I',arm.mem_read(base+symbols['fallback_calls'],4))[0]
        assert not struct.unpack('<I',arm.mem_read(base+symbols['helper_calls'],4))[0]
        assert not struct.unpack('<I',arm.mem_read(base+symbols['g_enter_idx'],4))[0]
        cases+=1
        rejections+=1
        return
    count={'x86':0,'native':0};hooks=[]
    if measure:
        for uc,key in ((x86,'x86'),(arm,'native')):
            uc.ctl_remove_cache(0xA0039752,0xA003AC60) if key=='x86' else uc.ctl_remove_cache(base,base+0x200000)
            hooks.append((uc,uc.hook_add(UC_HOOK_CODE,lambda u,a,s,d,key=key:count.__setitem__(key,count[key]+1))))
    x86.emu_start(0xA003AA9F if alpha else 0xA0039752,stop,count=50_000_000)
    arm.emu_start(base+symbols['check'],stop,count=50_000_000)
    for uc,h in hooks:uc.hook_del(h)
    assert x86.reg_read(X.UC_X86_REG_EIP)==stop,(label,'x86 runaway')
    assert arm.reg_read(A.UC_ARM64_REG_PC)==stop,(label,'ARM runaway')
    actual=struct.unpack('<8I',arm.mem_read(base+symbols['g_m'],32))
    expected=tuple(x86.reg_read(r) for r in regs)
    assert actual==expected,(label,'GPR',actual,expected)
    assert not (arm.reg_read(A.UC_ARM64_REG_X0)^x86.reg_read(X.UC_X86_REG_EFLAGS))&0x8D5,(label,'flags')
    for at,size in ((self,128),(inp,len(payload)),(out-32,len(output)),(sp-128,len(stack))):
        aa=bytes(arm.mem_read(guest+at,size));xx=bytes(x86.mem_read(at,size))
        if aa!=xx:
            k=next(i for i in range(size) if aa[i]!=xx[i])
            raise AssertionError((label,'memory',hex(at+k),aa[max(0,k-4):k+8].hex(),xx[max(0,k-4):k+8].hex()))
    state=bytes(arm.mem_read(base+symbols['g_m'],224))
    assert struct.unpack_from('<H',state,72)[0]==x86.reg_read(X.UC_X86_REG_FPCW)==cw,(label,'CW')
    assert x86.reg_read(X.UC_X86_REG_FPSW)&0x3800==0,(label,'x87 TOP')
    assert state[96:224]==xmm,(label,'XMM')
    assert struct.unpack_from('<i',state,64)[0]==2,(label,'fpu_cmp')
    if expected_fast:assert struct.unpack_from('<I',state,52)[0]==(backedges+n)&0xFFFFFFFF,(label,'backedges')
    assert struct.unpack_from('<Q',state,80)[0]==base+symbols['g_x87']+28*8,(label,'x87 pointer')
    assert arm.mem_read(base+symbols['g_x87']+28*8,64)==fp_window[28*8:36*8],(label,'live x87 window')
    for i in range(live):assert x86.reg_read(getattr(X,'UC_X86_REG_FP'+str(i)))==((8+i)<<60,0x3FFF),(label,'live original x87')
    assert arm.reg_read(A.UC_ARM64_REG_FPCR)==fpcr,(label,'host FPCR')
    calls=struct.unpack('<I',arm.mem_read(base+symbols['fallback_calls'],4))[0]
    assert arm.mem_read(base+symbols['g_enter_idx'],4)==raw(1),(label,'entry trace count')
    assert arm.mem_read(base+symbols['g_enter_trace'],4)==raw(0xA003AA9F if alpha else 0xA0039752),(label,'entry trace VA')
    assert bool(calls)!=expected_fast,(label,'guard',calls)
    if expected_fast:assert arm.reg_read(A.UC_ARM64_REG_FPSR)==0x08000000,(label,'host FPSR')
    cases+=1
    pixels_checked+=n
    if measure:
        for at,data in before.items():arm.mem_write(guest+at,data)
        arm.mem_write(base+symbols['g_m'],bytes(machine));arm.mem_write(base+symbols['g_x87'],bytes(fp_window))
        arm.mem_write(base+symbols['g_enter_idx'],raw(0));arm.mem_write(base+symbols['g_enter_trace'],bytes(4096))
        arm.reg_write(A.UC_ARM64_REG_FPCR,fpcr);arm.reg_write(A.UC_ARM64_REG_FPSR,0x08000000)
        arm.reg_write(A.UC_ARM64_REG_SP,native_stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
        arm.reg_write(A.UC_ARM64_REG_X0,alpha);arm.reg_write(A.UC_ARM64_REG_X1,2)
        count['lifted_arm64']=0
        arm.ctl_remove_cache(base,base+0x200000)
        h=arm.hook_add(UC_HOOK_CODE,lambda u,a,s,d:count.__setitem__('lifted_arm64',count['lifted_arm64']+1))
        arm.emu_start(base+symbols['check'],stop,count=50_000_000);arm.hook_del(h)
        assert struct.unpack('<8I',arm.mem_read(base+symbols['g_m'],32))==expected,(label,'lifted measure GPRs')
        for at,data in before.items():assert arm.mem_read(guest+at,len(data))==x86.mem_read(at,len(data)),(label,'lifted measure bytes')
        operations.append(dict(kernel='A8' if alpha else 'RGBX',pixels=n,**count))
        print(label,'instructions',count)

if __name__=='__main__':
    if SMALL_YIELD:
        for alpha in (0,1):
            for n in (1,7,4096):
                compare(alpha,[(bits(.25),)*4]*n,hook=True,try_reject=True,
                        label=f'yield threshold 1 {alpha} {n}')
        print('PASS RECOMP_YIELD_EVERY=1/non-null hook: six no-effect rejections')
        sys.exit(0)
    for alpha in (0,1):
        compare(alpha,[(bits(.25),bits(.5),bits(.75),bits(.5))],label=f'smoke {alpha}')
    for alpha in (0,1):
        for table in (0xA10518B0,0xA1051930):
            for parity in (0,1):
                for cw in (pc|rc|0x7F for pc in (0,0x200,0x300) for rc in (0,0x400,0x800,0xC00)):
                    for mode in (0,0x400000,0x800000,0xC00000):
                        row=[tuple(rng.randint(0x33800000,0x3F800000) if rng.randrange(4) else 0 for _ in range(4)) for _ in range(7)]
                        compare(alpha,row,x=0xBEEF0000+parity,y=0xCAFE0002,cw=cw,table=table,fpcr=mode,
                                label=f'environment {alpha} {table:x} {parity} {cw:x} {mode:x}')
    for alpha in (0,1):
        for n in (0,1,2,3,4,5,16,31,64,255,4096):
            for parity in (0,1):
                row=[tuple(bits(rng.randrange(256)/255) for _ in range(4)) for _ in range(n)]
                compare(alpha,row,x=parity,y=3,expected_fast=bool(n),label=f'count {alpha} {n} {parity}')
        for value in (0x80000000,0x80000001,1,0x337FFFFF,0x3F800001,0xBF800000,0x7F800000,0xFF800000,0x7FC00001,0x7F800001):
            row=[(value,)*4]
            compare(alpha,row,try_reject=True,label=f'reject value {alpha} {value:x}')
        for alias in ('input_output','stack_output','self_output','bias_output','input_stack','scale_output'):
            compare(alpha,[(bits(.25),)*4]*4,alias=alias,try_reject=True,label=f'alias {alpha} {alias}')
        for field in ((0x10,1),(0x54,1),(0x5C,1),(4,21),(0x68,4097),(0x34,0x18000)):
            compare(alpha,[(bits(.25),)*4],field=field,try_reject=True,label=f'unsupported {alpha} {field}')
        compare(alpha,[(bits(.25),)*4],cw=0x17F,try_reject=True,label='reserved precision')
        compare(alpha,[(bits(.25),)*4],cw=0x37E,try_reject=True,label='unmasked invalid')
        for v in (0x7F800000,0xFF800000,0x7FC00001,0x7F800001,0x80000000,bits(-.25),bits(2),bits(2**24)):
            compare(alpha,[(v,)*4],expected_fast=False,label=f'original fallback {alpha} {v:x}')
        for live in (0,2,4,6):
            for x in (0,1,2,3):
                compare(alpha,[(bits(.25),bits(.5),bits(.75),bits(1))]*8,
                        x=x,y=3,strides=(4096,256),live=live,label=f'offset/live {alpha} {x} {live}')
        compare(alpha,[(bits(.5),)*4],live=7,try_reject=True,label='x87 capacity')
        compare(alpha,[(bits(.25),)*4]*7,backedges=65530,hook=True,try_reject=True,label='yield boundary')
        compare(alpha,[(bits(.25),)*4]*7,backedges=23,label='backedge count')
        compare(alpha,[(bits(.25),)*4],bias_override=bits(.1),try_reject=True,label='off-grid bias')
        compare(alpha,[(bits(.25),)*4],bias_override=0x7F800001,try_reject=True,label='signaling NaN bias')
        unused=(0x7F800001,0x7FC01234,0xFF800000,bits(.5)) if alpha else (bits(.25),bits(.5),bits(.75),0x7F800001)
        compare(alpha,[unused]*4,label='unused components not evaluated')
    boundary=[]
    for k in range(255):
        mid=bits((k+.5)/255)
        boundary += [(v,)*4 for v in (mid-1,mid,mid+1)]
    for alpha in (0,1):
        for table in (0xA10518B0,0xA1051930):
            for x in (0,1):
                for pc in (0,0x200,0x300):
                    for mode in (0,0x400000,0x800000,0xC00000,0x3000000):
                        compare(alpha,boundary,x=x,y=2,cw=pc|0x7F,table=table,fpcr=mode,
                                label=f'byte thresholds {alpha} {table:x} {x} {pc:x} {mode:x}')
    for alpha in (0,1):
        row=[(bits(.25),bits(.5),bits(.75),bits((i&3)/4)) for i in range(256)]
        compare(alpha,row,table=0xA1051930,measure=True,label=f'256-pixel {alpha}')
    print('PASS original-x86/actual-ARM64 and no-effect rejection cases:',cases)
    small=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--small-yield'],cwd=ROOT,capture_output=True,text=True)
    (OUT/'small-yield.txt').write_text(small.stdout+small.stderr)
    assert small.returncode==0,small.stdout+small.stderr
    print(small.stdout.strip())
    profile=ROOT/'build/pgo/abb24fae-20261022.profdata'
    B.OUT=OUT/'patched'
    patched,_=B.override_source(ROOT/'generated/sims2-x86/sims2_A0038000.c',True)
    for kind,path,flags in (
        ('generated',patched,[f for f in B.GENERATED_FLAGS if f not in ('-w','-DRECOMP_GENERATED_CODE')]+
         ['-DRECOMP_LOCAL_REGS','-DRECOMP_PLAIN_MEMORY','-Igenerated/sims2-x86']),
        ('native',ROOT/'android/host/sims2_fast.c',B.HOST_FLAGS)):
        cmd=[str(B.CLANG),*flags,'-fprofile-instr-use='+str(profile),'-c',str(path),'-o',str(OUT/(kind+'.o'))]
        r=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
        (OUT/(kind+'.warnings.txt')).write_text(r.stderr)
        assert r.returncode==0,r.stderr
        if kind=='native':
            old=(OUT/'baseline/sims2_fast.c');old.parent.mkdir(exist_ok=True)
            old.write_text(path.read_text().split('/* The embedded D3DX X8R8G8B8/A8 row writers.')[0])
            baseline=subprocess.run([str(B.CLANG),*flags,'-fprofile-instr-use='+str(profile),'-c',str(old),'-o',str(OUT/'baseline.o')],cwd=ROOT,capture_output=True,text=True)
            (OUT/'baseline.warnings.txt').write_text(baseline.stderr)
            assert baseline.returncode==0,baseline.stderr
            def known_warning(text):return bool(re.fullmatch(r'warning: profile data may be out of date: of \d+ functions, 1 has mismatched data that will be ignored \[-Wprofile-instr-out-of-date\]\n1 warning generated\.\n',text))
            assert known_warning(r.stderr) and known_warning(baseline.stderr),(r.stderr,baseline.stderr)
            print('PASS complete native PGO unit: zero new warnings; same one pre-existing A015DC50 profile-hash warning as untouched baseline')
        else:
            assert not r.stderr.strip(),r.stderr
            print('PASS complete generated unit with production PGO: zero warnings')
    objdump=B.TOOLCHAIN/'bin/llvm-objdump.exe'
    (OUT/'native.s').write_text(subprocess.check_output([str(objdump),'-d',str(OUT/'native.o')],text=True))
    import json
    (OUT/'results.json').write_text(json.dumps(dict(cases=cases,pixels_checked=pixels_checked,
        no_effect_rejections=rejections,operation_counts=operations,
        small_yield_no_effect_rejections=6,
        generated_pgo_warnings=0,native_new_warnings=0,native_preexisting_profile_hash_warnings=1),indent=2))
    print(OUT)
