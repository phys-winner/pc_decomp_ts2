"""Build an actual-pthread ARM64 regression for the guest machine hand-over.

Run the resulting binary on the selected device; building alone is not a pass.
"""
import subprocess
from build_android import ROOT, CLANG
from check_virtual_query import function

PRELUDE = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef struct { unsigned regs; int depth; } void_thread;
static __thread void_thread local = {0,1}, *t_self;
static __thread uint64_t t_lock_wait_ns, t_held_since;
static __thread int t_main_thread;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcond = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t ticket_next, ticket_serving;
static _Atomic int machine_sleepers;
static unsigned guest_register;
static void void_regs_save(unsigned *out) { *out=guest_register; }
static void void_regs_load(const unsigned *in) { guest_register=*in; }
#define void_fatal(s) abort()
#define MAIN_SLICE_NS 2000000ull
'''
TEST = r'''
static void *waiting(void *unused) {
    (void)unused; t_self=&local;
    machine_acquire();
    /* The yielding owner already reserved its return, before we can run. */
    assert(machine_contended()); guest_register=222;
    yield_hook();
    assert(guest_register==222); machine_release(); return NULL;
}
static void *stress(void *unused) {
    (void)unused; t_self=&local;
    for (unsigned n=0; n<2000; ++n) {
        machine_acquire(); guest_register=n;
        yield_hook(); assert(guest_register==n); machine_release();
    }
    return NULL;
}
int main(void) {
    alarm(15); t_self=&local;
    machine_acquire(); guest_register=111;
    uint64_t before=atomic_load(&ticket_next);
    yield_hook(); assert(guest_register==111 && atomic_load(&ticket_next)==before);
    pthread_t other; assert(!pthread_create(&other,NULL,waiting,NULL));
    while(atomic_load(&ticket_next)<2) sched_yield();
    t_main_thread=1; t_held_since=now_ns();
    yield_hook(); assert(atomic_load(&ticket_serving)==0);
    t_held_since=now_ns()-MAIN_SLICE_NS; yield_hook();
    assert(guest_register==111); machine_release(); assert(!pthread_join(other,NULL));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving));
    pthread_t workers[4];
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],NULL,stress,NULL));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],NULL));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !atomic_load(&machine_sleepers));
    puts("PASS machine yield: reserved FIFO return, register preservation, main slice, 8000 contended turns");
    return 0;
}
'''

def main():
    source=(ROOT/'android/host/threads.c').read_text(encoding='utf-8')
    signatures=('static uint64_t now_ns(', 'static void machine_wait(', 'static void machine_acquire(',
                'static void machine_release(', 'static int machine_contended(', 'static void yield_hook(')
    out=ROOT/'build/test/machine-yield'
    out.mkdir(parents=True,exist_ok=True)
    fixture=out/'probe.c'
    fixture.write_text(PRELUDE+'\n'.join(function(source,s) for s in signatures)+TEST,encoding='utf-8')
    subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-Wl,-z,max-page-size=16384',str(fixture),'-o',str(out/'check-machine-yield')],check=True)
    print(out/'check-machine-yield')

if __name__=='__main__': main()
