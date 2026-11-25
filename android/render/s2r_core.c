/* The native renderers under the port's Direct3D 9 front end (s2r.h).
 *
 * The game's thread keeps everything Direct3D 9 defines (d3d9_device.c,
 * d3d9_resource.c): objects, locks, guest copies, the state model and its
 * recording for the render thread. Here, on the render thread, the front
 * end's objects get their backend objects and its recorded operations go to
 * the backend: a texture's s2r_image, a buffer's s2r_buffer (with the render
 * thread's own mirror of the contents, since the guest copy belongs to the
 * game's thread), a query's backend query, a shader's library entry.
 *
 * The backend is the launcher's choice (GameActivity.nativeRenderer, before
 * the game starts): OpenGL ES or Vulkan. It starts with the first device
 * (s2r_init from run_gl_init). When it cannot start, or later loses its GPU
 * device, the game stops and the launcher offers its next renderer (the
 * launcher's renderer failure policy: s2r_renderer_failed below); no other
 * renderer or driver takes over. The old Direct3D 9 -> GLES layer (d3d9_gl.c)
 * runs only when debug.s2.renderer=legacy asks for it. */
#include "s2r_internal.h"
#include "../host/d3d9_texcache.h"
#include "s2r.h"
#include "../host/hud.h"
#include "../host/sims2_settings.h"
#include "../launcher/native/replacements/renderer_failure.h"
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

void void_android_frame_presented(void) __attribute__((weak));
int void_android_game_started(void) __attribute__((weak));
void void_android_renderer_failed(const char *reason, int device_lost, int recorded) __attribute__((weak));
void void_readcache_flush_all(void);
void void_registry_flush(void);

/* ------------------------------------------------------- renderer failure */
static char fail_reason[PORT_RENDERER_FAILURE_REASON_MAX];
static int qa_failure;                    /* debug.s2.renderer_fail: 0 off, 1 start, 2 device lost at the first frame */
static char qa_value[92];

void s2r_fail_note(const char *format, ...)
{
    if (fail_reason[0]) return;          /* the first cause: later notes follow from it */
    va_list args;
    va_start(args, format);
    vsnprintf(fail_reason, sizeof fail_reason, format, args);
    va_end(args);
}

void s2r_renderer_failed(const char *reason, int device_lost)
{
    static int reporting;
    if (__atomic_exchange_n(&reporting, 1, __ATOMIC_ACQ_REL)) for (;;) pause();   /* another thread reports and ends the process */
    S2R_LOG("renderer failure (%s): %s", device_lost ? "GPU device lost" : "did not start", reason);
    void_readcache_flush_all();
    void_registry_flush();
    void_log_flush();
    /* The launcher's record first, with the time since the first frame this side presented (the Java side learns
       of that frame up to a poll later, and a record without it would make a loss in the window look like an
       ordinary crash next to the crash journal's first-frame mark); then GameActivity opens the launcher and ends
       the process (Launch.rendererFailed writes the record itself only when this one could not be written). */
    int recorded = port_renderer_failed(reason, device_lost) == 0;
    if (void_android_renderer_failed) void_android_renderer_failed(reason, device_lost, recorded);
    _exit(1);
}

void s2r_renderer_lost(const char *what) { s2r_renderer_failed(what, 1); }

static int presented_frames;
void s2r_renderer_presented(void)
{
    if (__atomic_fetch_add(&presented_frames, 1, __ATOMIC_RELAXED)) return;
    port_renderer_presented();
    if (qa_failure == 2) {
        char reason[160];
        snprintf(reason, sizeof reason, "QA switch debug.s2.renderer_fail=%s: GPU device loss simulated at the first frame", qa_value);
        s2r_renderer_lost(reason);
    }
}

int s2r_renderer_has_presented(void) { return __atomic_load_n(&presented_frames, __ATOMIC_RELAXED) != 0; }

/* debug.s2.renderer_fail=<renderer>[:lost] (QA; unset by default): the launcher's renderer of that id (gles, vulkan,
   vulkan-turnip, vulkan-panvk, the old turnip/panvk; "all" any) reports a start failure, with ":lost" a GPU device
   loss at its first presented frame, through the same path as a real failure (the launcher's dialog follows). The
   id is the launcher's choice (PORT_RENDERER), not a debug.s2.renderer override. */
static void qa_failure_read(void)
{
    char v[92] = "";
    __system_property_get("debug.s2.renderer_fail", v);
    if (!v[0]) return;
    snprintf(qa_value, sizeof qa_value, "%s", v);
    char *colon = strchr(v, ':');
    int lost = colon && !strcmp(colon + 1, "lost");
    if (colon) *colon = 0;
    const char *mode = getenv("PORT_RENDERER");
    if (!mode) mode = "";
    if (!strcmp(mode, "turnip")) mode = "vulkan-turnip";
    if (!strcmp(mode, "panvk")) mode = "vulkan-panvk";
    if (!strcmp(v, "turnip")) snprintf(v, sizeof v, "vulkan-turnip");
    if (!strcmp(v, "panvk")) snprintf(v, sizeof v, "vulkan-panvk");
    if (strcmp(v, "all") && strcmp(v, mode)) {
        S2R_LOG("QA switch debug.s2.renderer_fail=%s: not this renderer (%s)", qa_value, mode[0] ? mode : "none");
        return;
    }
    qa_failure = lost ? 2 : 1;
    S2R_LOG("QA switch debug.s2.renderer_fail=%s: %s will report a %s", qa_value, mode[0] ? mode : "the renderer",
        lost ? "GPU device loss at its first frame" : "start failure");
}

static int selected = S2R_BACKEND_NONE;   /* the launcher's choice */
static int active;                        /* the backend runs (render thread decided) */
static s2r_device device;
s2r_device *s2r_dev;
static char cache_dir[600];

int s2r_on(void) { return active; }
int s2r_selected_backend(void) { return selected; }
void s2r_set_mode(int backend) { selected = backend; }
const char *s2r_backend_name(int backend)
{
    return backend == S2R_BACKEND_VULKAN ? "Vulkan" : backend == S2R_BACKEND_GLES ? "OpenGL ES" : "the GLES layer";
}
const char *s2r_cache_dir(void) { return cache_dir; }

uint64_t s2r_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* What the [mem] report counts (render thread, read racily as statistics). */
static uint64_t stat_textures, stat_texture_bytes, stat_surfaces, stat_surface_bytes, stat_buffers, stat_buffer_bytes;

void s2r_gpu_stats(uint64_t *textures, uint64_t *texture_bytes, uint64_t *surfaces, uint64_t *surface_bytes,
    uint64_t *buffers, uint64_t *buffer_bytes, uint64_t *programs)
{
    uint32_t hits = 0, runtime = 0;
    uint64_t ns = 0;
    s2r_lib_stats(&hits, &runtime, &ns);
    *textures = stat_textures; *texture_bytes = stat_texture_bytes;
    *surfaces = stat_surfaces; *surface_bytes = stat_surface_bytes;
    *buffers = stat_buffers; *buffer_bytes = stat_buffer_bytes;
    *programs = runtime;
}

/* debug.s2.renderer=legacy|gles|vulkan|vulkan-turnip|vulkan-panvk (or the old turnip|panvk) overrides the launcher (development A/B runs;
   turnip and panvk: the native Vulkan renderer on that app-local driver). */
static int property_override(int launcher)
{
    char v[92] = "";
    __system_property_get("debug.s2.renderer", v);
    if (!strcmp(v, "legacy")) return S2R_BACKEND_NONE;
    if (!strcmp(v, "gles")) return S2R_BACKEND_GLES;
    int turnip = !strcmp(v, "vulkan-turnip") || !strcmp(v, "turnip");
    int panvk = !strcmp(v, "vulkan-panvk") || !strcmp(v, "panvk");
    if (!strcmp(v, "vulkan") || turnip || panvk) {
        s2r_set_vk_kind(turnip ? S2R_VK_TURNIP : panvk ? S2R_VK_PANVK : S2R_VK_SYSTEM);
        return S2R_BACKEND_VULKAN;
    }
    return launcher;
}

int s2r_init(d3d_device *mirror)
{
    if (active) { device.dev = mirror; return 1; }
    static int tried;
    if (tried) return 0;
    tried = 1;
    selected = property_override(selected);
    if (selected == S2R_BACKEND_NONE) {
        S2R_LOG("the Direct3D 9 -> GLES layer renders this session");
        return 0;
    }
    memset(&device, 0, sizeof device);
    device.dev = mirror;
    device.backend_kind = selected;
    device.be = selected == S2R_BACKEND_VULKAN ? &s2r_vulkan_backend : &s2r_gles_backend;
    device.gamma_identity = 1;
    for (int i = 0; i < 256; ++i) {
        device.gamma_lut[4 * i] = device.gamma_lut[4 * i + 1] = device.gamma_lut[4 * i + 2] = (uint8_t)i;
        device.gamma_lut[4 * i + 3] = 255;
    }
    s2r_dev = &device;
    qa_failure_read();
    if (qa_failure == 1) {
        char reason[160];
        snprintf(reason, sizeof reason, "QA switch debug.s2.renderer_fail=%s: start failure simulated", qa_value);
        s2r_renderer_failed(reason, 0);
    }
    s2r_lib_load();
    uint64_t start = s2r_now_ns();
    if (!device.be->init(&device)) {
        /* No other renderer takes over: the launcher offers its next one (renderer_failure.h). */
        char reason[PORT_RENDERER_FAILURE_REASON_MAX];
        snprintf(reason, sizeof reason, "%s", fail_reason[0] ? fail_reason : selected == S2R_BACKEND_VULKAN
            ? "Vulkan: the renderer could not start (sims2.log says why)" : "OpenGL ES: the renderer could not start (sims2.log says why)");
        S2R_LOG("the native %s renderer could not start: %s", s2r_backend_name(selected), reason);
        s2r_renderer_failed(reason, 0);
    }
    /* Constant limits the front end reports in the device caps: the uniform blocks hold the whole register files. */
    d3d_gl_limits.max_vs_vectors = 256;
    d3d_gl_limits.max_ps_vectors = 224;
    active = 1;
    S2R_LOG("native %s renderer started in %.1f ms", s2r_backend_name(selected), (double)(s2r_now_ns() - start) / 1e6);
    return 1;
}

