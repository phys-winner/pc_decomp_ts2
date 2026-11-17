/* Pre-converted DXT textures (android/texconv/s2tc.h): the launcher's texture
 * card re-encodes every DXT level of the installed game into ETC2 or ASTC 4x4
 * once (libs2texconv.so) and files it by a hash of the original blocks in
 * <game>/port_textures/textures.s2tc. Here, on the render thread, a DXT
 * level the game uploads is looked up by that hash; the first level of a
 * texture decides its GL storage: found, the texture is recreated in the
 * cache's compressed format and every level goes up as converted blocks;
 * not found (a texture the game generates itself, custom content), it keeps
 * the RGBA8 storage and the CPU decode of d3d9_format.c. A later level of a
 * compressed texture missing from the cache is encoded here with the
 * converter's own encoder (dlopen). S2_TEXCACHE=0 or <data>/notexcache turns
 * the cache off.
 *
 * A DXT texture the cache does not hold stays compressed too where the GLES
 * driver samples ATC (Adreno: no S3TC, but AMD's ATC, see d3d9_atc.c): its
 * first level decides ATC storage (DXT1 with transparent texels takes the
 * alpha form) and every level goes up transcoded block by block, 4-8 times
 * smaller than the RGBA8 decode. S2_ATC=0 or <data>/noatc keeps the decode. */
#define _GNU_SOURCE
#include "d3d9_internal.h"
#include "d3d9_atc.h"
#include "../texconv/s2tc.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif

enum { CACHE_UNDECIDED = 0, CACHE_RGBA = 1, CACHE_COMPRESSED = 2, CACHE_ATC = 3 };

static int state;                       /* 0 not opened, 1 open, -1 none */
static int fd = -1;
static s2tc_header header;
static s2tc_entry *entries;             /* the index, read once */
static GLenum storage;
static uint8_t *scratch;
static size_t scratch_cap;
static uint64_t hits, misses, fallbacks, compressed_textures, rgba_textures, hit_bytes;
static int atc_state;                   /* 0 untried, 1 on, -1 off */
static uint64_t atc_textures, atc_levels, atc_bytes, atc_ns, atc_lost_alpha;
static uint64_t last_report;

typedef int (*encode_fn)(int, const uint8_t *, uint32_t, uint32_t, uint8_t *);
typedef void (*decode_fn)(int, const uint8_t *, uint32_t, uint32_t, uint8_t *);
static encode_fn encoder;
static decode_fn decoder;

static int source_of(const d3d_format *f)
{
    switch (f->conv) {
    case CONV_DXT1: return S2TC_SRC_DXT1;
    case CONV_DXT3: return S2TC_SRC_DXT3;
    case CONV_DXT5: return S2TC_SRC_DXT5;
    default: return 0;
    }
}

static void open_cache(void)
{
    state = -1;
    const char *env = getenv("S2_TEXCACHE");
    char path[1200];
    snprintf(path, sizeof path, "%s/notexcache", void_data_dir());
    if ((env && *env == '0') || !access(path, F_OK)) { void_log("[texcache] off"); return; }
    snprintf(path, sizeof path, "%s/%s/%s", void_root_dir(), S2TC_DIR, S2TC_FILE);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (pread(fd, &header, sizeof header, 0) != (ssize_t)sizeof header || header.magic != S2TC_MAGIC ||
        header.version != S2TC_VERSION || !(header.flags & 1) || !header.entry_count ||
        header.entry_count > (1u << 26) || header.index_offset + header.entry_count * sizeof(s2tc_entry) > header.file_size) {
        void_log("[texcache] %s: not a complete cache", path);
        close(fd);
        fd = -1;
        return;
    }
    if (header.format == S2TC_FORMAT_ASTC) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        if (!ext || !strstr(ext, "GL_KHR_texture_compression_astc_ldr")) {
            void_log("[texcache] ASTC cache, but the GPU has no ASTC LDR: not used");
            close(fd);
            fd = -1;
            return;
        }
        storage = GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    } else {
        storage = GL_COMPRESSED_RGBA8_ETC2_EAC;
    }
    size_t bytes = (size_t)header.entry_count * sizeof(s2tc_entry);
    entries = (s2tc_entry *)malloc(bytes);
    size_t done = 0;
    while (entries && done < bytes) {
        ssize_t n = pread(fd, (uint8_t *)entries + done, bytes - done, (off_t)(header.index_offset + done));
        if (n <= 0) break;
        done += (size_t)n;
    }
    if (!entries || done != bytes) {
        void_log("[texcache] %s: index unreadable", path);
        free(entries);
        entries = NULL;
        close(fd);
        fd = -1;
        return;
    }
    state = 1;
    void_log("[texcache] %s: %s, %llu levels of %llu textures, %.0f MB", path,
        header.format == S2TC_FORMAT_ASTC ? "ASTC 4x4" : "ETC2", (unsigned long long)header.entry_count,
        (unsigned long long)header.textures, (double)header.file_size / 1048576.0);
}

