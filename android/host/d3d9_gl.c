/* The GL side of Direct3D 9: EGL, render targets, state, draws, blits, present. */
#define _GNU_SOURCE
#include "d3d9_internal.h"
#include "d3d9_texcache.h"
#include "hud.h"
#include "sims2_settings.h"
#include <errno.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include <sys/stat.h>
#include "d3d9_thread.inc"

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static int window_mode;
static int dump_every;
d3d_gl_limits_t d3d_gl_limits;
static uint32_t trace_frame;   /* defined with the tracing helpers */
static int old_mali_driver(const char *renderer, const char *version);
static void init_msaa(void);
static GLuint vao;
static GLuint upload_unit = 31;   /* texture uploads bind here, away from the sampler units */
#define FB_UNKNOWN 0xFFFFFFFFu
/* Binding caches (see bind_texture and d3d_gl_bind_targets). */
static GLuint unit_tex[32], unit_sampler[32];
static GLenum unit_target[32];
static int active_unit = -1;
static GLuint current_gl_program;
static int current_gl_program_valid;
static struct {GLuint program;GLint location,value;} ui_filters[64];

/* Both shader pipelines and ProcessVertices share one GL context. */
void d3d_gl_use_program(GLuint program)
{
    if (current_gl_program_valid && current_gl_program == program) return;
    glUseProgram(program);
    current_gl_program = program;
    current_gl_program_valid = 1;
}

void d3d_gl_delete_program(GLuint program)
{
    if (current_gl_program == program) current_gl_program_valid = 0;
    if(ui_filters[program%64].program==program)ui_filters[program%64].program=0;
    glDeleteProgram(program);
}
typedef struct { GLuint buffer; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t offset; GLuint divisor; } attr_state;
static attr_state attr_cache[16];
static GLuint elem_buffer = FB_UNKNOWN;
static int attr_cache_ready;
extern uint32_t d3d_constant_generation;

/* The Android activity provides the window; the shell build has none. */
#ifdef VOID_ANDROID_APP
#include <android/native_window.h>
void void_android_frame_presented(void);
void void_android_gpu_strings(const char *renderer, const char *version);
static void release_window(void *window) { if (window) ANativeWindow_release(window); }
#else
void void_android_frame_presented(void) {}
static void release_window(void *window) { (void)window; }
#endif
__attribute__((weak)) void *void_android_window_current(uint32_t *serial, int wait) { (void)wait; *serial = 0; return NULL; }
__attribute__((weak)) void void_android_window_ack(uint32_t serial) { (void)serial; }

/* The window surface follows the activity's (android_main.c): the context
 * stays current on a 1x1 pbuffer while there is none. */
static EGLConfig egl_config;
static EGLSurface egl_pbuffer = EGL_NO_SURFACE;
static uint32_t egl_window_serial;

static void attach_window_surface(void *window)
{
    EGLSurface next = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)window, NULL);
    release_window(window);
    if (next == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, next, next, egl_context)) {
        void_log("[gl] new window surface failed %04X", eglGetError());
        if (next != EGL_NO_SURFACE) eglDestroySurface(egl_display, next);
        return;
    }
    egl_surface = next;
    eglSwapInterval(egl_display, 1);
    void_log("[gl] presenting into a new window surface");
}

/* Take up the activity's current window if it changed since the last frame.
 * Without one (the app in the background) the game waits here, letting the
 * other guest threads run, until the activity has a surface again. */
static void follow_window(void)
{
    uint32_t serial;
    void *window = void_android_window_current(&serial, 0);
    if (serial == egl_window_serial && egl_surface != EGL_NO_SURFACE) {
        release_window(window);
        return;
    }
    eglMakeCurrent(egl_display, egl_pbuffer, egl_pbuffer, egl_context);
    if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    egl_surface = EGL_NO_SURFACE;
    if (!window) {
        void_android_window_ack(serial);
        void_log("[gl] surface gone: the game waits for the activity");
        d3d_rt_wait_begin();
        window = void_android_window_current(&serial, 1);
        d3d_rt_wait_end();
    }
    egl_window_serial = serial;
    if (window) attach_window_surface(window);
    void_android_window_ack(serial);
}

int d3d_gl_init(void)
{
    if (egl_context != EGL_NO_CONTEXT) return 1;
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(egl_display, NULL, NULL)) { void_log("[gl] eglInitialize failed %04X", eglGetError()); return 0; }
    void *window = void_android_window_current(&egl_window_serial, 1);   /* NULL in the shell build */
    window_mode = window != NULL;
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, window_mode ? EGL_WINDOW_BIT | EGL_PBUFFER_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(egl_display, attrs, &config, 1, &n) || !n) { void_log("[gl] no EGL config"); goto failed; }
    egl_config = config;
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_context == EGL_NO_CONTEXT) { void_log("[gl] eglCreateContext failed %04X", eglGetError()); goto failed; }
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    egl_pbuffer = eglCreatePbufferSurface(egl_display, config, pb);
    if (egl_pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, egl_pbuffer, egl_pbuffer, egl_context)) {
        void_log("[gl] EGL surface/current failed %04X", eglGetError());
        goto failed;
    }
    if (window_mode) attach_window_surface(window);
    else egl_surface = egl_pbuffer;
    current_gl_program_valid = 0;
    void_log("[gl] %s / %s / %s (%s)", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
        window_mode ? "window" : "pbuffer");
#ifdef VOID_ANDROID_APP
    void_android_gpu_strings((const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
#endif
    GLint value = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value); d3d_gl_limits.max_texture = (uint32_t)value;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value); d3d_gl_limits.max_3d_texture = (uint32_t)value;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &value); d3d_gl_limits.max_draw_buffers = (uint32_t)value;
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &value); d3d_gl_limits.max_vs_vectors = (uint32_t)value;
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &value); d3d_gl_limits.max_ps_vectors = (uint32_t)value;
    void_log("[gl] limits: texture %u, 3D texture %u, draw buffers %u, uniform vectors %u/%u", d3d_gl_limits.max_texture,
        d3d_gl_limits.max_3d_texture, d3d_gl_limits.max_draw_buffers, d3d_gl_limits.max_vs_vectors,
        d3d_gl_limits.max_ps_vectors);
    const char *dump = getenv("VOID_DUMP_FRAMES");
    dump_every = dump ? atoi(dump) : 0;
    const char *trace = getenv("VOID_TRACE_FRAME");
    if (trace) trace_frame = (uint32_t)strtoul(trace, NULL, 10);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &value);
    upload_unit = value > 32 ? 31u : (GLuint)(value > 21 ? value - 1 : 21);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    init_msaa();
    return 1;
failed:
    release_window(window);
    if (egl_pbuffer != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_pbuffer);
    if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
    egl_pbuffer = EGL_NO_SURFACE;
    egl_context = EGL_NO_CONTEXT;
    egl_display = EGL_NO_DISPLAY;
    return 0;
}

/* ------------------------------------------------------------- targets */
static GLuint fbo, read_fbo, draw_fbo;

/* ---------------------------------------------------------- render scale */
static float applied_scale = 0.0f;   /* the scale the registered targets have */
static float target_scale = 1.0f;    /* of the bound render target */
#define MAX_SCALED 256
static void *scaled[MAX_SCALED];
static int scaled_is_texture[MAX_SCALED], scaled_count;

/* GL pixels per D3D pixel for the screen-sized targets: the screen's pixels
 * times the render scale, over the game's (which the UI scale shrinks). */
float d3d_screen_scale(uint32_t backbuffer_width)
{
    int sw, sh, gw, gh;
    s2_physical_size(&sw, &sh);
    s2_game_resolution(&gw, &gh);
    if (backbuffer_width) gw = (int)backbuffer_width;
    float k = (float)sw / (float)(gw > 0 ? gw : sw) * (float)s2_render_scale() / 100.0f;
    /* The largest storage stays within the GL limit (and 8K). */
    uint32_t limit = d3d_gl_limits.max_texture ? d3d_gl_limits.max_texture : 8192;
    if (limit > 8192) limit = 8192;
    if ((float)gw * k > (float)limit) k = (float)limit / (float)gw;
    return k;
}

float d3d_scale_of(const d3d_surface *s)
{
    if (s->container) return s->container->scale;
    return s->scale > 0.0f ? s->scale : 1.0f;
}

uint32_t d3d_scaled_size(uint32_t size, float scale)
{
    if (scale == 1.0f) return size;
    uint32_t v = (uint32_t)((float)size * scale + 0.5f);
    return v ? v : 1;
}

static int scaled_px(uint32_t v, float k) { return k == 1.0f ? (int)v : (int)((float)(int32_t)v * k + 0.5f); }

void d3d_scaled_add(d3d_surface *s, d3d_texture *t)
{
    if (!s && !t) return;
    if (scaled_count >= MAX_SCALED) { void_log("[gl] more than %d screen-sized targets", MAX_SCALED); return; }
    scaled_is_texture[scaled_count] = t != NULL;
    scaled[scaled_count++] = t ? (void *)t : (void *)s;
    if (applied_scale == 0.0f) applied_scale = t ? t->scale : s->scale;
}

void d3d_scaled_remove(void *object)
{
    for (int i = 0; i < scaled_count; ++i) {
        if (scaled[i] != object) continue;
        scaled[i] = scaled[--scaled_count];
        scaled_is_texture[i] = scaled_is_texture[scaled_count];
        return;
    }
}

static void fb_forget_all(void);

/* At a present, after the frame went out: a changed render scale gives every
 * screen-sized target its new storage (contents lost, as after a reset; the
 * game draws the next frame whole). */
int d3d_scaled_retarget(float k)
{
    if (!scaled_count || k == applied_scale) { applied_scale = k; return 0; }
    for (int i = 0; i < scaled_count; ++i) {
        if (scaled_is_texture[i]) {
            d3d_texture *t = (d3d_texture *)scaled[i];
            t->scale = k;
            d3d_texture_storage(t);
        } else {
            d3d_surface *s = (d3d_surface *)scaled[i];
            s->scale = k;
            d3d_surface_storage(s);
        }
    }
    applied_scale = k;
    return scaled_count;
}

float d3d_scaled_applied(void) { return applied_scale; }

void d3d_scaled_follow(d3d_device *dev)
{
    float was = applied_scale, k = d3d_screen_scale(dev->pp[0]);
    int n = d3d_scaled_retarget(k);
    if (!n) return;
    void_log("[gl] render scale %.3f -> %.3f: %d targets now %ux%u", was, k, n,
        dev->backbuffer ? d3d_scaled_size(dev->backbuffer->width, k) : 0,
        dev->backbuffer ? d3d_scaled_size(dev->backbuffer->height, k) : 0);
    fb_forget_all();
    dev->fbo_dirty = 1;
}

static void attach(GLenum fb_target, GLenum attachment, d3d_surface *s)
{
    if (!s) { glFramebufferTexture2D(fb_target, attachment, GL_TEXTURE_2D, 0, 0); return; }
    GLenum target;
    GLint level, layer;
    GLuint tex = d3d_surface_gl_texture(s, &target, &level, &layer);
    if (target == GL_TEXTURE_3D) glFramebufferTextureLayer(fb_target, attachment, tex, level, layer);
    else glFramebufferTexture2D(fb_target, attachment, target, tex, level);
}

static GLenum depth_attachment(const d3d_surface *s)
{
    return s && s->fmt && s->fmt->stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
}

/* ------------------------------------------------------------- tracing
 * VOID_TRACE_FRAME=N logs every target change, clear, draw and blit of frame
 * N and dumps each render target when the frame leaves it (and the back
 * buffer at present) to <data>/frames/trace_N_SEQ.bmp, clamped to [0, 1]. */
static uint32_t trace_frame = 0xFFFFFFFFu, trace_frames = 1;
static unsigned trace_seq;
static d3d_surface *trace_rt;

static int tracing(d3d_device *d) { return d->frame - trace_frame < trace_frames; }
void d3d_shader_trace(void);

/* VOID_TRACE_DIFF: render target 0 before each traced draw, to report what
 * the draw changed. */
static uint8_t *trace_before;
static uint32_t trace_before_w, trace_before_h;

static int trace_read_rt0(d3d_device *d, uint8_t **out, uint32_t *w, uint32_t *h)
{
    d3d_surface *s = d->rt[0];
    if (!s || !s->fmt || s->fmt->is_float || s->fmt->depth) return 0;
    *w = d3d_scaled_size(s->width, d3d_scale_of(s)); *h = d3d_scaled_size(s->height, d3d_scale_of(s));
    *out = (uint8_t *)malloc((size_t)*w * *h * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, (GLsizei)*w, (GLsizei)*h, GL_RGBA, GL_UNSIGNED_BYTE, *out);
    return glGetError() == GL_NO_ERROR;
}

/* Whether the current frame is being traced (for the other D3D9 files). */
int d3d_trace_active(void) { return g_d3d_device && tracing(g_d3d_device); }

/* Trace the next `frames` frames (scripted "trace [N]"). */
void d3d_request_trace(int frames)
{
    if (!g_d3d_device) return;
    trace_frames = frames > 0 ? (uint32_t)frames : 1u;
    trace_frame = g_d3d_device->frame + 1;
}

static uint32_t shader_tag(const void *shader)
{
    if (!shader) return 0;
    const d3d_shader *sh = (const d3d_shader *)shader;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < sh->size; ++i) h = (h ^ sh->bytecode[i]) * 16777619u;
    return h;
}

static void trace_dump(d3d_device *d, d3d_surface *s, const char *why)
{
    if (!s || !s->fmt || s->fmt->depth) return;
    uint32_t w = s->width, h = s->height, rect[4] = { 0, 0, w, h };
    float *px = (float *)malloc((size_t)w * h * 16);
    if (!d3d_gl_read_rgba(s, rect, px)) {
        void_log("[trace] %s: surface %ux%u fmt %u unreadable", why, w, h, s->format);
        free(px);
        return;
    }
    float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < (size_t)w * h; ++i)
        for (int k = 0; k < 4; ++k) {
            float v = px[4 * i + k];
            if (v < lo[k]) lo[k] = v;
            if (v > hi[k]) hi[k] = v;
        }
    char path[1100];
    snprintf(path, sizeof path, "%s/frames", void_data_dir());
    mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/frames/trace_%u_%03u.bmp", void_data_dir(), d->frame, trace_seq++);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
        uint8_t header[54] = { 'B', 'M' };
        memcpy(header + 2, &size, 4);
        uint32_t v = 54; memcpy(header + 10, &v, 4);
        v = 40; memcpy(header + 14, &v, 4);
        memcpy(header + 18, &w, 4); memcpy(header + 22, &h, 4);
        header[26] = 1; header[28] = 24;
        fwrite(header, 1, 54, f);
        uint8_t zero[4] = {0};
        for (uint32_t y = h; y-- > 0;) {
            for (uint32_t x = 0; x < w; ++x) {
                const float *c = px + 4 * ((size_t)y * w + x);
                uint8_t bgr[3];
                for (int k = 0; k < 3; ++k) {
                    float t = c[2 - k];
                    bgr[k] = (uint8_t)(t <= 0.0f ? 0 : t >= 1.0f ? 255 : t * 255.0f + 0.5f);
                }
                fwrite(bgr, 1, 3, f);
            }
            fwrite(zero, 1, pad, f);
        }
        fclose(f);
    }
    void_log("[trace] %s: %ux%u fmt %u -> %s  r[%g,%g] g[%g,%g] b[%g,%g] a[%g,%g]", why, w, h, s->format, path,
        lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
    free(px);
}

/* What the render framebuffer object has attached. Re-attaching the very same
 * texture still ends a tiler's render pass (Mali stores and reloads every
 * tile), and the map set its depth surface ~290 times a frame: only real
 * changes reach GL. A deleted texture's name can come back, so deletion
 * forgets it (d3d_gl_forget_texture). */
/* EXT color textures resolve implicitly; depth renderbuffers keep samples across target changes. */
static PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC msaa_texture;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC msaa_storage;
static int msaa_samples, fb_samples;
static GLuint fb_depth_rb;

static void init_msaa(void)
{
    int requested = s2_render_msaa();
    msaa_samples = 0;
    if (requested != 2 && requested != 4) return;
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (extensions && strstr(extensions, "GL_EXT_multisampled_render_to_texture")) {
        msaa_texture = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
        msaa_storage = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
        GLint limit = 0;
        glGetIntegerv(GL_MAX_SAMPLES_EXT, &limit);
        if (msaa_texture && msaa_storage && limit >= 2) msaa_samples = requested <= limit ? requested : 2;
    }
    void_log("[gl] scene MSAA requested %dx, using %dx", requested, msaa_samples);
}

static GLuint screen_depth_msaa(d3d_surface *s, int samples)
{
    if (s->gl_msaa_samples != (uint32_t)samples) {
        if (s->gl_msaa_depth) glDeleteRenderbuffers(1, &s->gl_msaa_depth);
        glGenRenderbuffers(1, &s->gl_msaa_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, s->gl_msaa_depth);
        msaa_storage(GL_RENDERBUFFER, samples, s->fmt->internal,
            (GLsizei)d3d_scaled_size(s->width, d3d_scale_of(s)),
            (GLsizei)d3d_scaled_size(s->height, d3d_scale_of(s)));
        s->gl_msaa_samples = (uint32_t)samples;
    }
    return s->gl_msaa_depth;
}

typedef struct { GLuint tex; GLenum target; GLint level, layer; } fb_attachment;
static fb_attachment fb_color[4] = { { FB_UNKNOWN, 0, 0, 0 }, { FB_UNKNOWN, 0, 0, 0 }, { FB_UNKNOWN, 0, 0, 0 },
                                     { FB_UNKNOWN, 0, 0, 0 } };
static fb_attachment fb_depth = { FB_UNKNOWN, 0, 0, 0 };
static GLenum fb_depth_point = FB_UNKNOWN, fb_bufs[4];
static uint32_t fb_depth_w, fb_depth_h;
static GLsizei fb_buf_count = -1;

static fb_attachment attachment_of(d3d_surface *s)
{
    fb_attachment a = { 0, GL_TEXTURE_2D, 0, 0 };
    if (s) a.tex = d3d_surface_gl_texture(s, &a.target, &a.level, &a.layer);
    return a;
}

static int same_attachment(fb_attachment a, fb_attachment b)
{
    return a.tex == b.tex && a.target == b.target && a.level == b.level && a.layer == b.layer;
}

static void put_attachment(GLenum point, fb_attachment a)
{
    if (!a.tex) glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_2D, 0, 0);
    else if (fb_samples && point == GL_COLOR_ATTACHMENT0)
        msaa_texture(GL_FRAMEBUFFER, point, a.target, a.tex, a.level, fb_samples);
    else if (a.target == GL_TEXTURE_3D) glFramebufferTextureLayer(GL_FRAMEBUFFER, point, a.tex, a.level, a.layer);
    else glFramebufferTexture2D(GL_FRAMEBUFFER, point, a.target, a.tex, a.level);
}

static void fb_forget_all(void)
{
    for (int i = 0; i < 4; ++i) fb_color[i].tex = FB_UNKNOWN;
    fb_depth.tex = FB_UNKNOWN;
    fb_depth_point = FB_UNKNOWN;
    fb_buf_count = -1;
    fb_depth_rb = FB_UNKNOWN;
}

void d3d_gl_forget_texture(GLuint tex)
{
    for (int i = 0; i < 32; ++i) if (unit_tex[i] == tex) unit_tex[i] = 0;   /* deletion unbinds it everywhere */
    for (int i = 0; i < 4; ++i) if (fb_color[i].tex == tex) fb_color[i].tex = FB_UNKNOWN;
    if (fb_depth.tex == tex) { fb_depth.tex = FB_UNKNOWN; fb_depth_point = FB_UNKNOWN; }
}

void d3d_gl_bind_targets(d3d_device *dev)
{
    if (!dev->fbo_dirty) return;
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_surface *scene = dev->rt[0];
    // shortcut: scene color and standalone depth only; extend resolves before multisampling sampled depth or MRTs.
    int samples = msaa_samples && scene && (scene->container ? scene->container->screen_sized : scene->screen_sized)
        && (!scene->container || (scene->container->target == GL_TEXTURE_2D && !scene->level))
        && !dev->rt[1] && !dev->rt[2] && !dev->rt[3]
        && (!dev->ds || (dev->ds->screen_sized && !dev->ds->container)) ? msaa_samples : 0;
    if (samples != fb_samples) { fb_samples = samples; fb_forget_all(); }
    if (tracing(dev) && trace_rt != dev->rt[0]) {
        if (trace_rt) trace_dump(dev, trace_rt, "target done");
        trace_rt = dev->rt[0];
        void_log("[trace] target rt0 %p %ux%u fmt %u%s, rt1 %p, ds %p %ux%u fmt %u", (void *)dev->rt[0],
            dev->rt[0] ? dev->rt[0]->width : 0, dev->rt[0] ? dev->rt[0]->height : 0, dev->rt[0] ? dev->rt[0]->format : 0,
            dev->rt[0] == dev->backbuffer ? " (back buffer)" : "", (void *)dev->rt[1], (void *)dev->ds,
            dev->ds ? dev->ds->width : 0, dev->ds ? dev->ds->height : 0, dev->ds ? dev->ds->format : 0);
    }
    GLenum bufs[4];
    int last = 0, changed = 0;
    for (int i = 0; i < 4; ++i) {
        d3d_surface *s = dev->rt[i];
        int usable = s && s->fmt && s->fmt->internal;
        fb_attachment a = attachment_of(usable ? s : NULL);
        if (!same_attachment(a, fb_color[i])) {
            put_attachment(GL_COLOR_ATTACHMENT0 + (GLenum)i, a);
            fb_color[i] = a;
            changed = 1;
        }
        bufs[i] = usable ? GL_COLOR_ATTACHMENT0 + (GLenum)i : GL_NONE;
        if (usable) last = i + 1;
    }
    GLsizei count = last ? last : 1;
    if (count != fb_buf_count || memcmp(bufs, fb_bufs, sizeof(GLenum) * (size_t)count)) {
        glDrawBuffers(count, bufs);
        memcpy(fb_bufs, bufs, sizeof bufs);
        fb_buf_count = count;
    }
    fb_attachment z = attachment_of(dev->ds);
    GLenum point = dev->ds ? depth_attachment(dev->ds) : 0;
    d3d_surface *rt0 = dev->rt[0];
    target_scale = rt0 ? d3d_scale_of(rt0) : dev->ds ? d3d_scale_of(dev->ds) : 1.0f;
    if (!dev->ds && fb_depth_point != FB_UNKNOWN && fb_depth_point && fb_depth.tex != FB_UNKNOWN && rt0 &&
        fb_depth_w >= d3d_scaled_size(rt0->width, target_scale) && fb_depth_h >= d3d_scaled_size(rt0->height, target_scale)) {
        /* No depth surface: keep the last one attached while it covers the
         * colour target. Depth and stencil tests and clears all check d->ds,
         * so it is neither read nor written; detaching it would end the
         * render pass at every NULL <-> surface toggle (the map toggles
         * between passes ~10 times a frame). */
        z = fb_depth;
        point = fb_depth_point;
    }
    GLuint depth_rb = samples && dev->ds ? screen_depth_msaa(dev->ds, samples) : samples && point ? fb_depth_rb : 0;
    if (point != fb_depth_point || !same_attachment(z, fb_depth) || depth_rb != fb_depth_rb) {
        if (fb_depth_point == FB_UNKNOWN) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        } else if (fb_depth_point && fb_depth_point != point) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, fb_depth_point, GL_TEXTURE_2D, 0, 0);
        }
        if (point) {
            if (depth_rb) glFramebufferRenderbuffer(GL_FRAMEBUFFER, point, GL_RENDERBUFFER, depth_rb);
            else put_attachment(point, z);
        }
        fb_depth_rb = depth_rb;
        if (tracing(dev)) void_log("[trace] depth attachment %u -> %u (ds %p)", fb_depth.tex, z.tex, (void *)dev->ds);
        fb_depth = z;
        fb_depth_point = point;
        if (dev->ds) {
            fb_depth_w = d3d_scaled_size(dev->ds->width, d3d_scale_of(dev->ds));
            fb_depth_h = d3d_scaled_size(dev->ds->height, d3d_scale_of(dev->ds));
        }
        changed = 1;
    }
    GLenum status = changed ? glCheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_COMPLETE;
    GLenum msaa_error = samples && changed ? glGetError() : GL_NO_ERROR;
    if (samples && (status != GL_FRAMEBUFFER_COMPLETE || msaa_error)) {
        void_log("[gl] MSAA framebuffer unsupported (status %04X, error %04X); falling back to single-sample", status, msaa_error);
        msaa_samples = 0;
        fb_forget_all();
        d3d_gl_bind_targets(dev);
        return;
    }
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        static int warned;
        if (warned++ < 20)
            void_log("[gl] framebuffer incomplete %04X: rt0 %ux%u fmt %u, ds %ux%u fmt %u", status,
                dev->rt[0] ? dev->rt[0]->width : 0, dev->rt[0] ? dev->rt[0]->height : 0, dev->rt[0] ? dev->rt[0]->format : 0,
                dev->ds ? dev->ds->width : 0, dev->ds ? dev->ds->height : 0, dev->ds ? dev->ds->format : 0);
    }
    dev->fbo_dirty = 0;
}

