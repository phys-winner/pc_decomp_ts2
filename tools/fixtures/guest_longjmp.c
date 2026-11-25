#include "host.h"
#include "guest_jmp.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define esp g_esp

recomp_machine_t g_m;
ptrdiff_t g_mem_base;
recomp_ehframe_t *g_eh_frames;
uint32_t g_eh_obj, g_eh_info;
void (*recomp_yield_hook)(void);
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;
static unsigned cases;
static __thread unsigned mode, yields, internal_jump;
static __thread uint32_t last_env;
static pthread_mutex_t machine = PTHREAD_MUTEX_INITIALIZER;

void void_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void void_fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); exit(73);
}
void void_lift_trap(uint32_t va, const char *why) { void_fatal("trap %08X: %s", va, why); }
int void_vm_writable(uint32_t va, uint32_t bytes) { return va >= 0x10000 && (uint64_t)va + bytes <= 0x810000; }
#define CHECK(x) do { if (!(x)) void_fatal("check failed at %d: %s", __LINE__, #x); } while (0)

void sub_A0C7F740(void);
void sub_A0C7F8B0(void);
void sub_A0C49D03(void);
void sub_A0C49DC0(void);
/* The test has no guest SEH frame: these are the CRT's validation/notification boundaries. */
void sub_A08299EC(void) { CHECK(void_vm_writable(MEM32(g_esp + 4), 0x40)); g_esp += 4; }
void sub_A0C49E88(void) { g_eax = 1; g_esp += 8; }
void sub_A0829B90(void) { g_esp += 8; }
void sub_A0829AE0(void) { void_fatal("unexpected SEH local unwind"); }
void sub_A0859F9A(void) { void_fatal("unexpected SEH chain change"); }
void sub_A082F6BF(void) { void_fatal("Lua panic reached after a protected error"); }

static unsigned protect(uint32_t state, uint32_t fn)
{
    uint32_t _flag_k = g_flag_k, _flag_a = g_flag_a, _flag_b = g_flag_b, _cf = g_flag_cf;
    uint32_t sp = g_esp, bp = g_ebp, bx = g_ebx, si = g_esi, di = g_edi;
    PUSH32(g_esp, 0); PUSH32(g_esp, fn); PUSH32(g_esp, state);
    RECOMP_CALL(sub_A0C7F740);
    g_esp += 12;
    CHECK(g_esp == sp && g_ebp == bp && g_ebx == bx && g_esi == si && g_edi == di);
    return g_eax;
}

static void throw_error(uint32_t state, uint32_t value)
{
    uint32_t _flag_k = g_flag_k, _flag_a = g_flag_a, _flag_b = g_flag_b, _cf = g_flag_cf;
    g_ebx = 0xBAD00001; g_esi = 0xBAD00002; g_edi = 0xBAD00003;
    g_eh_frames = (recomp_ehframe_t *)(uintptr_t)0xBAD00004;
    g_eh_obj = 0xBAD00005; g_eh_info = 0xBAD00006;
    if (internal_jump) {
        uint32_t handler = MEM32(state + 0x58);
        MEM32(handler + 0x44) = value;
        PUSH32(g_esp, 0); PUSH32(g_esp, handler + 4);
        RECOMP_CALL(sub_A0C49DC0);
    } else {
        PUSH32(g_esp, value); PUSH32(g_esp, state);
        RECOMP_CALL(sub_A0C7F8B0);
    }
    void_fatal("Lua error returned to its throwing callback");
}

static void inner(void) { throw_error(MEM32(g_esp + 4), 2); }
static void callback(void)
{
    uint32_t state = MEM32(g_esp + 4);
    last_env = MEM32(state + 0x58) + 4;
    if (yields) {
        recomp_machine_t saved = g_m;
        recomp_ehframe_t *eh = g_eh_frames;
        uint32_t obj = g_eh_obj, info = g_eh_info;
        pthread_mutex_unlock(&machine);
        sched_yield();
        pthread_mutex_lock(&machine);
        g_m = saved; g_eh_frames = eh; g_eh_obj = obj; g_eh_info = info;
    }
    if (mode >= 2) {
        CHECK(protect(state, 0x7FF10020) == 2);
        CHECK(MEM32(state + 0x58) != 0);
    }
    if (mode == 1 || mode == 2) throw_error(state, mode == 1 ? 1 : 3);
    g_esp += 4;
}

recomp_func_t void_resolve(uint32_t va)
{
    if (va == 0x7FF10010) return callback;
    if (va == 0x7FF10020) return inner;
    void_fatal("unexpected callback %08X", va);
}

static void run_case(unsigned id, unsigned kind)
{
    uint32_t state = 0x12000 + id * 0x100, fs = 0x11000 + id * 0x100;
    g_fs_base = fs; MEM32(fs) = UINT32_MAX; MEM32(state + 0x58) = 0;
    g_esp = 0x100000 + id * 0x10000;
    g_ebp = 0x11111111; g_ebx = 0x22222222; g_esi = 0x33333333; g_edi = 0x44444444;
    g_eh_frames = NULL; g_eh_obj = 0x55555555; g_eh_info = 0x66666666;
    mode = kind & 3; internal_jump = kind & 4;
    unsigned value = protect(state, 0x7FF10010);
    CHECK(value == (mode == 1 ? 1 : mode == 2 ? 3 : 0));
    CHECK(MEM32(state + 0x58) == 0 && MEM32(fs) == UINT32_MAX);
    CHECK(!g_eh_frames && g_eh_obj == 0x55555555 && g_eh_info == 0x66666666);
    ++cases;
}

static void *worker(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    yields = 1;
    for (unsigned i = 0; i < 500; ++i) {
        pthread_mutex_lock(&machine);
        run_case(id, i % 8);
        pthread_mutex_unlock(&machine);
    }
    void_guest_jmp_reset();
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argv;
    void *base = (void *)((uintptr_t)VOID_GUEST_BASE + 0x10000);
    unsigned char *memory = mmap(base, 0x800000, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(memory == base); g_mem_base = (ptrdiff_t)VOID_GUEST_BASE;
    for (unsigned i = 0; i < 4000; ++i) run_case(i % 8, i % 8);
    if (argc > 1) {
        /* A completed setjmp owner must not leave a usable host landing pad behind. */
        g_edx = last_env;
        g_esp = MEM32(last_env + 0x10) + 4; g_ebp = MEM32(last_env);
        g_ebx = MEM32(last_env + 4); g_edi = MEM32(last_env + 8); g_esi = MEM32(last_env + 12);
        void_guest_jmp_try_resume();
        return 1;
    }
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 8)));
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_join(threads[i], NULL));
    CHECK(cases == 6000);
    munmap(memory, 0x800000);
    puts("PASS 6000 original Lua/CRT cases: normal, throw, nested throw then outer throw, nested throw then success; four guest threads with machine handoffs; nonvolatile registers, stack, Lua handler and EH state");
    return 0;
}
