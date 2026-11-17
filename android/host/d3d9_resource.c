/* Direct3D 9 resources: surfaces, textures, buffers, vertex declarations,
 * queries, state blocks and the implicit swap chain. */
#include "d3d9_internal.h"
#include <EGL/egl.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include "d3d9_thread.inc"


/* ------------------------------------------------------------------ util */
static uint32_t mip_levels(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t n = 1, m = w > h ? w : h;
    if (d > m) m = d;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

static void level_dims(uint32_t w, uint32_t h, uint32_t level, uint32_t *lw, uint32_t *lh)
{
    *lw = w >> level; *lh = h >> level;
    if (!*lw) *lw = 1;
    if (!*lh) *lh = 1;
}

/* What the renderer holds on the GPU, for the [mem] report (iostat.c):
 * live objects and the bytes their storage takes (estimated from the
 * storage format; GL/driver padding not included). Render thread only. */
static uint64_t stat_textures, stat_texture_bytes, stat_surfaces, stat_surface_bytes, stat_buffers, stat_buffer_bytes;

static int compressed_storage(GLenum internal)
{
    /* ETC2 RGB8, ETC2 RGBA8, ASTC 4x4; ATC RGB, explicit and interpolated alpha */
    return internal == 0x9274 || internal == 0x9278 || internal == 0x93B0 || internal == 0x8C92 || internal == 0x8C93 ||
           internal == 0x87EE;
}

static uint64_t storage_bytes(GLenum internal, uint32_t bits, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
    uint32_t faces)
{
    uint64_t total = 0;
    if (compressed_storage(internal)) bits = internal == 0x9274 || internal == 0x8C92 ? 4 : 8;
    for (uint32_t l = 0; l < levels; ++l) {
        uint64_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
        if (compressed_storage(internal)) { lw = (lw + 3) & ~3ull; lh = (lh + 3) & ~3ull; }
        total += lw * lh * ld * bits / 8;
    }
    return total * faces;
}

void d3d_gpu_stats(void_gpu_stats *out)
{
    out->textures = stat_textures; out->texture_bytes = stat_texture_bytes;
    out->surfaces = stat_surfaces; out->surface_bytes = stat_surface_bytes;
    out->buffers = stat_buffers; out->buffer_bytes = stat_buffer_bytes;
    d3d_shader_stats(&out->programs, &out->links, &out->link_ns, &out->compiles, &out->compile_ns);
}

static GLenum face_target(d3d_texture *t, uint32_t face)
{
    return t->rtype == D3DRTYPE_CUBETEXTURE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : t->target;
}

static int is_render_target_usage(uint32_t usage) { return (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0; }

static void apply_swizzle(GLenum target, const d3d_format *f, uint32_t usage)
{
    GLint sw[4] = { f->swizzle[0], f->swizzle[1], f->swizzle[2], f->swizzle[3] };
    if ((usage & D3DUSAGE_RENDERTARGET) && (f->d3d == 21 || f->d3d == 22 || f->d3d == 35)) {
        /* GPU-written BGRA formats are stored as RGBA. */
        sw[0] = GL_RED; sw[1] = GL_GREEN; sw[2] = GL_BLUE;
        sw[3] = f->d3d == 22 ? GL_ONE : GL_ALPHA;
    }
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, sw[0]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, sw[1]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, sw[2]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, sw[3]);
}

/* ================================================================ surfaces */
static void_com_class surface_class;

uint32_t d3d_wrap_surface(d3d_surface *s) { return s->com.guest; }

/* Resources die on the render thread, after every call recorded before
 * that may still use them; their guest memory goes on the game's thread. */
static void run_surface_destroy(void *args)
{
    d3d_surface *s = *(d3d_surface **)args;
    if (s->screen_sized) d3d_scaled_remove(s);
    if (s->gl_msaa_depth) { glDeleteRenderbuffers(1, &s->gl_msaa_depth); s->gl_msaa_depth = 0; s->gl_msaa_samples = 0; }
    if (s->gl) {
        d3d_gl_forget_texture(s->gl); glDeleteTextures(1, &s->gl);
        --stat_surfaces; stat_surface_bytes -= s->gl_bytes;
    }
    free(s);
}

static void surface_destroy(void_com_object *o)
{
    d3d_surface *s = (d3d_surface *)o;
    if (s->sysmem) { void_gfree(s->sysmem); s->sysmem = 0; }
    d3d_rt_call(run_surface_destroy, &s, sizeof s);
}

static void run_surface_create(void *args)
{
    d3d_surface *s = *(d3d_surface **)args;
    if (s->screen_sized) d3d_scaled_add(s, NULL);
    d3d_surface_storage(s);
}

d3d_surface *d3d_surface_new(d3d_device *dev, uint32_t w, uint32_t h, uint32_t format, uint32_t usage,
    uint32_t pool, uint32_t multisample)
{
    d3d_surface *s = (d3d_surface *)calloc(1, sizeof *s);
    s->dev = dev;
    s->width = w ? w : 1; s->height = h ? h : 1;
    s->format = format; s->usage = usage; s->pool = pool; s->multisample = multisample;
    s->fmt = d3d_format_find(format);
    s->pitch = s->fmt ? d3d_format_pitch(s->fmt, s->width) : s->width * 4;
    void_com_wrap(&s->com, &surface_class, 0);
    s->com.destroy = surface_destroy;
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) {
        s->sysmem = void_galloc(s->pitch * (s->fmt ? d3d_format_rows(s->fmt, s->height) : s->height));
    } else if (s->fmt && s->fmt->internal) {
        /* The back buffer and the targets of its size follow the render scale. */
        s->scale = 1.0f;
        if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && dev && s->width == dev->pp[0] &&
            s->height == dev->pp[1]) {
            s->scale = d3d_screen_scale(dev->pp[0]);
            s->screen_sized = 1;
        }
        d3d_rt_call(run_surface_create, &s, sizeof s);
    }
    return s;
}

/* A standalone surface's GL texture at its scaled size (again when the
 * render scale changes: the content is gone, as after a device reset). */
void d3d_surface_storage(d3d_surface *s)
{
    if (s->gl_msaa_depth) { glDeleteRenderbuffers(1, &s->gl_msaa_depth); s->gl_msaa_depth = 0; s->gl_msaa_samples = 0; }
    if (s->gl) {
        d3d_gl_forget_texture(s->gl); glDeleteTextures(1, &s->gl);
        --stat_surfaces; stat_surface_bytes -= s->gl_bytes;
    }
    glGenTextures(1, &s->gl);
    s->gl_bytes = storage_bytes(s->fmt->internal, s->fmt->gl_bits, d3d_scaled_size(s->width, s->scale),
        d3d_scaled_size(s->height, s->scale), 1, 1, 1);
    ++stat_surfaces; stat_surface_bytes += s->gl_bytes;
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, s->gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, s->fmt->internal, (GLsizei)d3d_scaled_size(s->width, s->scale),
        (GLsizei)d3d_scaled_size(s->height, s->scale));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    apply_swizzle(GL_TEXTURE_2D, s->fmt, s->usage | (s->fmt->depth ? 0 : D3DUSAGE_RENDERTARGET));
    GLenum e = glGetError();
    if (e) void_log("[d3d9] surface %ux%u (x%.2f) format %u: GL error %04X", s->width, s->height, s->scale, s->format, e);
}

GLuint d3d_surface_gl_texture(d3d_surface *s, GLenum *target, GLint *level, GLint *layer)
{
    if (s->container) {
        *target = face_target(s->container, s->face);
        *level = (GLint)s->level;
        *layer = (GLint)s->face;
        return s->container->gl;
    }
    *target = GL_TEXTURE_2D;
    *level = 0;
    *layer = 0;
    return s->gl;
}

/* Surfaces of a texture share the texture's reference count. */
static void_com_object *refcount_owner(d3d_surface *s) { return s->container ? &s->container->com : &s->com; }
void_com_object *d3d_surface_owner(d3d_surface *s) { return refcount_owner(s); }

COM_METHOD(surf, QueryInterface, 3)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    if (void_guid_equal(ARG(1), IID_IDirect3DSurface9) || void_guid_equal(ARG(1), IID_IDirect3DResource9) ||
        void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(refcount_owner(s));
        MEM32(ARG(2)) = THIS_;
        return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(surf, AddRef, 1) { return void_com_addref(refcount_owner((d3d_surface *)void_com_host(THIS_))); }
COM_METHOD(surf, Release, 1) { return void_com_release(refcount_owner((d3d_surface *)void_com_host(THIS_))); }
COM_METHOD(surf, GetDevice, 2)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    void_com_addref(&s->dev->com);
    MEM32(ARG(1)) = s->dev->com.guest;
    return D3D_OK;
}
COM_METHOD(surf, SetPrivateData, 5) { return D3D_OK; }
COM_METHOD(surf, GetPrivateData, 4) { return D3DERR_NOTFOUND; }
COM_METHOD(surf, FreePrivateData, 2) { return D3D_OK; }
COM_METHOD(surf, SetPriority, 2) { return 0; }
COM_METHOD(surf, GetPriority, 1) { return 0; }
COM_METHOD(surf, PreLoad, 1) { return 0; }
COM_METHOD(surf, GetType, 1) { return D3DRTYPE_SURFACE; }
COM_METHOD(surf, GetContainer, 3)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    if (s->container) {
        void_com_addref(&s->container->com);
        MEM32(ARG(2)) = s->container->com.guest;
    } else {
        void_com_addref(&s->dev->com);
        MEM32(ARG(2)) = s->dev->com.guest;
    }
    return D3D_OK;
}
COM_METHOD(surf, GetDesc, 2)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    uint32_t d = ARG(1);
    MEM32(d) = s->format; MEM32(d + 4) = D3DRTYPE_SURFACE; MEM32(d + 8) = s->usage; MEM32(d + 12) = s->pool;
    MEM32(d + 16) = s->multisample; MEM32(d + 20) = 0; MEM32(d + 24) = s->width; MEM32(d + 28) = s->height;
    return D3D_OK;
}

/* Guest storage that backs a lock: persistent for SYSTEMMEM/SCRATCH and
 * MANAGED textures (D3D keeps their system copy too), temporary otherwise. */
static uint32_t surface_storage(d3d_surface *s, int *persistent)
{
    *persistent = 1;
    if (s->sysmem) return s->sysmem;
    if (s->container) {
        d3d_texture *t = s->container;
        uint32_t index = s->face * t->levels + s->level;
        if (t->sysmem) {
            if (!t->sysmem[index]) t->sysmem[index] = void_galloc(s->pitch * d3d_format_rows(s->fmt, s->height));
            return t->sysmem[index];
        }
    }
    *persistent = 0;
    return void_galloc(s->pitch * d3d_format_rows(s->fmt, s->height));
}

