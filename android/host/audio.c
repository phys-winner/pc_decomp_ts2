/* Native audio output for the game's own mixer.
 *
 * The game renders interleaved stereo PCM16 straight into a FIFO in guest
 * memory (void_audio_want/region/commit) and the data callback of an AAudio
 * low-latency stream drains it: one copy from the game's mix to the device,
 * with no DirectSound emulation, second mixer or resampler, since the FIFO
 * runs at the stream's rate (the device's native rate when the game's mixer
 * supports it).
 *
 * The producer is the game's sound thread (sims2_audio.c). It sleeps, with the
 * guest machine released, until the callback leaves the FIFO below its
 * low-water mark, then renders to the high-water mark, one 10 ms mixer tick
 * higher. The low-water mark is one device burst plus a margin for the
 * producer's wake-up, its wait for the machine and the render: a FIFO underrun
 * widens the margin at once by how late the producer was, and every 10 s
 * without one halve its distance to what the producer's answers need.
 * The callback only copies PCM, posts a semaphore and notes the time.
 *
 * Between renders the producer runs the stream's life: (re)open with a 20 ms
 * to 2 s backoff, pause epochs (a fresh stream after the background, with the
 * FIFO kept), route changes reported by the error callback, a watchdog for a
 * callback that stopped coming (a low-latency stream that stalls twice is
 * reopened in the normal performance mode until the route changes), device
 * underruns (one more burst of device buffer) and the latency the game asks
 * for. Without a stream a null sink consumes the FIFO at the output rate, so
 * the game's audio clock keeps running; while the activity is paused nothing
 * is consumed or rendered.
 *
 * <userdata>/audio_fault, read at most once a second, injects failures for
 * testing: "disconnect [n]" (the next n streams lose their route),
 * "unavailable [n]" (the next n opens fail) or "stall [n]" (the next n
 * streams stop calling back). The file is removed; where it cannot be, the
 * same text does not apply again. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <math.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef VOID_ANDROID_APP
#include <aaudio/AAudio.h>
#include <dlfcn.h>

/* Android 8.0-8.1 (API 26-27, the APK's minimum) lack three calls this file used directly. */
static void stream_usage(AAudioStreamBuilder *builder)
{
    void (*usage)(AAudioStreamBuilder *, aaudio_usage_t) =
        (void (*)(AAudioStreamBuilder *, aaudio_usage_t))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
    void (*content)(AAudioStreamBuilder *, aaudio_content_type_t) =
        (void (*)(AAudioStreamBuilder *, aaudio_content_type_t))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType");
    if (usage) usage(builder, AAUDIO_USAGE_GAME);
    if (content) content(builder, AAUDIO_CONTENT_TYPE_MUSIC);
}

/* sem_timedwait against CLOCK_MONOTONIC (bionic's sem_timedwait_monotonic_np, API 28); before it,
   the remaining time on the wall clock (a clock step there only shortens or lengthens one wait of
   at most 50 ms). */
static int sem_wait_monotonic(sem_t *sem, const struct timespec *deadline)
{
    static int (*native)(sem_t *, const struct timespec *);
    static atomic_int looked;
    if (!atomic_load_explicit(&looked, memory_order_acquire)) {
        native = (int (*)(sem_t *, const struct timespec *))dlsym(RTLD_DEFAULT, "sem_timedwait_monotonic_np");
        atomic_store_explicit(&looked, 1, memory_order_release);
    }
    if (native) return native(sem, deadline);
    struct timespec mono, wall;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &wall);
    int64_t left = ((int64_t)deadline->tv_sec - mono.tv_sec) * 1000000000LL + (deadline->tv_nsec - mono.tv_nsec);
    if (left < 0) left = 0;
    int64_t until = (int64_t)wall.tv_sec * 1000000000LL + wall.tv_nsec + left;
    struct timespec absolute = { (time_t)(until / 1000000000LL), (long)(until % 1000000000LL) };
    return sem_timedwait(sem, &absolute);
}
#endif

#define FIFO_FRAMES 16384u                   /* a power of two: 341 ms at 48 kHz */
#define FIFO_MASK (FIFO_FRAMES - 1u)
#define BLOCK 16u                            /* producers render whole 16-frame blocks */
#define RETRY_FIRST_NS 20000000LL
#define RETRY_MAX_NS 2000000000LL
#define HEALTHY_NS 1000000000LL              /* a stream that lasts this long resets the backoff */
#define FIRST_CALLBACK_NS 500000000LL        /* a started stream calls back by then, */
#define CALLBACK_GAP_NS 250000000LL          /* and then at least this often */
/* A new stream's first callbacks drain its prefill before the producer's first
 * answer, and the server can run dry before the first callback: their gaps and
 * device underruns are the start's, not lateness to answer for. */
#define START_GRACE_NS 200000000LL
#define LOW_LATENCY_STALLS 2
#define MARGIN_FIRST_MS 6u
#define MARGIN_MAX_MS 250u                   /* the game's loading can keep the machine 200 ms */
#define LATENCY_POLL_NS 100000000LL
#define SUMMARY_NS 10000000000LL

/* ------------------------------------------------------------- logging */
/* The producer reports; only the log thread touches storage (session log or
 * <userdata>/sims2.audio.log, two rotating 8 MB files). */
#define AUDIO_LOG_SEGMENT_BYTES 8000000L
#define AUDIO_LOG_LINE_BYTES 640
#define AUDIO_LOG_QUEUE_COUNT 32

typedef struct {
    char line[AUDIO_LOG_LINE_BYTES];
    bool session_log;
} audio_log_entry;
static audio_log_entry audio_log_queue[AUDIO_LOG_QUEUE_COUNT];
static _Atomic unsigned audio_log_head, audio_log_tail;
static sem_t audio_log_ready;
static int audio_log_enabled;