/* --------------------------------------------------------------- state */
static GLenum compare(uint32_t f)
{
    static const GLenum map[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return f < 9 ? map[f] : GL_ALWAYS;
}

static GLenum blend_factor(uint32_t b, int alpha)
{
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return alpha ? GL_SRC_ALPHA : GL_SRC_COLOR;
    case 4: return alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return alpha ? GL_DST_ALPHA : GL_DST_COLOR;
    case 10: return alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    case 14: return alpha ? GL_CONSTANT_ALPHA : GL_CONSTANT_COLOR;
    case 15: return alpha ? GL_ONE_MINUS_CONSTANT_ALPHA : GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}

static GLenum blend_op(uint32_t op)
{
    switch (op) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum stencil_op(uint32_t op)
{
    static const GLenum map[] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return op < 9 ? map[op] : GL_KEEP;
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* The GL state d3d_gl_apply_state last set, to skip what did not change
 * (thirty-odd calls per draw, two thousand draws a neighborhood frame).
 * Code that sets these states directly calls d3d_gl_state_forget. */
static struct {
    int valid;
    GLboolean depth_test, depth_mask, blend, cull, stencil, scissor, offset;
    GLenum depth_func, cull_face, blend_src, blend_dst, blend_asrc, blend_adst, blend_op, blend_aop;
    uint32_t blend_color, stencil_key[10], color_masks;
    GLint scissor_box[4], viewport[4];
    float offset_slope, offset_bias, depth_range[2];
} gls;

void d3d_gl_state_forget(void) { gls.valid = 0; }

static void filter_done(d3d_device *device)
{
    glBindVertexArray(vao);current_gl_program_valid=0;active_unit=-1;
    glActiveTexture(GL_TEXTURE31);glBindSampler(31,0);glBindTexture(GL_TEXTURE_2D,0);unit_tex[31]=0;unit_sampler[31]=0;
    d3d_gl_state_forget();fb_forget_all();device->fbo_dirty=1;glBindFramebuffer(GL_FRAMEBUFFER,fbo);
}

static void set_cap(GLenum cap, GLboolean on, GLboolean *cached)
{
    if (gls.valid && *cached == on) return;
    if (on) glEnable(cap); else glDisable(cap);
    *cached = on;
}

void d3d_gl_apply_state(d3d_device *d)
{
    const uint32_t *rs = d->st.rs;
    if (!gls.valid) {
        memset(&gls, 0, sizeof gls);
        glFrontFace(GL_CCW);
    }
    /* Depth */
    set_cap(GL_DEPTH_TEST, rs[7] && d->ds, &gls.depth_test);
    GLenum depth_func = compare(rs[23]);
    if (!gls.valid || gls.depth_func != depth_func) { glDepthFunc(depth_func); gls.depth_func = depth_func; }
    GLboolean depth_mask = rs[14] ? GL_TRUE : GL_FALSE;
    if (!gls.valid || gls.depth_mask != depth_mask) { glDepthMask(depth_mask); gls.depth_mask = depth_mask; }
    /* Blend */
    set_cap(GL_BLEND, rs[27] != 0, &gls.blend);
    if (rs[27]) {
        uint32_t src = rs[19], dst = rs[20];
        if (src == 12) { src = 5; dst = 6; } else if (src == 13) { src = 6; dst = 5; }
        uint32_t asrc = src, adst = dst, aop = rs[171];
        if (rs[206]) {
            asrc = rs[207]; adst = rs[208]; aop = rs[209];
            if (asrc == 12) { asrc = 5; adst = 6; } else if (asrc == 13) { asrc = 6; adst = 5; }
        }
        GLenum bs = blend_factor(src, 0), bd = blend_factor(dst, 0), bas = blend_factor(asrc, 1), bad = blend_factor(adst, 1);
        if (!gls.valid || gls.blend_src != bs || gls.blend_dst != bd || gls.blend_asrc != bas || gls.blend_adst != bad) {
            glBlendFuncSeparate(bs, bd, bas, bad);
            gls.blend_src = bs; gls.blend_dst = bd; gls.blend_asrc = bas; gls.blend_adst = bad;
        }
        GLenum eq = blend_op(rs[171]), aeq = blend_op(aop);
        if (!gls.valid || gls.blend_op != eq || gls.blend_aop != aeq) {
            glBlendEquationSeparate(eq, aeq);
            gls.blend_op = eq; gls.blend_aop = aeq;
        }
        uint32_t bf = rs[193];
        if (!gls.valid || gls.blend_color != bf) {
            glBlendColor(((bf >> 16) & 255) / 255.0f, ((bf >> 8) & 255) / 255.0f, (bf & 255) / 255.0f, (bf >> 24) / 255.0f);
            gls.blend_color = bf;
        }
    }
    /* Cull: D3D clockwise is GL counter-clockwise in the flipped targets
     * (glFrontFace(GL_CCW) is set once, when the cache starts over). */
    set_cap(GL_CULL_FACE, rs[22] != 1, &gls.cull);
    if (rs[22] != 1) {
        GLenum face = rs[22] == 2 ? GL_FRONT : GL_BACK;
        if (!gls.valid || gls.cull_face != face) { glCullFace(face); gls.cull_face = face; }
    }
    /* Stencil */
    int stencil = rs[52] && d->ds && d->ds->fmt && d->ds->fmt->stencil;
    set_cap(GL_STENCIL_TEST, stencil != 0, &gls.stencil);
    if (stencil) {
        uint32_t key[10] = { rs[56], rs[57], rs[58], rs[53], rs[54], rs[55], rs[185], rs[185] ? rs[189] : 0,
                             rs[185] ? (rs[186] | rs[187] << 8 | rs[188] << 16) : 0, rs[59] };
        if (!gls.valid || memcmp(gls.stencil_key, key, sizeof key)) {
            glStencilFuncSeparate(GL_FRONT, compare(rs[56]), (GLint)rs[57], rs[58]);
            glStencilOpSeparate(GL_FRONT, stencil_op(rs[53]), stencil_op(rs[54]), stencil_op(rs[55]));
            if (rs[185]) {
                glStencilFuncSeparate(GL_BACK, compare(rs[189]), (GLint)rs[57], rs[58]);
                glStencilOpSeparate(GL_BACK, stencil_op(rs[186]), stencil_op(rs[187]), stencil_op(rs[188]));
            } else {
                glStencilFuncSeparate(GL_BACK, compare(rs[56]), (GLint)rs[57], rs[58]);
                glStencilOpSeparate(GL_BACK, stencil_op(rs[53]), stencil_op(rs[54]), stencil_op(rs[55]));
            }
            glStencilMask(rs[59]);
            memcpy(gls.stencil_key, key, sizeof key);
        }
    }
    /* Colour write masks */
    static const int mask_state[4] = { 168, 190, 191, 192 };
    uint32_t masks = (rs[168] & 15) | (rs[190] & 15) << 4 | (rs[191] & 15) << 8 | (rs[192] & 15) << 12;
    if (!gls.valid || gls.color_masks != masks) {
        for (int i = 0; i < 4; ++i) {
            uint32_t m = rs[mask_state[i]];
            glColorMaski((GLuint)i, (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
        }
        gls.color_masks = masks;
    }
    /* Scissor and viewport, in the target's GL pixels (render scale). */
    float k = target_scale;
    set_cap(GL_SCISSOR_TEST, rs[174] != 0, &gls.scissor);
    if (rs[174]) {
        GLint box[4] = { scaled_px(d->st.scissor[0], k), scaled_px(d->st.scissor[1], k),
                         scaled_px(d->st.scissor[0] + d->st.scissor[2], k) - scaled_px(d->st.scissor[0], k),
                         scaled_px(d->st.scissor[1] + d->st.scissor[3], k) - scaled_px(d->st.scissor[1], k) };
        if (!gls.valid || memcmp(gls.scissor_box, box, sizeof box)) {
            glScissor(box[0], box[1], box[2], box[3]);
            memcpy(gls.scissor_box, box, sizeof box);
        }
    }
    /* Depth bias */
    float bias = u2f(rs[195]), slope = u2f(rs[175]);
    set_cap(GL_POLYGON_OFFSET_FILL, bias != 0.0f || slope != 0.0f, &gls.offset);
    if (bias != 0.0f || slope != 0.0f) {
        float scale = (d->ds && d->ds->fmt && d->ds->fmt->bits == 16) ? 65535.0f : 16777215.0f;
        if (!gls.valid || gls.offset_slope != slope || gls.offset_bias != bias * scale) {
            glPolygonOffset(slope, bias * scale);
            gls.offset_slope = slope; gls.offset_bias = bias * scale;
        }
    }
    /* Viewport */
    GLint vp[4] = { scaled_px(d->st.viewport[0], k), scaled_px(d->st.viewport[1], k),
                    scaled_px(d->st.viewport[0] + d->st.viewport[2], k) - scaled_px(d->st.viewport[0], k),
                    scaled_px(d->st.viewport[1] + d->st.viewport[3], k) - scaled_px(d->st.viewport[1], k) };
    if (!gls.valid || memcmp(gls.viewport, vp, sizeof vp)) {
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        memcpy(gls.viewport, vp, sizeof vp);
    }
    if (!gls.valid || gls.depth_range[0] != d->st.viewport_z[0] || gls.depth_range[1] != d->st.viewport_z[1]) {
        glDepthRangef(d->st.viewport_z[0], d->st.viewport_z[1]);
        gls.depth_range[0] = d->st.viewport_z[0]; gls.depth_range[1] = d->st.viewport_z[1];
    }
    gls.valid = 1;
}

/* ------------------------------------------------------------- samplers */
typedef struct { uint32_t key[9]; GLuint sampler; } sampler_entry;
static sampler_entry sampler_cache[512];
static int sampler_count;

static GLenum address(uint32_t a)
{
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER_EXT;
    case 5: return GL_MIRRORED_REPEAT;          /* MIRRORONCE: closest available */
    default: return GL_REPEAT;
    }
}

static GLuint sampler_for(const uint32_t *ss, int no_filter, uint32_t levels, uint32_t lod)
{
    uint32_t key[9] = { ss[1], ss[2], ss[3], ss[4], no_filter ? 1 : ss[5], no_filter ? 1 : ss[6],
        no_filter ? (ss[7] ? 1u : 0u) : ss[7], ss[10], ss[9] > lod ? ss[9] : lod };
    (void)levels;
    for (int i = 0; i < sampler_count; ++i) if (!memcmp(sampler_cache[i].key, key, sizeof key)) return sampler_cache[i].sampler;
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)address(key[0]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)address(key[1]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, (GLint)address(key[2]));
    uint32_t bc = key[3];
    GLfloat border[4] = { ((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f };
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR_EXT, border);
    uint32_t mag = key[4], min = key[5], mip = key[6];
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag >= 2 ? GL_LINEAR : GL_NEAREST);
    GLenum minf;
    int linear = min >= 2;
    if (mip == 0) minf = linear ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) minf = linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    float aniso = (min == 3 || mag == 3) ? (float)(key[7] ? key[7] : 1) : 1.0f;
    if (aniso > 16.0f) aniso = 16.0f;
    glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)key[8]);
    if (sampler_count < 512) {
        memcpy(sampler_cache[sampler_count].key, key, sizeof key);
        sampler_cache[sampler_count++].sampler = s;
    }
    return s;
}

/* What each texture unit and vertex attribute holds, so a draw only sends GL
 * the bindings that changed: Mali revalidates at the draw for every binding
 * call, changed or not. Texture uploads bind on a unit of their own
 * (d3d_gl_upload_unit); deleted textures and buffers are forgotten. */

static void activate_unit(GLuint unit)
{
    if ((int)unit == active_unit) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    active_unit = (int)unit;
}

void d3d_gl_upload_unit(void) { activate_unit(upload_unit); }

void d3d_gl_forget_buffer(GLuint buffer)
{
    for (int i = 0; i < 16; ++i) if (attr_cache[i].buffer == buffer) attr_cache[i].buffer = FB_UNKNOWN;
    if (elem_buffer == buffer) elem_buffer = FB_UNKNOWN;
}

static void bind_texture(d3d_device *d, int slot, GLuint unit)
{
    d3d_texture *t = (d3d_texture *)d->st.textures[slot];
    if (!t || !t->gl) {
        if (unit_tex[unit]) {
            activate_unit(unit);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            unit_tex[unit] = 0;
        }
        return;
    }
    if (t->mips_dirty) d3d_generate_mips(t);
    if (unit_tex[unit] != t->gl || unit_target[unit] != t->target) {
        activate_unit(unit);
        glBindTexture(t->target, t->gl);
        unit_tex[unit] = t->gl;
        unit_target[unit] = t->target;
    }
    int no_filter = t->fmt->depth || (t->fmt->is_float && t->fmt->gl_bits / (t->fmt->format == GL_RED ? 1 : t->fmt->format == GL_RG ? 2 : 4) >= 32);
    if (t->levels < 2) {
        /* No mip chain: D3D samples level 0 whatever MIPFILTER says. */
        uint32_t ss[14];
        memcpy(ss, d->st.ss[slot], sizeof ss);
        ss[7] = 0;
        GLuint sampler = sampler_for(ss, no_filter, t->levels, t->gl_lod);
        if (unit_sampler[unit] != sampler) { glBindSampler(unit, sampler); unit_sampler[unit] = sampler; }
    } else {
        GLuint sampler = sampler_for(d->st.ss[slot], no_filter, t->levels, t->gl_lod);
        if (unit_sampler[unit] != sampler) { glBindSampler(unit, sampler); unit_sampler[unit] = sampler; }
    }
}

/* ------------------------------------------------------------ attributes */
typedef struct { GLint size; GLenum type; GLboolean norm; } attr_format;
static const attr_format attr_formats[] = {
    {1, GL_FLOAT, GL_FALSE}, {2, GL_FLOAT, GL_FALSE}, {3, GL_FLOAT, GL_FALSE}, {4, GL_FLOAT, GL_FALSE},
    {4, GL_UNSIGNED_BYTE, GL_TRUE}, {4, GL_UNSIGNED_BYTE, GL_FALSE}, {2, GL_SHORT, GL_FALSE}, {4, GL_SHORT, GL_FALSE},
    {4, GL_UNSIGNED_BYTE, GL_TRUE}, {2, GL_SHORT, GL_TRUE}, {4, GL_SHORT, GL_TRUE}, {2, GL_UNSIGNED_SHORT, GL_TRUE},
    {4, GL_UNSIGNED_SHORT, GL_TRUE}, {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE}, {4, GL_INT_2_10_10_10_REV, GL_TRUE},
    {2, GL_HALF_FLOAT, GL_FALSE}, {4, GL_HALF_FLOAT, GL_FALSE},
};

