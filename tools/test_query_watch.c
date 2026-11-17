/* Production COM query methods and render worker: deterministic GPU boundaries,
 * then real GLES visible/hidden/event results, with and without the worker. */
#include "../android/host/d3d9_internal.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include "../android/host/d3d9_thread.inc"

static int mock = 1;
static _Atomic unsigned fake_status = GL_TIMEOUT_EXPIRED, fail_fence, fail_watch_alloc, failed_allocs;
static unsigned fake_name, fake_fence;
static unsigned nap_calls, interrupt_nap;
static int query_nap(const struct timespec *delay, struct timespec *remaining)
{
    if (mock) {
        assert(delay->tv_sec == 0 && delay->tv_nsec > 0 && delay->tv_nsec <= 50000);
        ++nap_calls;
        if (interrupt_nap) { interrupt_nap = 0; remaining->tv_sec = 0; remaining->tv_nsec = 10000; errno = EINTR; return -1; }
    }
    return nanosleep(delay, remaining);
}
static void *watch_malloc(size_t n)
{
    if (atomic_exchange(&fail_watch_alloc, 0)) { atomic_fetch_add(&failed_allocs, 1); return NULL; }
    return malloc(n);
}
static void query_names(GLsizei n, GLuint *p)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) { glGenQueries(n, p); return; }
    while (n--) *p++ = ++fake_name;
}
static void query_delete(GLsizei n, const GLuint *p)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) glDeleteQueries(n, p);
}
static void query_begin(GLenum target, GLuint name)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) glBeginQuery(target, name);
}
static void query_end(GLenum target)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) glEndQuery(target);
}
static void query_value(GLuint name, GLenum key, GLuint *out)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) { glGetQueryObjectuiv(name, key, out); return; }
    if (key == GL_QUERY_RESULT_AVAILABLE) *out = atomic_load(&fake_status) != GL_TIMEOUT_EXPIRED;
    else { assert(key == GL_QUERY_RESULT && atomic_load(&fake_status) != GL_TIMEOUT_EXPIRED); *out = 1; }
}
static GLsync query_fence(GLenum condition, GLbitfield flags)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) return glFenceSync(condition, flags);
    return atomic_exchange(&fail_fence, 0) ? NULL : (GLsync)(uintptr_t)++fake_fence;
}
static GLenum query_wait(GLsync fence, GLbitfield flags, GLuint64 timeout)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) return glClientWaitSync(fence, flags, timeout);
    assert(fence && ((flags == 0 && timeout == 0) ||
        (flags == GL_SYNC_FLUSH_COMMANDS_BIT && timeout == 1000000)));
    return atomic_load(&fake_status);
}
static void query_delete_fence(GLsync fence)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) glDeleteSync(fence);
}
static void query_flush(void)
{
    assert(d3d_rt_on_render_thread());
    if (!mock) glFlush();
}
#undef glGenQueries
#undef glDeleteQueries
#undef glBeginQuery
#undef glEndQuery
#undef glGetQueryObjectuiv
#undef glFenceSync
#undef glClientWaitSync
#undef glDeleteSync
#undef glFlush
#define glGenQueries query_names
#define glDeleteQueries query_delete
#define glBeginQuery query_begin
#define glEndQuery query_end
#define glGetQueryObjectuiv query_value
#define glFenceSync query_fence
#define glClientWaitSync query_wait
#define glDeleteSync query_delete_fence
#define glFlush query_flush
#define malloc watch_malloc
#define nanosleep query_nap
#include "../android/host/d3d9_resource.c"
#undef malloc
#undef nanosleep

static uint32_t output;
/* The standalone runtime has no loaded game PE; PEB construction only needs its metadata. */
static uint32_t fixture_image;
uint32_t __wrap_void_module_exe_base(void) { return fixture_image; }
/* The device test owns a pbuffer, rather than waiting for an Android Activity. */
void *__wrap_void_android_window_current(uint32_t *serial, int wait)
{
    (void)wait; *serial = 0; return NULL;
}
static uint32_t issue(uint32_t guest, uint32_t flags)
{
    MEM32(g_esp + 4) = guest; MEM32(g_esp + 8) = flags;
    return qry_Issue_impl();
}
static uint32_t get(uint32_t guest, uint32_t flags)
{
    MEM32(g_esp + 4) = guest; MEM32(g_esp + 8) = output;
    MEM32(g_esp + 12) = 4; MEM32(g_esp + 16) = flags;
    return qry_GetData_impl();
}
static void release(uint32_t guest)
{
    MEM32(g_esp + 4) = guest; assert(qry_Release_impl() == 0);
}
static void run_hold(void *p) { usleep(*(unsigned *)p * 1000); }
static void hold(unsigned ms) { d3d_rt_call(run_hold, &ms, sizeof ms); d3d_rt_flush(); }
static int ready(d3d_query *q)
{
    uint64_t a = atomic_load_explicit(&QRY_RT(q)->answer, memory_order_acquire);
    return (a & ~3ull) == (QRY_RT(q)->serial << 2) && (a & 3u);
}
static void wait_unasked(d3d_query *q)
{
    uint64_t deadline = void_monotonic_ns() + 5000000000ull;
    while (!ready(q) && void_monotonic_ns() < deadline) usleep(100);
    assert(ready(q));
}
static void run_watch_count(void *p) { *(uint32_t *)p = watch_count; }
static uint32_t watched_count(void) { uint32_t n; d3d_rt_sync(run_watch_count, &n); return n; }
static uint32_t get_ready(uint32_t guest)
{
    uint64_t deadline = void_monotonic_ns() + 5000000000ull;
    uint32_t result;
    do { result = get(guest, 1); } while (result == S_FALSE_D3D && void_monotonic_ns() < deadline);
    assert(result != S_FALSE_D3D);
    return result;
}
static void run_mode(void *p) { query_watch_enabled = *(int *)p; }