/* --------------------------------------------------------------- images */
static int kind_of(uint32_t rtype) { return rtype == D3DRTYPE_CUBETEXTURE ? S2R_TEX_CUBE : rtype == D3DRTYPE_VOLUMETEXTURE ? S2R_TEX_3D : S2R_TEX_2D; }

s2r_image *s2r_texture_image(d3d_texture *t)
{
    s2r_image *img = t ? (s2r_image *)t->s2r : NULL;
    return img && img->backend ? img : NULL;
}

s2r_image *s2r_surface_image(d3d_surface *s)
{
    if (!s) return NULL;
    if (s->container) return s2r_texture_image(s->container);
    s2r_image *img = (s2r_image *)s->s2r;
    return img && img->backend ? img : NULL;
}

int s2r_surface_target(d3d_surface *s, s2r_target *out)
{
    memset(out, 0, sizeof *out);
    if (!s) return 0;
    if (s->container) {
        out->img = s2r_texture_image(s->container);
        out->face = s->face;
        out->level = s->level;
    } else out->img = s2r_surface_image(s);
    return out->img != NULL;
}

s2r_image *s2r_backbuffer_image(s2r_device *d)
{
    return d->dev ? s2r_surface_image(d->dev->backbuffer) : NULL;
}

int s2r_ui_fsr(void) { return s2_ui_fsr(); }

/* The launcher's MSAA for the scene (the screen-sized targets): 0, 2 or 4 samples. */
int s2r_msaa_samples(void)
{
    int samples = s2_render_msaa();
    return samples == 2 || samples == 4 ? samples : 0;
}

/* A screen-sized target (the back buffer, a target or depth surface of its size) is stored at
 * the render scale (d3d_screen_scale, d3d9_gl.c): width/height of the image are the stored size. */
static void scale_image(s2r_image *img, float scale, int screen_sized, uint32_t w, uint32_t h)
{
    img->scale = screen_sized && scale > 0.0f ? scale : 1.0f;
    img->screen_sized = screen_sized;
    img->d3d_width = w;
    img->d3d_height = h;
}

static uint32_t stored(uint32_t size, float scale) { return scale > 0.0f ? d3d_scaled_size(size, scale) : size; }

/* After a present: a changed render scale (the game's option, Reset) gives every screen-sized
 * target its new storage, as the Direct3D 9 layer's d3d_scaled_follow does. */
void s2r_scale_follow(s2r_device *d)
{
    if (!d->dev) return;
    float was = d3d_scaled_applied();
    int n = d3d_scaled_retarget(d3d_screen_scale(d->dev->pp[0]));
    if (n) S2R_LOG("render scale %.3f -> %.3f: %d screen-sized targets stored again", (double)was,
        (double)d3d_scaled_applied(), n);
}

void s2r_texture_storage(d3d_texture *t)
{
    s2r_texture_release(t);
    s2r_image *img = (s2r_image *)calloc(1, sizeof *img);
    if (!img) return;
    float scale = t->screen_sized ? t->scale : 1.0f;
    uint32_t usage = t->usage;
    /* a DXT texture the launcher's converted textures hold: the cache's blocks (s2r_texcache.c) */
    uint32_t converted = t->cache_state == D3D_TEXCACHE_COMPRESSED ? t->gl_internal : 0;
    int described = s2r_image_describe(img, kind_of(t->rtype), stored(t->width, scale), stored(t->height, scale), t->depth,
        t->levels, t->format, usage, converted);
    scale_image(img, scale, t->screen_sized, t->width, t->height);
    if (!described || !s2r_dev->be->image_create(s2r_dev, img)) {
        static int logged;
        if (logged++ < 16) S2R_LOG("texture %ux%u levels %u format %u usage %X has no storage", t->width, t->height,
            t->levels, t->format, t->usage);
        free(img);
        return;
    }
    if (img->depth_stencil) {
        static int depth_logged;
        if (!depth_logged++) S2R_LOG("depth texture %ux%u format %u (sampled as a dummy)", t->width, t->height, t->format);
    }
    t->s2r = img;
    ++stat_textures;
    stat_texture_bytes += img->bytes;
}