static const d3d_vertex_element *find_element(d3d_device *d, int usage, int index)
{
    d3d_decl *decl = (d3d_decl *)d->st.decl;
    if (!decl) return NULL;
    for (int i = 0; i < decl->count; ++i)
        if (decl->elements[i].usage == usage && decl->elements[i].usage_index == index && decl->elements[i].type < 17)
            return &decl->elements[i];
    /* POSITIONT feeds a POSITION input, as for pre-transformed vertices. */
    if (usage == 0) for (int i = 0; i < decl->count; ++i)
        if (decl->elements[i].usage == 9 && decl->elements[i].usage_index == index) return &decl->elements[i];
    return NULL;
}

static int bgra_of(d3d_device *d, int usage, int index)
{
    const d3d_vertex_element *e = find_element(d, usage, index);
    return e && e->type == 4;
}

/* ProcessVertices uses a private VAO, leaving the draw attribute cache intact.
 * Validate with 64-bit offsets before asking GL to read a guest stream. */
/* The native renderers have no GL buffers of the game's: ProcessVertices reads the vertex range
   from the guest copies through buffers of its own, deleted by d3d_process_inputs_done. */
static GLuint process_temp[16];
static int process_temp_count;

void d3d_process_inputs_done(void)
{
    if (process_temp_count) glDeleteBuffers(process_temp_count, process_temp);
    process_temp_count = 0;
}

int d3d_process_inputs(d3d_device *d, const MOJOSHADER_parseData *pd, uint32_t first, uint32_t count, int bind)
{
    static const uint8_t bytes[] = {4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
    if (pd->attribute_count > 16) return 0;
    for (int i = 0; i < pd->attribute_count; ++i) {
        const MOJOSHADER_attribute *a = &pd->attributes[i];
        const d3d_vertex_element *e = find_element(d, a->usage, a->index);
        d3d_buffer *b = e && e->stream < 16 ? (d3d_buffer *)d->st.streams[e->stream] : NULL;
        if (!e || !b || e->method || e->type >= 17) return 0;
        uint32_t stride = d->st.stream_stride[e->stream];
        /* D3D stride zero repeats one element; GLES zero advances packed
         * elements. Add constant attributes here if the game uses these
         * streams; until then reject a multi-vertex call before GL reads. */
        if (!stride && count > 1) return 0;
        uint64_t offset = (uint64_t)d->st.stream_offset[e->stream] + e->offset + (uint64_t)first * stride;
        uint64_t end = offset + (count ? (uint64_t)(count - 1) * stride + bytes[e->type] : 0);
        if (end > b->size || stride > INT32_MAX) return 0;
        if (bind) {
            const attr_format *f = &attr_formats[e->type];
            if (s2r_on()) {
                if (!b->shadow || process_temp_count >= 16) return 0;
                GLuint temp;
                glGenBuffers(1, &temp);
                process_temp[process_temp_count++] = temp;
                glBindBuffer(GL_ARRAY_BUFFER, temp);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(end - offset), GPTR(b->shadow + (uint32_t)offset), GL_STREAM_DRAW);
                offset = 0;
            } else glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            glVertexAttribPointer((GLuint)i, f->size, f->type, f->norm, (GLsizei)stride, (const void *)(uintptr_t)offset);
            glEnableVertexAttribArray((GLuint)i);
        }
    }
    return 1;
}

int d3d_shader_bind2(d3d_device *dev, d3d_shader *vs, d3d_shader *ps, int (*bgra)(d3d_device *, int, int),
    int *attr_count, const int **attr_usage, const int **attr_index, uint32_t *ps_samplers, uint32_t *vs_samplers,
    const int **ps_types, const int **vs_types);
void d3d_shader_upload(d3d_device *dev, float half_x, float half_y, int alpha_func, float alpha_ref);

static GLuint up_vbo, up_ibo;

/* UP draws (DrawPrimitiveUP/DrawIndexedPrimitiveUP) go to one persistently mapped buffer
 * (GL_EXT_buffer_storage): RING_SEGMENTS segments, one per frame in flight, each reused only
 * after the fence of the frame that last wrote it. A glBufferData per draw made the Adreno
 * driver allocate and free GPU memory with an ioctl each (UT3 2026-11-04: a third of the
 * frame's CPU time in the kernel). A frame that outgrows its segment uploads the old way. */
#define RING_SEGMENTS 3u
#define RING_SEGMENT_BYTES (8u << 20)
static GLuint ring_buffer;
static uint8_t *ring_map;
static uint32_t ring_segment, ring_used;
static GLsync ring_fence[RING_SEGMENTS];
static int ring_state;                   /* 0 untried, 1 mapped, -1 unavailable */

static int ring_ready(void)
{
    if (ring_state) return ring_state > 0;
    ring_state = -1;
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "GL_EXT_buffer_storage") || getenv("VOID_NO_RING")) return 0;
    PFNGLBUFFERSTORAGEEXTPROC storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
    if (!storage) return 0;
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    GLsizeiptr bytes = (GLsizeiptr)RING_SEGMENTS * RING_SEGMENT_BYTES;
    glGenBuffers(1, &ring_buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ring_buffer);
    storage(GL_COPY_WRITE_BUFFER, bytes, NULL, flags);
    ring_map = (uint8_t *)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, bytes, flags);
    if (!ring_map) {
        void_log("[gl] streaming ring unavailable (%04X)", glGetError());
        glDeleteBuffers(1, &ring_buffer);
        ring_buffer = 0;
        return 0;
    }
    ring_segment = ring_used = 0;
    ring_state = 1;
    void_log("[gl] streaming ring: %u x %u MiB", RING_SEGMENTS, RING_SEGMENT_BYTES >> 20);
    return 1;
}

/* Room for `bytes` in this frame's segment, 16-byte aligned, or NULL. */
static uint8_t *ring_alloc(uint32_t bytes, uint32_t *offset)
{
    if (!bytes || !ring_ready()) return NULL;
    uint32_t at = (ring_used + 15u) & ~15u;
    if ((uint64_t)at + bytes > RING_SEGMENT_BYTES) return NULL;
    ring_used = at + bytes;
    *offset = ring_segment * RING_SEGMENT_BYTES + at;
    return ring_map + *offset;
}

/* After a frame's last draw: fence its segment, move to the next one and wait
 * for the GPU to finish the frame that used it RING_SEGMENTS frames ago. */
static void ring_frame_end(void)
{
    if (ring_state <= 0) return;
    if (ring_fence[ring_segment]) glDeleteSync(ring_fence[ring_segment]);
    ring_fence[ring_segment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!ring_fence[ring_segment]) {
        void_log("[gl] streaming ring fence creation failed %04X; using uploads", glGetError());
        ring_state = -1;
        return;
    }
    ring_segment = (ring_segment + 1u) % RING_SEGMENTS;
    ring_used = 0;
    GLsync fence = ring_fence[ring_segment];
    if (!fence) return;
    for (int waits = 0;; ++waits) {
        GLenum result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) break;
        if (result == GL_WAIT_FAILED) {
            void_log("[gl] streaming ring fence wait failed %04X; using uploads", glGetError());
            ring_state = -1;        /* Never overwrite a segment whose GPU completion is unknown. */
            return;
        }
        if (waits < 3) void_log("[gl] streaming ring: GPU still on segment %u after %d s", ring_segment, waits + 1);
    }
    glDeleteSync(fence);
    ring_fence[ring_segment] = 0;
}
static uint32_t enabled_attribs;

static GLenum gl_mode(uint32_t p)
{
    switch (p) {
    case 1: return GL_POINTS;
    case 2: return GL_LINES;
    case 3: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLE_STRIP;
    case 6: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

/* A traced skinned draw (BLENDINDICES in the declaration) as the vertex
 * shader sees it, for tools/skin_replay.py: <data>/glsl/draw_<frame>_<n>.bin
 * holds the declaration, the streams' strides and offsets, the base vertex,
 * the indices, the vertices they span in each stream and the 256 vertex
 * shader float constants. */
static void trace_dump_skinned(d3d_device *d, uint32_t base_vertex, uint32_t start_index, uint32_t index_count)
{
    static uint32_t dumped, dumped_frame;
    d3d_decl *decl = (d3d_decl *)d->st.decl;
    d3d_buffer *ib = (d3d_buffer *)d->st.indices;
    if (!decl || !ib || !index_count) return;
    int skinned = 0;
    for (int i = 0; i < decl->count; ++i) if (decl->elements[i].usage == 2 && decl->elements[i].type < 17) skinned = 1;
    if (!skinned) return;
    if (dumped_frame != d->frame) { dumped_frame = d->frame; dumped = 0; }
    if (dumped >= 16) return;
    int i32 = ib->format == 102;
    uint32_t *indices = (uint32_t *)malloc((size_t)index_count * 4u), lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t v = i32 ? MEM32(ib->shadow + (start_index + i) * 4u) : MEM16(ib->shadow + (start_index + i) * 2u);
        indices[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    char path[1100];
    snprintf(path, sizeof path, "%s/glsl/draw_%u_%u.bin", void_data_dir(), d->frame, dumped++);
    FILE *f = fopen(path, "wb");
    if (!f) { free(indices); return; }
    uint32_t header[4] = { FOURCC('S','K','D','1'), (uint32_t)decl->count, base_vertex, index_count };
    fwrite(header, 4, 4, f);
    for (int i = 0; i < decl->count; ++i) {
        const d3d_vertex_element *e = &decl->elements[i];
        uint8_t row[8] = { (uint8_t)e->stream, e->type, e->usage, e->usage_index, (uint8_t)e->offset, (uint8_t)(e->offset >> 8), 0, 0 };
        fwrite(row, 1, 8, f);
    }
    uint32_t streams[4][3];
    for (int s = 0; s < 4; ++s) {
        streams[s][0] = d->st.streams[s] ? 1u : 0u;
        streams[s][1] = d->st.stream_stride[s];
        streams[s][2] = d->st.stream_offset[s];
    }
    fwrite(streams, 4, 12, f);
    fwrite(indices, 4, index_count, f);
    uint32_t range[2] = { lo, hi };
    fwrite(range, 4, 2, f);
    for (int s = 0; s < 4; ++s) {
        if (!streams[s][0]) continue;
        d3d_buffer *b = (d3d_buffer *)d->st.streams[s];
        uint32_t stride = streams[s][1], from = streams[s][2] + (base_vertex + lo) * stride, bytes = (hi - lo + 1) * stride;
        if (from + bytes > b->size) bytes = from < b->size ? b->size - from : 0;
        fwrite(&bytes, 4, 1, f);
        if (bytes) fwrite(GPTR(b->shadow + from), 1, bytes, f);
    }
    fwrite(d->st.vs_f, 4, 256 * 4, f);
    fclose(f);
    free(indices);
    void_log("[trace]   skinned draw dumped to %s (vertices %u..%u)", path, lo, hi);
}

static uint64_t draw_calls;   /* for the per-frame statistics */
static uint64_t swap_ns;      /* time in eglSwapBuffers: vsync and the GPU catching up */

/* A diagnostic read must not remap the storage a queued vertex draw uses. */
static int trace_buffer_word(GLuint buffer, uintptr_t offset, uint32_t *word)
{
    static GLuint scratch;
    GLint old_read = 0, old_write = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &old_read);
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &old_write);
    if (!scratch) {
        glGenBuffers(1, &scratch);
        glBindBuffer(GL_COPY_WRITE_BUFFER, scratch);
        glBufferData(GL_COPY_WRITE_BUFFER, sizeof *word, NULL, GL_STREAM_READ);
    } else glBindBuffer(GL_COPY_WRITE_BUFFER, scratch);
    glBindBuffer(GL_COPY_READ_BUFFER, buffer);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)offset, 0, sizeof *word);
    glBindBuffer(GL_COPY_READ_BUFFER, scratch);
    const void *mapped = glMapBufferRange(GL_COPY_READ_BUFFER, 0, sizeof *word, GL_MAP_READ_BIT);
    int ok = mapped != NULL;
    if (mapped) { memcpy(word, mapped, sizeof *word); ok = glUnmapBuffer(GL_COPY_READ_BUFFER) == GL_TRUE; }
    glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)old_read);
    glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)old_write);
    return ok;
}