static const s2tc_entry *find(int source, const uint8_t *data, uint32_t bytes, uint32_t w, uint32_t h)
{
    uint64_t key = s2tc_key(data, bytes, source);
    size_t lo = 0, hi = (size_t)header.entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < header.entry_count && entries[lo].key == key; ++lo) {
        const s2tc_entry *e = &entries[lo];
        if (e->source == source && e->source_bytes == bytes && e->width == w && e->height == h &&
            e->bytes == s2tc_encoded_bytes(e->encoding, w, h) && e->offset + e->bytes <= header.file_size)
            return e;
    }
    return NULL;
}

static uint8_t *scratch_for(size_t n)
{
    if (n > scratch_cap) {
        uint8_t *p = (uint8_t *)realloc(scratch, n);
        if (!p) return NULL;
        scratch = p;
        scratch_cap = n;
    }
    return scratch;
}

/* The entry's blocks as the storage format takes them (ETC2 RGB gains its
 * opaque alpha half). Returns the byte count, 0 on a read failure. */
static size_t entry_blocks(const s2tc_entry *e, uint8_t **out)
{
    int expand = e->encoding == S2TC_ENC_ETC2_RGB;
    size_t blocks = e->bytes / (expand ? 8u : 16u), total = blocks * 16;
    uint8_t *buf = scratch_for(total + (expand ? e->bytes : 0));
    if (!buf) return 0;
    uint8_t *raw = expand ? buf + total : buf;
    size_t done = 0;
    while (done < e->bytes) {
        ssize_t n = pread(fd, raw + done, e->bytes - done, (off_t)(e->offset + done));
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    if (expand)
        for (size_t b = 0; b < blocks; ++b) {
            memcpy(buf + b * 16, s2tc_opaque_eac, 8);
            memcpy(buf + b * 16 + 8, raw + b * 8, 8);
        }
    *out = buf;
    return total;
}

static int load_codec(void)
{
    static int tried;
    if (!tried) {
        tried = 1;
        void *lib = dlopen("libs2texconv.so", RTLD_NOW | RTLD_LOCAL);
        Dl_info self;
        if (!lib && dladdr((void *)d3d_texcache_upload, &self) && self.dli_fname) {
            /* A development library loaded from the app's files folder: its sibling. */
            char path[1024];
            snprintf(path, sizeof path, "%s", self.dli_fname);
            char *slash = strrchr(path, '/');
            if (slash) {
                snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libs2texconv.so");
                lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            }
        }
        if (lib) {
            encoder = (encode_fn)dlsym(lib, "s2texconv_encode_rgba");
            decoder = (decode_fn)dlsym(lib, "s2texconv_decode_dxt");
        }
        if (!encoder || !decoder) void_log("[texcache] libs2texconv.so encoder unavailable: %s", lib ? "symbols" : dlerror());
    }
    return encoder && decoder;
}

static void report(void)
{
    uint64_t now = void_monotonic_ns();
    if (now - last_report < 10000000000ull) return;
    last_report = now;
    void_log("[texcache] %llu levels from the cache (%.0f MB), %llu not in it, %llu encoded here; textures %llu compressed, "
        "%llu ATC (%llu levels, %.0f MB, %.2f s transcoding), %llu RGBA", (unsigned long long)hits, (double)hit_bytes / 1048576.0,
        (unsigned long long)misses, (unsigned long long)fallbacks, (unsigned long long)compressed_textures,
        (unsigned long long)atc_textures, (unsigned long long)atc_levels, (double)atc_bytes / 1048576.0, (double)atc_ns / 1e9,
        (unsigned long long)rgba_textures);
}

/* ATC sampling (the driver lists all three formats) unless turned off. */
static int atc_on(void)
{
    if (atc_state) return atc_state > 0;
    atc_state = -1;
    const char *env = getenv("S2_ATC");
    char path[1200];
    snprintf(path, sizeof path, "%s/noatc", void_data_dir());
    if ((env && *env == '0') || !access(path, F_OK)) { void_log("[texcache] ATC transcoding off"); return 0; }
    GLint count = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
    GLint *formats = count > 0 ? (GLint *)calloc((size_t)count, sizeof *formats) : NULL;
    int found = 0;
    if (formats) {
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats);
        for (GLint i = 0; i < count; ++i)
            found |= formats[i] == GL_ATC_RGB_AMD ? 1 : formats[i] == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ? 2 :
                     formats[i] == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD ? 4 : 0;
        free(formats);
    }
    if (found == 7) atc_state = 1;
    void_log("[texcache] ATC transcoding %s", atc_state > 0 ? "on: uncached DXT textures stay compressed" : "unavailable (no ATC formats)");
    return atc_state > 0;
}