COM_METHOD(surf, LockRect, 4)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    uint32_t out = ARG(1), rect = ARG(2), flags = ARG(3);
    if (s->locked) return D3DERR_INVALIDCALL;
    if (!s->fmt) return D3DERR_INVALIDCALL;
    int persistent;
    uint32_t mem = surface_storage(s, &persistent);
    s->lock_mem = mem;
    s->lock_flags = flags;
    s->locked = persistent ? 1 : 2;
    uint32_t x = 0, y = 0;
    s->lock_whole = !rect;
    if (rect) {
        x = MEM32(rect); y = MEM32(rect + 4);
        for (int i = 0; i < 4; ++i) s->lock_rect[i] = MEM32(rect + 4u * (uint32_t)i);
    } else {
        s->lock_rect[0] = s->lock_rect[1] = 0; s->lock_rect[2] = s->width; s->lock_rect[3] = s->height;
    }
    int gpu = s->container ? s->container->has_storage
                           : (s->fmt->internal && s->pool != D3DPOOL_SYSTEMMEM && s->pool != D3DPOOL_SCRATCH);
    if (!persistent && !(flags & D3DLOCK_DISCARD) && !s->fmt->block_bytes && !s->fmt->depth && gpu)
        d3d_read_surface(NULL, s, mem, s->pitch);   /* reading a GPU-only surface */
    uint32_t offset = s->fmt->block_bytes ? (y / 4) * s->pitch + (x / 4) * s->fmt->block_bytes
                                          : y * s->pitch + x * (s->fmt->bits / 8);
    MEM32(out) = s->pitch;
    MEM32(out + 4) = mem + offset;
    return D3D_OK;
}

/* A standalone GPU surface's rect from guest-layout rows (copied: the render
 * thread converts and uploads them). */
typedef struct { d3d_surface *s; uint32_t x0, y0, x1, y1, pitch; } surface_upload_args;   /* then the rows */

static void run_surface_upload(void *args)
{
    surface_upload_args *a = (surface_upload_args *)args;
    d3d_surface *s = a->s;
    const uint8_t *data = (const uint8_t *)(a + 1);
    void *owned;
    size_t size;
    const void *gl_data = d3d_format_convert(s->fmt, data, a->x1 - a->x0, a->y1 - a->y0, a->pitch, &owned, &size, 1);
    if (s->scale != 1.0f) {
        d3d_gl_upload_scaled(s, a->x0, a->y0, a->x1 - a->x0, a->y1 - a->y0, s->fmt->format, s->fmt->type, gl_data);
    } else {
        d3d_gl_upload_unit();
        glBindTexture(GL_TEXTURE_2D, s->gl);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)a->x0, (GLint)a->y0, (GLsizei)(a->x1 - a->x0), (GLsizei)(a->y1 - a->y0),
            s->fmt->format, s->fmt->type, gl_data);
    }
    free(owned);
}

/* Snapshot guest rows before returning to their writer. Source and destination pitches may differ. */
static void surface_upload_data(d3d_surface *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    if (s->container) {
        d3d_texture_upload_rect(s->container, s->face, s->level, x0, y0, w, h, data, pitch);
    } else if (s->fmt->internal && !s->fmt->depth && s->pool != D3DPOOL_SYSTEMMEM && s->pool != D3DPOOL_SCRATCH) {
        /* Standalone GPU surfaces are stored like render targets (d3d_surface_new). */
        uint32_t row = d3d_format_pitch(s->fmt, w), rows = d3d_format_rows(s->fmt, h);
        surface_upload_args *a = (surface_upload_args *)d3d_rt_begin(run_surface_upload, (uint32_t)sizeof *a + row * rows);
        a->s = s; a->x0 = x0; a->y0 = y0; a->x1 = x0 + w; a->y1 = y0 + h; a->pitch = row;
        for (uint32_t y = 0; y < rows; ++y) memcpy((uint8_t *)(a + 1) + (size_t)y * row, data + (size_t)y * pitch, row);
        d3d_rt_commit();
    }
}

/* Upload rect of a locked whole-surface allocation. Widen block formats to complete blocks. */
static void surface_upload_rect(d3d_surface *s, uint32_t mem, const uint32_t *rect)
{
    uint32_t x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
    if (s->fmt->block_bytes) {
        x0 &= ~3u; y0 &= ~3u;
        x1 = (x1 + 3) & ~3u; y1 = (y1 + 3) & ~3u;
    }
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t offset = s->fmt->block_bytes ? (y0 / 4) * s->pitch + (x0 / 4) * s->fmt->block_bytes
                                          : y0 * s->pitch + x0 * (s->fmt->bits / 8);
    surface_upload_data(s, x0, y0, x1 - x0, y1 - y0, (const uint8_t *)GPTR(mem + offset), s->pitch);
}

void d3d_surface_upload(d3d_surface *s)
{
    if (!s->lock_mem || !s->fmt) return;
    uint32_t whole[4] = { 0, 0, s->width, s->height };
    surface_upload_rect(s, s->lock_mem, s->lock_whole ? whole : s->lock_rect);
}

/* Rect (x0, y0, x1, y1) of a surface as logical RGBA, from its guest copy when
 * it has one, otherwise from the GPU. */
int d3d_surface_read_rgba(d3d_surface *s, const uint32_t *rect, float *out)
{
    if (!s->fmt) return 0;
    uint32_t w = rect[2] - rect[0], h = rect[3] - rect[1];
    uint32_t mem = s->sysmem;
    if (!mem && s->container && s->container->sysmem)
        mem = s->container->sysmem[s->face * s->container->levels + s->level];
    if (!mem) return d3d_read_rgba(s, rect, out);
    if (s->fmt->block_bytes) {
        /* Decode the whole blocks covering the rect, then crop. */
        uint32_t bx = rect[0] & ~3u, by = rect[1] & ~3u;
        uint32_t bw = ((rect[2] + 3) & ~3u) - bx, bh = ((rect[3] + 3) & ~3u) - by;
        float *tmp = (float *)malloc((size_t)bw * bh * 16);
        d3d_format_decode(s->fmt, (const uint8_t *)GPTR(mem + (by / 4) * s->pitch + (bx / 4) * s->fmt->block_bytes),
            bw, bh, s->pitch, tmp);
        for (uint32_t y = 0; y < h; ++y)
            memcpy(out + (size_t)y * w * 4, tmp + ((size_t)(rect[1] - by + y) * bw + (rect[0] - bx)) * 4, (size_t)w * 16);
        free(tmp);
        return 1;
    }
    d3d_format_decode(s->fmt, (const uint8_t *)GPTR(mem + rect[1] * s->pitch + rect[0] * (s->fmt->bits / 8)), w, h,
        s->pitch, out);
    return 1;
}

/* Store logical RGBA into rect (x0, y0, x1, y1) of a surface: into its guest
 * copy when it has one, and into its GPU copy when it has one. */
int d3d_surface_write_rgba(d3d_surface *s, const uint32_t *rect, const float *rgba)
{
    if (!s->fmt || s->fmt->block_bytes || s->fmt->depth) return 0;
    uint32_t w = rect[2] - rect[0], h = rect[3] - rect[1], bpp = s->fmt->bits / 8;
    uint32_t mem = s->sysmem;
    if (!mem && s->container && s->container->sysmem) {
        uint32_t index = s->face * s->container->levels + s->level;
        if (!s->container->sysmem[index])
            s->container->sysmem[index] = void_galloc(s->pitch * d3d_format_rows(s->fmt, s->height));
        mem = s->container->sysmem[index];
    }
    int temporary = !mem;
    if (temporary) mem = void_galloc(s->pitch * d3d_format_rows(s->fmt, s->height));
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = (uint8_t *)GPTR(mem + (rect[1] + y) * s->pitch + rect[0] * bpp);
        if (!d3d_format_encode(s->format, rgba + (size_t)y * w * 4, w, row)) {
            if (temporary) void_gfree(mem);
            return 0;
        }
    }
    if (!s->sysmem) surface_upload_rect(s, mem, rect);
    if (temporary) void_gfree(mem);
    return 1;
}

COM_METHOD(surf, UnlockRect, 1)
{
    d3d_surface *s = (d3d_surface *)void_com_host(THIS_);
    if (!s->locked) return D3DERR_INVALIDCALL;
    if (!(s->lock_flags & D3DLOCK_READONLY) && !s->sysmem) d3d_surface_upload(s);
    if (s->locked == 2) void_gfree(s->lock_mem);
    s->locked = 0;
    s->lock_mem = 0;
    return D3D_OK;
}

COM_METHOD(surf, GetDC, 2) { void_log("[d3d9] IDirect3DSurface9::GetDC unsupported"); return D3DERR_INVALIDCALL; }
COM_METHOD(surf, ReleaseDC, 2) { return D3D_OK; }

static const void_fn surface_methods[] = {
    surf_QueryInterface, surf_AddRef, surf_Release, surf_GetDevice, surf_SetPrivateData, surf_GetPrivateData,
    surf_FreePrivateData, surf_SetPriority, surf_GetPriority, surf_PreLoad, surf_GetType, surf_GetContainer,
    surf_GetDesc, surf_LockRect, surf_UnlockRect, surf_GetDC, surf_ReleaseDC,
};
static void_com_class surface_class = { "IDirect3DSurface9", 17, surface_methods, 0 };

/* ================================================================ textures */
static void_com_class texture_class, cube_class, volume_class;

static void run_texture_destroy(void *args)
{
    d3d_texture *t = *(d3d_texture **)args;
    if (t->screen_sized) d3d_scaled_remove(t);
    if (t->gl) {
        d3d_gl_forget_texture(t->gl); glDeleteTextures(1, &t->gl);
        --stat_textures; stat_texture_bytes -= t->gl_bytes;
    }
    free(t->surfaces);
    free(t);
}

static void texture_destroy(void_com_object *o)
{
    d3d_texture *t = (d3d_texture *)o;
    int n = t->faces * (int)t->levels;
    for (int i = 0; i < n; ++i) {
        if (t->surfaces && t->surfaces[i]) {
            /* The level surfaces go first: each records its own end, before
             * the texture's (the render thread may still name them). */
            d3d_surface *s = t->surfaces[i];
            s->com.refs = 1;
            void_com_release(&s->com);
        }
        if (t->sysmem && t->sysmem[i]) void_gfree(t->sysmem[i]);
    }
    free(t->sysmem);
    t->sysmem = NULL;
    d3d_rt_call(run_texture_destroy, &t, sizeof t);
}

static void run_texture_create(void *args)
{
    d3d_texture *t = *(d3d_texture **)args;
    if (t->screen_sized) d3d_scaled_add(NULL, t);
    d3d_texture_storage(t);
}

