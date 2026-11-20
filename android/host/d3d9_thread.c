/* The render thread. The game's thread records what the GL side of Direct3D
 * 9 has to do as calls (a function and a copy of its arguments) in a ring,
 * and one host thread that owns the GL context runs them in order, at most a
 * frame behind: on a draw the game's thread now pays for the state that
 * changed since the last one (d3d_rt_sync_state) and a few words, the GL
 * driver and our translation run beside it. Resource data travels by copy,
 * made where D3D hands it over (Unlock); whatever has to answer the game now
 * (a read-back) waits for the thread to get there first. Query results are
 * published by generation as the GPU completes them.
 * S2_RENDER_THREAD=0 runs every call inline, in the same order, through the
 * same code. */
#define _GNU_SOURCE
#include "d3d9_internal.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "d3d9_thread.inc"
#include "d3d9_rt_cores.inc"

#define RING_BYTES (32u << 20)
#define BIG_BYTES (RING_BYTES / 8)      /* larger payloads go on the heap */

typedef struct {
    uint32_t size;                      /* the whole record, 16-aligned; 0 in a wrap marker's place: skip to the end */
    uint32_t wrap;
    d3d_rt_fn fn;
} rec_t;

/* The two threads' positions sit on cache lines of their own: sharing one,
 * every record either side finished moved the line between the cores. The
 * game's thread publishes in batches (PUBLISH_*) and before any wait, and
 * reads the render thread's position only when the ring looks full. */
#define LINE __attribute__((aligned(128)))
#define PUBLISH_BYTES (64u << 10)
#define PUBLISH_RECORDS 48u
static uint8_t *ring;
static _Atomic uint64_t head LINE;      /* bytes published (the game's thread) */
static _Atomic uint64_t tail LINE;      /* bytes run (the render thread) */
static _Atomic int render_sleeping LINE;
static _Atomic int game_sleeping LINE;
static _Atomic uint64_t game_wait_until;
static _Atomic int game_wait_frames;
static uint64_t written LINE;           /* the game's thread: bytes recorded */
static uint64_t tail_seen;              /* its last look at tail */
static uint32_t unpublished;            /* records since the last publish */
static uint64_t open_at;                /* the record d3d_rt_begin handed out */
static uint32_t open_size;
static void *open_big;                  /* its heap payload, if big */
static d3d_rt_fn open_fn;
static int threaded = -1;
static pthread_t render_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t to_render = PTHREAD_COND_INITIALIZER, to_game = PTHREAD_COND_INITIALIZER;
static int (*_Atomic idle_poll)(int idle);
static _Atomic uint32_t frames_queued LINE;   /* presents recorded and not yet run */
static __thread int on_render;
/* Inline mode: each open call's arguments, a level per nested call (a call
 * that records another runs it before its own returns). */
#define INLINE_DEPTH 8
static __thread uint8_t *inline_payload[INLINE_DEPTH];
static __thread size_t inline_cap[INLINE_DEPTH];
static __thread int inline_depth;
static __thread d3d_rt_fn inline_fn[INLINE_DEPTH];

int d3d_rt_threaded(void) { return threaded > 0; }
int d3d_rt_on_render_thread(void) { return on_render || threaded <= 0; }
void d3d_rt_set_idle(int (*poll)(int idle)) { atomic_store_explicit(&idle_poll, poll, memory_order_release); }

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

static void wake(_Atomic int *sleeping, pthread_cond_t *cond)
{
    if (atomic_load(sleeping)) {
        pthread_mutex_lock(&lock);
        if (atomic_exchange(sleeping, 0)) pthread_cond_broadcast(cond);
        pthread_mutex_unlock(&lock);
    }
}

/* The game's thread waits for the render thread to run up to `until` bytes
 * (or, with frames, until at most that many presents are queued). Only the
 * wait for a frame lets other guest threads have the machine: there, as at
 * the swap it replaces, no D3D call is half done. */
static int game_waiting_done(uint64_t until, int frames)
{
    return frames >= 0 ? atomic_load(&frames_queued) <= (uint32_t)frames : atomic_load(&tail) >= until;
}

