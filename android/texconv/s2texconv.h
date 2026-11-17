/* The Sims 2 texture converter (s2texconv.cpp, libs2texconv.so): every DXT
 * mip level of the installed game re-encoded once into ETC2 or ASTC 4x4 and
 * filed by a hash of its original bytes in <game>/port_textures/textures.s2tc
 * (s2tc.h), which the renderer reads. The game's own files are not changed:
 * removing the conversion deletes that folder. Built for the launcher (JNI,
 * s2texconv_jni.cpp), for adb-shell runs (s2texconv_cli.cpp) and natively on
 * the PC for tests. */
#ifndef S2TEXCONV_H
#define S2TEXCONV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported for the renderer's dlopen (the library hides everything else). */
#define S2TEXCONV_API __attribute__((visibility("default")))

enum {
    S2TEXCONV_IDLE = 0,
    S2TEXCONV_SCANNING,
    S2TEXCONV_CONVERTING,
    S2TEXCONV_FINISHING,
    S2TEXCONV_REMOVING,
    S2TEXCONV_DONE,
    S2TEXCONV_FAILED,
    S2TEXCONV_CANCELLED,
};

typedef struct {
    int state;
    int64_t done, total;              /* packages */
    int64_t bytes_done, bytes_total;  /* texture resource bytes read (TXTR and LIFO) */
    int64_t levels;                   /* levels converted so far */
    char message[512];                /* failure reason, English */
} s2texconv_progress;

typedef struct {
    int64_t textures;                 /* TXTR resources in the single sampled package */
    int64_t bytes;                    /* the stored size of their TXTR and LIFO resources */
    int converted_format;             /* 0, 1 ETC2 or 2 ASTC: a complete conversion matching the sampled package */
    int64_t converted_textures;       /* TXTR resources with DXT levels it covers */
    int64_t converted_bytes;          /* the cache file */
    int stale;                        /* a conversion exists but the sampled package changed since */
} s2texconv_status;

/* One texture package sample and the existing cache. 0, or -1 with the reason in the progress message. */
S2TEXCONV_API int s2texconv_scan(const char *game_dir, s2texconv_status *out);
/* Converts every DXT level (format 1 ETC2, 2 ASTC); `threads` workers. 0 done,
 * 1 cancelled, -1 failed (reason in the progress message). */
S2TEXCONV_API int s2texconv_convert(const char *game_dir, int format, int threads);
/* Stored ZIP variants: borrowed read-only descriptor, launcher ZMI1 index, output in game_dir sidecar. */
S2TEXCONV_API int s2texconv_scan_zip(const char *game_dir, s2texconv_status *out, const char *index, int fd);
S2TEXCONV_API int s2texconv_convert_zip(const char *game_dir, int format, int threads, const char *index, int fd);
/* Deletes the conversion. 0, or -1. */
S2TEXCONV_API int s2texconv_remove(const char *game_dir);
S2TEXCONV_API void s2texconv_cancel(void);
S2TEXCONV_API void s2texconv_get_progress(s2texconv_progress *out);

/* The renderer's fallback for a level missing from the cache (a texture whose
 * other levels were found): RGBA8 (w*h*4) into `encoding` blocks (s2tc.h). 0. */
S2TEXCONV_API int s2texconv_encode_rgba(int encoding, const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out);
/* DXT1/3/5 blocks (S2TC_SRC_*) to RGBA8. */
S2TEXCONV_API void s2texconv_decode_dxt(int source, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif
