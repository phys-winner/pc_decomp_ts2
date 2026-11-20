/* Guest threads, the machine lock, handles and Win32 synchronization.
 *
 * One host pthread per guest thread. Only the thread holding the machine runs
 * lifted code or touches the global register file; a host call that blocks
 * releases the machine around the wait (void_blocking_begin/end), and lifted
 * loops hand it over periodically (recomp_yield_hook), as native32 did on
 * Windows. The lock is a FIFO ticket lock so a yield really passes the
 * machine to a waiting thread instead of re-acquiring it immediately. */
#define _GNU_SOURCE
#include "host.h"
#include "iostat.h"
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>

#define TLS_SLOTS 1088
#define GUEST_STACK_DEFAULT (1u << 20)
#define HOST_STACK_SIZE (32u << 20)

struct void_thread {
    void_regs regs;
    int depth;
    uint32_t id;
    uint32_t tib, peb;
    uint32_t stack_lo, stack_hi;
    uint32_t last_error;
    uint32_t tls[TLS_SLOTS];
    void_object *object;
    uint32_t start, param;
    uint32_t exit_code;
    int suspended;
    int guest_owned;                 /* created by CreateThread */
    jmp_buf exit_jb;
    int exit_armed;
    pthread_t pt;
    struct void_thread *next_thread;   /* every guest thread, for TLS slot resets */
    /* User APCs (QueueUserAPC), run by the thread's next alertable wait. */
    struct { uint32_t fn, param; } apc[64];
    int apc_count;
    pthread_cond_t wake;             /* this thread's waits sleep here (wait_core, SleepEx) */
};

static struct void_thread *all_threads;
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

static __thread void_thread *t_self;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcond = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t ticket_next __attribute__((aligned(128)));
static _Atomic uint64_t ticket_serving __attribute__((aligned(128)));
static _Atomic int machine_sleepers __attribute__((aligned(128)));
static uint32_t next_thread_id = 0x104;
static uint32_t peb_va;
static uint8_t tls_used[TLS_SLOTS];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------- machine lock */
/* Time this thread spent waiting for the machine (frame statistics). */
static __thread uint64_t t_lock_wait_ns;
/* When this thread last took the machine, and whether it is the game's main
 * thread (pin_guest_thread): see yield_hook. */