/* What the game's thread recorded becomes the render thread's to run. */
static void publish(void)
{
    unpublished = 0;
    if (atomic_load_explicit(&head, memory_order_relaxed) == written) return;
    atomic_store_explicit(&head, written, memory_order_release);
    wake(&render_sleeping, &to_render);
}

/* Waits of the game's thread for the render thread (frame statistics). */
static uint64_t sync_count, wait_ns;

static void game_wait(uint64_t until, int frames)
{
    publish();
    if (game_waiting_done(until, frames)) return;
    uint64_t start = void_monotonic_ns();
    for (int spin = 0; spin < 2000; ++spin) {
        if (game_waiting_done(until, frames)) { wait_ns += void_monotonic_ns() - start; return; }
        cpu_relax();
    }
    if (frames >= 0) void_blocking_begin();
    pthread_mutex_lock(&lock);
    atomic_store(&game_wait_until, until);
    atomic_store(&game_wait_frames, frames);
    for (;;) {
        atomic_store(&game_sleeping, 1);
        if (game_waiting_done(until, frames)) break;
        if (frames >= 0) {
            pthread_cond_wait(&to_game, &lock);
            continue;
        }
        /* Mid-call, only the audio producer may have the machine (ProcessVertices
         * and readbacks waited 50-100 ms here on Mali): check every millisecond. */
        pthread_mutex_unlock(&lock);
        int lent = void_machine_lend_urgent();
        pthread_mutex_lock(&lock);
        if (lent) continue;
        struct timespec until_ts;
        clock_gettime(CLOCK_REALTIME, &until_ts);
        until_ts.tv_nsec += 1000000L;
        if (until_ts.tv_nsec >= 1000000000L) { until_ts.tv_nsec -= 1000000000L; ++until_ts.tv_sec; }
        if (!game_waiting_done(until, frames)) pthread_cond_timedwait(&to_game, &lock, &until_ts);
    }
    atomic_store(&game_sleeping, 0);
    pthread_mutex_unlock(&lock);
    if (frames >= 0) void_blocking_end();
    wait_ns += void_monotonic_ns() - start;
}

/* Drop the inherited guest mask after EGL setup so driver workers keep it. */
static void leave_game_cores(void)
{
    static const char *const policies[] = { "auto", "big", "next", "any" };
    char file_policy[16] = "auto";
    const char *env = getenv("VOID_RT_CORES");
    if (!env || !*env) {
        const char *data = void_data_dir();
        if (data && *data) {
            char path[1100];
            snprintf(path, sizeof path, "%s/rt-cores", data);
            FILE *f = fopen(path, "r");
            if (f) { if (fscanf(f, "%15s", file_policy) != 1) strcpy(file_policy, "auto"); fclose(f); }
        }
        env = file_policy;
    }
    int policy = -1;
    for (int i = 0; i < 4; ++i) if (!strcmp(env, policies[i])) policy = i;
    if (policy < 0) { void_log("[rt] invalid VOID_RT_CORES policy; using auto"); policy = RT_CORES_AUTO; }
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 0 || n > CPU_SETSIZE) { void_log("[rt] render thread on inherited CPUs (policy %s, CPU count unknown)", policies[policy]); return; }
    long capacity[CPU_SETSIZE];
    for (int i = 0; i < n; ++i) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
        FILE *f = fopen(path, "r"); capacity[i] = 0;
        if (f) { if (fscanf(f, "%ld", &capacity[i]) != 1) capacity[i] = 0; fclose(f); }
    }
    cpu_set_t mine;
    int selected = d3d_rt_select_cores(capacity, (int)n, policy, &mine);
    if (!selected) {                         /* Undo a guest pin even without usable topology. */
        CPU_ZERO(&mine);
        for (int i = 0; i < n; ++i) CPU_SET(i, &mine);
    }
    if (sched_setaffinity(0, sizeof mine, &mine) == 0) {
        sched_getaffinity(0, sizeof mine, &mine);
        char list[160] = "";
        for (int i = 0, used = 0; i < n && used < 150; ++i)
            if (CPU_ISSET(i, &mine)) used += snprintf(list + used, sizeof list - (size_t)used, "%s%d", used ? "," : "", i);
        void_log("[rt] render thread on %d cores [%s] (policy %s, %s)", CPU_COUNT(&mine), list, policies[policy],
            !selected || policy == RT_CORES_ANY ? "unrestricted" : "capacity tier");
    } else void_log("[rt] render thread on inherited CPUs (policy %s, affinity call failed)", policies[policy]);
}

void d3d_rt_apply_cores(void)
{
    static __thread int applied;
    if (on_render && !applied) { applied = 1; leave_game_cores(); }
}

static _Atomic int render_tid;
int d3d_rt_render_tid(void) { return atomic_load(&render_tid); }

static void *render_main(void *unused)
{
    (void)unused;
    on_render = 1;
    atomic_store(&render_tid, (int)gettid());
    pthread_setname_np(pthread_self(), "s2-render");
    uint64_t at = 0;
    for (;;) {
        if (atomic_load_explicit(&head, memory_order_acquire) == at) {
            int (*poll)(int) = atomic_load_explicit(&idle_poll, memory_order_acquire);
            for (int idle = 1; poll && atomic_load_explicit(&head, memory_order_acquire) == at && poll(idle); idle = 2) {
                for (int spin = 0; spin < 200 && atomic_load_explicit(&head, memory_order_acquire) == at; ++spin)
                    cpu_relax();
                if (atomic_load_explicit(&head, memory_order_acquire) != at) break;
                struct timespec nap = { 0, 50000 };
                nanosleep(&nap, NULL);
            }
            int spin = 0;
            while (atomic_load_explicit(&head, memory_order_acquire) == at && spin < 4000) { cpu_relax(); ++spin; }
            if (atomic_load_explicit(&head, memory_order_acquire) == at) {
                pthread_mutex_lock(&lock);
                for (;;) {
                    atomic_store(&render_sleeping, 1);
                    if (atomic_load_explicit(&head, memory_order_acquire) != at) break;
                    pthread_cond_wait(&to_render, &lock);
                }
                atomic_store(&render_sleeping, 0);
                pthread_mutex_unlock(&lock);
            }
        }
        uint64_t published = atomic_load_explicit(&head, memory_order_acquire);
        while (at != published) {
            rec_t *r = (rec_t *)(ring + (at % RING_BYTES));
            if (r->wrap) {
                at += RING_BYTES - (at % RING_BYTES);
            } else {
                r->fn((uint8_t *)r + sizeof *r);
                at += r->size;
            }
            atomic_store_explicit(&tail, at, memory_order_release);
            /* Byte waits finish at their target; frame waits only need a completed Present. */
            if (atomic_load(&game_sleeping) && game_waiting_done(atomic_load(&game_wait_until),
                atomic_load(&game_wait_frames))) wake(&game_sleeping, &to_game);
        }
        int (*poll)(int) = atomic_load_explicit(&idle_poll, memory_order_acquire);
        if (poll) poll(0);
    }
    return NULL;
}

void d3d_rt_start(void)
{
    if (threaded >= 0) return;
    const char *env = getenv("S2_RENDER_THREAD");
    threaded = !(env && *env == '0');
    if (!threaded) { void_log("[rt] render thread off: GL calls run inline"); return; }
    ring = (uint8_t *)aligned_alloc(64, RING_BYTES);
    if (!ring || pthread_create(&render_thread, NULL, render_main, NULL) != 0) {
        void_log("[rt] no render thread: GL calls run inline");
        threaded = 0;
        return;
    }
    void_log("[rt] render thread started (%u MB ring)", RING_BYTES >> 20);
}

/* GL work that waits (vsync, the frame limiter, the activity's window): run
 * inline on the game's thread it lets other guest threads have the machine
 * meanwhile; the render thread holds none to let go. */
void d3d_rt_wait_begin(void) { if (!on_render) void_blocking_begin(); }
void d3d_rt_wait_end(void) { if (!on_render) void_blocking_end(); }

