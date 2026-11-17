/* Where load time goes: wall time per host service family (path resolution,
 * file I/O, sleeps, waits, the machine), summed over all guest threads and
 * separately for the game's main thread, with the main thread's CPU time.
 * Loading is mostly not CPU-bound on Android's shared storage (FUSE), and a
 * CPU profile shows none of the waiting; these counters attribute it.
 *
 * A reporter thread logs the change every 2 seconds while anything moved:
 *   [iostat] 2.0 s main cpu 0.31 held 0.40 | path 812 0.71 s, open 120 0.12 s, read 3400 1.20 s 34.1 MB ...
 * Each family is "calls seconds" over the interval; main: the main guest
 * thread's share of the same counters. S2_IOSTAT=0 turns the reporter off. */
#define _GNU_SOURCE
#include "host.h"
#include "iostat.h"
#include "readcache.h"

void void_files_trace_flush(void);   /* files.c */
#include <malloc.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef struct { _Atomic uint64_t calls, ns, bytes; } counter_t;
static counter_t all[VS_COUNT], main_only[VS_COUNT];
static const char *const names[VS_COUNT] = {
    "path", "open", "read", "write", "attr", "find", "findnext", "copy", "dirop", "close", "sleep", "wait", "machine"
};
static pthread_t main_thread;
static _Atomic int main_known;

uint64_t void_stat_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void void_stat_add(int kind, uint64_t ns, uint64_t bytes)
{
    if ((unsigned)kind >= VS_COUNT) return;
    atomic_fetch_add_explicit(&all[kind].calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&all[kind].ns, ns, memory_order_relaxed);
    if (bytes) atomic_fetch_add_explicit(&all[kind].bytes, bytes, memory_order_relaxed);
    if (void_is_main_guest_thread()) {
        atomic_fetch_add_explicit(&main_only[kind].calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&main_only[kind].ns, ns, memory_order_relaxed);
        if (bytes) atomic_fetch_add_explicit(&main_only[kind].bytes, bytes, memory_order_relaxed);
    }
}

void void_stat_main_thread(void)
{
    main_thread = pthread_self();
    atomic_store(&main_known, 1);
}

static double thread_cpu(pthread_t t)
{
    clockid_t id;
    struct timespec ts;
    if (pthread_getcpuclockid(t, &id) || clock_gettime(id, &ts)) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int append(char *line, int at, int cap, const char *name, uint64_t calls, uint64_t ns, uint64_t bytes)
{
    if (!calls || at >= cap) return at;
    at += snprintf(line + at, (size_t)(cap - at), " %s %llu %.2f", name, (unsigned long long)calls, (double)ns / 1e9);
    if (bytes && at < cap) at += snprintf(line + at, (size_t)(cap - at), " %.1fMB", (double)bytes / 1048576.0);
    return at < cap ? at : cap;
}

/* Where the process's memory is, every 10 s (S2_MEMSTAT=0 turns it off), to
 * tell a leak from a cache filling up: resident and swapped totals, the guest
 * window (committed, resident, the Win32 heaps' footprint and peak), the host
 * heap, and what the GLES renderer holds on the GPU with the GLSL compile and
 * link time so far. A count that only ever grows across town/lot round trips
 * is the suspect. */
static long proc_kb(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long value = -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) { value = strtol(line + n, NULL, 10); break; }
    fclose(f);
    return value;
}

static void report_memory(void)
{
    uint64_t committed, reserved, resident, footprint, peak, released;
    int heaps;
    void_vm_stats(&committed, &reserved, &resident);
    void_heap_stats(&footprint, &peak, &heaps, &released);
    struct mallinfo host = mallinfo();
    void_gpu_stats gpu;
    memset(&gpu, 0, sizeof gpu);
    d3d_gpu_stats(&gpu);
    uint64_t hits, misses, stored;
    d3d_program_cache_stats(&hits, &misses, &stored);
    const double mb = 1048576.0;
    void_log("[mem] rss %.0f MB swap %.0f MB | guest committed %.0f resident %.0f heaps %d footprint %.0f peak %.0f MB "
        "(freed pages returned %.0f MB) | "
        "host malloc %.0f MB | gl textures %llu %.0f MB surfaces %llu %.0f MB buffers %llu %.0f MB programs %llu "
        "(links %llu %.2f s, compiles %llu %.2f s; binaries loaded %llu, missed %llu, stored %.1f MB)",
        proc_kb("/proc/self/status", "VmRSS:") / 1024.0, proc_kb("/proc/self/status", "VmSwap:") / 1024.0,
        committed / mb, resident / mb, heaps, footprint / mb, peak / mb, released / mb, (double)host.uordblks / mb,
        (unsigned long long)gpu.textures, gpu.texture_bytes / mb, (unsigned long long)gpu.surfaces, gpu.surface_bytes / mb,
        (unsigned long long)gpu.buffers, gpu.buffer_bytes / mb, (unsigned long long)gpu.programs,
        (unsigned long long)gpu.links, gpu.link_ns / 1e9, (unsigned long long)gpu.compiles, gpu.compile_ns / 1e9,
        (unsigned long long)hits, (unsigned long long)misses, stored / mb);
}