static __thread uint64_t t_held_since;
static __thread int t_main_thread;
uint64_t void_lock_wait_ns(void) { return t_lock_wait_ns; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A FIFO ticket lock; a waiter sleeps on the condition (all guest threads
 * share the biggest cores, where spinning would only take the holder's turn). */
static void machine_wait(uint64_t ticket)
{
    if (atomic_load_explicit(&ticket_serving, memory_order_acquire) != ticket) {
        uint64_t start = now_ns();
        {
            pthread_mutex_lock(&mlock);
            atomic_fetch_add(&machine_sleepers, 1);
            while (atomic_load(&ticket_serving) != ticket) pthread_cond_wait(&mcond, &mlock);
            atomic_fetch_sub(&machine_sleepers, 1);
            pthread_mutex_unlock(&mlock);
        }
        uint64_t waited = now_ns() - start;
        t_lock_wait_ns += waited;
        void_stat_add(VS_MACHINE, waited, 0);
    }
    if (t_main_thread) t_held_since = now_ns();
}

static void machine_acquire(void)
{
    machine_wait(atomic_fetch_add(&ticket_next, 1));
}

static void machine_release(void)
{
    atomic_fetch_add(&ticket_serving, 1);
    if (atomic_load(&machine_sleepers)) {
        pthread_mutex_lock(&mlock);
        pthread_cond_broadcast(&mcond);
        pthread_mutex_unlock(&mlock);
    }
}

static int machine_contended(void)
{
    /* Racy read is fine: a missed waiter is picked up at the next edge. */
    return atomic_load_explicit(&ticket_next, memory_order_relaxed) - atomic_load_explicit(&ticket_serving, memory_order_relaxed) > 1;
}

/* UNICODE_STRING {Length, MaximumLength, Buffer} for a UTF-8 text. */
static void unicode_string(uint32_t at, const char *text)
{
    uint32_t units = void_utf8_to_gw(text, 0, 0);
    uint32_t buffer = void_galloc(2 * units);
    void_utf8_to_gw(text, buffer, units);
    MEM16(at) = (uint16_t)(2 * (units - 1));
    MEM16(at + 2) = (uint16_t)(2 * units);
    MEM32(at + 4) = buffer;
}

static uint32_t make_peb(void)
{
    if (peb_va) return peb_va;
    peb_va = void_galloc(0x1000);
    memset(GPTR(peb_va), 0, 0x1000);
    MEM8(peb_va + 2) = 0;                          /* BeingDebugged */
    MEM32(peb_va + 8) = void_module_exe_base();    /* ImageBaseAddress */
    MEM32(peb_va + 0x18) = void_process_heap();    /* ProcessHeap */
    MEM32(peb_va + 0x64) = 8;                      /* NumberOfProcessors */
    MEM32(peb_va + 0xA4) = 6;                      /* OSMajorVersion */
    MEM32(peb_va + 0xA8) = 2;                      /* OSMinorVersion */
    MEM16(peb_va + 0xAC) = 9200;                   /* OSBuildNumber */
    MEM32(peb_va + 0xB0) = 2;                      /* OSPlatformId: VER_PLATFORM_WIN32_NT */
    /* LoaderLock: an RTL_CRITICAL_SECTION nobody owns (crashpad's client
     * asks whether the current thread holds it). */
    uint32_t loader_lock = void_galloc(24);
    memset(GPTR(loader_lock), 0, 24);
    MEM32(loader_lock) = 0xFFFFFFFFu;              /* DebugInfo */
    MEM32(loader_lock + 4) = 0xFFFFFFFFu;          /* LockCount: free */
    MEM32(peb_va + 0xA0) = loader_lock;
    MEM32(peb_va + 0x1C) = loader_lock;            /* FastPebLock */
    /* Ldr: PEB_LDR_DATA with the exe as the only module of each list. */
    uint32_t ldr = void_galloc(0x30), entry = void_galloc(0x78);
    memset(GPTR(ldr), 0, 0x30);
    memset(GPTR(entry), 0, 0x78);
    MEM32(ldr) = 0x30;
    MEM8(ldr + 4) = 1;                             /* Initialized */
    for (uint32_t list = 0; list < 3; ++list) {    /* load, memory, initialization order */
        uint32_t head = ldr + 0x0C + 8 * list, link = entry + 8 * list;
        MEM32(head) = MEM32(head + 4) = link;
        MEM32(link) = MEM32(link + 4) = head;
    }
    MEM32(entry + 0x18) = void_module_exe_base();  /* DllBase */
    MEM32(entry + 0x1C) = void_module_exe_base() + MEM32(void_module_exe_base() + MEM32(void_module_exe_base() + 0x3C) + 0x28);
    MEM32(entry + 0x20) = MEM32(void_module_exe_base() + MEM32(void_module_exe_base() + 0x3C) + 0x50);
    unicode_string(entry + 0x24, "C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin\\Sims2EP9.exe");
    unicode_string(entry + 0x2C, "Sims2EP9.exe");
    MEM16(entry + 0x38) = 0xFFFF;                  /* LoadCount */
    MEM32(peb_va + 0x0C) = ldr;
    /* ProcessParameters: the image path and the command line. */
    uint32_t params = void_galloc(0x290);
    memset(GPTR(params), 0, 0x290);
    MEM32(params) = 0x290;                         /* MaximumLength */
    MEM32(params + 4) = 0x290;                     /* Length */
    MEM32(params + 8) = 1;                         /* Flags: normalized */
    extern const char *void_command_line;
    unicode_string(params + 0x24, "C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin\\");   /* CurrentDirectory.DosPath */
    unicode_string(params + 0x38, "C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin\\Sims2EP9.exe");
    unicode_string(params + 0x40, void_command_line);
    MEM32(peb_va + 0x10) = params;
    return peb_va;
}

/* Static TLS templates (pe.c): index = position in this list. */
#define STATIC_TLS_MAX 8
static struct { uint32_t start, size, zero_fill; } static_tls[STATIC_TLS_MAX];
static int static_tls_count;

/* TEB+0x2C: the array of this thread's static TLS blocks, one per module. */
static void static_tls_build(uint32_t tib)
{
    uint32_t array = MEM32(tib + 0x2C);
    if (!array || array == tib + 0xE10) {
        array = void_galloc(4 * STATIC_TLS_MAX);
        if (!array) void_fatal("static TLS array allocation failed");
        memset(GPTR(array), 0, 4 * STATIC_TLS_MAX);
        MEM32(tib + 0x2C) = array;
    }
    for (int i = 0; i < static_tls_count; ++i) {
        if (MEM32(array + 4 * (uint32_t)i)) continue;
        uint32_t total = static_tls[i].size + static_tls[i].zero_fill;
        uint32_t block = void_galloc(total ? total : 4);
        if (!block) void_fatal("static TLS block allocation failed");
        memset(GPTR(block), 0, total ? total : 4);
        if (static_tls[i].size) memcpy(GPTR(block), GPTR(static_tls[i].start), static_tls[i].size);
        MEM32(array + 4 * (uint32_t)i) = block;
    }
}

void void_tls_static_add(uint32_t start, uint32_t end, uint32_t zero_fill, uint32_t index_va)
{
    if (static_tls_count >= STATIC_TLS_MAX) void_fatal("too many static TLS modules");
    int index = static_tls_count++;
    static_tls[index].start = start;
    static_tls[index].size = end > start ? end - start : 0;
    static_tls[index].zero_fill = zero_fill;
    MEM32(index_va) = (uint32_t)index;
    pthread_mutex_lock(&threads_lock);
    for (void_thread *t = all_threads; t; t = t->next_thread) static_tls_build(t->tib);
    pthread_mutex_unlock(&threads_lock);
    void_log("[void] static TLS #%d: template %08X+%u, zero fill %u, index at %08X", index, start,
        static_tls[index].size, zero_fill, index_va);
}

static void_thread *thread_new(uint32_t guest_stack)
{
    void_thread *t = (void_thread *)calloc(1, sizeof *t);
    if (!t) void_fatal("thread allocation failed");
    pthread_cond_init(&t->wake, NULL);
    if (!guest_stack) guest_stack = GUEST_STACK_DEFAULT;
    guest_stack = (guest_stack + 0xFFFFu) & ~0xFFFFu;
    uint32_t lo = void_vm_alloc(0, guest_stack, 0x3000u, 0x04u);
    uint32_t tib = void_vm_alloc(0, 0x1000, 0x3000u, 0x04u);
    if (!lo || !tib) void_fatal("guest stack/TIB allocation failed");
    pthread_mutex_lock(&mlock);
    t->id = next_thread_id;
    next_thread_id += 4;
    pthread_mutex_unlock(&mlock);
    t->stack_lo = lo;
    t->stack_hi = lo + guest_stack;
    t->tib = tib;
    MEM32(tib + 0x00) = 0xFFFFFFFFu;        /* ExceptionList: end of chain */
    MEM32(tib + 0x04) = t->stack_hi;        /* StackBase */
    MEM32(tib + 0x08) = t->stack_lo;        /* StackLimit */
    MEM32(tib + 0x18) = tib;                /* Self */
    MEM32(tib + 0x20) = 0x100;              /* ClientId.UniqueProcess */
    MEM32(tib + 0x24) = t->id;              /* ClientId.UniqueThread */
    MEM32(tib + 0x2C) = 0;                  /* ThreadLocalStoragePointer: static_tls_build */
    MEM32(tib + 0x30) = make_peb();         /* ProcessEnvironmentBlock */
    static_tls_build(tib);
    /* The bridge-style headroom native32 kept above the first frame. */
    t->regs.esp = t->stack_hi - 0x100;
    t->regs.fs = tib;
    t->regs.fpu_cw = 0x027F;
    t->regs.fpu_cmp = 1;
    t->regs.mxcsr = 0x1F80;
    pthread_mutex_lock(&threads_lock);
    t->next_thread = all_threads;
    all_threads = t;
    pthread_mutex_unlock(&threads_lock);
    return t;
}

/* TlsAlloc and TlsFree zero the slot in every thread, as Windows does: a
 * module that frees its index and one that allocates it next must not see a
 * stale value (the MSVC CRT keeps a function pointer in its slot). */
static void tls_clear_everywhere(uint32_t index)
{
    pthread_mutex_lock(&threads_lock);
    for (void_thread *t = all_threads; t; t = t->next_thread) {
        t->tls[index] = 0;
        if (index < 64) MEM32(t->tib + 0xE10 + 4 * index) = 0;
    }
    pthread_mutex_unlock(&threads_lock);
}

static void pin_guest_thread(void);

void_thread *void_thread_self(void)
{
    if (!t_self) {
        /* A host thread entering the guest for the first time (the launcher
         * thread, a timer or audio callback) becomes a guest thread. */
        t_self = thread_new(GUEST_STACK_DEFAULT);
        t_self->object = void_object_new(VOID_OBJ_THREAD, 0);
        t_self->object->data = t_self;   /* DuplicateHandle(GetCurrentThread()) names it (QueueUserAPC) */
        t_self->pt = pthread_self();
        pin_guest_thread();
    }
    return t_self;
}

void void_machine_enter(void)
{
    void_thread *t = void_thread_self();
    if (t->depth++ == 0) {
        machine_acquire();
        void_regs_load(&t->regs);
    }
}

void void_machine_leave(void)
{
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("machine leave without a claim");
    if (--t->depth == 0) {
        void_regs_save(&t->regs);
        machine_release();
    }
}

void void_blocking_begin(void)
{
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("blocking call outside the guest machine");
    void_regs_save(&t->regs);
    machine_release();
}

void void_blocking_end(void)
{
    void_thread *t = t_self;
    machine_acquire();
    void_regs_load(&t->regs);
}

/* A lifted loop's periodic offer of the machine. The game's other threads
 * mostly poll (Sleep(1) loops, a thousand wake-ups a second each), and the
 * main thread handed them the machine at every offer: 2-5 ms of a frame went
 * on hand-overs at speed 3. It now offers only after holding the machine for
 * MAIN_SLICE_NS; the others still give way at once. */
#define MAIN_SLICE_NS 2000000ull
static void yield_hook(void)
{
    if (!machine_contended()) return;
    if (t_main_thread && now_ns() - t_held_since < MAIN_SLICE_NS) return;
    void_thread *t = t_self;
    if (!t || t->depth <= 0) void_fatal("yield outside the guest machine");
    void_regs_save(&t->regs);
    /* Reserve our return before releasing, so the next owner sees a contender even if we are preempted. */
    uint64_t ticket = atomic_fetch_add(&ticket_next, 1);
    machine_release();
    machine_wait(ticket);
    void_regs_load(&t->regs);
}

uint32_t void_thread_id(void) { return void_thread_self()->id; }
uint32_t void_get_last_error(void) { return void_thread_self()->last_error; }
void void_set_last_error(uint32_t e)
{
    void_thread *t = void_thread_self();
    t->last_error = e;
    MEM32(t->tib + 0x34) = e;
}

/* ---------------------------------------------------- guest call helpers */
uint32_t void_call_guest(uint32_t va, int nargs, const uint32_t *args, int stdcall_pops)
{
    recomp_func_t fn = void_resolve(va);
    uint32_t entry_esp = g_esp, caller = g_cur_func;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
    for (int i = nargs; i > 0; --i) PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, RECOMP_RETADDR);
    fn();
    uint32_t expected = stdcall_pops ? entry_esp : entry_esp - 4u * (uint32_t)nargs;
    if (g_esp != expected) {
        void_log("[void] guest call %08X: ESP %08X, expected %08X (args %d, %s)", va, g_esp, expected, nargs,
            stdcall_pops ? "stdcall" : "cdecl");
        void_lift_trap(va, "guest callback stack imbalance");
    }
    g_esp = entry_esp;
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
    g_cur_func = caller;
    return g_eax;
}

uint32_t void_call_guest_this(uint32_t va, uint32_t this_ptr, int nargs, const uint32_t *args)
{
    g_ecx = this_ptr;
    return void_call_guest(va, nargs, args, 1);
}

/* --------------------------------------------------------------- handles */
#define MAX_HANDLES 65536
static void_object *handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static uint32_t handle_hint;

void_object *void_object_new(int kind, size_t extra)
{
    void_object *o = (void_object *)calloc(1, sizeof *o + extra);
    if (!o) void_fatal("object allocation failed");
    o->kind = kind;
    o->refs = 1;
    if (extra) o->data = (void *)(o + 1);
    return o;
}

void void_object_release(void_object *o)
{
    if (!o) return;
    pthread_mutex_lock(&handle_lock);
    int last = --o->refs == 0;
    pthread_mutex_unlock(&handle_lock);
    if (last) {
        if (o->destroy) o->destroy(o);
        free(o);
    }
}

uint32_t void_handle_new(void_object *o)
{
    pthread_mutex_lock(&handle_lock);
    for (uint32_t n = 0; n < MAX_HANDLES; ++n) {
        uint32_t i = (handle_hint + n) % MAX_HANDLES;
        if (!i) continue;
        if (!handles[i]) {
            handles[i] = o;
            handle_hint = i + 1;
            pthread_mutex_unlock(&handle_lock);
            return 0x1000u + 4u * i;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    void_fatal("handle table full");
}

static uint32_t handle_index(uint32_t h)
{
    if (h < 0x1000u || (h & 3u)) return 0;
    uint32_t i = (h - 0x1000u) / 4u;
    return i < MAX_HANDLES ? i : 0;
}

static void_object *handle_lookup(uint32_t h, int kind, int retain)
{
    void_object *thread = NULL;
    if (h == 0xFFFFFFFEu) {           /* GetCurrentThread pseudo-handle */
        if (kind != VOID_OBJ_NONE && kind != VOID_OBJ_THREAD) return NULL;
        void_thread *self = void_thread_self();
        if (self) thread = self->object;
    }
    uint32_t i = handle_index(h);
    pthread_mutex_lock(&handle_lock);
    void_object *o = i ? handles[i] : thread;
    if (o && kind != VOID_OBJ_NONE && o->kind != kind) o = NULL;
    if (o && retain) ++o->refs;
    pthread_mutex_unlock(&handle_lock);
    return o;
}

void_object *void_handle_get(uint32_t h, int kind) { return handle_lookup(h, kind, 0); }
void_object *void_handle_acquire(uint32_t h, int kind) { return handle_lookup(h, kind, 1); }

int void_handle_close(uint32_t h)
{
    uint32_t i = handle_index(h);
    pthread_mutex_lock(&handle_lock);
    void_object *o = i ? handles[i] : NULL;
    if (o) handles[i] = NULL;
    pthread_mutex_unlock(&handle_lock);
    if (!o) return 0;
    void_object_release(o);
    return 1;
}

uint32_t void_handle_dup(uint32_t h)
{
    void_object *o = void_handle_acquire(h, VOID_OBJ_NONE);
    if (!o) return 0;
    return void_handle_new(o);
}

/* ---------------------------------------------------------------- waiting */
static int object_ready(void_object *o, uint32_t tid)
{
    switch (o->kind) {
    case VOID_OBJ_EVENT: case VOID_OBJ_THREAD: case VOID_OBJ_PROCESS: case VOID_OBJ_TIMER:
        return o->signaled;
    case VOID_OBJ_SEMAPHORE:
        return o->count > 0;
    case VOID_OBJ_MUTEX:
        return o->owner == 0 || o->owner == tid;
    default:
        return 1;                     /* files and others: always signaled */
    }
}

static void object_consume(void_object *o, uint32_t tid)
{
    switch (o->kind) {
    case VOID_OBJ_EVENT: case VOID_OBJ_TIMER:
        if (!o->manual_reset) o->signaled = 0;
        break;
    case VOID_OBJ_SEMAPHORE:
        --o->count;
        break;
    case VOID_OBJ_MUTEX:
        o->owner = tid;
        ++o->recursion;
        break;
    default:
        break;
    }
}

/* A wait wakes only the threads parked on what changed: each waiting thread
 * sleeps on its own condition, listed on every object it waits for. One
 * condition for all made each SetEvent, ReleaseMutex, sound notification and
 * multimedia-timer tick wake every waiting guest thread to look and sleep
 * again: near a thousand wake-ups a second each, every one preempting the
 * main thread on the core they share. (Under wait_lock.) */
static void wake_waiters(void_object *o)
{
    for (void_waiter *w = o->waiters; w; w = w->next) pthread_cond_signal(&((void_thread *)w->thread)->wake);
}

/* Threads waiting with an extra condition (MsgWaitForMultipleObjects: queued
 * input): void_wait_broadcast wakes them. */
static void_waiter *extra_waiters;

static void unlink_waiter(void_waiter **list, void_waiter *node)
{
    for (void_waiter **p = list; *p; p = &(*p)->next)
        if (*p == node) { *p = node->next; return; }
}

void void_object_signal(void_object *o)
{
    pthread_mutex_lock(&wait_lock);
    o->signaled = 1;
    wake_waiters(o);
    pthread_mutex_unlock(&wait_lock);
}

void void_wait_broadcast(void)
{
    pthread_mutex_lock(&wait_lock);
    for (void_waiter *w = extra_waiters; w; w = w->next) pthread_cond_signal(&((void_thread *)w->thread)->wake);
    pthread_mutex_unlock(&wait_lock);
}

/* Optional extra wake condition (MsgWaitForMultipleObjects: queued input). */
typedef int (*void_wait_extra)(void *);

static uint32_t wait_core(int count, void_object **objects, int wait_all, uint32_t timeout,
    void_wait_extra extra, void *extra_arg)
{
    void_thread *self = void_thread_self();
    uint32_t tid = self->id;
    void_waiter nodes[64], extra_node;
    int registered = 0;
    struct timespec deadline;
    if (timeout != INFINITE_) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000u;
        deadline.tv_nsec += (long)(timeout % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    int blocking = timeout != 0;
    uint64_t wait_start = blocking ? void_stat_now() : 0;
    if (blocking) void_blocking_begin();
    pthread_mutex_lock(&wait_lock);
    uint32_t result = WAIT_TIMEOUT_;
    for (;;) {
        if (wait_all) {
            int all = 1;
            for (int i = 0; i < count; ++i) if (!object_ready(objects[i], tid)) { all = 0; break; }
            if (all) {
                for (int i = 0; i < count; ++i) object_consume(objects[i], tid);
                result = WAIT_OBJECT_0_;
                break;
            }
        } else {
            int hit = -1;
            for (int i = 0; i < count; ++i) if (object_ready(objects[i], tid)) { hit = i; break; }
            if (hit >= 0) {
                object_consume(objects[hit], tid);
                result = WAIT_OBJECT_0_ + (uint32_t)hit;
                break;
            }
        }
        if (extra && extra(extra_arg)) { result = WAIT_OBJECT_0_ + (uint32_t)count; break; }
        if (!timeout) break;
        if (!registered) {
            for (int i = 0; i < count; ++i) {
                nodes[i].thread = self;
                nodes[i].next = objects[i]->waiters;
                objects[i]->waiters = &nodes[i];
            }
            if (extra) { extra_node.thread = self; extra_node.next = extra_waiters; extra_waiters = &extra_node; }
            registered = 1;
        }
        int rc = timeout == INFINITE_ ? pthread_cond_wait(&self->wake, &wait_lock)
                                      : pthread_cond_timedwait(&self->wake, &wait_lock, &deadline);
        if (rc == ETIMEDOUT) {
            result = WAIT_TIMEOUT_;
            /* One last check: a signal may have raced the timeout. */
            timeout = 0;
        }
    }
    if (registered) {
        for (int i = 0; i < count; ++i) unlink_waiter(&objects[i]->waiters, &nodes[i]);
        if (extra) unlink_waiter(&extra_waiters, &extra_node);
    }
    pthread_mutex_unlock(&wait_lock);
    if (blocking) { void_blocking_end(); void_stat_add(VS_WAIT, void_stat_now() - wait_start, 0); }
    return result;
}

uint32_t void_wait(int count, const uint32_t *hs, int wait_all, uint32_t timeout)
{
    void_object *objects[64];
    if (count <= 0 || count > 64) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    for (int i = 0; i < count; ++i) {
        objects[i] = void_handle_get(hs[i], VOID_OBJ_NONE);
        if (!objects[i]) {
            void_log("[void] wait on invalid handle %08X (caller %08X)", hs[i], g_cur_func);
            void_set_last_error(ERROR_INVALID_HANDLE_);
            return WAIT_FAILED_;
        }
    }
    return wait_core(count, objects, wait_all, timeout, NULL, NULL);
}

uint32_t void_wait_extra_cond(int count, const uint32_t *hs, uint32_t timeout, int (*extra)(void *), void *arg)
{
    void_object *objects[64];
    if (count < 0 || count > 63) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    for (int i = 0; i < count; ++i) {
        objects[i] = void_handle_get(hs[i], VOID_OBJ_NONE);
        if (!objects[i]) { void_set_last_error(ERROR_INVALID_HANDLE_); return WAIT_FAILED_; }
    }
    return wait_core(count, objects, 0, timeout, extra, arg);
}

/* ------------------------------------------------------------- kernel32 */
#define K32 "kernel32.dll"

WINAPI_EXPORT(K32, GetLastError, 0) { return void_get_last_error(); }
WINAPI_EXPORT(K32, SetLastError, 1) { void_set_last_error(ARG(0)); return 0; }
WINAPI_EXPORT(K32, GetCurrentThreadId, 0) { return void_thread_id(); }
WINAPI_EXPORT(K32, GetCurrentThread, 0) { return 0xFFFFFFFEu; }
WINAPI_EXPORT(K32, GetCurrentProcess, 0) { return 0xFFFFFFFFu; }
WINAPI_EXPORT(K32, GetCurrentProcessId, 0) { return 0x100u; }

WINAPI_EXPORT(K32, TlsAlloc, 0)
{
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 0; i < TLS_SLOTS; ++i) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&tls_lock);
            tls_clear_everywhere(i);
            return i;
        }
    }
    pthread_mutex_unlock(&tls_lock);
    return 0xFFFFFFFFu;              /* TLS_OUT_OF_INDEXES */
}

