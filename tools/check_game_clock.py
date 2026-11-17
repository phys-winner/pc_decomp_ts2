"""Execute the production guest clock with deterministic foreground/background time."""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile

import build_android as B


def main():
    source = (B.ROOT / 'android/host/kernel32.c').read_text()
    clocks = source[source.index('static pthread_mutex_t clock_lock'):source.index('static uint64_t boot_ns;')]
    prelude = r'''
#include <stdint.h>
typedef _Atomic int atomic_int;
typedef _Atomic unsigned atomic_uint;
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_acq_rel __ATOMIC_ACQ_REL
#define memory_order_release __ATOMIC_RELEASE
#define atomic_load_explicit __c11_atomic_load
#define atomic_store_explicit __c11_atomic_store
#define atomic_fetch_add_explicit __c11_atomic_fetch_add
#define atomic_thread_fence __c11_atomic_thread_fence
#define atomic_load(p) __c11_atomic_load(p, __ATOMIC_SEQ_CST)
#define PTHREAD_MUTEX_INITIALIZER 0
#define isfinite(x) __builtin_isfinite(x)
typedef int pthread_mutex_t;
int _fltused;
static int locks, bad;
static uint64_t now = 10000000000000ull;
static void pthread_mutex_lock(pthread_mutex_t *p) { if (locks++) bad = 1; }
static void pthread_mutex_unlock(pthread_mutex_t *p) { if (--locks) bad = 2; }
static int inject_pause;
void void_clock_pause(int paused);
static uint64_t void_monotonic_ns(void) {
    uint64_t captured = now;
    if (inject_pause) { inject_pause = 0; void_clock_pause(1); now += 900000000000ull; void_clock_pause(0); }
    return captured;
}
static void void_log(const char *fmt, ...) {}
'''
    checks = r'''
__declspec(dllexport) int check(void) {
    uint64_t t = game_ns();
    if (t != now) return 1;
    void_clock_pause(1);
    now += 300000000000ull;
    if (game_ns() != t) return 2;
    void_clock_pause(1);
    if (game_ns() != t) return 3;
    void_clock_pause(0);
    if (game_ns() != t || atomic_load(&clock_scaled)) return 4;
    now += 123456789ull;
    if (game_ns() != t + 123456789ull) return 5;
    t = game_ns();
    void_set_time_scale(2);
    now += 1000;
    if (game_ns() != t + 2000) return 6;
    t = game_ns();
    void_clock_pause(1);
    now += 999999;
    void_set_time_scale(.5);
    if (game_ns() != t) return 7;
    void_clock_pause(0);
    now += 1000;
    if (game_ns() != t + 500) return 8;
    t = game_ns();
    void_set_time_scale(1);
    if (game_ns() != t || atomic_load(&clock_scaled)) return 9;
    void_set_time_scale(0);
    void_set_time_scale(__builtin_nan(""));
    void_set_time_scale(__builtin_inf());
    now += 321;
    if (game_ns() != t + 321 || atomic_load(&clock_scaled)) return 10;
    for (int i = 0; i < 10000; ++i) {
        t = game_ns();
        void_clock_pause(1); now += 123456789;
        if (game_ns() != t) return 11;
        void_clock_pause(0); void_clock_pause(0);
        if (game_ns() != t || atomic_load(&clock_scaled)) return 12;
        now += 100;
        if (game_ns() != t + 100) return 13;
    }
    t = game_ns(); inject_pause = 1;
    if (game_ns() != t || inject_pause) return 14;
    return locks || bad;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-clock-') as directory:
        base = Path(directory)
        code, obj, dll = base/'check.c', base/'check.obj', base/'check.dll'
        code.write_text(prelude + clocks + checks)
        subprocess.run([str(B.CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(code), '-o', str(obj)], check=True)
        subprocess.run([str(B.CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        library = ctypes.CDLL(str(dll))
        try:
            assert library.check() == 0
        finally:
            _ctypes.FreeLibrary(library._handle)
    print('PASS actual-C game clock: pause, duplicate transitions, scale changes, 10000 resume cycles, lock-free normal clock and invalid scale rejection')


if __name__ == '__main__':
    main()
