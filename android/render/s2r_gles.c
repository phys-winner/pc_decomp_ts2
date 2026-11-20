/* OpenGL ES 3.2 backend of the native renderers (Painkiller's pkr_gles.c,
 * wiki note 23, as Heroes V's h5r_gles.c).
 *
 * GL runs on the port's render thread (d3d9_thread.c), which replays what the
 * game's thread recorded; the context is made there at the first device on
 * the current Android window and follows window changes at present. Every target, the back buffer included, is a texture
 * stored top-down (s2r_internal.h): Direct3D rects, viewports and scissors
 * are GL's own coordinates there, and the present blit flips once into the
 * window with the aspect kept. Programs are the build-time library's GLSL ES
 * text with the variant constants as #define lines; a variant the library
 * lacks is translated on the device by the same translator. */
#include "s2r_internal.h"
#include "s2r_spec.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/system_properties.h>
#include <unistd.h>

void void_android_gpu_strings(const char *renderer, const char *version) __attribute__((weak));

void *void_android_window_current(uint32_t *serial, int wait);
void void_android_window_ack(uint32_t serial);
void *void_android_window_acquire(uint32_t *serial);

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
typedef void (GL_APIENTRYP s2r_buffer_storage_fn)(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
typedef void (GL_APIENTRYP s2r_query_u64_fn)(GLuint id, GLenum pname, GLuint64 *params);
/* EXT_multisampled_render_to_texture: the scene's MSAA resolved as tiles are written (d3d9_gl.c init_msaa) */
typedef void (GL_APIENTRYP s2r_msaa_texture_fn)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
    GLsizei samples);
typedef void (GL_APIENTRYP s2r_msaa_storage_fn)(GLenum target, GLsizei samples, GLenum internal, GLsizei width, GLsizei height);
#ifndef GL_MAX_SAMPLES_EXT
#define GL_MAX_SAMPLES_EXT 0x8D57
#endif

#define LOGGL(...) void_log("[s2r-gl] " __VA_ARGS__)
/* RCAS sharpness: FsrRcasCon(0.4 stops), the Direct3D 9 layer's .757858283 */
static float fsr1_rcas_sharpness(void) { return 0.757858283f; }

typedef struct gl_tex {
    GLuint name, rb;              /* texture, or a renderbuffer for a depth surface */
    GLenum target, internal, format, type;
    int compressed, depth;
    uint32_t width, height, levels;
    GLuint ms_rb;                 /* a screen-sized depth surface's multisampled twin (scene MSAA) */
    uint32_t ms_samples;
} gl_tex;

enum { GL_BUF_STORES = 3 };

/* A vertex or index buffer: one GL buffer the driver renames when a draw in flight still reads it
 * (glBufferData orphaning on DISCARD, glBufferSubData otherwise), as the Direct3D 9 layer keeps
 * them. The persistent versions this backend had (up to eight full copies of every buffer, kept
 * until the buffer died, filled from a render-thread mirror of its contents) cost about 75 MB of
 * heap and some 60 MB of GPU memory in Veronaville on the Pixel 10a (2026-11-21). */
typedef struct gl_buf {
    GLuint name;
    uint32_t size;
    int dynamic;
    /* PowerVR: persistent coherent stores of a dynamic buffer (G.persistent_buffers), the one in use
       (`name`, `map`) and those a DISCARD retired, each with the fence of the draws that read it. */
    uint8_t *map;
    GLuint spare[GL_BUF_STORES];
    uint8_t *spare_map[GL_BUF_STORES];
    GLsync spare_fence[GL_BUF_STORES];
} gl_buf;

/* A framebuffer: up to four colour attachments (MRT), a depth renderbuffer, and the samples of
 * the scene's MSAA (attachment 0 multisampled-to-texture, depth the multisampled twin). */
typedef struct gl_fbo_key {
    GLuint color[4];
    GLenum ctarget[4];
    uint32_t face[4], level[4];
    GLuint depth_rb;
    uint32_t samples;
} gl_fbo_key;

typedef struct gl_stage {
    int pixel;
    const s2r_lib_shader *lib;    /* game shader; NULL: the fixed-function shader with `ff` */
    uint64_t variant;
    uint8_t ff[96];
    char *src;                    /* the text it compiles from (a program binary's key hashes it) */
    GLuint name;                  /* compiled when a program is linked from source (0 until then) */
    int failed;
    uint32_t uid;
    struct gl_stage *next;
} gl_stage;

typedef struct gl_program {
    uint32_t vs_uid, ps_uid;
    GLuint name;
    struct gl_program *next;
} gl_program;

typedef struct gl_query { GLuint name; GLsync fence; uint32_t type; int active, ended; } gl_query;

enum { RING_CHUNKS = 16, RING_CHUNK = 1u << 20 };

typedef struct gl_ring {
    GLuint name;
    GLenum target;
    uint32_t size, used, align, chunk_size, chunk, chunks;
    uint8_t *ptr;
    GLsync fence[RING_CHUNKS];
} gl_ring;

typedef struct gl_state {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface, pbuffer;
    EGLConfig config;
    ANativeWindow *window;
    uint32_t window_serial;
    int gles_minor;
    int has_aniso, has_astc;
    float max_aniso;
    s2r_buffer_storage_fn buffer_storage;
    /* Dynamic buffers in persistent coherent stores, rotated on DISCARD (PowerVR: its glBufferData
       orphaning and mapped or SubData writes into a buffer in flight wait for the GPU or copy it) */
    int persistent_buffers;
    uint64_t store_creates, store_retired;
    int swap_applied;
    pid_t thread;
    /* caches */
    gl_stage *stages[256];
    gl_program *programs[256];
    uint32_t next_uid;
    struct { uint64_t key; GLuint name; } samplers[256];
    int sampler_count;
    struct { gl_fbo_key key; GLuint fbo; } fbos[64];
    int fbo_count;
    gl_ring uniforms, stream;
    uint8_t last_block[4][4096];
    uint32_t last_block_size[4];
    /* frame fences: serial `frame` is being recorded; `completed` is the newest finished */
    uint64_t frame, completed;
    GLsync frame_fence[8];
    uint64_t frame_fence_serial[8];
    /* current GL state */
    GLuint cur_program, cur_fbo, cur_ebo;
    uint32_t enabled_attribs;
    struct { GLuint buf; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t offset; GLuint divisor; } attr[16];
    GLuint tex_unit[32]; GLenum tex_target[32]; GLuint sampler_unit[32];
    GLuint active_unit;           /* glActiveTexture's unit (texture uploads act on it) */
    GLuint vao;
    uint32_t raster_cache[48];
    int raster_valid;
    GLuint dummy[3];
    uint32_t target_w, target_h;
    float target_scale;           /* stored pixels per Direct3D pixel of the bound targets (render scale) */
    uint32_t target_colors;       /* colour attachments bound (MRT) */
    GLuint read_fbo;
    /* scene MSAA (EXT_multisampled_render_to_texture) */
    s2r_msaa_texture_fn msaa_texture;
    s2r_msaa_storage_fn msaa_storage;
    uint32_t msaa_samples;
    /* the present's filters (s2r_filter.c): programs, two output-size images used in turn */
    GLuint filter_prog[S2R_FILTER_COUNT];
    GLint filter_image_loc[S2R_FILTER_COUNT], filter_param_loc[S2R_FILTER_COUNT];
    int filter_failed[S2R_FILTER_COUNT];
    GLuint filter_tex[2], filter_fbo, filter_sampler;
    uint32_t filter_w, filter_h;
    /* present: a shader pass into the window (ARM drivers can leave a blit into the window black) */
    GLuint present_program, present_sampler[2], present_vao;
    GLint present_image_loc, present_origin_loc, present_target_loc;
    int present_failed;
    int has_s3tc;
    /* GPU time per frame (GL_EXT_disjoint_timer_query), as d3d9_gl.c measures it */
    GLuint gpu_query[6];
    uint64_t gpu_query_issued[6];
    uint32_t gpu_head, gpu_tail;
    int gpu_open, gpu_state, gpu_arm;   /* gpu_arm: begin the frame's query at its first pass */
    s2r_query_u64_fn gpu_u64;
} gl_state;

static gl_state G;

static void gl_check(const char *where)
{
    GLenum e = glGetError();
    if (e) {
        static int n;
        if (n++ < 40) LOGGL("GL error %04X at %s", e, where);
    }
}

/* ------------------------------------------------------------- EGL */
static int make_surface(void)
{
    uint32_t serial = 0;
    if (G.surface != EGL_NO_SURFACE) {
        eglMakeCurrent(G.display, G.pbuffer, G.pbuffer, G.context);
        eglDestroySurface(G.display, G.surface);
        G.surface = EGL_NO_SURFACE;
    }
    if (G.window) { ANativeWindow_release(G.window); G.window = NULL; }
    ANativeWindow *window = (ANativeWindow *)void_android_window_acquire(&serial);
    G.window_serial = serial;
    if (window) {
        G.window = window;
        G.surface = eglCreateWindowSurface(G.display, G.config, (EGLNativeWindowType)window, NULL);
        if (G.surface == EGL_NO_SURFACE || !eglMakeCurrent(G.display, G.surface, G.surface, G.context)) {
            LOGGL("window surface failed %04X", eglGetError());
            if (G.surface != EGL_NO_SURFACE) eglDestroySurface(G.display, G.surface);
            G.surface = EGL_NO_SURFACE;
            eglMakeCurrent(G.display, G.pbuffer, G.pbuffer, G.context);
        } else LOGGL("presenting into window %ux%u (serial %u)", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window), serial);
        G.swap_applied = -1;
    } else eglMakeCurrent(G.display, G.pbuffer, G.pbuffer, G.context);
    void_android_window_ack(serial);
    G.cur_fbo = (GLuint)-1;
    return G.surface != EGL_NO_SURFACE;
}

static void follow_window(void)
{
    uint32_t serial = 0;
    void *current = void_android_window_current(&serial, 0);   /* an acquired reference */
    int changed = serial != G.window_serial || (current != NULL) != (G.window != NULL);
    if (current) ANativeWindow_release((ANativeWindow *)current);
    if (changed) make_surface();
}

/* GL needs its context on this thread; the guest calls Direct3D from its main thread. */
static void own_thread(void)
{
    pid_t self = gettid();
    if (self == G.thread) return;
    static int logged;
    if (logged++ < 5) LOGGL("Direct3D called from thread %d (the context is on %d): context moved", (int)self, (int)G.thread);
    if (eglMakeCurrent(G.display, G.surface != EGL_NO_SURFACE ? G.surface : G.pbuffer,
            G.surface != EGL_NO_SURFACE ? G.surface : G.pbuffer, G.context)) G.thread = self;
}

static int has_ext(const char *name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return 1;
    }
    return 0;
}

static int ring_create(gl_ring *r, GLenum target, uint32_t chunk_size, uint32_t chunks, uint32_t align)
{
    memset(r, 0, sizeof *r);
    r->target = target;
    r->chunk_size = chunk_size;
    r->chunks = chunks > RING_CHUNKS ? RING_CHUNKS : chunks;
    r->size = r->chunks * chunk_size;
    r->align = align;
    glGenBuffers(1, &r->name);
    glBindBuffer(GL_COPY_WRITE_BUFFER, r->name);
    if (G.buffer_storage) {
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
        G.buffer_storage(GL_COPY_WRITE_BUFFER, r->size, NULL, flags);
        r->ptr = (uint8_t *)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, r->size, flags);
        if (!r->ptr) {
            glDeleteBuffers(1, &r->name);
            glGenBuffers(1, &r->name);
            glBindBuffer(GL_COPY_WRITE_BUFFER, r->name);
        }
    }
    if (!r->ptr) glBufferData(GL_COPY_WRITE_BUFFER, r->size, NULL, GL_DYNAMIC_DRAW);
    return 1;
}