WINAPI_EXPORT(K32, TlsFree, 1)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS || !tls_used[i]) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    tls_clear_everywhere(i);
    tls_used[i] = 0;
    return 1;
}

WINAPI_EXPORT(K32, TlsGetValue, 1)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    void_set_last_error(0);
    return void_thread_self()->tls[i];
}

WINAPI_EXPORT(K32, TlsSetValue, 2)
{
    uint32_t i = ARG(0);
    if (i >= TLS_SLOTS) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    void_thread *t = void_thread_self();
    t->tls[i] = ARG(1);
    if (i < 64) MEM32(t->tib + 0xE10 + 4 * i) = ARG(1);
    return 1;
}

/* Critical sections live in guest memory; contention waits on a generation
 * counter so an unlock between the owner check and the wait is never lost.
 * cs_waiters counts threads parked on it: a waiter registers while it still
 * holds the machine and only then lets it go, and a release runs on the
 * machine too, so a release that sees no waiters skips the wake-up (the
 * game's CRT takes a few thousand uncontended sections a frame). */
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cs_cond = PTHREAD_COND_INITIALIZER;
static uint64_t cs_generation;
static uint32_t cs_waiters;

static void cs_init(uint32_t cs, uint32_t spin)
{
    MEM32(cs + 0) = 0xFFFFFFFFu;     /* DebugInfo: none, as Windows 8+ leaves it */
    MEM32(cs + 4) = 0xFFFFFFFFu;     /* LockCount: free */
    MEM32(cs + 8) = 0;               /* RecursionCount */
    MEM32(cs + 12) = 0;              /* OwningThread */
    MEM32(cs + 16) = 0;              /* LockSemaphore */
    MEM32(cs + 20) = spin;           /* SpinCount */
}