static void mock_contract(d3d_device *d)
{
    uint32_t guest = d3d_query_create(d, 8);
    d3d_query *q = void_com_host(guest);
    MEM32(output) = 0xA5A55A5A;
    assert(get(guest, 0) == D3D_OK && MEM32(output) == 1); /* unissued EVENT */
    if (d3d_rt_threaded()) hold(200);
    issue(guest, 1); MEM32(output) = 0xA5A55A5A;
    uint64_t start = void_monotonic_ns();
    for (int i = 0; i < 16; ++i) assert(get(guest, i & 1) == S_FALSE_D3D && MEM32(output) == 0xA5A55A5A);
    assert(get(guest, 0) == S_FALSE_D3D);
    for (int i = 0; i < 8; ++i) assert(get(guest, 1) == S_FALSE_D3D && nap_calls == 0);
    interrupt_nap = 1;
    assert(get(guest, 1) == S_FALSE_D3D && nap_calls == 2 && MEM32(output) == 0xA5A55A5A);
    assert(void_monotonic_ns() - start < 100000000ull);
    d3d_rt_finish();
    atomic_store(&fake_status, GL_CONDITION_SATISFIED);
    if (d3d_rt_threaded()) wait_unasked(q);
    assert(get_ready(guest) == D3D_OK && MEM32(output) == 1);
    const uint32_t sizes[] = { 0, 3, 4, 16 };
    for (int has_out = 0; has_out < 2; ++has_out) for (int i = 0; i < 4; ++i) {
        MEM32(output) = 0xA5A55A5A; MEM32(g_esp + 8) = has_out ? output : 0; MEM32(g_esp + 12) = sizes[i];
        assert(qry_GetData_impl() == D3D_OK && MEM32(output) == (has_out && sizes[i] >= 4 ? 1u : 0xA5A55A5Au));
    }
    /* An old ready result cannot answer a newly issued generation. */
    atomic_store(&fake_status, GL_TIMEOUT_EXPIRED);
    if (d3d_rt_threaded()) hold(100);
    issue(guest, 1); MEM32(output) = 0xA5A55A5A;
    assert(get(guest, 0) == S_FALSE_D3D && MEM32(output) == 0xA5A55A5A);
    atomic_store(&fake_status, GL_WAIT_FAILED);
    assert(get_ready(guest) == D3DERR_NOTAVAILABLE && MEM32(output) == 0xA5A55A5A);
    atomic_store(&fail_fence, 1); issue(guest, 1);
    assert(get_ready(guest) == D3DERR_NOTAVAILABLE && MEM32(output) == 0xA5A55A5A);
    release(guest); d3d_rt_finish();
    assert(watched_count() == 0);

    if (d3d_rt_threaded()) {
        /* Hold every fake fence pending to exercise grow, failed grow and wrap. */
        enum { COUNT = 1280 };
        uint32_t objects[COUNT];
        atomic_store(&fake_status, GL_TIMEOUT_EXPIRED);
        atomic_store(&fail_watch_alloc, 1);
        for (unsigned i = 0; i < COUNT; ++i) { objects[i] = d3d_query_create(d, 8); issue(objects[i], 1); }
        d3d_rt_finish();
        assert(atomic_load(&failed_allocs) == 1 && watched_count() == COUNT - 1);
        d3d_query *missing = void_com_host(objects[1024]);
        atomic_store(&fake_status, GL_ALREADY_SIGNALED);
        wait_unasked(void_com_host(objects[COUNT - 1]));
        assert(!ready(missing));             /* allocation failure retained the explicit-poll fallback */
        assert(get_ready(objects[1024]) == D3D_OK);
        for (unsigned i = 0; i < COUNT; ++i) release(objects[i]);
        d3d_rt_finish(); assert(watched_count() == 0);
        atomic_store(&fake_status, GL_TIMEOUT_EXPIRED);
        for (int i = 0; i < 32; ++i) {
            guest = d3d_query_create(d, 8); issue(guest, 1); release(guest);
        }
        d3d_rt_finish(); assert(watched_count() == 0); /* no freed object remains watched */

        atomic_store(&fake_status, GL_ALREADY_SIGNALED);
        int off = 0, on = 1; d3d_rt_sync(run_mode, &off);
        guest = d3d_query_create(d, 8); hold(120); issue(guest, 1);
        start = void_monotonic_ns(); assert(get(guest, 1) == D3D_OK);
        assert(void_monotonic_ns() - start >= 80000000ull); /* A/B off retains the original round trip */
        release(guest); d3d_rt_finish(); d3d_rt_sync(run_mode, &on);
    } else assert(watched_count() == 0);
    puts("PASS production queries: asynchronous pending/output, unasked completion, stale generations, real error status, failed growth fallback, queue growth/wrap and release lifetime; inline queue stays empty");
}

