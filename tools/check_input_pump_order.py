"""Run two host threads through the actual input-pump C, with a paused batch."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/input.c').read_text(encoding='utf-8')
    state = source[source.index('typedef struct { uint32_t msg, wparam, lparam; } pending_t;'):
                   source.index('void void_input_screen_size(')]
    code = r'''
typedef unsigned int uint32_t;
typedef unsigned long long size_t;
typedef _Atomic unsigned pthread_mutex_t;
static pthread_mutex_t input_lock,pump_lock;
static _Atomic unsigned paused,release_first,second_blocked,posted_count;
static uint32_t posted[2];
#define VOID_TOUCH_QUERY_MESSAGE 0x7FFF0001u
#define VOID_TOUCH_QUERY_COOKIE 0x53325451u
static void pthread_mutex_lock(pthread_mutex_t *p){
    while(__c11_atomic_exchange(p,1,5))if(p==&pump_lock)__c11_atomic_store(&second_blocked,1,5);
}
static void pthread_mutex_unlock(pthread_mutex_t *p){__c11_atomic_store(p,0,5);}
static void *memcpy(void *d,const void *s,size_t n){char *o=d;const char *i=s;while(n--)*o++=*i++;return d;}
static void s2_touch_pump(void){}
static uint32_t void_window_main(void){return 1;}
static void void_post_message(uint32_t hwnd,uint32_t msg,uint32_t wp,uint32_t lp){
    if(msg==0x202){__c11_atomic_store(&paused,1,5);while(!__c11_atomic_load(&release_first,5)){} }
    unsigned n=__c11_atomic_load(&posted_count,5);if(n<2)posted[n]=msg;
    __c11_atomic_store(&posted_count,n+1,5);
}
'''
    code += state + ''.join(function(source, name) for name in
                           ('static void pend(', 'int void_input_touch_barrier(', 'void void_input_pump('))
    code += r'''
__declspec(dllexport) void prepare(void){pend(0x202,0,0);}
__declspec(dllexport) void pump(void){void_input_pump();}
__declspec(dllexport) int enqueue(void){return void_input_touch_barrier(7);}
__declspec(dllexport) unsigned status(unsigned which){
    if(which==0)return __c11_atomic_load(&paused,5);
    if(which==1)return __c11_atomic_load(&second_blocked,5);
    return __c11_atomic_load(&posted_count,5);
}
__declspec(dllexport) void release(void){__c11_atomic_store(&release_first,1,5);}
__declspec(dllexport) int check(void){return posted_count==2 && posted[0]==0x202 && posted[1]==VOID_TOUCH_QUERY_MESSAGE;}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-pump-order-') as directory:
        base = Path(directory)
        cfile, obj, library = base / 'check.c', base / 'check.obj', base / 'check.dll'
        cfile.write_text(code)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library))
        workers = []
        try:
            def wait_status(index):
                deadline = time.monotonic() + 3
                while not dll.status(index):
                    assert time.monotonic() < deadline, ('C boundary not reached', index)
                    time.sleep(.001)

            dll.prepare()
            workers.append(threading.Thread(target=dll.pump, daemon=True)); workers[-1].start()
            wait_status(0)  # Earlier UP detached, first pump paused before posting it.
            assert dll.enqueue() == 1
            workers.append(threading.Thread(target=dll.pump, daemon=True)); workers[-1].start()
            wait_status(1)  # Later marker pump is actually blocked on the pump mutex.
            assert dll.status(2) == 0
            dll.release()
            for worker in workers:
                worker.join(3)
                assert not worker.is_alive(), 'input pump did not return'
            assert dll.check() == 1
        finally:
            dll.release()
            for worker in workers:
                worker.join(3)
            if all(not worker.is_alive() for worker in workers):
                ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
                ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('PASS actual input C with two host threads: detached catalog UP posts before a concurrently queued query sentinel')


if __name__ == '__main__':
    main()