/* Leaving a chunk fences what was drawn from it; entering one waits for its previous fence. */
static void ring_next_chunk(gl_ring *r)
{
    uint32_t chunk = r->chunk, next = (chunk + 1) % r->chunks;
    if (r->fence[chunk]) glDeleteSync(r->fence[chunk]);
    r->fence[chunk] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (r->fence[next]) {
        GLenum res = glClientWaitSync(r->fence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (res == GL_TIMEOUT_EXPIRED) {
            d3d_rt_wait_begin();
            do res = glClientWaitSync(r->fence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
            while (res == GL_TIMEOUT_EXPIRED);
            d3d_rt_wait_end();
        }
        glDeleteSync(r->fence[next]);
        r->fence[next] = 0;
    }
    r->chunk = next;
    r->used = next * r->chunk_size;
    if (r == &G.uniforms) memset(G.last_block_size, 0, sizeof G.last_block_size);
}

/* `size` bytes into the ring; returns the offset (UINT32_MAX when larger than a chunk). */
static uint64_t ring_bytes[2], ring_bytes_start[2], ring_frame_max[2], ring_frame_start[2];

/* Every 600 frames: what the uniform and stream rings took a frame (their size is a choice: a
   ring a frame outgrows waits for the GPU). */
static void ring_report(uint32_t frame)
{
    for (int i = 0; i < 2; ++i) {
        uint64_t used = ring_bytes[i] - ring_frame_start[i];
        if (used > ring_frame_max[i]) ring_frame_max[i] = used;
        ring_frame_start[i] = ring_bytes[i];
    }
    if (frame % 600) return;
    LOGGL("rings a frame: uniforms %.1f KiB average, %.1f KiB peak of %u KiB; stream %.1f KiB average, %.1f KiB peak of %u KiB",
        (double)(ring_bytes[0] - ring_bytes_start[0]) / 600.0 / 1024.0, (double)ring_frame_max[0] / 1024.0, G.uniforms.size >> 10,
        (double)(ring_bytes[1] - ring_bytes_start[1]) / 600.0 / 1024.0, (double)ring_frame_max[1] / 1024.0, G.stream.size >> 10);
    for (int i = 0; i < 2; ++i) { ring_bytes_start[i] = ring_bytes[i]; ring_frame_max[i] = 0; }
}

static uint32_t ring_put(gl_ring *r, const void *data, uint32_t size)
{
    uint32_t aligned = (size + r->align - 1) / r->align * r->align;
    if (aligned > r->chunk_size) return UINT32_MAX;
    ring_bytes[r == &G.stream] += aligned;
    uint32_t at = (r->used + r->align - 1) / r->align * r->align;
    if (at + aligned > (r->chunk + 1) * r->chunk_size) { ring_next_chunk(r); at = r->used; }
    if (r->ptr) memcpy(r->ptr + at, data, size);
    else {
        glBindBuffer(GL_COPY_WRITE_BUFFER, r->name);
        void *dst = glMapBufferRange(GL_COPY_WRITE_BUFFER, (GLintptr)at, (GLsizeiptr)size,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (!dst) return UINT32_MAX;
        memcpy(dst, data, size);
        glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    }
    r->used = at + aligned;
    return at;
}

static int gl_init(s2r_device *d)
{
    (void)d;
    memset(&G, 0, sizeof G);
    G.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (G.display == EGL_NO_DISPLAY || !eglInitialize(G.display, NULL, NULL)) { LOGGL("eglInitialize failed"); return 0; }
    /* RGBA8888 window buffers, as the Direct3D 9 layer asks (d3d9_gl.c) */
    const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(G.display, attrs, &G.config, 1, &n) || n < 1) { LOGGL("no EGL config"); return 0; }
    for (int minor = 2; minor >= 1 && !G.context; --minor) {
        const EGLint ctx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE };
        G.context = eglCreateContext(G.display, G.config, EGL_NO_CONTEXT, ctx);
        if (G.context) G.gles_minor = minor;
    }
    if (!G.context) { LOGGL("OpenGL ES 3.1 context failed %04X", eglGetError()); return 0; }
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    G.pbuffer = eglCreatePbufferSurface(G.display, G.config, pb);
    G.surface = EGL_NO_SURFACE;
    eglMakeCurrent(G.display, G.pbuffer, G.pbuffer, G.context);
    G.thread = gettid();
    uint32_t serial = 0;
    void *first = void_android_window_current(&serial, 1);   /* waits for the first window */
    if (first) ANativeWindow_release((ANativeWindow *)first);
    make_surface();
    LOGGL("%s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    G.has_aniso = has_ext("GL_EXT_texture_filter_anisotropic");
    G.max_aniso = 1;
    if (G.has_aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &G.max_aniso);
    G.has_astc = has_ext("GL_KHR_texture_compression_astc_ldr");
    G.has_s3tc = has_ext("GL_EXT_texture_compression_s3tc") || has_ext("GL_EXT_texture_compression_dxt1");
    G.has_s3tc = G.has_s3tc && has_ext("GL_EXT_texture_compression_s3tc");
    s2r_set_native_support(G.has_s3tc, 1, G.has_astc);
    if (void_android_gpu_strings) void_android_gpu_strings((const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v); d3d_gl_limits.max_texture = (uint32_t)v;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &v); d3d_gl_limits.max_3d_texture = (uint32_t)v;
    /* Direct3D's four simultaneous render targets (MRT): colour attachments 0-3 */
    GLint draw_buffers = 1;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &draw_buffers);
    d3d_gl_limits.max_draw_buffers = draw_buffers >= 4 ? 4 : draw_buffers > 0 ? (uint32_t)draw_buffers : 1;
    /* The launcher's scene MSAA (d3d9_gl.c init_msaa): multisampled-to-texture, resolved per tile */
    uint32_t want = (uint32_t)s2r_msaa_samples();
    if (want && has_ext("GL_EXT_multisampled_render_to_texture")) {
        G.msaa_texture = (s2r_msaa_texture_fn)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
        G.msaa_storage = (s2r_msaa_storage_fn)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
        GLint limit = 0;
        glGetIntegerv(GL_MAX_SAMPLES_EXT, &limit);
        if (G.msaa_texture && G.msaa_storage && limit >= 2) G.msaa_samples = want <= (uint32_t)limit ? want : 2;
    }
    if (want) LOGGL("scene MSAA requested %ux, using %ux", want, G.msaa_samples);
    G.buffer_storage = has_ext("GL_EXT_buffer_storage") ? (s2r_buffer_storage_fn)eglGetProcAddress("glBufferStorageEXT") : NULL;
    {
        /* PowerVR D-Series (Pixel 10): plain buffers took the render thread to 55-150 ms a frame on a lot,
           as they took the old layer before its persistent stores (STATE.md, 2026-11-13). Development
           A/B: debug.s2.glbuffers=persistent|plain. */
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        char choice[92] = "";
        __system_property_get("debug.s2.glbuffers", choice);
        int powervr = renderer && strstr(renderer, "PowerVR") != NULL;
        G.persistent_buffers = G.buffer_storage && (!strcmp(choice, "persistent") || (powervr && strcmp(choice, "plain")));
        if (G.persistent_buffers) LOGGL("dynamic buffers in persistent coherent stores (%d a buffer at most)", GL_BUF_STORES + 1);
    }
    GLint align = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    /* constants: 1.45 MiB a frame on a lot (Monty Ranch, Pixel 10a, ring_report): 8 MiB hold five frames */
    ring_create(&G.uniforms, GL_UNIFORM_BUFFER, RING_CHUNK, 8, align > 0 ? (uint32_t)align : 256);
    /* DrawPrimitiveUP data: the interface's quads, a few KiB a frame (8 MiB; 32 before) */
    ring_create(&G.stream, GL_ARRAY_BUFFER, 1u << 20, 8, 16);
    G.frame = 1;
    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);
    glGenFramebuffers(1, &G.read_fbo);
    static const uint8_t black[4] = {0, 0, 0, 255};
    glGenTextures(3, G.dummy);
    glActiveTexture(GL_TEXTURE31);
    glBindTexture(GL_TEXTURE_2D, G.dummy[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glBindTexture(GL_TEXTURE_CUBE_MAP, G.dummy[1]);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
    for (int f = 0; f < 6; ++f) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glBindTexture(GL_TEXTURE_3D, G.dummy[2]);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 1, 1, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    G.tex_unit[31] = G.dummy[2]; G.tex_target[31] = GL_TEXTURE_3D;
    G.active_unit = 31;
    G.cur_fbo = (GLuint)-1;
    G.swap_applied = -1;
    LOGGL("GLES 3.%d, ASTC %s, S3TC %s, anisotropy %.0f, UBO align %u, rings %s, %u draw buffers", G.gles_minor,
        G.has_astc ? "yes" : "no", G.has_s3tc ? "yes" : "no", G.max_aniso, G.uniforms.align,
        G.uniforms.ptr ? "persistent" : "mapped per block", d3d_gl_limits.max_draw_buffers);
    G.target_scale = 1.0f;
    gl_check("init");
    return 1;
}

static void gl_shutdown(s2r_device *d)
{
    (void)d;
    if (G.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(G.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (G.surface != EGL_NO_SURFACE) eglDestroySurface(G.display, G.surface);
    if (G.pbuffer != EGL_NO_SURFACE) eglDestroySurface(G.display, G.pbuffer);
    if (G.context != EGL_NO_CONTEXT) eglDestroyContext(G.display, G.context);
    if (G.window) ANativeWindow_release(G.window);
    memset(&G, 0, sizeof G);
}

/* ------------------------------------------------------------- formats */
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#endif
static void gl_format(uint8_t gpu, GLenum *internal, GLenum *format, GLenum *type, int *compressed)
{
    *compressed = 0;
    *format = GL_RGBA; *type = GL_UNSIGNED_BYTE;
    switch (gpu) {
    case S2R_GPU_RGBA8: *internal = GL_RGBA8; break;
    case S2R_GPU_RG8: *internal = GL_RG8; *format = GL_RG; break;
    case S2R_GPU_R8: *internal = GL_R8; *format = GL_RED; break;
    case S2R_GPU_RG8_SNORM: *internal = GL_RG8_SNORM; *format = GL_RG; *type = GL_BYTE; break;
    case S2R_GPU_RGBA8_SNORM: *internal = GL_RGBA8_SNORM; *type = GL_BYTE; break;
    case S2R_GPU_RGB565: *internal = GL_RGB565; *format = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; break;
    case S2R_GPU_RGBA4: *internal = GL_RGBA4; *type = GL_UNSIGNED_SHORT_4_4_4_4; break;
    case S2R_GPU_RGB5A1: *internal = GL_RGB5_A1; *type = GL_UNSIGNED_SHORT_5_5_5_1; break;
    case S2R_GPU_RGB10A2: *internal = GL_RGB10_A2; *type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
    case S2R_GPU_R16F: *internal = GL_R16F; *format = GL_RED; *type = GL_HALF_FLOAT; break;
    case S2R_GPU_RG16F: *internal = GL_RG16F; *format = GL_RG; *type = GL_HALF_FLOAT; break;
    case S2R_GPU_RGBA16F: *internal = GL_RGBA16F; *type = GL_HALF_FLOAT; break;
    case S2R_GPU_R32F: *internal = GL_R32F; *format = GL_RED; *type = GL_FLOAT; break;
    case S2R_GPU_RG32F: *internal = GL_RG32F; *format = GL_RG; *type = GL_FLOAT; break;
    case S2R_GPU_RGBA32F: *internal = GL_RGBA32F; *type = GL_FLOAT; break;
    case S2R_GPU_D16: *internal = GL_DEPTH_COMPONENT16; *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; break;
    case S2R_GPU_D24S8: *internal = GL_DEPTH24_STENCIL8; *format = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; break;
    case S2R_GPU_D24X8: *internal = GL_DEPTH_COMPONENT24; *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; break;
    case S2R_GPU_D32F: *internal = GL_DEPTH_COMPONENT32F; *format = GL_DEPTH_COMPONENT; *type = GL_FLOAT; break;
    case S2R_GPU_ETC2_RGB: *internal = GL_COMPRESSED_RGB8_ETC2; *compressed = 1; break;
    case S2R_GPU_ETC2_RGBA: *internal = GL_COMPRESSED_RGBA8_ETC2_EAC; *compressed = 1; break;
    case S2R_GPU_ASTC_4x4: *internal = GL_COMPRESSED_RGBA_ASTC_4x4_KHR; *compressed = 1; break;
    case S2R_GPU_ASTC_5x5: *internal = GL_COMPRESSED_RGBA_ASTC_5x5_KHR; *compressed = 1; break;
    case S2R_GPU_ASTC_6x6: *internal = GL_COMPRESSED_RGBA_ASTC_6x6_KHR; *compressed = 1; break;
    case S2R_GPU_BC1: *internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; *compressed = 1; break;
    case S2R_GPU_BC2: *internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; *compressed = 1; break;
    case S2R_GPU_BC3: *internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; *compressed = 1; break;
    default: *internal = GL_RGBA8; break;
    }
}

static GLint gl_swizzle(uint8_t s)
{
    static const GLint map[6] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE };
    return s < 6 ? map[s] : GL_ZERO;
}

static void set_active_unit(GLuint unit)
{
    if (G.active_unit == unit) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    G.active_unit = unit;
}

static void bind_unit_texture(GLuint unit, GLenum target, GLuint name)
{
    if (G.tex_unit[unit] == name && G.tex_target[unit] == target) return;
    set_active_unit(unit);
    if (G.tex_target[unit] && G.tex_target[unit] != target && G.tex_unit[unit]) glBindTexture(G.tex_target[unit], 0);
    glBindTexture(target, name);
    G.tex_unit[unit] = name;
    G.tex_target[unit] = target;
}

/* Uploads and mip generation act on the active unit's binding: the scratch unit 31 must be the
   active one even when the texture was bound there already. */
static void bind_scratch(const gl_tex *t)
{
    bind_unit_texture(31, t->target, t->name);
    set_active_unit(31);
}

static int gl_image_create(s2r_device *d, s2r_image *img)
{
    (void)d;
    own_thread();
    gl_tex *t = (gl_tex *)calloc(1, sizeof *t);
    gl_format(img->gpu, &t->internal, &t->format, &t->type, &t->compressed);
    t->width = img->width; t->height = img->height; t->levels = img->levels;
    t->depth = img->depth_stencil;
    if (img->depth_stencil) {
        glGenRenderbuffers(1, &t->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, t->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, t->internal, (GLsizei)img->width, (GLsizei)img->height);
        gl_check("depth surface");
        img->backend = t;
        return 1;
    }
    t->target = img->kind == S2R_TEX_CUBE ? GL_TEXTURE_CUBE_MAP : img->kind == S2R_TEX_3D ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    glGenTextures(1, &t->name);
    bind_scratch(t);
    GLsizei levels = (GLsizei)(img->levels ? img->levels : 1);
    if (t->target == GL_TEXTURE_3D)
        glTexStorage3D(t->target, levels, t->internal, (GLsizei)img->width, (GLsizei)img->height, (GLsizei)img->depth);
    else glTexStorage2D(t->target, levels, t->internal, (GLsizei)img->width, (GLsizei)img->height);
    glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_R, gl_swizzle(img->swizzle[0]));
    glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_G, gl_swizzle(img->swizzle[1]));
    glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_B, gl_swizzle(img->swizzle[2]));
    glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_A, gl_swizzle(img->swizzle[3]));
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    GLenum e = glGetError();
    if (e) {
        LOGGL("texture %ux%u levels %u %s: GL error %04X", img->width, img->height, img->levels, s2r_gpu_name(img->gpu), e);
        glDeleteTextures(1, &t->name);
        free(t);
        return 0;
    }
    img->backend = t;
    return 1;
}