int d3d_gl_draw(d3d_device *d, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count,
    uint32_t prim_count, int indexed, uint32_t base_vertex, uint32_t start_index, uint32_t min_index,
    const void *up_vertices, uint32_t up_stride, const void *up_indices, int up_index32)
{
    (void)prim_count; (void)min_index;
    ++draw_calls;
    d3d_shader *vs = (d3d_shader *)d->st.vs, *ps = (d3d_shader *)d->st.ps;
    /* POSITIONT vertices are already transformed: D3D9 runs no vertex shader
     * for them (Wine: use_vs() is false for a position_transformed
     * declaration), only the fixed-function pass-through. */
    if (vs) {
        d3d_decl *decl = (d3d_decl *)d->st.decl;
        for (int i = 0; decl && i < decl->count; ++i)
            if (decl->elements[i].usage == 9 && decl->elements[i].usage_index == 0 && decl->elements[i].type < 17) {
                vs = NULL;
                break;
            }
    }
    d3d_gl_bind_targets(d);
    d3d_gl_apply_state(d);
    int attr_count;
    const int *attr_usage, *attr_index, *ps_types, *vs_types;
    uint32_t ps_samplers, vs_samplers;
    /* No shaders: the generated fixed-function pipeline. One shader: the
     * other half is generated inside d3d_shader_bind2. */
    if (!vs && !ps) {
        static int ff_usage[16], ff_index[16];
        if (!d3d_ff_bind(d, &attr_count, ff_usage, ff_index, &ps_samplers)) return 0;
        attr_usage = ff_usage;
        attr_index = ff_index;
        vs_samplers = 0;
    } else {
        if (!d3d_shader_bind2(d, vs, ps, bgra_of, &attr_count, &attr_usage, &attr_index, &ps_samplers, &vs_samplers,
                              &ps_types, &vs_types))
            return 0;
        float hx = d->st.viewport[2] ? 1.0f / (float)d->st.viewport[2] : 0.0f;
        float hy = d->st.viewport[3] ? 1.0f / (float)d->st.viewport[3] : 0.0f;
        int alpha_func = d->st.rs[15] ? (int)d->st.rs[25] : 8;
        d3d_shader_upload(d, hx, hy, alpha_func, (float)(d->st.rs[24] & 0xFF));
    }
    if(s2_ui_fsr()){
        unsigned slot=current_gl_program%64;
        if(ui_filters[slot].program!=current_gl_program){
            ui_filters[slot].program=current_gl_program;ui_filters[slot].location=glGetUniformLocation(current_gl_program,"vpUiFilter");ui_filters[slot].value=-99;
        }
        int value=d->ui_draw?3:-1;
        if(ui_filters[slot].location>=0&&ui_filters[slot].value!=value){glUniform1i(ui_filters[slot].location,value);ui_filters[slot].value=value;}
    }
    uint32_t hud = 0;
    int picture = 0;
    for (int s = 0; s < 16; ++s) {
        if (!(ps_samplers & (1u << s))) continue;
        bind_texture(d, s, (GLuint)s);
        if (d->st.textures[s]) {
            d3d_texture *texture = (d3d_texture *)d->st.textures[s];
            hud |= texture->hud_tag;
            if (texture->width >= 128 && texture->height >= 128) picture = 1;
        }
    }
    void_hud_draw(hud | (picture && d->rt[0] == d->backbuffer ? 0x80000000u : 0));
    for (int s = 0; s < 4; ++s) if (vs_samplers & (1u << s)) bind_texture(d, 16 + s, (GLuint)(16 + s));
    /* UP data */
    uint32_t up_bytes = 0, up_ring_at = 0;
    GLuint up_buffer = 0;
    if (up_vertices) {
        uint32_t vertices = vertex_count;
        if (indexed && up_indices) {
            uint32_t max = 0;
            for (uint32_t i = 0; i < vertex_count; ++i) {
                uint32_t v = up_index32 ? ((const uint32_t *)up_indices)[i] : ((const uint16_t *)up_indices)[i];
                if (v > max) max = v;
            }
            vertices = max + 1;
        }
        up_bytes = vertices * up_stride;
        /* UP data through the streaming ring: a glBufferData per draw made the Adreno driver
         * allocate (and later free) GPU memory with an ioctl each, a third of the frame's CPU
         * time in kernel calls on sprite- and particle-heavy scenes (UT3, 2026-11-04). */
        uint8_t *dst = ring_alloc(up_bytes, &up_ring_at);
        if (dst) {
            memcpy(dst, up_vertices, up_bytes);
            up_buffer = ring_buffer;
        } else {
            up_ring_at = 0;
            if (!up_vbo) glGenBuffers(1, &up_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, up_vbo);
            glBufferData(GL_ARRAY_BUFFER, up_bytes, up_vertices, GL_STREAM_DRAW);
            up_buffer = up_vbo;
        }
    }
    uint32_t instances = (d->st.stream_freq[0] & 0x40000000u) ? (d->st.stream_freq[0] & 0x3FFFFFFFu) : 0;
    uint32_t want = 0;
    if (!attr_cache_ready) {
        for (int i = 0; i < 16; ++i) attr_cache[i].buffer = FB_UNKNOWN;
        attr_cache_ready = 1;
    }
    for (int i = 0; i < attr_count; ++i) {
        const d3d_vertex_element *e = find_element(d, attr_usage[i], attr_index[i]);
        if (!e) {
            if (attr_usage[i] == 10) glVertexAttrib4f((GLuint)i, 1, 1, 1, 1);
            else glVertexAttrib4f((GLuint)i, 0, 0, 0, 1);
            continue;
        }
        const attr_format *f = &attr_formats[e->type];
        uintptr_t offset;
        GLsizei stride;
        GLuint buffer;
        if (up_vertices && e->stream == 0) {
            buffer = up_buffer;
            offset = (uintptr_t)up_ring_at + e->offset;
            stride = (GLsizei)up_stride;
        } else {
            d3d_buffer *b = (d3d_buffer *)d->st.streams[e->stream];
            if (!b) { glVertexAttrib4f((GLuint)i, 0, 0, 0, 1); continue; }
            buffer = b->gl;
            offset = d->st.stream_offset[e->stream] + e->offset;
            stride = (GLsizei)d->st.stream_stride[e->stream];
        }
        attr_state *c = &attr_cache[i];
        if (c->buffer != buffer || c->size != f->size || c->type != f->type || c->norm != f->norm || c->stride != stride ||
            c->offset != offset) {
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glVertexAttribPointer((GLuint)i, f->size, f->type, f->norm, stride, (const void *)offset);
            c->buffer = buffer; c->size = f->size; c->type = f->type; c->norm = f->norm; c->stride = stride; c->offset = offset;
        }
        GLuint divisor = (instances && (d->st.stream_freq[e->stream] & 0x80000000u)) ? 1u : 0u;
        if (c->divisor != divisor) { glVertexAttribDivisor((GLuint)i, divisor); c->divisor = divisor; }
        want |= 1u << i;
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t bit = 1u << i;
        if ((want & bit) && !(enabled_attribs & bit)) glEnableVertexAttribArray((GLuint)i);
        if (!(want & bit) && (enabled_attribs & bit)) glDisableVertexAttribArray((GLuint)i);
    }
    enabled_attribs = want;
    GLuint trace_query = 0;
    if (tracing(d)) {
        /* The first vertex as the draw reads it, and the transform constants. */
        const d3d_vertex_element *pe = find_element(d, 0, 0);
        uint32_t index = start_vertex;
        if (indexed && up_indices) {
            index = up_index32 ? ((const uint32_t *)up_indices)[0] : ((const uint16_t *)up_indices)[0];
            index += base_vertex;
        } else if (indexed && d->st.indices) {
            d3d_buffer *ib = (d3d_buffer *)d->st.indices;
            index = ib->format == 102 ? MEM32(ib->shadow + start_index * 4u) : MEM16(ib->shadow + start_index * 2u);
            index += base_vertex;
        }
        float pos[4] = { 0, 0, 0, 0 };
        if (pe && pe->type <= 3) {
            const uint8_t *vb = NULL;
            uint32_t stride = 0;
            if (up_vertices && pe->stream == 0) { vb = (const uint8_t *)up_vertices; stride = up_stride; }
            else if (d->st.streams[pe->stream]) {
                d3d_buffer *b = (d3d_buffer *)d->st.streams[pe->stream];
                vb = (const uint8_t *)GPTR(b->shadow + d->st.stream_offset[pe->stream]);
                stride = d->st.stream_stride[pe->stream];
            }
            if (vb) memcpy(pos, vb + (size_t)index * stride + pe->offset, 4u * (pe->type + 1u));
        }
        const float *c = &d->st.vs_f[0][0];
        void_log("[trace]   v%u pos (%g %g %g %g) usage %u type %d; c0 (%g %g %g %g) c1 (%g %g %g %g) c2 (%g %g %g %g) c3 (%g %g %g %g)",
            index, pos[0], pos[1], pos[2], pos[3], pe ? pe->usage : 99, pe ? pe->type : -1, c[0], c[1], c[2], c[3], c[4], c[5],
            c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        const d3d_vertex_element *ce = find_element(d, 10, 0);
        if (ce && (ce->type == 4 || ce->type == 5 || ce->type == 8)) {
            const uint8_t *colors = NULL;
            uint32_t stride = 0;
            if (up_vertices && ce->stream == 0) { colors = up_vertices; stride = up_stride; }
            else if (d->st.streams[ce->stream]) {
                d3d_buffer *b = (d3d_buffer *)d->st.streams[ce->stream];
                colors = GPTR(b->shadow + d->st.stream_offset[ce->stream]);
                stride = d->st.stream_stride[ce->stream];
            }
            if (colors) {
                uint32_t raw, gpu = 0;
                memcpy(&raw, colors + (size_t)index * stride + ce->offset, sizeof raw);
                GLuint buffer = up_vertices && ce->stream == 0 ? up_buffer : ((d3d_buffer *)d->st.streams[ce->stream])->gl;
                uintptr_t offset = (up_vertices && ce->stream == 0 ? up_ring_at : d->st.stream_offset[ce->stream]) +
                    (uintptr_t)index * stride + ce->offset;
                const void *mapped = NULL;
                if (buffer && buffer == ring_buffer) { mapped = ring_map + offset; memcpy(&gpu, mapped, sizeof gpu); }
                else {
                    if (trace_buffer_word(buffer, offset, &gpu)) mapped = &gpu;
                }
                void_log("[trace]   v%u COLOR0 shadow %08X GPU %08X%s type %u BGRA %u", index, raw, gpu,
                    mapped ? "" : " (unreadable)", ce->type, ce->type == 4);
            }
        }
        char tex[256];
        size_t tn = 0;
        tex[0] = 0;
        for (int s2 = 0; s2 < 8; ++s2) {
            d3d_texture *t = (d3d_texture *)d->st.textures[s2];
            if (t && (ps_samplers & (1u << s2)))
                tn += (size_t)snprintf(tex + tn, sizeof tex - tn, " s%d=%ux%u/%u%s", s2, t->width, t->height, t->format,
                    t->hud_tag ? " hud" : "");
        }
        uint32_t ck = (g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1);
        {
            d3d_decl *dd = (d3d_decl *)d->st.decl;
            char dl[256];
            size_t dn = 0;
            dl[0] = 0;
            for (int i = 0; dd && i < dd->count && dn < sizeof dl - 24; ++i)
                dn += (size_t)snprintf(dl + dn, sizeof dl - dn, " %u.%u:t%u@%u", dd->elements[i].usage, dd->elements[i].usage_index,
                    dd->elements[i].type, dd->elements[i].offset);
            void_log("[trace]   decl%s (fvf %X, set vs %08X)", dl, d->st.fvf, shader_tag(d->st.vs));
        }
        void_log("[trace] draw from %08X prim %u count %u%s vs %08X ps %08X%s blend %u(%u,%u) z %u/%u/%u cull %u cw %X vp %u,%u %ux%u%s",
            g_icall_from[ck], primitive, vertex_count, indexed ? " indexed" : "", shader_tag(vs), shader_tag(ps), (!vs && !ps) ? " (ff)" : "",
            d->st.rs[27], d->st.rs[19], d->st.rs[20], d->st.rs[7], d->st.rs[14], d->st.rs[23], d->st.rs[22], d->st.rs[168],
            d->st.viewport[0], d->st.viewport[1], d->st.viewport[2], d->st.viewport[3], tex);
        if (indexed && !up_vertices && !up_indices) trace_dump_skinned(d, base_vertex, start_index, vertex_count);
    }
    if (tracing(d)) {
        static int textures_traced;
        if (getenv("VOID_TRACE_TEXTURES") && textures_traced < 24) {
            d3d_texture *tt = (d3d_texture *)d->st.textures[0];
            if (tt && tt->rtype == D3DRTYPE_TEXTURE) {
                ++textures_traced;
                trace_dump(d, (d3d_surface *)void_com_host(d3d_texture_surface(tt, 0, 0)), "stage 0 texture");
            }
        }
        if (vs || ps) d3d_shader_trace();
        if (vs && !ps) {
            /* A vertex shader feeding the texture stage cascade: the stages it runs. */
            char line[400];
            size_t n = 0;
            line[0] = 0;
            for (int s = 0; s < 4 && n < sizeof line - 64; ++s) {
                const uint32_t *t = d->st.tss[s];
                n += (size_t)snprintf(line + n, sizeof line - n, " ts%d op %u(%X,%X) a %u(%X,%X)%s", s, t[1], t[2], t[3],
                    t[4], t[5], t[6], d->st.textures[s] ? " tex" : "");
                if (t[1] == 1) break;
            }
            void_log("[trace]   ff pixel:%s; tf %08X specular %u", line, d->st.rs[60], d->st.rs[29]);
        }
        if (!vs && !ps) {
            /* Fixed function: the declaration, stage 0 and the first vertices. */
            d3d_decl *decl = (d3d_decl *)d->st.decl;
            char line[512];
            size_t n = 0;
            for (int i = 0; decl && i < decl->count && n < sizeof line - 40; ++i)
                n += (size_t)snprintf(line + n, sizeof line - n, " [s%u+%u t%u u%u.%u]", decl->elements[i].stream,
                    decl->elements[i].offset, decl->elements[i].type, decl->elements[i].usage, decl->elements[i].usage_index);
            const uint32_t *t0 = d->st.tss[0], *t1 = d->st.tss[1];
            void_log("[trace]   ff decl%s fvf %X; ts0 op %u(%X,%X) a %u(%X,%X) tci %X ttf %X; ts1 op %u; lighting %u tf %08X",
                line, d->st.fvf, t0[1], t0[2], t0[3], t0[4], t0[5], t0[6], t0[11], t0[24], t1[1], d->st.rs[137], d->st.rs[60]);
            const uint8_t *vb = NULL;
            uint32_t stride = 0;
            if (up_vertices) { vb = (const uint8_t *)up_vertices; stride = up_stride; }
            else if (d->st.streams[0]) {
                d3d_buffer *b = (d3d_buffer *)d->st.streams[0];
                vb = (const uint8_t *)GPTR(b->shadow + d->st.stream_offset[0]);
                stride = d->st.stream_stride[0];
            }
            static int textures_dumped;
            d3d_texture *t0tex = (d3d_texture *)d->st.textures[0];
            if (t0tex && textures_dumped++ < 2 && t0tex->rtype == D3DRTYPE_TEXTURE)
                trace_dump(d, (d3d_surface *)void_com_host(d3d_texture_surface(t0tex, 0, 0)), "ff stage 0 texture");
            for (uint32_t v = 0; vb && v < 4 && v < vertex_count; ++v) {
                const uint32_t *w = (const uint32_t *)(vb + (size_t)(start_vertex + v) * stride);
                float f[8];
                memcpy(f, w, sizeof f);
                void_log("[trace]   v%u: %g %g %g %g | %08X %08X | %g %g", v, f[0], f[1], f[2], f[3], w[4], w[5], f[6], f[7]);
            }
        }
        GLint view[4], scissor[4], fb = 0, status = 0;
        GLfloat range[2];
        glGetIntegerv(GL_VIEWPORT, view);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetFloatv(GL_DEPTH_RANGE, range);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        status = (GLint)glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        void_log("[trace]   gl viewport %d,%d %dx%d depth %g..%g scissor %s %d,%d %dx%d cull %d depth %d stencil %d blend %d fb %d status %04X",
            view[0], view[1], view[2], view[3], range[0], range[1], glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off", scissor[0],
            scissor[1], scissor[2], scissor[3], glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_STENCIL_TEST),
            glIsEnabled(GL_BLEND), fb, status);
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        GLint af = glGetUniformLocation((GLuint)program, "vpAlphaFunc"), ar = glGetUniformLocation((GLuint)program, "vpAlphaRef");
        GLint afv = -1;
        GLfloat arv = -1;
        if (af >= 0) glGetUniformiv((GLuint)program, af, &afv);
        if (ar >= 0) glGetUniformfv((GLuint)program, ar, &arv);
        void_log("[trace]   alpha test rs %u func %u ref %u; program vpAlphaFunc %d vpAlphaRef %g", d->st.rs[15], d->st.rs[25],
            d->st.rs[24], afv, arv);
        if (getenv("VOID_DEBUG_NOALPHA") && af >= 0) glUniform1i(af, 8);
        if (getenv("VOID_DEBUG_NODEPTH")) glDisable(GL_DEPTH_TEST);
        if (getenv("VOID_DEBUG_NOCULL")) glDisable(GL_CULL_FACE);
        if (getenv("VOID_TRACE_DIFF")) {
            free(trace_before);
            trace_before = NULL;
            if (!trace_read_rt0(d, &trace_before, &trace_before_w, &trace_before_h)) { free(trace_before); trace_before = NULL; }
        }
        glGenQueries(1, &trace_query);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, trace_query);
    }
    GLenum mode = gl_mode(primitive);
    if (indexed) {
        if (up_indices) {
            uint32_t index_bytes = vertex_count * (up_index32 ? 4u : 2u), index_at = 0;
            uint8_t *dst = ring_alloc(index_bytes, &index_at);
            GLuint element = ring_buffer;
            if (dst) memcpy(dst, up_indices, index_bytes);
            else {
                index_at = 0;
                if (!up_ibo) glGenBuffers(1, &up_ibo);
                element = up_ibo;
            }
            if (elem_buffer != element) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element); elem_buffer = element; }
            if (!dst) glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_bytes, up_indices, GL_STREAM_DRAW);
            glDrawElements(mode, (GLsizei)vertex_count, up_index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                           (const void *)(uintptr_t)index_at);
        } else {
            d3d_buffer *ib = (d3d_buffer *)d->st.indices;
            if (!ib) { void_log("[gl] indexed draw without an index buffer"); return 0; }
            if (elem_buffer != ib->gl) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl); elem_buffer = ib->gl; }
            int i32 = ib->format == 102;
            const void *ptr = (const void *)(uintptr_t)(start_index * (i32 ? 4u : 2u));
            if (instances)
                glDrawElementsInstancedBaseVertex(mode, (GLsizei)vertex_count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, ptr,
                    (GLsizei)instances, (GLint)base_vertex);
            else
                glDrawElementsBaseVertex(mode, (GLsizei)vertex_count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, ptr, (GLint)base_vertex);
        }
    } else {
        if (instances) glDrawArraysInstanced(mode, (GLint)start_vertex, (GLsizei)vertex_count, (GLsizei)instances);
        else glDrawArrays(mode, (GLint)start_vertex, (GLsizei)vertex_count);
    }
    /* GL errors stay set until read: looking every 64th draw (and at every
     * traced one) still reports them, without a driver call per draw. */
    GLenum e = (draw_calls & 63u) == 0 || trace_query ? glGetError() : GL_NO_ERROR;
    if (e) {
        static int warned;
        if (warned++ < 20) {
            char log[1024]={0};GLint bound=0;glGetIntegerv(GL_CURRENT_PROGRAM,&bound);glValidateProgram(current_gl_program);glGetProgramInfoLog(current_gl_program,sizeof log,NULL,log);
            void_log("[gl] draw error %04X (prim %u count %u indexed %d, program %u bound %d): %s",e,primitive,vertex_count,indexed,current_gl_program,bound,log);
        }
    }
    if (trace_query) {
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(trace_query, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &trace_query);
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        void_log("[trace]   samples passed: %s (program %d, error %04X)", passed ? "yes" : "NONE", program, e);
        uint8_t *after = NULL;
        uint32_t w = 0, h = 0;
        if (trace_before && trace_read_rt0(d, &after, &w, &h) && w == trace_before_w && h == trace_before_h) {
            uint32_t changed = 0, x0 = w, y0 = h, x1 = 0, y1 = 0;
            double sum_before = 0, sum_after = 0;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t *a = trace_before + 4 * ((size_t)y * w + x), *b = after + 4 * ((size_t)y * w + x);
                    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
                        ++changed;
                        sum_before += a[0] + a[1] + a[2];
                        sum_after += b[0] + b[1] + b[2];
                        if (x < x0) x0 = x;
                        if (y < y0) y0 = y;
                        if (x > x1) x1 = x;
                        if (y > y1) y1 = y;
                    }
                }
            if (changed)
                void_log("[trace]   changed %u px in (%u,%u)-(%u,%u), mean rgb %.1f -> %.1f", changed, x0, y0, x1, y1,
                    sum_before / (3.0 * changed), sum_after / (3.0 * changed));
            else
                void_log("[trace]   changed nothing");
        }
        free(after);
    }
    return 1;
}