void d3d_texture_level_size(d3d_texture *t, uint32_t level, uint32_t *w, uint32_t *h)
{
    level_dims(t->width, t->height, level, w, h);
}

d3d_texture *d3d_texture_new(d3d_device *dev, uint32_t rtype, uint32_t w, uint32_t h, uint32_t d,
    uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool)
{
    const d3d_format *f = d3d_format_find(format);
    if (!f || !w || !h) return NULL;
    uint32_t full = mip_levels(w, h, rtype == D3DRTYPE_VOLUMETEXTURE ? d : 1);
    if (usage & D3DUSAGE_AUTOGENMIPMAP) levels = (levels == 0 || levels > 1) ? full : 1;
    if (!levels || levels > full) levels = full;
    d3d_texture *t = (d3d_texture *)calloc(1, sizeof *t);
    t->dev = dev;
    t->rtype = rtype;
    t->width = w; t->height = h; t->depth = d ? d : 1;
    t->levels = levels; t->usage = usage; t->format = format; t->pool = pool;
    t->fmt = f;
    t->faces = rtype == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    t->target = rtype == D3DRTYPE_CUBETEXTURE ? GL_TEXTURE_CUBE_MAP : rtype == D3DRTYPE_VOLUMETEXTURE ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    t->autogen_filter = 2;
    t->surfaces = (d3d_surface **)calloc((size_t)t->faces * levels, sizeof *t->surfaces);
    if (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
        t->sysmem = (uint32_t *)calloc((size_t)t->faces * levels, sizeof *t->sysmem);
    t->scale = 1.0f;
    if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH && f->internal) {
        /* A screen-sized target texture (a copy of the frame, a full-screen
         * pass) follows the render scale like the back buffer. */
        if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && rtype == D3DRTYPE_TEXTURE && levels == 1 &&
            dev && w == dev->pp[0] && h == dev->pp[1]) {
            t->scale = d3d_screen_scale(dev->pp[0]);
            t->screen_sized = 1;
        }
        t->has_storage = 1;
        d3d_rt_call(run_texture_create, &t, sizeof t);
    }
    return t;
}

/* A texture's GL storage at its scaled size (again when the render scale
 * changes). */
void d3d_texture_storage(d3d_texture *t)
{
    if (t->gl) {
        d3d_gl_forget_texture(t->gl); glDeleteTextures(1, &t->gl);
        --stat_textures; stat_texture_bytes -= t->gl_bytes;
    }
    glGenTextures(1, &t->gl);
    d3d_gl_upload_unit();
    glBindTexture(t->target, t->gl);
    GLsizei w = (GLsizei)d3d_scaled_size(t->width, t->scale), h = (GLsizei)d3d_scaled_size(t->height, t->scale);
    GLenum internal = t->gl_internal ? t->gl_internal : t->fmt->internal;
    t->gl_bytes = storage_bytes(internal, t->fmt->gl_bits, (uint32_t)w, (uint32_t)h,
        t->target == GL_TEXTURE_3D ? t->depth : 1, t->levels, (uint32_t)t->faces);
    ++stat_textures; stat_texture_bytes += t->gl_bytes;
    if (t->target == GL_TEXTURE_3D)
        glTexStorage3D(GL_TEXTURE_3D, (GLsizei)t->levels, internal, w, h, (GLsizei)t->depth);
    else
        glTexStorage2D(t->target, (GLsizei)t->levels, internal, w, h);
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    apply_swizzle(t->target, t->fmt, t->usage);
    GLenum e = glGetError();
    if (e) void_log("[d3d9] texture %ux%u (x%.2f) levels %u format %u: GL error %04X", t->width, t->height, t->scale,
        t->levels, t->format, e);
}

uint32_t d3d_wrap_texture(d3d_texture *t)
{
    void_com_class *cls = t->rtype == D3DRTYPE_CUBETEXTURE ? &cube_class : t->rtype == D3DRTYPE_VOLUMETEXTURE ? &volume_class : &texture_class;
    uint32_t guest = void_com_wrap(&t->com, cls, 0);
    t->com.destroy = texture_destroy;
    return guest;
}

uint32_t d3d_texture_surface(d3d_texture *t, uint32_t face, uint32_t level)
{
    if (level >= t->levels || face >= (uint32_t)t->faces) return 0;
    uint32_t index = face * t->levels + level;
    if (!t->surfaces[index]) {
        d3d_surface *s = (d3d_surface *)calloc(1, sizeof *s);
        s->dev = t->dev;
        level_dims(t->width, t->height, level, &s->width, &s->height);
        s->format = t->format; s->usage = t->usage; s->pool = t->pool;
        s->fmt = t->fmt;
        s->pitch = d3d_format_pitch(t->fmt, s->width);
        s->container = t;
        s->face = face;
        s->level = level;
        void_com_wrap(&s->com, &surface_class, 0);
        s->com.destroy = surface_destroy;
        t->surfaces[index] = s;
    }
    return t->surfaces[index]->com.guest;
}

/* Upload a w x h rect at (x, y) of a level from guest-layout data whose first
 * row (or block row) is at `data`: copied for the render thread, which
 * converts (DXT decoding among others) and uploads. */
typedef struct { d3d_texture *t; uint32_t face, level, x, y, w, h, pitch; } texture_upload_args;   /* then the rows */
static void texture_upload_gl(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch);

static void run_texture_upload(void *args)
{
    texture_upload_args *a = (texture_upload_args *)args;
    texture_upload_gl(a->t, a->face, a->level, a->x, a->y, a->w, a->h, (const uint8_t *)(a + 1), a->pitch);
}

void d3d_texture_upload_rect(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    if (!t->has_storage || !w || !h) return;
    uint32_t depth = t->target == GL_TEXTURE_3D ? (t->depth >> level ? t->depth >> level : 1) : 1;
    uint32_t row = d3d_format_pitch(t->fmt, w), rows = d3d_format_rows(t->fmt, h) * depth;
    texture_upload_args *a = (texture_upload_args *)d3d_rt_begin(run_texture_upload, (uint32_t)sizeof *a + row * rows);
    a->t = t; a->face = face; a->level = level; a->x = x; a->y = y; a->w = w; a->h = h; a->pitch = row;
    for (uint32_t r = 0; r < rows; ++r) memcpy((uint8_t *)(a + 1) + (size_t)r * row, data + (size_t)r * pitch, row);
    d3d_rt_commit();
}