static void forget_texture(GLuint name, GLuint rb)
{
    for (int i = 0; i < G.fbo_count; ++i) {
        const gl_fbo_key *k = &G.fbos[i].key;
        int hit = rb && k->depth_rb == rb;
        for (int c = 0; c < 4; ++c) if (name && k->color[c] == name) hit = 1;
        if (hit) {
            if (G.cur_fbo == G.fbos[i].fbo) { glBindFramebuffer(GL_FRAMEBUFFER, 0); G.cur_fbo = (GLuint)-1; }
            glDeleteFramebuffers(1, &G.fbos[i].fbo);
            G.fbos[i] = G.fbos[--G.fbo_count];
            --i;
        }
    }
    for (int u = 0; u < 32; ++u) if (name && G.tex_unit[u] == name) G.tex_unit[u] = 0;
}

static void gl_image_destroy(s2r_device *d, s2r_image *img)
{
    (void)d;
    gl_tex *t = (gl_tex *)img->backend;
    if (!t) return;
    own_thread();
    forget_texture(t->name, t->rb);
    if (t->ms_rb) { forget_texture(0, t->ms_rb); glDeleteRenderbuffers(1, &t->ms_rb); }
    if (t->name) glDeleteTextures(1, &t->name);
    if (t->rb) glDeleteRenderbuffers(1, &t->rb);
    free(t);
    img->backend = NULL;
}

static void gl_image_upload(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h, uint32_t depth, const void *data, uint32_t pitch, uint32_t slice)
{
    (void)d; (void)slice;
    gl_tex *t = (gl_tex *)img->backend;
    if (!t || !t->name) return;
    own_thread();
    bind_scratch(t);
    GLenum target = t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : t->target;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->compressed) {
        uint32_t size = s2r_gpu_pitch(img, w) * s2r_gpu_rows(img, h);
        glCompressedTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->internal, (GLsizei)size, data);
    } else {
        uint32_t bpp = s2r_gpu_texel_bytes(img->gpu);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch && pitch != w * bpp ? (GLint)(pitch / bpp) : 0);
        if (t->target == GL_TEXTURE_3D)
            glTexSubImage3D(target, (GLint)level, (GLint)x, (GLint)y, 0, (GLsizei)w, (GLsizei)h, (GLsizei)(depth ? depth : 1), t->format, t->type, data);
        else glTexSubImage2D(target, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->format, t->type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    GLenum e = glGetError();
    if (e) {
        static int logged;
        if (logged++ < 24)
            LOGGL("GL error %04X uploading %s level %u face %u at %u,%u %ux%u (image %ux%u, %u levels, d3d format %u%s), pitch %u",
                e, s2r_gpu_name(img->gpu), level, face, x, y, w, h, img->width, img->height, img->levels, img->d3dfmt,
                img->native_internal ? ", converted" : "", pitch);
    }
}

static void gl_image_mips(s2r_device *d, s2r_image *img)
{
    (void)d;
    gl_tex *t = (gl_tex *)img->backend;
    if (!t || !t->name || t->compressed) return;
    own_thread();
    bind_scratch(t);
    glGenerateMipmap(t->target);
    gl_check("mipmap");
}

/* ------------------------------------------------------------- targets */
static GLuint fbo_for(const gl_fbo_key *k, GLenum *status_out)
{
    for (int i = 0; i < G.fbo_count; ++i)
        if (!memcmp(&G.fbos[i].key, k, sizeof *k)) { if (status_out) *status_out = GL_FRAMEBUFFER_COMPLETE; return G.fbos[i].fbo; }
    if (G.fbo_count == 64) {
        if (G.cur_fbo == G.fbos[0].fbo) { glBindFramebuffer(GL_FRAMEBUFFER, 0); G.cur_fbo = (GLuint)-1; }
        glDeleteFramebuffers(1, &G.fbos[0].fbo);
        G.fbos[0] = G.fbos[--G.fbo_count];
    }
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum draws[4] = { GL_NONE, GL_NONE, GL_NONE, GL_NONE };
    GLsizei count = 1;
    for (int i = 0; i < 4; ++i) {
        if (!k->color[i]) continue;
        GLenum target = k->ctarget[i] == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + k->face[i] : GL_TEXTURE_2D;
        if (i == 0 && k->samples)
            G.msaa_texture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, k->color[0], (GLint)k->level[0], (GLsizei)k->samples);
        else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, target, k->color[i], (GLint)k->level[i]);
        draws[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        count = i + 1;
    }
    if (k->depth_rb) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, k->depth_rb);
    glDrawBuffers(count, draws);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status_out) *status_out = status;
    if (status != GL_FRAMEBUFFER_COMPLETE)
        LOGGL("framebuffer incomplete %04X (colors %u %u %u %u face %u depth %u samples %u)", status, k->color[0], k->color[1],
            k->color[2], k->color[3], k->face[0], k->depth_rb, k->samples);
    G.fbos[G.fbo_count].key = *k;
    G.fbos[G.fbo_count].fbo = fbo;
    ++G.fbo_count;
    G.cur_fbo = fbo;
    return fbo;
}

/* The framebuffer of one colour target (StretchRect, ColorFill, reads), no MSAA; the target's size. */
static GLuint target_fbo(const s2r_target *c, uint32_t *w, uint32_t *h)
{
    gl_tex *ct = c && c->img ? (gl_tex *)c->img->backend : NULL;
    gl_fbo_key k;
    memset(&k, 0, sizeof k);
    uint32_t tw = 1, th = 1;
    if (ct) {
        tw = ct->width >> c->level; th = ct->height >> c->level;
        if (!tw) tw = 1;
        if (!th) th = 1;
        k.color[0] = ct->name; k.ctarget[0] = ct->target; k.face[0] = c->face; k.level[0] = c->level;
    }
    *w = tw; *h = th;
    return fbo_for(&k, NULL);
}

/* The multisampled twin of a screen-sized depth surface (d3d9_gl.c screen_depth_msaa). */
static GLuint depth_ms(gl_tex *t, uint32_t samples)
{
    if (t->ms_rb && t->ms_samples == samples) return t->ms_rb;
    if (t->ms_rb) { forget_texture(0, t->ms_rb); glDeleteRenderbuffers(1, &t->ms_rb); }
    glGenRenderbuffers(1, &t->ms_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t->ms_rb);
    G.msaa_storage(GL_RENDERBUFFER, (GLsizei)samples, t->internal, (GLsizei)t->width, (GLsizei)t->height);
    t->ms_samples = samples;
    return t->ms_rb;
}

static void gpu_timer_begin(s2r_device *d);

/* The device's targets: render targets 0-3 of the first one's size (Direct3D requires equal
 * sizes), the depth surface, and the scene MSAA when render target 0 alone is screen-sized and
 * the depth surface too (d3d9_gl.c d3d_gl_bind_targets). */