/* A call record with `size` bytes of arguments: returns where to write them. */
void *d3d_rt_begin(d3d_rt_fn fn, uint32_t size)
{
    if (threaded <= 0 || on_render) {
        int d = inline_depth++;
        if (d >= INLINE_DEPTH) void_fatal("render calls nested %d deep", d);
        if (size > inline_cap[d]) {
            free(inline_payload[d]);
            inline_cap[d] = size > 4096 ? size : 4096;
            inline_payload[d] = (uint8_t *)malloc(inline_cap[d]);
        }
        inline_fn[d] = fn;
        return inline_payload[d];
    }
    open_fn = fn;
    open_big = NULL;
    uint32_t payload = size;
    if (size > BIG_BYTES) {
        open_big = malloc(size);
        payload = sizeof(void *) + sizeof(d3d_rt_fn);
    }
    uint32_t need = (uint32_t)((sizeof(rec_t) + payload + 15u) & ~15u);
    uint64_t at = written;
    uint32_t room_to_end = RING_BYTES - (uint32_t)(at % RING_BYTES);
    if (room_to_end < need) {
        /* No record straddles the end: mark the rest skipped. */
        if (at + room_to_end - tail_seen > RING_BYTES) {
            tail_seen = atomic_load_explicit(&tail, memory_order_acquire);
            if (at + room_to_end - tail_seen > RING_BYTES) {
                game_wait(at + room_to_end - RING_BYTES, -1);
                tail_seen = atomic_load_explicit(&tail, memory_order_acquire);
            }
        }
        ((rec_t *)(ring + at % RING_BYTES))->wrap = 1;
        at += room_to_end;
        written = at;
        publish();
    }
    if (at + need - tail_seen > RING_BYTES) {
        tail_seen = atomic_load_explicit(&tail, memory_order_acquire);
        if (at + need - tail_seen > RING_BYTES) {
            game_wait(at + need - RING_BYTES, -1);
            tail_seen = atomic_load_explicit(&tail, memory_order_acquire);
        }
    }
    rec_t *r = (rec_t *)(ring + at % RING_BYTES);
    r->wrap = 0;
    r->size = need;
    r->fn = fn;
    open_at = at;
    open_size = need;
    return open_big ? open_big : (uint8_t *)r + sizeof *r;
}

static void run_big(void *payload)
{
    void *p;
    d3d_rt_fn fn;
    memcpy(&p, payload, sizeof p);
    memcpy(&fn, (uint8_t *)payload + sizeof p, sizeof fn);
    fn(p);
    free(p);
}

/* Publish the open record (inline: run it now). */
void d3d_rt_commit(void)
{
    if (threaded <= 0 || on_render) {
        int d = inline_depth - 1;
        inline_fn[d](inline_payload[d]);
        --inline_depth;
        return;
    }
    rec_t *r = (rec_t *)(ring + open_at % RING_BYTES);
    if (open_big) {
        r->fn = run_big;
        uint8_t *p = (uint8_t *)r + sizeof *r;
        memcpy(p, &open_big, sizeof open_big);
        memcpy(p + sizeof open_big, &open_fn, sizeof open_fn);
        open_big = NULL;
    }
    written = open_at + open_size;
    if (++unpublished >= PUBLISH_RECORDS || written - atomic_load_explicit(&head, memory_order_relaxed) >= PUBLISH_BYTES)
        publish();
}

/* Hand over what is recorded now (a frame's end: the render thread starts on
 * it while the game goes on). */
void d3d_rt_flush(void)
{
    if (threaded > 0 && !on_render) publish();
}

void d3d_rt_call(d3d_rt_fn fn, const void *args, uint32_t size)
{
    void *p = d3d_rt_begin(fn, size);
    if (size) memcpy(p, args, size);
    d3d_rt_commit();
}

/* Everything recorded so far has run. */
void d3d_rt_finish(void)
{
    if (threaded <= 0 || on_render) return;
    game_wait(written, -1);
    tail_seen = atomic_load_explicit(&tail, memory_order_acquire);
}

