/* Process-lifetime cache of host directory listings (dircache.c). */
#ifndef VOID_DIRCACHE_H
#define VOID_DIRCACHE_H

#include <stddef.h>
#include <sys/stat.h>

enum { DC_NOT_LOADED = -2, DC_UNKNOWN = -1, DC_ABSENT = 0, DC_FOUND = 1 };

/* `name` in host directory `dir`, case-insensitively (an exact match first).
 * DC_FOUND copies the entry's actual name to `actual` and its readdir type
 * (DT_*, DT_UNKNOWN when the file system gave none) to *type; DC_UNKNOWN when
 * the directory cannot be listed (the caller resolves it the slow way);
 * DC_NOT_LOADED when `cached_only` and the folder has not been listed yet. */
int dircache_lookup(const char *dir, const char *name, char *actual, size_t cap, unsigned char *type, int cached_only);
/* stat(path) for a path inside a cached directory: an entry it does not hold
 * fails with ENOENT at once; results are kept until the entry changes. */
int dircache_stat(const char *path, struct stat *st);
/* Each live name of `dir` in listing order; 0 when it cannot be listed (or,
 * with `cached_only`, has not been listed yet). */
int dircache_list(const char *dir, void (*each)(void *arg, const char *name, unsigned char type), void *arg,
    int cached_only);
/* What the game (or the host) did to the file system. */
void dircache_created(const char *path, int is_dir);
void dircache_removed(const char *path);
void dircache_changed(const char *path);            /* contents/size/times */
void dircache_renamed(const char *from, const char *to);
void dircache_writer(const char *path, int delta);   /* open writable handles: no cached stat meanwhile */
int dircache_active(void);

#endif
