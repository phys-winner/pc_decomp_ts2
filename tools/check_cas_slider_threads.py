"""Real host threads test queued cancellation versus a started native command.

The production request/owner code executes in a Windows DLL. Guest ABI and the
actual FIFO are covered by check_cas_slider.py; here guest callbacks and queue
delivery are controlled boundaries, with real clocks, mutexes and concurrent
callers. No Android/device access.
"""
import ast
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from build_android import CLANG, ROOT
from check_virtual_query import function

OUT = ROOT / 'build/test/cas-slider-abi'


def source():
    template = ast.parse((ROOT / 'tools/check_touch_context.py').read_text())
    main = next(n for n in template.body if isinstance(n, ast.FunctionDef) and n.name == 'main')
    prelude = next(n.value.value for n in main.body if isinstance(n, ast.Assign) and
                   any(isinstance(t, ast.Name) and t.id == 'prelude' for t in n.targets))
    prelude = prelude[:prelude.index('#define JNIEXPORT')]
    prelude += '\nvoid sub_A06815D7_lifted(void){}void sub_A067D8A4_lifted(void){}\n'
    prelude = prelude.replace('typedef unsigned pthread_mutex_t;', 'typedef _Atomic unsigned pthread_mutex_t;')
    prelude = prelude.replace('typedef unsigned pthread_cond_t;', 'typedef _Atomic unsigned pthread_cond_t;')
    prelude = prelude.replace('static unsigned fixture_barrier;', 'static _Atomic unsigned fixture_barrier;')
    replace = {
        'static int clock_gettime(': r'''static unsigned long long (*fixture_clock)(int);
static int clock_gettime(int c,struct timespec *t){unsigned long long ns=fixture_clock(c);t->tv_sec=ns/1000000000;t->tv_nsec=ns%1000000000;return 0;}''',
        'static void pthread_mutex_lock(': r'''static void pthread_mutex_lock(pthread_mutex_t *p){while(__c11_atomic_exchange(p,1,5)){} ++lock_depth;}''',
        'static void pthread_mutex_unlock(': r'''static void pthread_mutex_unlock(pthread_mutex_t *p){--lock_depth;__c11_atomic_store(p,0,5);}''',
        'static int pthread_cond_timedwait(': r'''static _Atomic unsigned hold_next_waiter,waiter_paused,allow_waiter;
static int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *p,const struct timespec *t){
 unsigned seq=__c11_atomic_load(c,5);pthread_mutex_unlock(p);int expired=0;
 while(__c11_atomic_load(c,5)==seq){struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
  if(now.tv_sec>t->tv_sec || (now.tv_sec==t->tv_sec && now.tv_nsec>=t->tv_nsec)){expired=1;break;}}
 if(__c11_atomic_exchange(&hold_next_waiter,0,5)){__c11_atomic_store(&waiter_paused,1,5);while(!__c11_atomic_load(&allow_waiter,5)){} }
 pthread_mutex_lock(p);return expired;
}''',
        'static void pthread_cond_broadcast(': r'''static void pthread_cond_broadcast(pthread_cond_t *c){__c11_atomic_fetch_add(c,1,5);}''',
        'static uint32_t void_call_guest_this(': r'''static _Atomic unsigned setter_blocked,release_setter;
static unsigned value=50,changed,finished,refs;
static uint32_t void_call_guest_this(uint32_t va,uint32_t self,int n,const uint32_t *args){
 if(va==0x12345678)return 0x1000;
 if(va==0xA0957FE3){MEM32(args[1])=self+0xF8;++refs;return 1;}
 if(va==0xA0046390)return 0;
 if(va==0xA0145770)return 100;
 if(va==0xA0957FA3)return value;
 if(va==0x70B)return 1;
 if(va==0xA0958268){value=args[0];++changed;__c11_atomic_store(&setter_blocked,1,5);while(!__c11_atomic_load(&release_setter,5)){} return 1;}
 if(va==0x703){if(n)bad=3;return 3;}
 if(va==0x704){if(n!=5 || args[0]!=0x1700 || args[1]!=3 || args[2]!=0x887113A4 || args[3]!=3 || args[4])bad=1;++finished;return 1;}
 if(va==0x702){--refs;return refs;}
 bad=2;return 0;
}''',
    }
    # lock_depth must be declared before the replacement locking functions.
    prelude = prelude.replace('static unsigned lock_depth;', '')
    prelude = 'static unsigned lock_depth;\n' + prelude
    for signature, body in replace.items():
        start = prelude.index(signature); brace = prelude.index('{', start); depth = 1; end = brace + 1
        while depth:
            depth += (prelude[end] == '{') - (prelude[end] == '}'); end += 1
        prelude = prelude[:start] + body + '\n' + prelude[end:]
    touch = (ROOT / 'android/host/sims2_touch.c').read_text()
    actual = '\n'.join(line for line in touch.splitlines() if not line.startswith('#include'))
    checks = r'''
static void typed(uint32_t o,uint32_t v,uint32_t l,uint32_t d,const char *s){MEM32(o)=v;MEM32(v-4)=l;MEM32(l+12)=d;memcpy(GPTR(d+8),s,strlen(s)+1);}
__declspec(dllexport) void prepare(void *clock){
 fixture_clock=clock;win_manager=0x100;win_manager_thread=thread_id=1;touch_context=S2_TOUCH_CAS;
 slider_state=SLIDER_IDLE;slider_serial=pick_serial=pick_done=pick_pending=fixture_barrier=0;
 setter_blocked=release_setter=hold_next_waiter=waiter_paused=allow_waiter=0;
 value=50;changed=finished=0;refs=1;bad=0;g_eax=0xAA;g_ecx=0xBB;g_esp=0x1F00;
 MEM32(0x144)=0x200;MEM32(0x200)=0x400;MEM32(0x4BC)=0x12345678;
 typed(0x1000,0x1200,0x1800,0x1900,".?AVcGZWinSlider@@");
 MEM32(0x138)=0x1000;MEM32(0x134)=0;MEM32(0x105C)=0x1700;
 MEM32(0x1200)=0xA0957FE3;MEM32(0x1208)=0x702;MEM32(0x1338)=0x703;MEM32(0x1498)=0x704;
 MEM32(0x1348)=0x70B;
 MEM32(0x10F8)=0x1600;MEM32(0x1610)=0xA0957FA3;MEM32(0x1614)=0xA0958268;
 MEM32(0x161C)=0xA0046390;MEM32(0x1624)=0xA0145770;
}
__declspec(dllexport) unsigned long long step(void){return s2_touch_slider_step(700,500,1);}
__declspec(dllexport) unsigned long long receipt(unsigned serial){return s2_touch_slider_status(serial);}
__declspec(dllexport) void owner(void){unsigned serial=__c11_atomic_exchange(&fixture_barrier,0,5);if(serial)s2_touch_barrier(serial);}
__declspec(dllexport) unsigned state(unsigned what){
 if(what==0)return __c11_atomic_load(&fixture_barrier,5);
 if(what==1)return __c11_atomic_load(&setter_blocked,5);
 if(what==2)return __c11_atomic_load(&waiter_paused,5);
 if(what==3)return changed;if(what==4)return finished;if(what==5)return refs;
 if(what==6)return bad;if(what==7)return value;
 return g_eax==0xAA && g_ecx==0xBB && g_esp==0x1F00;
}
__declspec(dllexport) void release(void){__c11_atomic_store(&release_setter,1,5);}
__declspec(dllexport) void hold_waiter(void){__c11_atomic_store(&hold_next_waiter,1,5);}
__declspec(dllexport) void release_waiter(void){__c11_atomic_store(&allow_waiter,1,5);}
__declspec(dllexport) unsigned queries(void){
 unsigned serial=pick_serial;
 if(s2_touch_pick(1,2))return 1;s2_touch_request_pick(1,2);
 if(s2_touch_slider_step(700,500,1)!=S2_SLIDER_BUSY)return 2;
 return serial!=pick_serial;
}
'''
    return prelude + '''
static void sub_A0972F48(void){}
unsigned s2_touch_context(void){return atomic_load(&touch_context)|touch_input_flags;}
void s2_touch_input_context(unsigned flags){touch_input_flags=flags;}
static void void_input_cursor(uint32_t *x,uint32_t *y){*x=*y=0;}
''' + actual + checks


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfile = OUT / 'threads.c'; cfile.write_text(source())
    with tempfile.TemporaryDirectory(prefix='sims2-cas-step-') as directory:
        obj, library = Path(directory) / 'check.obj', Path(directory) / 'check.dll'
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-I' + str(ROOT / 'android/host'),
                        '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library)); workers = []; results = {}
        clock_type = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_int)
        now = clock_type(lambda kind: time.monotonic_ns() if kind == 1 else time.time_ns())
        dll.prepare.argtypes = [ctypes.c_void_p]; dll.step.restype = dll.receipt.restype = ctypes.c_uint64

        def wait_state(index):
            deadline = time.monotonic() + 3
            while not dll.state(index):
                assert time.monotonic() < deadline, ('boundary not reached', index)
                time.sleep(.001)

        def run(name, method):
            def target(): results[name] = method()
            worker = threading.Thread(target=target, daemon=True); workers.append(worker); worker.start(); return worker

        def join(worker):
            worker.join(3); assert not worker.is_alive(), 'native worker stuck'

        cases = []
        try:
            dll.prepare(now); user = run('started', dll.step); wait_state(0)
            owner = run('owner', dll.owner); wait_state(1); join(user)
            r = results['started']; serial = r >> 32
            assert r & 255 == 1 and serial and r & 0xFFFF00 == 0
            assert dll.receipt(serial) & 255 == 1 and dll.queries() == 0
            assert dll.state(3) == 1 and dll.state(4) == 0 and dll.state(5) == 2
            dll.release(); join(owner)
            r = dll.receipt(serial)
            assert r & 255 == 2 and (r >> 8) & 255 == 151 and (r >> 16) & 255 == 152
            assert dll.state(3) == dll.state(4) == dll.state(5) == 1
            assert dll.state(6) == 0 and dll.state(8) == 1
            dll.owner(); assert dll.state(3) == dll.state(4) == 1
            cases.append('started callback blocks beyond80ms; IN_PROGRESS, concurrent queries/new command blocked, then exactly one completion')

            dll.prepare(now); r = dll.step(); assert r & 255 == 5
            dll.owner(); assert dll.state(3) == dll.state(4) == 0 and dll.state(5) == 1
            cases.append('queued timeout followed by late owner dispatch has zero effects')

            dll.prepare(now); dll.hold_waiter(); user = run('expired', dll.step); wait_state(2)
            dll.owner(); assert dll.state(3) == dll.state(4) == 0
            dll.release_waiter(); join(user); assert results['expired'] & 255 == 5
            cases.append('owner expires queued request even while timed-out caller cannot resume')

            dll.prepare(now); dll.release(); dll.hold_waiter()
            first = run('first', dll.step); wait_state(0); dll.owner(); wait_state(2)
            second = run('second', dll.step); wait_state(0)
            dll.release_waiter(); join(first)
            assert results['first'] & 255 == 0, hex(results['first'])
            dll.owner(); join(second)
            assert results['second'] & 255 == 2
            assert dll.state(3) == dll.state(4) == 2 and dll.state(7) == 52 and dll.state(5) == 1
            assert dll.state(6) == 0 and dll.state(8) == 1
            cases.append('completed A waiter cannot cancel or mislabel newer B command')
        finally:
            dll.release(); dll.release_waiter()
            for worker in workers: worker.join(3)
            if all(not worker.is_alive() for worker in workers):
                ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
                ctypes.windll.kernel32.FreeLibrary(dll._handle)
    receipt = dict(pass_=True, real_host_thread_cases=cases,
        source_sha256=hashlib.sha256((ROOT / 'android/host/sims2_touch.c').read_bytes()).hexdigest(),
        limits='Windows host command synchronization; guest calls and queue delivery are boundary mocks, ARM64 original API/FIFO tested separately; no device actions')
    (OUT / 'threads-results.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('PASS actual native CAS command with real host threads:', len(cases), 'cases')


if __name__ == '__main__': main()
