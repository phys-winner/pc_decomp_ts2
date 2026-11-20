"""Build a real-pthread probe for the production sync wrapper and guest scheduler.

Run its binary on the selected device. GL queue completion is an explicit worker
boundary; check_render_wait.py separately stresses the production queue.
"""
import subprocess

from build_android import CLANG, ROOT
from check_machine_yield import PRELUDE
from check_virtual_query import function


def main():
    threads = (ROOT / 'android/host/threads.c').read_text(encoding='utf-8')
    render = (ROOT / 'android/host/d3d9_thread.c').read_text(encoding='utf-8')
    source = PRELUDE + '\n'
    source += '\n'.join(function(threads, signature) for signature in (
        'static uint64_t now_ns(', 'static int thread_urgent(', 'static void machine_poke(', 'static int park_ticket(', 'static void unpark_ticket(', 'static int skip_parked(', 'static void handoff_note(', 'static void machine_wait(', 'static uint64_t take_ticket(', 'static void machine_acquire(',
        'static void machine_release(', 'static int machine_contended(', 'static void yield_hook('))
    source += r'''
#include <string.h>
typedef void (*d3d_rt_fn)(void *);
static int threaded=1,on_render;
static uint64_t sync_count;
static void (*recomp_yield_hook)(void);
static pthread_mutex_t job_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_wake=PTHREAD_COND_INITIALIZER;
static int job_pending,job_done,worker_stop;
static _Atomic unsigned command_busy,completed,audio_turns;
static unsigned hook_calls;
static void d3d_rt_call(d3d_rt_fn,const void *,uint32_t);
static void d3d_rt_finish(void);
'''
    source += '\n'.join(line for line in render.splitlines() if line.startswith((
        'typedef struct { d3d_rt_fn fn; void *arg; } sync_call;', 'static void run_sync('))) + '\n'
    source += function(render, 'void d3d_rt_sync(')
    source += r'''
static sync_call queued;
static void d3d_rt_call(d3d_rt_fn fn,const void *args,uint32_t size){
 assert(fn==run_sync&&size==sizeof queued&&guest_register==111);
 pthread_mutex_lock(&job_lock);assert(!job_pending);
 memcpy(&queued,args,sizeof queued);job_pending=1;job_done=0;
 atomic_store(&command_busy,1);pthread_cond_broadcast(&job_wake);pthread_mutex_unlock(&job_lock);
}
static void d3d_rt_finish(void){
 pthread_mutex_lock(&job_lock);
 while(!job_done)pthread_cond_wait(&job_wake,&job_lock);
 pthread_mutex_unlock(&job_lock);
 assert(!atomic_load(&command_busy)&&guest_register==111);
}
static void *worker(void *unused){
 (void)unused;
 pthread_mutex_lock(&job_lock);
 for(;;){
  while(!job_pending&&!worker_stop)pthread_cond_wait(&job_wake,&job_lock);
  if(worker_stop)break;
  pthread_mutex_unlock(&job_lock);run_sync(&queued);pthread_mutex_lock(&job_lock);
  atomic_store(&command_busy,0);job_pending=0;job_done=1;pthread_cond_broadcast(&job_wake);
 }
 pthread_mutex_unlock(&job_lock);return NULL;
}
static void operation(void *arg){
 unsigned *value=arg;*value=0x12345678;atomic_fetch_add(&completed,1);
}
static void guarded_yield(void){
 assert(!atomic_load(&command_busy)&&guest_register==111);
 assert(!pthread_mutex_trylock(&job_lock));pthread_mutex_unlock(&job_lock);
 ++hook_calls;yield_hook();assert(guest_register==111);
}
static void *audio(void *unused){
 (void)unused;t_self=&local;
 for(unsigned i=0;i<2000;++i){
  machine_acquire();assert(!atomic_load(&command_busy));
  assert(atomic_load(&completed));guest_register=222;atomic_fetch_add(&audio_turns,1);
  yield_hook();assert(guest_register==222);machine_release();
 }
 return NULL;
}
int main(void){
 alarm(30);t_self=&local;t_main_thread=1;machine_acquire();guest_register=111;
 pthread_t renderer,decoder;assert(!pthread_create(&renderer,NULL,worker,NULL));
 unsigned value=0;d3d_rt_sync(operation,&value);assert(value==0x12345678&&!hook_calls);
 recomp_yield_hook=guarded_yield;
 assert(!pthread_create(&decoder,NULL,audio,NULL));
 while(!machine_contended())sched_yield();
 for(unsigned i=0;i<2000;++i){
  value=0;t_held_since=now_ns()-MAIN_SLICE_NS;d3d_rt_sync(operation,&value);
  assert(value==0x12345678&&guest_register==111&&hook_calls==i+1);
 }
 assert(atomic_load(&audio_turns)>0&&sync_count==2001);
 machine_release();assert(!pthread_join(decoder,NULL));
 pthread_mutex_lock(&job_lock);worker_stop=1;pthread_cond_broadcast(&job_wake);pthread_mutex_unlock(&job_lock);
 assert(!pthread_join(renderer,NULL));
 /* Inline/reentrant render execution owns no guest machine to hand over. */
 threaded=0;value=0;d3d_rt_sync(operation,&value);assert(value==0x12345678&&hook_calls==2000);
 threaded=1;on_render=1;value=0;d3d_rt_sync(operation,&value);assert(value==0x12345678&&hook_calls==2000);
 assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving));
 puts("PASS sync yield: 2000 real worker completions, guest audio turns, no yield during command, owner register restored, null/inline/render-thread guards");
 return 0;
}
'''
    output = ROOT / 'build/test/render-sync-yield'
    output.mkdir(parents=True, exist_ok=True)
    fixture, binary = output / 'probe.c', output / 'check-render-sync-yield'
    fixture.write_text(source, encoding='utf-8')
    subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-Wl,-z,max-page-size=16384',
                    str(fixture), '-o', str(binary)], check=True)
    print(binary)


if __name__ == '__main__':
    main()