/* ---------------------------------------------------------------- clear */
void d3d_gl_clear(d3d_device *d, uint32_t count, const uint32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    d3d_gl_bind_targets(d);
    if (tracing(d)) void_log("[trace] clear flags %X color %08X z %g stencil %u rects %u", flags, color, z, stencil, count);
    GLbitfield mask = 0;
    if ((flags & 1u) && d->rt[0]) {
        mask |= GL_COLOR_BUFFER_BIT;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (int i = 1; i < 4; ++i) glColorMaski((GLuint)i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
    }
    if ((flags & 2u) && d->ds) { mask |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepthf(z); }
    if ((flags & 4u) && d->ds && d->ds->fmt && d->ds->fmt->stencil) { mask |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xFF); glClearStencil((GLint)stencil); }
    if (!mask) return;
    /* The cleared area is the viewport, narrowed by the scissor and rects. */
    int vx = (int)d->st.viewport[0], vy = (int)d->st.viewport[1];
    int vx2 = vx + (int)d->st.viewport[2], vy2 = vy + (int)d->st.viewport[3];
    if (d->st.rs[174]) {
        int sx = (int)d->st.scissor[0], sy = (int)d->st.scissor[1];
        int sx2 = sx + (int)d->st.scissor[2], sy2 = sy + (int)d->st.scissor[3];
        if (sx > vx) vx = sx;
        if (sy > vy) vy = sy;
        if (sx2 < vx2) vx2 = sx2;
        if (sy2 < vy2) vy2 = sy2;
    }
    d3d_gl_state_forget();
    glEnable(GL_SCISSOR_TEST);
    float k = target_scale;
    if (!count || !rects) {
        if (vx2 > vx && vy2 > vy) {
            glScissor(scaled_px((uint32_t)vx, k), scaled_px((uint32_t)vy, k), scaled_px((uint32_t)vx2, k) - scaled_px((uint32_t)vx, k),
                scaled_px((uint32_t)vy2, k) - scaled_px((uint32_t)vy, k));
            glClear(mask);
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            int x1 = (int)rects[4 * i], y1 = (int)rects[4 * i + 1];
            int x2 = (int)rects[4 * i + 2], y2 = (int)rects[4 * i + 3];
            if (x1 < vx) x1 = vx;
            if (y1 < vy) y1 = vy;
            if (x2 > vx2) x2 = vx2;
            if (y2 > vy2) y2 = vy2;
            if (x2 <= x1 || y2 <= y1) continue;
            glScissor(scaled_px((uint32_t)x1, k), scaled_px((uint32_t)y1, k), scaled_px((uint32_t)x2, k) - scaled_px((uint32_t)x1, k),
                scaled_px((uint32_t)y2, k) - scaled_px((uint32_t)y1, k));
            glClear(mask);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

/* -------------------------------------------------------- blits and reads */
void d3d_gl_blit(d3d_surface *src, const uint32_t *sr, d3d_surface *dst, const uint32_t *dr, uint32_t filter)
{
    if (tracing(d3d_rt_device())) {
        GLboolean cm[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        void_log("[trace] blit %ux%u fmt %u (%u,%u,%u,%u) -> %ux%u fmt %u (%u,%u,%u,%u) colour mask was %d%d%d%d", src->width,
            src->height, src->format, sr[0], sr[1], sr[2], sr[3], dst->width, dst->height, dst->format, dr[0], dr[1], dr[2], dr[3],
            cm[0], cm[1], cm[2], cm[3]);
        trace_dump(d3d_rt_device(), src, "blit source");
    }
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    int depth = src->fmt && src->fmt->depth;
    GLenum attachment = depth ? depth_attachment(src) : GL_COLOR_ATTACHMENT0;
    /* A destination the main framebuffer holds is written through it: Mali
     * keeps a clear of that framebuffer pending as the load operation of
     * its render pass and resolves it when the pass ends, after - and over -
     * writes made through other framebuffers (Mali-G76 r26p0). */
    d3d_device *dev = d3d_rt_device();
    int slot = -1;
    if (dev) {
        if (depth) slot = dev->ds == dst ? 4 : -1;
        else for (int i = 0; i < 4; ++i) if (dev->rt[i] == dst) slot = i;
    }
    if (slot >= 0) {
        d3d_gl_bind_targets(dev);
        // A single-sample blit cannot draw into a multisampled FBO; EXT resolves before texture writes.
        if (!depth && fb_samples) slot = -1;
    }
    if (slot >= 0) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        if (slot < 4) {
            GLenum bufs[4] = { GL_NONE, GL_NONE, GL_NONE, GL_NONE };
            bufs[slot] = GL_COLOR_ATTACHMENT0 + (GLenum)slot;
            glDrawBuffers(slot + 1, bufs);
        }
    } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        attach(GL_DRAW_FRAMEBUFFER, depth ? depth_attachment(dst) : GL_COLOR_ATTACHMENT0, dst);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    attach(GL_READ_FRAMEBUFFER, attachment, src);
    glDisable(GL_SCISSOR_TEST);
    /* StretchRect ignores the write masks; some GL drivers apply them to
     * blits. The next draw re-applies the D3D masks. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    GLbitfield mask = depth ? (GL_DEPTH_BUFFER_BIT | (src->fmt->stencil ? GL_STENCIL_BUFFER_BIT : 0)) : GL_COLOR_BUFFER_BIT;
    float ks = d3d_scale_of(src), kd = d3d_scale_of(dst);
    int resample = ks != kd || (sr[2] - sr[0]) != (dr[2] - dr[0]) || (sr[3] - sr[1]) != (dr[3] - dr[1]);
    d3d_gl_state_forget();
    glBlitFramebuffer(scaled_px(sr[0], ks), scaled_px(sr[1], ks), scaled_px(sr[2], ks), scaled_px(sr[3], ks),
        scaled_px(dr[0], kd), scaled_px(dr[1], kd), scaled_px(dr[2], kd), scaled_px(dr[3], kd),
        mask, (!depth && (filter >= 2 || (resample && ks != kd))) ? GL_LINEAR : GL_NEAREST);
    GLenum e = glGetError();
    if (e) void_log("[gl] StretchRect %ux%u fmt %u -> %ux%u fmt %u error %04X", src->width, src->height, src->format,
        dst->width, dst->height, dst->format, e);
    if (tracing(d3d_rt_device())) {
        GLenum rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        GLint rb = 0, db = 0;
        glGetIntegerv(GL_READ_BUFFER, &rb);
        glGetIntegerv(GL_DRAW_BUFFER0, &db);
        void_log("[trace]   blit read fb %04X buffer %04X, draw fb %04X buffer %04X", rs, rb, ds, db);
        trace_dump(d3d_rt_device(), dst, "blit result");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_rt_device()->fbo_dirty = 1;
}

/* Logical RGBA of rect (x0, y0, x1, y1) of a GPU surface: storage rows are
 * D3D rows, so the rect reads unchanged. Returns 0 if GL cannot read it. */
/* A scaled surface's rectangle, resampled to D3D pixels, attached to
 * read_fbo at (0,0) for glReadPixels (a reused scratch texture). */
static int read_scaled(d3d_surface *s, const uint32_t *r)
{
    static GLuint scratch, scratch_fbo;
    static GLenum scratch_format;
    static GLsizei scratch_w, scratch_h;
    GLsizei w = (GLsizei)(r[2] - r[0]), h = (GLsizei)(r[3] - r[1]);
    if (!scratch || scratch_format != s->fmt->internal || w > scratch_w || h > scratch_h) {
        if (scratch) { d3d_gl_forget_texture(scratch); glDeleteTextures(1, &scratch); }
        if (!scratch_fbo) glGenFramebuffers(1, &scratch_fbo);
        glGenTextures(1, &scratch);
        d3d_gl_upload_unit();
        glBindTexture(GL_TEXTURE_2D, scratch);
        scratch_w = w > scratch_w ? w : scratch_w;
        scratch_h = h > scratch_h ? h : scratch_h;
        glTexStorage2D(GL_TEXTURE_2D, 1, s->fmt->internal, scratch_w, scratch_h);
        scratch_format = s->fmt->internal;
    }
    float k = d3d_scale_of(s);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scratch_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratch, 0);
    d3d_gl_state_forget();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(scaled_px(r[0], k), scaled_px(r[1], k), scaled_px(r[2], k), scaled_px(r[3], k), 0, 0, w, h,
        GL_COLOR_BUFFER_BIT, s->fmt->is_float ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch_fbo);
    return 1;
}

/* The read/resolve and state restoration are shared by logical-float reads
 * and byte-exact A8R8G8B8/X8R8G8B8 surface locks. -1 means ReadPixels ran but
 * failed, 0 means no read: logical reads retain their old failure output. */
static int read_rgba_pixels(d3d_surface *s, const uint32_t *r, GLenum type, void *out)
{
    if (!s->fmt || s->fmt->depth || !s->fmt->internal) return 0;
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    uint32_t origin[4] = { 0, 0, r[2] - r[0], r[3] - r[1] };
    if (d3d_scale_of(s) != 1.0f) {
        read_scaled(s, r);
        r = origin;
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLsizei w = (GLsizei)(r[2] - r[0]), h = (GLsizei)(r[3] - r[1]);
    int ok = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        while (glGetError()) {}
        glReadPixels((GLint)r[0], (GLint)r[1], w, h, GL_RGBA, type, out);
        ok = glGetError() == GL_NO_ERROR ? 1 : -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_rt_device()->fbo_dirty = 1;
    return ok;
}

int d3d_gl_read_rgba(d3d_surface *s, const uint32_t *r, float *out)
{
    if (!s->fmt || s->fmt->depth || !s->fmt->internal) return 0;
    uint32_t count = (r[2] - r[0]) * (r[3] - r[1]);
    uint8_t *tmp = s->fmt->is_float ? NULL : (uint8_t *)malloc((size_t)count * 4);
    if (!s->fmt->is_float && !tmp) return 0;
    int ok = read_rgba_pixels(s, r, s->fmt->is_float ? GL_FLOAT : GL_UNSIGNED_BYTE, tmp ? (void *)tmp : out);
    if (ok && tmp) d3d_gl_layout_to_rgba(GL_RGBA, GL_UNSIGNED_BYTE, tmp, count, out);
    free(tmp);
    if (ok <= 0) return 0;
    /* Standalone GPU surfaces are stored like render targets (d3d_surface_new). */
    uint32_t usage = s->container ? s->container->usage : (s->usage | D3DUSAGE_RENDERTARGET);
    GLint sw[4];
    d3d_storage_swizzle(s->fmt, usage, sw);
    d3d_swizzle_rgba(sw, out, count);
    return 1;
}

/* A whole GPU surface into guest memory in its own format (LockRect). */
void d3d_gl_read_surface(d3d_surface *src, uint32_t dst_mem, uint32_t dst_pitch)
{
    uint32_t w = src->width, h = src->height, rect[4] = { 0, 0, w, h };
    int bytes = src->fmt && !src->fmt->depth && src->fmt->internal && (src->format == 21 || src->format == 22);
    float *px = bytes ? NULL : (float *)malloc((size_t)w * h * 16);
    uint8_t *raw = bytes ? (uint8_t *)malloc((size_t)w * h * 4) : NULL;
    int read_ok = bytes ? (raw && read_rgba_pixels(src, rect, GL_UNSIGNED_BYTE, raw) > 0)
                        : (px && d3d_gl_read_rgba(src, rect, px));
    if (!read_ok) {
        void_log("[gl] surface %ux%u format %u cannot be read back", w, h, src->format);
        for (uint32_t y = 0; y < h; ++y) memset(GPTR(dst_mem + y * dst_pitch), 0, dst_pitch);
    } else if (bytes) {
        /* GL stores ordinary BGRA textures as BGRA, GPU-written targets as
         * RGBA. Both byte layouts round-trip exactly through the old float
         * decoder/encoder; X8's logical alpha is always one. */
        uint32_t usage = src->container ? src->container->usage : (src->usage | D3DUSAGE_RENDERTARGET);
        int swap = (usage & D3DUSAGE_RENDERTARGET) != 0;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = raw + (size_t)y * w * 4;
            uint8_t *d = (uint8_t *)GPTR(dst_mem + y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x) {
                d[4*x] = s[4*x + (swap ? 2 : 0)]; d[4*x+1] = s[4*x+1];
                d[4*x+2] = s[4*x + (swap ? 0 : 2)]; d[4*x+3] = src->format == 22 ? 255 : s[4*x+3];
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y)
            if (!d3d_format_encode(src->format, px + (size_t)y * w * 4, w, (uint8_t *)GPTR(dst_mem + y * dst_pitch))) {
                void_log("[gl] read back of format %u unsupported", src->format);
                break;
            }
    }
    {
        /* Picture-sized reads (thumbnails, snapshots): the mean colour shows
         * whether all channels came back. */
        static uint32_t pictures;
        if (read_ok && w >= 128 && h >= 128 && pictures < 64) {
            ++pictures;
            double sum[4] = { 0, 0, 0, 0 };
            if (px) {
                for (size_t i = 0; i < (size_t)w * h; ++i) for (int k = 0; k < 4; ++k) sum[k] += px[4 * i + k];
            } else {
                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t *row = (const uint8_t *)GPTR(dst_mem + y * dst_pitch);
                    for (uint32_t x = 0; x < w; ++x) for (int k = 0; k < 4; ++k)
                        sum[k] += row[4*x + (k == 0 ? 2 : k == 2 ? 0 : k)] / 255.0f;
                }
            }
            double n = (double)w * h;
            void_log("[gl] read back %ux%u format %u (usage %X%s, scale %.3f): mean %.3f %.3f %.3f %.3f", w, h,
                src->format, src->container ? src->container->usage : src->usage, src->container ? ", texture" : "",
                (double)d3d_scale_of(src), sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
        }
    }
    if (void_verbose) {
        static uint32_t reads;
        if (reads++ % 64 == 0 || w * h <= 64) {
            float byte_color[4] = { 0, 0, 0, 0 };
            if (read_ok && bytes) {
                const uint8_t *p = (const uint8_t *)GPTR(dst_mem + (h / 2) * dst_pitch) + (w / 2) * 4;
                for (int k = 0; k < 4; ++k) byte_color[k] = p[k == 0 ? 2 : k == 2 ? 0 : k] / 255.0f;
            }
            const float *c = px ? px + ((size_t)(h / 2) * w + w / 2) * 4 : byte_color;
            VLOG("[gl] read back %ux%u format %u (read %u): centre %.3f %.3f %.3f %.3f, first word %08X", w, h,
                src->format, reads, (double)c[0], (double)c[1], (double)c[2], (double)c[3], MEM32(dst_mem));
        }
    }
    free(px);
    free(raw);
}

/* Pixels written into a scaled surface (LockRect, UpdateSurface on a
 * screen-sized target): uploaded at D3D size, stretched into place. */
void d3d_gl_upload_scaled(d3d_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, GLenum format, GLenum type,
    const void *data)
{
    static int noted;
    if (!noted++) void_log("[gl] upload into a scaled %ux%u target (format %u)", s->width, s->height, s->format);
    GLuint tex;
    glGenTextures(1, &tex);
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, s->fmt->internal, (GLsizei)w, (GLsizei)h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, format, type, data);
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    d3d_gl_state_forget();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float k = d3d_scale_of(s);
    glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, scaled_px(x, k), scaled_px(y, k), scaled_px(x + w, k), scaled_px(y + h, k),
        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_gl_forget_texture(tex);
    glDeleteTextures(1, &tex);
    d3d_rt_device()->fbo_dirty = 1;
}

void d3d_gl_fill(d3d_surface *dst, const uint32_t *r, uint32_t color)
{
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst);
    d3d_gl_state_forget();
    float k = d3d_scale_of(dst);
    glEnable(GL_SCISSOR_TEST);
    glScissor(scaled_px(r[0], k), scaled_px(r[1], k), scaled_px(r[2], k) - scaled_px(r[0], k), scaled_px(r[3], k) - scaled_px(r[1], k));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_rt_device()->fbo_dirty = 1;
}

/* --------------------------------------------------------------- present */
void d3d_gl_dump_frame(d3d_device *dev, const char *path)
{
    d3d_surface *bb = dev->backbuffer;
    uint32_t w = d3d_scaled_size(bb->width, d3d_scale_of(bb)), h = d3d_scaled_size(bb->height, d3d_scale_of(bb));
    if (!w || !h || (uint64_t)w * h > SIZE_MAX / 4) {
        void_log("[gl] frame dump invalid size %ux%u", w, h); remove(path); return;
    }
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px) { void_log("[gl] frame dump allocation failed %ux%u", w, h); remove(path); return; }
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR;)
        void_log("[gl] frame dump prior GL error %04X", error);
    GLboolean discarded = glIsEnabled(GL_RASTERIZER_DISCARD), tf_active = GL_FALSE, tf_paused = GL_FALSE;
    glGetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE, &tf_active); glGetBooleanv(GL_TRANSFORM_FEEDBACK_PAUSED, &tf_paused);
    GLenum state_error = glGetError();
    void_log("[gl] frame %u dump state: raster discard %u transform feedback active/paused %u/%u error %04X",
        dev->frame, discarded, tf_active, tf_paused, state_error);
    GLint old_read, old_draw, old_read_buffer, old_pack, pack_align, pack_row, pack_x, pack_y;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack); glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row); glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_x); glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_y);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, bb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), error = glGetError();
    if (status == GL_FRAMEBUFFER_COMPLETE && error == GL_NO_ERROR) {
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        error = glGetError();
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack); glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row); glPixelStorei(GL_PACK_SKIP_PIXELS, pack_x); glPixelStorei(GL_PACK_SKIP_ROWS, pack_y);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read); glReadBuffer((GLenum)old_read_buffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw);
    dev->fbo_dirty = 1;
    if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
        void_log("[gl] frame dump failed: FBO %04X GL error %04X (%ux%u)", status, error, w, h);
        free(px); remove(path); return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
        uint8_t header[54] = { 'B', 'M' };
        memcpy(header + 2, &size, 4);
        uint32_t v = 54; memcpy(header + 10, &v, 4);
        v = 40; memcpy(header + 14, &v, 4);
        memcpy(header + 18, &w, 4); memcpy(header + 22, &h, 4);
        header[26] = 1; header[28] = 24;
        fwrite(header, 1, 54, f);
        uint8_t zero[4] = {0};
        /* BMP rows run bottom-up; the backbuffer's row 0 is the top. */
        for (uint32_t y = h; y-- > 0;) {
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t *p = px + ((size_t)y * w + x) * 4;
                uint8_t bgr[3] = { p[2], p[1], p[0] };
                fwrite(bgr, 1, 3, f);
            }
            fwrite(zero, 1, pad, f);
        }
        fclose(f);
        void_log("[gl] frame %u written to %s", dev->frame, path);
    }
    free(px);
}