static void *reporter(void *arg)
{
    (void)arg;
    const char *memstat = getenv("S2_MEMSTAT");
    int memory_every = memstat && *memstat == '0' ? 0 : 5;   /* reporter ticks (2 s) */
    unsigned tick = 0;
    uint64_t last_all[VS_COUNT][3] = {{0}}, last_main[VS_COUNT][3] = {{0}};
    double last_cpu = 0;
    uint64_t last_time = void_stat_now(), last_hits = 0, last_misses = 0, last_writes = 0, last_runs = 0;
    for (;;) {
        struct timespec ts = { 2, 0 };
        while (nanosleep(&ts, &ts)) {}
        uint64_t now = void_stat_now();
        char line[1600];
        int at = 0, cap = (int)sizeof line, moved = 0;
        double cpu = atomic_load(&main_known) ? thread_cpu(main_thread) : 0;
        uint64_t d[VS_COUNT][3], m[VS_COUNT][3];
        for (int k = 0; k < VS_COUNT; ++k) {
            uint64_t a[3] = { atomic_load(&all[k].calls), atomic_load(&all[k].ns), atomic_load(&all[k].bytes) };
            uint64_t b[3] = { atomic_load(&main_only[k].calls), atomic_load(&main_only[k].ns), atomic_load(&main_only[k].bytes) };
            for (int i = 0; i < 3; ++i) {
                d[k][i] = a[i] - last_all[k][i];
                m[k][i] = b[i] - last_main[k][i];
                last_all[k][i] = a[i];
                last_main[k][i] = b[i];
            }
            /* Machine hand-overs and short sleeps happen every frame; only I/O,
             * waits over 100 ms in total or real sleeping count as activity. */
            if (k == VS_MACHINE ? 0 : k == VS_SLEEP || k == VS_WAIT ? d[k][1] > 100000000ull : d[k][0] != 0) moved = 1;
        }
        at += snprintf(line, sizeof line, "[iostat] %.1f s main cpu %.2f |", (double)(now - last_time) / 1e9, cpu - last_cpu);
        for (int k = 0; k < VS_COUNT; ++k) at = append(line, at, cap, names[k], m[k][0], m[k][1], m[k][2]);
        if (at < cap) at += snprintf(line + at, (size_t)(cap - at), " | all");
        for (int k = 0; k < VS_COUNT; ++k) at = append(line, at, cap, names[k], d[k][0], d[k][1], d[k][2]);
        uint64_t hits, misses;
        void_readcache_stats(&hits, &misses);
        if (at < cap && (hits != last_hits || misses != last_misses))
            at += snprintf(line + at, (size_t)(cap - at), " | rcache hit %llu miss %llu",
                (unsigned long long)(hits - last_hits), (unsigned long long)(misses - last_misses));
        last_hits = hits;
        last_misses = misses;
        uint64_t writes, runs;
        void_readcache_write_stats(&writes, &runs);
        if (at < cap && (writes != last_writes || runs != last_runs))
            at += snprintf(line + at, (size_t)(cap - at), " | wcache writes %llu runs %llu",
                (unsigned long long)(writes - last_writes), (unsigned long long)(runs - last_runs));
        last_writes = writes;
        last_runs = runs;
        last_cpu = cpu;
        last_time = now;
        if (moved) void_log("%s", line);
        if (memory_every && ++tick % (unsigned)memory_every == 0) report_memory();
        void_files_trace_flush();
    }
    return NULL;
}

void void_stat_start(void)
{
    const char *env = getenv("S2_IOSTAT");
    if (env && *env == '0') return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &attr, reporter, NULL)) pthread_setname_np(t, "s2-iostat");
    pthread_attr_destroy(&attr);
}