static GLuint program, target, color;
static void run_gpu_init(void *unused)
{
    (void)unused; mock = 0;
    assert(d3d_gl_init());
    d3d_rt_apply_cores();
    const char *vs = "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}";
    const char *fs = "#version 300 es\nprecision highp float;out vec4 c;void main(){c=vec4(1.);}";
    GLuint shaders[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
    glShaderSource(shaders[0], 1, &vs, NULL); glShaderSource(shaders[1], 1, &fs, NULL);
    glCompileShader(shaders[0]); glCompileShader(shaders[1]);
    program = glCreateProgram(); glAttachShader(program, shaders[0]); glAttachShader(program, shaders[1]); glLinkProgram(program);
    GLint good; glGetProgramiv(program, GL_LINK_STATUS, &good); assert(good);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
    glGenFramebuffers(1, &target); glBindFramebuffer(GL_FRAMEBUFFER, target);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}
static void run_draw(void *p)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target); glViewport(0, 0, 32, 32); glUseProgram(program);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (*(int *)p) { glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 0, 0); }
    glDrawArrays(GL_TRIANGLES, 0, 3); glDisable(GL_SCISSOR_TEST);
}
static void run_pixel(void *p)
{
    uint8_t pixel[4]; glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(pixel[0] == (*(int *)p ? 0 : 255) && pixel[3] == 255);
    assert(glGetError() == GL_NO_ERROR);
}
static void gpu_contract(d3d_device *d)
{
    d3d_rt_sync(run_gpu_init, NULL);
    uint32_t guest = d3d_query_create(d, 9);
    d3d_query *q = void_com_host(guest);
    for (int i = 0; i < 16; ++i) {
        int hidden = i & 1;
        if (i == 4) QRY_RT(q)->serial = 0x3FFFFFFFull; /* no inherited 30-bit generation truncation */
        if (i == 8) QRY_RT(q)->serial = UINT64_MAX - 1;
        issue(guest, 2); d3d_rt_call(run_draw, &hidden, sizeof hidden); issue(guest, 1); d3d_rt_flush();
        if (d3d_rt_threaded()) wait_unasked(q);
        MEM32(output) = 0xA5A55A5A;
        assert(get_ready(guest) == D3D_OK && MEM32(output) == (hidden ? 0 : 1024));
        d3d_rt_sync(run_pixel, &hidden);
    }
    release(guest);
    guest = d3d_query_create(d, 8); q = void_com_host(guest); issue(guest, 1); d3d_rt_flush();
    if (d3d_rt_threaded()) wait_unasked(q);
    assert(get_ready(guest) == D3D_OK && MEM32(output) == 1);
    release(guest); d3d_rt_finish(); assert(watched_count() == 0);
    puts("PASS real GLES: 16 alternating visible/hidden results and pixels, repeated query generations including 30-bit boundary and uint64 wrap, actual EVENT fence; all GL on its owner");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); alarm(45);
    assert(void_guest_reserve());
    fixture_image = void_vm_alloc(0x400000, 0x2000, 0x3000u, 4u); /* reserve/commit, read/write */
    assert(fixture_image == 0x400000);
    MEM32(fixture_image + 0x3C) = 0x80;
    MEM32(fixture_image + 0x80 + 0x28) = 0x1000;
    MEM32(fixture_image + 0x80 + 0x50) = 0x2000;
    void_threads_init(); void_dispatch_init(); void_machine_enter();
    setenv("S2_RENDER_THREAD", argc > 1 ? argv[1] : "1", 1); setenv("S2_QUERY_WATCH", "1", 1);
    g_esp = void_galloc(16384) + 8192; output = void_galloc(16); assert(g_esp && output);
    d3d_rt_start();
    static d3d_device device; device.st.viewport[2] = device.st.viewport[3] = 32;
    mock_contract(&device); gpu_contract(&device);
    puts("PASS query watch complete");
    return 0;
}
