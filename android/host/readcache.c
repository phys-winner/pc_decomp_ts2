/* File blocks in memory: the game's small reads, and its small writes until
 * they are written back (the job of Windows' cache manager).
 *
 * Reads. The game reads its packages in hundreds of thousands of small
 * pieces (a DBPF index entry, a resource header, a 500-byte resource): one
 * start plus the first load of a town made 385,000 ReadFile calls, 96% of
 * them 4 KB or less, mostly SetFilePointer + ReadFile pairs scattered over a
 * few files (objects.package alone: 121,000). On Android's shared storage
 * each is a FUSE request (~20-75 us with the machine hand-over around it).
 * Simulated on a trace of that run, 32 KB blocks in a 32 MB cache answer 92%
 * of the reads of game files from memory.
 *
 * Writes. The DBPF writer saves a package as: write a header, seek ahead,
 * ask the position, seek back, write a resource; and it reads back what it
 * wrote. That was 49,000 writes over the same run, each a FUSE round trip
 * (~0.28 ms). Written bytes stay in their blocks (dirty) and go to the file
 * in runs of adjacent blocks: when a handle that wrote them is closed or
 * flushed, before anything looks at the file another way (a big read, a
 * mapping, its times, a copy, a rename), at 16 MB dirty, 2 s after the
 * first dirty byte (a background thread), at exit and when the activity
 * pauses. The simulation: 49,000 writes become ~4,700.
 *
 * Before each resource it copies, the writer also makes the file longer with
 * SetEndOfFile up to the resource's end (31,000 times in a first town load),
 * then writes the resource there. A longer file stays in memory too: zeros up
 * to the new end, which is made on disk at write-back, so the pair costs no
 * FUSE request (it was a write-back plus an ftruncate, ~1.1 ms).
 *
 * Blocks are keyed by the host path's hash and the path's write generation:
 * a write past the cache, a truncation, creation or deletion of the path
 * bumps it (dirty blocks are written back first), a rename drops the whole
 * cache, so a block of an older version of a file is never served.
 * S2_READCACHE=0 (or <data>/noreadcache) turns all of it off. */
#define _GNU_SOURCE
#include "host.h"
#include "readcache.h"
#include "iostat.h"
#include "dircache.h"
#include "../texconv/s2tc.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define RC_SHIFT 15u
#define RC_BLOCK (1u << RC_SHIFT)                /* 32 KB */
#ifndef RC_SLOTS                                 /* (tests shrink it) */
#define RC_SLOTS 1024u                           /* 32 MB */
#endif
#define RC_INDEX (RC_SLOTS * 2u)
#define RC_GENS 4096u
#define RC_EMPTY 0u                              /* index entries: slot + 1, or these */
#define RC_DELETED 0xFFFFFFFFu
#define RC_FILES 32u                             /* files with dirty blocks at once */
#ifndef RC_DIRTY_CAP
#define RC_DIRTY_CAP (16u << 20)
#endif
#define RC_DIRTY_AGE_NS 2000000000ull
#define RC_RUN_IOV 64u

typedef struct {
    uint64_t key;                                /* path hash; 0 free */
    uint32_t block, gen;
    uint32_t len;                                /* loaded: the file's bytes in the block (< RC_BLOCK: it ends here) */
    uint32_t lo, hi;                             /* dirty bytes [lo, hi); lo == hi: clean */
    uint8_t loaded;                              /* data[0, len) is the file; otherwise only [lo, hi) is known */
    uint8_t referenced;
    uint8_t *data;
} rc_slot;

typedef struct {                                 /* a file with dirty blocks */
    uint64_t key;                                /* 0 free */
    int fd;                                      /* a duplicate of a writer's descriptor */
    off_t size;                                  /* its size with the dirty bytes */
    off_t disk;                                  /* its size on disk (only write-back changes it) */
    char *path;
} rc_file;

static rc_slot slots[RC_SLOTS];
static uint32_t index_of[RC_INDEX];              /* open addressing over slots */
static uint32_t tombstones;
static uint32_t clock_hand;
static pthread_mutex_t rc_lock = PTHREAD_MUTEX_INITIALIZER;
static int rc_state;                             /* 0 unknown, 1 on, 2 off */
/* Write generations of paths, by hash (a collision only costs misses). */
static uint32_t gens[RC_GENS];
static rc_file dirty_files[RC_FILES];
static uint64_t dirty_bytes, dirty_since;
static int flusher_started;
static uint64_t rc_hits, rc_misses, rc_writes, rc_runs;
/* In-flight disk reads can precede a write-back, including full blocks. */
static uint64_t writebacks;