/* A DXT rect of an ATC texture, block-aligned as D3D locks are: transcoded and uploaded. */
static int atc_upload(d3d_texture *t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *data,
    uint32_t pitch)
{
    GLenum storage = t->gl_internal;
    size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * d3d_atc_block_bytes(storage);
    uint8_t *buf = scratch_for(need);
    if (!buf) return 1;
    uint64_t started = void_monotonic_ns();
    if (storage == GL_ATC_RGB_AMD && d3d_atc_storage(t->fmt->conv, data, w, h, pitch) != GL_ATC_RGB_AMD && !atc_lost_alpha++)
        void_log("[texcache] a later level of an opaque DXT1 texture has transparent texels: drawn opaque");
    size_t bytes = d3d_atc_transcode(t->fmt->conv, storage, data, w, h, pitch, buf);
    atc_ns += void_monotonic_ns() - started;
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, storage, (GLsizei)bytes, buf);
    GLenum err = glGetError();
    if (err) void_log("[texcache] ATC level %u %ux%u at %u,%u: GL error %04X", level, w, h, x, y, err);
    ++atc_levels;
    atc_bytes += bytes;
    report();
    return 1;
}

/* A DXT level (or block-aligned rect of one) for texture t, render thread.
 * 1 when uploaded here; 0 leaves it to the RGBA8 path. */
int d3d_texcache_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    int source = source_of(t->fmt);
    if (!source || t->target != GL_TEXTURE_2D || face || t->scale != 1.0f || (t->usage & D3DUSAGE_AUTOGENMIPMAP) ||
        t->cache_state == CACHE_RGBA)
        return 0;
    if (t->cache_state == CACHE_ATC) return atc_upload(t, level, x, y, w, h, data, pitch);
    if (!state) open_cache();
    if (state != 1 && !atc_on()) return 0;   /* neither: the RGBA8 decode, as before */
    uint32_t lw, lh;
    d3d_texture_level_size(t, level, &lw, &lh);
    uint32_t row = d3d_format_pitch(t->fmt, w);
    int full = !x && !y && w == lw && h == lh && pitch == row;
    const s2tc_entry *e = state == 1 && full ? find(source, data, s2tc_dxt_bytes(source, lw, lh), lw, lh) : NULL;
    if (t->cache_state == CACHE_UNDECIDED) {
        if (!e) {
            /* Not converted by the launcher: ATC where the GPU has it (nothing is
             * in the RGBA8 storage yet, so it is recreated compressed). */
            GLenum atc = atc_on() ? d3d_atc_storage(t->fmt->conv, data, w, h, pitch) : 0;
            if (atc) {
                t->cache_state = CACHE_ATC;
                t->gl_internal = atc;
                d3d_texture_storage(t);
                ++atc_textures;
                return atc_upload(t, level, x, y, w, h, data, pitch);
            }
            t->cache_state = CACHE_RGBA;
            ++rgba_textures;
            ++misses;
            report();
            return 0;
        }
        /* Nothing is in the RGBA8 storage yet: recreate it compressed. */
        t->cache_state = CACHE_COMPRESSED;
        t->gl_internal = storage;
        d3d_texture_storage(t);
        ++compressed_textures;
    }
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, t->gl);
    if (e) {
        uint8_t *blocks = NULL;
        size_t bytes = entry_blocks(e, &blocks);
        if (bytes) {
            glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, storage, (GLsizei)bytes, blocks);
            GLenum err = glGetError();
            if (err) void_log("[texcache] level %u %ux%u: GL error %04X", level, lw, lh, err);
            ++hits;
            hit_bytes += bytes;
            report();
            return 1;
        }
    }
    /* A compressed texture's level or rect the cache lacks: encode it here. */
    ++misses;
    if (!load_codec()) return 1;
    uint32_t rows = d3d_format_rows(t->fmt, h);
    size_t dxt = (size_t)row * rows, rgba = (size_t)w * h * 4;
    int encoding = storage == GL_COMPRESSED_RGBA_ASTC_4x4_KHR ? S2TC_ENC_ASTC_4x4 : S2TC_ENC_ETC2_RGBA;
    size_t out = s2tc_encoded_bytes(encoding, w, h);
    uint8_t *buf = scratch_for(dxt + rgba + out);
    if (!buf) return 1;
    for (uint32_t r = 0; r < rows; ++r) memcpy(buf + (size_t)r * row, data + (size_t)r * pitch, row);
    decoder(source, buf, w, h, buf + dxt);
    if (encoder(encoding, buf + dxt, w, h, buf + dxt + rgba)) return 1;
    glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, storage, (GLsizei)out,
        buf + dxt + rgba);
    ++fallbacks;
    report();
    return 1;
}
