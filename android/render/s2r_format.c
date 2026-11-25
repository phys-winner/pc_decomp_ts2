/* Direct3D formats as the native backends store them.
 *
 * The table of android/host/d3d9_format.c stays the one truth about a D3D
 * format: its upload conversion (d3d_format_convert: DXT and ATI blocks
 * decoded, 16-bit layouts rotated, BGRA swapped for render targets), its
 * storage and its sampling swizzle (d3d_storage_swizzle). This file maps the
 * storage to a backend-neutral format (s2r_gpu_format) and the swizzle to
 * S2R_SW_*. Two exceptions keep blocks compressed: DXT1/3/5 where the backend
 * samples BC (Vulkan textureCompressionBC, GLES S3TC), and the launcher's
 * converted textures (ETC2/ASTC blocks of port_textures, s2r_texcache.c). */
#include "s2r_internal.h"

#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#endif

static int native_dxt, native_etc2 = 1, native_astc;   /* native_dxt: S2R_DXT* bits */

static int dxt_bit(int conv) { return conv == CONV_DXT1 ? S2R_DXT1 : conv == CONV_DXT3 ? S2R_DXT3 : conv == CONV_DXT5 ? S2R_DXT5 : 0; }

static uint8_t gpu_of_internal(GLenum internal)
{
    switch (internal) {
    case GL_RGBA8: case GL_RGB8: return S2R_GPU_RGBA8;      /* R8G8B8 widens to four bytes on upload */
    case GL_RG8: return S2R_GPU_RG8;
    case GL_R8: return S2R_GPU_R8;
    case GL_RG8_SNORM: return S2R_GPU_RG8_SNORM;
    case GL_RGBA8_SNORM: return S2R_GPU_RGBA8_SNORM;
    case GL_RGB565: return S2R_GPU_RGB565;
    case GL_RGBA4: return S2R_GPU_RGBA4;
    case GL_RGB5_A1: return S2R_GPU_RGB5A1;
    case GL_RGB10_A2: return S2R_GPU_RGB10A2;
    case GL_R16F: return S2R_GPU_R16F;
    case GL_RG16F: return S2R_GPU_RG16F;
    case GL_RGBA16F: return S2R_GPU_RGBA16F;
    case GL_R32F: return S2R_GPU_R32F;
    case GL_RG32F: return S2R_GPU_RG32F;
    case GL_RGBA32F: return S2R_GPU_RGBA32F;
    case GL_DEPTH_COMPONENT16: return S2R_GPU_D16;
    case GL_DEPTH24_STENCIL8: return S2R_GPU_D24S8;
    case GL_DEPTH_COMPONENT32F: return S2R_GPU_D32F;
    case GL_DEPTH_COMPONENT24: return S2R_GPU_D24X8;
    case GL_COMPRESSED_RGB8_ETC2: return S2R_GPU_ETC2_RGB;
    case GL_COMPRESSED_RGBA8_ETC2_EAC: return S2R_GPU_ETC2_RGBA;
    case GL_COMPRESSED_RGBA_ASTC_4x4_KHR: return S2R_GPU_ASTC_4x4;
    case GL_COMPRESSED_RGBA_ASTC_5x5_KHR: return S2R_GPU_ASTC_5x5;
    case GL_COMPRESSED_RGBA_ASTC_6x6_KHR: return S2R_GPU_ASTC_6x6;
    default: return S2R_GPU_NONE;
    }
}

static uint8_t sw_of(GLint gl)
{
    switch (gl) {
    case GL_RED: return S2R_SW_R;
    case GL_GREEN: return S2R_SW_G;
    case GL_BLUE: return S2R_SW_B;
    case GL_ALPHA: return S2R_SW_A;
    case GL_ZERO: return S2R_SW_ZERO;
    default: return S2R_SW_ONE;
    }
}

uint32_t s2r_mip_levels(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t n = 1;
    while (w > 1 || h > 1 || d > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; d = d > 1 ? d / 2 : 1; ++n; }
    return n;
}

void s2r_level_size(const s2r_image *img, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *d)
{
    uint32_t lw = img->width >> level, lh = img->height >> level, ld = img->depth >> level;
    *w = lw ? lw : 1;
    *h = lh ? lh : 1;
    if (d) *d = img->kind == S2R_TEX_3D ? (ld ? ld : 1) : 1;
}

int s2r_gpu_block(uint8_t gpu, uint32_t *bw, uint32_t *bh, uint32_t *bytes)
{
    switch (gpu) {
    case S2R_GPU_ETC2_RGB: case S2R_GPU_BC1: *bw = *bh = 4; *bytes = 8; return 1;
    case S2R_GPU_ETC2_RGBA: case S2R_GPU_ASTC_4x4: case S2R_GPU_BC2: case S2R_GPU_BC3: *bw = *bh = 4; *bytes = 16; return 1;
    case S2R_GPU_ASTC_5x5: *bw = *bh = 5; *bytes = 16; return 1;
    case S2R_GPU_ASTC_6x6: *bw = *bh = 6; *bytes = 16; return 1;
    default: *bw = *bh = 1; *bytes = 0; return 0;
    }
}

