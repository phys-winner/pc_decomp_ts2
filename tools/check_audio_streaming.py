"""Execute the real native audio output (audio.c) and SND driver (sims2_audio.c) with spies.

audio.c runs against a scripted AAudio device whose data callback is driven by
a simulated clock: the producer's semaphore wait advances time and runs the
callbacks, so the FIFO, the watermarks, the margin, pauses, route losses, the
watchdog and the latency estimate are exercised as on a device. The SND driver
overrides run against a guest stack and a fake output.
"""
import re
import unittest
from audio_probe import ROOT, compile_probe, CLANG

FLAGS = ("/clang:--target=x86_64-pc-windows-msvc", "/clang:-Wno-unused-function")

AUDIO_ENV = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif
#define VOID_ANDROID_APP 1
/* The transport checks compare samples exactly; test_audio_dc_block turns the filter on. */
#define AUDIO_DC_CUT_DEFAULT 0
#define __thread _Thread_local
/* C11 atomics through clang's builtins (the NDK's <stdatomic.h> expects bionic). */
typedef enum { memory_order_relaxed = __ATOMIC_RELAXED, memory_order_acquire = __ATOMIC_ACQUIRE,
               memory_order_release = __ATOMIC_RELEASE, memory_order_acq_rel = __ATOMIC_ACQ_REL,
               memory_order_seq_cst = __ATOMIC_SEQ_CST } memory_order;
#define atomic_load_explicit __c11_atomic_load
#define atomic_store_explicit __c11_atomic_store
#define atomic_exchange_explicit __c11_atomic_exchange
#define atomic_fetch_add_explicit __c11_atomic_fetch_add
#define atomic_load(p) __c11_atomic_load(p, __ATOMIC_SEQ_CST)
#define atomic_store(p, v) __c11_atomic_store(p, v, __ATOMIC_SEQ_CST)
#define atomic_exchange(p, v) __c11_atomic_exchange(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_add(p, v) __c11_atomic_fetch_add(p, v, __ATOMIC_SEQ_CST)
/* ----------------------------------------------------------- the host */
static unsigned char guest[0x20000];
#define GPTR(va) ((void *)(guest + (uint32_t)(va)))
static int machine = 1, urgent;
static void void_blocking_begin(void) { assert(machine); machine = 0; }
static void void_blocking_end(void) { assert(!machine); machine = 1; }
static void void_machine_urgent(int on) { urgent = on; }
static uint32_t heap_top = 0x1000;
static uint32_t void_galloc(uint32_t size)
{
    assert(machine);
    uint32_t va = heap_top;
    heap_top += (size + 15u) & ~15u;
    assert(heap_top <= sizeof guest);
    return va;
}
static int64_t now_ns = 1000000000LL;
static uint64_t void_monotonic_ns(void) { return (uint64_t)now_ns; }
static char data_dir[600];
static const char *void_data_dir(void) { return data_dir; }
static char session[256][640];
static unsigned session_count;
static void void_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(session[session_count % 256], sizeof session[0], format, args);
    va_end(args);
    ++session_count;
}
static const char *logged(const char *needle)
{
    for (unsigned i = session_count > 256 ? session_count - 256 : 0; i < session_count; ++i)
        if (strstr(session[i % 256], needle)) return session[i % 256];
    return NULL;
}
static void void_fatal(const char *format, ...) { (void)format; abort(); }
static uint64_t pause_state;
uint64_t void_audio_pause_state(void) { return pause_state; }
uint32_t void_audio_open(uint32_t min_rate, uint32_t max_rate);
void void_audio_close(void);
int void_audio_wait(uint32_t timeout_ms);
uint32_t void_audio_want(void);
uint32_t void_audio_region(uint32_t *guest_out, uint32_t frames);
void void_audio_commit(uint32_t frames);
uint32_t void_audio_pending_ms(void);
void void_audio_wake(void);
/* ----------------------------------------------------- POSIX stand-ins */
typedef int pthread_mutex_t;
typedef int pthread_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static int lock_depth, names_set;
static int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; assert(!machine && !lock_depth); lock_depth = 1; return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; assert(lock_depth == 1); lock_depth = 0; return 0; }
static int pthread_create(pthread_t *t, void *attr, void *(*fn)(void *), void *arg) { (void)t; (void)attr; (void)fn; (void)arg; return 0; }
static int pthread_detach(pthread_t t) { (void)t; return 0; }
static pthread_t pthread_self(void) { return 1; }
static int pthread_setname_np(pthread_t t, const char *name) { (void)t; assert(!strcmp(name, "snd producer")); ++names_set; return 0; }
#define PRIO_PROCESS 0
static int nice_now, nice_floor = -16;
static int getpriority(int which, int who) { (void)which; (void)who; return nice_now; }
static int setpriority(int which, int who, int value)
{
    (void)which; (void)who;
    if (value < nice_floor) { errno = 13; return -1; }
    nice_now = value;
    return 0;
}
static int usleep(unsigned us) { now_ns += (int64_t)us * 1000; return 0; }
static int test_unlink(const char *path) { return remove(path); }
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
static int test_rename(const char *from, const char *to) { return MoveFileExA(from, to, 1) ? 0 : -1; }
#define unlink test_unlink
#define rename test_rename
typedef struct { int count; } sem_t;
static int sem_init(sem_t *s, int shared, unsigned value) { (void)shared; s->count = (int)value; return 0; }
static int sem_destroy(sem_t *s) { (void)s; return 0; }
static int sem_post(sem_t *s) { ++s->count; return 0; }
static int sem_wait(sem_t *s)
{
    if (s->count > 0) { --s->count; return 0; }
    errno = EINVAL;
    return -1;
}
static int device_run(int64_t deadline, sem_t *wake);
static int sem_timedwait_monotonic_np(sem_t *s, const struct timespec *until)
{
    assert(!machine && !lock_depth);
    if (s->count > 0) { --s->count; return 0; }
    int64_t deadline = (int64_t)until->tv_sec * 1000000000LL + until->tv_nsec;
    assert(deadline >= now_ns);
    if (device_run(deadline, s)) { --s->count; return 0; }
    errno = ETIMEDOUT;
    return -1;
}
/* --------------------------------------------------- a scripted AAudio */
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_data_callback_result_t;
#define AAUDIO_OK 0
#define AAUDIO_ERROR_DISCONNECTED (-899)
#define AAUDIO_ERROR_INVALID_STATE (-895)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define AAUDIO_ERROR_INVALID_FORMAT (-883)
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_SHARING_MODE_SHARED 1
#define AAUDIO_PERFORMANCE_MODE_NONE 10
#define AAUDIO_PERFORMANCE_MODE_POWER_SAVING 11
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12
#define AAUDIO_USAGE_GAME 14
#define AAUDIO_CONTENT_TYPE_MUSIC 2
#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
#define AAUDIO_CALLBACK_RESULT_STOP 1
#define CLOCK_MONOTONIC 1
typedef struct fake_stream AAudioStream;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *, void *, void *, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream *, void *, aaudio_result_t);
typedef struct {
    int live, format, channels, rate, sharing, mode, usage, content;
    AAudioStream_dataCallback data;
    AAudioStream_errorCallback error;
} AAudioStreamBuilder;
struct fake_stream {
    AAudioStreamBuilder asked;
    int rate, mode, burst, capacity, size, started, closed, halted;
    int32_t xruns;
    int64_t written, next_ns, started_ns;
};
static AAudioStreamBuilder builder;
static struct fake_stream streams[256];
static struct fake_stream *running;
static int stream_count, close_count, start_count, size_sets;
static int dev_native = 48000, dev_burst = 256, dev_normal_burst = 960, dev_capacity = 2048, dev_grant_low = 1;
static int dev_latency = 480;
static int64_t dev_start_delay = 2000000;
static aaudio_result_t open_script[32], start_script;
static int open_count, open_index;
#define PLAYED_MAX (48000 * 400)
static int16_t played[PLAYED_MAX];
static uint32_t played_count;
static int recording = 1;

static aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **out)
{
    assert(!machine && lock_depth == 1 && !builder.live);
    memset(&builder, 0, sizeof builder);
    builder.live = 1;
    *out = &builder;
    return AAUDIO_OK;
}
static void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int32_t v) { b->format = v; }
static void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v) { b->channels = v; }
static void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v) { b->rate = v; }
static void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, int32_t v) { b->sharing = v; }
static void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int32_t v) { b->mode = v; }
static void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int32_t v) { b->usage = v; }
static void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int32_t v) { b->content = v; }
static void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback f, void *u) { assert(!u); b->data = f; }
static void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback f, void *u) { assert(!u); b->error = f; }
static aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out)
{
    assert(!machine && lock_depth == 1 && b == &builder && b->live);
    aaudio_result_t result = open_index < open_count ? open_script[open_index++] : AAUDIO_OK;
    if (result != AAUDIO_OK) return result;
    assert(stream_count < 256);
    struct fake_stream *s = &streams[stream_count++];
    memset(s, 0, sizeof *s);
    s->asked = *b;
    s->rate = b->rate ? b->rate : dev_native;
    s->mode = b->mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY && dev_grant_low ? b->mode : AAUDIO_PERFORMANCE_MODE_NONE;
    s->burst = s->mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? dev_burst : dev_normal_burst;
    s->capacity = s->mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? dev_capacity : 3 * dev_normal_burst;
    s->size = s->capacity;
    *out = s;
    return AAUDIO_OK;
}
static aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { assert(b->live); b->live = 0; return AAUDIO_OK; }
static aaudio_result_t AAudioStream_requestStart(AAudioStream *s)
{
    assert(!machine && lock_depth == 1 && !s->closed && !s->started);
    if (start_script) { aaudio_result_t r = start_script; start_script = 0; return r; }
    s->started = 1;
    s->started_ns = now_ns;
    s->next_ns = now_ns + dev_start_delay;
    ++start_count;
    running = s;
    return AAUDIO_OK;
}
static aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { assert(!machine && !s->closed); s->started = 0; return AAUDIO_OK; }
static aaudio_result_t AAudioStream_close(AAudioStream *s)
{
    assert(!machine && !s->closed);
    s->closed = 1;
    s->started = 0;
    ++close_count;
    if (running == s) running = NULL;
    return AAUDIO_OK;
}
static int32_t AAudioStream_getFormat(AAudioStream *s) { return s->asked.format; }
static int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s->asked.channels; }
static int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s->rate; }
static int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s->burst; }
static int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { return s->capacity; }
static int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s->size; }
static aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t frames)
{
    assert(!machine && !s->closed && frames > 0);
    ++size_sets;
    s->size = frames < s->capacity ? frames : s->capacity;
    return s->size;
}
static int32_t AAudioStream_getPerformanceMode(AAudioStream *s) { return s->mode; }
static int32_t AAudioStream_getSharingMode(AAudioStream *s) { return s->asked.sharing; }
static int32_t AAudioStream_getDeviceId(AAudioStream *s) { (void)s; return 3; }
static int32_t AAudioStream_getState(AAudioStream *s) { return s->started ? 4 : 3; }
static int32_t AAudioStream_getXRunCount(AAudioStream *s) { return s->xruns; }
static int64_t AAudioStream_getFramesWritten(AAudioStream *s) { return s->written; }
static int64_t AAudioStream_getFramesRead(AAudioStream *s) { return s->written > s->size ? s->written - s->size : 0; }
static aaudio_result_t AAudioStream_getTimestamp(AAudioStream *s, int32_t clock, int64_t *position, int64_t *at)
{
    assert(clock == CLOCK_MONOTONIC && !machine);
    if (!s->started || s->written < dev_latency) return AAUDIO_ERROR_INVALID_STATE;
    *position = s->written - dev_latency;
    *at = now_ns;
    return AAUDIO_OK;
}
static const char *AAudio_convertResultToText(aaudio_result_t r) { (void)r; return "fake"; }
/* The device: the running stream's callback every burst up to `deadline`;
 * with `wake`, returns once the producer's semaphore holds a post. Plays the
 * left channel into played[] (the right one must mirror it). */
