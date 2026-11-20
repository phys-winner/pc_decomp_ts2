/* The launcher's converted DXT levels (port_textures/textures.s2tc, android/texconv/s2tc.h) for
 * both renderers: the cache file, its lookups and the converter's encoder for levels it lacks
 * (d3d9_texcache.c). The Direct3D 9 -> GLES layer uses them in d3d_texcache_upload, the native
 * renderers in s2r_texcache.c. Render thread only: one scratch buffer serves every call. */
#ifndef D3D9_TEXCACHE_H
#define D3D9_TEXCACHE_H

#include "d3d9_internal.h"
#include "../texconv/s2tc.h"

/* d3d_texture.cache_state */
enum { D3D_TEXCACHE_UNDECIDED = 0, D3D_TEXCACHE_RGBA = 1, D3D_TEXCACHE_COMPRESSED = 2, D3D_TEXCACHE_ATC = 3 };

/* Opens the cache once. S2TC_FORMAT_ETC2 or S2TC_FORMAT_ASTC when its blocks can be sampled
 * (`etc2`, `astc`: what the GPU samples), else 0: no cache, turned off, or not sampled here. */
int d3d_texcache_open(int etc2, int astc);
/* S2TC_SRC_* of a format's DXT blocks, 0 for every other format. */
int d3d_texcache_source(const d3d_format *f);
/* The entry of a whole w x h level whose DXT blocks are `dxt` (tightly packed), NULL when the
 * cache does not hold it. */
const s2tc_entry *d3d_texcache_find(int source, const uint8_t *dxt, uint32_t w, uint32_t h);
/* An entry's blocks for a texture stored as `encoding` (S2TC_ENC_*): ETC2 RGB blocks gain an opaque
 * EAC alpha half for ETC2 RGBA storage, ETC2 RGBA blocks lose theirs for ETC2 RGB storage. Scratch
 * memory valid until the next call; NULL on a read failure or a mismatch. */
const uint8_t *d3d_texcache_blocks(const s2tc_entry *e, int encoding, size_t *bytes);
/* A w x h rect of DXT rows (`pitch` apart) encoded here as `encoding` (libs2texconv.so's own
 * encoder). Scratch memory valid until the next call; NULL when the encoder is missing or fails. */
const uint8_t *d3d_texcache_encode(int source, int encoding, const uint8_t *dxt, uint32_t w, uint32_t h, uint32_t pitch,
    size_t *bytes);

typedef struct d3d_texcache_stats {
    uint64_t hits, hit_bytes;             /* levels uploaded from the cache */
    uint64_t misses;                      /* levels it lacked */
    uint64_t encoded, encoded_ns;         /* levels or rects encoded here, and the time it took */
    uint64_t compressed_textures, rgb_textures, rgba_textures;   /* storage chosen: converted (of them ETC2 RGB), RGBA8 */
    uint64_t alpha_dropped;               /* ETC2 RGBA levels drawn opaque in an ETC2 RGB texture */
    uint64_t atc_textures, atc_levels, atc_bytes, atc_ns, atc_lost_alpha;   /* the old layer's ATC transcoding */
} d3d_texcache_stats;
extern d3d_texcache_stats d3d_texcache_counts;
/* The [texcache] line when the counts changed, at most every 10 s (uploads and presents call it). */
void d3d_texcache_report(void);

#endif
