/* Frame statistics, pacing and the aspect fit shared by the native backends.
 *
 * The statistics line is the Direct3D 9 -> GLES layer's "[gl] frame" line
 * (d3d9_gl.c d3d_gl_present) with the backend's name, so the same parser
 * (build/.../parse.py, check_long_frame_log.py) reads both: per frame the wall
 * time, the game thread's CPU time, the render thread's CPU time and its
 * waits, the machine wait, draws, the backend's present call, the cadence
 * outliers and the GPU time; plus what the native backends count (render
 * passes, streamed vertices, uploads). */
#include "s2r_internal.h"
#include "../host/sims2_settings.h"
#include <errno.h>
#include <time.h>

void void_thunk_stats(uint32_t frames) __attribute__((weak));

/* Fit `sw` x `sh` into `dw` x `dh` keeping the aspect ratio, centred. */
void s2r_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!sw || !sh || !dw || !dh) { *x = *y = 0; *w = (int32_t)dw; *h = (int32_t)dh; return; }
    double s = (double)dw / sw < (double)dh / sh ? (double)dw / sw : (double)dh / sh;
    int32_t fw = (int32_t)(sw * s + 0.5), fh = (int32_t)(sh * s + 0.5);
    /* within two pixels of the window: fill it (as d3d_gl_present) */
    if ((uint32_t)fw + 2 >= dw && (uint32_t)fh + 2 >= dh) { fw = (int32_t)dw; fh = (int32_t)dh; }
    *w = fw; *h = fh;
    *x = ((int32_t)dw - fw) / 2;
    *y = ((int32_t)dh - fh) / 2;
}

/* The Frame Rate Limit option (sims2_settings.c): each present waits for its
 * slot; a frame that comes late starts the schedule over instead of rushing
 * the next ones (d3d9_gl.c pace_frame). */
void s2r_pace(void)
{
    static uint64_t next_ns;
    int fps = s2_fps_limit();
    if (fps <= 0) { next_ns = 0; return; }
    uint64_t period = 1000000000ull / (uint64_t)fps, now = void_monotonic_ns();
    if (next_ns > now) {
        struct timespec until = { (time_t)(next_ns / 1000000000ull), (long)(next_ns % 1000000000ull) };
        d3d_rt_wait_begin();
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {}
        d3d_rt_wait_end();
        now = void_monotonic_ns();
    }
    next_ns = next_ns && now < next_ns + period / 2 ? next_ns + period : now + period;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec cpu;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    return (uint64_t)cpu.tv_sec * 1000000000ull + (uint64_t)cpu.tv_nsec;
}

/* Every 600 frames, or every 5 s when frames are slow (as d3d_gl_present). */
void s2r_frame_presented(s2r_device *d, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns)
{
    static uint64_t start, cpu_start, game_start, wait_start, syncs_start, rt_wait_start, gpu_start, gpu_frames_start;
    static uint64_t upload_start, texture_start, stream_start;
    static uint64_t cadence_last, cadence_max;
    static uint32_t cadence_frames, cadence_over90, frame_start;
    static double present_sum;
    static uint64_t draws, prims, streamed, passes, loads;
    uint64_t now = void_monotonic_ns();
    if (cadence_last) {
        uint64_t elapsed = now - cadence_last;
        if (elapsed > cadence_max) cadence_max = elapsed;
        if (elapsed > 1000000000ull / 90u) ++cadence_over90;
        ++cadence_frames;
    }
    cadence_last = now;
    present_sum += d->present_ms;
    draws += d->frame_draws;
    prims += d->frame_prims;
    streamed += d->frame_streamed;
    passes += d->frame_passes;
    loads += d->frame_loads;
    d->frame_draws = d->frame_prims = d->frame_streamed = d->frame_passes = d->frame_loads = 0;
    uint32_t frames = d->frame - frame_start;
    if (start && frames < 600 && now - start < 5000000000ull) return;
    uint64_t cpu = thread_cpu_ns();
    if (start && frames) {
        double dt = (double)(now - start) / 1e9;
        char gpu[48] = "";
        if (d->gpu_frames > gpu_frames_start)
            snprintf(gpu, sizeof gpu, "; gpu %.2f ms", (double)(d->gpu_ns - gpu_start) / 1e6 / (double)(d->gpu_frames - gpu_frames_start));
        void_log("[gl] frame %u: %.1f fps; per frame %.1f ms wall, %.1f ms cpu, %.1f ms render (%.1f ms waited, %.1f syncs), "
            "%.1f ms machine wait, %.0f draws, %.1f ms swap; cadence max %llu ns, over90 %u/%u%s; %s native: %.0f passes, "
            "%.1f loads, %.0f streamed, %.0f primitives, %.1f KiB uploads (%.1f textures, %.1f streamed)",
            d->frame, frames / dt, 1000.0 * dt / frames, (double)(game_cpu_ns - game_start) / 1e6 / frames,
            (double)(cpu - cpu_start) / 1e6 / frames, (double)(rt_wait_ns - rt_wait_start) / 1e6 / frames,
            (double)(syncs - syncs_start) / frames, (double)(game_wait_ns - wait_start) / 1e6 / frames,
            (double)draws / frames, present_sum / frames, (unsigned long long)cadence_max, cadence_over90, cadence_frames, gpu,
            d->be->name, (double)passes / frames, (double)loads / frames, (double)streamed / frames, (double)prims / frames,
            (double)(d->upload_bytes - upload_start) / 1024.0 / frames,
            (double)(d->upload_texture - texture_start) / 1024.0 / frames,
            (double)(d->upload_stream - stream_start) / 1024.0 / frames);
        if (void_thunk_stats) void_thunk_stats(frames);
    }
    start = now; cpu_start = cpu; game_start = game_cpu_ns; wait_start = game_wait_ns; syncs_start = syncs;
    rt_wait_start = rt_wait_ns; frame_start = d->frame; gpu_start = d->gpu_ns; gpu_frames_start = d->gpu_frames;
    upload_start = d->upload_bytes; texture_start = d->upload_texture; stream_start = d->upload_stream;
    present_sum = 0;
    draws = prims = streamed = passes = loads = 0;
    cadence_max = 0;
    cadence_frames = cadence_over90 = 0;
}