void s2r_texture_release(d3d_texture *t)
{
    s2r_image *img = (s2r_image *)t->s2r;
    if (!img) return;
    if (img->backend) s2r_dev->be->image_destroy(s2r_dev, img);
    --stat_textures;
    stat_texture_bytes -= img->bytes;
    free(img);
    t->s2r = NULL;
}

/* Guest-layout rows of a level rect into the backend's layout and onto the GPU. */
/* R8G8B8 (the format table's GL_RGB rows, three bytes a texel) is stored as RGBA8
 * (s2r_format.c): its converted rows widen to four bytes here. Uploaded as they
 * were, three-byte rows read as four-byte texels sheared lot walls into dark
 * diagonal stripes (Veronaville, the Montecchi house). */
static const void *widen_rgb(const d3d_format *f, const void *gpu, uint32_t w, uint32_t rows, void **owned, size_t *size)
{
    if (!gpu || f->format != GL_RGB || f->type != GL_UNSIGNED_BYTE) return gpu;
    size_t n = (size_t)w * rows;
    uint8_t *out = (uint8_t *)malloc(n * 4);
    if (!out) return NULL;
    const uint8_t *in = (const uint8_t *)gpu;
    for (size_t i = 0; i < n; ++i) {
        out[4 * i] = in[3 * i]; out[4 * i + 1] = in[3 * i + 1]; out[4 * i + 2] = in[3 * i + 2]; out[4 * i + 3] = 255;
    }
    free(*owned);
    *owned = out;
    *size = n * 4;
    return out;
}

/* Rows of a w x h rect resampled (nearest) to the stored rect of an image the render scale stores
 * larger or smaller: x0,y0 and the size in stored pixels, malloc'ed rows of the stored width. */
static void *scale_rows(const s2r_image *img, const void *rows, uint32_t row_pitch, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h, uint32_t *sx, uint32_t *sy, uint32_t *sw, uint32_t *sh)
{
    float k = s2r_scale_of(img);
    uint32_t bpp = s2r_gpu_texel_bytes(img->gpu);
    int32_t x0 = s2r_scaled((int32_t)x, k), y0 = s2r_scaled((int32_t)y, k);
    int32_t x1 = s2r_scaled((int32_t)(x + w), k), y1 = s2r_scaled((int32_t)(y + h), k);
    if (x1 > (int32_t)img->width) x1 = (int32_t)img->width;
    if (y1 > (int32_t)img->height) y1 = (int32_t)img->height;
    if (!bpp || x1 <= x0 || y1 <= y0) return NULL;
    *sx = (uint32_t)x0; *sy = (uint32_t)y0; *sw = (uint32_t)(x1 - x0); *sh = (uint32_t)(y1 - y0);
    uint8_t *out = (uint8_t *)malloc((size_t)*sw * *sh * bpp);
    if (!out) return NULL;
    for (uint32_t r = 0; r < *sh; ++r) {
        uint32_t from_y = (uint32_t)(((float)r + 0.5f) / k);
        if (from_y >= h) from_y = h - 1;
        const uint8_t *src = (const uint8_t *)rows + (size_t)from_y * row_pitch;
        uint8_t *dst = out + (size_t)r * *sw * bpp;
        for (uint32_t c = 0; c < *sw; ++c) {
            uint32_t from_x = (uint32_t)(((float)c + 0.5f) / k);
            if (from_x >= w) from_x = w - 1;
            memcpy(dst + (size_t)c * bpp, src + (size_t)from_x * bpp, bpp);
        }
    }
    return out;
}

/* Converted rows into an image: as they are, or resampled into a scaled target's stored pixels. */
static void put_rows(s2r_image *img, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint32_t depth, const void *gpu, uint32_t row)
{
    if (s2r_scale_of(img) != 1.0f && level == 0 && depth == 1 && !img->compressed_dxt) {
        static int logged;
        if (!logged++) S2R_LOG("pixels written into a %ux%u target the render scale stores at %ux%u: resampled",
            img->d3d_width, img->d3d_height, img->width, img->height);
        uint32_t sx, sy, sw, sh;
        void *scaled = scale_rows(img, gpu, row, x, y, w, h, &sx, &sy, &sw, &sh);
        if (!scaled) return;
        uint32_t srow = s2r_gpu_pitch(img, sw);
        s2r_dev->be->image_upload(s2r_dev, img, face, 0, sx, sy, sw, sh, 1, scaled, srow, srow * sh);
        free(scaled);
        return;
    }
    s2r_dev->be->image_upload(s2r_dev, img, face, level, x, y, w, h, depth, gpu, row, row * h);
}

