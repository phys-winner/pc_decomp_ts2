/* Pre-converted DXT textures (android/texconv/s2tc.h): the launcher's texture
 * card re-encodes every DXT level of the installed game into ETC2 or ASTC 4x4
 * once (libs2texconv.so) and files it by a hash of the original blocks in
 * <cache root>/port_textures/textures.s2tc. On the render thread a DXT level
 * the game uploads is looked up by that hash; the first level of a texture
 * decides its storage: found, the texture is recreated in the cache's
 * compressed format and every level goes up as converted blocks; not found (a
 * texture the game generates itself, custom content), it keeps the RGBA8
 * storage and the CPU decode of d3d9_format.c. A later level of a compressed
 * texture missing from the cache is encoded here with the converter's own
 * encoder (dlopen). S2_TEXCACHE=0 or <data>/notexcache turns the cache off.
 *
 * The cache itself (d3d9_texcache.h: open, lookup, blocks, encoder) serves both
 * renderers: d3d_texcache_upload below is the Direct3D 9 -> GLES layer's use
 * of it, android/render/s2r_texcache.c the native renderers'. The index is
 * mapped, not read into the heap, and its pages are let go 5 s after the last
 * lookup (loading over): 5.6 MB of the process's memory for the Legacy
 * Collection.
 *
 * A DXT texture the cache does not hold stays compressed too where the old
 * layer's GLES driver samples ATC (Adreno: no S3TC, but AMD's ATC, see
 * d3d9_atc.c): its first level decides ATC storage (DXT1 with transparent
 * texels takes the alpha form) and every level goes up transcoded block by
 * block, 4-8 times smaller than the RGBA8 decode. S2_ATC=0 or <data>/noatc
 * keeps the decode. */
#define _GNU_SOURCE
#include "d3d9_texcache.h"
#include "d3d9_atc.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif

d3d_texcache_stats d3d_texcache_counts;

static int state;                       /* 0 not opened, 1 open, -1 none */
static int cache_format;                /* S2TC_FORMAT_* of the open cache */
static int fd = -1;
static s2tc_header header;
static const s2tc_entry *entries;       /* the index: mapped, or read when it cannot be */
static void *index_map;
static size_t index_map_bytes;
static uint64_t index_used_ns;          /* the last lookup since the mapped pages were last let go; 0: none */
static uint8_t *scratch;
static size_t scratch_cap;
static uint64_t last_report, last_reported;

typedef int (*encode_fn)(int, const uint8_t *, uint32_t, uint32_t, uint8_t *);
typedef void (*decode_fn)(int, const uint8_t *, uint32_t, uint32_t, uint8_t *);
static encode_fn encoder;
static decode_fn decoder;

int d3d_texcache_source(const d3d_format *f)
{
    switch (f ? f->conv : CONV_NONE) {
    case CONV_DXT1: return S2TC_SRC_DXT1;
    case CONV_DXT3: return S2TC_SRC_DXT3;
    case CONV_DXT5: return S2TC_SRC_DXT5;
    default: return 0;
    }
}

static void close_cache(void)
{
    if (index_map) munmap(index_map, index_map_bytes);
    else free((void *)entries);
    index_map = NULL;
    entries = NULL;
    if (fd >= 0) close(fd);
    fd = -1;
}

/* The index, mapped read-only (its offset rounded down to a page); NULL when the mapping fails. */
static const s2tc_entry *map_index(size_t bytes)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (header.index_offset & 7)) return NULL;
    uint64_t base = header.index_offset / (uint64_t)page * (uint64_t)page;
    size_t lead = (size_t)(header.index_offset - base);
    void *map = mmap(NULL, lead + bytes, PROT_READ, MAP_SHARED, fd, (off_t)base);
    if (map == MAP_FAILED) return NULL;
    index_map = map;
    index_map_bytes = lead + bytes;
    return (const s2tc_entry *)((const uint8_t *)map + lead);
}

static const s2tc_entry *read_index(size_t bytes)
{
    uint8_t *copy = (uint8_t *)malloc(bytes);
    size_t done = 0;
    while (copy && done < bytes) {
        ssize_t n = pread(fd, copy + done, bytes - done, (off_t)(header.index_offset + done));
        if (n <= 0) break;
        done += (size_t)n;
    }
    if (copy && done == bytes) return (const s2tc_entry *)copy;
    free(copy);
    return NULL;
}

static void open_cache(int etc2, int astc)
{
    state = -1;
    const char *env = getenv("S2_TEXCACHE");
    char path[1200];
    snprintf(path, sizeof path, "%s/notexcache", void_data_dir());
    if ((env && *env == '0') || !access(path, F_OK)) { void_log("[texcache] off"); return; }
    const char *root = getenv("PORT_TEXTURE_CACHE_ROOT");
    if (root && *root && strcmp(root, void_root_dir())) {
        snprintf(path, sizeof path, "%s/%s/%s", root, S2TC_DIR, S2TC_FILE);
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        snprintf(path, sizeof path, "%s/%s/%s", void_root_dir(), S2TC_DIR, S2TC_FILE);
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) { void_log("[texcache] no converted textures (%s)", path); return; }
    if (pread(fd, &header, sizeof header, 0) != (ssize_t)sizeof header || header.magic != S2TC_MAGIC ||
        header.version != S2TC_VERSION || !(header.flags & 1) || !header.entry_count ||
        header.entry_count > (1u << 26) || header.index_offset + header.entry_count * sizeof(s2tc_entry) > header.file_size ||
        (header.format != S2TC_FORMAT_ETC2 && header.format != S2TC_FORMAT_ASTC)) {
        void_log("[texcache] %s: not a complete cache", path);
        close_cache();
        return;
    }
    const char *name = header.format == S2TC_FORMAT_ASTC ? "ASTC 4x4" : "ETC2";
    if (!(header.format == S2TC_FORMAT_ASTC ? astc : etc2)) {
        void_log("[texcache] %s cache, but the GPU does not sample %s: not used", name,
            header.format == S2TC_FORMAT_ASTC ? "ASTC LDR" : "ETC2");
        close_cache();
        return;
    }
    size_t bytes = (size_t)header.entry_count * sizeof(s2tc_entry);
    entries = map_index(bytes);
    int mapped = entries != NULL;
    if (!entries) entries = read_index(bytes);
    if (!entries) {
        void_log("[texcache] %s: index unreadable", path);
        close_cache();
        return;
    }
    cache_format = (int)header.format;
    state = 1;
    uint32_t covered = s2tc_covered(header.sources);
    void_log("[texcache] %s: %s, %llu levels of %llu textures, %.0f MB, index %s (%.1f MB), sources%s%s%s", path, name,
        (unsigned long long)header.entry_count, (unsigned long long)header.textures, (double)header.file_size / 1048576.0,
        mapped ? "mapped" : "read", (double)bytes / 1048576.0,
        (covered & S2TC_SOURCE_BIT(S2TC_SRC_DXT1)) ? " DXT1" : "", (covered & S2TC_SOURCE_BIT(S2TC_SRC_DXT3)) ? " DXT3" : "",
        (covered & S2TC_SOURCE_BIT(S2TC_SRC_DXT5)) ? " DXT5" : "");
}

int d3d_texcache_open(int etc2, int astc)
{
    if (!state) open_cache(etc2, astc);
    return state == 1 ? cache_format : 0;
}

