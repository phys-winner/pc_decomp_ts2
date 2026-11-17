/* Host directory listings, read once per process and kept.
 *
 * Android's shared storage (/storage/emulated/0) is a FUSE file system served
 * by MediaProvider. Measured on the Redmi Note 8 Pro (Android 13): a lookup of
 * a missing name costs ~450 us (the kernel caches no negative entries), an
 * open+close ~810 us and listing a two-entry directory ~1.6 ms, against 4-13 us
 * on the lower file system. The game resolves every path case-insensitively,
 * probes thousands of absent files (cursors, sounds, movies in each of 16
 * packs) and enumerates the same folders again and again: one start resolved
 * 20,248 paths (6,908 distinct, in 939 folders) for 63 s of wall time.
 *
 * A folder's names are now read once (one READDIR) and answered from memory:
 * case-insensitive lookups, misses included, the listing for FindFirstFile and
 * stat results once fetched. Everything the game changes goes through the
 * file layer (and the host's own writes through dircache_created etc.), which
 * keeps the listings current. Files another app adds or removes while the game
 * runs are seen at the next start, as the original game itself only scans its
 * folders when it starts. S2_DIRCACHE=0 (or <data>/nodircache) turns it off. */
#define _GNU_SOURCE
#include "host.h"
#include "dircache.h"
#include "zip_files.h"
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    char *name;                 /* NULL: removed */
    uint32_t hash;              /* of the ASCII-folded name */
    unsigned char type;         /* DT_* */
    unsigned char stat_valid;
    int writers;
    struct stat st;
} dc_entry;

typedef struct dc_dir {
    char *path;
    uint32_t path_hash;
    struct dc_dir *next;
    dc_entry *items;            /* listing order */
    uint32_t count, cap, live;
    int32_t *index;             /* open addressing over items; -1 empty */
    uint32_t index_mask;
} dc_dir;

#define DC_BUCKETS 4096u
static dc_dir *table[DC_BUCKETS];
static pthread_mutex_t dc_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int state;        /* 0 unknown, 1 on, 2 off */
static uint64_t loads, load_entries;

int dircache_active(void)
{
    int s = atomic_load(&state);
    if (!s) {
        const char *env = getenv("S2_DIRCACHE");
        char flag[1200];
        snprintf(flag, sizeof flag, "%s/nodircache", void_data_dir());
        s = (env && *env == '0') || !access(flag, F_OK) ? 2 : 1;
        atomic_store(&state, s);
        void_log("[files] directory cache %s", s == 1 ? "on" : "off");
    }
    return s == 1;
}

static uint32_t exact_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static uint32_t folded_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void index_insert(dc_dir *d, uint32_t item)
{
    uint32_t i = d->items[item].hash & d->index_mask;
    while (d->index[i] >= 0) i = (i + 1) & d->index_mask;
    d->index[i] = (int32_t)item;
}

static void reindex(dc_dir *d)
{
    uint32_t size = 16;
    while (size < d->live * 2 + 2) size <<= 1;
    free(d->index);
    d->index = (int32_t *)malloc(size * sizeof(int32_t));
    if (!d->index) void_fatal("dircache: out of memory");
    memset(d->index, 0xFF, size * sizeof(int32_t));
    d->index_mask = size - 1;
    for (uint32_t k = 0; k < d->count; ++k)
        if (d->items[k].name) index_insert(d, k);
}

static dc_entry *add_entry(dc_dir *d, const char *name, unsigned char type)
{
    if (d->count == d->cap) {
        uint32_t cap = d->cap ? d->cap * 2 : 16;
        dc_entry *items = (dc_entry *)realloc(d->items, cap * sizeof(dc_entry));
        if (!items) void_fatal("dircache: out of memory");
        d->items = items;
        d->cap = cap;
    }
    dc_entry *e = &d->items[d->count];
    memset(e, 0, sizeof *e);
    e->name = strdup(name);
    e->hash = folded_hash(name);
    e->type = type;
    ++d->count;
    ++d->live;
    if (!d->index || d->live * 2 > d->index_mask + 1) reindex(d);
    else index_insert(d, d->count - 1);
    return e;
}

/* The exact name, else the first case-insensitive match in listing order. */
static dc_entry *find_entry(dc_dir *d, const char *name)
{
    uint32_t h = folded_hash(name);
    dc_entry *folded = NULL;
    for (uint32_t i = h & d->index_mask; d->index[i] >= 0; i = (i + 1) & d->index_mask) {
        dc_entry *e = &d->items[d->index[i]];
        if (e->hash != h || strcasecmp(e->name, name)) continue;
        if (!strcmp(e->name, name)) return e;
        if (!folded || e < folded) folded = e;
    }
    return folded;
}

static void remove_entry(dc_dir *d, dc_entry *e)
{
    free(e->name);
    e->name = NULL;
    --d->live;
    reindex(d);
}