void s2r_texture_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    s2r_image *img = s2r_texture_image(t);
    if (!img || !w || !h || level >= img->levels) return;
    /* DXT levels the launcher converted to ETC2/ASTC go up as they are (the image may be created again) */
    if (!img->compressed_dxt && t->fmt->block_bytes) {
        if (s2r_texcache_upload(t, face, level, x, y, w, h, data, pitch)) return;
        if (!(img = s2r_texture_image(t))) return;
    }
    uint32_t depth = img->kind == S2R_TEX_3D ? (t->depth >> level ? t->depth >> level : 1) : 1;
    uint64_t bytes = 0;
    if (img->compressed_dxt) {
        /* DXT blocks as they are: the device samples BC */
        static int logged;
        if (!logged++) S2R_LOG("DXT textures stored as BC (the GPU samples them): no decode, the launcher's converted textures "
            "are not needed");
        s2r_dev->be->image_upload(s2r_dev, img, face, level, x, y, w, h, depth, data, pitch, pitch * d3d_format_rows(t->fmt, h));
        bytes = (uint64_t)pitch * d3d_format_rows(t->fmt, h) * depth;
    } else {
        void *owned = NULL;
        size_t size = 0;
        const void *gpu = d3d_format_convert(t->fmt, data, w, h * depth, pitch, &owned, &size,
            (t->usage & D3DUSAGE_RENDERTARGET) != 0);
        gpu = widen_rgb(t->fmt, gpu, w, h * depth, &owned, &size);
        if (!gpu) { free(owned); return; }
        uint32_t row = s2r_gpu_pitch(img, w);
        put_rows(img, face, level, x, y, w, h, depth, gpu, row);
        bytes = size;
        free(owned);
    }
    s2r_dev->upload_bytes += bytes;
    s2r_dev->upload_texture += bytes;
    if (level == 0 && (t->usage & D3DUSAGE_AUTOGENMIPMAP)) t->mips_dirty = 1;
}

void s2r_texture_generate_mips(d3d_texture *t)
{
    s2r_image *img = s2r_texture_image(t);
    t->mips_dirty = 0;
    if (img && img->levels > 1) s2r_dev->be->image_mips(s2r_dev, img);
}

void s2r_surface_storage(d3d_surface *s)
{
    s2r_surface_release(s);
    if (!s->fmt || !s->fmt->internal) return;
    s2r_image *img = (s2r_image *)calloc(1, sizeof *img);
    if (!img) return;
    float scale = s->screen_sized ? s->scale : 1.0f;
    /* Standalone GPU surfaces are stored like render targets (d3d_surface_new). */
    uint32_t usage = s->usage | (s->fmt->depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET);
    if (s->multisample > 1) {
        static int logged;
        if (!logged++) S2R_LOG("multisampled surfaces (%u samples) are drawn without MSAA", s->multisample);
    }
    int described = s2r_image_describe(img, S2R_TEX_2D, stored(s->width, scale), stored(s->height, scale), 1, 1, s->format,
        usage, 0);
    scale_image(img, scale, s->screen_sized, s->width, s->height);
    if (!described || !s2r_dev->be->image_create(s2r_dev, img)) {
        S2R_LOG("surface %ux%u format %u usage %X has no storage", s->width, s->height, s->format, s->usage);
        free(img);
        return;
    }
    if (s2r_dev->dev && s == s2r_dev->dev->backbuffer) img->is_backbuffer = 1;
    s->s2r = img;
    ++stat_surfaces;
    stat_surface_bytes += img->bytes;
    if (img->is_backbuffer) s2r_dev->be->backbuffer(s2r_dev);
}

void s2r_surface_release(d3d_surface *s)
{
    s2r_image *img = (s2r_image *)s->s2r;
    if (!img) return;
    if (img->backend) s2r_dev->be->image_destroy(s2r_dev, img);
    --stat_surfaces;
    stat_surface_bytes -= img->bytes;
    free(img);
    s->s2r = NULL;
}

void s2r_surface_upload(d3d_surface *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const uint8_t *data,
    uint32_t pitch)
{
    s2r_image *img = s2r_surface_image(s);
    if (!img || x1 <= x0 || y1 <= y0) return;
    void *owned = NULL;
    size_t size = 0;
    const void *gpu = d3d_format_convert(s->fmt, data, x1 - x0, y1 - y0, pitch, &owned, &size, 1);
    gpu = widen_rgb(s->fmt, gpu, x1 - x0, y1 - y0, &owned, &size);
    if (!gpu) { free(owned); return; }
    uint32_t row = s2r_gpu_pitch(img, x1 - x0);
    put_rows(img, 0, 0, x0, y0, x1 - x0, y1 - y0, 1, gpu, row);
    s2r_dev->upload_bytes += size;
    s2r_dev->upload_texture += size;
    free(owned);
}

/* -------------------------------------------------------------- buffers */
s2r_buffer *s2r_buffer_of(d3d_buffer *b)
{
    s2r_buffer *sb = b ? (s2r_buffer *)b->s2r : NULL;
    return sb && sb->backend ? sb : NULL;
}