static void texture_upload_gl(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    if (!t->gl || !w || !h) return;
    /* DXT levels the launcher converted to ETC2/ASTC go up as they are. */
    if (t->fmt->block_bytes && d3d_texcache_upload(t, face, level, x, y, w, h, data, pitch)) return;
    void *owned;
    size_t size;
    const void *gl_data = d3d_format_convert(t->fmt, data, w, h, pitch, &owned, &size, (t->usage & D3DUSAGE_RENDERTARGET) != 0);
    if (t->scale != 1.0f && t->target == GL_TEXTURE_2D && level == 0) {
        d3d_surface level0;
        memset(&level0, 0, sizeof level0);
        level0.container = t;
        level0.width = t->width; level0.height = t->height;
        level0.format = t->format; level0.fmt = t->fmt;
        d3d_gl_upload_scaled(&level0, x, y, w, h, t->fmt->format, t->fmt->type, gl_data);
        free(owned);
        return;
    }
    d3d_gl_upload_unit();
    glBindTexture(t->target, t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->target == GL_TEXTURE_3D) {
        uint32_t d = t->depth >> level;
        if (!d) d = 1;
        glTexSubImage3D(GL_TEXTURE_3D, (GLint)level, (GLint)x, (GLint)y, 0, (GLsizei)w, (GLsizei)h, (GLsizei)d, t->fmt->format,
            t->fmt->type, gl_data);
    } else {
        glTexSubImage2D(face_target(t, face), (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->fmt->format,
            t->fmt->type, gl_data);
    }
    GLenum e = glGetError();
    if (e) void_log("[d3d9] upload %ux%u at %u,%u level %u format %u: GL error %04X", w, h, x, y, level, t->format, e);
    free(owned);
    if (level == 0 && (t->usage & D3DUSAGE_AUTOGENMIPMAP)) t->mips_dirty = 1;
}

void d3d_texture_upload_level(d3d_texture *t, uint32_t face, uint32_t level, const uint8_t *data, uint32_t pitch)
{
    uint32_t w, h;
    level_dims(t->width, t->height, level, &w, &h);
    d3d_texture_upload_rect(t, face, level, 0, 0, w, h, data, pitch);
}

void d3d_generate_mips(d3d_texture *t)
{
    if (!t->gl || t->levels < 2) return;
    d3d_gl_upload_unit();
    glBindTexture(t->target, t->gl);
    glGenerateMipmap(t->target);
    t->mips_dirty = 0;
}

static void run_generate_mips(void *args) { d3d_generate_mips(*(d3d_texture **)args); }
void d3d_generate_mips_later(d3d_texture *t) { d3d_rt_call(run_generate_mips, &t, sizeof t); }

typedef struct { d3d_texture *t; uint32_t lod; } lod_args;
static void run_set_lod(void *args) { lod_args *a = (lod_args *)args; a->t->gl_lod = a->lod; }

#define TEX ((d3d_texture *)void_com_host(THIS_))

COM_METHOD(tex, QueryInterface, 3)
{
    d3d_texture *t = TEX;
    /* Each texture kind answers its own interface only: the renderer tells
     * 2D, cube and volume textures apart with these queries. */
    const uint8_t *own = t->rtype == D3DRTYPE_CUBETEXTURE ? IID_IDirect3DCubeTexture9
                       : t->rtype == D3DRTYPE_VOLUMETEXTURE ? IID_IDirect3DVolumeTexture9 : IID_IDirect3DTexture9;
    if (void_guid_equal(ARG(1), own) ||
        void_guid_equal(ARG(1), IID_IDirect3DBaseTexture9) || void_guid_equal(ARG(1), IID_IDirect3DResource9) ||
        void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&t->com);
        MEM32(ARG(2)) = THIS_;
        return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(tex, AddRef, 1) { return void_com_addref(&TEX->com); }
COM_METHOD(tex, Release, 1) { return void_com_release(&TEX->com); }
COM_METHOD(tex, GetDevice, 2) { d3d_texture *t = TEX; void_com_addref(&t->dev->com); MEM32(ARG(1)) = t->dev->com.guest; return D3D_OK; }
COM_METHOD(tex, SetPrivateData, 5) { return D3D_OK; }
COM_METHOD(tex, GetPrivateData, 4) { return D3DERR_NOTFOUND; }
COM_METHOD(tex, FreePrivateData, 2) { return D3D_OK; }
COM_METHOD(tex, SetPriority, 2) { return 0; }
COM_METHOD(tex, GetPriority, 1) { return 0; }
COM_METHOD(tex, PreLoad, 1) { return 0; }
COM_METHOD(tex, GetType, 1) { return TEX->rtype; }
COM_METHOD(tex, SetLOD, 2)
{
    d3d_texture *t = TEX;
    uint32_t old = t->lod;
    t->lod = ARG(1) < t->levels ? ARG(1) : t->levels - 1;
    lod_args a = { t, t->lod };
    d3d_rt_call(run_set_lod, &a, sizeof a);
    return old;
}
COM_METHOD(tex, GetLOD, 1) { return TEX->lod; }
COM_METHOD(tex, GetLevelCount, 1) { d3d_texture *t = TEX; return (t->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : t->levels; }
COM_METHOD(tex, SetAutoGenFilterType, 2) { TEX->autogen_filter = ARG(1); return D3D_OK; }
COM_METHOD(tex, GetAutoGenFilterType, 1) { return TEX->autogen_filter; }
COM_METHOD(tex, GenerateMipSubLevels, 1) { d3d_generate_mips_later(TEX); return 0; }

static uint32_t level_desc(d3d_texture *t, uint32_t level, uint32_t out)
{
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    uint32_t w, h;
    level_dims(t->width, t->height, level, &w, &h);
    MEM32(out) = t->format; MEM32(out + 4) = D3DRTYPE_SURFACE; MEM32(out + 8) = t->usage; MEM32(out + 12) = t->pool;
    MEM32(out + 16) = 0; MEM32(out + 20) = 0; MEM32(out + 24) = w; MEM32(out + 28) = h;
    return D3D_OK;
}

COM_METHOD(tex, GetLevelDesc, 3) { return level_desc(TEX, ARG(1), ARG(2)); }
COM_METHOD(tex, GetSurfaceLevel, 3)
{
    d3d_texture *t = TEX;
    uint32_t s = d3d_texture_surface(t, 0, ARG(1));
    if (!s) return D3DERR_INVALIDCALL;
    void_com_addref(&t->com);
    MEM32(ARG(2)) = s;
    return D3D_OK;
}

/* LockRect on the texture is LockRect on its level surface. */
static uint32_t surface_call(uint32_t surface_guest, int method, int nargs, const uint32_t *args)
{
    uint32_t call_args[4];
    call_args[0] = surface_guest;
    for (int i = 0; i < nargs; ++i) call_args[i + 1] = args[i];
    uint32_t vt = MEM32(surface_guest);
    return void_call_guest(MEM32(vt + 4u * (uint32_t)method), nargs + 1, call_args, 1);
}

COM_METHOD(tex, LockRect, 5)
{
    uint32_t s = d3d_texture_surface(TEX, 0, ARG(1));
    if (!s) return D3DERR_INVALIDCALL;
    uint32_t args[3] = { ARG(2), ARG(3), ARG(4) };
    return surface_call(s, 13, 3, args);
}
COM_METHOD(tex, UnlockRect, 2)
{
    uint32_t s = d3d_texture_surface(TEX, 0, ARG(1));
    if (!s) return D3DERR_INVALIDCALL;
    return surface_call(s, 14, 0, NULL);
}
COM_METHOD(tex, AddDirtyRect, 2) { return D3D_OK; }

static const void_fn texture_methods[] = {
    tex_QueryInterface, tex_AddRef, tex_Release, tex_GetDevice, tex_SetPrivateData, tex_GetPrivateData,
    tex_FreePrivateData, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType, tex_SetLOD, tex_GetLOD,
    tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType, tex_GenerateMipSubLevels,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect, tex_AddDirtyRect,
};
static void_com_class texture_class = { "IDirect3DTexture9", 22, texture_methods, 0 };

COM_METHOD(cube, GetCubeMapSurface, 4)
{
    d3d_texture *t = TEX;
    uint32_t s = d3d_texture_surface(t, ARG(1), ARG(2));
    if (!s) return D3DERR_INVALIDCALL;
    void_com_addref(&t->com);
    MEM32(ARG(3)) = s;
    return D3D_OK;
}
COM_METHOD(cube, LockRect, 6)
{
    uint32_t s = d3d_texture_surface(TEX, ARG(1), ARG(2));
    if (!s) return D3DERR_INVALIDCALL;
    uint32_t args[3] = { ARG(3), ARG(4), ARG(5) };
    return surface_call(s, 13, 3, args);
}
COM_METHOD(cube, UnlockRect, 3)
{
    uint32_t s = d3d_texture_surface(TEX, ARG(1), ARG(2));
    if (!s) return D3DERR_INVALIDCALL;
    return surface_call(s, 14, 0, NULL);
}
COM_METHOD(cube, AddDirtyRect, 3) { return D3D_OK; }

static const void_fn cube_methods[] = {
    tex_QueryInterface, tex_AddRef, tex_Release, tex_GetDevice, tex_SetPrivateData, tex_GetPrivateData,
    tex_FreePrivateData, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType, tex_SetLOD, tex_GetLOD,
    tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType, tex_GenerateMipSubLevels,
    tex_GetLevelDesc, cube_GetCubeMapSurface, cube_LockRect, cube_UnlockRect, cube_AddDirtyRect,
};
static void_com_class cube_class = { "IDirect3DCubeTexture9", 22, cube_methods, 0 };

COM_TODO(vol, GetVolumeLevel)
COM_TODO(vol, LockBox)
COM_TODO(vol, UnlockBox)
COM_METHOD(vol, AddDirtyBox, 2) { return D3D_OK; }
static const void_fn volume_methods[] = {
    tex_QueryInterface, tex_AddRef, tex_Release, tex_GetDevice, tex_SetPrivateData, tex_GetPrivateData,
    tex_FreePrivateData, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType, tex_SetLOD, tex_GetLOD,
    tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType, tex_GenerateMipSubLevels,
    tex_GetLevelDesc, vol_GetVolumeLevel, vol_LockBox, vol_UnlockBox, vol_AddDirtyBox,
};
static void_com_class volume_class = { "IDirect3DVolumeTexture9", 22, volume_methods, 0 };

/* UpdateSurface / UpdateTexture: system memory -> default pool. */
uint32_t d3d_update_surface(d3d_surface *src, uint32_t src_rect, d3d_surface *dst, uint32_t dst_point)
{
    if (!src || !dst || src == dst || src->dev != dst->dev || !src->fmt || !dst->fmt ||
        src->format != dst->format || src->fmt->depth || !dst->fmt->internal ||
        src->pool != D3DPOOL_SYSTEMMEM || dst->pool != D3DPOOL_DEFAULT ||
        src->locked || dst->locked || src->multisample || dst->multisample ||
        (src->usage & D3DUSAGE_RENDERTARGET) || (dst->container && !dst->container->has_storage) ||
        (src_rect && !void_vm_readable(src_rect, 16)) || (dst_point && !void_vm_readable(dst_point, 8)))
        return D3DERR_INVALIDCALL;
    uint32_t r[4] = { 0, 0, src->width, src->height }, x = 0, y = 0;
    if (src_rect) for (unsigned i = 0; i < 4; ++i) r[i] = MEM32(src_rect + i * 4);
    if (dst_point) { x = MEM32(dst_point); y = MEM32(dst_point + 4); }
    if (r[2] <= r[0] || r[3] <= r[1] || r[2] > src->width || r[3] > src->height ||
        x > dst->width || y > dst->height || r[2] - r[0] > dst->width - x || r[3] - r[1] > dst->height - y)
        return D3DERR_INVALIDCALL;
    uint32_t w = r[2] - r[0], h = r[3] - r[1], block = src->fmt->block_bytes;
    if (block && ((r[0] | r[1] | x | y) & 3u ||
        ((w & 3u) && (r[2] != src->width || x + w != dst->width)) ||
        ((h & 3u) && (r[3] != src->height || y + h != dst->height)))) return D3DERR_INVALIDCALL;
    uint32_t mem = src->sysmem;
    if (!mem && src->container && src->container->sysmem)
        mem = src->container->sysmem[src->face * src->container->levels + src->level];
    uint32_t row = d3d_format_pitch(src->fmt, w), rows = d3d_format_rows(src->fmt, h);
    uint64_t offset = block ? (uint64_t)(r[1] / 4) * src->pitch + (r[0] / 4) * block :
        (uint64_t)r[1] * src->pitch + (uint64_t)r[0] * (src->fmt->bits / 8);
    uint64_t bytes = (uint64_t)(rows - 1) * src->pitch + row;
    if (!mem || row > src->pitch || offset + bytes > UINT32_MAX || (uint64_t)mem + offset + bytes > UINT32_MAX ||
        !void_vm_readable(mem + (uint32_t)offset, (uint32_t)bytes)) return D3DERR_INVALIDCALL;
    surface_upload_data(dst, x, y, w, h, (const uint8_t *)GPTR(mem + (uint32_t)offset), src->pitch);
    return D3D_OK;
}

void d3d_update_texture(d3d_texture *src, d3d_texture *dst)
{
    if (!src->sysmem) { void_log("[d3d9] UpdateTexture from a non-system texture"); return; }
    for (int f = 0; f < src->faces; ++f) {
        for (uint32_t l = 0; l < src->levels && l < dst->levels; ++l) {
            uint32_t mem = src->sysmem[f * (int)src->levels + (int)l];
            if (!mem) continue;
            uint32_t w, h;
            level_dims(src->width, src->height, l, &w, &h);
            d3d_texture_upload_level(dst, (uint32_t)f, l, (const uint8_t *)GPTR(mem), d3d_format_pitch(src->fmt, w));
        }
    }
}

/* ================================================================= buffers */
static void_com_class vbuffer_class, ibuffer_class;

static _Atomic int buffer_subdata = -1, buffer_persistent = -1;
static PFNGLBUFFERSTORAGEEXTPROC buffer_storage;
typedef struct d3d_buffer_storage {
    GLuint gl[3];
    uint8_t *map[3];
    GLsync fence[3];
    unsigned current;
} d3d_buffer_storage;

static void buffer_storage_init(void)
{
    if (atomic_load_explicit(&buffer_persistent, memory_order_relaxed) >= 0) return;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    int powervr = renderer && strstr(renderer, "PowerVR") != NULL;
    atomic_store_explicit(&buffer_subdata, powervr, memory_order_relaxed);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (powervr && extensions && strstr(extensions, "GL_EXT_buffer_storage"))
        buffer_storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
    atomic_store_explicit(&buffer_persistent, buffer_storage != NULL, memory_order_relaxed);
    if (buffer_storage) void_log("[d3d9] persistent PowerVR streaming buffers on");
}

static void buffer_store_delete(d3d_buffer *b, unsigned slot)
{
    d3d_buffer_storage *s = b->storage;
    if (s->fence[slot]) { glDeleteSync(s->fence[slot]); s->fence[slot] = NULL; }
    if (!s->gl[slot]) return;
    d3d_gl_forget_buffer(s->gl[slot]);
    glDeleteBuffers(1, &s->gl[slot]);
    s->gl[slot] = 0; s->map[slot] = NULL;
    --stat_buffers; stat_buffer_bytes -= b->size;
}

static void buffer_storage_free(d3d_buffer *b)
{
    for (unsigned i = 0; i < 3; ++i) buffer_store_delete(b, i);
    free(b->storage); b->storage = NULL; b->gl = 0;
}

static int buffer_store_create(d3d_buffer *b, unsigned slot)
{
    d3d_buffer_storage *s = b->storage;
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    glGenBuffers(1, &s->gl[slot]);
    ++stat_buffers; stat_buffer_bytes += b->size;
    glBindBuffer(GL_COPY_WRITE_BUFFER, s->gl[slot]);
    /* ProcessVertices and ordinary write locks still use synchronized SubData. */
    buffer_storage(GL_COPY_WRITE_BUFFER, b->size, NULL, flags | GL_DYNAMIC_STORAGE_BIT_EXT);
    s->map[slot] = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, b->size, flags);
    if (s->map[slot]) return 1;
    void_log("[d3d9] persistent buffer mapping failed %04X; using uploads", glGetError());
    buffer_storage_free(b);
    return 0;
}

static int buffer_storage_upload(d3d_buffer *b, int whole, int unsynchronized,
    uint32_t lo, uint32_t length, const uint8_t *src)
{
    d3d_buffer_storage *s = b->storage;
    if (whole) {
        unsigned old = s->current, next = (old + 1u) % 3u;
        s->fence[old] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!s->fence[old]) void_log("[d3d9] buffer fence failed %04X; retiring this store", glGetError());
        if (s->gl[next]) {
            GLenum ready = s->fence[next] ? glClientWaitSync(s->fence[next], 0, 0) : GL_WAIT_FAILED;
            if (s->fence[next] && ready == GL_WAIT_FAILED)
                void_log("[d3d9] buffer fence poll failed %04X; retiring this store", glGetError());
            if (ready != GL_ALREADY_SIGNALED && ready != GL_CONDITION_SATISFIED)
                buffer_store_delete(b, next); /* Deletion retires busy storage; never wait or overwrite it. */
            else { glDeleteSync(s->fence[next]); s->fence[next] = NULL; }
        }
        if (!s->gl[next] && !buffer_store_create(b, next)) {
            glGenBuffers(1, &b->gl);
            ++stat_buffers; stat_buffer_bytes += b->size;
            return 0; /* The record owns a complete snapshot for the ordinary upload. */
        }
        s->current = next;
        b->gl = s->gl[next];
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, b->gl);
    if (whole || unsynchronized) {
        memcpy(s->map[s->current] + lo, src, length);
        /* Keep PowerVR's vertex/index caches in step with persistent CPU writes. */
        glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);
    }
    else glBufferSubData(GL_COPY_WRITE_BUFFER, lo, length, src);
    return 1;
}