WINAPI_EXPORT(K32, InitializeCriticalSection, 1) { cs_init(ARG(0), 0); return 0; }
WINAPI_EXPORT(K32, InitializeCriticalSectionAndSpinCount, 2) { cs_init(ARG(0), ARG(1)); return 1; }
WINAPI_EXPORT(K32, DeleteCriticalSection, 1) { MEM32(ARG(0) + 12) = 0; MEM32(ARG(0) + 8) = 0; return 0; }

static void cs_enter(uint32_t cs)
{
    uint32_t tid = void_thread_id();
    for (;;) {
        uint32_t owner = MEM32(cs + 12);
        if (!owner) {
            MEM32(cs + 12) = tid;
            MEM32(cs + 8) = 1;
            MEM32(cs + 4) = 0;
            return;
        }
        if (owner == tid) {
            MEM32(cs + 8) += 1;
            MEM32(cs + 4) += 1;
            return;
        }
        pthread_mutex_lock(&cs_lock);
        uint64_t seen = cs_generation;
        __atomic_add_fetch(&cs_waiters, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&cs_lock);
        void_blocking_begin();
        pthread_mutex_lock(&cs_lock);
        while (cs_generation == seen) pthread_cond_wait(&cs_cond, &cs_lock);
        __atomic_sub_fetch(&cs_waiters, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&cs_lock);
        void_blocking_end();
    }
}

