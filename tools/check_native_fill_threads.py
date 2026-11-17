"""Semantic Fill transport with real Windows threads/clocks; no device actions.

The actual shared native command request/owner state and Fill helper execute. Guest
mode/event/reference calls and queue delivery are strict boundary mocks; sibling
check_native_fill.py exercises the original lifted handlers and their ABI.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

import check_object_rotate_threads as R
from build_android import CLANG, ROOT

OUT=ROOT/'build/test/room-fill-v15d/threads'
GUEST=r'''
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 g_ecx=self;g_eax=0xFEED;
 if(va==0x12345678){
  if(self!=0x200||n!=2||args[0]!=700||args[1]!=500||atomic_load(&rot_refs)!=2||MEM32(0x10C)!=2||MEM32(0x80FC)!=2)bad=201;
  atomic_fetch_add(&rot_picks,1);
  if(atomic_load(&rot_block_stage)==2){atomic_store(&rot_blocked,1);while(!atomic_load(&rot_allow)){} }
  return 0x8000;
 }
 if(va==0xA078D60A){if(self!=0x1000||n)bad=202;atomic_fetch_add(&rot_adds,1);return atomic_fetch_add(&rot_refs,1)+1;}
 if(va==0xA078D8E9){if(self!=0x1000||n||atomic_load(&rot_refs)!=2)bad=203;atomic_fetch_add(&rot_releases,1);return atomic_fetch_add(&rot_refs,(unsigned)-1)-1;}
 if(va==0xA0145240){if(self!=0x100||n||MEM32(0x10C)!=1)bad=210;atomic_fetch_add(&manager_add,1);return ++MEM32(0x10C);}
 if(va==0xA01A85B0){if(self!=0x100||n||MEM32(0x10C)!=2||atomic_load(&rot_refs)!=1||MEM32(0x80FC)!=1)bad=211;atomic_fetch_add(&manager_release,1);return --MEM32(0x10C);}
 if(va==0xA05B84E8){if(self!=0x8000||n||MEM32(0x80FC)!=1)bad=212;atomic_fetch_add(&scene_add,1);return ++MEM32(0x80FC);}
 if(va==0xA05BBE91){if(self!=0x8000||n||MEM32(0x80FC)!=2||atomic_load(&rot_refs)!=1||MEM32(0x10C)!=2)bad=213;atomic_fetch_add(&scene_release,1);return --MEM32(0x80FC);}
 if(va==0xA097340A){if(self!=0x100||n!=1||args[0]!=0x8000||MEM32(0x10C)!=2||MEM32(0x80FC)!=2)bad=214;atomic_fetch_add(&capture_release,1);if(MEM32(0x134)==args[0])MEM32(0x134)=0;return 1;}
 if(va==0xA0685954||va==0xA0693287){
  if(self!=0x1000||n!=1||(args[0]!=0x10003&&args[0]!=0x10001)||!MEM32(self+painter_slot))bad=204;
  if(va!=(fill_kind==1?0xA0685954u:0xA0693287u))bad=205;
  MEM32(self+mode_slot)=(args[0]&0xFFFF)-1;if(fill_kind==1)MEM32(self+0x90)=MEM32(self+mode_slot);
  if(args[0]==0x10001)atomic_fetch_add(&fill_restore,1);return 1;
 }
 if(va==0xA078DC91){
  if(self!=0x1000||n!=4||args[0]!=1||args[2]!=0x6000||args[3]||atomic_load(&rot_refs)!=2)bad=206;
  if(MEM32(0x6000)!=700||MEM32(0x6004)!=500||MEM32(self+mode_slot)!=2)bad=207;
  if(args[1]==4){
   atomic_fetch_add(&rot_starts,1);MEM8(self+0x10)=MEM8(self+0x1C)=1;MEM32(0x134)=0x8000;
   if(atomic_load(&rot_block_stage)==1){atomic_store(&rot_blocked,1);while(!atomic_load(&rot_allow)){} }
   fill_callback_change();
  }else if(args[1]==5){
   atomic_fetch_add(&rot_stops,1);if(MEM32(self+painter_slot))atomic_fetch_add(&fill_effect,1);
   MEM8(self+0x10)=MEM8(self+0x1C)=0;MEM32(0x134)=0;
  }else bad=208;
  return 1;
 }
 bad=209;return 0;
}
'''
CHECK=r'''
static uint64_t expected_token;
static void typed(uint32_t o,uint32_t v,uint32_t l,uint32_t d,const char *name){MEM32(o)=v;MEM32(v-4)=l;MEM32(l+12)=d;memcpy(GPTR(d+8),name,strlen(name)+1);}
static void fill_callback_change(void){
 unsigned change=atomic_load(&rot_callback_mode);
 if(change==1)atomic_store(&touch_context,S2_TOUCH_BUILD|S2_TOUCH_MODAL);
 if(change==2){MEM32(tool_handler+0x20)=0x3000;MEM32(0x1000+painter_slot)=0;}
 if(change==3)atomic_fetch_add(&cas_pan_epoch,1);
 if(change==4)MEM32(0x1004)=0;
}
__declspec(dllexport) void prepare(void *clock,unsigned kind){
 fixture_clock=clock;for(unsigned i=0;i<sizeof words/sizeof words[0];++i)words[i]=0;
 win_manager=0x100;win_manager_thread=thread_id=1;tool_handler=0x5000;
 atomic_store(&touch_context,S2_TOUCH_BUILD);touch_input_flags=0;
 slider_state=SLIDER_IDLE;slider_serial=pick_serial=pick_done=pick_pending=fixture_barrier=0;ui_command_kind=ui_command_started=0;
 atomic_store(&cas_pan_epoch,7);rot_width=1212;rot_height=540;
 atomic_store(&rot_refs,1);atomic_store(&rot_starts,0);atomic_store(&rot_stops,0);atomic_store(&rot_adds,0);atomic_store(&rot_releases,0);
 atomic_store(&rot_picks,0);atomic_store(&rot_allocs,0);atomic_store(&rot_frees,0);atomic_store(&fill_restore,0);atomic_store(&fill_effect,0);
 atomic_store(&manager_add,0);atomic_store(&manager_release,0);atomic_store(&scene_add,0);atomic_store(&scene_release,0);atomic_store(&capture_release,0);
 atomic_store(&rot_block_stage,0);atomic_store(&rot_blocked,0);atomic_store(&rot_allow,0);atomic_store(&rot_callback_mode,0);
 setter_blocked=release_setter=hold_next_waiter=waiter_paused=allow_waiter=0;
 bad=0;g_eax=0xAA;g_ecx=0xBB;g_esp=0x1F00;shown_pointer=shown_vtable=tool_generation=0;atomic_store(&shown_tool,0);
 MEM32(win_manager)=0xA0E5B240;MEM32(win_manager+0xC)=1;
 MEM32(0x144)=0x200;MEM32(0x200)=0x400;MEM32(0x4BC)=0x12345678;MEM8(win_manager+0x1E0)=1;
 fill_kind=kind;painter_slot=kind==1?0x64:0x50;mode_slot=kind==1?0x8C:0x58;
 typed(0x1000,kind==1?0xA0DE8DA8u:0xA0DE9DCCu,0x7000,0x7100,
   kind==1?".?AVcTSUserToolFloorPattern@@":".?AVcTSUserToolWallpaper@@");
 MEM32(tool_handler+0x20)=0x1000;MEM32(tool_handler+0x28)=0x10001;
 MEM32(0x1004)=0x80F8;MEM32(0x1020)=1;MEM32(0x1000+painter_slot)=0x1700;
 MEM32(0x1700)=kind==1?0xA0DEBC7Cu:0xA0DEBFECu;
 MEM32(0x8000)=0xA0DD89FC;MEM32(0x8004)=win_manager;MEM32(0x80F8)=0xA0DD8CB8;MEM32(0x80E8)=3;MEM32(0x80FC)=1;
 s2_touch_pump();expected_token=s2_touch_tool_token();
}
__declspec(dllexport) unsigned long long request(void){return s2_touch_fill_room(700,500,expected_token,S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,7);}
__declspec(dllexport) unsigned long long receipt(unsigned serial){uint64_t r=s2_touch_slider_status(serial);return r?r:(uint64_t)serial<<32;}
__declspec(dllexport) void owner(void){unsigned s=__c11_atomic_exchange(&fixture_barrier,0,5);if(s)s2_touch_barrier(s);}
__declspec(dllexport) void configure(unsigned stage){atomic_store(&rot_block_stage,stage);}
__declspec(dllexport) void change_during_down(unsigned kind){atomic_store(&rot_callback_mode,kind);}
__declspec(dllexport) void release(void){atomic_store(&rot_allow,1);}
__declspec(dllexport) void invalidate(unsigned kind){
 if(kind==0)atomic_fetch_add(&cas_pan_epoch,1);
 if(kind==1)MEM32(tool_handler+0x20)=0x3000;
 if(kind==2)rot_width=800;
 if(kind==3)atomic_store(&touch_context,S2_TOUCH_BUILD|S2_TOUCH_MODAL);
 if(kind==4)MEM32(0x1000+painter_slot)=0;
}
__declspec(dllexport) unsigned busy_queries(void){
 unsigned s=pick_serial;
 if(s2_touch_pick(1,2)||s2_touch_ui_command(S2_UI_SCALE,200)||s2_touch_slider_step(700,500,1)!=S2_SLIDER_BUSY||
   s2_touch_rotate_object(700,500,(expected_token&0xFFFFFFFF00000000ull)|2,S2_TOUCH_BUILD,((uint64_t)540<<32)|1212,7,-1)!=S2_SLIDER_BUSY||pick_serial!=s)return 1;
 return 0;
}
__declspec(dllexport) unsigned state(unsigned k){
 if(k==0)return atomic_load(&fixture_barrier);if(k==1)return atomic_load(&rot_blocked);if(k==2)return atomic_load(&rot_refs);
 if(k==3)return atomic_load(&rot_starts);if(k==4)return atomic_load(&rot_stops);if(k==5)return atomic_load(&rot_adds);
 if(k==6)return atomic_load(&rot_releases);if(k==7)return atomic_load(&rot_allocs);if(k==8)return atomic_load(&rot_frees);
 if(k==9)return atomic_load(&fill_restore);if(k==10)return atomic_load(&fill_effect);if(k==11)return bad;
 if(k==12)return MEM8(0x1010)||MEM8(0x101C)||MEM32(win_manager+0x34);
 if(k==13)return g_eax==0xAA&&g_ecx==0xBB&&g_esp==0x1F00;if(k==14)return atomic_load(&rot_picks);
 if(k==15)return atomic_load(&manager_add);if(k==16)return atomic_load(&manager_release);
 if(k==17)return atomic_load(&scene_add);if(k==18)return atomic_load(&scene_release);if(k==19)return atomic_load(&capture_release);
 if(k==20)return MEM32(0x10C);if(k==21)return MEM32(0x80FC);return MEM32(win_manager+0x34);
}
'''


def source():
    s=R.source();s=s[:s.index('static uint64_t expected_token;')]
    s='static unsigned fill_kind,painter_slot,mode_slot;static _Atomic unsigned fill_restore,fill_effect,manager_add,manager_release,scene_add,scene_release,capture_release;static void fill_callback_change(void);\n'+s
    s=R.replace_function(s,'static uint32_t void_call_guest_this(',GUEST)
    s=s.replace('if(n!=44)bad=112;', 'if(n!=8)bad=112;')
    return s+CHECK


def main():
    OUT.mkdir(parents=True,exist_ok=True);cfile=OUT/'threads.c';cfile.write_text(source(),encoding='utf-8');cases=[]
    with tempfile.TemporaryDirectory(prefix='sims2-fill-threads-') as folder:
        obj,library=Path(folder)/'check.obj',Path(folder)/'check.dll'
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-I'+str(ROOT/'android/host'),'-c',str(cfile),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(library),str(obj)],check=True)
        dll=ctypes.CDLL(str(library));workers=[];results={}
        clock_type=ctypes.CFUNCTYPE(ctypes.c_uint64,ctypes.c_int);now=clock_type(lambda k:time.monotonic_ns() if k==1 else time.time_ns())
        dll.prepare.argtypes=[ctypes.c_void_p,ctypes.c_uint];dll.request.restype=dll.receipt.restype=ctypes.c_uint64
        def run(name,fn):
            t=threading.Thread(target=lambda:results.__setitem__(name,fn()),daemon=True);workers.append(t);t.start();return t
        def join(t):t.join(3);assert not t.is_alive(),'native worker stuck'
        def wait(k):
            until=time.monotonic()+3
            while not dll.state(k):
                assert time.monotonic()<until,('boundary not reached',k,[dll.state(i) for i in range(15)])
                time.sleep(.001)
        def paired(restore=1,effect=1):
            assert [dll.state(k) for k in (2,3,4,5,6,7,8,9,10)]==[1,1,1,1,1,1,1,restore,effect]
            assert not dll.state(11) and not dll.state(12) and dll.state(13)
            assert [dll.state(k) for k in (15,16,17,18,20,21)]==[1]*6
        try:
            for kind,label in ((1,'FloorPattern'),(2,'Wallpaper')):
                dll.prepare(now,kind);dll.configure(1);user=run('started',dll.request);wait(0);owner=run('owner',dll.owner);wait(1);join(user)
                r=results['started'];serial=r>>32;assert serial and r&255==1 and dll.receipt(serial)&255==1
                assert dll.request()&255==6 and not dll.busy_queries();assert dll.state(2)==2 and dll.state(3)==1 and not dll.state(4)
                dll.release();join(owner);assert dll.receipt(serial)&255==2;paired();dll.owner();assert dll.state(3)==dll.state(4)==1
                cases.append(label+': STARTED beyond80ms retains serial/BUSY and completes one Down/Up, mode restore, ref/allocation cleanup without replay')

            dll.prepare(now,1);r=dll.request();assert r&255==1;time.sleep(2.05);assert dll.receipt(r>>32)&255==5;dll.owner();assert not dll.state(3) and not dll.state(4) and dll.state(2)==1 and not dll.state(7) and not dll.state(11)
            cases.append('QUEUED2s timeout then late owner has zero Down/Up/ref/allocation effects')
            dll.prepare(now,1);r=dll.request();assert r&255==1;time.sleep(.32);dll.owner()
            assert dll.receipt(r>>32)&255==2 and dll.state(3)==dll.state(4)==1 and not dll.state(11)
            cases.append('QUEUED owner delayed400ms executes exactly once with the original receipt')

            for invalid,label in enumerate(('epoch','tool identity','logical size','modal context','missing painter')):
                dll.prepare(now,1);user=run('invalid',dll.request);wait(0);dll.invalidate(invalid);dll.owner();join(user)
                assert results['invalid']&255==4 and not dll.state(3) and not dll.state(4) and dll.state(2)==1 and not dll.state(7) and not dll.state(11)
                cases.append(label+' changes after enqueue: owner rejects before Down/allocation')
            for invalid,label in ((0,'epoch'),(1,'tool identity')):
                dll.prepare(now,1);dll.configure(2);user=run('pick_wait',dll.request);wait(0);owner=run('pick_owner',dll.owner);wait(1);join(user)
                serial=results['pick_wait']>>32;assert results['pick_wait']&255==1 and dll.state(2)==2
                dll.invalidate(invalid);dll.release();join(owner)
                assert dll.receipt(serial)&255==4 and not dll.state(3) and not dll.state(4) and dll.state(2)==1 and dll.state(5)==dll.state(6)==1 and not dll.state(7) and not dll.state(11)
                assert [dll.state(k) for k in (15,16,17,18,20,21)]==[1]*6
                cases.append(label+' changes during STARTED picker: retained old target released without Down')
            for change,label in ((1,'context change'),(2,'tool replacement and UnInit-null painter'),(3,'layout epoch change')):
                dll.prepare(now,1);dll.configure(1);dll.change_during_down(change);user=run('changed_down',dll.request);wait(0);owner=run('changed_owner',dll.owner);wait(1);join(user)
                serial=results['changed_down']>>32;assert results['changed_down']&255==1;dll.release();join(owner)
                assert dll.receipt(serial)&255==0 and dll.receipt(serial)>>32==serial;paired(restore=0 if change==2 else 1,effect=0 if change==2 else 1)
                cases.append(label+' after Down: original retained Up completes once, UNKNOWN result, unsafe mode restore skipped for dead painter')
            dll.prepare(now,1);dll.configure(1);dll.change_during_down(4);user=run('detached_down',dll.request);wait(0);owner=run('detached_owner',dll.owner);wait(1);join(user)
            serial=results['detached_down']>>32;assert results['detached_down']&255==1;dll.release();join(owner)
            assert dll.receipt(serial)&255==0 and dll.receipt(serial)>>32==serial
            assert [dll.state(k) for k in (2,3,4,5,6,7,8,9,10)]==[1,1,0,1,1,1,1,0,0]
            assert [dll.state(k) for k in (15,16,17,18,19,20,21)]==[1]*7
            assert not dll.state(11) and dll.state(13) and dll.state(12) and not dll.state(22)
            cases.append('Owner detaches after Down: retain manager/viewport, skip unsafe Up/restore, release only original capture and refs, UNKNOWN without replay')
        finally:
            dll.release()
            for t in workers:t.join(3)
            if all(not t.is_alive() for t in workers):
                ctypes.windll.kernel32.FreeLibrary.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.FreeLibrary(dll._handle)
    result={'pass':True,'real_host_thread_cases':cases,'production_sha256':hashlib.sha256((ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(cfile.read_bytes()).hexdigest(),
            'limits':'Actual production shared request/owner state and Fill helper with real Windows clocks/atomic locks/threads. Guest mode/event/ref/picker calls and queue delivery are strict boundary mocks. Original lifted mode/Down/Up ABI and real ARM64 FIFO checked separately. STARTED can commit partial/changed-context native effects; UNKNOWN never means rollback. No room-ID policy, visible geometry/funds/device acceptance.'}
    (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('PASS real-thread native Fill:',len(cases),'cases')


if __name__=='__main__':main()