static void audio_log_append(const char *line)
{
    const char *dir = void_data_dir();
    if (!dir || !*dir) return;
    size_t size = strlen(line);
    char path[1200], previous[1200];
    snprintf(path, sizeof path, "%s/sims2.audio.log", dir);
    snprintf(previous, sizeof previous, "%s/sims2.audio.1.log", dir);
    FILE *file = fopen(path, "ab");
    if (!file) return;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return; }
    long length = ftell(file);
    if (length < 0 || size > AUDIO_LOG_SEGMENT_BYTES) { fclose(file); return; }
    if (length > AUDIO_LOG_SEGMENT_BYTES - (long)size) {
        fclose(file);
        if (rename(path, previous)) return;
        file = fopen(path, "ab");
        if (!file) return;
    }
    fwrite(line, 1, size, file);
    fclose(file);
}

static void *audio_log_worker(void *unused)
{
    (void)unused;
    for (;;) {
        if (sem_wait(&audio_log_ready)) {
            if (errno == EINTR) continue;
            return NULL;
        }
        unsigned tail = atomic_load_explicit(&audio_log_tail, memory_order_relaxed);
        if (tail == atomic_load_explicit(&audio_log_head, memory_order_acquire)) continue;
        const audio_log_entry *entry = &audio_log_queue[tail % AUDIO_LOG_QUEUE_COUNT];
        if (entry->session_log) void_log("%s", entry->line);
        else audio_log_append(entry->line);
        atomic_store_explicit(&audio_log_tail, tail + 1, memory_order_release);
    }
}

void void_audio_log_start(void)
{
    pthread_t worker;
    if (sem_init(&audio_log_ready, 0, 0)) {
        void_log("[audio] diagnostic queue initialization failed: %d", errno);
        return;
    }
    int error = pthread_create(&worker, NULL, audio_log_worker, NULL);
    if (error) {
        sem_destroy(&audio_log_ready);
        void_log("[audio] diagnostic worker failed: %d", error);
        return;
    }
    pthread_detach(worker);
    audio_log_enabled = 1;
}

static void audio_log_enqueue(const char *line, bool session_log)
{
    if (!audio_log_enabled) {           /* no worker (the command-line build) */
        if (session_log) void_log("%s", line);
        return;
    }
    unsigned head = atomic_load_explicit(&audio_log_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&audio_log_tail, memory_order_acquire) >= AUDIO_LOG_QUEUE_COUNT) return;
    audio_log_entry *entry = &audio_log_queue[head % AUDIO_LOG_QUEUE_COUNT];
    snprintf(entry->line, sizeof entry->line, "%s", line);
    entry->session_log = session_log;
    atomic_store_explicit(&audio_log_head, head + 1, memory_order_release);
    sem_post(&audio_log_ready);
}

__attribute__((format(printf, 1, 2)))
static void audio_log_message(const char *format, ...)
{
    char line[AUDIO_LOG_LINE_BYTES];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    audio_log_enqueue(line, true);
}

/* Stream losses and returns, at most 8 lines in 10 s. */
static int64_t log_window;
static unsigned log_count;

static int audio_may_log(int64_t now)
{
    if (!log_window || now - log_window >= 10000000000LL) {
        if (log_count > 8) audio_log_message("[audio] %u more loss/return lines suppressed", log_count - 8);
        log_window = now;
        log_count = 0;
    }
    return ++log_count <= 8;
}

/* ------------------------------------------------------------- the FIFO */
static uint32_t fifo_va;                     /* FIFO_FRAMES stereo int16 frames in guest memory */
static uint32_t out_rate;                    /* frames per second of the FIFO and the stream */
static _Atomic uint64_t fifo_in, fifo_out;   /* frames committed / consumed */
static _Atomic uint32_t low_water, high_water;
static _Atomic int demand_armed;             /* the producer sleeps and wants a post */
static sem_t demand;
static _Atomic int demand_ready;
static _Atomic int active;                   /* between void_audio_open and void_audio_close */
static _Atomic int primed;                   /* the producer delivered since the stream's start grace */
static _Atomic int64_t grace_until;          /* the current stream's START_GRACE_NS */
static _Atomic uint32_t underruns, underrun_frames;   /* callbacks the FIFO could not fill */
static _Atomic int64_t demand_posted_ns;     /* when the callback last woke the producer */
static _Atomic uint32_t device_queue;        /* frames in the stream, not yet presented */
/* The age of the mix when it is heard (the [latency] line). Each FIFO block
 * remembers when SND committed it; each callback records which stream frame
 * its first FIFO frame became; the producer turns these into ages with the
 * stream's presentation timestamp. The DirectSound build of 20 November 2026
 * was measured the same way, from the Unlock of SND's buffer. */
#define AGE_RING 256u
#define AGE_BINS 256u                        /* 1 ms each, the last one open */
static int64_t fifo_made_ns[FIFO_FRAMES / BLOCK];
static struct { int64_t frame, made; } age_ring[AGE_RING];
static _Atomic uint32_t age_head;            /* written by the callback */
static uint32_t age_tail;                    /* read by the producer */
static int64_t stream_frames;                /* the callback's frames since the stream started */
static uint32_t age_hist[AGE_BINS], age_count, age_max_us;
static double age_sum_ms;

#ifndef VOID_ANDROID_APP
uint64_t void_audio_pause_state(void) { return 0; }
#endif

static uint32_t fifo_level(void)
{
    uint64_t out = atomic_load_explicit(&fifo_out, memory_order_acquire);
    return (uint32_t)(atomic_load_explicit(&fifo_in, memory_order_acquire) - out);
}

/* The consumer: up to `frames` frames into `out` (NULL: drop them), silence
 * after what the FIFO holds; wakes the producer below the low-water mark. */