static void run_buffer_destroy(void *args)
{
    d3d_buffer *b = *(d3d_buffer **)args;
    if (b->storage) buffer_storage_free(b);
    if (b->gl) {
        d3d_gl_forget_buffer(b->gl); glDeleteBuffers(1, &b->gl);
        --stat_buffers; stat_buffer_bytes -= b->size;
    }
    free(b);
}

static void buffer_destroy(void_com_object *o)
{
    d3d_buffer *b = (d3d_buffer *)o;
    if (b->shadow) { void_gfree(b->shadow); b->shadow = 0; }
    d3d_rt_call(run_buffer_destroy, &b, sizeof b);
}

static void run_buffer_create(void *args)
{
    d3d_buffer *b = *(d3d_buffer **)args;
    buffer_storage_init();
    if (buffer_storage && (b->usage & D3DUSAGE_DYNAMIC)) {
        b->storage = (d3d_buffer_storage *)calloc(1, sizeof *b->storage);
        if (b->storage && buffer_store_create(b, 0)) { b->gl = b->storage->gl[0]; return; }
    }
    glGenBuffers(1, &b->gl);
    ++stat_buffers; stat_buffer_bytes += b->size;
    /* Created through the copy target, which no vertex array object records. */
    glBindBuffer(GL_COPY_WRITE_BUFFER, b->gl);
    glBufferData(GL_COPY_WRITE_BUFFER, b->size, NULL, (b->usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

uint32_t d3d_buffer_create(d3d_device *dev, int index, uint32_t length, uint32_t usage, uint32_t format, uint32_t pool)
{
    if (!length) return 0;
    d3d_buffer *b = (d3d_buffer *)calloc(1, sizeof *b);
    b->dev = dev; b->index = index; b->size = length; b->usage = usage; b->format = format; b->pool = pool;
    b->shadow = void_galloc(length);
    if (!b->shadow) { free(b); return 0; }
    d3d_rt_call(run_buffer_create, &b, sizeof b);
    b->dirty_lo = 0xFFFFFFFFu;
    uint32_t guest = void_com_wrap(&b->com, index ? &ibuffer_class : &vbuffer_class, 0);
    b->com.destroy = buffer_destroy;
    return guest;
}

/* The dirty guest range for the GL buffer (on unlock, and before a draw that
 * reads a buffer still being written): copied for the render thread. */
typedef struct { d3d_buffer *b; uint32_t lo, length, whole, unsynchronized; } buffer_upload_args;   /* then the bytes */

static void run_buffer_upload(void *args)
{
    buffer_upload_args *a = (buffer_upload_args *)args;
    d3d_buffer *b = a->b;
    int subdata = atomic_load_explicit(&buffer_subdata, memory_order_relaxed);
    if (subdata < 0) {
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        /* PowerVR mapping submits queued tile work even for NOOVERWRITE ranges. */
        subdata = renderer && strstr(renderer, "PowerVR") != NULL;
        atomic_store_explicit(&buffer_subdata, subdata, memory_order_relaxed);
    }
    /* Upload through the copy target: it belongs to no vertex array object,
     * so neither the bound VAO's element buffer nor its attributes change. */
    GLenum target = GL_COPY_WRITE_BUFFER;
    glBindBuffer(target, b->gl);
    const uint8_t *src = (const uint8_t *)(a + 1);
    uint32_t length = a->length;
    if (b->storage) {
        if (buffer_storage_upload(b, a->whole, a->unsynchronized, a->lo, length, src)) return;
        glBindBuffer(target, b->gl);
    }
    if (a->whole) {
        glBufferData(target, b->size, src, a->whole == 2 ? GL_STREAM_DRAW :
            (b->usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    } else if (a->unsynchronized && !subdata) {
        /* Only D3DLOCK_NOOVERWRITE locks: the game promises not to touch what
         * queued draws read, so write without waiting for the GPU. A plain
         * glBufferSubData into a buffer in flight makes Mali copy the whole
         * buffer or stall (map: CPU-skinned segments appended draw by draw).
         * No GL_MAP_INVALIDATE_RANGE_BIT: with it, Adreno 735 garbles
         * vertices that queued draws of the same render pass still read (the
         * neighborhood chooser's panels fly across the screen); the whole
         * range is written anyway. */
        void *dst = glMapBufferRange(target, a->lo, length, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (dst) {
            memcpy(dst, src, length);
            glUnmapBuffer(target);
        } else {
            glBufferSubData(target, a->lo, length, src);
        }
    } else {
        glBufferSubData(target, a->lo, length, src);
    }
}

/* Bytes the buffers sent (frame statistics): all, and of full uploads the
 * part outside what was written. */
uint64_t d3d_buffer_bytes, d3d_buffer_spare_bytes, d3d_buffer_uploads;

void d3d_buffer_flush(d3d_buffer *b)
{
    if (b->dirty_lo >= b->dirty_hi) return;
    int whole = b->discard_pending || (b->dirty_lo == 0 && b->dirty_hi >= b->size);
    /* Without persistent storage, small PowerVR buffers retain the old
     * bounded orphan path. Snapshot before later guest writes. */
    if (!whole && !b->overwrite_pending && (b->usage & D3DUSAGE_DYNAMIC) &&
        b->size <= (64u << 10) && atomic_load_explicit(&buffer_subdata, memory_order_relaxed) == 1 &&
        atomic_load_explicit(&buffer_persistent, memory_order_relaxed) != 1)
        whole = 2;
    uint32_t lo = whole ? 0 : b->dirty_lo, length = whole ? b->size : b->dirty_hi - b->dirty_lo;
    d3d_buffer_bytes += length;
    ++d3d_buffer_uploads;
    if (whole) d3d_buffer_spare_bytes += b->size - (b->dirty_hi - b->dirty_lo);
    buffer_upload_args *a = (buffer_upload_args *)d3d_rt_begin(run_buffer_upload, (uint32_t)sizeof *a + length);
    a->b = b; a->lo = lo; a->length = length; a->whole = (uint32_t)whole; a->unsynchronized = !b->overwrite_pending;
    memcpy(a + 1, (const uint8_t *)GPTR(b->shadow) + lo, length);
    d3d_rt_commit();
    b->dirty_lo = 0xFFFFFFFFu;
    b->dirty_hi = 0;
    b->discard_pending = 0;
    b->overwrite_pending = 0;
}

#define BUF ((d3d_buffer *)void_com_host(THIS_))
COM_METHOD(buf, QueryInterface, 3)
{
    d3d_buffer *b = BUF;
    if (void_guid_equal(ARG(1), b->index ? IID_IDirect3DIndexBuffer9 : IID_IDirect3DVertexBuffer9) ||
        void_guid_equal(ARG(1), IID_IDirect3DResource9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&b->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(buf, AddRef, 1) { return void_com_addref(&BUF->com); }
COM_METHOD(buf, Release, 1) { return void_com_release(&BUF->com); }
COM_METHOD(buf, GetDevice, 2) { d3d_buffer *b = BUF; void_com_addref(&b->dev->com); MEM32(ARG(1)) = b->dev->com.guest; return D3D_OK; }
COM_METHOD(buf, SetPrivateData, 5) { return D3D_OK; }
COM_METHOD(buf, GetPrivateData, 4) { return D3DERR_NOTFOUND; }
COM_METHOD(buf, FreePrivateData, 2) { return D3D_OK; }
COM_METHOD(buf, SetPriority, 2) { return 0; }
COM_METHOD(buf, GetPriority, 1) { return 0; }
COM_METHOD(buf, PreLoad, 1) { return 0; }
COM_METHOD(buf, GetType, 1) { return BUF->index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER; }

COM_METHOD(buf, Lock, 5)
{
    d3d_buffer *b = BUF;
    uint32_t offset = ARG(1), size = ARG(2), out = ARG(3), flags = ARG(4);
    if (offset > b->size) return D3DERR_INVALIDCALL;
    if (!size || offset + size > b->size) size = b->size - offset;
    if (!(flags & D3DLOCK_READONLY)) {
        if (offset < b->dirty_lo) b->dirty_lo = offset;
        if (offset + size > b->dirty_hi) b->dirty_hi = offset + size;
        if (flags & D3DLOCK_DISCARD) b->discard_pending = 1;
        else if (!(flags & D3DLOCK_NOOVERWRITE)) b->overwrite_pending = 1;
    }
    ++b->locks;
    MEM32(out) = b->shadow + offset;
    return D3D_OK;
}

COM_METHOD(buf, Unlock, 1)
{
    d3d_buffer *b = BUF;
    if (!b->locks) return D3DERR_INVALIDCALL;
    if (--b->locks == 0) d3d_buffer_flush(b);
    return D3D_OK;
}

COM_METHOD(buf, GetDesc, 2)
{
    d3d_buffer *b = BUF;
    uint32_t d = ARG(1);
    MEM32(d) = b->index ? b->format : 100u;          /* D3DFMT_VERTEXDATA */
    MEM32(d + 4) = b->index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER;
    MEM32(d + 8) = b->usage; MEM32(d + 12) = b->pool; MEM32(d + 16) = b->size;
    if (!b->index) MEM32(d + 20) = b->format;          /* FVF */
    return D3D_OK;
}

static const void_fn buffer_methods[] = {
    buf_QueryInterface, buf_AddRef, buf_Release, buf_GetDevice, buf_SetPrivateData, buf_GetPrivateData,
    buf_FreePrivateData, buf_SetPriority, buf_GetPriority, buf_PreLoad, buf_GetType, buf_Lock, buf_Unlock, buf_GetDesc,
};
static void_com_class vbuffer_class = { "IDirect3DVertexBuffer9", 14, buffer_methods, 0 };
static void_com_class ibuffer_class = { "IDirect3DIndexBuffer9", 14, buffer_methods, 0 };

/* ===================================================== vertex declarations */
static void_com_class decl_class;

static void decl_add(d3d_decl *d, uint16_t stream, uint16_t offset, uint8_t type, uint8_t usage, uint8_t index)
{
    if (d->count >= 64) return;
    d3d_vertex_element *e = &d->elements[d->count++];
    e->stream = stream; e->offset = offset; e->type = type; e->method = 0; e->usage = usage; e->usage_index = index;
}

static const uint8_t type_sizes[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0};

static void run_free(void *args) { free(*(void **)args); }

/* An object the render thread may still name: freed after its calls. */
void d3d_free_later(void_com_object *o) { d3d_rt_call(run_free, &o, sizeof o); }

uint32_t d3d_decl_create(d3d_device *dev, uint32_t elements, uint32_t fvf)
{
    d3d_decl *d = (d3d_decl *)calloc(1, sizeof *d);
    d->dev = dev;
    d->fvf = fvf;
    if (elements) {
        for (uint32_t e = elements; MEM16(e) != 0xFF && d->count < 64; e += 8)
            decl_add(d, MEM16(e), MEM16(e + 2), MEM8(e + 4), MEM8(e + 6), MEM8(e + 7));
    } else {
        uint16_t off = 0;
        uint32_t pos = fvf & 0x400E;
        if (pos == 0x002) { decl_add(d, 0, off, 2, 0, 0); off += 12; }
        else if (pos == 0x004) { decl_add(d, 0, off, 3, 9, 0); off += 16; }
        else if (pos == 0x4002) { decl_add(d, 0, off, 3, 0, 0); off += 16; }
        else if (pos >= 0x006 && pos <= 0x00E) {
            int betas = (int)((pos - 0x004) / 2);    /* XYZB1 = 0x006 ... XYZB5 = 0x00E */
            decl_add(d, 0, off, 2, 0, 0); off += 12;
            int indexed = (fvf & 0x1000u) || (fvf & 0x8000u);
            int weights = betas - (indexed ? 1 : 0);
            if (weights > 0) { decl_add(d, 0, off, (uint8_t)(weights - 1), 1, 0); off += (uint16_t)(4 * weights); }
            if (indexed) { decl_add(d, 0, off, (fvf & 0x8000u) ? 4 : 5, 2, 0); off += 4; }
        }
        if (fvf & 0x010) { decl_add(d, 0, off, 2, 3, 0); off += 12; }
        if (fvf & 0x020) { decl_add(d, 0, off, 0, 4, 0); off += 4; }
        if (fvf & 0x040) { decl_add(d, 0, off, 4, 10, 0); off += 4; }
        if (fvf & 0x080) { decl_add(d, 0, off, 4, 10, 1); off += 4; }
        uint32_t texcount = (fvf >> 8) & 15;
        for (uint32_t i = 0; i < texcount; ++i) {
            uint32_t fmt = (fvf >> (16 + 2 * i)) & 3;
            static const uint8_t types[4] = {1, 2, 3, 0};
            static const uint8_t sizes[4] = {8, 12, 16, 4};
            decl_add(d, 0, off, types[fmt], 5, (uint8_t)i);
            off += sizes[fmt];
        }
    }
    uint32_t guest = void_com_wrap(&d->com, &decl_class, 0);
    d->com.destroy = d3d_free_later;     /* draws recorded before its release still read it */
    return guest;
}

#define DECL ((d3d_decl *)void_com_host(THIS_))
COM_METHOD(decl, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IDirect3DVertexDeclaration9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&DECL->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(decl, AddRef, 1) { return void_com_addref(&DECL->com); }
COM_METHOD(decl, Release, 1) { return void_com_release(&DECL->com); }
COM_METHOD(decl, GetDevice, 2) { d3d_decl *d = DECL; void_com_addref(&d->dev->com); MEM32(ARG(1)) = d->dev->com.guest; return D3D_OK; }
COM_METHOD(decl, GetDeclaration, 3)
{
    d3d_decl *d = DECL;
    uint32_t out = ARG(1), count = ARG(2);
    if (out) {
        for (int i = 0; i < d->count; ++i) {
            uint32_t e = out + 8u * (uint32_t)i;
            MEM16(e) = d->elements[i].stream; MEM16(e + 2) = d->elements[i].offset;
            MEM8(e + 4) = d->elements[i].type; MEM8(e + 5) = d->elements[i].method;
            MEM8(e + 6) = d->elements[i].usage; MEM8(e + 7) = d->elements[i].usage_index;
        }
        uint32_t end = out + 8u * (uint32_t)d->count;
        MEM16(end) = 0xFF; MEM16(end + 2) = 0; MEM8(end + 4) = 17; MEM8(end + 5) = 0; MEM8(end + 6) = 0; MEM8(end + 7) = 0;
    }
    MEM32(count) = (uint32_t)d->count + 1;
    return D3D_OK;
}
static const void_fn decl_methods[] = { decl_QueryInterface, decl_AddRef, decl_Release, decl_GetDevice, decl_GetDeclaration };
static void_com_class decl_class = { "IDirect3DVertexDeclaration9", 5, decl_methods, 0 };

/* ================================================================= queries */
static void_com_class query_class;
#define QRY ((d3d_query *)void_com_host(THIS_))

typedef struct {
    d3d_query q;
    uint64_t serial;                        /* game: current issue generation */
    uint64_t rt_serial;                     /* render: generation executed */
    _Atomic uint64_t answer;                /* render publishes generation << 2 | result */
    _Atomic int polling;
} d3d_query_rt;
#define QRY_RT(q) ((d3d_query_rt *)(q))
enum { QUERY_VISIBLE = 1, QUERY_HIDDEN = 2, QUERY_ERROR = 3 };

typedef struct { d3d_query *q; uint64_t serial; } watched;
static watched *watch;
static uint32_t watch_head, watch_count, watch_cap;
static int watch_flushed = 1, query_watch_enabled = -1;
static _Atomic uint64_t poll_answered_idle;
static uint64_t poll_calls[2], poll_pending[2], poll_recorded, poll_sleep_ns, poll_cached;

static void watch_push(d3d_query *q)
{
    if (!query_watch_enabled || !d3d_rt_threaded()) return;
    if (watch_count == watch_cap) {
        if (watch_cap > UINT32_MAX / 2u) return;
        uint32_t cap = watch_cap ? watch_cap * 2u : 1024u;
        watched *grown = (watched *)malloc((size_t)cap * sizeof *grown);
        if (!grown) return;                 /* GetData still records explicit polls. */
        for (uint32_t i = 0; i < watch_count; ++i) grown[i] = watch[(watch_head + i) % watch_cap];
        free(watch);
        watch = grown; watch_cap = cap; watch_head = 0;
    }
    watch[(watch_head + watch_count++) % watch_cap] = (watched){ q, QRY_RT(q)->rt_serial };
    watch_flushed = 0;
}

static void watch_remove(d3d_query *q)
{
    for (uint32_t i = 0; i < watch_count; ++i)
        if (watch[(watch_head + i) % watch_cap].q == q) watch[(watch_head + i) % watch_cap].q = NULL;
}

static int query_answered(d3d_query *q)
{
    uint64_t a = atomic_load_explicit(&QRY_RT(q)->answer, memory_order_relaxed);
    return (a & ~3ull) == (QRY_RT(q)->rt_serial << 2) && (a & 3u);
}

static int query_answer(d3d_query *q)
{
    uint32_t result;
    if (q->type == 8) {
        GLenum status = q->fence ? glClientWaitSync(q->fence, 0, 0) : GL_WAIT_FAILED;
        if (status == GL_TIMEOUT_EXPIRED) return 0;
        result = status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED ? QUERY_VISIBLE : QUERY_ERROR;
    } else if (!q->gl) {
        result = QUERY_ERROR;
    } else {
        GLuint available = 0, any = 0;
        glGetQueryObjectuiv(q->gl, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return 0;
        glGetQueryObjectuiv(q->gl, GL_QUERY_RESULT, &any);
        result = any ? QUERY_VISIBLE : QUERY_HIDDEN;
    }
    atomic_store_explicit(&QRY_RT(q)->answer, (QRY_RT(q)->rt_serial << 2) | result, memory_order_release);
    return 1;
}

static int query_watch(int idle)
{
    while (watch_count) {
        watched *e = &watch[watch_head];
        d3d_query *q = e->q;
        if (q && QRY_RT(q)->rt_serial == e->serial && !query_answered(q)) {
            if (!query_answer(q)) break;
            atomic_fetch_add_explicit(&poll_answered_idle, 1, memory_order_relaxed);
        }
        watch_head = (watch_head + 1u) % watch_cap;
        --watch_count;
    }
    if (idle && watch_count && !watch_flushed) { glFlush(); watch_flushed = 1; }
    return watch_count != 0;
}

static void query_watch_init(void)
{
    if (query_watch_enabled >= 0) return;
    const char *env = getenv("S2_QUERY_WATCH");
    query_watch_enabled = 1;
    if (env && *env) query_watch_enabled = *env != '0';
    else if (void_data_dir() && *void_data_dir()) {
        char path[1100];
        snprintf(path, sizeof path, "%s/query-watch", void_data_dir());
        FILE *f = fopen(path, "r");
        if (f) { query_watch_enabled = fgetc(f) != '0'; fclose(f); }
    }
    if (query_watch_enabled) d3d_rt_set_idle(query_watch);
    void_log("[rt] query watch %s", query_watch_enabled ? "on" : "off (synchronous GetData)");
}

static void run_query_destroy(void *args)
{
    d3d_query *q = *(d3d_query **)args;
    watch_remove(q);
    if (q->gl) glDeleteQueries(1, &q->gl);
    if (q->fence) glDeleteSync(q->fence);
    free(q);
}
static void query_destroy(void_com_object *o) { d3d_query *q = (d3d_query *)o; d3d_rt_call(run_query_destroy, &q, sizeof q); }
static void run_query_create(void *args) { d3d_query *q = *(d3d_query **)args; if (q->type == 9) glGenQueries(1, &q->gl); }

uint32_t d3d_query_create(d3d_device *dev, uint32_t type)
{
    query_watch_init();
    d3d_query *q = (d3d_query *)calloc(1, sizeof(d3d_query_rt));
    if (!q) return 0;
    atomic_init(&QRY_RT(q)->answer, 0);
    atomic_init(&QRY_RT(q)->polling, 0);
    q->dev = dev;
    q->type = type;
    d3d_rt_call(run_query_create, &q, sizeof q);
    uint32_t guest = void_com_wrap(&q->com, &query_class, 0);
    q->com.destroy = query_destroy;
    return guest;
}

COM_METHOD(qry, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IDirect3DQuery9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&QRY->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(qry, AddRef, 1) { return void_com_addref(&QRY->com); }
COM_METHOD(qry, Release, 1) { return void_com_release(&QRY->com); }
COM_METHOD(qry, GetDevice, 2) { d3d_query *q = QRY; void_com_addref(&q->dev->com); MEM32(ARG(1)) = q->dev->com.guest; return D3D_OK; }
COM_METHOD(qry, GetType, 1) { return QRY->type; }
COM_METHOD(qry, GetDataSize, 1) { d3d_query *q = QRY; return q->type == 4 ? 16u : 4u; }

/* Issue generations cross the queue by value; GL objects stay on its consumer. */
typedef struct { d3d_query *q; uint32_t flags; uint64_t serial; } issue_args;
static void run_query_issue(void *args)
{
    issue_args *a = (issue_args *)args;
    d3d_query *q = a->q;
    QRY_RT(q)->rt_serial = a->serial;
    if (q->type == 9) {
        if (a->flags & 2u) glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q->gl);
        if (a->flags & 1u) glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
    } else if (q->type == 8 && (a->flags & 1u)) {
        if (q->fence) glDeleteSync(q->fence);
        q->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    if (a->flags & 1u) watch_push(q);
}

COM_METHOD(qry, Issue, 2)
{
    d3d_query *q = QRY;
    uint32_t flags = ARG(1);
    if (q->type == 9) {
        uint32_t gl = 0;
        if (flags & 2u) { q->issued = 1; q->ended = 0; gl |= 2u; }
        if ((flags & 1u) && q->issued) { q->ended = 1; q->issued = 0; gl |= 1u; }
        if (!gl) return D3D_OK;
        flags = gl;
    } else if (q->type == 8 && (flags & 1u)) {
        q->ended = 1;
    } else {
        return D3D_OK;
    }
    issue_args a = { q, flags, ++QRY_RT(q)->serial };
    d3d_rt_call(run_query_issue, &a, sizeof a);
    return D3D_OK;
}

typedef struct { d3d_query *q; int wait; int ready; GLuint any; } result_args;
static void run_query_result(void *args)
{
    result_args *a = (result_args *)args;
    d3d_query *q = a->q;
    if (q->type == 8) {
        a->ready = -1;
        if (q->fence) {
            /* Brief flush waits avoid the game's repeated synchronous polling round trips. */
            GLenum status = glClientWaitSync(q->fence, a->wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                a->wait ? 1000000ull : 0);
            a->ready = status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED ? 1
                : status == GL_TIMEOUT_EXPIRED ? 0 : -1;
        }
        return;
    }
    GLuint available = 0;
    glGetQueryObjectuiv(q->gl, GL_QUERY_RESULT_AVAILABLE, &available);
    a->ready = available || a->wait;
    if (a->ready) glGetQueryObjectuiv(q->gl, GL_QUERY_RESULT, &a->any);
}

static uint32_t query_getdata_sync(d3d_query *q, uint32_t out, uint32_t size, uint32_t flags)
{
    if (q->type == 4) {
        if (out && size >= 16) { MEM32(out) = FOURCC('C','A','C','H'); MEM32(out + 4) = 1; MEM32(out + 8) = 16; MEM32(out + 12) = 7; }
        return D3D_OK;
    }
    if (q->type == 8) {
        result_args a = { q, (flags & 1u) != 0, 0, 0 };
        if (q->ended) d3d_rt_sync(run_query_result, &a);
        if (a.ready < 0) return D3DERR_NOTAVAILABLE;
        if (q->ended && !a.ready) return S_FALSE_D3D;
        if (out && size >= 4) MEM32(out) = 1;
        return D3D_OK;
    }
    if (!q->ended) { if (out && size >= 4) MEM32(out) = 0; return D3D_OK; }
    result_args a = { q, (flags & 1u) != 0, 0, 0 };
    d3d_rt_sync(run_query_result, &a);
    if (!a.ready) return S_FALSE_D3D;
    /* GLES reports only "any samples": answer with the viewport's pixel count. */
    if (out && size >= 4) MEM32(out) = a.any ? q->dev->st.viewport[2] * q->dev->st.viewport[3] : 0;
    return D3D_OK;
}

typedef struct { d3d_query *q; uint32_t flush; } poll_args;
static void run_query_poll(void *args)
{
    const poll_args *a = (const poll_args *)args;
    if (a->flush) { glFlush(); watch_flushed = 1; }
    if (!query_answered(a->q)) query_answer(a->q);
    atomic_store_explicit(&QRY_RT(a->q)->polling, 0, memory_order_release);
}

static int query_result(d3d_query *q, uint32_t flags)
{
    d3d_query_rt *x = QRY_RT(q);
    uint64_t a = atomic_load_explicit(&x->answer, memory_order_acquire);
    if ((a & ~3ull) == (x->serial << 2) && (a & 3u)) { ++poll_cached; return (int)(a & 3u); }
    if (!d3d_rt_threaded() || !atomic_exchange_explicit(&x->polling, 1, memory_order_acq_rel)) {
        poll_args p = { q, flags & 1u };
        ++poll_recorded;
        d3d_rt_call(run_query_poll, &p, sizeof p);
        d3d_rt_flush();
        a = atomic_load_explicit(&x->answer, memory_order_acquire);
        if ((a & ~3ull) == (x->serial << 2) && (a & 3u)) return (int)(a & 3u);
    }
    return 0;
}

static _Thread_local unsigned query_pending_spins;
static uint32_t query_pending_result(uint32_t flags)
{
    if (!(flags & 1u)) { query_pending_spins = 0; return S_FALSE_D3D; }
    if (query_pending_spins < 8) { ++query_pending_spins; return S_FALSE_D3D; }
    struct timespec delay = { 0, 50000 };
    uint64_t start = void_monotonic_ns();
    void_blocking_begin();
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    void_blocking_end();
    poll_sleep_ns += void_monotonic_ns() - start;
    return S_FALSE_D3D;
}

COM_METHOD(qry, GetData, 4)
{
    d3d_query *q = QRY;
    uint32_t out = ARG(1), size = ARG(2), flags = ARG(3);
    if (q->type == 4) { query_pending_spins = 0; return query_getdata_sync(q, out, size, flags); }
    int kind = q->type == 8;
    ++poll_calls[kind];
    if (!query_watch_enabled) {
        uint32_t r = query_getdata_sync(q, out, size, flags);
        if (r == S_FALSE_D3D) ++poll_pending[kind];
        return r;
    }
    int result = q->ended ? query_result(q, flags) : (q->type == 8 ? QUERY_VISIBLE : QUERY_HIDDEN);
    if (!result) { ++poll_pending[kind]; return query_pending_result(flags); }
    query_pending_spins = 0;
    if (result == QUERY_ERROR) return D3DERR_NOTAVAILABLE;
    if (out && size >= 4) MEM32(out) = q->type == 8 ? 1u : result == QUERY_VISIBLE
        ? q->dev->st.viewport[2] * q->dev->st.viewport[3] : 0;
    return D3D_OK;
}

void d3d_query_log(uint32_t frames)
{
    if (!frames || query_watch_enabled < 0) return;
    static uint64_t last[7];
    uint64_t now[7] = { poll_calls[0], poll_calls[1], poll_pending[0] + poll_pending[1], poll_recorded,
        poll_cached, atomic_load_explicit(&poll_answered_idle, memory_order_relaxed), poll_sleep_ns };
    void_log("[rt] queries per frame: %.2f occlusion, %.2f event, %.2f pending; %.2f handed over, %.2f cached, "
             "%.2f answered unasked; %.3f ms asleep", (double)(now[0] - last[0]) / frames,
             (double)(now[1] - last[1]) / frames, (double)(now[2] - last[2]) / frames,
             (double)(now[3] - last[3]) / frames, (double)(now[4] - last[4]) / frames,
             (double)(now[5] - last[5]) / frames, (double)(now[6] - last[6]) / 1e6 / frames);
    memcpy(last, now, sizeof last);
}

static const void_fn query_methods[] = { qry_QueryInterface, qry_AddRef, qry_Release, qry_GetDevice, qry_GetType,
    qry_GetDataSize, qry_Issue, qry_GetData };
static void_com_class query_class = { "IDirect3DQuery9", 8, query_methods, 0 };

/* ============================================================= state blocks */
static void_com_class stateblock_class;
#define SB ((d3d_stateblock *)void_com_host(THIS_))

void d3d_stateblock_apply(d3d_stateblock *sb);

/* Pixel and vertex state membership, from the D3D9 documentation. */
static const uint8_t pixel_rs[] = {7,9,14,15,16,19,20,23,24,25,26,27,34,35,36,37,38,48,52,53,54,55,56,57,58,59,60,
    128,129,130,131,132,133,134,135,168,170,171,174,175,176,185,186,187,188,189,190,191,192,193,194,195,198,199,200,201,
    202,203,204,205,206,207,208,209};
static const uint8_t vertex_rs[] = {9,28,29,35,36,37,38,48,136,137,139,140,141,142,143,145,146,147,148,151,152,154,155,
    156,157,158,159,160,161,162,163,166,167,170,172,173,178,179,180,181,182,183,184,185,186,187,188,189};

static void stateblock_destroy(void_com_object *o)
{
    d3d_stateblock *sb = (d3d_stateblock *)o;
    d3d_state *s = &sb->state;
    for (int i = 0; i < 20; ++i) d3d_ref_set(&s->textures[i], NULL);
    for (int i = 0; i < 16; ++i) d3d_ref_set(&s->streams[i], NULL);
    d3d_ref_set(&s->indices, NULL);
    d3d_ref_set(&s->decl, NULL);
    d3d_ref_set(&s->vs, NULL);
    d3d_ref_set(&s->ps, NULL);
    free(sb);
}

uint32_t d3d_stateblock_create(d3d_device *dev, uint32_t type)
{
    d3d_stateblock *sb = (d3d_stateblock *)calloc(1, sizeof *sb);
    sb->dev = dev;
    if (type == 1 || type == 2) {                  /* ALL, PIXELSTATE */
        if (type == 1) memset(sb->rec_rs, 1, sizeof sb->rec_rs);
        else for (size_t i = 0; i < sizeof pixel_rs; ++i) sb->rec_rs[pixel_rs[i]] = 1;
        for (int i = 0; i < 20; ++i) for (int k = 0; k < 14; ++k) sb->rec_ss[i][k] = 1;
        for (int i = 0; i < 8; ++i) for (int k = 0; k < 33; ++k) sb->rec_tss[i][k] = 1;
        memset(sb->rec_ps_f, 1, sizeof sb->rec_ps_f);
        memset(sb->rec_ps_i, 1, sizeof sb->rec_ps_i);
        memset(sb->rec_ps_b, 1, sizeof sb->rec_ps_b);
        sb->rec_ps = 1;
    }
    if (type == 1 || type == 3) {                  /* ALL, VERTEXSTATE */
        if (type == 3) for (size_t i = 0; i < sizeof vertex_rs; ++i) sb->rec_rs[vertex_rs[i]] = 1;
        memset(sb->rec_vs_f, 1, sizeof sb->rec_vs_f);
        memset(sb->rec_vs_i, 1, sizeof sb->rec_vs_i);
        memset(sb->rec_vs_b, 1, sizeof sb->rec_vs_b);
        memset(sb->rec_light, 1, sizeof sb->rec_light);
        sb->rec_vs = 1; sb->rec_decl = 1; sb->rec_fvf = 1;
        for (int i = 16; i < 20; ++i) for (int k = 0; k < 14; ++k) sb->rec_ss[i][k] = 1;
    }
    if (type == 1) {
        memset(sb->rec_tex, 1, sizeof sb->rec_tex);
        memset(sb->rec_stream, 1, sizeof sb->rec_stream);
        memset(sb->rec_transform, 1, sizeof sb->rec_transform);
        memset(sb->rec_clip, 1, sizeof sb->rec_clip);
        sb->rec_viewport = sb->rec_scissor = sb->rec_indices = sb->rec_material = 1;
    }
    uint32_t guest = void_com_wrap(&sb->com, &stateblock_class, 0);
    sb->com.destroy = stateblock_destroy;
    if (type) d3d_stateblock_capture(sb);
    return guest;
}

void d3d_stateblock_capture(d3d_stateblock *sb)
{
    /* Capture copies the recorded categories. The block holds references on
     * the objects it names, as D3D9 state blocks do (Wine increments them in
     * wined3d_stateblock_capture): a texture released after capture stays
     * valid for Apply. */
    d3d_state *src = &sb->dev->st, *s = &sb->state;
    for (int i = 0; i < 256; ++i) if (sb->rec_rs[i]) s->rs[i] = src->rs[i];
    for (int i = 0; i < 20; ++i) for (int k = 0; k < 14; ++k) if (sb->rec_ss[i][k]) s->ss[i][k] = src->ss[i][k];
    for (int i = 0; i < 8; ++i) for (int k = 0; k < 33; ++k) if (sb->rec_tss[i][k]) s->tss[i][k] = src->tss[i][k];
    for (int i = 0; i < 20; ++i) if (sb->rec_tex[i]) d3d_ref_set(&s->textures[i], src->textures[i]);
    for (int i = 0; i < 16; ++i) if (sb->rec_stream[i]) {
        d3d_ref_set(&s->streams[i], src->streams[i]); s->stream_offset[i] = src->stream_offset[i];
        s->stream_stride[i] = src->stream_stride[i]; s->stream_freq[i] = src->stream_freq[i];
    }
    for (uint32_t i = 0; i < D3D_SOFTWARE_VS_CONSTANTS; ++i) if (sb->rec_vs_f[i]) memcpy(s->vs_f[i], src->vs_f[i], 16);
    for (int i = 0; i < 224; ++i) if (sb->rec_ps_f[i]) memcpy(s->ps_f[i], src->ps_f[i], 16);
    for (uint32_t i = 0; i < D3D_SOFTWARE_VS_INT_CONSTANTS; ++i) if (sb->rec_vs_i[i]) memcpy(s->vs_i[i], src->vs_i[i], 16);
    for (uint32_t i = 0; i < D3D_SOFTWARE_VS_BOOL_CONSTANTS; ++i) if (sb->rec_vs_b[i]) s->vs_b[i] = src->vs_b[i];
    for (int i = 0; i < 16; ++i) {
        if (sb->rec_ps_i[i]) memcpy(s->ps_i[i], src->ps_i[i], 16);
        if (sb->rec_ps_b[i]) s->ps_b[i] = src->ps_b[i];
    }
    for (int i = 0; i < 512; ++i) if (sb->rec_transform[i]) memcpy(s->transforms[i], src->transforms[i], 64);
    if (sb->rec_viewport) { memcpy(s->viewport, src->viewport, 16); memcpy(s->viewport_z, src->viewport_z, 8); }
    if (sb->rec_scissor) memcpy(s->scissor, src->scissor, 16);
    if (sb->rec_indices) d3d_ref_set(&s->indices, src->indices);
    if (sb->rec_decl) d3d_ref_set(&s->decl, src->decl);
    if (sb->rec_fvf) s->fvf = src->fvf;
    if (sb->rec_vs) d3d_ref_set(&s->vs, src->vs);
    if (sb->rec_ps) d3d_ref_set(&s->ps, src->ps);
    if (sb->rec_material) memcpy(s->material, src->material, 68);
    for (int i = 0; i < 8; ++i) if (sb->rec_light[i]) { memcpy(s->lights[i], src->lights[i], 104); s->light_enable[i] = src->light_enable[i]; }
    for (int i = 0; i < 6; ++i) if (sb->rec_clip[i]) memcpy(s->clip_planes[i], src->clip_planes[i], 16);
}

COM_METHOD(sb, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IDirect3DStateBlock9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&SB->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(sb, AddRef, 1) { return void_com_addref(&SB->com); }
COM_METHOD(sb, Release, 1) { return void_com_release(&SB->com); }
COM_METHOD(sb, GetDevice, 2) { d3d_stateblock *s = SB; void_com_addref(&s->dev->com); MEM32(ARG(1)) = s->dev->com.guest; return D3D_OK; }
COM_METHOD(sb, Capture, 1) { d3d_stateblock_capture(SB); return D3D_OK; }
COM_METHOD(sb, Apply, 1) { d3d_stateblock_apply(SB); return D3D_OK; }
static const void_fn stateblock_methods[] = { sb_QueryInterface, sb_AddRef, sb_Release, sb_GetDevice, sb_Capture, sb_Apply };
static void_com_class stateblock_class = { "IDirect3DStateBlock9", 6, stateblock_methods, 0 };

/* ============================================================== swap chain */
typedef struct { void_com_object com; d3d_device *dev; } swapchain_t;
#define SC ((swapchain_t *)void_com_host(THIS_))
COM_METHOD(sc, QueryInterface, 3)
{
    if (void_guid_equal(ARG(1), IID_IDirect3DSwapChain9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&SC->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(sc, AddRef, 1) { return void_com_addref(&SC->com); }
COM_METHOD(sc, Release, 1) { return void_com_release(&SC->com); }
COM_METHOD(sc, Present, 6) { d3d_present(SC->dev); return D3D_OK; }
COM_METHOD(sc, GetFrontBufferData, 2) { return D3DERR_INVALIDCALL; }
COM_METHOD(sc, GetBackBuffer, 4)
{
    swapchain_t *s = SC;
    void_com_addref(&s->dev->backbuffer->com);
    MEM32(ARG(3)) = s->dev->backbuffer->com.guest;
    return D3D_OK;
}
COM_METHOD(sc, GetRasterStatus, 2) { MEM32(ARG(1)) = 0; MEM32(ARG(1) + 4) = 0; return D3D_OK; }
COM_METHOD(sc, GetDisplayMode, 2)
{
    swapchain_t *s = SC;
    uint32_t out = ARG(1);
    MEM32(out) = s->dev->pp[0]; MEM32(out + 4) = s->dev->pp[1]; MEM32(out + 8) = 60; MEM32(out + 12) = 22;
    return D3D_OK;
}
COM_METHOD(sc, GetDevice, 2) { swapchain_t *s = SC; void_com_addref(&s->dev->com); MEM32(ARG(1)) = s->dev->com.guest; return D3D_OK; }
COM_METHOD(sc, GetPresentParameters, 2)
{
    swapchain_t *s = SC;
    for (int i = 0; i < 14; ++i) MEM32(ARG(1) + 4u * (uint32_t)i) = s->dev->pp[i];
    return D3D_OK;
}
static const void_fn swapchain_methods[] = { sc_QueryInterface, sc_AddRef, sc_Release, sc_Present, sc_GetFrontBufferData,
    sc_GetBackBuffer, sc_GetRasterStatus, sc_GetDisplayMode, sc_GetDevice, sc_GetPresentParameters };
static void_com_class swapchain_class = { "IDirect3DSwapChain9", 10, swapchain_methods, 0 };

uint32_t d3d_swapchain_create(d3d_device *dev)
{
    swapchain_t *s = (swapchain_t *)calloc(1, sizeof *s);
    s->dev = dev;
    return void_com_wrap(&s->com, &swapchain_class, 0);
}
