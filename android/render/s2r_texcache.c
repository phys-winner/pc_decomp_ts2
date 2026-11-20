/* The launcher's converted textures in the native renderers (d3d9_texcache.h,
 * android/texconv/s2tc.h).
 *
 * Where the backend does not sample DXT itself (no BC: Mali, and OpenGL ES on
 * Adreno), the front end's DXT levels were decoded to RGBA8 on the CPU at
 * upload, 4-8 times the memory of the blocks. With a conversion from the
 * launcher's texture card the first level the game uploads into a DXT texture
 * decides, as in the Direct3D 9 -> GLES layer (d3d_texcache_upload): found in
 * the cache, the texture's image is created again in the cache's format and
 * every level goes up as converted blocks; not found (a texture the game makes
 * itself, custom content), the texture keeps the RGBA8 decode. A later level
 * or rect the cache lacks is encoded here with libs2texconv.so's encoder.
 *
 * One difference from the old layer: an ETC2 cache's opaque DXT1 texture (its
 * first level stored as ETC2 RGB) keeps ETC2 RGB storage, 8 bytes a block as
 * DXT1 itself, instead of ETC2 RGBA8 with an opaque alpha half added to every
 * block. DXT3/5 textures and every texture of an ASTC cache (ASTC 4x4: 16
 * bytes a block) store their alpha. */
#include "s2r_internal.h"
#include "../host/d3d9_texcache.h"

#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif

/* S2TC_ENC_* of a converted texture's storage. */
static int encoding_of(GLenum storage)
{
    switch (storage) {
    case GL_COMPRESSED_RGB8_ETC2: return S2TC_ENC_ETC2_RGB;
    case GL_COMPRESSED_RGBA8_ETC2_EAC: return S2TC_ENC_ETC2_RGBA;
    case GL_COMPRESSED_RGBA_ASTC_4x4_KHR: return S2TC_ENC_ASTC_4x4;
    default: return 0;
    }
}

static void put_blocks(s2r_image *img, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *blocks,
    size_t bytes)
{
    uint32_t pitch = s2r_gpu_pitch(img, w);
    s2r_dev->be->image_upload(s2r_dev, img, 0, level, x, y, w, h, 1, blocks, pitch, pitch * s2r_gpu_rows(img, h));
    s2r_dev->upload_bytes += bytes;
    s2r_dev->upload_texture += bytes;
}

int s2r_texcache_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    d3d_texcache_stats *c = &d3d_texcache_counts;
    s2r_image *img = s2r_texture_image(t);
    int source = d3d_texcache_source(t->fmt);
    if (!img || !source || img->compressed_dxt || img->kind != S2R_TEX_2D || face || img->render_target || img->autogen ||
        t->screen_sized || t->cache_state == D3D_TEXCACHE_RGBA)
        return 0;
    int format = d3d_texcache_open(s2r_etc2_supported(), s2r_astc_supported());
    if (!format) {
        t->cache_state = D3D_TEXCACHE_RGBA;
        return 0;
    }
    uint32_t lw, lh;
    d3d_texture_level_size(t, level, &lw, &lh);
    int full = !x && !y && w == lw && h == lh && pitch == d3d_format_pitch(t->fmt, w);
    const s2tc_entry *e = full ? d3d_texcache_find(source, data, lw, lh) : NULL;
    if (t->cache_state == D3D_TEXCACHE_UNDECIDED) {
        if (!e) {
            t->cache_state = D3D_TEXCACHE_RGBA;
            ++c->rgba_textures;
            ++c->misses;
            d3d_texcache_report();
            return 0;
        }
        GLenum storage = format == S2TC_FORMAT_ASTC ? GL_COMPRESSED_RGBA_ASTC_4x4_KHR :
            source == S2TC_SRC_DXT1 && e->encoding == S2TC_ENC_ETC2_RGB ? GL_COMPRESSED_RGB8_ETC2 : GL_COMPRESSED_RGBA8_ETC2_EAC;
        /* Nothing is in the RGBA8 image yet: it is created again in the converted format. */
        t->cache_state = D3D_TEXCACHE_COMPRESSED;
        t->gl_internal = storage;
        s2r_texture_storage(t);
        img = s2r_texture_image(t);
        if (!img || !img->native_internal) {
            static int logged;
            if (logged++ < 4) S2R_LOG("texture %ux%u levels %u: no %s storage, decoded to RGBA8", t->width, t->height, t->levels,
                format == S2TC_FORMAT_ASTC ? "ASTC" : "ETC2");
            t->cache_state = D3D_TEXCACHE_RGBA;
            t->gl_internal = 0;
            s2r_texture_storage(t);
            ++c->rgba_textures;
            return 0;
        }
        ++c->compressed_textures;
        if (storage == GL_COMPRESSED_RGB8_ETC2) ++c->rgb_textures;
    }
    if (level >= img->levels) return 1;
    int encoding = encoding_of(t->gl_internal);
    size_t bytes = 0;
    const uint8_t *blocks = e ? d3d_texcache_blocks(e, encoding, &bytes) : NULL;
    if (blocks) {
        put_blocks(img, level, 0, 0, lw, lh, blocks, bytes);
        ++c->hits;
        c->hit_bytes += bytes;
    } else {
        /* a level or rect of a converted texture the cache lacks */
        ++c->misses;
        blocks = d3d_texcache_encode(source, encoding, data, w, h, pitch, &bytes);
        if (blocks) put_blocks(img, level, x, y, w, h, blocks, bytes);
    }
    d3d_texcache_report();
    return 1;
}
