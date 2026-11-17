"""Execute production whole-field text C and original lifted QI/getters on ARM64.

The input owner command delivery and original setter bodies are explicit boundary
fixtures. Real FIFO transport is covered separately by the native UI owner tests;
the independently run original-x86 setter ABI oracle supplies the setter proof.
No Android/device interaction or production source changes.
"""
import hashlib
import io
import json
import re
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

import build_android as B
import check_touch_barrier as T
from check_virtual_query import function

OUT = B.ROOT / 'build/test/native-text-v15'

EXTRA = r'''
static uint32_t manager=0x11000,hit_window=0x13000;
static unsigned refs,adds,releases,setters,notifications,allocations,frees,fail_alloc;
static unsigned scale=200,width=1212,height=540,held_keys,queued_kind,queued_serial,queue_accept=1;
static unsigned setter_mode,first_setter_va,last_setter_args,read_getters,qi_calls;
static uint64_t during_receipt;static unsigned nested_busy,retained_during_setter;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
void (*recomp_yield_hook)(void);
static uint32_t active_vtable;
static void value_set(const uint8_t *s,unsigned n){
    uint32_t object=hit_window+0x100;
    if(active_vtable==0xA0E583B8u){MEM32(hit_window+0x110)=0xA0D9B470u;object=hit_window+0x114;}
    memset(GPTR(object),0,24);
    if(n<=15){memcpy(GPTR(object),s,n);MEM8(object+n)=0;MEM32(object+20)=15;}
    else{MEM32(object)=0x23000;memcpy(GPTR(0x23000),s,n);MEM8(0x23000+n)=0;MEM32(object+20)=512;}
    MEM32(object+16)=n;
}
static void thunk_add(void){if(g_ecx!=hit_window||lock_depth)bad=10;++adds;g_eax=++refs;g_esp+=4;}
static void thunk_release(void){if((g_ecx!=0x130F8&&g_ecx!=0x13000)||lock_depth||!refs)bad=11;++releases;g_eax=--refs;g_esp+=4;}
static void thunk_pick(void){
    if(g_ecx!=MEM32(manager+0x44)||ARG(0)!=700||ARG(1)!=500||lock_depth)bad=12;
    ++picker_calls;g_eax=hit_window;g_esp+=12;
}
static void setter(unsigned n,uint32_t va){
    if(g_ecx!=0x130F8||lock_depth||refs!=2)bad=13;
    ++setters;first_setter_va=va;last_setter_args=n;
    uint32_t bytes=ARG(0);unsigned length=n==3?ARG(1):(unsigned)strlen(GPTR(bytes));
    if(n==3&&ARG(2)!=1)bad=14;
    if(setter_mode==2){
        during_receipt=s2_touch_text_status(text_serial);
        nested_busy=(unsigned)s2_touch_text_replace((const uint8_t *)"nested",6);
        s2_touch_cancel_text();s2_touch_text_pump(manager);retained_during_setter=refs;
    }
    if(setter_mode!=3)value_set(GPTR(bytes),length);
    ++notifications;
    if(setter_mode==1)context^=S2_TOUCH_MODAL;
    if(setter_mode==4)hit_window=0x14000;
    if(setter_mode==5)MEM32(0x130F8)=0;
    if(setter_mode==6)MEM32(0x13000)=0;
    g_eax=1;g_esp+=4*(n+1);
}
static void thunk_set_edit(void){setter(3,0xA09520EDu);}
static void thunk_set_line(void){setter(1,0xA095ED6Du);}
static void thunk_bad(void){bad=15;g_eax=0;g_esp+=4;}
void sub_A095079A(void);void sub_A095E9E7(void);void sub_A01C1C80(void);
void sub_A0148BA0(void);void sub_A01ACD4F(void);void sub_A0149D00(void);
static void original_qi_edit(void){++qi_calls;sub_A095079A();}
static void original_qi_line(void){++qi_calls;sub_A095E9E7();}
static void original_get_edit(void){++read_getters;sub_A01C1C80();}
static void original_get_line(void){++read_getters;sub_A01ACD4F();}
recomp_func_t void_resolve(uint32_t va){switch(va){
case 0xA095079A:return original_qi_edit;case 0xA095E9E7:return original_qi_line;
case 0xA01C1C80:return original_get_edit;case 0xA0148BA0:return sub_A0148BA0;
case 0xA01ACD4F:return original_get_line;case 0xA0149D00:return sub_A0149D00;
case 0xA09520ED:return thunk_set_edit;case 0xA095ED6D:return thunk_set_line;
case 0x701:return thunk_add;case 0x702:return thunk_release;case 0x703:return thunk_pick;
default:return thunk_bad;}}
void void_lift_trap(uint32_t va,const char *text){(void)va;(void)text;bad=16;}
static void drive(void){
    if(!queued_serial)return;
    unsigned kind=queued_kind,serial=queued_serial;queued_kind=queued_serial=0;
    if(mode==2)mono_ns+=500000000;
    if(mode==3)mono_ns+=400000000;
    void_regs save;void_regs_save(&save);s2_touch_text_owner(kind,serial,manager);void_regs_load(&save);
}
uint32_t cases,checks;
#define REQUIRE(c) do{++checks;if(!(c))return __LINE__;}while(0)
static const uint8_t cyrillic[]={0xD0,0xA1,0xD0,0xB5,0xD0,0xBC,0xD1,0x8C,0xD1,0x8F,0x20,0xD0,0x81,0xD0,0xB6,0};
static void setup(unsigned type,const uint8_t *value,unsigned length){
    memset(GPTR(0x10000),0,0x20000);memset(&g_m,0,sizeof g_m);memset(&g_x87,0,sizeof g_x87);
    g_stp=g_stbuf+X87_CENTRE;g_fp_bias=3;g_eax=0xAA;g_ecx=0xBB;g_edx=0xCC;g_ebx=0xDD;
    g_esp=0x2E000;g_ebp=0x2F000;g_esi=0xEE;g_edi=0xFF;g_fs_base=0x1234;g_cur_func=0xABCD;
    g_flag_k=FK_CMP;g_flag_a=123;g_flag_b=45;g_flag_cf=1;g_mxcsr=0x1F80;g_fpu_cw=0x27F;void_fpu_cmp=1;
    g_eh_frames=(void *)(uintptr_t)0x1111;g_eh_obj=222;g_eh_info=333;
    for(unsigned i=0;i<8;++i){g_stp[i]=i*.25;X87_SHADOW(g_stp)[i]=i+400;g_mm[i]=i+500;memset(&g_xmm[i],i,sizeof g_xmm[i]);}
    refs=1;adds=releases=setters=notifications=allocations=frees=fail_alloc=0;
    queued_kind=queued_serial=0;queue_accept=1;bad=waits=picker_calls=read_getters=qi_calls=0;
    scale=200;width=1212;height=540;held_keys=0;mode=1;mono_ns=time_fail=0;lock_depth=0;
    manager=0x11000;hit_window=0x13000;context=S2_TOUCH_CAS|S2_TOUCH_FOCUSED_TEXT;input_context=0;
    text_serial=text_state=text_kind=text_result=text_epoch_request=text_abandoned=0;
    memset(&text_session,0,sizeof text_session);atomic_store(&text_epoch,0);
    setter_mode=first_setter_va=last_setter_args=nested_busy=retained_during_setter=0;during_receipt=0;
    MEM32(manager+0x44)=0x12000;MEM32(manager+0x38)=hit_window;
    MEM32(0x12000)=0x12200;MEM32(0x122BC)=0x703;MEM32(0x120E8)=3;
    MEM32(hit_window+0x58)=0x12000;MEM32(hit_window+0xE8)=0x8003;
    MEM32(hit_window+0x160)=1;MEM8(hit_window+0x165)=1;
    active_vtable=type?0xA0E583B8u:0xA0E56138u;MEM32(hit_window)=active_vtable;
    MEM32(active_vtable+4)=0x701;MEM32(active_vtable+8)=0x702;
    MEM32(hit_window+0xF8)=type?0xA0E58670u:0xA0E56404u;
    MEM32(MEM32(hit_window+0xF8)+8)=0x702;
    // The caption is deliberately not the editable value.
    memcpy(GPTR(0x22000),"DIFFERENT CAPTION",18);MEM32(hit_window+0x18)=0x22000;
    value_set(value,length);
}
static int read_value(const uint8_t *expected,unsigned n){
    uint8_t out[513];unsigned count=999;void_regs before={0},after={0};void_regs_save(&before);
    int ok=s2_touch_text_read(700,500,out,&count);void_regs_save(&after);
    return ok&&count==n&&!memcmp(out,expected,n)&&!out[n]&&refs==2&&adds==1&&!releases&&!bad&&
        !lock_depth&&!memcmp(&before,&after,sizeof before);
}
static int unchanged_bank_replace(const uint8_t *value,unsigned n,unsigned expected){
    void_regs before={0},after={0};void_regs_save(&before);uint64_t receipt=s2_touch_text_replace(value,n);void_regs_save(&after);
    return (receipt&255)==expected&&(receipt>>32)&&s2_touch_text_status((unsigned)(receipt>>32))==receipt&&
        !memcmp(&before,&after,sizeof before)&&!bad&&!lock_depth;
}
uint32_t selftest(void){
    for(unsigned type=0;type<2;++type){
        setup(type,cyrillic,15);REQUIRE(read_value(cyrillic,15));
        REQUIRE(unchanged_bank_replace((const uint8_t *)"paste-middle",12,S2_SLIDER_CHANGED));
        REQUIRE(refs==1&&adds==1&&releases==1&&setters==1&&notifications==1&&allocations==frees);
        REQUIRE(first_setter_va==(type?0xA095ED6Du:0xA09520EDu)&&last_setter_args==(type?1:3));++cases;
        setup(type,(const uint8_t *)"old",3);mode=3;REQUIRE(read_value((const uint8_t *)"old",3));
        REQUIRE(unchanged_bank_replace((const uint8_t *)"delayed",7,S2_SLIDER_CHANGED));
        REQUIRE(setters==1&&refs==1&&releases==1&&mono_ns==800000000);drive();REQUIRE(setters==1);++cases;
        setup(type,(const uint8_t *)"abc",3);REQUIRE(read_value((const uint8_t *)"abc",3));
        REQUIRE(unchanged_bank_replace(NULL,0,S2_SLIDER_CHANGED));REQUIRE(setters==1&&refs==1&&releases==1);++cases;
        setup(type,NULL,0);REQUIRE(read_value(NULL,0));
        REQUIRE(unchanged_bank_replace(cyrillic,15,S2_SLIDER_CHANGED));REQUIRE(setters==1&&refs==1&&releases==1);++cases;
        setup(type,cyrillic,15);REQUIRE(read_value(cyrillic,15));
        REQUIRE(unchanged_bank_replace(cyrillic,15,S2_SLIDER_LIMIT));REQUIRE(!setters&&!notifications&&refs==1&&releases==1);++cases;
        // Short/long storage paths both execute actual original getters.
        uint8_t longvalue[129];memset(longvalue,'A',128);longvalue[128]=0;
        setup(type,longvalue,128);REQUIRE(read_value(longvalue,128));s2_touch_cancel_text();s2_touch_text_pump(manager);REQUIRE(refs==1&&releases==1);++cases;
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));
        void_regs bank_before={0},bank_after={0};void_regs_save(&bank_before);
        s2_touch_cancel_text();REQUIRE(refs==2);s2_touch_text_pump(manager);void_regs_save(&bank_after);
        REQUIRE(refs==1&&releases==1&&!bad&&!memcmp(&bank_before,&bank_after,sizeof bank_before));++cases;
        setup(type,(const uint8_t *)"\xED\xA0\x80",3);uint8_t invalid_out[513];unsigned invalid_n=0;
        REQUIRE(!s2_touch_text_read(700,500,invalid_out,&invalid_n));REQUIRE(refs==1&&adds==1&&releases==1&&!setters&&!bad);++cases;
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));queue_accept=0;
        REQUIRE((s2_touch_text_replace((const uint8_t *)"new",3)&255)==S2_SLIDER_BUSY);REQUIRE(refs==2&&!setters&&!queued_serial);
        s2_touch_cancel_text();s2_touch_text_pump(manager);REQUIRE(refs==1&&!bad);++cases;
        for(unsigned reject=0;reject<12;++reject){
            setup(type,(const uint8_t *)"old",3);
            if(reject==0){if(type)MEM8(0x13165)=0;else MEM32(0x13160)=0;}
            if(reject==1)MEM32(0x130E8)&=~0x8000u;
            if(reject==2)MEM32(0x130E8)&=~1u;
            if(reject==3)MEM32(0x120E8)=1;
            if(reject==4)MEM32(manager+0x34)=0x13000;
            if(reject==5)MEM32(manager+0x38)=0x12000;
            if(reject==6)held_keys=0x8000;
            if(reject==7)context|=S2_TOUCH_LOADING;
            if(reject==8)MEM32(0x13058)=0;
            if(reject==9)MEM32(0x13000)=0;
            if(reject==10)fail_alloc=1;
            if(reject==11)MEM32(0x130F8)=0;
            uint8_t out[513];unsigned length=0;REQUIRE(!s2_touch_text_read(700,500,out,&length));REQUIRE(!setters&&refs==1&&!bad);++cases;
        }
        for(unsigned stale=0;stale<10;++stale){
            setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));
            if(stale==0)context^=S2_TOUCH_MODAL;if(stale==1)++scale;if(stale==2)++width;if(stale==3)++height;
            if(stale==4)MEM32(manager+0x44)=0;if(stale==5)value_set((const uint8_t *)"new",3);
            if(stale==6)hit_window=0x14000;if(stale==7)s2_touch_cancel_text();
            if(stale==8)MEM32(manager+0x38)=0;if(stale==9)MEM32(0x130E8)=0;
            REQUIRE(unchanged_bank_replace((const uint8_t *)"edited",6,S2_SLIDER_REJECTED));REQUIRE(!setters&&refs==1&&releases==1);++cases;
        }
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));mode=0;
        uint64_t timeout=s2_touch_text_replace((const uint8_t *)"late",4);REQUIRE((timeout&255)==S2_SLIDER_CANCELLED&&refs==2&&!setters);mode=1;drive();REQUIRE(!setters&&refs==2);s2_touch_cancel_text();s2_touch_text_pump(manager);REQUIRE(refs==1);++cases;
        setup(type,(const uint8_t *)"old",3);mode=0;uint8_t out[513];unsigned n=0;
        REQUIRE(!s2_touch_text_read(700,500,out,&n));mode=1;drive();REQUIRE(refs==1&&!adds&&!setters);++cases;
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));mode=2;
        REQUIRE(unchanged_bank_replace((const uint8_t *)"late",4,S2_SLIDER_CANCELLED));REQUIRE(!setters&&refs==2);s2_touch_cancel_text();s2_touch_text_pump(manager);REQUIRE(refs==1);++cases;
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));setter_mode=2;
        REQUIRE(unchanged_bank_replace((const uint8_t *)"once",4,S2_SLIDER_FAILED));
        REQUIRE((during_receipt&255)==S2_SLIDER_IN_PROGRESS&&(during_receipt>>32)==text_serial&&nested_busy==S2_SLIDER_BUSY&&retained_during_setter==2&&setters==1&&refs==1);++cases;
        for(unsigned mode=1;mode<=6;++mode){
            if(mode==2)continue;setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));setter_mode=mode;
            // LineInput reads its still-validated embedded GZString directly, not the interface VT.
            unsigned expected=mode==5&&type?S2_SLIDER_CHANGED:S2_SLIDER_FAILED;
            REQUIRE(unchanged_bank_replace((const uint8_t *)"changed",7,expected));REQUIRE(setters==1&&refs==1&&releases==1);++cases;
        }
        setup(type,(const uint8_t *)"old",3);context=0;REQUIRE(read_value((const uint8_t *)"old",3));REQUIRE(unchanged_bank_replace((const uint8_t *)"save",4,S2_SLIDER_CHANGED));++cases;
    }
    uint8_t bytes[513];memset(bytes,'A',sizeof bytes);
    REQUIRE(valid_utf8(bytes,128));REQUIRE(!valid_utf8(bytes,129));REQUIRE(valid_utf8(NULL,0));
    const uint8_t *invalid[]={ (const uint8_t *)"\0",(const uint8_t *)"\xC0\x80",(const uint8_t *)"\xED\xA0\x80",(const uint8_t *)"\xF4\x90\x80\x80",(const uint8_t *)"\xE2\x82",(const uint8_t *)"\x80",(const uint8_t *)"\xF0\x80\x80\x80"};
    unsigned lengths[]={1,2,3,4,2,1,4};
    for(unsigned i=0;i<7;++i){REQUIRE(!valid_utf8(invalid[i],lengths[i]));REQUIRE((s2_touch_text_replace(invalid[i],lengths[i])&255)==S2_SLIDER_REJECTED);++cases;}
    for(unsigned i=0;i<64;++i){bytes[i*4]=0xF0;bytes[i*4+1]=0x9F;bytes[i*4+2]=0x98;bytes[i*4+3]=0x80;}
    REQUIRE(valid_utf8(bytes,256));memcpy(bytes+256,bytes,4);REQUIRE(!valid_utf8(bytes,260));
    for(unsigned type=0;type<2;++type){
        setup(type,(const uint8_t *)"old",3);REQUIRE(read_value((const uint8_t *)"old",3));
        REQUIRE(unchanged_bank_replace(bytes,256,S2_SLIDER_CHANGED));REQUIRE(refs==1&&setters==1&&!bad);++cases;
    }
    for(unsigned i=0;i<128;++i){bytes[i*3]=0xE4;bytes[i*3+1]=0xB8;bytes[i*3+2]=0xAD;}
    REQUIRE(valid_utf8(bytes,384));
    setup(0,bytes,384);REQUIRE(read_value(bytes,384));s2_touch_cancel_text();s2_touch_text_pump(manager);REQUIRE(refs==1&&!bad);++cases;
    REQUIRE((s2_touch_text_replace(bytes,513)&255)==S2_SLIDER_REJECTED);REQUIRE((s2_touch_text_replace(NULL,1)&255)==S2_SLIDER_REJECTED);++cases;
    REQUIRE(s2_touch_text_status(0)==0);REQUIRE(s2_touch_text_status(0xDEADBEEFu)==0xDEADBEEF00000000ull);++cases;
    return 0;
}
'''


