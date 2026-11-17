"""Build actual render-queue POSIX stress fixtures; run printed binaries on Android.

No adb/build/install is performed. Original queue comes from the preserved PGO
host snapshot; GL and guest-machine ownership are controlled boundaries.
"""
import hashlib
import json
import subprocess

import build_android as B

OUT = B.ROOT / 'build/test/render-wait'
OUT.mkdir(parents=True, exist_ok=True)
PRELUDE = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
typedef void (*d3d_rt_fn)(void *);
static void (*recomp_yield_hook)(void);
static _Atomic unsigned game_wakes, render_wakes, blocked, blocks, bad;
static uint64_t void_monotonic_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void void_blocking_begin(void) { atomic_fetch_add(&blocked,1);atomic_fetch_add(&blocks,1); }
static void void_blocking_end(void) { if(atomic_fetch_sub(&blocked,1)!=1)atomic_store(&bad,1); }
static int void_helper_cores(void *p) { (void)p;return 0; }
static const char *void_data_dir(void) { return NULL; }
static void void_log(const char *fmt,...) { (void)fmt; }
static void void_fatal(const char *fmt,...) { (void)fmt;abort(); }
void d3d_rt_commit(void);
void d3d_rt_frame_done(void);
static int counted_broadcast(pthread_cond_t *p);
static void fixture_before_game_arm(void), fixture_before_render_arm(void);
#define pthread_cond_broadcast counted_broadcast
'''
CHECKS = r'''
#undef pthread_cond_broadcast
static int counted_broadcast(pthread_cond_t *p) {
    atomic_fetch_add(p==&to_game?&game_wakes:&render_wakes,1);
    return pthread_cond_broadcast(p);
}
static int inject_arm, render_probe_done;
static void *publish_before_arm(void *unused) {
    (void)unused;
    if(inject_arm==1) { atomic_store(&tail,7);wake(&game_sleeping,&to_game); }
    if(inject_arm==2) { atomic_store(&frames_queued,1);wake(&game_sleeping,&to_game); }
    if(inject_arm==3) { atomic_store(&head,64);wake(&render_sleeping,&to_render); }
    return NULL;
}
static void fixture_before_game_arm(void) {
    if(inject_arm==1||inject_arm==2) { pthread_t p;pthread_create(&p,NULL,publish_before_arm,NULL);pthread_join(p,NULL);inject_arm=0; }
}
static void fixture_before_render_arm(void) {
    if(inject_arm==3) { pthread_t p;pthread_create(&p,NULL,publish_before_arm,NULL);pthread_join(p,NULL);inject_arm=0; }
}
static void probe_done(void *unused) { (void)unused;render_probe_done=1;pthread_exit(NULL); }
static void arm_order_probes(void) {
    /* Another thread finishes after the last spin, immediately before arming.
     * Arming must precede the final predicate check; no later event rescues it. */
    inject_arm=1;game_wait(7,-1);atomic_store(&tail,0);
    atomic_store(&frames_queued,2);inject_arm=2;game_wait(0,1);atomic_store(&frames_queued,0);
    ring=aligned_alloc(64,RING_BYTES);if(!ring)abort();
    rec_t *r=(rec_t*)ring;*r=(rec_t){64,0,probe_done};
    inject_arm=3;pthread_t p;pthread_create(&p,NULL,render_main,NULL);pthread_join(p,NULL);
    if(!render_probe_done||inject_arm)abort();
    free(ring);ring=NULL;atomic_store(&head,0);atomic_store(&tail,0);
    atomic_store(&render_sleeping,0);atomic_store(&game_sleeping,0);
    atomic_store(&game_wakes,0);atomic_store(&render_wakes,0);atomic_store(&blocks,0);
}
static _Atomic int stop_spurious;
static void *spurious(void *unused) {
    (void)unused;
    while(!atomic_load(&stop_spurious)) {
        pthread_mutex_lock(&lock);
        pthread_cond_broadcast(&to_game);pthread_cond_broadcast(&to_render);
        pthread_mutex_unlock(&lock);usleep(50);
    }
    return NULL;
}
typedef struct { uint32_t seq,size,frame,delay; } packet;
static unsigned recorded,seen,frames_seen,syncs;
static void consume(void *data) {
    packet *p=data;
    if(p->seq!=seen++||p->size<sizeof *p||((uint8_t*)data)[p->size-1]!=0x59)atomic_store(&bad,2);
    if(p->delay)usleep(p->delay);
    if(p->frame) { ++frames_seen;d3d_rt_frame_done(); }
}
static void record(unsigned size,unsigned frame,unsigned delay,int shrink) {
    if(frame)d3d_rt_frame_queued();
    packet *p=d3d_rt_begin(consume,size+(shrink?64:0));
    *p=(packet){recorded++,size,frame,delay};((uint8_t*)p)[size-1]=0x59;
    if(shrink)d3d_rt_commit_size(size);else d3d_rt_commit();
}
static void check_sync(void *data) { unsigned *n=data;if(*n!=seen)atomic_store(&bad,3);++syncs; }
int main(int argc,char **argv) {
    alarm(30);
    int noisy=argc>1&&atoi(argv[1]);
    arm_order_probes();
    d3d_rt_start();
    pthread_t waker;
    if(noisy&&threaded)pthread_create(&waker,NULL,spurious,NULL);
    uint64_t start=void_monotonic_ns();
    for(unsigned f=0;f<80;++f) {
        for(unsigned j=0;j<64;++j)record(64+(j%7)*16,0,threaded?20:0,j%3==0);
        record(64,1,0,0);d3d_rt_frame_wait(f%3==0?0:1);
        if(f%9==0) { unsigned expected=recorded;d3d_rt_sync(check_sync,&expected); }
    }
    d3d_rt_finish();
    if(seen!=recorded||frames_seen!=80||syncs!=9||atomic_load(&blocked))atomic_store(&bad,4);
    /* More than a ring of queued data exercises wrap markers and producer-full waits. */
    for(unsigned i=0;i<400;++i)record(128*1024,0,threaded?200:0,0);
    record(BIG_BYTES+4096,0,0,0);
    d3d_rt_finish();
    /* Empty waits, and work published only by a fence. */
    for(unsigned i=0;i<300;++i) {
        d3d_rt_finish();record(64,0,0,0);
        unsigned expected=recorded;d3d_rt_sync(check_sync,&expected);
    }
    if(noisy&&threaded) { atomic_store(&stop_spurious,1);pthread_join(waker,NULL); }
    if(seen!=recorded||atomic_load(&blocked)||atomic_load(&frames_queued))atomic_store(&bad,5);
    printf("%s arm_probes=3 records=%u frames=%u syncs=%u game_wakes=%u render_wakes=%u blocks=%u ns=%llu noisy=%d threaded=%d\n",
        atomic_load(&bad)?"FAIL":"PASS",seen,frames_seen,syncs,atomic_load(&game_wakes),atomic_load(&render_wakes),
        atomic_load(&blocks),(unsigned long long)(void_monotonic_ns()-start),noisy,threaded);
    return atomic_load(&bad)?1:0;
}
'''
manifest = {}
for name, path in (
    ('before', B.ROOT / 'build/arm64-pgogen/host-snapshot/android/host/d3d9_thread.c'),
    ('after', B.ROOT / 'android/host/d3d9_thread.c'),
):
    original = path.read_text()
    queue = original[:original.index('/* ------------------------------------------------------------- the mirror')]
    queue = queue.replace('#include "d3d9_rt_cores.inc"', (B.ROOT / 'android/host/d3d9_rt_cores.inc').read_text())
    queue = '\n'.join(line for line in queue.splitlines() if not line.startswith('#include'))
    assert queue.count('atomic_store(&game_sleeping, 1);') == 1
    assert queue.count('atomic_store(&render_sleeping, 1);') == 1
    queue = queue.replace('atomic_store(&game_sleeping, 1);', 'fixture_before_game_arm(); atomic_store(&game_sleeping, 1);')
    queue = queue.replace('atomic_store(&render_sleeping, 1);', 'fixture_before_render_arm(); atomic_store(&render_sleeping, 1);')
    cfile, binary = OUT / f'{name}.c', OUT / f'{name}.elf'
    cfile.write_text(PRELUDE + queue + CHECKS)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-std=gnu11', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-Wno-unused-variable', str(cfile), '-o', str(binary)], check=True)
    manifest[name] = {'source': str(path), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                      'queue_sha256': hashlib.sha256(queue.encode()).hexdigest(), 'binary': str(binary)}
    print(binary)
    if name == 'after':
        # These intentionally broken copies prove the interleaving probes catch
        # checking a predicate before its sleeping flag becomes visible.
        for probe in ('game', 'render'):
            if probe == 'game':
                good = 'for (;;) {\n        fixture_before_game_arm(); atomic_store(&game_sleeping, 1);\n        if (game_waiting_done(until, frames)) break;'
                broken = 'while (!game_waiting_done(until, frames)) {\n        fixture_before_game_arm(); atomic_store(&game_sleeping, 1);'
            else:
                good = 'for (;;) {\n                    fixture_before_render_arm(); atomic_store(&render_sleeping, 1);\n                    if (atomic_load_explicit(&head, memory_order_acquire) != at) break;'
                broken = 'while (atomic_load_explicit(&head, memory_order_acquire) == at) {\n                    fixture_before_render_arm(); atomic_store(&render_sleeping, 1);'
            assert queue.count(good) == 1, probe
            path = OUT / f'negative-{probe}.c'
            path.write_text(PRELUDE + queue.replace(good, broken) + CHECKS.replace('alarm(30);', 'alarm(2);'))
            subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-std=gnu11', str(path),
                            '-o', str(path.with_suffix('.elf'))], check=True)
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2))
print('Run before.elf and after.elf, each with arguments 0, 1; also S2_RENDER_THREAD=0 after.elf 0.')
print('negative-game.elf 0 and negative-render.elf 0 must be killed by SIGALRM (exit142).')