static int device_run(int64_t deadline, sem_t *wake)
{
    static int16_t pcm[2 * 4096];
    while (running && running->started && !running->halted && running->next_ns <= deadline) {
        struct fake_stream *s = running;
        now_ns = s->next_ns;
        aaudio_data_callback_result_t result = s->asked.data(s, NULL, pcm, s->burst);
        for (int i = 0; i < s->burst; ++i) {
            assert(pcm[2 * i + 1] == (int16_t)-pcm[2 * i]);
            if (!recording) continue;
            assert(played_count < PLAYED_MAX);
            played[played_count++] = pcm[2 * i];
        }
        s->written += s->burst;
        s->next_ns += (int64_t)s->burst * 1000000000LL / s->rate;
        if (result == AAUDIO_CALLBACK_RESULT_STOP) s->halted = 1;
        if (wake && wake->count) return 1;
    }
    if (deadline > now_ns) now_ns = deadline;
    return 0;
}
'''

PRODUCER = r'''
#define EXPECT(condition, ...) do { if (!(condition)) { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); assert(condition); } } while (0)
/* The frames the device took from the FIFO: the pattern is never 0. */
static uint32_t heard(void)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < played_count; ++i) count += played[i] != 0;
    return count;
}
/* The game's side: render `delay_ns` after the wake-up (machine, lock, render). */
static uint32_t produced, fills;
static void produce(int64_t delay_ns)
{
    if (delay_ns) { void_blocking_begin(); device_run(now_ns + delay_ns, NULL); void_blocking_end(); }
    uint32_t want = void_audio_want();
    fills += want != 0;
    while (want) {
        uint32_t at, n = void_audio_region(&at, want);
        if (!n) break;
        assert(n % 16 == 0 && at >= fifo_va && at + 4u * n <= fifo_va + 4u * FIFO_FRAMES);
        int16_t *pcm = (int16_t *)GPTR(at);
        for (uint32_t i = 0; i < n; ++i) {
            int16_t v = (int16_t)(1 + produced++ % 30000);
            pcm[2 * i] = v;
            pcm[2 * i + 1] = (int16_t)-v;
        }
        void_audio_commit(n);
        want -= n;
    }
    assert(fifo_level() <= atomic_load(&high_water) + 15u);
}
static void run(int64_t duration_ns, int64_t delay_ns)
{
    for (int64_t end = now_ns + duration_ns; now_ns < end;)
        if (void_audio_wait(20)) produce(delay_ns);
}
/* The played PCM after its first sample: every frame in order, none twice;
 * returns the silent frames, `skips` (when not NULL) the frames never played. */
static uint32_t check_played(uint32_t from, uint32_t *skips)
{
    uint32_t silent = 0, missing = 0, expected = 0;
    for (uint32_t i = from; i < played_count; ++i) {
        if (!played[i]) { if (expected) ++silent; continue; }
        uint32_t value = (uint32_t)played[i];
        if (expected && value != expected) {
            assert(skips);
            missing += (value + 30000u - expected) % 30000u;
        }
        expected = value % 30000u + 1u;
    }
    if (skips) *skips = missing;
    return silent;
}
'''


def audio_source():
    source = (ROOT / "android/host/audio.c").read_text(encoding="utf8")
    dropped = ("#define _GNU_SOURCE", '#include "host.h"', "#include <semaphore.h>", "#include <stdatomic.h>",
               "#include <sys/resource.h>", "#include <unistd.h>", "#include <aaudio/AAudio.h>")
    lines = [line for line in source.split("\n") if line.strip() not in dropped]
    return "\n".join(lines)


def audio_harness(body):
    return AUDIO_ENV + audio_source() + PRODUCER + body


SND_ENV = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char window[0x01000000];      /* guest 0xA1000000..0xA2000000 */
#define GPTR(va) ((void *)(window + ((uint32_t)(va) - 0xA1000000u)))
#define MEM8(va) (*(uint8_t *)GPTR(va))
#define MEM16(va) (*(uint16_t *)GPTR(va))
#define MEM32(va) (*(uint32_t *)GPTR(va))
static uint32_t g_eax, g_esp, g_cur_func = 0xA00E9C68u;
#define ARG(i) MEM32(g_esp + 4u + 4u * (uint32_t)(i))
static int void_vm_writable(uint32_t va, uint32_t size) { return va >= 0xA1000000u && va + size <= 0xA2000000u; }
static char last_log[512];
static unsigned logs;
static void void_log(const char *format, ...) { snprintf(last_log, sizeof last_log, "%s", format); ++logs; }
static uint32_t void_thread_id(void) { return 0x118; }
#define WINAPI_ORDINAL(dll, symbol, ordinal, nargs) static uint32_t symbol##_impl(void)
typedef struct { int kind; uint32_t owner; } void_object;
#define VOID_OBJ_MUTEX 3
static void_object snd_lock = { VOID_OBJ_MUTEX, 0 };
static void_object *void_handle_get(uint32_t handle, int kind) { return handle == 0x77u && kind == VOID_OBJ_MUTEX ? &snd_lock : NULL; }
static uint64_t clock_ns;
static uint64_t void_monotonic_ns(void) { return clock_ns += 1000; }
static uint32_t blocked_owner, blocked_calls;
static uint64_t blocked_ns;
static void void_audio_blocked_by(uint32_t owner, uint64_t ns) { blocked_owner = owner; blocked_ns = ns; ++blocked_calls; }
static unsigned producer_stops;
static void void_audio_producer_stopped(void) { ++producer_stops; }
/* The output, scripted. */
static uint32_t out_rate = 48000, out_pending = 37, opens, closes, open_min, open_max, waits, commits[8], commit_count;
static uint32_t want_frames, regions[8], region_count, region_index, wait_script[16], wait_count, wait_index;
static int stop_after_waits = -1;
static uint32_t void_audio_open(uint32_t min_rate, uint32_t max_rate) { ++opens; open_min = min_rate; open_max = max_rate; return out_rate; }
static void void_audio_close(void) { ++closes; }
static uint32_t void_audio_pending_ms(void) { return out_pending; }
static uint32_t void_audio_want(void) { return want_frames; }
static uint32_t void_audio_region(uint32_t *guest, uint32_t frames)
{
    if (region_index >= region_count) return 0;
    uint32_t n = regions[region_index++];
    assert(n <= frames);
    *guest = 0xA1800000u + 4u * (region_index - 1u) * 0x1000u;
    return n;
}
static void void_audio_commit(uint32_t frames) { assert(commit_count < 8); commits[commit_count++] = frames; }
static int void_audio_wait(uint32_t timeout_ms)
{
    assert(timeout_ms == 20);
    ++waits;
    if (stop_after_waits >= 0 && (int)waits >= stop_after_waits) MEM8(0xA10FE995u) = 0;
    return wait_index < wait_count ? (int)wait_script[wait_index++] : 0;
}
/* Guest calls: the stack discipline of a call, and what each callee does. */
static uint32_t calls[64], call_args[64][4], call_count, rendered[8], render_out[8], render_count, mixer_params[3];
void sub_A00E9E0D(void);
static uint32_t void_call_guest(uint32_t va, int nargs, const uint32_t *args, int pops)
{
    assert(pops == 0 && nargs >= 0 && nargs <= 4 && call_count < 64);
    uint32_t entry = g_esp;
    calls[call_count] = va;
    for (int i = 0; i < nargs; ++i) call_args[call_count][i] = args[i];
    ++call_count;
    g_esp -= 4u * (uint32_t)nargs + 4u;            /* the arguments and the return address */
    for (int i = 0; i < nargs; ++i) MEM32(g_esp + 4u + 4u * (uint32_t)i) = args[i];
    if (va == 0xA00EC268u) {                       /* the mixer reads its parameter block */
        assert(args[0] >= g_esp + 4u * (uint32_t)nargs + 4u);
        memcpy(mixer_params, GPTR(args[0]), sizeof mixer_params);
    } else if (va == 0xA00E99A4u) {                /* render frames to *out */
        uint32_t out = MEM32(args[0]);
        assert(args[0] >= g_esp + 12u && render_count < 8);
        render_out[render_count] = out;
        rendered[render_count++] = args[1];
        for (uint32_t i = 0; i < args[1]; ++i) MEM32(out + 4u * i) = 0x00010001u * (i + 1u);
    } else if (va == 0xA00EA0CCu) {                /* lock; the fill; unlock */
        uint32_t before = g_esp;
        g_esp -= 4u;
        sub_A00E9E0D();
        assert(g_esp == before);
    }
    g_esp = entry;
    g_eax = 0;
    return 0;
}
'''


