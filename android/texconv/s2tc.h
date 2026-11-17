/* The pre-converted texture cache shared by the launcher's converter
 * (android/texconv/s2texconv.cpp, libs2texconv.so) and the game's renderer
 * (android/host/d3d9_texcache.c).
 *
 * The Sims 2 keeps its textures as TXTR resources (RCOL cImageData) in DBPF
 * packages, the larger mip levels in LIFO resources (cLevelInfo). The game
 * copies each level's DXT1/DXT3/DXT5 blocks unchanged into a locked Direct3D
 * surface; GPUs without S3TC (the Mali family) then had every level decoded
 * to RGBA8 on the CPU at load time. The converter re-encodes every DXT level
 * of the installed game once, on the device, into ETC2 or ASTC 4x4, and files
 * it under a hash of the original DXT bytes. When the game uploads a level
 * whose bytes are found here, the renderer uploads the converted blocks
 * instead: no decode, 4x (DXT1 8x) less GPU memory than RGBA8.
 *
 * <game>/port_textures/textures.s2tc, little-endian:
 *   s2tc_header (128 bytes)
 *   data: each entry's blocks, 16-byte aligned
 *   index: entry_count s2tc_entry records sorted by key, at index_offset
 * Keys are XXH64 of the original level bytes with the source format as the
 * seed; a lookup also checks the byte count and dimensions. An entry's ETC2
 * RGB blocks (opaque DXT levels) become ETC2 RGBA8 at upload with an opaque
 * EAC alpha block before each, so every converted texture of one format
 * shares one GL storage format whatever its levels hold. */
#ifndef S2TC_H
#define S2TC_H

#include <stdint.h>
#include <string.h>

#define S2TC_MAGIC 0x43543253u          /* "S2TC" */
#define S2TC_VERSION 1u
#define S2TC_DIR "port_textures"
#define S2TC_FILE "textures.s2tc"

enum { S2TC_FORMAT_ETC2 = 1, S2TC_FORMAT_ASTC = 2 };          /* the whole cache */
enum { S2TC_SRC_DXT1 = 1, S2TC_SRC_DXT3 = 3, S2TC_SRC_DXT5 = 5 };
enum { S2TC_ENC_ETC2_RGB = 1, S2TC_ENC_ETC2_RGBA = 2, S2TC_ENC_ASTC_4x4 = 3 };

typedef struct {
    uint32_t magic, version, format, flags;      /* flags bit 0: complete */
    uint64_t entry_count, index_offset;
    uint64_t data_offset, file_size;
    uint64_t textures;                            /* TXTR resources with DXT levels */
    uint64_t levels;                              /* levels converted (before deduplication) */
    uint64_t source_bytes;                        /* their DXT bytes */
    uint64_t converted_bytes;                     /* the data area */
    uint64_t signature;                           /* of the game's package files (sizes, names) */
    uint8_t reserved[128 - 88];
} s2tc_header;

typedef struct {
    uint64_t key;
    uint64_t offset;                              /* from the start of the file */
    uint32_t source_bytes, bytes;
    uint16_t width, height;
    uint8_t source, encoding;
    uint16_t reserved;
} s2tc_entry;

#if defined(__cplusplus)
static_assert(sizeof(s2tc_header) == 128, "s2tc_header");
static_assert(sizeof(s2tc_entry) == 32, "s2tc_entry");
#else
_Static_assert(sizeof(s2tc_header) == 128, "s2tc_header");
_Static_assert(sizeof(s2tc_entry) == 32, "s2tc_entry");
#endif

/* ---------------------------------------------------------------- XXH64
 * The xxHash 64-bit algorithm (Yann Collet), written from its published
 * specification. */
#define S2TC_P1 0x9E3779B185EBCA87ull
#define S2TC_P2 0xC2B2AE3D27D4EB4Full
#define S2TC_P3 0x165667B19E3779F9ull
#define S2TC_P4 0x85EBCA77C2B2AE63ull
#define S2TC_P5 0x27D4EB2F165667C5ull

static inline uint64_t s2tc_rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t s2tc_read64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t s2tc_read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t s2tc_round(uint64_t acc, uint64_t input)
{
    acc += input * S2TC_P2;
    acc = s2tc_rotl(acc, 31);
    return acc * S2TC_P1;
}
static inline uint64_t s2tc_merge(uint64_t acc, uint64_t val)
{
    acc ^= s2tc_round(0, val);
    return acc * S2TC_P1 + S2TC_P4;
}

static inline uint64_t s2tc_xxh64(const void *input, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)input, *end = p + len;
    uint64_t h;
    if (len >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + S2TC_P1 + S2TC_P2, v2 = seed + S2TC_P2, v3 = seed, v4 = seed - S2TC_P1;
        do {
            v1 = s2tc_round(v1, s2tc_read64(p)); p += 8;
            v2 = s2tc_round(v2, s2tc_read64(p)); p += 8;
            v3 = s2tc_round(v3, s2tc_read64(p)); p += 8;
            v4 = s2tc_round(v4, s2tc_read64(p)); p += 8;
        } while (p <= limit);
        h = s2tc_rotl(v1, 1) + s2tc_rotl(v2, 7) + s2tc_rotl(v3, 12) + s2tc_rotl(v4, 18);
        h = s2tc_merge(h, v1);
        h = s2tc_merge(h, v2);
        h = s2tc_merge(h, v3);
        h = s2tc_merge(h, v4);
    } else {
        h = seed + S2TC_P5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= s2tc_round(0, s2tc_read64(p));
        h = s2tc_rotl(h, 27) * S2TC_P1 + S2TC_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)s2tc_read32(p) * S2TC_P1;
        h = s2tc_rotl(h, 23) * S2TC_P2 + S2TC_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p) * S2TC_P5;
        h = s2tc_rotl(h, 11) * S2TC_P1;
        ++p;
    }
    h ^= h >> 33;
    h *= S2TC_P2;
    h ^= h >> 29;
    h *= S2TC_P3;
    h ^= h >> 32;
    return h;
}

/* A level's key: its DXT bytes, seeded by their format. */
static inline uint64_t s2tc_key(const void *dxt, size_t bytes, int source)
{
    return s2tc_xxh64(dxt, bytes, 0x5332544300000000ull | (uint64_t)(unsigned)source);
}

static inline uint32_t s2tc_dxt_bytes(int source, uint32_t w, uint32_t h)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * (source == S2TC_SRC_DXT1 ? 8u : 16u);
}

static inline uint32_t s2tc_encoded_bytes(int encoding, uint32_t w, uint32_t h)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * (encoding == S2TC_ENC_ETC2_RGB ? 8u : 16u);
}

/* The EAC alpha block of a fully opaque 4x4 block (base 255, multiplier 0). */
static const uint8_t s2tc_opaque_eac[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };

#endif