static void cs_leave(uint32_t cs)
{
    uint32_t tid = void_thread_id();
    if (MEM32(cs + 12) != tid) {
        void_log("[void] LeaveCriticalSection(%08X) by non-owner %u (owner %u, caller %08X)",
            cs, tid, MEM32(cs + 12), g_cur_func);
        return;
    }
    if (--MEM32(cs + 8) == 0) {
        MEM32(cs + 12) = 0;
        MEM32(cs + 4) = 0xFFFFFFFFu;
        if (__atomic_load_n(&cs_waiters, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&cs_lock);
            ++cs_generation;
            pthread_cond_broadcast(&cs_cond);
            pthread_mutex_unlock(&cs_lock);
        }
    } else {
        MEM32(cs + 4) -= 1;
    }
}

WINAPI_EXPORT(K32, EnterCriticalSection, 1) { cs_enter(ARG(0)); return 0; }
WINAPI_EXPORT(K32, LeaveCriticalSection, 1) { cs_leave(ARG(0)); return 0; }
WINAPI_EXPORT(K32, TryEnterCriticalSection, 1)
{
    uint32_t cs = ARG(0), tid = void_thread_id(), owner = MEM32(cs + 12);
    if (owner && owner != tid) return 0;
    cs_enter(cs);
    return 1;
}

/* Slim reader/writer locks and condition variables (the static UCRT uses
 * them): the lock word is 0 free, 1 held exclusively, 2*n held shared by n;
 * a condition variable's own word is a wake generation. State changes run on
 * the machine; a waiter parks on one host condition and re-checks after every
 * release or wake anywhere, exactly as the critical sections above do. */
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static uint64_t sync_generation;
static uint32_t sync_waiters;