static void bind_targets(s2r_device *d)
{
    if (G.gpu_arm) { G.gpu_arm = 0; gpu_timer_begin(d); }
    s2r_target c[4], z;
    int n = s2r_targets(d, c, &z);
    gl_fbo_key k;
    memset(&k, 0, sizeof k);
    uint32_t tw = 1, th = 1, colors = 0;
    float scale = 1.0f;
    int have = 0;
    for (int i = 0; i < n && i < 4; ++i) {
        gl_tex *ct = c[i].img ? (gl_tex *)c[i].img->backend : NULL;
        if (!ct || !ct->name) continue;
        uint32_t w = ct->width >> c[i].level, h = ct->height >> c[i].level;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!have) { tw = w; th = h; scale = s2r_scale_of(c[i].img); have = 1; }
        else if (w != tw || h != th) continue;
        k.color[i] = ct->name; k.ctarget[i] = ct->target; k.face[i] = c[i].face; k.level[i] = c[i].level;
        colors = (uint32_t)i + 1;
    }
    gl_tex *zt = z.img ? (gl_tex *)z.img->backend : NULL;
    if (zt && !zt->rb) zt = NULL;
    if (zt) {
        if (!have) { tw = zt->width; th = zt->height; scale = s2r_scale_of(z.img); }
        else if (zt->width < tw || zt->height < th) {
            static int logged;
            if (!logged++) LOGGL("depth %ux%u smaller than colour %ux%u: depth dropped for those draws", zt->width, zt->height, tw, th);
            zt = NULL;
        }
    }
    uint32_t samples = 0;
    if (G.msaa_samples && colors == 1 && k.color[0] && !k.level[0] && k.ctarget[0] == GL_TEXTURE_2D && c[0].img->screen_sized &&
        (!zt || z.img->screen_sized)) samples = G.msaa_samples;
    k.samples = samples;
    if (zt) k.depth_rb = samples ? depth_ms(zt, samples) : zt->rb;
    GLenum status = GL_FRAMEBUFFER_COMPLETE;
    GLuint fbo = fbo_for(&k, &status);
    if (samples && status != GL_FRAMEBUFFER_COMPLETE) {
        LOGGL("MSAA framebuffer unsupported (status %04X): single-sample from now on", status);
        G.msaa_samples = 0;
        k.samples = 0;
        if (zt) k.depth_rb = zt->rb;
        fbo = fbo_for(&k, NULL);
    }
    if (G.cur_fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        G.cur_fbo = fbo;
        G.raster_valid = 0;
        ++d->frame_passes;
    }
    G.target_w = tw; G.target_h = th;
    G.target_scale = scale;
    G.target_colors = colors;
}

/* ------------------------------------------------------------- frame fences */
static void frames_poll(void)
{
    for (int i = 0; i < 8; ++i) {
        if (!G.frame_fence[i]) continue;
        GLenum r = glClientWaitSync(G.frame_fence[i], 0, 0);
        if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
            if (G.frame_fence_serial[i] > G.completed) G.completed = G.frame_fence_serial[i];
            glDeleteSync(G.frame_fence[i]);
            G.frame_fence[i] = 0;
        }
    }
}

static void frames_close(void)
{
    frames_poll();
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < 8; ++i) {
        if (!G.frame_fence[i]) { slot = i; break; }
        if (G.frame_fence_serial[i] < oldest) { oldest = G.frame_fence_serial[i]; slot = i; }
    }
    if (G.frame_fence[slot]) {
        d3d_rt_wait_begin();
        while (glClientWaitSync(G.frame_fence[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull) == GL_TIMEOUT_EXPIRED) {}
        d3d_rt_wait_end();
        if (G.frame_fence_serial[slot] > G.completed) G.completed = G.frame_fence_serial[slot];
        glDeleteSync(G.frame_fence[slot]);
    }
    G.frame_fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    G.frame_fence_serial[slot] = G.frame;
    ++G.frame;
}

/* ------------------------------------------------------------- buffers */
static void persistent_store(gl_buf *g, GLuint *name, uint8_t **map)
{
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    glGenBuffers(1, name);
    glBindBuffer(GL_COPY_WRITE_BUFFER, *name);
    /* GL_DYNAMIC_STORAGE_BIT_EXT: plain (synchronized) writes still go through glBufferSubData. */
    G.buffer_storage(GL_COPY_WRITE_BUFFER, g->size, NULL, flags | GL_DYNAMIC_STORAGE_BIT_EXT);
    *map = (uint8_t *)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, g->size, flags);
    ++G.store_creates;
    if (!*map) {
        LOGGL("persistent buffer mapping failed %04X: plain buffers from now on", glGetError());
        glDeleteBuffers(1, name);
        *name = 0;
        G.persistent_buffers = 0;
    }
}

static void forget_buffer(GLuint name)
{
    for (int i = 0; i < 16; ++i) if (G.attr[i].buf == name) G.attr[i].buf = (GLuint)-1;
    if (G.cur_ebo == name) G.cur_ebo = (GLuint)-1;
}

static void *gl_buffer_create(s2r_device *d, s2r_buffer *b)
{
    (void)d;
    own_thread();
    gl_buf *g = (gl_buf *)calloc(1, sizeof *g);
    g->size = b->size;
    g->dynamic = (b->usage & D3DUSAGE_DYNAMIC) != 0;
    if (g->dynamic && G.persistent_buffers) {
        persistent_store(g, &g->name, &g->map);
        if (g->name) return g;
    }
    glGenBuffers(1, &g->name);
    glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
    glBufferData(GL_COPY_WRITE_BUFFER, b->size, NULL, g->dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    gl_check("buffer create");
    return g;
}

/* DISCARD: the store the queued draws read is fenced and set aside; the write goes into a spare the GPU
   is done with (polled, never waited for), else a new store; with all spares busy the oldest is deleted,
   which the driver defers until its draws are done. */
static void persistent_discard(gl_buf *g)
{
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    int free_slot = -1, ready = -1;
    for (int i = 0; i < GL_BUF_STORES; ++i) {
        if (!g->spare[i]) { if (free_slot < 0) free_slot = i; continue; }
        if (ready >= 0) continue;
        GLenum r = g->spare_fence[i] ? glClientWaitSync(g->spare_fence[i], 0, 0) : GL_ALREADY_SIGNALED;
        if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) ready = i;
    }
    GLuint name = 0;
    uint8_t *map = NULL;
    if (ready >= 0) {
        name = g->spare[ready]; map = g->spare_map[ready];
        if (g->spare_fence[ready]) glDeleteSync(g->spare_fence[ready]);
        g->spare[ready] = 0; g->spare_fence[ready] = 0;
        if (free_slot < 0) free_slot = ready;
    } else {
        if (free_slot < 0) {
            free_slot = 0;   /* all busy: retire the first (deleted while in flight: the driver keeps it) */
            forget_buffer(g->spare[0]);
            glDeleteBuffers(1, &g->spare[0]);
            if (g->spare_fence[0]) glDeleteSync(g->spare_fence[0]);
            g->spare[0] = 0; g->spare_fence[0] = 0;
            ++G.store_retired;
        }
        persistent_store(g, &name, &map);
        if (!name) {   /* mapping failed: this buffer becomes a plain one */
            if (fence) glDeleteSync(fence);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
            g->map = NULL;
            return;
        }
    }
    g->spare[free_slot] = g->name; g->spare_map[free_slot] = g->map; g->spare_fence[free_slot] = fence;
    g->name = name; g->map = map;
}

/* DISCARD orphans the storage (draws in flight keep the old one), NOOVERWRITE writes unsynchronized
 * (the game promises no draw reads those bytes), a plain write goes through glBufferSubData. */