/* fn(arg) on the render thread, after everything before it; the game's
 * thread waits for it (arg may point at the caller's locals). */
typedef struct { d3d_rt_fn fn; void *arg; } sync_call;
static void run_sync(void *payload) { sync_call *c = (sync_call *)payload; c->fn(c->arg); }

void d3d_rt_wait_stats(uint64_t *syncs, uint64_t *waited_ns) { *syncs = sync_count; *waited_ns = wait_ns; }

void d3d_rt_sync(d3d_rt_fn fn, void *arg)
{
    if (threaded <= 0 || on_render) { fn(arg); return; }
    ++sync_count;
    sync_call c = { fn, arg };
    d3d_rt_call(run_sync, &c, sizeof c);
    d3d_rt_finish();
    /* Long readback batches must let the guest audio decoder refill between completed operations. */
    if (recomp_yield_hook) recomp_yield_hook();
}

/* A present about to be recorded, then the wait after it: the game's thread
 * runs at most `ahead` frames before the render thread (1: it records frame
 * N+1 while N is drawn). */
void d3d_rt_frame_queued(void)
{
    if (threaded > 0) atomic_fetch_add(&frames_queued, 1);
}

void d3d_rt_frame_wait(int ahead)
{
    if (threaded > 0) game_wait(0, ahead);
}

void d3d_rt_frame_done(void)
{
    if (threaded <= 0) return;
    atomic_fetch_sub(&frames_queued, 1);
}

/* Publish the open record with only `used` bytes of its arguments. */
void d3d_rt_commit_size(uint32_t used)
{
    if (threaded > 0 && !on_render && !open_big) {
        uint32_t need = (uint32_t)((sizeof(rec_t) + used + 15u) & ~15u);
        if (need < open_size) {
            ((rec_t *)(ring + open_at % RING_BYTES))->size = need;
            open_size = need;
        }
    }
    d3d_rt_commit();
}

/* ------------------------------------------------------------- the mirror
 * The render thread draws through a device of its own whose state follows
 * the game's: before each call that reads it the game's thread sends what
 * changed since the last one, as (offset, bytes) patches. Only 64-byte pieces
 * touched by setters are compared against what was sent. Resource pointers
 * travel as they are: the game's state holds
 * a reference on each, and a resource's destruction is recorded after the
 * state change that let it go. The render targets travel the same way, after
 * the state, at TARGETS_AT. */
static d3d_device mirror;
static d3d_state sent;
typedef struct { d3d_surface *rt[4], *ds, *backbuffer; } targets_t;
static targets_t sent_targets;
#define TARGETS_AT ((uint32_t)((sizeof(d3d_state) + 63u) & ~63u))
_Static_assert(TARGETS_AT + sizeof(targets_t) <= UINT16_MAX * 4u,
    "render-state patch offsets and lengths must fit 16-bit word counts");
#define CONSTANTS_LO ((uint32_t)offsetof(d3d_state, vs_f))
#define CONSTANTS_HI ((uint32_t)offsetof(d3d_state, viewport))

d3d_device *d3d_rt_device(void) { return &mirror; }

typedef struct { uint8_t *at, *last; uint32_t last_end; } patcher;

static void patch(patcher *w, uint32_t off, const void *data, uint32_t len)
{
    if (w->last && w->last_end == off) {          /* extends the previous patch */
        uint16_t h[2];
        memcpy(h, w->last, 4);
        h[1] = (uint16_t)(h[1] + len / 4);
        memcpy(w->last, h, 4);
    } else {
        uint16_t h[2] = { (uint16_t)(off / 4), (uint16_t)(len / 4) };
        w->last = w->at;
        memcpy(w->at, h, 4);
        w->at += 4;
    }
    memcpy(w->at, data, len);
    w->at += len;
    w->last_end = off + len;
}

/* Whether 64 bytes differ: inline, the compiler's vector compare (a memcmp
 * call per piece was most of the cost of a draw's sync). */