static void sync_wake(void)
{
    if (!__atomic_load_n(&sync_waiters, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&sync_lock);
    ++sync_generation;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}

/* Park until anything is released or woken; 0 when `deadline` (ns) passed. */
static int sync_park(uint64_t deadline)
{
    pthread_mutex_lock(&sync_lock);
    uint64_t seen = sync_generation;
    __atomic_add_fetch(&sync_waiters, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&sync_lock);
    void_blocking_begin();
    pthread_mutex_lock(&sync_lock);
    int ok = 1;
    while (sync_generation == seen && ok) {
        if (!deadline) { pthread_cond_wait(&sync_cond, &sync_lock); continue; }
        uint64_t now = now_ns();
        if (now >= deadline) { ok = 0; break; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t abs = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (deadline - now);
        ts.tv_sec = (time_t)(abs / 1000000000ull);
        ts.tv_nsec = (long)(abs % 1000000000ull);
        pthread_cond_timedwait(&sync_cond, &sync_lock, &ts);
    }
    __atomic_sub_fetch(&sync_waiters, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&sync_lock);
    void_blocking_end();
    return ok;
}

static void srw_acquire(uint32_t lock, int shared)
{
    for (;;) {
        uint32_t v = MEM32(lock);
        if (shared ? !(v & 1u) : v == 0) { MEM32(lock) = shared ? v + 2u : 1u; return; }
        sync_park(0);
    }
}

static void srw_release(uint32_t lock, int shared)
{
    uint32_t v = MEM32(lock);
    MEM32(lock) = shared ? (v >= 2u ? v - 2u : 0u) : 0u;
    sync_wake();
}

WINAPI_EXPORT(K32, InitializeSRWLock, 1) { MEM32(ARG(0)) = 0; return 0; }
WINAPI_EXPORT(K32, AcquireSRWLockExclusive, 1) { srw_acquire(ARG(0), 0); return 0; }
WINAPI_EXPORT(K32, AcquireSRWLockShared, 1) { srw_acquire(ARG(0), 1); return 0; }
WINAPI_EXPORT(K32, ReleaseSRWLockExclusive, 1) { srw_release(ARG(0), 0); return 0; }
WINAPI_EXPORT(K32, ReleaseSRWLockShared, 1) { srw_release(ARG(0), 1); return 0; }
WINAPI_EXPORT(K32, TryAcquireSRWLockExclusive, 1)
{
    if (MEM32(ARG(0))) return 0;
    MEM32(ARG(0)) = 1;
    return 1;
}
WINAPI_EXPORT(K32, TryAcquireSRWLockShared, 1)
{
    if (MEM32(ARG(0)) & 1u) return 0;
    MEM32(ARG(0)) += 2;
    return 1;
}
WINAPI_EXPORT(K32, InitializeConditionVariable, 1) { MEM32(ARG(0)) = 0; return 0; }
WINAPI_EXPORT(K32, WakeConditionVariable, 1) { MEM32(ARG(0)) += 1; sync_wake(); return 0; }
WINAPI_EXPORT(K32, WakeAllConditionVariable, 1) { MEM32(ARG(0)) += 1; sync_wake(); return 0; }

/* Release `lock`, wait for a wake of `cv` (or the timeout), reacquire. */
static uint32_t cv_sleep(uint32_t cv, uint32_t timeout, void (*release)(uint32_t, int), void (*acquire)(uint32_t, int),
                         uint32_t lock, int shared)
{
    uint32_t seen = MEM32(cv);
    uint64_t deadline = timeout == INFINITE_ ? 0 : now_ns() + (uint64_t)timeout * 1000000ull + 1;
    release(lock, shared);
    int ok = 1;
    while (MEM32(cv) == seen && ok) ok = sync_park(deadline);
    acquire(lock, shared);
    if (!ok && MEM32(cv) == seen) { void_set_last_error(1460u); return 0; }   /* ERROR_TIMEOUT */
    return 1;
}

static void cs_acquire_mode(uint32_t cs, int shared) { (void)shared; cs_enter(cs); }
static void cs_release_mode(uint32_t cs, int shared)
{
    (void)shared;
    uint32_t tid = void_thread_id();
    if (MEM32(cs + 12) == tid && MEM32(cs + 8) == 1) {
        MEM32(cs + 8) = 0;
        MEM32(cs + 12) = 0;
        MEM32(cs + 4) = 0xFFFFFFFFu;
        pthread_mutex_lock(&cs_lock);
        ++cs_generation;
        pthread_cond_broadcast(&cs_cond);
        pthread_mutex_unlock(&cs_lock);
        sync_wake();
    } else {
        cs_leave(cs);
    }
}

WINAPI_EXPORT(K32, SleepConditionVariableSRW, 4)
{
    return cv_sleep(ARG(0), ARG(2), srw_release, srw_acquire, ARG(1), (ARG(3) & 1u) != 0);
}
WINAPI_EXPORT(K32, SleepConditionVariableCS, 3)
{
    return cv_sleep(ARG(0), ARG(2), cs_release_mode, cs_acquire_mode, ARG(1), 0);
}
WINAPI_EXPORT(K32, InitializeCriticalSectionEx, 3) { cs_init(ARG(0), ARG(1)); return 1; }

/* Interlocked operations run under the machine lock: plain arithmetic is
 * already atomic with respect to every other guest thread. */
WINAPI_EXPORT(K32, InterlockedIncrement, 1) { return ++MEM32(ARG(0)); }
WINAPI_EXPORT(K32, InterlockedDecrement, 1) { return --MEM32(ARG(0)); }
WINAPI_EXPORT(K32, InterlockedExchange, 2) { uint32_t old = MEM32(ARG(0)); MEM32(ARG(0)) = ARG(1); return old; }
WINAPI_EXPORT(K32, InterlockedExchangeAdd, 2) { uint32_t old = MEM32(ARG(0)); MEM32(ARG(0)) = old + ARG(1); return old; }
WINAPI_EXPORT(K32, InterlockedCompareExchange, 3)
{
    uint32_t old = MEM32(ARG(0));
    if (old == ARG(2)) MEM32(ARG(0)) = ARG(1);
    return old;
}

/* Events, mutexes, semaphores. Names are accepted and ignored except for
 * the single-instance mutex the launcher creates, which is process-local. */
static uint32_t create_event(int manual, int initial)
{
    void_object *o = void_object_new(VOID_OBJ_EVENT, 0);
    o->manual_reset = manual;
    o->signaled = initial;
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, CreateEventA, 4) { return create_event(ARG(1) != 0, ARG(2) != 0); }
WINAPI_EXPORT(K32, CreateEventW, 4) { return create_event(ARG(1) != 0, ARG(2) != 0); }

static uint32_t event_op(uint32_t h, int op)
{
    void_object *o = void_handle_get(h, VOID_OBJ_EVENT);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_mutex_lock(&wait_lock);
    if (op == 1) { o->signaled = 1; wake_waiters(o); }
    else if (op == 0) o->signaled = 0;
    else { o->signaled = 1; wake_waiters(o); }
    pthread_mutex_unlock(&wait_lock);
    if (op == 2) {
        /* PulseEvent: release current waiters, then reset. */
        void_blocking_begin();
        sched_yield();
        void_blocking_end();
        pthread_mutex_lock(&wait_lock);
        o->signaled = 0;
        pthread_mutex_unlock(&wait_lock);
    }
    return 1;
}

WINAPI_EXPORT(K32, SetEvent, 1) { return event_op(ARG(0), 1); }
WINAPI_EXPORT(K32, ResetEvent, 1) { return event_op(ARG(0), 0); }
WINAPI_EXPORT(K32, PulseEvent, 1) { return event_op(ARG(0), 2); }

static uint32_t create_mutex(int initial_owner)
{
    void_object *o = void_object_new(VOID_OBJ_MUTEX, 0);
    if (initial_owner) { o->owner = void_thread_id(); o->recursion = 1; }
    void_set_last_error(0);
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, CreateMutexA, 3) { return create_mutex(ARG(1) != 0); }
WINAPI_EXPORT(K32, CreateMutexW, 3) { return create_mutex(ARG(1) != 0); }
WINAPI_EXPORT(K32, ReleaseMutex, 1)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_MUTEX);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_mutex_lock(&wait_lock);
    if (o->owner != void_thread_id()) { pthread_mutex_unlock(&wait_lock); void_set_last_error(ERROR_NOT_OWNER_); return 0; }
    if (--o->recursion == 0) { o->owner = 0; wake_waiters(o); }
    pthread_mutex_unlock(&wait_lock);
    return 1;
}

static uint32_t create_semaphore(int initial, int maximum)
{
    void_object *o = void_object_new(VOID_OBJ_SEMAPHORE, 0);
    o->count = initial;
    o->max_count = maximum;
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, CreateSemaphoreA, 4) { return create_semaphore((int)ARG(1), (int)ARG(2)); }
WINAPI_EXPORT(K32, CreateSemaphoreW, 4) { return create_semaphore((int)ARG(1), (int)ARG(2)); }
WINAPI_EXPORT(K32, ReleaseSemaphore, 3)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_SEMAPHORE);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_mutex_lock(&wait_lock);
    int previous = o->count;
    if ((int)ARG(1) <= 0 || previous + (int)ARG(1) > o->max_count) {
        pthread_mutex_unlock(&wait_lock);
        void_set_last_error(0x12Au);  /* ERROR_TOO_MANY_POSTS */
        return 0;
    }
    o->count += (int)ARG(1);
    wake_waiters(o);
    pthread_mutex_unlock(&wait_lock);
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)previous;
    return 1;
}

WINAPI_EXPORT(K32, WaitForSingleObject, 2)
{
    uint32_t h = ARG(0);
    return void_wait(1, &h, 0, ARG(1));
}

WINAPI_EXPORT(K32, WaitForSingleObjectEx, 3)
{
    uint32_t h = ARG(0);
    return void_wait(1, &h, 0, ARG(1));
}

WINAPI_EXPORT(K32, WaitForMultipleObjects, 4)
{
    uint32_t count = ARG(0), list[64];
    if (!count || count > 64) { void_set_last_error(ERROR_INVALID_PARAMETER_); return WAIT_FAILED_; }
    for (uint32_t i = 0; i < count; ++i) list[i] = MEM32(ARG(1) + 4 * i);
    return void_wait((int)count, list, ARG(2) != 0, ARG(3));
}

static void sleep_ms(uint32_t ms)
{
    if (!ms) {
        if (machine_contended()) { void_blocking_begin(); sched_yield(); void_blocking_end(); }
        else sched_yield();
        return;
    }
    VSTAT_BEGIN(sleep_start);
    void_blocking_begin();
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    void_blocking_end();
    VSTAT_END(sleep_start, VS_SLEEP, 0);
}