static void gl_buffer_write(s2r_device *d, s2r_buffer *b, uint32_t offset, uint32_t size, const void *data, int mode)
{
    (void)d;
    gl_buf *g = (gl_buf *)b->backend;
    if (!g) return;
    own_thread();
    if (g->map) {
        if (mode == S2R_WRITE_DISCARD) persistent_discard(g);
        if (!data || offset >= g->size) return;
        if (offset + size > g->size) size = g->size - offset;
        if (g->map && mode != S2R_WRITE_PLAIN) {
            memcpy(g->map + offset, data, size);
            /* Keeps PowerVR's vertex and index fetch in step with the persistent CPU writes (without it
               the old layer drew stale Sim portraits, 2026-11-13). */
            glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);
            return;
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
        if (g->map) { glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data); return; }
        if (mode == S2R_WRITE_DISCARD) glBufferData(GL_COPY_WRITE_BUFFER, g->size, NULL, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
        return;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
    if (mode == S2R_WRITE_DISCARD) glBufferData(GL_COPY_WRITE_BUFFER, g->size, NULL, g->dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    if (!data || offset >= g->size) return;
    if (offset + size > g->size) size = g->size - offset;
    if (mode == S2R_WRITE_NOOVERWRITE) {
        void *dst = glMapBufferRange(GL_COPY_WRITE_BUFFER, offset, size,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (dst) { memcpy(dst, data, size); glUnmapBuffer(GL_COPY_WRITE_BUFFER); return; }
    }
    glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
}

static void gl_buffer_destroy(s2r_device *d, s2r_buffer *b)
{
    (void)d;
    gl_buf *g = (gl_buf *)b->backend;
    if (!g) return;
    own_thread();
    forget_buffer(g->name);
    glDeleteBuffers(1, &g->name);
    for (int i = 0; i < GL_BUF_STORES; ++i) {
        if (g->spare[i]) { forget_buffer(g->spare[i]); glDeleteBuffers(1, &g->spare[i]); }
        if (g->spare_fence[i]) glDeleteSync(g->spare_fence[i]);
    }
    free(g);
    b->backend = NULL;
}

/* ------------------------------------------------------------- programs */
static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    d3d_rt_wait_begin();
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    d3d_rt_wait_end();
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        LOGGL("GLSL compile failed: %s", log);
        LOGGL("source:\n%s", src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static gl_stage *stage_get(int pixel, const s2r_lib_shader *lib, uint64_t variant, const uint8_t *ff, size_t ff_size,
    const s2r_draw *draw, const uint8_t modes[16])
{
    unsigned bucket = ((uint32_t)(uintptr_t)lib * 2654435761u ^ (uint32_t)variant ^ (uint32_t)(variant >> 32) ^
                       (lib ? 0 : (pixel ? draw->ff.ps_hash : draw->ff.vs_hash)) ^ (uint32_t)pixel) & 255u;
    for (gl_stage *s = G.stages[bucket]; s; s = s->next) {
        if (s->pixel != pixel || s->lib != lib || s->variant != variant) continue;
        if (!lib && memcmp(s->ff, ff, ff_size)) continue;
        return s->failed ? NULL : s;
    }
    static char defines[8192];
    const char *body;
    if (lib) {
        body = lib->text;
        s2r_shader_defines(defines, sizeof defines, pixel, modes, draw->alpha_func, draw->fog, draw->fog_depth);
    } else {
        if (!s2r_ff_valid(&draw->ff, pixel)) return NULL;
        body = s2r_builtin_gles(pixel ? S2R_BUILTIN_FF_PS : S2R_BUILTIN_FF_VS);
        if (!body) return NULL;
        s2r_ff_defines(&draw->ff, pixel, defines, sizeof defines);
        if (!pixel) {
            size_t n = strlen(defines);
            s2r_shader_defines(defines + n, sizeof defines - n, 0, modes, 0, 0, 0);
        }
    }
    char *src = s2r_gles_program_text(body, defines);
    if (pixel && s2r_ui_fsr()) {
        /* UI FSR 1: the interface's 2D reads through EASU (port_vpos.w, set per draw) */
        char *filtered = s2r_ui_filter_source(src);
        if (filtered) { free(src); src = filtered; }
    }
    if (s2r_trace_frame) LOGGL("%s %s source:\n%s", lib ? lib->name : "fixed-function", pixel ? "pixel" : "vertex", src);
    gl_stage *s = (gl_stage *)calloc(1, sizeof *s);
    s->pixel = pixel; s->lib = lib; s->variant = variant;
    if (!lib) memcpy(s->ff, ff, ff_size);
    s->src = src;
    s->uid = ++G.next_uid;
    s->next = G.stages[bucket];
    G.stages[bucket] = s;
    return s;
}

/* A stage's shader object, compiled the first time a program links from source. */
static int stage_compile(gl_stage *s)
{
    if (s->name) return 1;
    if (s->failed || !s->src) return 0;
    s->name = compile(s->pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, s->src);
    if (!s->name) s->failed = 1;
    return s->name != 0;
}

static gl_program *program_get(s2r_device *d, const s2r_draw *draw)
{
    uint8_t modes[16];
    uint64_t decode = s2r_decode_modes(d, draw->vs, 0, modes) | (uint64_t)draw->fog_depth << 48;
    gl_stage *vs = draw->vs ? stage_get(0, draw->vs, decode, NULL, 0, draw, modes)
                            : stage_get(0, NULL, decode, draw->ff.vs, sizeof draw->ff.vs, draw, modes);
    uint64_t ps_variant = draw->ps ? (uint64_t)draw->alpha_func | ((uint64_t)draw->fog << 4) : 0;
    gl_stage *ps = draw->ps ? stage_get(1, draw->ps, ps_variant, NULL, 0, draw, NULL)
                            : stage_get(1, NULL, 0, draw->ff.ps, sizeof draw->ff.ps, draw, NULL);
    if (!vs || !ps) return NULL;
    unsigned slot = (vs->uid * 31u + ps->uid) & 255u;
    for (gl_program *p = G.programs[slot]; p; p = p->next)
        if (p->vs_uid == vs->uid && p->ps_uid == ps->uid) return p->name ? p : NULL;
    gl_program *p = (gl_program *)calloc(1, sizeof *p);
    p->vs_uid = vs->uid; p->ps_uid = ps->uid;
    /* The Direct3D 9 layer's program binaries (<cache>/glprog, d3d9_shader.c): a program linked in
       an earlier session loads without compiling, keyed by the exact text of both halves and the
       driver; the shader compiler's memory then stays unused. */
    d3d_program_key key = d3d_program_key_of(vs->src, ps->src, NULL);
    GLuint prog = d3d_program_cache_load(key);
    if (prog) {
        p->name = prog;
        p->next = G.programs[slot];
        G.programs[slot] = p;
        return p;
    }
    if (!stage_compile(vs) || !stage_compile(ps)) { p->next = G.programs[slot]; G.programs[slot] = p; return NULL; }
    prog = glCreateProgram();
    glAttachShader(prog, vs->name);
    glAttachShader(prog, ps->name);
    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    d3d_rt_wait_begin();
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    d3d_rt_wait_end();
    if (ok) d3d_program_cache_store(key, prog);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        LOGGL("link failed (vs %s ps %s): %s", draw->vs ? draw->vs->name : "ff", draw->ps ? draw->ps->name : "ff", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    p->name = prog;
    p->next = G.programs[slot];
    G.programs[slot] = p;
    return prog ? p : NULL;
}

/* ------------------------------------------------------------- uniforms */
static void upload_block(int binding, const void *data, uint32_t size)
{
    if (!size) return;
    if (size <= sizeof G.last_block[0] && size == G.last_block_size[binding] && !memcmp(G.last_block[binding], data, size)) return;
    uint32_t at = ring_put(&G.uniforms, data, size);
    if (at == UINT32_MAX) return;
    glBindBufferRange(GL_UNIFORM_BUFFER, (GLuint)binding, G.uniforms.name, (GLintptr)at, size);
    if (size <= sizeof G.last_block[0]) { memcpy(G.last_block[binding], data, size); G.last_block_size[binding] = size; }
    else G.last_block_size[binding] = 0;
}

/* ------------------------------------------------------------- vertex input */
static int gl_vertex_format(uint8_t type, GLint *size, GLenum *gltype, GLboolean *norm)
{
    switch (type) {
    case 0: *size = 1; *gltype = GL_FLOAT; *norm = GL_FALSE; return 1;
    case 1: *size = 2; *gltype = GL_FLOAT; *norm = GL_FALSE; return 1;
    case 2: *size = 3; *gltype = GL_FLOAT; *norm = GL_FALSE; return 1;
    case 3: *size = 4; *gltype = GL_FLOAT; *norm = GL_FALSE; return 1;
    case 4: *size = 4; *gltype = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return 1;
    case 5: *size = 4; *gltype = GL_UNSIGNED_BYTE; *norm = GL_FALSE; return 1;
    case 6: *size = 2; *gltype = GL_SHORT; *norm = GL_FALSE; return 1;
    case 7: *size = 4; *gltype = GL_SHORT; *norm = GL_FALSE; return 1;
    case 8: *size = 4; *gltype = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return 1;
    case 9: *size = 2; *gltype = GL_SHORT; *norm = GL_TRUE; return 1;
    case 10: *size = 4; *gltype = GL_SHORT; *norm = GL_TRUE; return 1;
    case 11: *size = 2; *gltype = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return 1;
    case 12: *size = 4; *gltype = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return 1;
    case 13: *size = 4; *gltype = GL_UNSIGNED_INT_2_10_10_10_REV; *norm = GL_FALSE; return 1;
    case 14: *size = 4; *gltype = GL_INT_2_10_10_10_REV; *norm = GL_TRUE; return 1;
    case 15: *size = 2; *gltype = GL_HALF_FLOAT; *norm = GL_FALSE; return 1;
    case 16: *size = 4; *gltype = GL_HALF_FLOAT; *norm = GL_FALSE; return 1;
    default: return 0;
    }
}

/* Every input location of the vertex half from its declaration element; `up` the UP data's ring offset. */
static void setup_vertices(s2r_device *d, const s2r_draw *draw, int32_t base_shift, uint32_t up)
{
    uint32_t wanted = 0;
    uint32_t inputs = draw->vs ? draw->vs->input_mask : S2R_FF_INPUTS;
    for (uint32_t loc = 0; loc < 16; ++loc) {
        if (!(inputs & (1u << loc))) continue;
        const d3d_vertex_element *e = s2r_input_element(d, draw->vs, loc);
        if (!e) continue;
        GLint size; GLenum type; GLboolean norm;
        if (!gl_vertex_format(e->type, &size, &type, &norm)) continue;
        GLuint buffer;
        uint32_t stride;
        uintptr_t offset;
        if (draw->up_vertices) {
            if (e->stream) continue;
            buffer = G.stream.name;
            stride = draw->up_stride;
            offset = up + e->offset;
        } else {
            s2r_buffer *b = e->stream < 16 ? s2r_buffer_of((d3d_buffer *)d->dev->st.streams[e->stream]) : NULL;
            gl_buf *g = b ? (gl_buf *)b->backend : NULL;
            if (!g) continue;
            buffer = g->name;
            stride = d->dev->st.stream_stride[e->stream];
            offset = d->dev->st.stream_offset[e->stream] + e->offset + (uintptr_t)((int64_t)base_shift * stride);
        }
        wanted |= 1u << loc;
        GLuint divisor = draw->instances && e->stream < 16 && (d->dev->st.stream_freq[e->stream] & 0x80000000u) ? 1u : 0u;
        if (G.attr[loc].divisor != divisor) { glVertexAttribDivisor(loc, divisor); G.attr[loc].divisor = divisor; }
        if (G.attr[loc].buf != buffer || G.attr[loc].size != size || G.attr[loc].type != type ||
            G.attr[loc].norm != norm || G.attr[loc].stride != (GLsizei)stride || G.attr[loc].offset != offset) {
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glVertexAttribPointer(loc, size, type, norm, (GLsizei)stride, (const void *)offset);
            G.attr[loc].buf = buffer; G.attr[loc].size = size; G.attr[loc].type = type;
            G.attr[loc].norm = norm; G.attr[loc].stride = (GLsizei)stride; G.attr[loc].offset = offset;
        }
    }
    uint32_t change = wanted ^ G.enabled_attribs;
    for (uint32_t loc = 0; loc < 16; ++loc) {
        if (!(change & (1u << loc))) continue;
        if (wanted & (1u << loc)) glEnableVertexAttribArray(loc);
        else { glDisableVertexAttribArray(loc); glVertexAttrib4f(loc, 0, 0, 0, 1); }
    }
    G.enabled_attribs = wanted;
}

/* ------------------------------------------------------------- samplers */
static GLenum gl_cmp(uint32_t f) { return f >= 1 && f <= 8 ? GL_NEVER + f - 1 : GL_ALWAYS; }

static GLuint sampler_for(const uint32_t *ss, uint32_t levels, uint32_t first_level)
{
    uint32_t mag = ss[5], min = ss[6], mip = levels > 1 ? ss[7] : 0;
    uint32_t au = ss[1], av = ss[2], aw = ss[3];
    uint32_t aniso = (min == 3 || mag == 3) ? (ss[10] ? ss[10] : 1) : 1;
    uint32_t max_level = first_level;                     /* s2r_stage_first_level */
    float bias;
    memcpy(&bias, &ss[8], 4);
    if (!(bias > -16.0f && bias < 16.0f)) bias = 0.0f;
    int32_t bias_q = (int32_t)(bias * 64.0f);
    uint64_t key = (uint64_t)mag | (uint64_t)min << 4 | (uint64_t)mip << 8 | (uint64_t)au << 12 | (uint64_t)av << 16 |
                   (uint64_t)aw << 20 | (uint64_t)(aniso & 31) << 24 | (uint64_t)(max_level & 15) << 29 | (uint64_t)(uint16_t)bias_q << 33;
    for (int i = 0; i < G.sampler_count; ++i) if (G.samplers[i].key == key) return G.samplers[i].name;
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
    GLenum minf;
    int lin = min != 1;
    if (mip == 0) minf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) minf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    static const GLint wraps[6] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT };
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wraps[au < 6 ? au : 1]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wraps[av < 6 ? av : 1]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, wraps[aw < 6 ? aw : 1]);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)max_level);
    if (bias != 0.0f) {
        static int logged;
        if (!logged++) LOGGL("mip LOD bias %.2f is not applied (no GL ES sampler parameter)", bias);
    }
    if (G.has_aniso && aniso > 1) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)aniso < G.max_aniso ? (float)aniso : G.max_aniso);
    if (G.sampler_count < 256) { G.samplers[G.sampler_count].key = key; G.samplers[G.sampler_count].name = s; ++G.sampler_count; }
    return s;
}