def snd_harness(body):
    source = (ROOT / "android/host/sims2_audio.c").read_text(encoding="utf8")
    source = "\n".join(line for line in source.split("\n") if line.strip() != '#include "host.h"')
    return SND_ENV + source + body


class SoundStreamingTest(unittest.TestCase):
    # ------------------------------------------------------------ audio.c
    def test_aaudio_low_latency_stream_at_the_device_rate(self):
        compile_probe("audio-open", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000 && machine && !lock_depth);
    struct fake_stream *s = &streams[0];
    /* The rate probe is the stream: low latency, shared, PCM16 stereo, a game, callbacks. */
    assert(stream_count == 1 && start_count == 1 && running == s);
    assert(!s->asked.rate && s->asked.mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    assert(s->asked.sharing == AAUDIO_SHARING_MODE_SHARED && s->asked.format == AAUDIO_FORMAT_PCM_I16);
    assert(s->asked.channels == 2 && s->asked.usage == AAUDIO_USAGE_GAME && s->asked.content == AAUDIO_CONTENT_TYPE_MUSIC);
    assert(s->asked.data && s->asked.error && s->size == 2 * dev_burst);
    /* Low water: one burst and the 6 ms margin; high water one 10 ms tick above. */
    assert(atomic_load(&low_water) == 544 && atomic_load(&high_water) == 1024);
    assert(logged("output: low-latency (asked low-latency), 48000 Hz, burst 256, buffer 2048 -> 512 of 2048 frames"));
    void_audio_close();
    assert(close_count == 1 && !running && !atomic_load(&active) && !void_audio_want());
    /* A rate the mixer does not support: the probe goes, the stream asks for 48 kHz. */
    dev_native = 96000;
    assert(void_audio_open(11025, 48000) == 48000);
    assert(stream_count == 3 && streams[1].closed && streams[2].asked.rate == 48000 && running == &streams[2]);
    void_audio_close();
    dev_native = 44100;
    assert(void_audio_open(11025, 48000) == 44100 && stream_count == 4 && !streams[3].asked.rate && running == &streams[3]);
    void_audio_close();
    assert(names_set == 0 && !urgent);
    /* Paused at the open: no stream until the foreground. */
    dev_native = 48000;
    pause_state = 1;
    assert(void_audio_open(11025, 48000) == 48000 && stream_count == 4 && !running);
    assert(!void_audio_wait(20) && stream_count == 4 && !void_audio_want());
    pause_state = 2;
    void_audio_wait(20);
    assert(stream_count == 5 && streams[4].asked.rate == 48000 && running == &streams[4]);
    void_audio_close();
    /* Denied low latency: a normal stream keeps 20 ms of device queue at least. */
    dev_grant_low = 0;
    assert(void_audio_open(11025, 48000) == 48000 && running->mode == AAUDIO_PERFORMANCE_MODE_NONE);
    assert(running->size == 1920 && logged("output: normal (asked low-latency)"));
    assert(atomic_load(&low_water) == 1248 && atomic_load(&high_water) == 1728);
    assert(names_set == 1 && urgent && nice_now == -16);
    puts("PASS audio open: low-latency shared PCM16 stereo game stream at the device rate, probe kept, 96 kHz -> 48 kHz, pause, normal fallback");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_fifo_continuity_watermarks_and_latency(self):
        compile_probe("audio-steady", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    int64_t start = now_ns;
    run(10000000000LL, 2000000);
    uint32_t skips = 0;
    /* 10 s: every frame played once, in order, without a gap after the first. */
    assert(!check_played(0, &skips) && !skips && !atomic_load(&underruns));
    EXPECT(played_count >= 479000 && played_count <= 481000, "played %u", played_count);
    EXPECT(produced - heard() == fifo_level(), "produced %u heard %u level %u", produced, heard(), fifo_level());
    assert(fifo_level() <= 1024 && atomic_load(&low_water) == 544 && margin_us == 6000);
    /* About one render per 10 ms tick, each only after the callback asked. */
    EXPECT(fills >= 900 && fills <= 1100, "fills %u", fills);
    /* FIFO and device queue: (level + 480 presented later) frames. */
    uint32_t pending = void_audio_pending_ms();
    assert(pending == (fifo_level() + 480u) * 1000u / 48000u && pending >= 15 && pending <= 32);
    assert(logged("[audio] last 10 s: peak 30000") && logged("underruns 0 (0 frames), device xruns 0, errors 0, low-latency 48000 Hz"));
    /* The presented age of the mix: its time in the FIFO (544-1024 frames) plus
     * the device's 480 frames, from the callbacks' frames and the timestamps. */
    const char *age = logged("[latency] last 10 s: presented age mean");
    double mean = 0, worst = 0;
    unsigned p50 = 0, p95 = 0, callbacks = 0;
    EXPECT(age && sscanf(age, "[latency] last 10 s: presented age mean %lf p50 <%u p95 <%u max %lf ms over %u callbacks",
                         &mean, &p50, &p95, &worst, &callbacks) == 5, "latency line: %s", age ? age : "(none)");
    EXPECT(mean >= 15.0 && mean <= 35.0 && p95 <= 36 && worst <= 36.0 && callbacks >= 1500,
           "age mean %.1f p50 %u p95 %u max %.1f over %u", mean, p50, p95, worst, callbacks);
    /* The game stops its mixer before closing: the FIFO runs dry without an underrun. */
    void_audio_producer_stopped();
    void_blocking_begin();
    device_run(now_ns + 100000000, NULL);
    void_blocking_end();
    assert(!fifo_level() && !atomic_load(&underruns) && margin_us == 6000);
    assert(now_ns - start >= 10000000000LL && machine && !lock_depth);
    puts("PASS audio FIFO: 10 s gapless and in order, low 544/high 1024 frames, a render per tick on demand, 15-32 ms pending");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_slow_producer_widens_the_margin_then_narrows(self):
        compile_probe("audio-margin", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    run(2000000000LL, 2000000);
    /* An answer 18 ms late: one jump covers it (its silence, a quarter more). */
    run(3000000000LL, 18000000);
    uint32_t count = atomic_load(&underruns), skips = 0, widened = margin_us;
    EXPECT(count >= 1 && count <= 3 && margin_us >= 18000 && margin_us <= 30000, "underruns %u margin %u", count, margin_us);
    assert(atomic_load(&low_water) >= 256 + 18 * 48);
    run(5000000000LL, 18000000);
    EXPECT(atomic_load(&underruns) == count, "underruns %u after %u", atomic_load(&underruns), count);   /* adapted */
    /* Silence where the FIFO ran dry, but no frame lost or repeated. */
    assert(check_played(0, &skips) > 0 && !skips);
    assert(logged("[audio] FIFO underrun ("));
    /* A 200 ms stall (the game's loading): one jump to cover it, not dozens. */
    recording = 0;
    void_audio_wait(20);
    produce(200000000);
    run(1000000000LL, 2000000);
    uint32_t stalled = atomic_load(&underruns) - count;
    EXPECT(stalled >= 1 && margin_us >= 200000 && margin_us <= 250000, "stall underruns %u margin %u", stalled, margin_us);
    run(1000000000LL, 200000000);
    EXPECT(atomic_load(&underruns) - count == stalled, "repeated stalls %u", atomic_load(&underruns) - count - stalled);
    /* Quiet windows halve the distance to what the answers need (1.5 x 2 ms + 1 ms, at least 6 ms). */
    run(20000000000LL, 2000000);
    uint32_t after_two = margin_us;            /* the window with the stall does not relax */
    run(60000000000LL, 2000000);
    EXPECT(after_two < 160000 && margin_us <= 8000 && atomic_load(&low_water) <= 256 + 8 * 48 + 16,
           "after two windows %u, after a minute more %u (widened %u)", after_two, margin_us, widened);
    run(30000000000LL, 2000000);
    assert(margin_us == 6000 && atomic_load(&low_water) == 544);
    puts("PASS audio margin: late answers widen it at once by their lateness, a 200 ms stall jumps once, quiet windows halve the distance back to 6 ms");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_pause_keeps_the_fifo_and_resumes_in_a_new_stream(self):
        compile_probe("audio-pause", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    run(1000000000LL, 2000000);
    uint32_t level = fifo_level(), heard = played_count;
    assert(level > 0 && !check_played(0, NULL));
    /* The activity pauses: callbacks before the producer notices play silence and keep the FIFO. */
    pause_state = 1;
    device_run(now_ns + 20000000, NULL);
    assert(fifo_level() == level && played_count > heard);
    for (uint32_t i = heard; i < played_count; ++i) assert(!played[i]);
    uint32_t paused_silence = played_count - heard;
    void_audio_wake();
    run(2000000000LL, 2000000);
    assert(!running && close_count == 1 && fifo_level() == level && !void_audio_want());
    /* Resume: a new stream at the FIFO's rate plays on from the same frame. */
    pause_state = 2;
    run(1000000000LL, 2000000);
    assert(running == &streams[1] && streams[1].asked.rate == 48000 && start_count == 2);
    uint32_t skips = 0, silent = check_played(0, &skips);
    EXPECT(silent == paused_silence && !skips && !atomic_load(&underruns), "silent %u expected %u skips %u underruns %u",
           silent, paused_silence, skips, atomic_load(&underruns));
    puts("PASS audio pause: silence without consuming, stream closed, mixer frozen, resumed gapless in a new stream");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_route_loss_reopens_with_backoff_and_null_sink(self):
        compile_probe("audio-route", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    run(2000000000LL, 2000000);
    /* A healthy stream loses its route: closed and reopened at once, nothing lost. */
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    run(1500000000LL, 2000000);
    uint32_t skips = 0;
    assert(stream_count == 2 && streams[0].closed && running == &streams[1] && streams[1].asked.rate == 48000);
    assert(!check_played(0, &skips) && !skips && errors == 1);
    /* A young stream fails and two opens fail: retries 20, 40 and 80 ms later, the
     * null sink running the game's clock meanwhile, with no burst afterwards. */
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    run(100000000LL, 2000000);
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    open_script[0] = open_script[1] = AAUDIO_ERROR_UNAVAILABLE;
    open_count = 2;
    int64_t lost = now_ns;
    uint32_t made = produced;
    while (stream_count < 4) { if (void_audio_wait(20)) produce(2000000); }
    int64_t outage = streams[3].started_ns - lost;
    assert(open_index == 2 && outage >= 140000000LL && outage <= 160000000LL);
    assert(fifo_level() <= atomic_load(&high_water) + 15u);
    run(1000000000LL, 2000000);
    /* What the null sink drained is the outage, at the output rate. */
    check_played(0, &skips);
    uint64_t drained = (uint64_t)skips, expected = (uint64_t)outage * 48000u / 1000000000u;
    EXPECT(drained + 1024 >= expected && drained <= expected + 1024 && produced > made,
           "drained %llu expected %llu outage %lld produced %u made %u", (unsigned long long)drained,
           (unsigned long long)expected, (long long)outage, produced, made);
    EXPECT(errors == 5 && logged("output unavailable: error -889 (fake); next attempt in 40 ms"), "errors %u", errors);
    puts("PASS audio route: healthy loss reopens at once gapless; young loss and failed opens back off 20/40/80 ms, null sink keeps time");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_watchdog_and_normal_mode_until_the_route_changes(self):
        compile_probe("audio-watchdog", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    run(2000000000LL, 2000000);
    /* The callback stops: after 250 ms the stream is replaced... */
    running->halted = 1;
    dev_start_delay = 700000000LL;
    int64_t stopped = now_ns;
    while (stream_count < 2) { if (void_audio_wait(20)) produce(2000000); }
    EXPECT(streams[1].started_ns - stopped >= 250000000LL && streams[1].started_ns - stopped <= 300000000LL,
           "replaced after %lld ns", (long long)(streams[1].started_ns - stopped));
    assert(streams[1].asked.mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY && stalls == 1 && !prefer_normal);
    /* ...and one that never calls back after 500 ms: a second low-latency
     * stall, so the next asks for the normal mode. */
    dev_start_delay = 2000000;
    while (stream_count < 3) { if (void_audio_wait(20)) produce(2000000); }
    EXPECT(streams[2].started_ns - streams[1].started_ns >= 500000000LL && streams[2].started_ns - streams[1].started_ns <= 550000000LL,
           "first callback gap %lld ns", (long long)(streams[2].started_ns - streams[1].started_ns));
    assert(streams[2].asked.mode == AAUDIO_PERFORMANCE_MODE_NONE && prefer_normal && stalls == 2);
    assert(logged("stream stopped calling back") && logged("in the normal mode until the route changes"));
    /* A route change asks for low latency again. */
    run(1500000000LL, 2000000);
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    run(500000000LL, 2000000);
    EXPECT(stream_count == 4 && streams[3].asked.mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY && !prefer_normal && !stalls,
           "streams %d mode %d prefer_normal %d stalls %d", stream_count, streams[stream_count - 1].asked.mode, prefer_normal, stalls);
    /* Device underruns: one burst more of device queue each time. */
    running->xruns = 1;
    run(100000000LL, 2000000);
    assert(running->size == 768 && stat_xruns == 1 && logged("device underruns 1: buffer 512 -> 768 frames"));
    puts("PASS audio watchdog: 250 ms gap and 500 ms first-callback stalls reopen, two go normal until a route change; xruns +1 burst");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_audio_dc_block_removes_a_held_offset(self):
        compile_probe("audio-dc-block", audio_harness(r'''
/* SND's pause: a tone that stops at its peak and stays there. */
static int16_t tone(uint32_t i)
{
    const uint32_t hold = 24000u;                 /* 0.5 s of tone, then the held peak */
    double phase = 2.0 * DC_PI * 1000.0 * (i < hold ? i : hold) / 48000.0;
    return (int16_t)lrint(10000.0 * sin(phase + DC_PI / 2.0));
}
static uint32_t fed;
static void feed(void)
{
    uint32_t want = void_audio_want();
    while (want) {
        uint32_t at, n = void_audio_region(&at, want);
        if (!n) break;
        int16_t *pcm = (int16_t *)GPTR(at);
        for (uint32_t i = 0; i < n; ++i) { int16_t v = tone(fed++); pcm[2 * i] = v; pcm[2 * i + 1] = (int16_t)-v; }
        void_audio_commit(n);
        want -= n;
    }
}
int main(void)
{
    dc_cut = 1;
    assert(void_audio_open(11025, 48000) == 48000);
    for (int64_t end = now_ns + 1500000000LL; now_ns < end;)
        if (void_audio_wait(20)) feed();
    /* played[] starts at the first heard frame; the tone is 24000 frames long. */
    uint32_t first = 0;
    while (first < played_count && !played[first]) ++first;
    assert(played_count - first > 60000);
    int peak = 0, step = 0;
    for (uint32_t i = first + 4800; i < first + 23000; ++i) if (abs(played[i]) > peak) peak = abs(played[i]);
    for (uint32_t i = first + 1; i < first + 40000; ++i)
        if (abs(played[i] - played[i - 1]) > step) step = abs(played[i] - played[i - 1]);
    int late = 0;
    for (uint32_t i = first + 24000 + 4800; i < first + 60000; ++i) if (abs(played[i]) > late) late = abs(played[i]);
    /* The tone passes (10 Hz is far below 1 kHz), the held 10000 fades below 1% in
     * 100 ms, and no step exceeds the tone's own steepest one (1309 per frame). */
    EXPECT(peak >= 9950 && peak <= 10050, "tone peak %d", peak);
    EXPECT(late <= 100, "held offset after 100 ms: %d", late);
    EXPECT(step <= 1320, "largest step %d", step);
    assert(!atomic_load(&underruns));
    puts("PASS audio DC cut: a 1 kHz tone passes, an offset held like SND's pause fades below 1% in 100 ms without a step");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_start_grace_of_a_new_stream(self):
        compile_probe("audio-start-grace", audio_harness(r'''
int main(void)
{
    assert(void_audio_open(11025, 48000) == 48000);
    run(2000000000LL, 2000000);
    assert(margin_us == 6000 && !atomic_load(&underruns));
    /* The route changes to an output without low latency (a capture): the new
     * stream's first burst of 960 frames finds 100 in the FIFO. That gap is
     * the start's, not the producer's lateness: no underrun, no wider margin. */
    dev_grant_low = 0;
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    uint32_t level = fifo_level();
    if (level > 100) fifo_consume(NULL, level - 100, 0);
    uint32_t heard_before = played_count;
    run(100000000LL, 2000000);
    EXPECT(stream_count == 2 && running == &streams[1] && running->mode == AAUDIO_PERFORMANCE_MODE_NONE &&
           running->burst == 960 && running->size == 1920, "streams %d burst %d size %d", stream_count,
           running ? running->burst : -1, running ? running->size : -1);
    uint32_t silent = check_played(heard_before, NULL);
    EXPECT(silent >= 800 && !atomic_load(&underruns) && margin_us == 6000 && atomic_load(&low_water) == 960 + 288,
           "silent %u underruns %u margin %u low %u", silent, atomic_load(&underruns), margin_us, atomic_load(&low_water));
    /* The server ran dry before the first callbacks: counted, the queue kept. */
    running->xruns = 1;
    run(50000000LL, 2000000);
    EXPECT(running->size == 1920 && stat_xruns == 1 && !logged("device underruns"), "size %d xruns %d", running->size, (int)stat_xruns);
    /* After the start, device underruns add a burst and late answers count again. */
    run(300000000LL, 2000000);
    running->xruns = 2;
    run(50000000LL, 2000000);
    EXPECT(running->size == 2880 && stat_xruns == 2 && logged("device underruns 2: buffer 1920 -> 2880 frames"),
           "size %d xruns %d", running->size, (int)stat_xruns);
    run(500000000LL, 40000000LL);
    EXPECT(atomic_load(&underruns) > 0 && margin_us > 6000, "late answers: underruns %u margin %u",
           atomic_load(&underruns), margin_us);
    puts("PASS audio start: a new stream's start gap and device underruns in its first 200 ms leave the margin and the "
         "device queue alone; after them both count");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_aaudio_fault_injection_file(self):
        directory = ROOT / "build/tests/audio-fault/data"
        directory.mkdir(parents=True, exist_ok=True)
        compile_probe("audio-fault", audio_harness(r'''
static void inject(const char *text)
{
    char path[700];
    snprintf(path, sizeof path, "%s/audio_fault", data_dir);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}
int main(void)
{
    snprintf(data_dir, sizeof data_dir, "%s", "''' + directory.as_posix() + r'''");
    assert(void_audio_open(11025, 48000) == 48000);
    run(1500000000LL, 2000000);
    inject("disconnect 1\n");
    run(1500000000LL, 2000000);
    assert(stream_count == 2 && logged("fault injection: disconnect x1"));
    inject("stall 1\n");
    run(1100000000LL, 2000000);
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);   /* the next stream stalls */
    run(1000000000LL, 2000000);
    assert(stream_count == 4 && streams[2].halted && stalls == 1);
    inject("unavailable 2\n");
    run(1100000000LL, 2000000);
    running->asked.error(running, NULL, AAUDIO_ERROR_DISCONNECTED);
    run(1000000000LL, 2000000);
    assert(stream_count == 5 && !fault_opens && logged("fault injection: unavailable x2"));
    char path[700];
    snprintf(path, sizeof path, "%s/audio_fault", data_dir);
    assert(!fopen(path, "r"));
    puts("PASS audio fault injection: disconnect, stall and unavailable from <userdata>/audio_fault, file consumed");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_audio_log_rotation_has_a_combined_16_mb_limit(self):
        directory = ROOT / "build/tests/audio-log-cap/data"
        directory.mkdir(parents=True, exist_ok=True)
        compile_probe("audio-log-cap", audio_harness(r'''
static void seed(const char *path, long length)
{
    FILE *f = fopen(path, "wb");
    assert(f && !fseek(f, length - 1, SEEK_SET));
    assert(fputc('#', f) == '#' && !fclose(f));
}
static long length(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f);
    fclose(f);
    return size;
}
int main(void)
{
    snprintf(data_dir, sizeof data_dir, "%s", "''' + directory.as_posix() + r'''");
    char current[700], previous[700];
    snprintf(current, sizeof current, "%s/sims2.audio.log", data_dir);
    snprintf(previous, sizeof previous, "%s/sims2.audio.1.log", data_dir);
    seed(current, 7999996);
    seed(previous, 8000000);
    audio_log_append("abc\n");
    assert(length(current) == 8000000 && length(previous) == 8000000);
    audio_log_append("latest\n");
    assert(length(current) == 7 && length(previous) == 8000000);
    FILE *f = fopen(previous, "rb");
    char ending[5];
    assert(f && !fseek(f, -4, SEEK_END));
    assert(fgets(ending, sizeof ending, f) && !strcmp(ending, "abc\n"));
    fclose(f);
    /* Without the worker, session lines still reach the log. */
    audio_log_message("[audio] early %d", 1);
    assert(logged("[audio] early 1"));
    audio_log_enabled = 1;
    for (unsigned i = 0; i < AUDIO_LOG_QUEUE_COUNT + 1; i++) audio_log_enqueue("evt\n", false);
    assert(audio_log_ready.count == AUDIO_LOG_QUEUE_COUNT && length(current) == 7);
    unsigned before = session_count;
    audio_log_worker(NULL);
    assert(length(current) == 7 + 4 * AUDIO_LOG_QUEUE_COUNT &&
           atomic_load_explicit(&audio_log_head, memory_order_relaxed) == atomic_load_explicit(&audio_log_tail, memory_order_relaxed));
    audio_log_enqueue("[audio] summary", true);
    audio_log_enqueue("end\n", false);
    assert(session_count == before);
    audio_log_worker(NULL);
    assert(session_count == before + 1 && length(current) == 11 + 4 * AUDIO_LOG_QUEUE_COUNT);
    assert(!remove(current) && !remove(previous));
    puts("PASS audio log: bounded non-blocking queue, separate destinations, 16,000,000 bytes across two rotating files");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_pause_generation_keeps_short_and_duplicate_transitions(self):
        source = (ROOT / "android/host/android_main.c").read_text(encoding="utf8")
        functions = "\n".join(source[source.index(s):source.index("\n}\n", source.index(s)) + 3] for s in (
            "uint64_t void_audio_pause_state(",
            "JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativePause("))
        compile_probe("audio-pause-generation", r'''
#include <assert.h>
#include <stdint.h>
#define JNIEXPORT
#define JNICALL
typedef void JNIEnv;
typedef void *jobject;
typedef unsigned char jboolean;
static void s2_touch_reset_cas_pan(void) {}
static void void_clock_pause(int paused) { (void)paused; }
static unsigned flushes, registry_flushes, wakes;
static void void_readcache_flush_all(void) { ++flushes; }
static void void_registry_flush(void) { ++registry_flushes; }
static void void_audio_wake(void) { ++wakes; }
static uint64_t audio_pause_state;
''' + functions + r'''
int main(void) {
    assert(void_audio_pause_state() == 0);
    for (unsigned i = 0; i < 10000; ++i) {
        uint64_t before = void_audio_pause_state();
        Java_org_sims2_port_GameActivity_nativePause(0,0,0);
        assert(void_audio_pause_state() == before);
        Java_org_sims2_port_GameActivity_nativePause(0,0,1);
        uint64_t paused = void_audio_pause_state();
        assert(paused == before + 3);
        Java_org_sims2_port_GameActivity_nativePause(0,0,1);
        assert(void_audio_pause_state() == paused);
        Java_org_sims2_port_GameActivity_nativePause(0,0,0);
        assert(void_audio_pause_state() == before + 4);
        assert(flushes == 2 * (i + 1) && registry_flushes == flushes && wakes == 4 * (i + 1));
    }
    return 0;
}
''', {}, compiler_override=CLANG, extra_flags=FLAGS)

    # ------------------------------------------------------- sims2_audio.c
    def test_snd_driver_probe_open_close_and_latency(self):
        compile_probe("snd-driver", snd_harness(r'''
static void enter(uint32_t a0, uint32_t a1)
{
    g_esp = 0xA1F00000u;
    MEM32(g_esp) = 0xA0001234u;                    /* the return address */
    MEM32(g_esp + 4) = a0;
    MEM32(g_esp + 8) = a1;
}
int main(void)
{
    /* probe(&max_channels, formats) */
    enter(0xA1100000u, 0xA1100010u);
    memset(GPTR(0xA1100010u), 0xCC, 4);
    sub_A00E9ACB();
    assert(g_eax == 0 && g_esp == 0xA1F00004u && MEM8(0xA1100000u) == 2);
    assert(MEM8(0xA1100010u) == 1 && MEM8(0xA1100011u) == 1 && !MEM8(0xA1100012u) && !MEM8(0xA1100013u));
    enter(0xA1100000u, 0);
    sub_A00E9ACB();
    assert(g_eax == 0 && g_esp == 0xA1F00004u);
    /* open(device, start mixer): the output's rate into SND and its snapshot, the mixer's block. */
    MEM16(0xA10FE9A0u) = 11025; MEM16(0xA10FE9A2u) = 48000; MEM16(0xA10FE9C6u) = 44100;
    MEM16(0xA10FEA76u) = 44100; MEM8(0xA10FE9CAu) = 60; MEM8(0xA10FE9DFu) = 2; MEM8(0xA10FE9E0u) = 0;
    MEM32(0xA10FEC3Cu) = MEM32(0xA10FEC40u) = 0x55555555u;
    enter(0, 1);
    sub_A00E9C68();
    assert(g_eax == 0 && g_esp == 0xA1F00004u && opens == 1 && open_min == 11025 && open_max == 48000);
    assert(MEM16(0xA10FE9C6u) == 48000 && MEM16(0xA10FEA76u) == 48000);
    assert(call_count == 1 && calls[0] == 0xA00EC268u && call_args[0][0] < 0xA1F00000u);
    assert(mixer_params[0] == 48000 && mixer_params[1] == 0xA00EB551u && mixer_params[2] == (60u | 2u << 8 | 1u << 16));
    assert(!MEM32(0xA10FEC3Cu) && !MEM32(0xA10FEC40u));
    /* Without the mixer start; a mono setting becomes stereo in both copies. */
    MEM8(0xA10FE9DFu) = 1; MEM8(0xA10FEA8Fu) = 1; out_rate = 44100;
    enter(0, 0);
    sub_A00E9C68();
    assert(call_count == 1 && MEM8(0xA10FE9DFu) == 2 && MEM8(0xA10FEA8Fu) == 2 && MEM16(0xA10FE9C6u) == 44100);
    /* latency() and close(device, free mixer). */
    enter(0, 0);
    sub_A00E9931();
    assert(g_eax == 37 && g_esp == 0xA1F00004u);
    enter(0, 0);
    sub_A00E98CB();
    assert(closes == 1 && call_count == 1 && g_esp == 0xA1F00004u);
    enter(0, 1);
    sub_A00E98CB();
    assert(closes == 2 && call_count == 2 && calls[1] == 0xA00EBEB1u && g_eax == 0 && g_esp == 0xA1F00004u);
    /* DirectSound is gone. */
    uint32_t out = 0xA1100020u;
    MEM32(out) = 0x12345678u;
    g_esp = 0xA1F00000u; MEM32(g_esp + 8) = out;
    assert(DirectSoundCreate_impl() == 0x88780078u && !MEM32(out));
    puts("PASS SND driver: stereo probe, device rate in config and snapshot, mixer block, forced stereo, latency, close, no DirectSound");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)

    def test_snd_fill_and_service_thread(self):
        compile_probe("snd-service", snd_harness(r'''
int main(void)
{
    /* The fill renders want frames into the FIFO's regions: two pieces at its end. */
    want_frames = 1008; regions[0] = 640; regions[1] = 368; region_count = 2;
    g_esp = 0xA1F00000u; MEM32(g_esp) = 0xA0005678u;
    sub_A00E9E0D();
    assert(g_esp == 0xA1F00004u && render_count == 2 && rendered[0] == 640 && rendered[1] == 368);
    assert(render_out[0] == 0xA1800000u && render_out[1] == 0xA1804000u);
    assert(commit_count == 2 && commits[0] == 640 && commits[1] == 368);
    assert(MEM32(0xA1800000u + 4u * 639u) == 0x00010001u * 640u);
    /* Nothing wanted, no room: nothing rendered. */
    want_frames = 0;
    g_esp = 0xA1F00000u;
    sub_A00E9E0D();
    assert(render_count == 2 && g_esp == 0xA1F00004u);
    /* The thread renders when the output asks, until SND stops it. */
    call_count = 0; render_count = 0; commit_count = 0;
    region_index = 0; region_count = 8; want_frames = 480;
    for (int i = 0; i < 8; ++i) regions[i] = 480;
    wait_script[0] = 0; wait_script[1] = 1; wait_script[2] = 0; wait_script[3] = 1; wait_count = 4;
    stop_after_waits = 5;
    MEM8(0xA10FE995u) = 1; MEM8(0xA10FE994u) = 1;
    MEM32(0xA10FEBD4u) = 0x77u; snd_lock.owner = 0x104u;
    g_esp = 0xA1F00000u; MEM32(g_esp) = 0xA0009999u; MEM32(g_esp + 4) = 0;
    sub_A00EA0DD();
    assert(waits == 5 && call_count == 4 && calls[0] == 0xA00EA0CCu && calls[1] == 0xA00E99A4u);
    assert(calls[2] == 0xA00EA0CCu && calls[3] == 0xA00E99A4u);
    assert(render_count == 2 && commit_count == 2 && !MEM8(0xA10FE994u));
    assert(blocked_calls == 2 && blocked_owner == 0x104u && blocked_ns > 0 && producer_stops == 1);
    assert(g_eax == 0 && g_esp == 0xA1F00008u);
    puts("PASS SND service: fill in FIFO regions across the wrap, renders only on demand, stops with SND, stdcall return");
    return 0;
}
'''), {}, compiler_override=CLANG, extra_flags=FLAGS)


if __name__ == "__main__":
    unittest.main()