WINAPI_EXPORT(K32, Sleep, 1) { sleep_ms(ARG(0)); return 0; }

/* User APCs. The queue and the wake-up go through wait_lock/wait_cond, the
 * condition every guest wait already sleeps on. An alertable SleepEx runs
 * the queued APCs (stdcall, one argument) on its own thread and returns
 * WAIT_IO_COMPLETION; the game wakes its worker threads this way, with an
 * empty APC. */
static void_thread *thread_of_handle(uint32_t h)
{
    if (h == 0xFFFFFFFEu) return void_thread_self();
    void_object *o = void_handle_get(h, VOID_OBJ_THREAD);
    return o ? (void_thread *)o->data : NULL;
}

WINAPI_EXPORT(K32, QueueUserAPC, 3)
{
    void_thread *t = thread_of_handle(ARG(1));
    if (!t) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    pthread_mutex_lock(&wait_lock);
    int ok = t->apc_count < 64;
    if (ok) {
        t->apc[t->apc_count].fn = ARG(0);
        t->apc[t->apc_count].param = ARG(2);
        ++t->apc_count;
        pthread_cond_signal(&t->wake);       /* its alertable SleepEx sleeps there */
    }
    pthread_mutex_unlock(&wait_lock);
    if (!ok) void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
    return (uint32_t)ok;
}

/* Run (and drain) the calling thread's APCs; returns how many ran. */
static int run_apcs(void_thread *t)
{
    int ran = 0;
    for (;;) {
        pthread_mutex_lock(&wait_lock);
        if (!t->apc_count) { pthread_mutex_unlock(&wait_lock); return ran; }
        uint32_t fn = t->apc[0].fn, param = t->apc[0].param;
        memmove(&t->apc[0], &t->apc[1], sizeof t->apc[0] * (size_t)(t->apc_count - 1));
        --t->apc_count;
        pthread_mutex_unlock(&wait_lock);
        uint32_t args[1] = { param };
        void_call_guest(fn, 1, args, 1);
        ++ran;
    }
}

WINAPI_EXPORT(K32, SleepEx, 2)
{
    uint32_t ms = ARG(0);
    if (!ARG(1)) { sleep_ms(ms); return 0; }
    void_thread *t = void_thread_self();
    if (run_apcs(t)) return 0xC0u;           /* WAIT_IO_COMPLETION */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (ms != INFINITE_) {
        uint64_t ns = (uint64_t)deadline.tv_nsec + (uint64_t)(ms % 1000u) * 1000000ull;
        deadline.tv_sec += (time_t)(ms / 1000u) + (time_t)(ns / 1000000000ull);
        deadline.tv_nsec = (long)(ns % 1000000000ull);
    }
    void_blocking_begin();
    pthread_mutex_lock(&wait_lock);
    int timed_out = 0;
    while (!t->apc_count && !timed_out) {
        if (ms == INFINITE_) pthread_cond_wait(&t->wake, &wait_lock);
        else timed_out = pthread_cond_timedwait(&t->wake, &wait_lock, &deadline) == ETIMEDOUT;
    }
    pthread_mutex_unlock(&wait_lock);
    void_blocking_end();
    return run_apcs(t) ? 0xC0u : 0;
}
WINAPI_EXPORT(K32, SwitchToThread, 0) { sleep_ms(0); return 1; }

/* ---------------------------------------------------------------- threads */
static void thread_object_destroy(void_object *o) { (void)o; }

/* Every guest thread waits while another holds the machine, so a holder on a
 * little core stretches everyone's time: on the Helio G90T the Sound.dll
 * stream thread decoded Vorbis on an A55 (cpu_capacity 435 against the A76's
 * 1024) while the main thread waited for the machine. Guest threads run on
 * the biggest cores (cpu_capacity, else the highest maximum frequency); host
 * threads such as the mixer and the GL driver's keep the default mask.
 * VOID_BIG_CORES=0 turns this off. */
static cpu_set_t big_cores, next_cores;
static int big_cores_count, next_cores_count;
static atomic_int main_pinned;
static atomic_int main_guest_tid;
static pthread_once_t big_cores_once = PTHREAD_ONCE_INIT;

static long cpu_value(int cpu, const char *name)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long value = -1;
    if (fscanf(f, "%ld", &value) != 1) value = -1;
    fclose(f);
    return value;
}

static void big_cores_init(void)
{
    const char *env = getenv("VOID_BIG_CORES");
    if (env && *env == '0') return;
    long value[CPU_SETSIZE];
    int cpus = 0, use_capacity = cpu_value(0, "cpu_capacity") > 0;
    for (; cpus < CPU_SETSIZE; ++cpus) {
        value[cpus] = use_capacity ? cpu_value(cpus, "cpu_capacity") : cpu_value(cpus, "cpufreq/cpuinfo_max_freq");
        if (value[cpus] <= 0) break;
    }
    long best = 0, worst = 0;
    for (int i = 0; i < cpus; ++i) {
        if (value[i] > best) best = value[i];
        if (!worst || value[i] < worst) worst = value[i];
    }
    if (cpus < 2 || best == worst) return;       /* homogeneous or unknown: leave the scheduler alone */
    CPU_ZERO(&big_cores);
    for (int i = 0; i < cpus; ++i) if (value[i] == best) { CPU_SET(i, &big_cores); ++big_cores_count; }
    /* A next tier at least 60% as capable is preferred for host helpers. */
    CPU_ZERO(&next_cores);
    for (int i = 0; i < cpus; ++i)
        if (value[i] != best && value[i] * 10 >= best * 6) { CPU_SET(i, &next_cores); ++next_cores_count; }
    char list[64] = "";
    for (int i = 0, n = 0; i < cpus && n < 56; ++i)
        if (CPU_ISSET(i, &big_cores)) n += snprintf(list + n, sizeof list - (size_t)n, "%s%d", n ? "," : "", i);
    void_log("[void] the main guest thread runs on cpus %s (%s %ld; others %ld), %d next fastest take the rest", list,
        use_capacity ? "capacity" : "max frequency", best, worst, next_cores_count);
}

/* All guest threads share the biggest cores (the first is the game's main
 * thread, see yield_hook). Running the others on the next fastest cores gave
 * nothing back: the machine lets one run at a time, and the main thread's
 * core, idle while it waited for them, fell below the governor's 95% target
 * load (uag) and its clock with it, to 1.2 GHz from 1.7. */
int void_is_main_guest_thread(void) { return t_main_thread; }

static void pin_guest_thread(void)
{
    t_main_thread = atomic_exchange(&main_pinned, 1) == 0;
    if (t_main_thread) { atomic_store(&main_guest_tid, (int)gettid()); void_stat_main_thread(); }
    pthread_once(&big_cores_once, big_cores_init);
    if (big_cores_count) sched_setaffinity(0, sizeof big_cores, &big_cores);
}