/* Units 4..19 pixel samplers, 20..23 vertex samplers (the library's bindings). */
static void bind_textures(s2r_device *d, const s2r_draw *draw, const s2r_target *color)
{
    /* a texture bound as a render target reads as black (Direct3D: undefined) */
    uint32_t ps_mask = draw->ps ? draw->ps->samplers : 0xFFu;
    uint32_t vs_mask = draw->vs ? draw->vs->samplers & 15u : 0;
    for (uint32_t i = 0; i < 20; ++i) {
        uint32_t stage = i < 16 ? i : 16 + (i - 16);
        int pixel = i < 16;
        if (pixel ? !(ps_mask & (1u << i)) : !(vs_mask & (1u << (i - 16)))) continue;
        GLuint unit = 4 + i;
        s2r_image *img = s2r_stage_image(d, stage);
        gl_tex *g = img ? (gl_tex *)img->backend : NULL;
        uint32_t want_kind;
        if (pixel && draw->ps) want_kind = (draw->ps->sampler_types >> (2 * i)) & 3u;
        else if (!pixel && draw->vs) want_kind = (draw->vs->sampler_types >> (2 * (i - 16))) & 3u;
        else want_kind = img ? (uint32_t)img->kind : 0u;
        GLenum want = want_kind == 1 ? GL_TEXTURE_CUBE_MAP : want_kind == 2 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
        int bound = 0;
        for (int c = 0; color && c < 4; ++c) if (color[c].img && color[c].img == img) bound = 1;
        if (!g || !g->name || g->target != want || bound) {
            bind_unit_texture(unit, want, G.dummy[want_kind > 2 ? 0 : want_kind]);
            continue;
        }
        bind_unit_texture(unit, g->target, g->name);
        GLuint sampler = sampler_for(d->dev->st.ss[stage], img->levels, s2r_stage_first_level(d, stage, img->levels));
        if (G.sampler_unit[unit] != sampler) { glBindSampler(unit, sampler); G.sampler_unit[unit] = sampler; }
    }
}

/* ------------------------------------------------------------- raster state */
static GLenum gl_blend(uint32_t b)
{
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA; case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE; case 14: return GL_CONSTANT_COLOR; case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}

static GLenum gl_stencil_op(uint32_t op)
{
    static const GLenum ops[9] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return op < 9 ? ops[op] : GL_KEEP;
}

static GLenum gl_blend_op(uint32_t op)
{
    static const GLenum ops[6] = { GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
    return ops[op < 6 ? op : 1];
}

static void apply_raster(s2r_device *d, int has_depth)
{
    const uint32_t *rs = d->dev->st.rs;
    uint32_t key[48];
    static const uint16_t fields[] = { 7, 23, 14, 52, 53, 54, 55, 56, 57, 58, 59, 27, 19, 20, 171, 168, 22, 174, 206, 207,
        208, 209, 185, 186, 187, 188, 189, 193, 195, 175, 190, 191, 192 };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; ++i) key[n++] = rs[fields[i]];
    key[n++] = (uint32_t)has_depth;
    uint32_t viewport[4], scissor[4];
    memcpy(viewport, d->dev->st.viewport, sizeof viewport);
    memcpy(scissor, d->dev->st.scissor, sizeof scissor);
    memcpy(&key[n], viewport, 16); n += 4;
    memcpy(&key[n++], &d->dev->st.viewport_z[0], 4); memcpy(&key[n++], &d->dev->st.viewport_z[1], 4);
    memcpy(&key[n], scissor, 16); n += 4;
    key[n++] = G.cur_fbo;
    memcpy(&key[n++], &G.target_scale, 4);
    key[n++] = G.target_colors;
    if (G.raster_valid && !memcmp(key, G.raster_cache, n * 4)) return;
    memcpy(G.raster_cache, key, n * 4);
    G.raster_valid = 1;
    int ztest = has_depth && rs[7] != 0;
    if (ztest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(gl_cmp(rs[23]));
    glDepthMask(has_depth && rs[14] ? GL_TRUE : GL_FALSE);
    if (has_depth && rs[52]) {
        glEnable(GL_STENCIL_TEST);
        /* GL's front faces are D3D's clockwise ones (glFrontFace(GL_CCW) on the flipped image). */
        glStencilFuncSeparate(GL_FRONT, gl_cmp(rs[56]), (GLint)(rs[57] & 0xFF), rs[58]);
        glStencilOpSeparate(GL_FRONT, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        if (rs[185]) {
            glStencilFuncSeparate(GL_BACK, gl_cmp(rs[189]), (GLint)(rs[57] & 0xFF), rs[58]);
            glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[186]), gl_stencil_op(rs[187]), gl_stencil_op(rs[188]));
        } else {
            glStencilFuncSeparate(GL_BACK, gl_cmp(rs[56]), (GLint)(rs[57] & 0xFF), rs[58]);
            glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        }
        glStencilMask(rs[59]);
    } else glDisable(GL_STENCIL_TEST);
    if (rs[27]) {
        glEnable(GL_BLEND);
        if (rs[206]) {
            glBlendFuncSeparate(gl_blend(rs[19]), gl_blend(rs[20]), gl_blend(rs[207]), gl_blend(rs[208]));
            glBlendEquationSeparate(gl_blend_op(rs[171]), gl_blend_op(rs[209]));
        } else {
            glBlendFunc(gl_blend(rs[19]), gl_blend(rs[20]));
            glBlendEquation(gl_blend_op(rs[171]));
        }
        uint32_t bf = rs[193];
        glBlendColor((float)((bf >> 16) & 255) / 255.0f, (float)((bf >> 8) & 255) / 255.0f, (float)(bf & 255) / 255.0f,
            (float)(bf >> 24) / 255.0f);
    } else glDisable(GL_BLEND);
    uint32_t cw = rs[168];
    glColorMask((cw & 1) != 0, (cw & 2) != 0, (cw & 4) != 0, (cw & 8) != 0);
    /* D3DRS_COLORWRITEENABLE1-3 for render targets 1-3 (MRT; glColorMaski is OpenGL ES 3.2) */
    for (uint32_t i = 1; G.gles_minor >= 2 && i < G.target_colors && i < 4; ++i) {
        uint32_t m = rs[189 + i];
        glColorMaski(i, (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
    }
    /* cull: targets are stored top-down, so D3D's clockwise front is GL's CCW */
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK); }
    float bias, slope;
    memcpy(&bias, &rs[195], 4);
    memcpy(&slope, &rs[175], 4);
    if (has_depth && (bias != 0.0f || slope != 0.0f)) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, bias * 16777215.0f);
    } else glDisable(GL_POLYGON_OFFSET_FILL);
    /* Direct3D pixels in the target's stored pixels (the render scale), edges scaled alike */
    float k = G.target_scale;
    int32_t vx = s2r_scaled((int32_t)viewport[0], k), vy = s2r_scaled((int32_t)viewport[1], k);
    glViewport(vx, vy, s2r_scaled((int32_t)(viewport[0] + viewport[2]), k) - vx,
        s2r_scaled((int32_t)(viewport[1] + viewport[3]), k) - vy);
    glDepthRangef(d->dev->st.viewport_z[0], d->dev->st.viewport_z[1]);
    if (rs[174]) {
        glEnable(GL_SCISSOR_TEST);
        int32_t sx = s2r_scaled((int32_t)scissor[0], k), sy = s2r_scaled((int32_t)scissor[1], k);
        glScissor(sx, sy, s2r_scaled((int32_t)(scissor[0] + scissor[2]), k) - sx, s2r_scaled((int32_t)(scissor[1] + scissor[3]), k) - sy);
    } else glDisable(GL_SCISSOR_TEST);
}

/* ------------------------------------------------------------- clear, draw */
static void clear_rects(uint32_t flags, const int32_t *rects, uint32_t count, uint32_t color, float z, uint32_t stencil,
    int has_color, int has_depth)
{
    if (!has_color) flags &= ~1u;
    if (!has_depth) flags &= ~6u;
    if (!flags) return;
    G.raster_valid = 0;
    GLbitfield bits = 0;
    if (flags & 1) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((float)((color >> 16) & 255) / 255.0f, (float)((color >> 8) & 255) / 255.0f,
            (float)(color & 255) / 255.0f, (float)(color >> 24) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 2) { glDepthMask(GL_TRUE); glClearDepthf(z); bits |= GL_DEPTH_BUFFER_BIT; }
    if (flags & 4) { glStencilMask(0xFF); glClearStencil((GLint)stencil); bits |= GL_STENCIL_BUFFER_BIT; }
    for (uint32_t i = 0; i < count; ++i) {
        int32_t x0 = rects[4 * i], y0 = rects[4 * i + 1], x1 = rects[4 * i + 2], y1 = rects[4 * i + 3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)G.target_w) x1 = (int32_t)G.target_w;
        if (y1 > (int32_t)G.target_h) y1 = (int32_t)G.target_h;
        if (x1 <= x0 || y1 <= y0) continue;
        if (x0 == 0 && y0 == 0 && x1 == (int32_t)G.target_w && y1 == (int32_t)G.target_h) glDisable(GL_SCISSOR_TEST);
        else { glEnable(GL_SCISSOR_TEST); glScissor(x0, y0, x1 - x0, y1 - y0); }
        glClear(bits);
    }
    gl_check("clear");
}

static void gl_clear(s2r_device *d, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z,
    uint32_t stencil)
{
    own_thread();
    bind_targets(d);
    const d3d_state *ds = &d->dev->st;
    int32_t x0 = (int32_t)ds->viewport[0], y0 = (int32_t)ds->viewport[1];
    int32_t x1 = x0 + (int32_t)ds->viewport[2], y1 = y0 + (int32_t)ds->viewport[3];
    if (ds->rs[174]) {
        int32_t sl = (int32_t)ds->scissor[0], st = (int32_t)ds->scissor[1];
        int32_t sr = sl + (int32_t)ds->scissor[2], sb = st + (int32_t)ds->scissor[3];
        if (sl > x0) x0 = sl;
        if (st > y0) y0 = st;
        if (sr < x1) x1 = sr;
        if (sb < y1) y1 = sb;
    }
    int32_t clipped[64 * 4];
    uint32_t n = 0;
    if (!count) { clipped[0] = x0; clipped[1] = y0; clipped[2] = x1; clipped[3] = y1; n = 1; }
    else for (uint32_t i = 0; i < count && i < 64; ++i) {
        int32_t a = rects[4 * i] > x0 ? rects[4 * i] : x0, b = rects[4 * i + 1] > y0 ? rects[4 * i + 1] : y0;
        int32_t c = rects[4 * i + 2] < x1 ? rects[4 * i + 2] : x1, e = rects[4 * i + 3] < y1 ? rects[4 * i + 3] : y1;
        if (c <= a || e <= b) continue;
        clipped[4 * n] = a; clipped[4 * n + 1] = b; clipped[4 * n + 2] = c; clipped[4 * n + 3] = e;
        ++n;
    }
    s2r_target c[4], zt;
    s2r_targets(d, c, &zt);
    for (uint32_t i = 0; i < 4 * n; ++i) clipped[i] = s2r_scaled(clipped[i], G.target_scale);
    clear_rects(flags, clipped, n, color, z, stencil, G.target_colors != 0, zt.img != NULL);
}

static void gl_fill(s2r_device *d, const s2r_target *dst, const uint32_t *rect, uint32_t color)
{
    (void)d;
    own_thread();
    uint32_t w, h;
    GLuint fbo = target_fbo(dst, &w, &h);
    if (G.cur_fbo != fbo) { glBindFramebuffer(GL_FRAMEBUFFER, fbo); G.cur_fbo = fbo; }
    G.target_w = w; G.target_h = h;
    G.target_colors = 1;
    float k = s2r_scale_of(dst->img);
    int32_t r[4] = { s2r_scaled((int32_t)rect[0], k), s2r_scaled((int32_t)rect[1], k), s2r_scaled((int32_t)rect[2], k),
                     s2r_scaled((int32_t)rect[3], k) };
    clear_rects(1, r, 1, color, 1.0f, 0, 1, 0);
    G.raster_valid = 0;
}