const s2tc_entry *d3d_texcache_find(int source, const uint8_t *dxt, uint32_t w, uint32_t h)
{
    if (state != 1 || !source || !w || !h) return NULL;
    if (index_map) index_used_ns = void_monotonic_ns();
    uint32_t bytes = s2tc_dxt_bytes(source, w, h);
    uint64_t key = s2tc_key(dxt, bytes, source);
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

static int read_exact(uint8_t *out, size_t bytes, uint64_t offset)
{
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, out + done, bytes - done, (off_t)(offset + done));
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

const uint8_t *d3d_texcache_blocks(const s2tc_entry *e, int encoding, size_t *bytes)
{
    if (state != 1 || !e) return NULL;
    int astc_entry = e->encoding == S2TC_ENC_ASTC_4x4, astc_storage = encoding == S2TC_ENC_ASTC_4x4;
    if (astc_entry != astc_storage) return NULL;
    if (e->encoding == encoding) {
        uint8_t *buf = scratch_for(e->bytes);
        if (!buf || !read_exact(buf, e->bytes, e->offset)) return NULL;
        *bytes = e->bytes;
        return buf;
    }
    if (e->encoding == S2TC_ENC_ETC2_RGB && encoding == S2TC_ENC_ETC2_RGBA) {
        /* an opaque EAC alpha block before each colour block */
        size_t blocks = e->bytes / 8u, total = blocks * 16u;
        uint8_t *buf = scratch_for(total + e->bytes);
        if (!buf) return NULL;
        uint8_t *raw = buf + total;
        if (!read_exact(raw, e->bytes, e->offset)) return NULL;
        for (size_t b = 0; b < blocks; ++b) {
            memcpy(buf + b * 16, s2tc_opaque_eac, 8);
            memcpy(buf + b * 16 + 8, raw + b * 8, 8);
        }
        *bytes = total;
        return buf;
    }
    if (e->encoding == S2TC_ENC_ETC2_RGBA && encoding == S2TC_ENC_ETC2_RGB) {
        /* the colour half of each block (ETC2 RGBA8's colour block is an ETC2 RGB8 block) */
        size_t blocks = e->bytes / 16u;
        uint8_t *buf = scratch_for(e->bytes);
        if (!buf || !read_exact(buf, e->bytes, e->offset)) return NULL;
        for (size_t b = 0; b < blocks; ++b) memmove(buf + b * 8, buf + b * 16 + 8, 8);
        *bytes = blocks * 8u;
        if (!d3d_texcache_counts.alpha_dropped++)
            void_log("[texcache] a level with transparent texels in a texture stored opaque (ETC2 RGB): drawn opaque");
        return buf;
    }
    return NULL;
}

static int load_codec(void)
{
    static int tried;
    if (!tried) {
        tried = 1;
        void *lib = dlopen("libs2texconv.so", RTLD_NOW | RTLD_LOCAL);
        Dl_info self;
        if (!lib && dladdr((void *)d3d_texcache_encode, &self) && self.dli_fname) {
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

const uint8_t *d3d_texcache_encode(int source, int encoding, const uint8_t *dxt, uint32_t w, uint32_t h, uint32_t pitch,
    size_t *bytes)
{
    if (!source || !w || !h || !load_codec()) return NULL;
    uint64_t started = void_monotonic_ns();
    uint32_t row = ((w + 3) / 4) * (source == S2TC_SRC_DXT1 ? 8u : 16u), rows = (h + 3) / 4;
    size_t packed = pitch == row ? 0 : (size_t)row * rows, rgba = (size_t)w * h * 4, out = s2tc_encoded_bytes(encoding, w, h);
    uint8_t *buf = scratch_for(packed + rgba + out);
    if (!buf) return NULL;
    if (packed) {
        for (uint32_t r = 0; r < rows; ++r) memcpy(buf + (size_t)r * row, dxt + (size_t)r * pitch, row);
        dxt = buf;
    }
    decoder(source, dxt, w, h, buf + packed);
    if (encoder(encoding, buf + packed, w, h, buf + packed + rgba)) return NULL;
    d3d_texcache_counts.encoded++;
    d3d_texcache_counts.encoded_ns += void_monotonic_ns() - started;
    *bytes = out;
    return buf + packed + rgba;
}

void d3d_texcache_report(void)
{
    const d3d_texcache_stats *c = &d3d_texcache_counts;
    uint64_t now = void_monotonic_ns(), seen = c->hits + c->misses + c->encoded + c->compressed_textures + c->rgba_textures +
        c->atc_levels;
    if (index_used_ns && now - index_used_ns > 5000000000ull) {
        /* Loading is over: the index pages the lookups brought in (readahead pulls most of it) leave
         * the process, and fault back in from the page cache when the next load looks something up. */
        index_used_ns = 0;
        madvise(index_map, index_map_bytes, MADV_DONTNEED);
        static int logged;
        if (!logged++) void_log("[texcache] index pages let go after loading (%.1f MB mapped)", (double)index_map_bytes / 1048576.0);
    }
    if (seen == last_reported || now - last_report < 10000000000ull) return;
    last_report = now;
    last_reported = seen;
    char atc[160] = "";
    if (c->atc_textures)
        snprintf(atc, sizeof atc, ", %llu ATC (%llu levels, %.0f MB, %.2f s transcoding)", (unsigned long long)c->atc_textures,
            (unsigned long long)c->atc_levels, (double)c->atc_bytes / 1048576.0, (double)c->atc_ns / 1e9);
    void_log("[texcache] %llu levels from the cache (%.0f MB), %llu not in it, %llu encoded here (%.2f s); textures %llu "
        "compressed (%llu ETC2 RGB), %llu RGBA%s", (unsigned long long)c->hits, (double)c->hit_bytes / 1048576.0,
        (unsigned long long)c->misses, (unsigned long long)c->encoded, (double)c->encoded_ns / 1e9,
        (unsigned long long)c->compressed_textures, (unsigned long long)c->rgb_textures, (unsigned long long)c->rgba_textures, atc);
}

/* ------------------------------------------- the Direct3D 9 -> GLES layer */
static int atc_state;                   /* 0 untried, 1 on, -1 off */

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
    d3d_texcache_stats *c = &d3d_texcache_counts;
    GLenum storage = t->gl_internal;
    size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * d3d_atc_block_bytes(storage);
    uint8_t *buf = scratch_for(need);
    if (!buf) return 1;
    uint64_t started = void_monotonic_ns();
    if (storage == GL_ATC_RGB_AMD && d3d_atc_storage(t->fmt->conv, data, w, h, pitch) != GL_ATC_RGB_AMD && !c->atc_lost_alpha++)
        void_log("[texcache] a later level of an opaque DXT1 texture has transparent texels: drawn opaque");
    size_t bytes = d3d_atc_transcode(t->fmt->conv, storage, data, w, h, pitch, buf);
    c->atc_ns += void_monotonic_ns() - started;
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, storage, (GLsizei)bytes, buf);
    GLenum err = glGetError();
    if (err) void_log("[texcache] ATC level %u %ux%u at %u,%u: GL error %04X", level, w, h, x, y, err);
    ++c->atc_levels;
    c->atc_bytes += bytes;
    d3d_texcache_report();
    return 1;
}

/* Whether this GLES context samples ASTC LDR (asked once). */
static int gl_astc(void)
{
    static int known = -1;
    if (known < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        known = ext && strstr(ext, "GL_KHR_texture_compression_astc_ldr") != NULL;
    }
    return known;
}

/* A DXT level (or block-aligned rect of one) for texture t, render thread.
 * 1 when uploaded here; 0 leaves it to the RGBA8 path. */
int d3d_texcache_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch)
{
    d3d_texcache_stats *c = &d3d_texcache_counts;
    int source = d3d_texcache_source(t->fmt);
    if (!source || t->target != GL_TEXTURE_2D || face || t->scale != 1.0f || (t->usage & D3DUSAGE_AUTOGENMIPMAP) ||
        t->cache_state == D3D_TEXCACHE_RGBA)
        return 0;
    if (t->cache_state == D3D_TEXCACHE_ATC) return atc_upload(t, level, x, y, w, h, data, pitch);
    int format = d3d_texcache_open(1, gl_astc());
    if (!format && !atc_on()) return 0;   /* neither: the RGBA8 decode, as before */
    GLenum storage = format == S2TC_FORMAT_ASTC ? GL_COMPRESSED_RGBA_ASTC_4x4_KHR : GL_COMPRESSED_RGBA8_ETC2_EAC;
    int encoding = format == S2TC_FORMAT_ASTC ? S2TC_ENC_ASTC_4x4 : S2TC_ENC_ETC2_RGBA;
    uint32_t lw, lh;
    d3d_texture_level_size(t, level, &lw, &lh);
    uint32_t row = d3d_format_pitch(t->fmt, w);
    int full = !x && !y && w == lw && h == lh && pitch == row;
    const s2tc_entry *e = format && full ? d3d_texcache_find(source, data, lw, lh) : NULL;
    if (t->cache_state == D3D_TEXCACHE_UNDECIDED) {
        if (!e) {
            /* Not converted by the launcher: ATC where the GPU has it (nothing is
             * in the RGBA8 storage yet, so it is recreated compressed). */
            GLenum atc = atc_on() ? d3d_atc_storage(t->fmt->conv, data, w, h, pitch) : 0;
            if (atc) {
                t->cache_state = D3D_TEXCACHE_ATC;
                t->gl_internal = atc;
                d3d_texture_storage(t);
                ++c->atc_textures;
                return atc_upload(t, level, x, y, w, h, data, pitch);
            }
            t->cache_state = D3D_TEXCACHE_RGBA;
            ++c->rgba_textures;
            ++c->misses;
            d3d_texcache_report();
            return 0;
        }
        /* Nothing is in the RGBA8 storage yet: recreate it compressed. */
        t->cache_state = D3D_TEXCACHE_COMPRESSED;
        t->gl_internal = storage;
        d3d_texture_storage(t);
        ++c->compressed_textures;
    }
    d3d_gl_upload_unit();
    glBindTexture(GL_TEXTURE_2D, t->gl);
    if (e) {
        size_t bytes = 0;
        const uint8_t *blocks = d3d_texcache_blocks(e, encoding, &bytes);
        if (blocks) {
            glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, storage, (GLsizei)bytes, blocks);
            GLenum err = glGetError();
            if (err) void_log("[texcache] level %u %ux%u: GL error %04X", level, lw, lh, err);
            ++c->hits;
            c->hit_bytes += bytes;
            d3d_texcache_report();
            return 1;
        }
    }
    /* A compressed texture's level or rect the cache lacks: encode it here. */
    ++c->misses;
    size_t out = 0;
    const uint8_t *blocks = d3d_texcache_encode(source, encoding, data, w, h, pitch, &out);
    if (!blocks) return 1;
    glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, storage, (GLsizei)out,
        blocks);
    d3d_texcache_report();
    return 1;
}