uint64_t void_readcache_key(const char *host_path)
{
    uint64_t k = s2tc_xxh64(host_path, strlen(host_path), 0x52434b31u);
    return k ? k : 1;
}

static int active(void)
{
    if (!rc_state) {
        const char *env = getenv("S2_READCACHE");
        char flag[1200];
        snprintf(flag, sizeof flag, "%s/noreadcache", void_data_dir());
        rc_state = (env && *env == '0') || !access(flag, F_OK) ? 2 : 1;
        void_log("[files] block cache %s (%u KB blocks, %u MB, write-back up to %u MB)", rc_state == 1 ? "on" : "off",
            RC_BLOCK >> 10, (RC_BLOCK >> 10) * RC_SLOTS >> 10, RC_DIRTY_CAP >> 20);
    }
    return rc_state == 1;
}

int void_readcache_active(void) { return active(); }

static uint32_t gen_of(uint64_t key) { return gens[key & (RC_GENS - 1)]; }

static uint32_t hash_slot(uint64_t key, uint32_t block)
{
    uint64_t h = key ^ ((uint64_t)block * 0x9E3779B97F4A7C15ull);
    h ^= h >> 31;
    return (uint32_t)h & (RC_INDEX - 1);
}

static void index_put(uint32_t s)
{
    for (uint32_t i = hash_slot(slots[s].key, slots[s].block), n = 0; n < RC_INDEX; i = (i + 1) & (RC_INDEX - 1), ++n) {
        if (index_of[i] == RC_EMPTY || index_of[i] == RC_DELETED) {
            if (index_of[i] == RC_DELETED) --tombstones;
            index_of[i] = s + 1;
            return;
        }
    }
}

/* Deleted entries only end at a rebuild: probes stop at empty ones. */
static void index_rebuild(void)
{
    memset(index_of, 0, sizeof index_of);
    tombstones = 0;
    for (uint32_t s = 0; s < RC_SLOTS; ++s)
        if (slots[s].key) index_put(s);
}

/* Frees the slot of index entry i (it must be clean). */
static void index_drop(uint32_t i)
{
    slots[index_of[i] - 1].key = 0;
    index_of[i] = RC_DELETED;
    if (++tombstones > RC_INDEX / 4) index_rebuild();
}

static void unindex(uint32_t s)
{
    for (uint32_t i = hash_slot(slots[s].key, slots[s].block), n = 0; n < RC_INDEX; i = (i + 1) & (RC_INDEX - 1), ++n) {
        if (index_of[i] == RC_EMPTY) break;
        if (index_of[i] == s + 1) { index_drop(i); return; }
    }
    slots[s].key = 0;
}

/* The slot holding (key, block) at the current generation, or -1; a clean
 * block of an older generation met on the way is freed (dirty blocks are
 * written back before a generation changes). rc_lock held. */
static int find(uint64_t key, uint32_t block)
{
    uint32_t gen = gen_of(key);
    for (uint32_t i = hash_slot(key, block), n = 0; n < RC_INDEX; i = (i + 1) & (RC_INDEX - 1), ++n) {
        uint32_t e = index_of[i];
        if (e == RC_EMPTY) return -1;
        if (e == RC_DELETED) continue;
        rc_slot *s = &slots[e - 1];
        if (s->key != key || s->block != block) continue;
        if (s->gen == gen) return (int)(e - 1);
        if (s->lo == s->hi) index_drop(i);
        return -1;                               /* one entry per (key, block) */
    }
    return -1;
}

/* ------------------------------------------------------------ write-back */
static rc_file *file_entry(uint64_t key)
{
    for (uint32_t i = 0; i < RC_FILES; ++i)
        if (dirty_files[i].key == key) return &dirty_files[i];
    return NULL;
}

static void writeback(uint64_t key);

/* A new write generation for `key`. Generations are shared by keys with the
 * same low bits, so every dirty file sharing it is written back first: a
 * dirty block must never turn stale. rc_lock held. */