/* Development A/B: only the main guest thread recorded by this process. */
int void_guest_affinity(int mode)
{
    if (mode < 0 || mode > 2) return 0;
    int tid = atomic_load(&main_guest_tid);
    if (!tid) return 0;
    pthread_once(&big_cores_once, big_cores_init);
    if (!big_cores_count || (mode == 1 && !next_cores_count)) return 0;
    cpu_set_t mask = mode == 1 ? next_cores : big_cores;
    if (mode == 2)
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &next_cores)) CPU_SET(cpu, &mask);
    static const char *const names[] = { "big", "next", "union" };
    if (sched_setaffinity(tid, sizeof mask, &mask)) {
        void_log("[void] main guest affinity %s failed: %s", names[mode], strerror(errno));
        return 0;
    }
    void_log("[void] main guest affinity %s: TID %d, %d CPUs", names[mode], tid, CPU_COUNT(&mask));
    return 1;
}

/* Prefer the next tier for host helpers. With spare biggest cores, share
 * those rather than force rendering onto little cores; keep a sole biggest
 * core for the guest. Returns 0 to leave scheduling alone. */
int void_helper_cores(void *cpu_set_out)
{
    cpu_set_t *out = (cpu_set_t *)cpu_set_out;
    pthread_once(&big_cores_once, big_cores_init);
    if (!big_cores_count) return 0;
    if (next_cores_count) { *out = next_cores; return 1; }
    if (big_cores_count >= 2) { *out = big_cores; return 1; }
    CPU_ZERO(out);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    int count = 0;
    for (int cpu = 0; cpu < n && cpu < CPU_SETSIZE; ++cpu)
        if (!CPU_ISSET(cpu, &big_cores)) { CPU_SET(cpu, out); ++count; }
    return count > 0;
}

static void *thread_main(void *arg)
{
    void_thread *t = (void_thread *)arg;
    t_self = t;
    pin_guest_thread();
    if (t->suspended) {
        pthread_mutex_lock(&wait_lock);
        while (t->suspended) pthread_cond_wait(&wait_cond, &wait_lock);
        pthread_mutex_unlock(&wait_lock);
    }
    void_machine_enter();
    if (!setjmp(t->exit_jb)) {
        t->exit_armed = 1;
        uint32_t args[1] = { t->param };
        g_esp = t->stack_hi - 0x100;
        g_fs_base = t->tib;
        t->exit_code = void_call_guest(t->start, 1, args, 1);
    }
    t->exit_armed = 0;
    VLOG("[void] guest thread %u exits with %u", t->id, t->exit_code);
    void_machine_leave();
    pthread_mutex_lock(&wait_lock);
    t->object->signaled = 1;
    t->object->count = (int)t->exit_code;
    wake_waiters(t->object);
    pthread_mutex_unlock(&wait_lock);
    void_object_release(t->object);
    return NULL;
}

WINAPI_EXPORT(K32, CreateThread, 6)
{
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tid_out = ARG(5);
    void_thread *t = thread_new((flags & 0x10000u) ? stack : (stack > GUEST_STACK_DEFAULT ? stack : 0));
    t->start = start;
    t->param = param;
    t->guest_owned = 1;
    t->suspended = (flags & 4u) != 0;     /* CREATE_SUSPENDED */
    t->object = void_object_new(VOID_OBJ_THREAD, 0);
    t->object->data = t;
    t->object->destroy = thread_object_destroy;
    t->object->refs = 2;                  /* the handle and the running thread */
    uint32_t handle = void_handle_new(t->object);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &attr, thread_main, t)) {
        pthread_attr_destroy(&attr);
        void_handle_close(handle);
        void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
        return 0;
    }
    pthread_attr_destroy(&attr);
    if (tid_out) MEM32(tid_out) = t->id;
    VLOG("[void] CreateThread start=%08X param=%08X id=%u handle=%08X%s", start, param, t->id, handle,
        t->suspended ? " suspended" : "");
    return handle;
}

WINAPI_EXPORT(K32, ResumeThread, 1)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    if (!o || !o->data) return 0xFFFFFFFFu;
    void_thread *t = (void_thread *)o->data;
    pthread_mutex_lock(&wait_lock);
    uint32_t previous = (uint32_t)t->suspended;
    t->suspended = 0;
    pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_lock);
    return previous;
}

RAW_EXPORT(K32, FreeLibraryAndExitThread)
{
    void_module_free(ARG(0));
    void_thread *t = void_thread_self();
    t->exit_code = ARG(1);
    if (!t->exit_armed) void_fatal("FreeLibraryAndExitThread on a thread without a guest root frame");
    void_eh_reset_thread();
    longjmp(t->exit_jb, 1);
}

RAW_EXPORT(K32, ExitThread)
{
    void_thread *t = void_thread_self();
    t->exit_code = ARG(0);
    if (!t->exit_armed) void_fatal("ExitThread on a thread without a guest root frame");
    void_eh_reset_thread();
    longjmp(t->exit_jb, 1);
}

WINAPI_EXPORT(K32, GetExitCodeThread, 2)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_THREAD);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    MEM32(ARG(1)) = o->signaled ? (uint32_t)o->count : 259u;   /* STILL_ACTIVE */
    return 1;
}

WINAPI_EXPORT(K32, TerminateThread, 2)
{
    void_log("[void] TerminateThread(%08X, %u) requested by %08X: unsupported, ignored", ARG(0), ARG(1), g_cur_func);
    return 0;
}

WINAPI_EXPORT(K32, GetThreadPriority, 1) { return 0; }
WINAPI_EXPORT(K32, SetThreadPriority, 2) { return 1; }
WINAPI_EXPORT(K32, SetThreadAffinityMask, 2) { return 1; }
WINAPI_EXPORT(K32, SetThreadIdealProcessor, 2) { return 0; }

WINAPI_EXPORT(K32, DuplicateHandle, 7)
{
    uint32_t source = ARG(1), target_out = ARG(3), options = ARG(6);
    uint32_t h;
    if (source == 0xFFFFFFFEu) {
        void_object *o = void_thread_self()->object;
        pthread_mutex_lock(&handle_lock);
        ++o->refs;
        pthread_mutex_unlock(&handle_lock);
        h = void_handle_new(o);
    } else if (source == 0xFFFFFFFFu) {
        void_object *o = void_object_new(VOID_OBJ_PROCESS, 0);
        h = void_handle_new(o);
    } else {
        h = void_handle_dup(source);
    }
    if (!h) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (options & 1u) void_handle_close(source);   /* DUPLICATE_CLOSE_SOURCE */
    if (target_out) MEM32(target_out) = h;
    return 1;
}

WINAPI_EXPORT(K32, CloseHandle, 1)
{
    uint32_t h = ARG(0);
    if (h == 0xFFFFFFFEu || h == 0xFFFFFFFFu) return 1;
    if (!void_handle_close(h)) {
        void_set_last_error(ERROR_INVALID_HANDLE_);
        return 0;
    }
    return 1;
}

void void_threads_init(void)
{
    recomp_yield_hook = yield_hook;
}

/* The thread that runs the game's entry point. */
void void_thread_bind_main(void)
{
    void_thread *t = void_thread_self();
    (void)t;
}

void void_thread_exit_current_for_process(void) {}