static GLenum gl_prim(uint32_t type)
{
    switch (type) {
    case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLE_STRIP; case 6: return GL_TRIANGLE_FAN; default: return GL_TRIANGLES;
    }
}

static void gl_draw(s2r_device *d, const s2r_draw *draw)
{
    own_thread();
    bind_targets(d);
    s2r_target c[4], zt;
    s2r_targets(d, c, &zt);
    gl_program *p = program_get(d, draw);
    if (!p) return;
    if (G.cur_program != p->name) { glUseProgram(p->name); G.cur_program = p->name; }
    uint32_t up = 0, up_index = 0;
    if (draw->up_vertices) {
        up = ring_put(&G.stream, draw->up_vertices, draw->up_vertex_bytes);
        if (up == UINT32_MAX) return;
        if (draw->up_indices) {
            up_index = ring_put(&G.stream, draw->up_indices, draw->up_index_bytes);
            if (up_index == UINT32_MAX) return;
        }
    }
    setup_vertices(d, draw, 0, up);
    static uint8_t constants[256 * 16 + 64 * 16];
    extern int s2r_trace_frame;
    for (int stage = 0; stage < 2; ++stage) {
        const s2r_lib_shader *x = stage ? draw->ps : draw->vs;
        if (!x) continue;
        uint32_t bytes = s2r_pack_constants(x, &d->dev->st, constants, sizeof constants);
        if (s2r_trace_frame && bytes) {
            /* development: the packed constants as d3d9_shader.c's trace prints them */
            const float *f = (const float *)constants;
            char line[900];
            int n = 0;
            uint32_t total = bytes / 16;
            for (uint32_t i = 0; i < total && n < 820; ++i) {
                if (i >= 8 && i + 8 < total) continue;
                n += snprintf(line + n, sizeof line - (size_t)n, " [%u](%g %g %g %g)", i, (double)f[4 * i], (double)f[4 * i + 1],
                    (double)f[4 * i + 2], (double)f[4 * i + 3]);
            }
            LOGGL("  %s constants %016llx:%s", stage ? "ps" : "vs", (unsigned long long)x->hash, line);
        }
        upload_block(stage, constants, bytes);
    }
    if (!draw->vs || !draw->ps) {
        static s2r_ff_uniforms ff;
        s2r_ff_uniforms_fill(d, draw, &ff);
        upload_block(2, &ff, sizeof ff);
    }
    s2r_port_block port;
    s2r_port_block_fill(d, draw, G.target_scale, G.target_h, &port);
    upload_block(3, &port, sizeof port);
    bind_textures(d, draw, c);
    gl_tex *zt_gl = zt.img ? (gl_tex *)zt.img->backend : NULL;
    apply_raster(d, zt_gl != NULL);
    GLenum prim = gl_prim(draw->primitive);
    if (draw->indexed) {
        GLuint ebo;
        GLenum type;
        uintptr_t offset;
        int32_t base = draw->base_vertex;
        if (draw->up_indices) {
            ebo = G.stream.name;
            type = draw->up_index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            offset = up_index;
            base = 0;
        } else {
            s2r_buffer *ib = s2r_buffer_of((d3d_buffer *)d->dev->st.indices);
            gl_buf *g = ib ? (gl_buf *)ib->backend : NULL;
            if (!g) return;
            ebo = g->name;
            type = ib->format == 102 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            offset = (uintptr_t)draw->start * (ib->format == 102 ? 4u : 2u);
        }
        if (G.cur_ebo != ebo) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); G.cur_ebo = ebo; }
        if (draw->instances)
            glDrawElementsInstancedBaseVertex(prim, (GLsizei)draw->count, type, (const void *)offset, (GLsizei)draw->instances, base);
        else if (base) glDrawElementsBaseVertex(prim, (GLsizei)draw->count, type, (const void *)offset, base);
        else glDrawElements(prim, (GLsizei)draw->count, type, (const void *)offset);
    } else if (draw->instances)
        glDrawArraysInstanced(prim, draw->up_vertices ? 0 : (GLint)draw->start, (GLsizei)draw->count, (GLsizei)draw->instances);
    else glDrawArrays(prim, draw->up_vertices ? 0 : (GLint)draw->start, (GLsizei)draw->count);
    gl_check("draw");
}

/* ------------------------------------------------------------- copies */
static int gl_blit(s2r_device *d, const s2r_target *src, const uint32_t *sr, const s2r_target *dst, const uint32_t *dr, int linear)
{
    (void)d;
    own_thread();
    gl_tex *s = (gl_tex *)src->img->backend, *t = (gl_tex *)dst->img->backend;
    if (!s || !t || s->compressed || t->compressed || s->depth != t->depth) return 0;
    uint32_t w, h;
    gl_fbo_key zk;
    memset(&zk, 0, sizeof zk);
    zk.depth_rb = s->rb;
    GLuint sf = s->depth ? fbo_for(&zk, NULL) : target_fbo(src, &w, &h);
    zk.depth_rb = t->rb;
    GLuint tf = t->depth ? fbo_for(&zk, NULL) : target_fbo(dst, &w, &h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tf);
    glDisable(GL_SCISSOR_TEST);
    /* rects in each image's stored pixels (the render scale) */
    float ks = s2r_scale_of(src->img), kd = s2r_scale_of(dst->img);
    GLint a[4], b[4];
    for (int i = 0; i < 4; ++i) { a[i] = s2r_scaled((int32_t)sr[i], ks); b[i] = s2r_scaled((int32_t)dr[i], kd); }
    int same = a[2] - a[0] == b[2] - b[0] && a[3] - a[1] == b[3] - b[1];
    glBlitFramebuffer(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
        s->depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT,
        s->depth || same || !linear ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    G.cur_fbo = (GLuint)-1;
    G.raster_valid = 0;
    gl_check("StretchRect");
    return 1;
}

static int gl_image_read(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, const uint32_t *rect, void *out, uint32_t pitch)
{
    (void)d;
    gl_tex *t = (gl_tex *)img->backend;
    if (!t || !t->name || t->compressed || t->depth) return 0;
    own_thread();
    GLenum target = t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
    if (t->target == GL_TEXTURE_3D) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, G.read_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, t->name, (GLint)level);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        uint32_t bpp = s2r_gpu_texel_bytes(img->gpu);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(pitch / bpp));
        glReadPixels((GLint)rect[0], (GLint)rect[1], (GLsizei)(rect[2] - rect[0]), (GLsizei)(rect[3] - rect[1]), t->format, t->type, out);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    G.cur_fbo = (GLuint)-1;
    gl_check("read");
    return ok;
}

/* ------------------------------------------------------------- present */
static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = vs && fs ? glCreateProgram() : 0;
    if (prog) {
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof log, NULL, log);
            LOGGL("present program link failed: %s", log);
            glDeleteProgram(prog);
            prog = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return prog;
}

/* GPU time per frame: one TIME_ELAPSED query spans the game's passes of a
 * frame (gl_present), read back a few frames later once available (no
 * waiting); frames a disjoint event touched are dropped (d3d9_gl.c gpu_timer_*).
 * S2_GPU_TIMER=0 or debug.s2.gputimer=0: off. */
static void gpu_timer_end(void)
{
    if (G.gpu_state <= 0 || !G.gpu_open) return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    G.gpu_open = 0;
    ++G.gpu_head;
}

static void gpu_timer_begin(s2r_device *d)
{
    if (!G.gpu_state) {
        const char *env = getenv("S2_GPU_TIMER");
        char prop[92] = "";
        __system_property_get("debug.s2.gputimer", prop);
        if (prop[0]) env = prop;
        G.gpu_u64 = (s2r_query_u64_fn)eglGetProcAddress("glGetQueryObjectui64vEXT");
        G.gpu_state = (!env || *env != '0') && has_ext("GL_EXT_disjoint_timer_query") && G.gpu_u64 ? 1 : -1;
        const char *renderer = (const char *)glGetString(GL_RENDERER), *version = (const char *)glGetString(GL_VERSION);
        if (renderer && version && strstr(renderer, "Mali-G51") && strstr(version, "v1.r18p0")) G.gpu_state = -1;
        if (G.gpu_state > 0) glGenQueries(6, G.gpu_query);
        LOGGL("GPU frame timer %s", G.gpu_state > 0 ? "on" : "unavailable");
    }
    if (G.gpu_state <= 0) return;
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    while (G.gpu_tail != G.gpu_head) {
        GLuint id = G.gpu_query[G.gpu_tail % 6u], available = 0;
        glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available && !disjoint) break;
        if (available && !disjoint) {
            GLuint64 ns = 0;
            G.gpu_u64(id, GL_QUERY_RESULT, &ns);
            uint64_t age = s2r_now_ns() - G.gpu_query_issued[G.gpu_tail % 6u];
            if (ns > age + 1000000ull) {
                LOGGL("GPU timer returned an impossible duration (%llu ns after %llu ns); disabled",
                    (unsigned long long)ns, (unsigned long long)age);
                G.gpu_state = -1;
                return;
            }
            d->gpu_ns += ns;
            ++d->gpu_frames;
        }
        ++G.gpu_tail;
        if (!available) break;
    }
    if (G.gpu_head - G.gpu_tail >= 6u) return;
    G.gpu_query_issued[G.gpu_head % 6u] = s2r_now_ns();
    glBeginQuery(GL_TIME_ELAPSED_EXT, G.gpu_query[G.gpu_head % 6u]);
    G.gpu_open = 1;
}

/* The back buffer into the fitted rect of the window, flipped once (the image
 * is stored top-down, the window's rows go up). A draw rather than a blit: ARM
 * window blits can stay black with a correct source (d3d9_gl.c
 * shader_present_driver); nearest when the sizes match, linear otherwise. */
