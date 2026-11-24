/* Android's performance hints (ADPF): each frame the game's thread reports
 * how long it worked on it against the frame's budget, for itself and the
 * render thread, so the governor raises their clocks when frames run long
 * instead of waiting for its own load averages (the prime core idled along
 * near 1.7 GHz of 3.0 with the game thread busy 93%). Looked up at run time
 * (API 33): nothing happens where the service or the library is missing. */
#define _GNU_SOURCE
#include "host.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <unistd.h>

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

static APerformanceHintSession *session;
static int (*update_target)(APerformanceHintSession *, int64_t);
static int (*report_actual)(APerformanceHintSession *, int64_t);
static int64_t target_ns;
static int tried;

void void_perf_hint_start(int32_t game_tid, int32_t render_tid, int64_t target)
{
    if (tried++) return;
    const char *option = getenv("PORT_OPTION_ADPF");
    if (!option || (strcmp(option, "1") && strcmp(option, "true"))) {
        void_log("[hint] performance hints off (launcher option)"); return;
    }
    const char *env = getenv("S2_PERF_HINT");
    if (env && *env == '0') return;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    APerformanceHintManager *(*get_manager)(void) = (APerformanceHintManager *(*)(void))dlsym(lib, "APerformanceHint_getManager");
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t) =
        (APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))dlsym(lib,
            "APerformanceHint_createSession");
    update_target = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    report_actual = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    if (!get_manager || !create || !update_target || !report_actual) { void_log("[hint] no performance hints here"); return; }
    APerformanceHintManager *manager = get_manager();
    if (!manager) { void_log("[hint] no performance hint manager"); return; }
    int32_t tids[2] = { game_tid, render_tid };
    session = create(manager, tids, render_tid > 0 ? 2 : 1, target);
    target_ns = target;
    void_log("[hint] performance hint session %s (threads %d, %d; target %.2f ms)", session ? "open" : "refused", game_tid,
        render_tid, (double)target / 1e6);
}

/* The frame's work and its budget (nanoseconds; the budget follows the frame
 * rate limit). */
void void_perf_hint_report(int64_t actual, int64_t target)
{
    if (!session) return;
    if (target != target_ns && target > 0) { update_target(session, target); target_ns = target; }
    if (actual > 0) report_actual(session, actual);
}