static uint32_t fifo_consume(int16_t *out, uint32_t frames, int wake)
{
    uint64_t at = atomic_load_explicit(&fifo_out, memory_order_relaxed);
    uint64_t level = atomic_load_explicit(&fifo_in, memory_order_acquire) - at;
    uint32_t n = level < frames ? (uint32_t)level : frames;
    if (out) {
        const uint32_t *ring = (const uint32_t *)GPTR(fifo_va);
        uint32_t index = (uint32_t)at & FIFO_MASK;
        uint32_t first = FIFO_FRAMES - index < n ? FIFO_FRAMES - index : n;
        memcpy(out, ring + index, (size_t)first * 4u);
        memcpy(out + 2u * first, ring, (size_t)(n - first) * 4u);
        if (n < frames) memset(out + 2u * n, 0, (size_t)(frames - n) * 4u);
        if (n < frames && atomic_load_explicit(&primed, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&underruns, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&underrun_frames, frames - n, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&fifo_out, at + n, memory_order_release);
    if (wake && level - n < atomic_load_explicit(&low_water, memory_order_relaxed) &&
        atomic_exchange_explicit(&demand_armed, 0, memory_order_acq_rel)) {
        atomic_store_explicit(&demand_posted_ns, (int64_t)void_monotonic_ns(), memory_order_relaxed);
        sem_post(&demand);
    }
    return n;
}

/* --------------------------------------------------- the producer's side */
static uint32_t burst_frames;                /* the stream's burst, a mixer tick without one */
/* The producer's answer time the low-water mark allows beyond a burst: what
 * it learns outlives the game's own close and reopen of its sound system. */
static uint32_t margin_us = MARGIN_FIRST_MS * 1000u;
static uint32_t seen_underruns, seen_underrun_frames;
static uint32_t last_response_us;            /* the latest fill that answered a post */
static int64_t null_anchor, summary_at;
static __thread int64_t priority_at;         /* the game can restart its sound thread */
static uint32_t errors;                      /* failed opens, lost routes and stalls */
/* The summary's window: PCM the game committed and the producer's answers to
 * the callback: its wake-up, its wait for the machine, the game's own lock
 * and the render (when the producer woke, took the machine, began the fill). */
static int stat_peak;
static double stat_sum;
static int64_t stat_dc[2];                   /* sums per channel: a constant offset shows as their mean */
static uint64_t stat_frames;
static uint32_t stat_renders, stat_underruns, stat_underrun_frames;
static uint32_t stat_response_us, stat_wake_us, stat_machine_us, stat_lock_us, stat_render_us;
static uint32_t stat_blocked_us, stat_blocker;
static int32_t stat_xruns;
static int64_t woke_ns, machine_ns, fill_ns;

static void stat_max_us(uint32_t *max, int64_t ns)
{
    int64_t us = ns > 0 ? ns / 1000 : 0;
    if (us > (int64_t)*max) *max = us > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static void set_watermarks(void)
{
    uint32_t tick = out_rate / 100u / BLOCK * BLOCK;
    if (!tick) tick = BLOCK;
    uint32_t margin = (uint32_t)((uint64_t)margin_us * out_rate / 1000000u);
    uint32_t low = (burst_frames + margin + BLOCK - 1u) / BLOCK * BLOCK;
    uint32_t cap = FIFO_FRAMES - tick - BLOCK;
    if (low > cap) low = cap;
    atomic_store_explicit(&low_water, low, memory_order_relaxed);
    atomic_store_explicit(&high_water, low + tick, memory_order_relaxed);
}

/* A FIFO underrun means the producer answered later than the margin allows:
 * widen it at once to cover that answer (the silence it caused, or the
 * latest measured answer) with a quarter to spare, by half a burst (2 ms) at
 * least, up to MARGIN_MAX_MS. */
static void margin_grow(int64_t now)
{
    uint32_t count = atomic_load_explicit(&underruns, memory_order_relaxed);
    if (count == seen_underruns) return;
    uint32_t frames = atomic_load_explicit(&underrun_frames, memory_order_relaxed);
    uint32_t silent_us = out_rate ? (uint32_t)((uint64_t)(frames - seen_underrun_frames) * 1000000u / out_rate) : 0u;
    seen_underruns = count;
    seen_underrun_frames = frames;
    uint32_t step = out_rate ? (uint32_t)((uint64_t)burst_frames * 500000u / out_rate) : 0u;
    if (step < 2000u) step = 2000u;
    if (step < silent_us / 4u * 5u) step = silent_us / 4u * 5u;
    uint64_t wanted = (uint64_t)margin_us + step, answer = (uint64_t)last_response_us * 5u / 4u + 1000u;
    if (wanted < answer) wanted = answer;
    if (wanted > MARGIN_MAX_MS * 1000u) wanted = MARGIN_MAX_MS * 1000u;
    uint32_t before = margin_us;
    margin_us = (uint32_t)wanted;
    set_watermarks();
    if (margin_us != before && audio_may_log(now))
        audio_log_message("[audio] FIFO underrun (%.1f ms silent, answer %.1f ms): margin %.1f -> %.1f ms (low %u, high %u frames)",
                          silent_us / 1000.0, last_response_us / 1000.0, before / 1000.0, margin_us / 1000.0,
                          atomic_load(&low_water), atomic_load(&high_water));
}

/* A summary window without an underrun halves the distance to what the
 * producer needed in it (1.5 times its slowest answer and 1 ms), 1 ms at least,
 * down to MARGIN_FIRST_MS. */
static void margin_relax(uint32_t window_underruns, uint32_t window_response_us)
{
    if (window_underruns) return;
    uint64_t target = (uint64_t)window_response_us * 3u / 2u + 1000u;
    if (target < MARGIN_FIRST_MS * 1000u) target = MARGIN_FIRST_MS * 1000u;
    if (margin_us <= target) return;
    uint32_t step = (uint32_t)((margin_us - target) / 2u);
    if (step < 1000u) step = 1000u;
    margin_us = margin_us - step > target ? margin_us - step : (uint32_t)target;
    set_watermarks();
}

/* With no stream the FIFO drains at the output rate; time with nothing to
 * drain is not owed afterwards. */
static void null_sink(int64_t now)
{
    if (!null_anchor) { null_anchor = now; return; }
    uint64_t frames = (uint64_t)(now - null_anchor) * out_rate / 1000000000u;
    if (!frames) return;
    null_anchor += (int64_t)(frames * 1000000000u / out_rate);
    uint32_t level = fifo_level();
    fifo_consume(NULL, frames < level ? (uint32_t)frames : level, 0);
}

/* The mixer's thread competes with the game's for the biggest cores: ask for
 * the audio priority (-16, granted only to some apps), else the game's own
 * -10; Android can lower it again, so it is re-asserted every second. */
static void producer_priority(int64_t now)
{
    if (priority_at && now - priority_at < 1000000000LL) return;
    priority_at = now;
    int nice = getpriority(PRIO_PROCESS, 0);
    if (nice > -16 && setpriority(PRIO_PROCESS, 0, -16) && nice > -10) setpriority(PRIO_PROCESS, 0, -10);
}

/* ------------------------------------------------------------- AAudio */
#ifdef VOID_ANDROID_APP
static pthread_mutex_t device_lock = PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *stream;                 /* the producer's (and open/close's) under device_lock */
static _Atomic(AAudioStream *) failed_stream;
static _Atomic int failed_code;
static _Atomic int64_t callback_ns;          /* the latest data callback */
static _Atomic int callback_stop;            /* fault injection: stop at the next callback */
static uint64_t stream_epoch;
static int64_t stream_started_ns, retry_at, retry_delay, latency_at;
static int stream_low, prefer_normal, stalls;
static int32_t stream_xruns;
static unsigned fault_disconnects, fault_opens, fault_stalls;
static int64_t fault_polled;
static char fault_last[64];

static aaudio_data_callback_result_t data_callback(AAudioStream *s, void *user, void *data, int32_t count)
{
    (void)s; (void)user;
    uint32_t frames = count > 0 ? (uint32_t)count : 0u;
    int16_t *out = (int16_t *)data;
    atomic_store_explicit(&callback_ns, (int64_t)void_monotonic_ns(), memory_order_relaxed);
    int64_t first = stream_frames;
    stream_frames += frames;
    if (atomic_exchange_explicit(&callback_stop, 0, memory_order_relaxed)) {
        memset(out, 0, (size_t)frames * 4u);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    /* In the background the FIFO waits for the next stream. */
    if (void_audio_pause_state() & 1u) {
        memset(out, 0, (size_t)frames * 4u);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint64_t at = atomic_load_explicit(&fifo_out, memory_order_relaxed);
    if (fifo_consume(out, frames, 1)) {
        uint32_t head = atomic_load_explicit(&age_head, memory_order_relaxed);
        age_ring[head % AGE_RING].frame = first;
        age_ring[head % AGE_RING].made = fifo_made_ns[(uint32_t)(at / BLOCK) & (FIFO_FRAMES / BLOCK - 1u)];
        atomic_store_explicit(&age_head, head + 1u, memory_order_release);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* A lost route or a dead audio server: the producer reopens. */
static void error_callback(AAudioStream *s, void *user, aaudio_result_t error)
{
    (void)user;
    atomic_store(&failed_code, (int)error);
    atomic_store(&failed_stream, s);
    if (atomic_load(&demand_ready)) sem_post(&demand);
}

static const char *mode_name(int mode)
{
    return mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low-latency" :
           mode == AAUDIO_PERFORMANCE_MODE_POWER_SAVING ? "power-saving" : "normal";
}

static void fault_poll(int64_t now)
{
    if (fault_polled && now - fault_polled < 1000000000LL) return;
    fault_polled = now;
    const char *dir = void_data_dir();
    if (!dir || !*dir) return;
    char path[1200], line[64] = "", kind[16] = "";
    snprintf(path, sizeof path, "%s/audio_fault", dir);
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (!fgets(line, sizeof line, file)) line[0] = 0;
    fclose(file);
    if (unlink(path)) {
        if (!strcmp(line, fault_last)) return;
        snprintf(fault_last, sizeof fault_last, "%s", line);
    }
    unsigned count = 1;
    if (sscanf(line, "%15s %u", kind, &count) < 1) return;
    if (count > 1000) count = 1000;
    if (!strcmp(kind, "disconnect")) fault_disconnects = count;
    else if (!strcmp(kind, "unavailable")) fault_opens = count;
    else if (!strcmp(kind, "stall")) fault_stalls = count;
    else { audio_log_message("[audio] fault injection: unknown request '%s'", kind); return; }
    audio_log_message("[audio] fault injection: %s x%u", kind, count);
}

/* Event lines for <userdata>/sims2.audio.log, each kind at most every 5 s. */
enum { EV_STARTED, EV_OPEN, EV_START, EV_ROUTE, EV_STALL, EV_XRUN, EV_EVENTS };

static void audio_report(unsigned event, int code, int64_t now)
{
    static const char *const names[EV_EVENTS] = { "started", "open", "start", "route", "stall", "underrun" };
    static struct { int64_t last; unsigned seen, suppressed; } reports[EV_EVENTS];
    if (reports[event].seen && now - reports[event].last < 5000000000LL) {
        ++reports[event].suppressed;
        return;
    }
    char line[AUDIO_LOG_LINE_BYTES];
    snprintf(line, sizeof line,
        "[audio] time=%lld uptime_ms=%lld event=%s code=%d suppressed=%u epoch=%llu requested=%s "
        "device=%d state=%d sharing=%d mode=%d rate=%d channels=%d burst=%d buffer=%d capacity=%d "
        "frames_written=%lld frames_read=%lld xruns=%d fifo=%u low=%u high=%u underruns=%u errors=%u\n",
        (long long)time(NULL), (long long)(now / 1000000), names[event], code, reports[event].suppressed,
        (unsigned long long)void_audio_pause_state(), stream_low ? "low-latency" : "normal",
        stream ? (int)AAudioStream_getDeviceId(stream) : -1, stream ? (int)AAudioStream_getState(stream) : -1,
        stream ? (int)AAudioStream_getSharingMode(stream) : -1, stream ? (int)AAudioStream_getPerformanceMode(stream) : -1,
        stream ? (int)AAudioStream_getSampleRate(stream) : -1, stream ? (int)AAudioStream_getChannelCount(stream) : -1,
        stream ? (int)AAudioStream_getFramesPerBurst(stream) : -1, stream ? (int)AAudioStream_getBufferSizeInFrames(stream) : -1,
        stream ? (int)AAudioStream_getBufferCapacityInFrames(stream) : -1,
        stream ? (long long)AAudioStream_getFramesWritten(stream) : -1, stream ? (long long)AAudioStream_getFramesRead(stream) : -1,
        stream ? (int)AAudioStream_getXRunCount(stream) : -1, fifo_level(), atomic_load(&low_water),
        atomic_load(&high_water), atomic_load(&underruns), errors);
    reports[event].seen = 1;
    reports[event].last = now;
    reports[event].suppressed = 0;
    audio_log_enqueue(line, false);
}

static aaudio_result_t device_create(AAudioStream **out, int32_t rate, int low)
{
    AAudioStreamBuilder *builder = NULL;
    *out = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return result;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    if (rate > 0) AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, low ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_NONE);
    stream_usage(builder);
    AAudioStreamBuilder_setDataCallback(builder, data_callback, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, error_callback, NULL);
    result = AAudioStreamBuilder_openStream(builder, out);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) { *out = NULL; return result; }
    if (AAudioStream_getFormat(*out) != AAUDIO_FORMAT_PCM_I16 || AAudioStream_getChannelCount(*out) != 2 ||
        (rate > 0 && AAudioStream_getSampleRate(*out) != rate)) {
        AAudioStream_close(*out);
        *out = NULL;
        return AAUDIO_ERROR_INVALID_FORMAT;
    }
    return AAUDIO_OK;
}

static void stream_close(void)
{
    if (!stream) return;
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);                  /* no callback runs after this */
    stream = NULL;
    age_tail = atomic_load(&age_head);
    atomic_store(&failed_stream, NULL);
    atomic_store(&device_queue, 0);
}

/* An open or a start failed, or a stream failed within a second: retry
 * 20 ms, 40 ms, ... up to 2 s later, the null sink keeping the game's clock. */
static void open_failed(int64_t now, aaudio_result_t error)
{
    ++errors;
    stream_close();
    retry_delay = !retry_delay ? RETRY_FIRST_NS : retry_delay < RETRY_MAX_NS / 2 ? retry_delay * 2 : RETRY_MAX_NS;
    retry_at = now + retry_delay;
    audio_report(EV_OPEN, (int)error, now);
    if (audio_may_log(now))
        audio_log_message("[audio] output unavailable: error %d (%s); next attempt in %lld ms", (int)error,
                          AAudio_convertResultToText(error), (long long)(retry_delay / 1000000));
}

/* Open (or take `opened`), size the device buffer, start. */
static void stream_open(int64_t now, uint64_t pause, AAudioStream *opened)
{
    int low = !prefer_normal;
    aaudio_result_t result = AAUDIO_OK;
    AAudioStream *s = opened;
    if (!s) {
        if (fault_opens) { --fault_opens; result = AAUDIO_ERROR_UNAVAILABLE; }
        else result = device_create(&s, (int32_t)out_rate, low);
    }
    if (result != AAUDIO_OK) { open_failed(now, result); return; }
    stream = s;
    stream_low = low;
    stream_xruns = 0;
    /* Two bursts in the low-latency mode; the normal mode's bursts are
     * larger, and its queue keeps at least 20 ms. */
    int32_t burst = AAudioStream_getFramesPerBurst(s), capacity = AAudioStream_getBufferCapacityInFrames(s);
    int32_t before = AAudioStream_getBufferSizeInFrames(s);
    if (burst > 0) {
        int64_t target = 2LL * burst;
        if (!low && target < (int64_t)out_rate / 50) target = ((int64_t)out_rate / 50 + burst - 1) / burst * burst;
        if (capacity > 0 && target > capacity) target = capacity;
        AAudioStream_setBufferSizeInFrames(s, (int32_t)target);
    }
    burst_frames = burst > 0 && (uint32_t)burst < FIFO_FRAMES / 4u ? (uint32_t)burst : out_rate / 100u;
    set_watermarks();
    atomic_store(&callback_ns, 0);
    atomic_store(&primed, 0);
    atomic_store(&grace_until, now + START_GRACE_NS);
    stream_frames = 0;                           /* no callback runs before the start */
    age_tail = atomic_load(&age_head);
    if (fault_stalls) { --fault_stalls; atomic_store(&callback_stop, 1); }
    result = AAudioStream_requestStart(s);
    if (result != AAUDIO_OK) { open_failed(now, result); return; }
    stream_epoch = pause;
    stream_started_ns = now;
    null_anchor = 0;
    audio_report(EV_STARTED, 0, now);
    if (audio_may_log(now))
        audio_log_message("[audio] output: %s (asked %s), %d Hz, burst %d, buffer %d -> %d of %d frames, device %d, "
                          "FIFO low %u high %u", mode_name(AAudioStream_getPerformanceMode(s)), low ? "low-latency" : "normal",
                          (int)AAudioStream_getSampleRate(s), (int)burst, (int)before,
                          (int)AAudioStream_getBufferSizeInFrames(s), (int)capacity, (int)AAudioStream_getDeviceId(s),
                          atomic_load(&low_water), atomic_load(&high_water));
}

/* The error callback named this stream: a changed route (headset, Bluetooth,
 * the system capturing the output) or a restarted audio server. */
static void stream_failed(int64_t now, int code)
{
    int young = now - stream_started_ns < HEALTHY_NS;
    audio_report(EV_ROUTE, code, now);
    if (code == AAUDIO_ERROR_DISCONNECTED) { prefer_normal = 0; stalls = 0; }
    if (young) { open_failed(now, code); return; }
    ++errors;
    stream_close();
    retry_at = 0;
    retry_delay = 0;
    if (audio_may_log(now))
        audio_log_message("[audio] stream error %d (%s); reopening", code, AAudio_convertResultToText(code));
}

static int stream_stalled(int64_t now)
{
    int64_t last = atomic_load_explicit(&callback_ns, memory_order_relaxed);
    if (last < stream_started_ns) return now - stream_started_ns > FIRST_CALLBACK_NS;
    return now - last > CALLBACK_GAP_NS;
}

static void stream_restart_stalled(int64_t now)
{
    int64_t last = atomic_load_explicit(&callback_ns, memory_order_relaxed);
    ++errors;
    ++stalls;
    audio_report(EV_STALL, stalls, now);
    if (stream_low && stalls >= LOW_LATENCY_STALLS) prefer_normal = 1;
    if (audio_may_log(now))
        audio_log_message("[audio] %s stream stopped calling back (%lld ms); reopening%s", stream_low ? "low-latency" : "normal",
                          (long long)((now - (last >= stream_started_ns ? last : stream_started_ns)) / 1000000),
                          prefer_normal && stream_low ? " in the normal mode until the route changes" : "");
    stream_close();
    retry_at = 0;
}

/* The ages of the callbacks recorded since the last timestamp: when their first
 * frame is heard (position `position` was heard at `at`) minus when SND
 * committed it. */
static void age_drain(int64_t position, int64_t at)
{
    uint32_t head = atomic_load_explicit(&age_head, memory_order_acquire);
    if (head - age_tail > AGE_RING) age_tail = head - AGE_RING;
    for (; age_tail != head; ++age_tail) {
        int64_t frame = age_ring[age_tail % AGE_RING].frame, made = age_ring[age_tail % AGE_RING].made;
        if (!made || !out_rate) continue;
        int64_t heard = at + (frame - position) * 1000000000LL / (int64_t)out_rate;
        int64_t us = (heard - made) / 1000;
        if (us <= 0 || us > 2000000) continue;
        age_sum_ms += (double)us / 1000.0;
        ++age_count;
        if ((uint32_t)us > age_max_us) age_max_us = (uint32_t)us;
        uint32_t bin = (uint32_t)(us / 1000);
        ++age_hist[bin < AGE_BINS - 1u ? bin : AGE_BINS - 1u];
    }
}

/* Device underruns: the callback came late; one burst more of device queue. */
static void stream_tune(int64_t now)
{
    int32_t xruns = AAudioStream_getXRunCount(stream);
    if (xruns > stream_xruns) {
        int32_t size = AAudioStream_getBufferSizeInFrames(stream), capacity = AAudioStream_getBufferCapacityInFrames(stream);
        int32_t burst = AAudioStream_getFramesPerBurst(stream), grown = size;
        if (now - stream_started_ns >= START_GRACE_NS && size > 0 && burst > 0 && capacity > size)
            grown = AAudioStream_setBufferSizeInFrames(stream, size + burst < capacity ? size + burst : capacity);
        stat_xruns += xruns - stream_xruns;
        stream_xruns = xruns;
        audio_report(EV_XRUN, (int)grown, now);
        if (grown > size && audio_may_log(now))
            audio_log_message("[audio] device underruns %d: buffer %d -> %d frames", (int)xruns, (int)size, (int)grown);
    }
    if (retry_delay && now - stream_started_ns >= HEALTHY_NS) retry_delay = 0;
    if (now - latency_at < LATENCY_POLL_NS) return;
    latency_at = now;
    int64_t written = AAudioStream_getFramesWritten(stream), position = 0, at = 0, queued;
    aaudio_result_t stamped = AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &at);
    int64_t stamp_now = (int64_t)void_monotonic_ns();
    if (stamped == AAUDIO_OK && at > 0 && at <= stamp_now) age_drain(position, at);
    if (stamped == AAUDIO_OK && at > 0 && at <= stamp_now)
        queued = written - (position + (stamp_now - at) * (int64_t)out_rate / 1000000000LL);
    else
        queued = written - AAudioStream_getFramesRead(stream);
    if (queued < 0) queued = 0;
    if (queued > (int64_t)out_rate) queued = out_rate;
    atomic_store(&device_queue, (uint32_t)queued);
}
#endif

/* The stream's life, run by the producer with the machine released. Returns
 * whether a stream consumes the FIFO. */
static int audio_service(int64_t now)
{
#ifdef VOID_ANDROID_APP
    fault_poll(now);
    uint64_t pause = void_audio_pause_state();
    pthread_mutex_lock(&device_lock);
    /* L2: the background ends the stream; a new foreground epoch opens a new one. */
    if (stream && pause != stream_epoch) stream_close();
    if (stream) {
        AAudioStream *failed = atomic_load(&failed_stream);
        int code = atomic_load(&failed_code);
        if (fault_disconnects && failed != stream) { --fault_disconnects; failed = stream; code = AAUDIO_ERROR_DISCONNECTED; }
        if (failed == stream) stream_failed(now, code);
        else if (stream_stalled(now)) stream_restart_stalled(now);
        else stream_tune(now);
    }
    if (!stream && atomic_load(&active) && !(pause & 1u) && now >= retry_at) stream_open(now, pause, NULL);
    int running = stream != NULL;
    pthread_mutex_unlock(&device_lock);
    return running;
#else
    (void)now;
    return 0;
#endif
}

static void summary(int64_t now)
{
    if (!summary_at) { summary_at = now; return; }
    if (now - summary_at < SUMMARY_NS) return;
    summary_at = now;
    uint32_t count = atomic_load(&underruns), frames = atomic_load(&underrun_frames);
    const char *mode = "none";
    int burst = -1, buffer = -1;
#ifdef VOID_ANDROID_APP
    pthread_mutex_lock(&device_lock);
    if (stream) {
        mode = mode_name(AAudioStream_getPerformanceMode(stream));
        burst = (int)AAudioStream_getFramesPerBurst(stream);
        buffer = (int)AAudioStream_getBufferSizeInFrames(stream);
    }
    pthread_mutex_unlock(&device_lock);
#endif
    audio_log_message("[audio] last 10 s: peak %d, rms %.0f, dc %.0f %.0f, underruns %u (%u frames), device xruns %d, errors %u, "
                      "%s %u Hz, burst %d, buffer %d, FIFO low %u high %u, latency %u ms, renders %u, "
                      "max response %.1f ms (wake %.1f, machine %.1f, lock %.1f, render %.1f), "
                      "slowest lock %.1f ms held by thread %u",
                      stat_peak, stat_frames ? sqrt(stat_sum / (2.0 * (double)stat_frames)) : 0.0,
                      stat_frames ? (double)stat_dc[0] / (double)stat_frames : 0.0,
                      stat_frames ? (double)stat_dc[1] / (double)stat_frames : 0.0,
                      count - stat_underruns, frames - stat_underrun_frames, (int)stat_xruns, errors, mode, out_rate,
                      burst, buffer, atomic_load(&low_water), atomic_load(&high_water), void_audio_pending_ms(),
                      stat_renders, stat_response_us / 1000.0, stat_wake_us / 1000.0, stat_machine_us / 1000.0,
                      stat_lock_us / 1000.0, stat_render_us / 1000.0, stat_blocked_us / 1000.0, stat_blocker);
    if (age_count) {
        uint32_t p50 = 0, p95 = 0, seen = 0;
        for (uint32_t b = 0; b < AGE_BINS; ++b) {
            seen += age_hist[b];
            if (!p50 && seen * 2u >= age_count) p50 = b + 1u;
            if (!p95 && seen * 20u >= age_count * 19u) { p95 = b + 1u; break; }
        }
        audio_log_message("[latency] last 10 s: presented age mean %.1f p50 <%u p95 <%u max %.1f ms over %u callbacks",
                          age_sum_ms / age_count, p50, p95, age_max_us / 1000.0, age_count);
    }
    memset(age_hist, 0, sizeof age_hist);
    age_count = age_max_us = 0;
    age_sum_ms = 0;
    margin_relax(count - stat_underruns, stat_response_us);
    stat_peak = 0;
    stat_sum = 0;
    stat_dc[0] = stat_dc[1] = 0;
    stat_frames = 0;
    stat_renders = 0;
    stat_response_us = stat_wake_us = stat_machine_us = stat_lock_us = stat_render_us = 0;
    stat_blocked_us = stat_blocker = 0;
    stat_xruns = 0;
    stat_underruns = count;
    stat_underrun_frames = frames;
}

/* ------------------------------------------------------------- the API */
uint32_t void_audio_open(uint32_t min_rate, uint32_t max_rate)
{
    if (!fifo_va) fifo_va = void_galloc(FIFO_FRAMES * 4u);   /* lives as long as the process */
    if (!fifo_va) void_fatal("audio FIFO allocation failed");
    if (!atomic_load(&demand_ready)) {
        if (sem_init(&demand, 0, 0)) void_fatal("audio semaphore: %d", errno);
        atomic_store(&demand_ready, 1);
    }
    void_blocking_begin();
    int64_t now = (int64_t)void_monotonic_ns();
    uint32_t rate = 48000u;
    if (rate > max_rate) rate = max_rate;
    if (rate < min_rate) rate = min_rate;
    atomic_store(&fifo_in, 0);
    atomic_store(&fifo_out, 0);
    atomic_store(&primed, 0);
    seen_underruns = atomic_load(&underruns);
    seen_underrun_frames = atomic_load(&underrun_frames);
#ifdef VOID_ANDROID_APP
    pthread_mutex_lock(&device_lock);
    stream_close();
    retry_at = retry_delay = 0;
    prefer_normal = stalls = 0;
    uint64_t pause = void_audio_pause_state();
    AAudioStream *probe = NULL;
    /* The device's own rate, when the game's mixer can run at it. */
    if (!(pause & 1u) && device_create(&probe, 0, 1) == AAUDIO_OK) {
        uint32_t native = (uint32_t)AAudioStream_getSampleRate(probe);
        if (native >= min_rate && native <= max_rate) rate = native;
        else { AAudioStream_close(probe); probe = NULL; }
        audio_log_message("[audio] device rate %u Hz; the game mixes at %u Hz (supported %u..%u), margin %.1f ms",
                          native, rate, min_rate, max_rate, margin_us / 1000.0);
    }
    out_rate = rate;
    burst_frames = rate / 100u;
    set_watermarks();
    atomic_store(&active, 1);
    if (!(pause & 1u)) stream_open(now, pause, probe);
    pthread_mutex_unlock(&device_lock);
#else
    out_rate = rate;
    burst_frames = rate / 100u;
    set_watermarks();
    atomic_store(&active, 1);
    audio_log_message("[audio] no audio device in this build: a null sink paces the game's mixer at %u Hz", rate);
#endif
    void_blocking_end();
    return rate;
}

void void_audio_close(void)
{
    if (!atomic_load(&active)) return;
    void_blocking_begin();
    atomic_store(&active, 0);            /* first: the producer reopens nothing after this */
#ifdef VOID_ANDROID_APP
    pthread_mutex_lock(&device_lock);
    stream_close();
    pthread_mutex_unlock(&device_lock);
#endif
    atomic_store(&fifo_in, 0);
    atomic_store(&fifo_out, 0);
    audio_log_message("[audio] output closed by the game");
    void_blocking_end();
}

int void_audio_wait(uint32_t timeout_ms)
{
    if (!atomic_load(&active)) {         /* nothing to feed: no busy loop either */
        void_blocking_begin();
        usleep(10000);
        void_blocking_end();
        return 0;
    }
    void_blocking_begin();
    int64_t now = (int64_t)void_monotonic_ns(), end = now + (int64_t)timeout_ms * 1000000LL;
    int want = 0;
    if (!priority_at) {                  /* this thread's first wait */
        pthread_setname_np(pthread_self(), "snd producer");
        void_machine_urgent(1);
    }
    producer_priority(now);
    for (;;) {
        int running = audio_service(now);
        uint64_t pause = void_audio_pause_state();
        if (!(pause & 1u)) {
            if (running) null_anchor = 0;
            else null_sink(now);
            margin_grow(now);
            summary(now);
            if (fifo_level() < atomic_load(&low_water)) { want = 1; break; }
        } else {
            null_anchor = 0;
        }
        if (now >= end) break;
        atomic_store_explicit(&demand_armed, 1, memory_order_release);
        if (!(pause & 1u) && fifo_level() < atomic_load(&low_water)) { want = 1; break; }
        int64_t step = (pause & 1u) ? 50000000LL : running ? 20000000LL : 5000000LL, until = now + step;
        if (until > end) until = end;
        struct timespec deadline = { (time_t)(until / 1000000000LL), (long)(until % 1000000000LL) };
        while (sem_wait_monotonic(&demand, &deadline) && errno == EINTR) {}
        now = (int64_t)void_monotonic_ns();
    }
    woke_ns = now;
    void_blocking_end();
    machine_ns = (int64_t)void_monotonic_ns();
    return want;
}

uint32_t void_audio_want(void)
{
    fill_ns = (int64_t)void_monotonic_ns();
    if (!atomic_load(&active) || (void_audio_pause_state() & 1u)) return 0;
    uint32_t level = fifo_level(), high = atomic_load(&high_water);
    if (level >= high) return 0;
    uint32_t want = (high - level + BLOCK - 1u) / BLOCK * BLOCK, room = (FIFO_FRAMES - level) / BLOCK * BLOCK;
    return want < room ? want : room;
}

uint32_t void_audio_region(uint32_t *guest, uint32_t frames)
{
    uint64_t at = atomic_load_explicit(&fifo_in, memory_order_relaxed);
    uint32_t level = (uint32_t)(at - atomic_load_explicit(&fifo_out, memory_order_acquire));
    uint32_t index = (uint32_t)at & FIFO_MASK, n = FIFO_FRAMES - level;
    if (n > FIFO_FRAMES - index) n = FIFO_FRAMES - index;
    if (n > frames) n = frames;
    n -= n % BLOCK;
    *guest = fifo_va + index * 4u;
    return n;
}

/* SND holds paused voices at their last sample: while the game is paused its
 * mix is a constant (thousands on a channel), which a stream stop or start
 * turns into a click and the speaker carries as a steady offset. A one-pole
 * high-pass at 10 Hz removes it before the FIFO; nothing below 10 Hz is sound,
 * and a held offset fades out in about 50 ms without a step. */
#define DC_CUT_HZ 10.0
#define DC_PI 3.14159265358979323846   /* M_PI is not in every math.h */
#ifndef AUDIO_DC_CUT_DEFAULT
#define AUDIO_DC_CUT_DEFAULT 1
#endif
static int dc_cut = AUDIO_DC_CUT_DEFAULT;
static double dc_x[2], dc_y[2], dc_r;
static uint32_t dc_rate;

static void dc_block(int16_t *pcm, uint32_t frames)
{
    if (dc_rate != out_rate) {
        dc_rate = out_rate;
        dc_r = out_rate ? 1.0 - 2.0 * DC_PI * DC_CUT_HZ / (double)out_rate : 0.0;
    }
    for (uint32_t i = 0; i < 2u * frames; ++i) {
        unsigned c = i & 1u;
        double x = pcm[i], y = x - dc_x[c] + dc_r * dc_y[c];
        dc_x[c] = x;
        dc_y[c] = y;
        long v = lrint(y);
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void void_audio_commit(uint32_t frames)
{
    if (!frames) return;
    uint64_t at = atomic_load_explicit(&fifo_in, memory_order_relaxed);
    int16_t *pcm = (int16_t *)GPTR(fifo_va) + 2u * ((uint32_t)at & FIFO_MASK);
    if (dc_cut) dc_block(pcm, frames);
    for (uint32_t i = 0; i < 2u * frames; ++i) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > stat_peak) stat_peak = v;
        stat_sum += (double)pcm[i] * pcm[i];
        stat_dc[i & 1u] += pcm[i];
    }
    stat_frames += frames;
    int64_t made = (int64_t)void_monotonic_ns();
    for (uint32_t b = 0; b < frames / BLOCK; ++b)
        fifo_made_ns[(uint32_t)(at / BLOCK + b) & (FIFO_FRAMES / BLOCK - 1u)] = made;
    atomic_store_explicit(&fifo_in, at + frames, memory_order_release);
    int64_t now = (int64_t)void_monotonic_ns();
    if (!atomic_load_explicit(&primed, memory_order_relaxed) && now >= atomic_load_explicit(&grace_until, memory_order_relaxed))
        atomic_store(&primed, 1);
    int64_t posted = atomic_exchange_explicit(&demand_posted_ns, 0, memory_order_relaxed);
    /* A fill that answers the callback's post: where the time went. */
    if (posted && posted <= woke_ns && woke_ns <= machine_ns && machine_ns <= fill_ns) {
        last_response_us = 0;
        stat_max_us(&last_response_us, now - posted);
        stat_max_us(&stat_response_us, now - posted);
        stat_max_us(&stat_wake_us, woke_ns - posted);
        stat_max_us(&stat_machine_us, machine_ns - woke_ns);
        stat_max_us(&stat_lock_us, fill_ns - machine_ns);
    }
    stat_max_us(&stat_render_us, now - fill_ns);
    ++stat_renders;
}

uint32_t void_audio_pending_ms(void)
{
    uint32_t rate = out_rate;
    if (!rate || !atomic_load(&active)) return 0;
    return (uint32_t)(((uint64_t)fifo_level() + atomic_load(&device_queue)) * 1000u / rate);
}

void void_audio_wake(void)
{
    if (atomic_load(&demand_ready)) sem_post(&demand);
}

/* The game stops its mixer (to close the output, or for good): the silence
 * that follows is not the producer's lateness. */
void void_audio_producer_stopped(void)
{
    atomic_store(&primed, 0);
}

void void_audio_blocked_by(uint32_t owner, uint64_t ns)
{
    uint32_t before = stat_blocked_us;
    stat_max_us(&stat_blocked_us, (int64_t)ns);
    if (stat_blocked_us != before) stat_blocker = owner;
}