static int shader_present(GLuint image, uint32_t image_w, uint32_t image_h, int32_t x, int32_t y, int32_t w, int32_t h)
{
    static int mode = -1;
    if (mode < 0) {
        char v[92] = "";
        __system_property_get("debug.s2.glpresent", v);
        mode = !strcmp(v, "blit") ? 0 : 1;
        LOGGL("window present: %s", mode ? "shader pass" : "framebuffer blit (debug.s2.glpresent)");
    }
    if (!mode || G.present_failed) return 0;
    if (!G.present_program) {
        /* d3d9_filter.c's window pass, verified on Mali: an empty vertex array, a strip from
           gl_VertexID, the source coordinate from gl_FragCoord (no varying). */
        static const char *vs_src = "#version 300 es\n"
            "void main(){vec2 p=vec2(gl_VertexID&1,gl_VertexID>>1);gl_Position=vec4(p*2.0-1.0,0,1);}";
        static const char *fs_src = "#version 300 es\nprecision highp float;\n"
            "uniform highp sampler2D image;uniform highp vec2 origin;uniform highp vec2 target;layout(location=0)out vec4 color;\n"
            "void main(){vec2 uv=(gl_FragCoord.xy-origin)/target;color=vec4(texture(image,vec2(uv.x,1.0-uv.y)).rgb,1.0);}";
        GLuint prog = link_program(vs_src, fs_src);
        if (!prog) { G.present_failed = 1; LOGGL("present program failed: the window gets a blit"); return 0; }
        G.present_program = prog;
        G.present_image_loc = glGetUniformLocation(prog, "image");
        G.present_origin_loc = glGetUniformLocation(prog, "origin");
        G.present_target_loc = glGetUniformLocation(prog, "target");
        glGenVertexArrays(1, &G.present_vao);
        glGenSamplers(2, G.present_sampler);
        for (int i = 0; i < 2; ++i) {
            glSamplerParameteri(G.present_sampler[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(G.present_sampler[i], GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(G.present_sampler[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(G.present_sampler[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    int linear = (uint32_t)w != image_w || (uint32_t)h != image_h;
    bind_unit_texture(31, GL_TEXTURE_2D, image);
    glBindSampler(31, G.present_sampler[linear]);
    G.sampler_unit[31] = G.present_sampler[linear];
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(x, y, w, h);
    glUseProgram(G.present_program);
    G.cur_program = G.present_program;
    glUniform1i(G.present_image_loc, 31);
    glUniform2f(G.present_origin_loc, (float)x, (float)y);
    glUniform2f(G.present_target_loc, (float)w, (float)h);
    glBindVertexArray(G.present_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(G.vao);
    glBindSampler(31, 0);
    G.sampler_unit[31] = 0;
    G.raster_valid = 0;
    gl_check("present pass");
    return 1;
}

/* The launcher's filters into output-size images (s2r_filter.c, d3d9_filter.c's order): each pass
 * a full-screen strip reading the previous result; the last image goes to the window pass. 0 when a
 * program is missing (the scene is shown unfiltered). */
static GLuint filter_program(int which)
{
    if (G.filter_prog[which] || G.filter_failed[which]) return G.filter_prog[which];
    char *fs = s2r_filter_fragment(which, 0);
    GLuint prog = fs ? link_program(s2r_filter_vertex(0), fs) : 0;
    free(fs);
    if (!prog) { G.filter_failed[which] = 1; LOGGL("filter %s failed: not applied", s2r_filter_name(which)); return 0; }
    G.filter_prog[which] = prog;
    G.filter_image_loc[which] = glGetUniformLocation(prog, "image");
    G.filter_param_loc[which] = glGetUniformLocation(prog, "s2r_filter");
    LOGGL("filter %s ready", s2r_filter_name(which));
    return prog;
}

static GLuint run_filters(GLuint input, const int *passes, int count, uint32_t w, uint32_t h)
{
    if (G.filter_w != w || G.filter_h != h || !G.filter_tex[0]) {
        for (int i = 0; i < 2; ++i) {
            if (G.filter_tex[i]) { forget_texture(G.filter_tex[i], 0); glDeleteTextures(1, &G.filter_tex[i]); }
            glGenTextures(1, &G.filter_tex[i]);
            bind_unit_texture(31, GL_TEXTURE_2D, G.filter_tex[i]);
            set_active_unit(31);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        }
        G.filter_w = w; G.filter_h = h;
        LOGGL("present filters at %ux%u", w, h);
    }
    if (!G.filter_fbo) glGenFramebuffers(1, &G.filter_fbo);
    if (!G.filter_sampler) {
        glGenSamplers(1, &G.filter_sampler);
        glSamplerParameteri(G.filter_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(G.filter_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(G.filter_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(G.filter_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!G.present_vao) glGenVertexArrays(1, &G.present_vao);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(G.present_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, G.filter_fbo);
    G.cur_fbo = (GLuint)-1;
    GLuint source = input, result = 0;
    for (int i = 0; i < count; ++i) {
        GLuint prog = filter_program(passes[i]);
        if (!prog) { result = 0; break; }
        GLuint out = G.filter_tex[i & 1];
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        bind_unit_texture(31, GL_TEXTURE_2D, source);
        glBindSampler(31, G.filter_sampler);
        G.sampler_unit[31] = G.filter_sampler;
        glUseProgram(prog);
        G.cur_program = prog;
        glUniform1i(G.filter_image_loc[passes[i]], 31);
        glUniform4f(G.filter_param_loc[passes[i]], (float)w, (float)h, fsr1_rcas_sharpness(), 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        source = result = out;
    }
    glBindVertexArray(G.vao);
    glBindSampler(31, 0);
    G.sampler_unit[31] = 0;
    G.raster_valid = 0;
    gl_check("present filters");
    return result;
}

static void gl_present(s2r_device *d)
{
    own_thread();
    follow_window();
    s2r_image *shown = s2r_backbuffer_image(d);
    if (G.surface == EGL_NO_SURFACE || !shown || !shown->backend) {
        gpu_timer_end();
        frames_close();
        glFlush();
        s2r_pace();
        G.gpu_arm = 1;
        d->present_ms = 0;
        return;
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(G.display, G.surface, EGL_WIDTH, &sw);
    eglQuerySurface(G.display, G.surface, EGL_HEIGHT, &sh);
    gl_tex *bb = (gl_tex *)shown->backend;
    gl_fbo_key fk;
    memset(&fk, 0, sizeof fk);
    fk.color[0] = bb->name; fk.ctarget[0] = GL_TEXTURE_2D;
    GLuint src = fbo_for(&fk, NULL);
    /* depth is not needed after the frame: tilers skip writing it back */
    s2r_image *zimg = d->dev->auto_depth ? s2r_surface_image(d->dev->auto_depth) : NULL;
    gl_tex *zt = zimg ? (gl_tex *)zimg->backend : NULL;
    if (zt && zt->rb) {
        fk.depth_rb = zt->ms_rb && G.msaa_samples ? zt->ms_rb : zt->rb;
        fk.samples = zt->ms_rb && G.msaa_samples && shown->screen_sized ? G.msaa_samples : 0;
        if (fk.depth_rb == zt->ms_rb && !fk.samples) fk.depth_rb = zt->rb;
        GLuint with_depth = fbo_for(&fk, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, with_depth);
        const GLenum discard = GL_DEPTH_STENCIL_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
    }
    /* The frame's GPU time covers the game's passes: the query begins at the frame's first pass
       (bind_targets) and ends here, before the window's. Begun right after eglSwapBuffers and
       ended after the window pass (d3d9_gl.c's placement, fine in that layer), it made the
       Mali-G76 MC4 (r26p0) fail a job of this backend every frame: dmesg "Unhandled Page fault
       ... at VA 0x0 ... access type WRITE" on job slot 1, SurfaceFlinger fences with error -14,
       and a flat colour on the screen although glReadPixels of the window was right. */
    gpu_timer_end();
    /* the back buffer's aspect as the game knows it (the render scale stores it larger or smaller) */
    int32_t x, y, w, h;
    s2r_fit(shown->d3d_width ? shown->d3d_width : bb->width, shown->d3d_height ? shown->d3d_height : bb->height,
        (uint32_t)sw, (uint32_t)sh, &x, &y, &w, &h);
    GLuint image = bb->name;
    uint32_t image_w = bb->width, image_h = bb->height;
    int passes[3];
    int count = w > 0 && h > 0 ? s2r_filter_chain(bb->width, bb->height, (uint32_t)w, (uint32_t)h, passes) : 0;
    if (count) {
        GLuint filtered = run_filters(bb->name, passes, count, (uint32_t)w, (uint32_t)h);
        if (filtered) {
            image = filtered; image_w = (uint32_t)w; image_h = (uint32_t)h;
            fk.color[0] = filtered; fk.depth_rb = 0; fk.samples = 0;
            src = fbo_for(&fk, NULL);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* the whole window cleared, as the Direct3D 9 layer does: no load of the old buffer, black bars */
    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!shader_present(image, image_w, image_h, x, y, w, h))
        glBlitFramebuffer(0, 0, (GLint)image_w, (GLint)image_h, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT,
            (uint32_t)w == image_w && (uint32_t)h == image_h ? GL_NEAREST : GL_LINEAR);
    if (G.swap_applied != 1) { eglSwapInterval(G.display, 1); G.swap_applied = 1; }
    frames_close();
    s2r_pace();
    uint64_t start = s2r_now_ns();
    d3d_rt_wait_begin();
    if (!eglSwapBuffers(G.display, G.surface)) {
        LOGGL("eglSwapBuffers failed %04X: the surface is made again", eglGetError());
        make_surface();
    }
    d3d_rt_wait_end();
    d->present_ms = (double)(s2r_now_ns() - start) / 1e6;
    ring_report(d->frame);
    G.gpu_arm = 1;
    G.cur_fbo = (GLuint)-1;
    G.raster_valid = 0;
    gl_check("present");
}

/* ------------------------------------------------------------- queries */
static void *gl_query_create(s2r_device *d, uint32_t type)
{
    (void)d;
    own_thread();
    gl_query *q = (gl_query *)calloc(1, sizeof *q);
    q->type = type;
    if (type == 9) glGenQueries(1, &q->name);
    return q;
}

static void gl_query_issue(s2r_device *d, s2r_query *hq, uint32_t flags)
{
    (void)d;
    gl_query *q = (gl_query *)hq->backend;
    if (!q) return;
    own_thread();
    if (q->type == 8) {
        if (flags & 1u) {
            if (q->fence) glDeleteSync(q->fence);
            q->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            q->ended = 1;
        }
        return;
    }
    if (flags & 2u) {
        if (q->active) glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
        glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q->name);
        q->active = 1;
        q->ended = 0;
    }
    if ((flags & 1u) && q->active) {
        glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
        q->active = 0;
        q->ended = 1;
    }
}

static int gl_query_result(s2r_device *d, s2r_query *hq, uint32_t *value, int flush)
{
    gl_query *q = (gl_query *)hq->backend;
    if (!q || !q->ended) { *value = q && q->type == 8 ? 1u : d->dev->st.viewport[2] * d->dev->st.viewport[3]; return 1; }
    own_thread();
    if (q->type == 8) {
        if (q->fence) {
            GLenum r = glClientWaitSync(q->fence, flush ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
            if (r == GL_TIMEOUT_EXPIRED) return 0;
        }
        *value = 1;
        return 1;
    }
    GLuint ready = 0, any = 0;
    glGetQueryObjectuiv(q->name, GL_QUERY_RESULT_AVAILABLE, &ready);
    if (!ready) { if (flush) glFlush(); return 0; }
    glGetQueryObjectuiv(q->name, GL_QUERY_RESULT, &any);
    /* GLES reports only "any samples": the viewport's pixel count stands for visible */
    *value = any ? d->dev->st.viewport[2] * d->dev->st.viewport[3] : 0;
    return 1;
}

static void gl_query_destroy(s2r_device *d, s2r_query *hq)
{
    (void)d;
    gl_query *q = (gl_query *)hq->backend;
    if (!q) return;
    own_thread();
    if (q->active) glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
    if (q->name) glDeleteQueries(1, &q->name);
    if (q->fence) glDeleteSync(q->fence);
    free(q);
    hq->backend = NULL;
}

static void gl_backbuffer(s2r_device *d)
{
    (void)d;
    G.cur_fbo = (GLuint)-1;
    G.raster_valid = 0;
}

static void gl_finish(s2r_device *d)
{
    (void)d;
    d3d_rt_wait_begin();
    glFinish();
    d3d_rt_wait_end();
    G.completed = G.frame - 1;
}

const s2r_backend s2r_gles_backend = {
    "gles", gl_init, gl_shutdown, gl_image_create, gl_image_destroy, gl_image_upload, gl_image_mips, gl_image_read,
    gl_buffer_create, gl_buffer_write, gl_buffer_destroy, gl_clear, gl_draw, gl_blit, gl_fill, gl_present,
    gl_query_create, gl_query_issue, gl_query_result, gl_query_destroy, gl_backbuffer, gl_finish,
};
