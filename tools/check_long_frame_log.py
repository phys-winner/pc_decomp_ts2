"""Check QA-only outlier clocks/deltas against the production logger body."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_gl.c').read_text(encoding='utf-8')
    actual = function(source, 'static void long_frame_log(')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned frame; } d3d_device;
struct timespec { long tv_sec,tv_nsec; };
#define CLOCK_THREAD_CPUTIME_ID 7
static uint64_t swap_ns,draw_calls,cpu;
static unsigned clocks,logs,failed;
static char last[512];
static const char *qa_dir(void){return "/data/Sims2/userdata-qa-poco";}
static const char *normal_dir(void){return "/data/Sims2/userdata";}
static int clock_gettime(int kind,struct timespec *t){
 assert(kind==7);++clocks;if(failed)return -1;t->tv_sec=(long)(cpu/1000000000);t->tv_nsec=(long)(cpu%1000000000);return 0;
}
static void void_log(const char *fmt,...){va_list args;va_start(args,fmt);vsnprintf(last,sizeof(last),fmt,args);va_end(args);++logs;}
'''
    checks = r'''
int main(void){
 d3d_device d={17};
 normal_log(&d,1000000000,10,20,30,40);assert(!clocks&&!logs);
 cpu=100;long_frame_log(&d,1000000000,1000,2000,3,4000);assert(clocks==1&&!logs);
 cpu=200;swap_ns=10;draw_calls=4;long_frame_log(&d,1050000000,1100,2010,4,4020);assert(clocks==2&&!logs);
 cpu=500;swap_ns=30;draw_calls=10;d.frame=19;
 long_frame_log(&d,1100000001,1600,2040,6,4070);
 assert(logs==1&&!strcmp(last,"[gl-frame] frame 19 mono 1100000001 cadence 50000001 gamecpu 500 rendercpu 300 machinewait 30 queuewait 50 syncs 2 swap 20 draws 6"));
 failed=1;long_frame_log(&d,1200000001,1700,2050,7,4080);assert(logs==1);
 failed=0;cpu=600;long_frame_log(&d,1200000002,1800,2060,8,4090);assert(logs==2);
 puts("PASS long-frame QA scope, strict50ms threshold, monotonicstamp and exact per-frame CPU/wait/swap/draw deltas, clock failure");
 return 0;
}
'''
    harness = (prelude + '#define void_data_dir normal_dir\n#define long_frame_log normal_log\n' +
               actual + '\n#undef long_frame_log\n#undef void_data_dir\n#define void_data_dir qa_dir\n' + actual + checks)
    compile_probe('long-frame-log', harness, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