static void bump(uint64_t key)
{
    uint32_t g = (uint32_t)key & (RC_GENS - 1);
    for (uint32_t i = 0; i < RC_FILES; ++i)
        if (dirty_files[i].key && ((uint32_t)dirty_files[i].key & (RC_GENS - 1)) == g) writeback(dirty_files[i].key);
    ++gens[g];
}

static int by_block(const void *a, const void *b)
{
    uint32_t x = slots[*(const uint32_t *)a].block, y = slots[*(const uint32_t *)b].block;
    return x < y ? -1 : x > y;
}

/* pwritev until done: 0, or -1 with errno set. */
static int write_run(int fd, struct iovec *iov, int count, off_t at)
{
    while (count > 0) {
        ssize_t w = pwritev(fd, iov, count, at);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) { errno = ENOSPC; return -1; }
        at += w;
        while (count > 0 && (size_t)w >= iov->iov_len) { w -= (ssize_t)iov->iov_len; ++iov; --count; }
        if (count > 0) { iov->iov_base = (uint8_t *)iov->iov_base + w; iov->iov_len -= (size_t)w; }
    }
    return 0;
}

/* Write the dirty blocks of `key` to its file, adjacent blocks in one call,
 * and make the file as long as SetEndOfFile left it. rc_lock held; the
 * machine need not be (it touches no guest state). A failed write is logged
 * and its blocks are dropped, like a failed lazy write on Windows. */
static void writeback(uint64_t key)
{
    rc_file *e = file_entry(key);
    if (!e) return;
    ++writebacks;
    static uint32_t list[RC_SLOTS];
    uint32_t n = 0;
    for (uint32_t s = 0; s < RC_SLOTS; ++s)
        if (slots[s].key == key && slots[s].lo < slots[s].hi) list[n++] = s;
    qsort(list, n, sizeof list[0], by_block);
    VSTAT_BEGIN(start);
    uint64_t bytes = 0;
    int failed = 0;
    off_t end = e->disk;                         /* the file's end on disk after the runs */
    for (uint32_t i = 0; i < n && !failed;) {
        struct iovec iov[RC_RUN_IOV];
        int count = 0;
        off_t at = ((off_t)slots[list[i]].block << RC_SHIFT) + slots[list[i]].lo;
        for (;;) {
            rc_slot *s = &slots[list[i]];
            iov[count].iov_base = s->data + s->lo;
            iov[count].iov_len = s->hi - s->lo;
            bytes += s->hi - s->lo;
            ++count;
            ++i;
            off_t stop = ((off_t)s->block << RC_SHIFT) + s->hi;
            if (stop > end) end = stop;
            if (i >= n || count == (int)RC_RUN_IOV) break;
            rc_slot *t = &slots[list[i]];
            if (s->hi != RC_BLOCK || t->lo != 0 || t->block != s->block + 1) break;
        }
        ++rc_runs;
        if (write_run(e->fd, iov, count, at)) {
            void_log("[files] write-back of %s at %lld failed: %s", e->path ? e->path : "?", (long long)at, strerror(errno));
            failed = 1;
        }
    }
    if (!failed && e->size > end && ftruncate(e->fd, e->size)) {
        void_log("[files] write-back of %s: setting its size %lld failed: %s", e->path ? e->path : "?",
            (long long)e->size, strerror(errno));
        failed = 1;
    }
    VSTAT_END(start, VS_WRITE, bytes);
    for (uint32_t i = 0; i < n; ++i) {
        rc_slot *s = &slots[list[i]];
        dirty_bytes -= s->hi - s->lo;
        s->lo = s->hi = 0;
        if (!s->loaded) unindex(list[i]);        /* holds nothing else */
    }
    if (e->path) dircache_changed(e->path);
    close(e->fd);
    free(e->path);
    memset(e, 0, sizeof *e);
    if (!dirty_bytes) {                          /* (a file made longer only waits too) */
        uint32_t i = 0;
        while (i < RC_FILES && !dirty_files[i].key) ++i;
        if (i == RC_FILES) dirty_since = 0;
    }
    if (failed) bump(key);                       /* the loaded blocks no longer match the file */
}

