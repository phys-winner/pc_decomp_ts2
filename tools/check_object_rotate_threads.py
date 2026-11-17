"""Production shared rotate/slider state with real host threads and clocks.

Guest handler, references, messages and queue delivery are controlled boundaries.
Original lifted handler/ABI and actual ARM64 FIFO are covered by the two sibling
object-rotate helpers. No Android/device actions or production changes.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from build_android import CLANG, ROOT
import check_cas_slider_threads as S

OUT=ROOT/'build/test/object-rotate-v15c/threads'
GLOBALS=r'''
static int rot_width=1212,rot_height=540;
static _Atomic unsigned rot_refs,rot_starts,rot_stops,rot_adds,rot_releases,rot_picks,rot_allocs,rot_frees,rot_constructs,rot_destroys;
static _Atomic unsigned rot_block_stage,rot_blocked,rot_allow,rot_callback_mode;
static void rot_callback_change(void);
'''
GUEST=r'''
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 g_ecx=self;g_eax=0xFEED;
 if(va==0xA067AD3A){if(n||args)bad=112;return 80;}
 if(va==0x12345678){
  if(self!=0x200||n!=2||args[0]!=700||args[1]!=500||atomic_load(&rot_refs)!=2)bad=101;
  atomic_fetch_add(&rot_picks,1);
  if(atomic_load(&rot_block_stage)==2){atomic_store(&rot_blocked,1);while(!atomic_load(&rot_allow)){} }
  return 0x2000;
 }
 if(va==0xA067BE89){if(self!=0x1000||n)bad=102;return MEM32(0x1064)!=0;}
 if(va==0xA078D60A){if(self!=0x1000||n)bad=103;atomic_fetch_add(&rot_adds,1);return atomic_fetch_add(&rot_refs,1)+1;}
 if(va==0xA078D8E9){if(self!=0x1000||n||atomic_load(&rot_refs)!=2)bad=104;atomic_fetch_add(&rot_releases,1);return atomic_fetch_add(&rot_refs,(unsigned)-1)-1;}
 if(va==0xA01C033D){if(self!=0x6000||n)bad=105;atomic_fetch_add(&rot_constructs,1);return 1;}
 if(va==0xA01C0915){if(self!=0x6000||n!=1)bad=106;MEM32(self)=args[0];return 1;}
 if(va==0xA01C0874){if(self!=0x6000||n!=1||args[0]!=0xFFFFFFFFu)bad=107;return 1;}
 if(va==0xA01C0388){if(self!=0x6000||n)bad=108;atomic_fetch_add(&rot_destroys,1);return 1;}
 if(va==0xA067C356){
  if(self!=0x1000||n!=2||args[0]!=0x6000||args[1]||atomic_load(&rot_refs)!=2)bad=109;
  unsigned id=MEM32(0x6000);
  if(id==0xACB3A629u){
   atomic_fetch_add(&rot_starts,1);MEM8(0x113E)=1;
   if(atomic_load(&rot_block_stage)==1){atomic_store(&rot_blocked,1);while(!atomic_load(&rot_allow)){} }
   rot_callback_change();
  }else if(id==0x2FE3C0CEu){atomic_fetch_add(&rot_stops,1);MEM8(0x113E)=0;}else bad=110;
  return 1;
 }
 bad=111;return 0;
}
'''
CHECK=r'''
static uint64_t expected_token;
static void typed(uint32_t o,uint32_t v,uint32_t l,uint32_t d,const char *name){MEM32(o)=v;MEM32(v-4)=l;MEM32(l+12)=d;memcpy(GPTR(d+8),name,strlen(name)+1);}
static void rot_callback_change(void){
 if(atomic_load(&rot_callback_mode)){atomic_fetch_add(&cas_pan_epoch,1);MEM32(tool_handler+0x20)=0x3000;}
}
__declspec(dllexport) void prepare(void *clock){
 fixture_clock=clock;for(unsigned i=0;i<sizeof words/sizeof words[0];++i)words[i]=0;win_manager=0x100;win_manager_thread=thread_id=1;
 atomic_store(&touch_context,S2_TOUCH_BUY);touch_input_flags=0;tool_handler=0x5000;
 slider_state=SLIDER_IDLE;slider_serial=pick_serial=pick_done=pick_pending=fixture_barrier=0;ui_command_kind=ui_command_started=0;
 atomic_store(&cas_pan_epoch,7);rot_width=1212;rot_height=540;
 atomic_store(&rot_refs,1);atomic_store(&rot_starts,0);atomic_store(&rot_stops,0);atomic_store(&rot_adds,0);atomic_store(&rot_releases,0);
 atomic_store(&rot_picks,0);atomic_store(&rot_allocs,0);atomic_store(&rot_frees,0);atomic_store(&rot_constructs,0);atomic_store(&rot_destroys,0);
 atomic_store(&rot_block_stage,0);atomic_store(&rot_blocked,0);atomic_store(&rot_allow,0);atomic_store(&rot_callback_mode,0);
 setter_blocked=release_setter=hold_next_waiter=waiter_paused=allow_waiter=0;
 bad=0;g_eax=0xAA;g_ecx=0xBB;g_esp=0x1F00;shown_pointer=shown_vtable=tool_generation=0;atomic_store(&shown_tool,0);
 MEM32(0x144)=0x200;MEM32(0x200)=0x400;MEM32(0x4BC)=0x12345678;
 typed(0x1000,0xA0DE83FCu,0x7000,0x7100,".?AVcTSUserToolObject@@");
 MEM32(0x3000)=0xA0DE83FCu;MEM32(tool_handler+0x20)=0x1000;MEM32(0x1064)=MEM32(0x3064)=0x1700;
 typed(0x1700,0xA0DEBB54u,0x7800,0x7900,".?AVcTSUserToolUndoableObject@@");MEM32(0x1754)=1;MEM32(0x175C)=0x1800;
 typed(0x2000,0x2200,0x2400,0x2500,".?AVcTSWinSceneGraph@@");
 s2_touch_pump();expected_token=s2_touch_tool_token();
}
__declspec(dllexport) unsigned long long request(void){return s2_touch_rotate_object(700,500,expected_token,S2_TOUCH_BUY,((uint64_t)540<<32)|1212,7,-1);}
__declspec(dllexport) unsigned long long receipt(unsigned serial){uint64_t r=s2_touch_slider_status(serial);return r?r:(uint64_t)serial<<32;}
__declspec(dllexport) void owner(void){unsigned s=__c11_atomic_exchange(&fixture_barrier,0,5);if(s)s2_touch_barrier(s);}
__declspec(dllexport) void configure(unsigned stage){atomic_store(&rot_block_stage,stage);}
__declspec(dllexport) void change_during_start(void){atomic_store(&rot_callback_mode,1);}
__declspec(dllexport) void release(void){atomic_store(&rot_allow,1);}
__declspec(dllexport) void invalidate(unsigned kind){
 if(kind==0)atomic_fetch_add(&cas_pan_epoch,1);
 if(kind==1)MEM32(tool_handler+0x20)=0x3000;
 if(kind==2)rot_width=800;
 if(kind==3)atomic_store(&touch_context,S2_TOUCH_BUY|S2_TOUCH_MODAL);
 if(kind==4)MEM32(0x1064)=0;
}
__declspec(dllexport) unsigned busy_queries(void){
 unsigned s=pick_serial;
 if(s2_touch_pick(1,2)||s2_touch_ui_command(S2_UI_SCALE,200)||s2_touch_slider_step(700,500,1)!=S2_SLIDER_BUSY||pick_serial!=s)return 1;
 return 0;
}
__declspec(dllexport) unsigned state(unsigned k){
 if(k==0)return atomic_load(&fixture_barrier);if(k==1)return atomic_load(&rot_blocked);if(k==2)return atomic_load(&rot_refs);
 if(k==3)return atomic_load(&rot_starts);if(k==4)return atomic_load(&rot_stops);if(k==5)return atomic_load(&rot_adds);
 if(k==6)return atomic_load(&rot_releases);if(k==7)return atomic_load(&rot_allocs);if(k==8)return atomic_load(&rot_frees);
 if(k==9)return atomic_load(&rot_constructs);if(k==10)return atomic_load(&rot_destroys);if(k==11)return bad;
 if(k==12)return MEM8(0x113E);if(k==13)return g_eax==0xAA&&g_ecx==0xBB&&g_esp==0x1F00;return atomic_load(&rot_picks);
}
'''


def replace_function(source,signature,body):
    start=source.index(signature);opening=source.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[:start]+body+'\n'+source[end:]


def source():
    s=S.source();s=s[:s.index('static void typed(')]
    s=GLOBALS+s
    s=s.replace('static uint32_t words[8192];','static uint32_t words[65536];')
    s=s.replace('#define GPTR(a) ((uint8_t *)words+(uint32_t)(a))','#define GPTR(a) ((uint8_t *)words+((uint32_t)(a)&0x3FFFFu))')
    s=s.replace('#define MEM32(a) words[(uint32_t)(a)/4]','#define MEM32(a) (*(uint32_t *)GPTR(a))')
    s=s.replace('return readable && p+n<=sizeof words;', 'return readable && ((p>=0x100 && (uint64_t)p+n<=sizeof words)||(p>=0xA0DE0000u && (uint64_t)p+n<=0xA0DF0000ull));')
    s=replace_function(s,'static void void_input_screen_size(', 'static void void_input_screen_size(int *w,int *h){*w=rot_width;*h=rot_height;}')
    s=replace_function(s,'static uint32_t void_galloc(', 'static uint32_t void_galloc(uint32_t n){if(n!=44)bad=112;atomic_fetch_add(&rot_allocs,1);return 0x6000;}')
    s=replace_function(s,'static void void_gfree(', 'static void void_gfree(uint32_t p){if(p!=0x6000)bad=113;atomic_fetch_add(&rot_frees,1);}')
    s=replace_function(s,'static uint32_t void_call_guest_this(',GUEST)
    return s+CHECK


def main():
    OUT.mkdir(parents=True,exist_ok=True);cfile=OUT/'threads.c';cfile.write_text(source(),encoding='utf-8')
    cases=[]
    with tempfile.TemporaryDirectory(prefix='sims2-rotate-threads-') as folder:
        obj,library=Path(folder)/'check.obj',Path(folder)/'check.dll'
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-I'+str(ROOT/'android/host'),'-c',str(cfile),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(library),str(obj)],check=True)
        dll=ctypes.CDLL(str(library));workers=[];results={}
        clock_type=ctypes.CFUNCTYPE(ctypes.c_uint64,ctypes.c_int);now=clock_type(lambda k:time.monotonic_ns() if k==1 else time.time_ns())
        dll.prepare.argtypes=[ctypes.c_void_p];dll.request.restype=dll.receipt.restype=ctypes.c_uint64
        def run(name,fn):
            t=threading.Thread(target=lambda:results.__setitem__(name,fn()),daemon=True);workers.append(t);t.start();return t
        def join(t):t.join(3);assert not t.is_alive(),'native worker stuck'
        def wait(k):
            until=time.monotonic()+3
            while not dll.state(k):
                assert time.monotonic()<until,('boundary not reached',k)
                time.sleep(.001)
        def paired():
            assert [dll.state(k) for k in (2,3,4,5,6,7,8,9,10)]==[1]*9
            assert not dll.state(11) and not dll.state(12) and dll.state(13)
        try:
            dll.prepare(now);dll.configure(1);user=run('started',dll.request);wait(0);owner=run('owner',dll.owner);wait(1);join(user)
            r=results['started'];serial=r>>32;assert serial and r&255==1 and dll.receipt(serial)&255==1
            assert dll.request()&255==6 and not dll.busy_queries();assert dll.state(2)==2 and dll.state(3)==1 and not dll.state(4)
            dll.release();join(owner);assert dll.receipt(serial)&255==2;paired();dll.owner();assert dll.state(3)==dll.state(4)==1
            cases.append('STARTED beyond80ms gives same-serial IN_PROGRESS/BUSY; exactly one START/STOP, message/ref cleanup and no replay')

            dll.prepare(now);r=dll.request();assert r&255==1;time.sleep(2.05);assert dll.receipt(r>>32)&255==5;dll.owner();assert not dll.state(3) and not dll.state(4) and dll.state(2)==1 and not dll.state(7) and not dll.state(11)
            cases.append('QUEUED2s timeout then late owner has zero START/STOP/ref/message effects')

            dll.prepare(now);r=dll.request();assert r&255==1;time.sleep(.32);dll.owner()
            assert dll.receipt(r>>32)&255==2 and dll.state(3)==dll.state(4)==1 and not dll.state(11)
            cases.append('QUEUED owner delayed400ms executes exactly once with the original receipt')

            for invalid,label in enumerate(('epoch','tool identity','logical size','modal context','empty hand')):
                dll.prepare(now);user=run('invalid',dll.request);wait(0);dll.invalidate(invalid);dll.owner();join(user)
                assert results['invalid']&255==4 and not dll.state(3) and not dll.state(4) and dll.state(2)==1 and not dll.state(7) and not dll.state(11), (label,hex(results['invalid']),[dll.state(i) for i in range(14)])
                cases.append(label+' changes after enqueue: owner rejects without START or message allocation')

            for invalid,label in ((0,'epoch'),(1,'tool identity')):
                dll.prepare(now);dll.configure(2);user=run('pick_wait',dll.request);wait(0);owner=run('pick_owner',dll.owner);wait(1);join(user)
                serial=results['pick_wait']>>32;assert results['pick_wait']&255==1 and dll.state(2)==2
                dll.invalidate(invalid);dll.release();join(owner)
                assert dll.receipt(serial)&255==4 and not dll.state(3) and not dll.state(4) and dll.state(2)==1 and dll.state(5)==dll.state(6)==1 and not dll.state(7) and not dll.state(11)
                cases.append(label+' changes during STARTED callback-capable picker: retained old tool released, no START')

            dll.prepare(now);dll.configure(1);dll.change_during_start();user=run('replace_tool',dll.request);wait(0);owner=run('replace_owner',dll.owner);wait(1);join(user)
            serial=results['replace_tool']>>32;assert results['replace_tool']&255==1;dll.release();join(owner);assert dll.receipt(serial)&255==2;paired()
            cases.append('START callback changes epoch/current tool; STOP and release still target retained original exactly once')
        finally:
            dll.release()
            for t in workers:t.join(3)
            if all(not t.is_alive() for t in workers):
                ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.FreeLibrary(dll._handle)
    result={'pass':True,'real_host_thread_cases':cases,'production_sha256':hashlib.sha256((ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(cfile.read_bytes()).hexdigest(),
            'limits':'Actual shared native command/rotation helper state with real Windows threads/clocks/atomic locks. Guest methods, picker, message lifecycle, reference calls and queue delivery are strict boundaries; original handler/ref/message and ARM64 actual FIFO checked separately. No visible-pose/funds/camera/device acceptance.'}
    (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('PASS real-thread object rotation:',len(cases),'cases')


if __name__=='__main__':main()