void s2r_buffer_create(d3d_buffer *b)
{
    s2r_buffer *sb = (s2r_buffer *)calloc(1, sizeof *sb);
    if (!sb) return;
    sb->index = b->index;
    sb->size = b->size;
    sb->usage = b->usage;
    sb->format = b->format;
    /* No copy of the contents here: OpenGL ES renames a buffer a draw in flight reads, Vulkan
       makes its copy when a buffer is first written in place while in flight (vk_buffer_write).
       A mirror of every buffer cost 75 MB of heap in Veronaville (Pixel 10a, 2026-11-21). */
    sb->backend = s2r_dev->be->buffer_create(s2r_dev, sb);
    if (!sb->backend) {
        S2R_LOG("%s buffer of %u bytes has no storage", b->index ? "index" : "vertex", b->size);
        free(sb);
        return;
    }
    b->s2r = sb;
    ++stat_buffers;
    stat_buffer_bytes += b->size;
}

void s2r_buffer_upload(d3d_buffer *b, uint32_t lo, uint32_t length, int whole, int unsynchronized, const uint8_t *src)
{
    s2r_buffer *sb = s2r_buffer_of(b);
    if (!sb || lo >= sb->size) return;
    if (length > sb->size - lo) length = sb->size - lo;
    if (sb->mirror) memcpy(sb->mirror + lo, src, length);
    int mode = whole ? S2R_WRITE_DISCARD : unsynchronized ? S2R_WRITE_NOOVERWRITE : S2R_WRITE_PLAIN;
    s2r_dev->be->buffer_write(s2r_dev, sb, lo, length, src, mode);
    s2r_dev->upload_bytes += length;
}

void s2r_buffer_release(d3d_buffer *b)
{
    s2r_buffer *sb = (s2r_buffer *)b->s2r;
    if (!sb) return;
    s2r_dev->be->buffer_destroy(s2r_dev, sb);
    free(sb->mirror);
    free(sb);
    b->s2r = NULL;
    --stat_buffers;
    stat_buffer_bytes -= b->size;
}

/* -------------------------------------------------------------- shaders */
s2r_shader_info *s2r_shader_of(d3d_shader *s)
{
    if (!s) return NULL;
    if (!s->s2r) s->s2r = calloc(1, sizeof(s2r_shader_info));
    return (s2r_shader_info *)s->s2r;
}

void s2r_shader_release(d3d_shader *s)
{
    /* Library and device entries outlive the shader: the backends' programs name them. */
    free(s->s2r);
    s->s2r = NULL;
}

/* -------------------------------------------------------------- queries */
void s2r_query_create_rt(d3d_query *q)
{
    s2r_query *sq = (s2r_query *)calloc(1, sizeof *sq);
    if (!sq) return;
    sq->type = q->type;
    sq->backend = s2r_dev->be->query_create(s2r_dev, q->type);
    q->s2r = sq;
}

void s2r_query_issue_rt(d3d_query *q, uint32_t flags)
{
    s2r_query *sq = (s2r_query *)q->s2r;
    if (!sq) return;
    if (flags & 2u) { sq->issued = 1; sq->ended = 0; }
    if (flags & 1u) { sq->ended = 1; sq->issued = 0; }
    s2r_dev->be->query_issue(s2r_dev, sq, flags);
}

int s2r_query_answer(d3d_query *q, int flush, int *visible)
{
    s2r_query *sq = (s2r_query *)q->s2r;
    if (!sq) return -1;
    uint32_t value = 0;
    uint64_t start = s2r_now_ns();
    int ready = s2r_dev->be->query_result(s2r_dev, sq, &value, flush);
    s2r_dev->query_wait_ns += s2r_now_ns() - start;
    if (!ready) return 0;
    *visible = value != 0;
    return 1;
}

void s2r_query_release(d3d_query *q)
{
    s2r_query *sq = (s2r_query *)q->s2r;
    if (!sq) return;
    s2r_dev->be->query_destroy(s2r_dev, sq);
    free(sq);
    q->s2r = NULL;
}

void s2r_flush(void)
{
    /* GL hands the GPU what is recorded; Vulkan submits at the present (and on a FLUSH poll). */
    if (s2r_dev && s2r_dev->backend_kind == S2R_BACKEND_GLES) glFlush();
}

/* ------------------------------------------------------------ operations */
void s2r_draw_call(d3d_device *d, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count, uint32_t prim_count,
    int indexed, uint32_t base_vertex, uint32_t start_index, uint32_t min_index, const void *up_vertices,
    uint32_t up_stride, const void *up_indices, int up_index32)
{
    s2r_dev->dev = d;
    s2r_dev->frame = d->frame;
    if (d->rt[1] || d->rt[2] || d->rt[3]) {
        static int logged;
        if (!logged++) S2R_LOG("multiple render targets: a draw binds render targets 1-3 (%s %s %s)", d->rt[1] ? "1" : "-",
            d->rt[2] ? "2" : "-", d->rt[3] ? "3" : "-");
    }
    s2r_draw_primitive(s2r_dev, primitive, indexed ? start_index : start_vertex, vertex_count, prim_count, indexed,
        (int32_t)base_vertex, min_index, up_vertices, up_stride, up_indices, up_index32);
}