static void writeback_all(void)
{
    for (uint32_t i = 0; i < RC_FILES; ++i)
        if (dirty_files[i].key) writeback(dirty_files[i].key);
}

static void *flusher(void *arg)
{
    (void)arg;
    cpu_set_t all;                               /* not the guest's big core */
    CPU_ZERO(&all);
    long cores = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < cores && i < CPU_SETSIZE; ++i) CPU_SET(i, &all);
    if (cores > 0) sched_setaffinity(0, sizeof all, &all);
    for (;;) {
        struct timespec ts = { 0, 500000000 };
        while (nanosleep(&ts, &ts)) {}
        pthread_mutex_lock(&rc_lock);
        if (dirty_since && void_stat_now() - dirty_since > RC_DIRTY_AGE_NS) writeback_all();
        pthread_mutex_unlock(&rc_lock);
    }
    return NULL;
}

/* The dirty-file entry of `key`, made from the writer's descriptor. rc_lock held. */
static rc_file *ensure_entry(uint64_t key, int fd, const char *path)
{
    rc_file *e = file_entry(key);
    if (e) return e;
    for (uint32_t i = 0; i < RC_FILES && !e; ++i)
        if (!dirty_files[i].key) e = &dirty_files[i];
    if (!e) { writeback_all(); e = &dirty_files[0]; }
    struct stat st;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0 || fstat(copy, &st)) {
        if (copy >= 0) close(copy);
        return NULL;
    }
    e->key = key;
    e->fd = copy;
    e->size = e->disk = st.st_size;
    e->path = path ? strdup(path) : NULL;
    if (!flusher_started) {
        flusher_started = 1;
        pthread_t t;
        if (!pthread_create(&t, NULL, flusher, NULL)) pthread_detach(t);
    }
    return e;
}

/* ---------------------------------------------------------------- blocks */
/* A slot for a new block (CLOCK replacement; a dirty victim's file is
 * written back first). rc_lock held. */
static int take_slot(void)
{
    for (uint32_t tries = 0; tries < 4 * RC_SLOTS; ++tries) {
        uint32_t s = clock_hand;
        clock_hand = (clock_hand + 1) & (RC_SLOTS - 1);
        if (slots[s].key && slots[s].referenced) { slots[s].referenced = 0; continue; }
        if (slots[s].key && slots[s].lo < slots[s].hi) writeback(slots[s].key);
        if (slots[s].key) unindex(s);
        if (!slots[s].data) {
            slots[s].data = (uint8_t *)malloc(RC_BLOCK);
            if (!slots[s].data) return -1;
        }
        memset(&slots[s], 0, offsetof(rc_slot, data));
        return (int)s;
    }
    return -1;
}

static uint32_t loaded_len(rc_slot *s);

/* A write at `from` past the file's end `end` leaves a hole of zeros: the
 * loaded blocks in [end, from) hold zeros up to `from`. rc_lock held. */
static void zero_hole(uint64_t key, off_t end, off_t from)
{
    for (uint32_t i = 0; i < RC_SLOTS; ++i) {
        rc_slot *s = &slots[i];
        if (s->key != key || !s->loaded || s->len == RC_BLOCK || s->gen != gen_of(key)) continue;
        off_t start = (off_t)s->block << RC_SHIFT;
        if (start + (off_t)RC_BLOCK <= end || start >= from) continue;
        off_t to = from - start;
        uint32_t upto = to > (off_t)RC_BLOCK ? RC_BLOCK : (uint32_t)to;
        if (upto > s->len) {
            memset(s->data + s->len, 0, upto - s->len);
            s->len = upto;
        }
    }
}

/* Cache a block read from disk at write generation `gen`, write-back count
 * `epoch`. 1 cached, 0 not (the file changed meanwhile, or no memory), -1
 * read it again (short, and a write-back may have made the file longer on
 * disk: the eviction here can be one). rc_lock held. */
static int insert(uint64_t key, uint32_t block, uint32_t gen, uint64_t epoch, const uint8_t *data, uint32_t len)
{
    if (gen != gen_of(key)) return 0;            /* the file changed meanwhile */
    if (writebacks != epoch) return -1;
    int s = take_slot();
    if (s < 0) return 0;
    if (writebacks != epoch) return -1;           /* the slot stays free */
    memcpy(slots[s].data, data, len);
    slots[s].key = key;
    slots[s].block = block;
    slots[s].gen = gen;
    slots[s].len = len;
    slots[s].loaded = 1;
    slots[s].referenced = 1;
    index_put((uint32_t)s);
    loaded_len(&slots[s]);                       /* a hole up to dirty blocks reads zeros */
    return 1;
}

/* Read the rest of a block known only by its dirty bytes. rc_lock held (and
 * the machine: rare, so not released). 0, or -1 with errno set. */
static int fill(rc_slot *s, int fd)
{
    uint8_t *data = (uint8_t *)malloc(RC_BLOCK);
    if (!data) { errno = ENOMEM; return -1; }
    VSTAT_BEGIN(start);
    ssize_t got = 0;
    while ((uint32_t)got < RC_BLOCK) {
        ssize_t r = pread(fd, data + got, RC_BLOCK - (size_t)got, ((off_t)s->block << RC_SHIFT) + got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r < 0) got = -1; break; }
        got += r;
    }
    VSTAT_END(start, VS_READ, got > 0 ? (uint64_t)got : 0);
    if (got < 0) { int e = errno; free(data); errno = e; return -1; }
    memset(data + got, 0, RC_BLOCK - (size_t)got);
    memcpy(data + s->lo, s->data + s->lo, s->hi - s->lo);
    free(s->data);
    s->data = data;
    s->len = (uint32_t)got > s->hi ? (uint32_t)got : s->hi;
    s->loaded = 1;
    loaded_len(s);
    return 0;
}

/* The bytes a loaded block holds: past the file's end on disk, a file that
 * dirty blocks made longer reads zeros (a hole) up to its new size. */
static uint32_t loaded_len(rc_slot *s)
{
    if (s->len < RC_BLOCK) {
        rc_file *e = file_entry(s->key);
        off_t end = e ? e->size - ((off_t)s->block << RC_SHIFT) : 0;
        if (end > (off_t)s->len) {
            uint32_t to = end > (off_t)RC_BLOCK ? RC_BLOCK : (uint32_t)end;
            memset(s->data + s->len, 0, to - s->len);
            s->len = to;
        }
    }
    return s->len;
}

/* Read `size` bytes at `offset` of `fd` (whose host path hashes to `key`)
 * into `out` through the cache. Returns the bytes read (short only at the
 * end of the file), -1 with errno, -2 when the read is not cached, or -3
 * when `fd` is -1 and a block is not in memory. A missing block is read
 * with the machine released (the caller holds it). */