static inline int piece_differs(const uint8_t *a, const uint8_t *b)
{
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
        uint64_t u, v;
        memcpy(&u, a + 8 * i, 8);
        memcpy(&v, b + 8 * i, 8);
        x |= u ^ v;
    }
    return x != 0;
}

static void compare(patcher *w, const uint8_t *now, uint8_t *was, uint32_t *bits, uint32_t *active, uint32_t groups)
{
    for (uint32_t group = 0; group < groups; ++group) {
        uint32_t pending = active[group];
        active[group] = 0;
        while (pending) {
            uint32_t word = group * 32u + (uint32_t)__builtin_ctz(pending);
            pending &= pending - 1;
            if (word >= (sizeof(d3d_state) + 2047u) / 2048u) continue;
            uint32_t b = bits[word];
            bits[word] = 0;
            while (b) {
                uint32_t off = (word * 32u + (uint32_t)__builtin_ctz(b)) * 64u;
                b &= b - 1;
                if (off >= sizeof(d3d_state)) continue;
                uint32_t len = sizeof(d3d_state) - off < 64 ? (uint32_t)sizeof(d3d_state) - off : 64;
                if (len == 64 ? !piece_differs(now + off, was + off) : !memcmp(now + off, was + off, len)) continue;
                memcpy(was + off, now + off, len);
                patch(w, off, now + off, len);
            }
        }
    }
}

static void apply_state(void *args)
{
    const uint8_t *p = (const uint8_t *)args;
    for (;;) {
        uint16_t h[2];
        memcpy(h, p, 4);
        p += 4;
        if (!h[1]) break;
        uint32_t off = h[0] * 4u, len = h[1] * 4u;
        if (off >= TARGETS_AT) {
            targets_t t;
            memcpy(&t, p, sizeof t);
            memcpy(mirror.rt, t.rt, sizeof t.rt);
            mirror.ds = t.ds;
            mirror.backbuffer = t.backbuffer;
            mirror.fbo_dirty = 1;
        } else {
            memcpy((uint8_t *)&mirror.st + off, p, len);
            if (off < CONSTANTS_HI && off + len > CONSTANTS_LO) ++d3d_constant_generation;
        }
        p += len;
    }
}

void d3d_rt_sync_state(d3d_device *d)
{
    uint32_t reserve = (uint32_t)sizeof(d3d_state) + 4u * ((sizeof(d3d_state) + 63u) / 64u) + 8u + (uint32_t)sizeof(targets_t);
    uint8_t *start = (uint8_t *)d3d_rt_begin(apply_state, reserve);
    patcher w = { start, NULL, 0 };
    const uint8_t *now = (const uint8_t *)&d->st;
    uint8_t *was = (uint8_t *)&sent;
    compare(&w, now, was, d->state_dirty, d->state_dirty_active, sizeof d->state_dirty_active / sizeof d->state_dirty_active[0]);
    targets_t t;
    memcpy(t.rt, d->rt, sizeof t.rt);
    t.ds = d->ds;
    t.backbuffer = d->backbuffer;
    if (memcmp(&t, &sent_targets, sizeof t)) {
        sent_targets = t;
        w.last = NULL;
        patch(&w, TARGETS_AT, &t, sizeof t);
    }
    uint16_t end[2] = { 0, 0 };
    memcpy(w.at, end, 4);
    w.at += 4;
    d3d_rt_commit_size((uint32_t)(w.at - start));
}

/* The mirror starts as the device does (CreateDevice, Reset): its present
 * parameters, frame count and state follow with the first sync. */
typedef struct { uint32_t pp[14]; uint32_t frame; } mirror_init_args;
static void mirror_init(void *args)
{
    const mirror_init_args *a = (const mirror_init_args *)args;
    memcpy(mirror.pp, a->pp, sizeof mirror.pp);
    mirror.frame = a->frame;
    mirror.fbo_dirty = 1;
}

void d3d_rt_device_reset(d3d_device *d)
{
    mirror_init_args a;
    memcpy(a.pp, d->pp, sizeof a.pp);
    a.frame = d->frame;
    d3d_dirty_all(d);
    d3d_rt_call(mirror_init, &a, sizeof a);
}