static dc_dir *find_dir(const char *path, uint32_t h)
{
    for (dc_dir *d = table[h & (DC_BUCKETS - 1)]; d; d = d->next)
        if (d->path_hash == h && !strcmp(d->path, path)) return d;
    return NULL;
}

static dc_dir *new_dir(const char *path, uint32_t h)
{
    dc_dir *d = (dc_dir *)calloc(1, sizeof *d);
    if (!d) void_fatal("dircache: out of memory");
    d->path = strdup(path);
    d->path_hash = h;
    reindex(d);
    d->next = table[h & (DC_BUCKETS - 1)];
    table[h & (DC_BUCKETS - 1)] = d;
    return d;
}

static void drop_dir(dc_dir *d)
{
    dc_dir **slot = &table[d->path_hash & (DC_BUCKETS - 1)];
    while (*slot && *slot != d) slot = &(*slot)->next;
    if (*slot) *slot = d->next;
    for (uint32_t k = 0; k < d->count; ++k) free(d->items[k].name);
    free(d->items);
    free(d->index);
    free(d->path);
    free(d);
}

static int host_stat(const char *path, struct stat *st)
{
    if (!stat(path, st)) return 0;
    return (errno == ENOENT || errno == ENOTDIR) ? s2_zip_stat(path, st) : -1;
}

static void zip_child(void *arg, const char *name, unsigned char type)
{
    dc_dir *d = arg;
    if (!find_entry(d, name)) add_entry(d, name, type);
}

/* The cached listing of `path`, reading it now if needed. dc_lock held. */
static dc_dir *get_dir(const char *path)
{
    uint32_t h = exact_hash(path);
    dc_dir *d = find_dir(path, h);
    if (d) return d;
    DIR *listing = opendir(path);
    struct stat st;
    if (!listing && (s2_zip_stat(path, &st) || !S_ISDIR(st.st_mode))) return NULL;
    d = new_dir(path, h);
    struct dirent *e;
    while (listing && (e = readdir(listing))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        add_entry(d, e->d_name, e->d_type);
    }
    if (listing) closedir(listing);
    s2_zip_list(path, zip_child, d);
    ++loads;
    load_entries += d->live;
    if ((loads & 255) == 0)
        void_log("[files] directory cache: %llu folders, %llu names", (unsigned long long)loads, (unsigned long long)load_entries);
    return d;
}

/* Split "dir/leaf" in place; NULL leaf when there is no separator. */
static char *split(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return NULL;
    *slash = 0;
    return slash + 1;
}

int dircache_lookup(const char *dir, const char *name, char *actual, size_t cap, unsigned char *type, int cached_only)
{
    if (!dircache_active() && !s2_zip_path(dir)) return cached_only ? DC_NOT_LOADED : DC_UNKNOWN;
    pthread_mutex_lock(&dc_lock);
    dc_dir *d = cached_only ? find_dir(dir, exact_hash(dir)) : get_dir(dir);
    int r = cached_only ? DC_NOT_LOADED : DC_UNKNOWN;
    if (d) {
        dc_entry *e = find_entry(d, name);
        if (e) {
            snprintf(actual, cap, "%s", e->name);
            if (type) *type = e->stat_valid ? (S_ISDIR(e->st.st_mode) ? DT_DIR : DT_REG) : e->type;
            r = DC_FOUND;
        } else {
            r = DC_ABSENT;
        }
    }
    pthread_mutex_unlock(&dc_lock);
    return r;
}