ssize_t void_readcache_pread(int fd, uint64_t key, void *out, size_t size, off_t offset)
{
    if (!key || size > 2 * RC_BLOCK || offset < 0 || !active()) return -2;
    uint8_t *dst = (uint8_t *)out;
    size_t done = 0;
    uint8_t *buffer = NULL;
    while (done < size) {
        off_t at = offset + (off_t)done;
        uint32_t block = (uint32_t)(at >> RC_SHIFT), within = (uint32_t)(at & (RC_BLOCK - 1));
        uint32_t want = size - done < RC_BLOCK - within ? (uint32_t)(size - done) : RC_BLOCK - within;
        pthread_mutex_lock(&rc_lock);
        int found = find(key, block);
        if (found >= 0) {
            rc_slot *s = &slots[found];
            if (!s->loaded && !(within >= s->lo && within + want <= s->hi)) {
                if (fd < 0) { pthread_mutex_unlock(&rc_lock); free(buffer); return -3; }
                if (fill(s, fd)) {
                    int e = errno;
                    pthread_mutex_unlock(&rc_lock);
                    free(buffer);
                    errno = e;
                    return done ? (ssize_t)done : -1;
                }
            }
            s->referenced = 1;
            uint32_t len = s->loaded ? loaded_len(s) : s->hi;
            uint32_t n = within < len ? len - within : 0;
            if (n > want) n = want;
            memcpy(dst + done, s->data + within, n);
            ++rc_hits;
            pthread_mutex_unlock(&rc_lock);
            done += n;
            if (n < want) break;                                /* end of file */
            continue;
        }
        if (fd < 0) { pthread_mutex_unlock(&rc_lock); free(buffer); return -3; }
        uint32_t gen = gen_of(key);
        uint64_t epoch = writebacks;
        ++rc_misses;
        pthread_mutex_unlock(&rc_lock);
        if (!buffer) buffer = (uint8_t *)malloc(RC_BLOCK);
        if (!buffer) return done ? (ssize_t)done : -2;
        ssize_t got = 0;
        void_blocking_begin();
        while ((uint32_t)got < RC_BLOCK) {                      /* a short read is the end of the file */
            ssize_t r = pread(fd, buffer + got, RC_BLOCK - (size_t)got, ((off_t)block << RC_SHIFT) + got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { if (r < 0) got = -1; break; }
            got += r;
        }
        int e = errno;
        void_blocking_end();
        if (got < 0) { free(buffer); errno = e; return done ? (ssize_t)done : -1; }
        pthread_mutex_lock(&rc_lock);
        /* A replacement can leave fd on the old inode: read it uncached, never under the new generation. */
        if (gen != gen_of(key)) {
            pthread_mutex_unlock(&rc_lock);
            free(buffer);
            return -2;
        }
        if (find(key, block) >= 0 || insert(key, block, gen, epoch, buffer, (uint32_t)got)) {
            pthread_mutex_unlock(&rc_lock);
            continue;                                           /* served from the slot, or read again */
        }
        /* Not cached: a file made longer by dirty blocks reads zeros past its end on disk. */
        rc_file *grown = file_entry(key);
        off_t end = grown ? grown->size - ((off_t)block << RC_SHIFT) : 0;
        if (end > got) {
            uint32_t to = end > (off_t)RC_BLOCK ? RC_BLOCK : (uint32_t)end;
            memset(buffer + got, 0, to - (uint32_t)got);
            got = to;
        }
        pthread_mutex_unlock(&rc_lock);
        uint32_t n = within < (uint32_t)got ? (uint32_t)got - within : 0;
        if (n > want) n = want;
        memcpy(dst + done, buffer + within, n);
        done += n;
        if (n < want) break;
    }
    free(buffer);
    return (ssize_t)done;
}

/* Write `size` bytes at `offset` of the file of `fd` (host path `path`,
 * hashed to `key`) into its blocks. Returns `size`, -1 with errno, or -2
 * when the write is not cached (the caller writes the file itself, after
 * void_readcache_flush_key). */
ssize_t void_readcache_pwrite(int fd, uint64_t key, const char *path, const void *in, size_t size, off_t offset)
{
    if (!key || !size || size > 2 * RC_BLOCK || offset < 0 || fd < 0 || !active()) return -2;
    const uint8_t *src = (const uint8_t *)in;
    size_t done = 0;
    pthread_mutex_lock(&rc_lock);
    if (!ensure_entry(key, fd, path)) { pthread_mutex_unlock(&rc_lock); return -2; }
    while (done < size) {
        off_t at = offset + (off_t)done;
        uint32_t block = (uint32_t)(at >> RC_SHIFT), within = (uint32_t)(at & (RC_BLOCK - 1));
        uint32_t n = size - done < RC_BLOCK - within ? (uint32_t)(size - done) : RC_BLOCK - within;
        int found = find(key, block);
        if (found < 0) {
            found = take_slot();
            if (found < 0) { pthread_mutex_unlock(&rc_lock); errno = ENOMEM; return -1; }
            slots[found].key = key;
            slots[found].block = block;
            slots[found].gen = gen_of(key);
            index_put((uint32_t)found);
        }
        rc_slot *s = &slots[found];
        if (!s->loaded && s->lo < s->hi && (within > s->hi || within + n < s->lo)) {
            /* Apart from the dirty bytes of a block whose other bytes are
             * unknown: write those first, then start over with this one. */
            writeback(key);
            continue;
        }
        /* After take_slot or writeback the file may have no entry. */
        rc_file *e = ensure_entry(key, fd, path);
        if (!e) { pthread_mutex_unlock(&rc_lock); errno = EIO; return -1; }
        if (!s->loaded && s->lo == s->hi && ((off_t)block << RC_SHIFT) >= e->disk) {
            /* Past the end on disk a new block holds zeros up to the file's
             * size: known in full, so later writes anywhere in it stay here. */
            s->loaded = 1;
            s->len = 0;
            loaded_len(s);
        }
        if (at > e->size) zero_hole(key, e->size, at);
        s->referenced = 1;
        if (s->loaded && within > s->len) memset(s->data + s->len, 0, within - s->len);
        memcpy(s->data + within, src + done, n);
        uint32_t lo = within, hi = within + n;
        if (s->lo < s->hi) {
            if (s->lo < lo) lo = s->lo;
            if (s->hi > hi) hi = s->hi;
        }
        dirty_bytes += (hi - lo) - (s->hi - s->lo);
        s->lo = lo;
        s->hi = hi;
        if (s->loaded) { if (within + n > s->len) s->len = within + n; }
        else s->len = s->hi;
        off_t end = ((off_t)block << RC_SHIFT) + within + n;
        if (end > e->size) e->size = end;
        if (!dirty_since) dirty_since = void_stat_now();
        done += n;
    }
    ++rc_writes;
    if (dirty_bytes > RC_DIRTY_CAP) writeback_all();
    pthread_mutex_unlock(&rc_lock);
    return (ssize_t)size;
}

int void_readcache_set_end(int fd, uint64_t key, const char *path, off_t size)
{
    if (!key || fd < 0 || size < 0 || !active()) return 1;
    pthread_mutex_lock(&rc_lock);
    rc_file *e = file_entry(key);
    off_t now;
    struct stat st;
    if (e) now = e->size;
    else if (!fstat(fd, &st)) now = st.st_size;
    else { pthread_mutex_unlock(&rc_lock); return 1; }
    if (size != now) {
        if (size < now || !(e = ensure_entry(key, fd, path))) { pthread_mutex_unlock(&rc_lock); return 1; }
        zero_hole(key, e->size, size);
        e->size = size;
        if (!dirty_since) dirty_since = void_stat_now();
    }
    pthread_mutex_unlock(&rc_lock);
    return 0;
}

int void_readcache_size(uint64_t key, off_t *size)
{
    if (!key || rc_state != 1) return 0;
    pthread_mutex_lock(&rc_lock);
    rc_file *e = file_entry(key);
    if (e) *size = e->size;
    pthread_mutex_unlock(&rc_lock);
    return e != NULL;
}

void void_readcache_flush_key(uint64_t key)
{
    if (!key || rc_state != 1) return;
    pthread_mutex_lock(&rc_lock);
    writeback(key);
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_flush_path(const char *host_path)
{
    if (host_path && rc_state == 1) void_readcache_flush_key(void_readcache_key(host_path));
}

void void_readcache_flush_dir(const char *host_dir)
{
    if (!host_dir || rc_state != 1) return;
    size_t n = strlen(host_dir);
    while (n && host_dir[n - 1] == '/') --n;
    pthread_mutex_lock(&rc_lock);
    for (uint32_t i = 0; i < RC_FILES; ++i) {
        const char *p = dirty_files[i].path;
        if (dirty_files[i].key && (!p || (!strncmp(p, host_dir, n) && p[n] == '/')))
            writeback(dirty_files[i].key);
    }
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_flush_all(void)
{
    if (rc_state != 1) return;
    pthread_mutex_lock(&rc_lock);
    writeback_all();
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_changed(const char *host_path)
{
    if (host_path) void_readcache_changed_key(void_readcache_key(host_path));
}

void void_readcache_changed_key(uint64_t key)
{
    pthread_mutex_lock(&rc_lock);
    writeback(key);
    bump(key);
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_clear(void)
{
    pthread_mutex_lock(&rc_lock);
    writeback_all();
    for (uint32_t i = 0; i < RC_GENS; ++i) ++gens[i];  /* in-flight reads must not repopulate renamed paths */
    for (uint32_t i = 0; i < RC_SLOTS; ++i) slots[i].key = 0;
    memset(index_of, 0, sizeof index_of);
    tombstones = 0;
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_stats(uint64_t *hits, uint64_t *misses)
{
    pthread_mutex_lock(&rc_lock);
    *hits = rc_hits;
    *misses = rc_misses;
    pthread_mutex_unlock(&rc_lock);
}

void void_readcache_write_stats(uint64_t *writes, uint64_t *runs)
{
    pthread_mutex_lock(&rc_lock);
    *writes = rc_writes;
    *runs = rc_runs;
    pthread_mutex_unlock(&rc_lock);
}