def source():
    prelude = T.PRELUDE[:T.PRELUDE.index('typedef struct {uint32_t hwnd')]
    for signature in ('void s2_touch_cancel_text(', 'void s2_touch_text_pump(', 'void s2_touch_text_owner(', 'int s2_touch_text_editable(',
                      'void void_input_screen_size(', 'uint32_t void_galloc(', 'void void_gfree('):
        prelude = re.sub(r'^'+re.escape(signature)+r'.*\n','',prelude,flags=re.M)
    prelude = prelude.replace('static void drive(void);', 'static void drive(void);\nstatic unsigned mono_ns,time_fail;')
    prelude = prelude.replace('t->tv_sec=10;t->tv_nsec=0;return 0;', 't->tv_sec=10;t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns:0;return time_fail;')
    prelude = prelude.replace('long budget=(c==&pick_ready && slider_state!=1 && slider_state!=2)?500000000:80000000;', 'long budget=500000000;')
    prelude = prelude.replace('return a>=0x10000 && (uint64_t)a+n<=0x30000;',
                              'return (a>=0x10000 && (uint64_t)a+n<=0x30000)||(a>=0xA0E50000u && (uint64_t)a+n<=0xA0E60000u);')
    prelude = prelude.replace('int16_t void_input_key_state(uint32_t key){(void)key;return 0;}', 'static unsigned held_keys;int16_t void_input_key_state(uint32_t key){(void)key;return held_keys;}')
    prelude = prelude.replace('int s2_ui_scale(void){return 200;}', 'static unsigned scale;int s2_ui_scale(void){return scale;}')
    prelude += r'''
static uint32_t manager;static unsigned width,height,queue_accept,queued_kind,queued_serial,allocations,frees,fail_alloc;
uint32_t s2_touch_owner_manager(void){return manager;}
void void_input_screen_size(int *w,int *h){*w=width;*h=height;}
int s2_touch_ui_command(unsigned kind,uint32_t serial){if(!queue_accept)return 0;queued_kind=kind;queued_serial=serial;return 1;}
uint32_t void_galloc(uint32_t n){(void)n;if(fail_alloc)return 0;++allocations;return 0x26000;}
void void_gfree(uint32_t a){if(a!=0x26000)bad=9;++frees;}
'''
    paths = {name:(B.ROOT/f'android/host/{name}.c').read_text(encoding='utf-8') for name in ('core','threads','sims2_text')}
    actual = ''.join(function(paths['core'],s) for s in ('void void_regs_save(','void void_regs_load('))
    actual += '\n'.join(line for line in paths['sims2_text'].splitlines() if not line.startswith('#include'))
    actual += ''.join(function(paths['threads'],s) for s in ('uint32_t void_call_guest(','uint32_t void_call_guest_this('))
    lifted = ''
    for chunk,name in [('A0950000','A095079A'),('A0958000','A095E9E7'),('A01C0000','A01C1C80'),
                       ('A0148000','A0148BA0'),('A01A8000','A01ACD4F'),('A0148000','A0149D00')]:
        lifted += function((B.ROOT/f'generated/sims2-x86/sims2_{chunk}.c').read_text(),f'void sub_{name}(void) {{')
    regs=('eax','ecx','edx','ebx','esp','ebp','esi','edi')
    lifted='\n'.join(f'#define {r} g_{r}' for r in regs)+'\n#define _xmm g_xmm\n'+lifted
    lifted+='\n'.join(f'\n#undef {r}' for r in regs)+'\n#undef _xmm\n'
    return prelude+actual+EXTRA+lifted


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    cfile,binary=OUT/'check.c',OUT/'check.elf'
    cfile.write_text(source(),encoding='utf-8')
    subprocess.run([str(B.CLANG),*B.COMMON,'-O2','-fno-builtin','-fno-stack-protector','-mno-outline-atomics','-nostdlib',
                    '-Iandroid/host','-Iandroid/runtime','-Wl,--gc-sections,-e,selftest',str(cfile),'-o',str(binary)],check=True)
    elf=ELFFile(io.BytesIO(binary.read_bytes()))
    symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);base,guest,stack,stop=0x10000000,0x2000000000,0x50000000,0x60000000
    for addr,size in [(base,0x1000000),(guest+0x10000,0x20000),(guest+0xA0E50000,0x10000),(stack,0x10000),(stop,0x1000)]:uc.mem_map(addr,size)
    for seg in elf.iter_segments():
        if seg['p_type']=='PT_LOAD':uc.mem_write(base+seg['p_vaddr'],seg.data())
    reloc=elf.get_section_by_name('.rela.dyn')
    if reloc:
        for r in reloc.iter_relocations():
            assert r['r_info_type']==1027
            uc.mem_write(base+r['r_offset'],(base+r['r_addend']).to_bytes(8,'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0,stack+0x100);uc.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);uc.reg_write(A.UC_ARM64_REG_LR,stop)
    uc.emu_start(base+symbols['selftest'],stop,count=100000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC)==stop
    failure=uc.reg_read(A.UC_ARM64_REG_X0)
    state={n:int.from_bytes(uc.mem_read(base+symbols[n],4),'little') for n in ('cases','checks','bad','refs','setters','adds','releases')}
    assert failure==0,('C assertion line',failure,state)
    result={'pass':True,**state,'actual':'Production sims2_text.c; actual core register/EH/x87/SIMD save/restore; actual guest-call arity checks; six original lifted QI/getter bodies for TextEdit and LineInput.',
            'boundaries':'Owner-command dispatch and OS timedwait are controlled boundaries. Original setter bodies are strict3arg/1arg mutation/notification mocks, not original game rendering/UI. Separate raw-original-x86 setter oracle and real owner FIFO tests are required. No device acceptance.',
            'production_sha256':hashlib.sha256((B.ROOT/'android/host/sims2_text.c').read_bytes()).hexdigest(),
            'fixture_sha256':hashlib.sha256(cfile.read_bytes()).hexdigest()}
    (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS native full-field ARM64:',state['cases'],'groups,',state['checks'],'assertions; UTF8, QI/getters, identity, refs, deadline and exactly-once guards')


if __name__=='__main__':main()