int dircache_stat(const char *path, struct stat *st)
{
    if (!dircache_active() && !s2_zip_path(path)) return host_stat(path, st);
    char work[4096];
    snprintf(work, sizeof work, "%s", path);
    char *leaf = split(work);
    /* Listings omit dot entries; FindFirstFile synthesizes them for empty Windows folders. */
    if (!leaf || !*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..")) return host_stat(path, st);
    pthread_mutex_lock(&dc_lock);
    dc_dir *d = find_dir(work, exact_hash(work));
    dc_entry *e = d ? find_entry(d, leaf) : NULL;
    if (d && (!e || strcmp(e->name, leaf))) {
        /* Only exact names: the caller resolved the path through the cache. */
        pthread_mutex_unlock(&dc_lock);
        if (!e) { errno = ENOENT; return -1; }
        return host_stat(path, st);
    }
    if (e && e->stat_valid) {
        *st = e->st;
        pthread_mutex_unlock(&dc_lock);
        return 0;
    }
    int r = host_stat(path, st);
    if (e && !r && !e->writers) {
        e->st = *st;
        e->stat_valid = 1;
        e->type = S_ISDIR(st->st_mode) ? DT_DIR : S_ISREG(st->st_mode) ? DT_REG : e->type;
    }
    pthread_mutex_unlock(&dc_lock);
    return r;
}

int dircache_list(const char *dir, void (*each)(void *arg, const char *name, unsigned char type), void *arg,
    int cached_only)
{
    if (!dircache_active() && !s2_zip_path(dir)) return 0;
    pthread_mutex_lock(&dc_lock);
    dc_dir *d = cached_only ? find_dir(dir, exact_hash(dir)) : get_dir(dir);
    if (d)
        for (uint32_t k = 0; k < d->count; ++k)
            if (d->items[k].name) each(arg, d->items[k].name, d->items[k].type);
    pthread_mutex_unlock(&dc_lock);
    return d != NULL;
}

/* The entry for `path` in its cached parent listing, or NULL. dc_lock held. */
static dc_entry *entry_of(const char *path, dc_dir **dir_out, char *work, size_t cap, char **leaf_out)
{
    snprintf(work, cap, "%s", path);
    char *leaf = split(work);
    if (leaf_out) *leaf_out = leaf;
    if (!leaf) return NULL;
    dc_dir *d = find_dir(work, exact_hash(work));
    if (dir_out) *dir_out = d;
    return d ? find_entry(d, leaf) : NULL;
}

void dircache_created(const char *path, int is_dir)
{
    if (!path || (!dircache_active() && !s2_zip_path(path))) return;
    char work[4096], *leaf;
    dc_dir *d = NULL;
    pthread_mutex_lock(&dc_lock);
    dc_entry *e = entry_of(path, &d, work, sizeof work, &leaf);
    if (d) {
        if (!e) e = add_entry(d, leaf, is_dir ? DT_DIR : DT_REG);
        e->type = is_dir ? DT_DIR : DT_REG;
        e->stat_valid = 0;
    }
    if (is_dir) {
        uint32_t h = exact_hash(path);
        if (!find_dir(path, h)) new_dir(path, h);       /* a new folder is empty */
    }
    pthread_mutex_unlock(&dc_lock);
}

/* Drop the listings of `path` and of everything below it. dc_lock held. */
static void drop_tree(const char *path)
{
    size_t n = strlen(path);
    for (uint32_t b = 0; b < DC_BUCKETS; ++b) {
        dc_dir *d = table[b];
        while (d) {
            dc_dir *next = d->next;
            if (!strncmp(d->path, path, n) && (d->path[n] == 0 || d->path[n] == '/')) drop_dir(d);
            d = next;
        }
    }
}

void dircache_removed(const char *path)
{
    if (!path || (!dircache_active() && !s2_zip_path(path))) return;
    char work[4096];
    dc_dir *d = NULL;
    pthread_mutex_lock(&dc_lock);
    dc_entry *e = entry_of(path, &d, work, sizeof work, NULL);
    int was_dir = !e || e->type == DT_DIR || e->type == DT_UNKNOWN || (e->stat_valid && S_ISDIR(e->st.st_mode));
    if (d && e) remove_entry(d, e);
    if (was_dir) drop_tree(path);
    pthread_mutex_unlock(&dc_lock);
}

void dircache_changed(const char *path)
{
    if (!path || (!dircache_active() && !s2_zip_path(path))) return;
    char work[4096];
    pthread_mutex_lock(&dc_lock);
    dc_entry *e = entry_of(path, NULL, work, sizeof work, NULL);
    if (e) e->stat_valid = 0;
    pthread_mutex_unlock(&dc_lock);
}

void dircache_writer(const char *path, int delta)
{
    if (!path || (!dircache_active() && !s2_zip_path(path))) return;
    char work[4096];
    pthread_mutex_lock(&dc_lock);
    dc_entry *e = entry_of(path, NULL, work, sizeof work, NULL);
    if (e) {
        e->writers += delta;
        if (e->writers < 0) e->writers = 0;
        e->stat_valid = 0;
    }
    pthread_mutex_unlock(&dc_lock);
}

void dircache_renamed(const char *from, const char *to)
{
    if (!from || !to || (!dircache_active() && !s2_zip_path(from) && !s2_zip_path(to))) return;
    char work[4096];
    dc_dir *d = NULL;
    pthread_mutex_lock(&dc_lock);
    dc_entry *e = entry_of(from, &d, work, sizeof work, NULL);
    int is_dir = e ? (e->stat_valid ? S_ISDIR(e->st.st_mode) : e->type == DT_DIR) : 0;
    int unknown = !e || e->type == DT_UNKNOWN;
    if (d && e) remove_entry(d, e);
    drop_tree(from);
    pthread_mutex_unlock(&dc_lock);
    if (unknown) {
        struct stat st;
        is_dir = !stat(to, &st) && S_ISDIR(st.st_mode);
    }
    pthread_mutex_lock(&dc_lock);
    drop_tree(to);
    pthread_mutex_unlock(&dc_lock);
    /* The moved folder's own listing is read again when it is next used. */
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", to);
    pthread_mutex_lock(&dc_lock);
    dc_entry *t = entry_of(copy, &d, work, sizeof work, NULL);
    if (d) {
        char *leaf = strrchr(copy, '/');
        if (!t && leaf) t = add_entry(d, leaf + 1, is_dir ? DT_DIR : DT_REG);
        if (t) { t->type = is_dir ? DT_DIR : DT_REG; t->stat_valid = 0; }
    }
    pthread_mutex_unlock(&dc_lock);
}