/* GPU time per frame (GL_EXT_disjoint_timer_query): one TIME_ELAPSED query
 * spans each frame's commands, from the swap before to the swap after it, and
 * is read back a few frames later only once available (no waiting). Frames
 * that a disjoint event touched are dropped. S2_GPU_TIMER=0 turns it off. */
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif
#define GPU_QUERIES 6u
typedef void (*query_u64_fn)(GLuint id, GLenum pname, GLuint64 *params);
static GLuint gpu_query[GPU_QUERIES];
static uint64_t gpu_query_issued[GPU_QUERIES];
static uint32_t gpu_query_head, gpu_query_tail;   /* queries begun / collected */
static int gpu_query_open, gpu_timer_state;       /* state: 0 untried, 1 on, -1 off */
static query_u64_fn gpu_query_u64;
static uint64_t gpu_ns, gpu_frames;

static void gpu_timer_end(void)
{
    if (gpu_timer_state <= 0 || !gpu_query_open) return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    gpu_query_open = 0;
    ++gpu_query_head;
}

static void gpu_timer_begin(void)
{
    if (!gpu_timer_state) {
        const char *env = getenv("S2_GPU_TIMER");
        const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
        gpu_query_u64 = (query_u64_fn)eglGetProcAddress("glGetQueryObjectui64vEXT");
        gpu_timer_state = (!env || *env != '0') && extensions && strstr(extensions, "GL_EXT_disjoint_timer_query") &&
            gpu_query_u64 ? 1 : -1;
        if (old_mali_driver((const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION)))
            gpu_timer_state = -1;
        if (gpu_timer_state > 0) glGenQueries(GPU_QUERIES, gpu_query);
        void_log("[gl] GPU frame timer %s", gpu_timer_state > 0 ? "on" : "unavailable");
    }
    if (gpu_timer_state <= 0) return;
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);   /* reading it clears it */
    while (gpu_query_tail != gpu_query_head) {
        GLuint id = gpu_query[gpu_query_tail % GPU_QUERIES], available = 0;
        glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        if (!available && !disjoint) break;
        if (available && !disjoint) {
            GLuint64 ns = 0;
            gpu_query_u64(id, GL_QUERY_RESULT, &ns);
            uint64_t age = void_monotonic_ns() - gpu_query_issued[gpu_query_tail % GPU_QUERIES];
            if (ns > age + 1000000ull) {
                void_log("[gl] GPU timer returned an impossible duration (%llu ns after %llu ns); disabled",
                    (unsigned long long)ns, (unsigned long long)age);
                gpu_timer_state = -1;
                return;
            }
            gpu_ns += ns;
            ++gpu_frames;
        }
        ++gpu_query_tail;
        if (!available) break;     /* a disjoint frame still in flight: collect it next time */
    }
    if (gpu_query_head - gpu_query_tail >= GPU_QUERIES) return;   /* all in flight: skip this frame */
    gpu_query_issued[gpu_query_head % GPU_QUERIES] = void_monotonic_ns();
    glBeginQuery(GL_TIME_ELAPSED_EXT, gpu_query[gpu_query_head % GPU_QUERIES]);
    gpu_query_open = 1;
}