void s2r_clear(d3d_device *d, uint32_t count, const uint32_t *rects, uint32_t flags, uint32_t color, float z,
    uint32_t stencil)
{
    s2r_dev->dev = d;
    int32_t local[64 * 4];
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count * 4; ++i) local[i] = (int32_t)rects[i];
    s2r_dev->be->clear(s2r_dev, count, local, flags, color, z, stencil);
}

void s2r_blit(d3d_surface *src, const uint32_t *sr, d3d_surface *dst, const uint32_t *dr, uint32_t filter)
{
    s2r_target a, b;
    if (!s2r_surface_target(src, &a) || !s2r_surface_target(dst, &b)) return;
    if (!s2r_dev->be->blit(s2r_dev, &a, sr, &b, dr, filter == 2)) {
        static int logged;
        if (logged++ < 8) S2R_LOG("StretchRect %ux%u format %u -> %ux%u format %u not supported", src->width, src->height,
            src->format, dst->width, dst->height, dst->format);
    }
}

void s2r_fill(d3d_surface *dst, const uint32_t *rect, uint32_t color)
{
    s2r_target t;
    if (!s2r_surface_target(dst, &t)) return;
    s2r_dev->be->fill(s2r_dev, &t, rect, color);
}

/* A rect of a surface in the backend's layout, then as logical RGBA. */
static int read_gpu(d3d_surface *s, const uint32_t *rect, uint8_t **out, s2r_image **image)
{
    s2r_target t;
    if (!s->fmt || s->fmt->depth || !s2r_surface_target(s, &t) || t.img->compressed_dxt || t.img->native_internal) return 0;
    uint32_t w = rect[2] - rect[0], h = rect[3] - rect[1];
    uint32_t pitch = s2r_gpu_pitch(t.img, w);
    uint8_t *buf = (uint8_t *)malloc((size_t)pitch * h);
    if (!buf) return 0;
    float k = s2r_scale_of(t.img);
    if (k != 1.0f && !t.level) {
        /* the stored pixels of the rect, then one per Direct3D pixel (the Direct3D 9 layer's read_scaled) */
        uint32_t sr[4] = { (uint32_t)s2r_scaled((int32_t)rect[0], k), (uint32_t)s2r_scaled((int32_t)rect[1], k),
                           (uint32_t)s2r_scaled((int32_t)rect[2], k), (uint32_t)s2r_scaled((int32_t)rect[3], k) };
        if (sr[2] > t.img->width) sr[2] = t.img->width;
        if (sr[3] > t.img->height) sr[3] = t.img->height;
        uint32_t sw = sr[2] > sr[0] ? sr[2] - sr[0] : 0, sh = sr[3] > sr[1] ? sr[3] - sr[1] : 0;
        uint32_t spitch = s2r_gpu_pitch(t.img, sw), bpp = s2r_gpu_texel_bytes(t.img->gpu);
        uint8_t *big = sw && sh && bpp ? (uint8_t *)malloc((size_t)spitch * sh) : NULL;
        if (!big || !s2r_dev->be->image_read(s2r_dev, t.img, t.face, 0, sr, big, spitch)) { free(big); free(buf); return 0; }
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t from_y = (uint32_t)(((float)y + 0.5f) * k);
            if (from_y >= sh) from_y = sh - 1;
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t from_x = (uint32_t)(((float)x + 0.5f) * k);
                if (from_x >= sw) from_x = sw - 1;
                memcpy(buf + (size_t)y * pitch + (size_t)x * bpp, big + (size_t)from_y * spitch + (size_t)from_x * bpp, bpp);
            }
        }
        free(big);
    } else if (!s2r_dev->be->image_read(s2r_dev, t.img, t.face, t.level, rect, buf, pitch)) { free(buf); return 0; }
    *out = buf;
    *image = t.img;
    return 1;
}

int s2r_read_rgba(d3d_surface *s, const uint32_t *rect, float *out)
{
    uint8_t *buf;
    s2r_image *img;
    if (!read_gpu(s, rect, &buf, &img)) return 0;
    uint32_t count = (rect[2] - rect[0]) * (rect[3] - rect[1]);
    /* the GPU layout is the D3D format's GL layout (d3d_format_convert's output) */
    GLenum format = s->fmt->format, type = s->fmt->type;
    if (s->fmt->conv == CONV_BGRA_TO_RGBA || (img->render_target && (s->format == 21 || s->format == 22 || s->format == 35)))
        format = GL_RGBA;
    d3d_gl_layout_to_rgba(format, type, buf, count, out);
    free(buf);
    uint32_t usage = s->container ? s->container->usage : (s->usage | D3DUSAGE_RENDERTARGET);
    GLint sw[4];
    d3d_storage_swizzle(s->fmt, usage, sw);
    d3d_swizzle_rgba(sw, out, count);
    return 1;
}

