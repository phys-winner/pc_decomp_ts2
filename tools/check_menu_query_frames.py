"""Execute the real hit-test handshake for every tap phase at 5-8 FPS and a stalled owner."""
import _ctypes
import ctypes
import subprocess
import tempfile
from pathlib import Path

import build_android as B
from check_virtual_query import function


def main():
    text = (B.ROOT/'android/host/sims2_touch.c').read_text()
    prelude = r'''
typedef unsigned uint32_t;
typedef long long time_t;
struct timespec {time_t tv_sec;long long tv_nsec;};
typedef int pthread_mutex_t,pthread_cond_t;
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
enum {SLIDER_IDLE,SLIDER_QUEUED,SLIDER_STARTED};
static pthread_mutex_t pick_lock;
static pthread_cond_t pick_ready;
static int pick_x,pick_y,pick_pending,pick_diagnostic,pick_result;
static unsigned pick_serial,pick_done,pick_owner_context,query_zero_logs,slider_state,slider_serial,ui_command_kind,ui_command_started;
static uint32_t pick_owner_root,pick_owner_hit;
static int owner_ms,queued=1,depth,waits,clock_failed;
static void ui_command_expire(void){}
static int clock_gettime(int clock,struct timespec *t){t->tv_sec=10;t->tv_nsec=0;return clock_failed;}
static void pthread_mutex_lock(pthread_mutex_t *p){++depth;}
static void pthread_mutex_unlock(pthread_mutex_t *p){--depth;}
static int void_input_touch_barrier(unsigned serial){return queued;}
static void void_log(const char *f,...){}
static int pthread_cond_timedwait(pthread_cond_t *c,pthread_mutex_t *p,const struct timespec *t){
 ++waits;long long budget=(t->tv_sec-10)*1000+t->tv_nsec/1000000;
 if(budget>500||budget<=0)return 22;
 if(owner_ms>budget)return 110;
 pick_done=pick_serial;pick_result=1;pick_owner_context=8;pick_owner_root=0x1000;pick_owner_hit=0x2000;return 0;
}
'''
    checks = r'''
static void reset(void){pick_pending=pick_done=pick_serial=query_zero_logs=slider_state=slider_serial=ui_command_kind=ui_command_started=waits=depth=clock_failed=0;queued=1;}
__declspec(dllexport) int check(void){
 int periods[]={125,143,200};
 for(int p=0;p<3;++p)for(int phase=0;phase<periods[p];++phase){
  reset();owner_ms=periods[p]-phase;
  if(s2_touch_pick(700,300)!=1||waits!=1||depth||pick_owner_hit!=0x2000)return 1;
 }
 reset();owner_ms=700;unsigned previous=pick_serial;
 if(s2_touch_pick(700,300)||pick_pending||pick_serial!=previous+2||depth)return 2;
 reset();queued=0;if(s2_touch_pick(700,300)||waits||pick_pending)return 3;
 reset();slider_state=SLIDER_QUEUED;if(s2_touch_pick(700,300)||waits||pick_pending)return 4;
 reset();ui_command_started=1;if(s2_touch_pick(700,300)||waits||pick_pending)return 5;
 reset();clock_failed=1;if(s2_touch_pick(700,300)||waits||pick_pending)return 6;
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-menu-query-') as directory:
        out=Path(directory)
        source,obj,dll=out/'check.c',out/'check.obj',out/'check.dll'
        source.write_text(prelude+function(text,'int s2_touch_pick(')+checks)
        subprocess.run([str(B.CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin',
                        '-fno-stack-protector','-c',str(source),'-o',str(obj)],check=True)
        subprocess.run([str(B.CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry',
                        '/out:'+str(dll),str(obj)],check=True)
        library=ctypes.CDLL(str(dll))
        try:
            result=library.check()
            assert result==0, ('production query failed slow-frame/timeout case',result)
        finally:
            _ctypes.FreeLibrary(library._handle)
    print('PASS 468 actual-C tap phases at 5/7/8 FPS, fresh owner ACK, bounded stalled-owner cancellation, queue failure and busy ownership; original widget dispatch is a boundary')


if __name__=='__main__':
    main()