static volatile int dump_requested;

/* Dump the next presented frame (scripted "shot"). */
void d3d_request_frame_dump(void) { dump_requested = 1; }

/* The Frame Rate Limit option (sims2_settings.c): each present waits for its
 * slot, the machine handed over meanwhile so the other guest threads run.
 * A frame that comes late starts the schedule over instead of rushing the
 * next ones. */
static void pace_frame(void)
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

static int old_mali_driver(const char *renderer, const char *version)
{
    return renderer && version && strstr(renderer, "Mali-G51") && strstr(version, "v1.r18p0");
}

static int shader_present_driver(const char *vendor, const char *renderer)
{
    /* ARM window blits can stay black despite a correct source framebuffer. */
    return (vendor && strstr(vendor, "ARM")) ||
        (renderer && (strstr(renderer, "Mali") || strstr(renderer, "Immortalis")));
}

static int shader_present_enabled(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = shader_present_driver((const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
        char path[1100];
        snprintf(path, sizeof path, "%s/shader-present", void_data_dir());
        FILE *f = fopen(path, "r");
        if (f) { int c = fgetc(f); if (c == '0' || c == '1') mode = c == '1'; fclose(f); }
        void_log("[gl] window presenter: %s", mode ? "shader" : "framebuffer blit");
    }
    return mode;
}

static int swap_window_surface(void)
{
    static unsigned failures;
    static int report_success = 1;
    if (eglSwapBuffers(egl_display, egl_surface)) {
        if (report_success) void_log("[gl] window swap succeeded (serial %u)", egl_window_serial);
        report_success = 0;
        return 1;
    }
    report_success = 1;
    EGLint error = eglGetError();
    if (++failures <= 8 || !(failures & (failures - 1)))
        void_log("[gl] window swap failed %04X (failure %u, serial %u); recreating the surface", error, failures, egl_window_serial);
    eglMakeCurrent(egl_display, egl_pbuffer, egl_pbuffer, egl_context);
    eglDestroySurface(egl_display, egl_surface);
    egl_surface = EGL_NO_SURFACE;
    return 0;
}

/* QA outliers join CPU sample timestamps without timing every GL operation. */
static void long_frame_log(d3d_device *dev, uint64_t now, uint64_t game_cpu, uint64_t machine_wait,
    uint64_t syncs, uint64_t queue_wait)
{
    static int enabled = -1;
    static uint64_t last, cpu_last, game_last, machine_last, syncs_last, queue_last, swap_last, draws_last;
    if (enabled < 0) {
        const char *data = void_data_dir(), *profile = data ? strrchr(data, '/') : NULL;
        profile = profile ? profile + 1 : data;
        enabled = profile && !strncmp(profile, "userdata-qa-", 12) && profile[12];
    }
    if (!enabled) return;
    struct timespec clock;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &clock)) return;
    uint64_t cpu = (uint64_t)clock.tv_sec * 1000000000ull + (uint64_t)clock.tv_nsec;
    if (last && now - last > 50000000ull)
        void_log("[gl-frame] frame %u mono %llu cadence %llu gamecpu %llu rendercpu %llu machinewait %llu "
            "queuewait %llu syncs %llu swap %llu draws %llu", dev->frame,
            (unsigned long long)now, (unsigned long long)(now - last),
            (unsigned long long)(game_cpu - game_last), (unsigned long long)(cpu - cpu_last),
            (unsigned long long)(machine_wait - machine_last), (unsigned long long)(queue_wait - queue_last),
            (unsigned long long)(syncs - syncs_last), (unsigned long long)(swap_ns - swap_last),
            (unsigned long long)(draw_calls - draws_last));
    last = now; cpu_last = cpu; game_last = game_cpu; machine_last = machine_wait;
    syncs_last = syncs; queue_last = queue_wait; swap_last = swap_ns; draws_last = draw_calls;
}

void d3d_gl_present(d3d_device *dev, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns)
{
    int frame_presented = 0;
    ring_frame_end();
    static struct timespec fps_start, cpu_start;
    static uint64_t wait_start, draws_start, swap_start_ns, game_cpu_start, syncs_start, rt_wait_start, gpu_ns_start, gpu_frames_start;
    static uint64_t cadence_last_ns, cadence_max_ns;
    static uint32_t cadence_frames, cadence_over90;
    static uint32_t fps_frame;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Present-start cadence includes the previous swap and the next frame's
     * work; keep outliers that a five-second average would hide. */
    uint64_t cadence_now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    long_frame_log(dev, cadence_now_ns, game_cpu_ns, game_wait_ns, syncs, rt_wait_ns);
    if (cadence_last_ns) {
        uint64_t elapsed = cadence_now_ns - cadence_last_ns;
        if (elapsed > cadence_max_ns) cadence_max_ns = elapsed;
        if (elapsed > 1000000000ull / 90u) ++cadence_over90;
        ++cadence_frames;
    }
    cadence_last_ns = cadence_now_ns;
    /* Every 600 frames, or every 5 s when frames are slow. "cpu" is the game's
     * thread, "render" the render thread's (when it runs apart). */
    if (dev->frame - fps_frame >= 600 || !fps_start.tv_sec || now.tv_sec - fps_start.tv_sec >= 5) {
        struct timespec cpu;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        double dt = (double)(now.tv_sec - fps_start.tv_sec) + (double)(now.tv_nsec - fps_start.tv_nsec) / 1e9;
        double drender = (double)(cpu.tv_sec - cpu_start.tv_sec) + (double)(cpu.tv_nsec - cpu_start.tv_nsec) / 1e9;
        double dgame = (double)(game_cpu_ns - game_cpu_start) / 1e9;
        uint32_t frames = dev->frame - fps_frame;
        if (fps_start.tv_sec && frames) {
            char render[96] = "";
            if (d3d_rt_threaded())
                snprintf(render, sizeof render, ", %.1f ms render (%.1f ms waited, %.1f syncs)", 1000.0 * drender / frames,
                    (double)(rt_wait_ns - rt_wait_start) / 1e6 / frames, (double)(syncs - syncs_start) / frames);
            char gpu[48] = "";
            if (gpu_frames > gpu_frames_start)
                snprintf(gpu, sizeof gpu, "; gpu %.2f ms", (double)(gpu_ns - gpu_ns_start) / 1e6 / (double)(gpu_frames - gpu_frames_start));
            void_log("[gl] frame %u: %.1f fps; per frame %.1f ms wall, %.1f ms cpu%s, %.1f ms machine wait, %.0f draws, "
                "%.1f ms swap; cadence max %llu ns, over90 %u/%u%s", dev->frame, frames / dt, 1000.0 * dt / frames, 1000.0 * dgame / frames, render,
                (double)(game_wait_ns - wait_start) / 1e6 / frames, (double)(draw_calls - draws_start) / frames,
                (double)(swap_ns - swap_start_ns) / 1e6 / frames, (unsigned long long)cadence_max_ns,
                cadence_over90, cadence_frames, gpu);
        }
        gpu_ns_start = gpu_ns;
        gpu_frames_start = gpu_frames;
        if (fps_start.tv_sec && frames) void_thunk_stats(frames);
        draws_start = draw_calls;
        swap_start_ns = swap_ns;
        fps_start = now;
        cpu_start = cpu;
        game_cpu_start = game_cpu_ns;
        syncs_start = syncs;
        rt_wait_start = rt_wait_ns;
        wait_start = game_wait_ns;
        fps_frame = dev->frame;
        cadence_max_ns = 0;
        cadence_frames = cadence_over90 = 0;
    }
    if (tracing(dev)) {
        if (trace_rt && trace_rt != dev->backbuffer) trace_dump(dev, trace_rt, "target done");
        trace_dump(dev, dev->backbuffer, "present");
        trace_rt = NULL;
        void_log("[trace] frame %u presented", dev->frame);
    }
    if (dump_requested) {
        dump_requested = 0;
        char path[1100];
        snprintf(path, sizeof path, "%s/frames", void_data_dir());
        mkdir(path, 0775);
        snprintf(path, sizeof path, "%s/frames/shot_%05u.bmp", void_data_dir(), dev->frame);
        d3d_gl_dump_frame(dev, path);
    }
    if (dump_every && dev->frame % (uint32_t)dump_every == 0) {
        char path[1100];
        snprintf(path, sizeof path, "%s/frames", void_data_dir());
        mkdir(path, 0775);
        snprintf(path, sizeof path, "%s/frames/frame_%05u.bmp", void_data_dir(), dev->frame);
        d3d_gl_dump_frame(dev, path);
    }
    if (window_mode) follow_window();
    if (window_mode && egl_surface != EGL_NO_SURFACE) {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &sw);
        eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &sh);
        d3d_surface *bb = dev->backbuffer;
        if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, bb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        d3d_gl_state_forget();
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        /* Fit with the original aspect ratio; flip rows to the window's. */
        float scale = fminf((float)sw / (float)bb->width, (float)sh / (float)bb->height);
        int dw = (int)((float)bb->width * scale), dh = (int)((float)bb->height * scale);
        if(sw-dw<=2&&sh-dh<=2){dw=sw;dh=sh;}
        int dx = (sw - dw) / 2, dy = (sh - dh) / 2;
        GLint bw = (GLint)d3d_scaled_size(bb->width, d3d_scale_of(bb)), bh = (GLint)d3d_scaled_size(bb->height, d3d_scale_of(bb));
        GLuint output = bb->container ? bb->container->gl : bb->gl;
        if(s2_render_upscaler()||s2_render_aa()){
            GLuint filtered=d3d_filter_present(bb->container?bb->container->gl:bb->gl,bw,bh,dw,dh);
            filter_done(dev);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo);
            if(filtered){glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,filtered,0);bw=dw;bh=dh;output=filtered;}
            else attach(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,bb);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
        }
        int shader_present = shader_present_enabled();
        if (shader_present && d3d_filter_output(output, dx, dy, dw, dh)) {
            filter_done(dev);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, bw, bh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT,
                bw == dw && bh == dh ? GL_NEAREST : GL_LINEAR);
            if (shader_present) filter_done(dev);
        }
        if (tracing(dev)) {
            GLenum error = glGetError();
            EGLint samples = -1, sample_buffers = -1;
            eglGetConfigAttrib(egl_display, egl_config, EGL_SAMPLES, &samples);
            eglGetConfigAttrib(egl_display, egl_config, EGL_SAMPLE_BUFFERS, &sample_buffers);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
            GLenum source_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            uint32_t source_pixel = 0, window_pixel = 0;
            glReadPixels(bw / 2, bh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &source_pixel);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            GLenum window_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            glReadPixels(sw / 2, sh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &window_pixel);
            void_log("[trace] present %dx%d -> %dx%d: shader %d error %04X FBO %04X/%04X samples %d/%d center %08X/%08X readerror %04X",
                bw, bh, sw, sh, shader_present, error, source_status, window_status, sample_buffers, samples,
                source_pixel, window_pixel, glGetError());
        }
        gpu_timer_end();
        pace_frame();
        /* The swap waits for vsync: let other guest threads run meanwhile. */
        struct timespec swap_start, swap_end;
        clock_gettime(CLOCK_MONOTONIC, &swap_start);
        d3d_rt_wait_begin();
        frame_presented = swap_window_surface();
        d3d_rt_wait_end();
        gpu_timer_begin();
        clock_gettime(CLOCK_MONOTONIC, &swap_end);
        swap_ns += (uint64_t)(swap_end.tv_sec - swap_start.tv_sec) * 1000000000ull + (uint64_t)swap_end.tv_nsec -
                   (uint64_t)swap_start.tv_nsec;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        dev->fbo_dirty = 1;
    } else {
        gpu_timer_end();
        glFlush();
        pace_frame();
        gpu_timer_begin();
    }
    d3d_scaled_follow(dev);
    d3d_texcache_report();
    void_hud_present();
    if (frame_presented || !window_mode) void_android_frame_presented();
}