/* A whole GPU surface into guest memory in its own format (GetRenderTargetData, LockRect). */
void s2r_read_surface(d3d_surface *src, uint32_t dst_mem, uint32_t dst_pitch)
{
    uint32_t w = src->width, h = src->height, rect[4] = { 0, 0, w, h };
    uint8_t *buf = NULL;
    s2r_image *img = NULL;
    int bytes = src->format == 21 || src->format == 22;
    int ok = 0;
    if (bytes && read_gpu(src, rect, &buf, &img) && img->gpu == S2R_GPU_RGBA8) {
        /* render targets store natural RGBA, textures Direct3D's BGRA order */
        int swap = img->render_target;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = buf + (size_t)y * w * 4;
            uint8_t *d = (uint8_t *)GPTR(dst_mem + y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x) {
                d[4 * x] = s[4 * x + (swap ? 2 : 0)]; d[4 * x + 1] = s[4 * x + 1];
                d[4 * x + 2] = s[4 * x + (swap ? 0 : 2)]; d[4 * x + 3] = src->format == 22 ? 255 : s[4 * x + 3];
            }
        }
        ok = 1;
    } else {
        float *px = (float *)malloc((size_t)w * h * 16);
        if (px && s2r_read_rgba(src, rect, px)) {
            ok = 1;
            for (uint32_t y = 0; y < h; ++y)
                if (!d3d_format_encode(src->format, px + (size_t)y * w * 4, w, (uint8_t *)GPTR(dst_mem + y * dst_pitch))) {
                    S2R_LOG("read back of format %u unsupported", src->format);
                    break;
                }
        }
        free(px);
    }
    free(buf);
    if (!ok) {
        static int logged;
        if (logged++ < 8) S2R_LOG("surface %ux%u format %u cannot be read back", w, h, src->format);
        for (uint32_t y = 0; y < h; ++y) memset(GPTR(dst_mem + y * dst_pitch), 0, dst_pitch);
    }
}

void s2r_present(d3d_device *d, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns)
{
    s2r_dev->dev = d;
    s2r_dev->frame = d->frame;
    s2r_dev->be->present(s2r_dev);
    s2r_scale_follow(s2r_dev);
    d3d_texcache_report();
    s2r_frame_presented(s2r_dev, game_cpu_ns, game_wait_ns, syncs, rt_wait_ns);
    void_hud_present();
    if (void_android_frame_presented) void_android_frame_presented();
}

/* ------------------------------------------------------------------ JNI */
/* GameActivity, before nativeStart: the launcher's renderer (0 the old GLES
 * layer, 1 native OpenGL ES, 2 native Vulkan on the system driver, 3 on Mesa
 * Turnip, 4 on Mesa PanVK), where the APK's libraries were extracted (the
 * app-local drivers and adrenotools), the app cache (program binaries, SPIR-V,
 * the Vulkan pipeline cache). */
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeRenderer(JNIEnv *env, jclass type, jint mode,
    jstring library_dir, jstring cache)
{
    (void)type;
    if (mode < 0 || mode > 4 || (void_android_game_started && void_android_game_started())) return;
    port_renderer_failure_init();   /* after Launch.attach: the record's path and PORT_RENDERER */
    selected = mode >= 2 ? S2R_BACKEND_VULKAN : mode == 1 ? S2R_BACKEND_GLES : S2R_BACKEND_NONE;
    const char *dir = library_dir ? (*env)->GetStringUTFChars(env, library_dir, NULL) : NULL;
    s2r_set_vk_driver(mode == 3 ? S2R_VK_TURNIP : mode == 4 ? S2R_VK_PANVK : S2R_VK_SYSTEM, dir);
    if (dir) (*env)->ReleaseStringUTFChars(env, library_dir, dir);
    if (cache) {
        const char *c = (*env)->GetStringUTFChars(env, cache, NULL);
        if (c) {
            snprintf(cache_dir, sizeof cache_dir, "%s", c);
            (*env)->ReleaseStringUTFChars(env, cache, c);
        }
    }
}

/* The app's private cache folder (empty before nativeRenderer): the GLES program binaries go there too. */
const char *void_app_cache_dir(void) { return cache_dir; }

/* The renderer this session draws with once the device exists (-1 before): 0 the old GLES layer, 1 native GLES,
 * 2 Vulkan on the system driver, 3 on Turnip, 4 on PanVK. */
JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeRendererActive(JNIEnv *env, jclass type)
{
    (void)env; (void)type;
    if (!g_d3d_device) return -1;
    if (!active) return 0;
    if (device.backend_kind != S2R_BACKEND_VULKAN) return 1;
    int driver = s2r_vk_active_driver();
    return driver == S2R_VK_TURNIP ? 3 : driver == S2R_VK_PANVK ? 4 : 2;
}