uint32_t s2r_gpu_texel_bytes(uint8_t gpu)
{
    switch (gpu) {
    case S2R_GPU_R8: return 1;
    case S2R_GPU_RG8: case S2R_GPU_RG8_SNORM: case S2R_GPU_RGB565: case S2R_GPU_RGBA4: case S2R_GPU_RGB5A1:
    case S2R_GPU_R16F: case S2R_GPU_D16: return 2;
    case S2R_GPU_RG16F: case S2R_GPU_R32F: case S2R_GPU_RGBA8: case S2R_GPU_RGBA8_SNORM: case S2R_GPU_RGB10A2:
    case S2R_GPU_D24S8: case S2R_GPU_D32F: case S2R_GPU_D24X8: return 4;
    case S2R_GPU_RGBA16F: case S2R_GPU_RG32F: return 8;
    case S2R_GPU_RGBA32F: return 16;
    default: return 4;
    }
}

uint32_t s2r_gpu_pitch(const s2r_image *img, uint32_t w)
{
    uint32_t bw, bh, bytes;
    if (s2r_gpu_block(img->gpu, &bw, &bh, &bytes)) return (w + bw - 1) / bw * bytes;
    return w * s2r_gpu_texel_bytes(img->gpu);
}

uint32_t s2r_gpu_rows(const s2r_image *img, uint32_t h)
{
    uint32_t bw, bh, bytes;
    if (s2r_gpu_block(img->gpu, &bw, &bh, &bytes)) return (h + bh - 1) / bh;
    return h;
}

const char *s2r_gpu_name(uint8_t gpu)
{
    static const char *const names[S2R_GPU_COUNT] = { "none", "RGBA8", "RG8", "R8", "RG8_SNORM", "RGBA8_SNORM", "RGB565",
        "RGBA4", "RGB5A1", "RGB10A2", "R16F", "RG16F", "RGBA16F", "R32F", "RG32F", "RGBA32F", "D16", "D24S8", "D32F",
        "D24X8", "ETC2_RGB", "ETC2_RGBA", "ASTC4x4", "ASTC5x5", "ASTC6x6", "BC1", "BC2", "BC3" };
    return gpu < S2R_GPU_COUNT ? names[gpu] : "?";
}

/* Fills img's format facts; 0 when the format has no storage here. */
int s2r_image_describe(s2r_image *img, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t d3dfmt,
    uint32_t usage, uint32_t native_internal)
{
    const d3d_format *f = d3d_format_find(d3dfmt);
    memset(img, 0, sizeof *img);
    if (!f || !w || !h) return 0;
    img->kind = kind;
    img->width = w; img->height = h; img->depth = kind == S2R_TEX_3D ? (d ? d : 1) : 1;
    img->levels = levels ? levels : 1;
    img->layers = kind == S2R_TEX_CUBE ? 6 : 1;
    img->d3dfmt = d3dfmt;
    img->fmt = f;
    img->native_internal = native_internal;
    img->depth_stencil = f->depth;
    img->has_stencil = f->stencil;
    img->render_target = (usage & D3DUSAGE_RENDERTARGET) != 0 && !f->depth;
    img->autogen = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
    if (native_internal) {
        img->gpu = gpu_of_internal(native_internal);
        img->swizzle[0] = S2R_SW_R; img->swizzle[1] = S2R_SW_G; img->swizzle[2] = S2R_SW_B; img->swizzle[3] = S2R_SW_A;
    } else if ((native_dxt & dxt_bit(f->conv)) && !img->render_target) {
        /* DXT blocks as they are where the device samples that format (a format it lacks goes through the
           launcher's cache or the decode: s2r_texcache.c); the decoded path's swizzle is the identity too */
        img->gpu = f->conv == CONV_DXT1 ? S2R_GPU_BC1 : f->conv == CONV_DXT3 ? S2R_GPU_BC2 : S2R_GPU_BC3;
        img->compressed_dxt = 1;
        img->swizzle[0] = S2R_SW_R; img->swizzle[1] = S2R_SW_G; img->swizzle[2] = S2R_SW_B; img->swizzle[3] = S2R_SW_A;
    } else {
        img->gpu = gpu_of_internal(f->internal);
        GLint sw[4];
        d3d_storage_swizzle(f, usage | (img->render_target ? D3DUSAGE_RENDERTARGET : 0), sw);
        for (int k = 0; k < 4; ++k) img->swizzle[k] = sw_of(sw[k]);
    }
    if (img->gpu == S2R_GPU_NONE) return 0;
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < img->levels; ++l) {
        uint32_t lw, lh, ld;
        s2r_level_size(img, l, &lw, &lh, &ld);
        bytes += (uint64_t)s2r_gpu_pitch(img, lw) * s2r_gpu_rows(img, lh) * ld * img->layers;
    }
    img->bytes = bytes;
    return 1;
}

/* The backends report what they sample natively when they start. */
void s2r_set_native_support(int dxt, int etc2, int astc)
{
    native_dxt = dxt & S2R_DXT_ALL;
    native_etc2 = etc2;
    native_astc = astc;
}

int s2r_bc_supported(void) { return native_dxt; }
const char *s2r_dxt_names(int mask)
{
    static const char *const names[8] = { "no", "DXT1 only", "DXT3 only", "DXT1+DXT3 only", "DXT5 only", "DXT1+DXT5 only",
        "DXT3+DXT5 only", "yes" };
    return names[mask & S2R_DXT_ALL];
}
int s2r_etc2_supported(void) { return native_etc2; }
int s2r_astc_supported(void) { return native_astc; }
