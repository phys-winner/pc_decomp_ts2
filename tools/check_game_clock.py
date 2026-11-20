"""Execute the production guest clock with deterministic foreground/background time."""
import ctypes
import _ctypes
from pathlib import Path
import subprocess
import tempfile

import build_android as B


def main():
    source = (B.ROOT / 'android/host/kernel32.c').read_text(encoding='utf-8')
    constants = '\n'.join(line for line in source.splitlines() if line.startswith(('#define QPC_HZ ', '#define TSC_HZ '))) + '\n'
    clocks = source[source.index('static pthread_mutex_t clock_lock'):source.index('uint64_t void_now_100ns')]
    shims = source[source.index('WINAPI_EXPORT(K32, GetTickCount'):source.index('WINAPI_EXPORT(K32, GetSystemTimeAsFileTime')]
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
#define atomic_compare_exchange_strong_explicit __c11_atomic_compare_exchange_strong
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
static uint64_t guest[8];
static uint32_t g_eax, g_esp, timer_cache;
#define MEM32(a) (*(uint32_t *)((a) == 0xA10CB63Cu ? &timer_cache : (uint32_t *)((char *)guest + (a))))
#define MEM64(a) (*(uint64_t *)((char *)guest + (a)))
#define ARG(i) MEM32(g_esp + 4u + 4u * (i))
#define WINAPI_EXPORT(dll, name, nargs) static uint32_t name(void)
'''
    checks = r'''
static uint64_t read_qpc(void) {
    ARG(0) = 32;
    return QueryPerformanceCounter() ? MEM64(32) : 0;
}

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

    const uint64_t qpc = read_qpc(), tsc = void_rdtsc();
    if (qpc != 3600ull * QPC_HZ || GetTickCount() != 3600000) return 15;
    if (!QueryPerformanceFrequency() || MEM64(32) != 10000000) return 16;
    void_clock_pause(1);
    now += 3600000000ull;
    if (read_qpc() != qpc || void_rdtsc() != tsc) return 17;
    ARG(1) = 40;
    timer_cache = 48;
    sub_A01C906A();
    if (g_eax != 1 || g_esp != 4 || MEM64(32) != 10000000 ||
        MEM64(40) != 3400000000ull || MEM64(48) != MEM64(40)) return 18;
    g_esp = 0;
    void_clock_pause(0);
    now += 430000000000ull;
    if (read_qpc() - qpc != 430ull * QPC_HZ || void_rdtsc() - tsc != 430ull * TSC_HZ ||
        GetTickCount() != 4030000) return 19;
    void_set_time_scale(2);
    now += 1000000000ull;
    if (read_qpc() - qpc != 432ull * QPC_HZ || void_rdtsc() - tsc != 432ull * TSC_HZ) return 20;
    return locks || bad;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-clock-') as directory:
        base = Path(directory)
        code, obj, dll = base/'check.c', base/'check.obj', base/'check.dll'
        code.write_text(prelude + constants + clocks + shims + checks)
        subprocess.run([str(B.CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-strict-aliasing', '-fno-stack-protector', '-c', str(code), '-o', str(obj)], check=True)
        subprocess.run([str(B.CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        library = ctypes.CDLL(str(dll))
        try:
            status = library.check()
            assert status == 0, status
        finally:
            _ctypes.FreeLibrary(library._handle)
    print('PASS actual-C game clock: 10000 pause cycles, scaling, fixed calibration and synchronized QPC/ticks/TSC')


if __name__ == '__main__':
    main()
