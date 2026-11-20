/* Files: Windows paths over host directories, file handles, directory
 * enumeration and file mappings.
 *
 * The guest sees one drive. "C:\Games\The Sims 2 Legacy" is the read-mostly game
 * installation; profile folders and the rest of C: are writable directories
 * under the host data root. Every component is resolved case-insensitively,
 * as NTFS lookups are. */
#define _GNU_SOURCE
#include "host.h"
#include "iostat.h"
#include "dircache.h"
#include "readcache.h"
#include "zip_files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>

#define WIN_MAX_PATH 260

typedef struct { const char *win; char host[1024]; } mount_t;
static mount_t mounts[8];
static int mount_count;
static char game_root[1024], data_root[1024];
/* <data>/pathtrace present at start: every resolution with its time (diagnostics). */
static FILE *path_trace;
/* <data>/iotrace present at start: every file call (diagnostics):
 *   O handle access disposition us path | R/W handle size offset(-1: position) position us | S handle distance method result | C handle */
static FILE *io_trace;
static pthread_mutex_t io_trace_lock = PTHREAD_MUTEX_INITIALIZER;
#define IOTRACE(...) do { if (io_trace) { pthread_mutex_lock(&io_trace_lock); fprintf(io_trace, __VA_ARGS__); \
    pthread_mutex_unlock(&io_trace_lock); } } while (0)
static char cwd[WIN_MAX_PATH] = "C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin";
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

const char *void_root_dir(void) { return game_root; }
const char *void_data_dir(void) { return data_root; }

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    }
    mkdir(tmp, 0775);
}

static void add_mount(const char *win, const char *host_fmt, const char *base)
{
    mount_t *m = &mounts[mount_count++];
    m->win = win;
    snprintf(m->host, sizeof m->host, host_fmt, base);
    mkdirs(m->host);
}

int void_files_init(const char *game, const char *data)
{
    snprintf(game_root, sizeof game_root, "%s", game);
    snprintf(data_root, sizeof data_root, "%s", data);
    if (!s2_zip_init(game)) return 0;
    /* Longest prefixes first. */
    add_mount("C:\\Users\\Player\\AppData\\Local\\Temp", "%s/Temp", data);
    add_mount("C:\\Users\\Player\\Documents", "%s/Documents", data);
    add_mount("C:\\Users\\Player\\AppData", "%s/AppData", data);
    add_mount("C:\\Users\\Public\\Documents", "%s/PublicDocuments", data);
    add_mount("C:\\ProgramData", "%s/ProgramData", data);
    add_mount("C:\\Games\\The Sims 2 Legacy", "%s", game);
    add_mount("C:", "%s/drive_c", data);
    /* The drive root lists what a Windows C: has above the mounts (the game
     * walks a path from the root, testing each directory by enumerating it). */
    static const char *const root_dirs[] = {"Games", "ProgramData", "Users", "Users/Player", "Users/Public", "Windows"};
    for (size_t i = 0; i < sizeof root_dirs / sizeof root_dirs[0]; ++i) {
        char path[1200];
        snprintf(path, sizeof path, "%s/drive_c/%s", data, root_dirs[i]);
        mkdirs(path);
    }
    struct stat st;
    if (stat(game_root, &st) || !S_ISDIR(st.st_mode)) {
        void_log("[void] game root %s is missing", game_root);
        return 0;
    }
    char trace[1100];
    snprintf(trace, sizeof trace, "%s/iotrace", data);
    if (!access(trace, F_OK)) {
        snprintf(trace, sizeof trace, "%s/iotrace.log", data);
        io_trace = fopen(trace, "w");
        if (io_trace) setvbuf(io_trace, NULL, _IOFBF, 1 << 18);
    }
    snprintf(trace, sizeof trace, "%s/pathtrace", data);
    if (!access(trace, F_OK)) {
        snprintf(trace, sizeof trace, "%s/pathtrace.log", data);
        path_trace = fopen(trace, "w");
        if (path_trace) setvbuf(path_trace, NULL, _IOFBF, 1 << 16);
    }
    return 1;
}

/* Port overlays: a file under the overlay directory replaces the game's file
 * at the same path below the game root (tools/build_overlay.py: the UI
 * package with the port's settings). Exact file paths only: a directory
 * listing still shows the original, which has the same name. */
typedef struct { char win[600]; char host[1100]; } overlay_t;
static overlay_t overlays[64];
static int overlay_count;

static void scan_overlay(const char *host_dir, const char *win_dir)
{
    DIR *d = opendir(host_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char host[1100], win[600];
        snprintf(host, sizeof host, "%s/%s", host_dir, e->d_name);
        snprintf(win, sizeof win, "%s\\%s", win_dir, e->d_name);
        struct stat st;
        if (stat(host, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_overlay(host, win);
        } else if (overlay_count < (int)(sizeof overlays / sizeof overlays[0])) {
            snprintf(overlays[overlay_count].win, sizeof overlays[0].win, "%s", win);
            snprintf(overlays[overlay_count].host, sizeof overlays[0].host, "%s", host);
            ++overlay_count;
            void_log("[void] overlay %s -> %s", win, host);
        }
    }
    closedir(d);
}

void void_files_overlay(const char *dir) { scan_overlay(dir, "C:\\Games\\The Sims 2 Legacy"); }

const char *void_overlay_path(const char *windows_path)
{
    for (int i = 0; i < overlay_count; ++i)
        if (!strcasecmp(windows_path, overlays[i].win)) return overlays[i].host;
    return NULL;
}

/* Normalize a Windows path to "C:\a\b" (no trailing separator, no . or ..).
 * Returns 0 if it cannot be represented. */
static int normalize(const char *in, char *out, size_t cap)
{
    char work[1024];
    size_t n = 0;
    const char *p = in;
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') {
            snprintf(work, sizeof work, "C:%s", p + 2);
        } else {
            pthread_mutex_lock(&cwd_lock);
            snprintf(work, sizeof work, "%s\\%s", cwd, p + 2);
            pthread_mutex_unlock(&cwd_lock);
        }
    } else if (p[0] == '\\' || p[0] == '/') {
        snprintf(work, sizeof work, "C:%s", p);
    } else {
        pthread_mutex_lock(&cwd_lock);
        snprintf(work, sizeof work, "%s\\%s", cwd, p);
        pthread_mutex_unlock(&cwd_lock);
    }
    /* Split and rebuild. */
    char *parts[128];
    int count = 0;
    for (char *s = work; *s; ++s) if (*s == '/') *s = '\\';
    char *save = NULL;
    char *tok = strtok_r(work + 2, "\\", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !*tok) {
        } else if (!strcmp(tok, "..")) {
            if (count) --count;
        } else {
            /* Windows strips trailing dots and spaces from components. */
            size_t len = strlen(tok);
            while (len > 1 && (tok[len - 1] == '.' || tok[len - 1] == ' ')) tok[--len] = 0;
            if (count == 128) return 0;
            parts[count++] = tok;
        }
        tok = strtok_r(NULL, "\\", &save);
    }
    n = (size_t)snprintf(out, cap, "C:");
    for (int i = 0; i < count; ++i) {
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "\\%s", parts[i]);
        if (n >= cap) return 0;
    }
    if (count == 0) snprintf(out, cap, "C:\\");
    return 1;
}

/* Append component `name` to host path `base`, matching an existing entry
 * case-insensitively. Returns 1 if the entry exists (*type: its DT_* type,
 * DT_UNKNOWN when not known), 0 if not, -1 when `cached_only` and its folder
 * has no cached listing yet (dircache.c). */
static int resolve_component(char *base, size_t cap, const char *name, unsigned char *type, int cached_only)
{
    size_t len = strlen(base);
    char actual[512];
    int cached = dircache_lookup(len ? base : "/", name, actual, sizeof actual, type, cached_only);
    if (cached == DC_FOUND) { snprintf(base + len, cap - len, "/%s", actual); return 1; }
    if (cached == DC_ABSENT) { snprintf(base + len, cap - len, "/%s", name); return 0; }
    if (cached == DC_NOT_LOADED) return -1;
    *type = DT_UNKNOWN;
    snprintf(base + len, cap - len, "/%s", name);
    struct stat st;
    if (!lstat(base, &st)) return 1;
    base[len] = 0;
    DIR *d = opendir(len ? base : "/");
    int found = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, name)) {
                snprintf(base + len, cap - len, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
    }
    if (!found) snprintf(base + len, cap - len, "/%s", name);
    return found;
}

/* Windows path (UTF-8) -> host path. *exists_parent reports whether every
 * component but the last exists (ERROR_PATH_NOT_FOUND vs FILE_NOT_FOUND). */
static char *to_host_uncounted(const char *windows_path, int *parent_exists, char *normalized_out, int *type);
static pthread_mutex_t path_trace_lock = PTHREAD_MUTEX_INITIALIZER;
/* *type (optional): the final component's DT_* type, DT_UNKNOWN when it
 * exists but its type is not known, -1 when it does not exist. */
static char *to_host_typed(const char *windows_path, int *parent_exists, char *normalized_out, int *type)
{
    VSTAT_BEGIN(start);
    int ok = 0, kind = -1;
    char *host = to_host_uncounted(windows_path, &ok, normalized_out, &kind);
    if (parent_exists) *parent_exists = ok;
    if (type) *type = kind;
    uint64_t ns = void_stat_now() - start;
    void_stat_add(VS_PATH, ns, 0);
    if (path_trace) {
        struct stat st;
        int exists = host && !lstat(host, &st);
        pthread_mutex_lock(&path_trace_lock);
        fprintf(path_trace, "%llu %d %d %s\n", (unsigned long long)(ns / 1000), ok, exists, windows_path ? windows_path : "(null)");
        pthread_mutex_unlock(&path_trace_lock);
    }
    return host;
}

static char *to_host(const char *windows_path, int *parent_exists, char *normalized_out)
{
    return to_host_typed(windows_path, parent_exists, normalized_out, NULL);
}

/* One pass over the components; -1 when `cached_only` met an uncached folder. */
static int walk_components(char *host, const char *rest, int cached_only, int *ok_out, int *type)
{
    int ok = 1, found = 1;
    unsigned char last_type = DT_DIR;     /* the mount's own folder */
    char component[512];
    while (*rest) {
        while (*rest == '\\') ++rest;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\' && k < sizeof component - 1) { component[k] = rest[k]; ++k; }
        component[k] = 0;
        rest += k;
        int last = !*rest || (rest[0] == '\\' && !rest[1]);
        if (!ok) {
            size_t len = strlen(host);
            snprintf(host + len, 2048 - len, "/%s", component);
            found = 0;
        } else {
            unsigned char t = DT_UNKNOWN;
            int exists = resolve_component(host, 2048, component, &t, cached_only);
            if (exists < 0) return -1;
            if (!exists && !last) ok = 0;
            found = exists;
            last_type = t;
        }
    }
    *ok_out = ok;
    *type = found ? last_type : -1;
    return 0;
}

static char *to_host_uncounted(const char *windows_path, int *parent_exists, char *normalized_out, int *type)
{
    char norm[1024];
    if (!windows_path || !normalize(windows_path, norm, sizeof norm)) return NULL;
    if (normalized_out) snprintf(normalized_out, 1024, "%s", norm);
    const char *overlay = void_overlay_path(norm);
    if (overlay) {
        if (parent_exists) *parent_exists = 1;
        if (type) *type = DT_REG;
        return strdup(overlay);
    }
    const mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < mount_count; ++i) {
        size_t l = strlen(mounts[i].win);
        if (!strncasecmp(norm, mounts[i].win, l) && (norm[l] == 0 || norm[l] == '\\') && l > best_len) {
            best = &mounts[i];
            best_len = l;
        }
    }
    if (!best) return NULL;
    char *host = (char *)malloc(2048);
    const char *rest = norm + best_len;
    int ok = 1, kind = -1;
    /* Folders already listed answer from memory while holding the machine;
     * any other lookup touches FUSE, which must not hold off the guest audio
     * decoder: then the whole walk runs again with the machine released. */
    snprintf(host, 2048, "%s", best->host);
    if (walk_components(host, rest, 1, &ok, &kind) < 0) {
        snprintf(host, 2048, "%s", best->host);
        void_blocking_begin();
        walk_components(host, rest, 0, &ok, &kind);
        void_blocking_end();
    }
    if (parent_exists) *parent_exists = ok;
    if (type) *type = kind;
    return host;
}

char *void_path_to_host(const char *windows_path) { return to_host(windows_path, NULL, NULL); }

/* The diagnostics traces reach the disk every few seconds (iostat.c), as the process ends by a kill. */
void void_files_trace_flush(void)
{
    if (io_trace) { pthread_mutex_lock(&io_trace_lock); fflush(io_trace); pthread_mutex_unlock(&io_trace_lock); }
    if (path_trace) { pthread_mutex_lock(&path_trace_lock); fflush(path_trace); pthread_mutex_unlock(&path_trace_lock); }
}

uint32_t void_cwd_get(char *out, uint32_t cap)
{
    pthread_mutex_lock(&cwd_lock);
    uint32_t n = (uint32_t)snprintf(out, cap, "%s", cwd);
    pthread_mutex_unlock(&cwd_lock);
    return n;
}

int void_cwd_set(const char *windows_path)
{
    char norm[1024];
    if (!normalize(windows_path, norm, sizeof norm)) return 0;
    char *host = to_host(norm, NULL, NULL);
    struct stat st;
    int ok = host && !dircache_stat(host, &st) && S_ISDIR(st.st_mode);
    free(host);
    if (!ok) return 0;
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", norm);
    pthread_mutex_unlock(&cwd_lock);
    return 1;
}

uint32_t void_errno_to_win32(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND_;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND_;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED_;
    case EEXIST: return ERROR_FILE_EXISTS_;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY_;
    case EBADF: return ERROR_INVALID_HANDLE_;
    case EINVAL: return ERROR_INVALID_PARAMETER_;
    case ENOSPC: return 112u;          /* ERROR_DISK_FULL */
    case ENOTEMPTY: return 145u;       /* ERROR_DIR_NOT_EMPTY */
    default: return ERROR_ACCESS_DENIED_;
    }
}

/* ----------------------------------------------------------- file objects */
typedef struct file_t {
    int fd;
    int std_stream;       /* 0 none, 1 stdin, 2 stdout, 3 stderr */
    int is_dir;
    char *host_path;
    int delete_on_close;
    int writable;         /* counted in the directory cache's writers */
    char line[512];       /* buffered guest console output */
    size_t line_len;
    /* Sequential WriteFile data not yet handed to the file system (see
     * file_flush): the game saves a package in thousands of small writes,
     * each a ~0.25 ms MediaProvider round trip on FUSE. */
    uint8_t *wbuf;
    uint32_t wlen;
    int werr;             /* errno of a failed flush, reported once */
    struct file_t *wnext; /* the dirty list */
    int wlisted;
    uint64_t path_key;    /* readcache.c: the host path's hash (0: no path) */
    int cached_reads;     /* readable: small reads go through readcache.c */
    /* Not opened yet (fd -1): create_file_uncounted defers the open of an
     * existing file to its first read or write (file_open_now). */
    int lazy;
    int lazy_flags;       /* open(2) flags for then */
    off_t lazy_pos;       /* the file position until then */
    int cache_wrote;      /* wrote into readcache.c's blocks: written back when closed */
    const zm_entry *zip_entry;
    struct file_t *file_next;
} file_t;

#define WBUF_CAP (256u << 10)
static file_t *dirty_files;           /* handles holding buffered writes (machine held) */
static file_t *live_files;            /* machine held; identities follow renamed paths */
static pthread_mutex_t zip_copy_lock = PTHREAD_MUTEX_INITIALIZER;

/* Write the buffered data at the descriptor's position: every operation
 * that reads or moves that position flushes first, so it is still where
 * the first buffered byte belongs. Returns 0, or -1 with errno set. */
static int file_flush(file_t *f)
{
    if (!f || !f->wlen) return 0;
    VSTAT_BEGIN(start);
    uint32_t done = 0;
    int rc = 0;
    while (done < f->wlen) {
        ssize_t n = write(f->fd, f->wbuf + done, f->wlen - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            f->werr = errno;
            rc = -1;
            break;
        }
        if (n == 0) { f->werr = ENOSPC; rc = -1; break; }
        done += (uint32_t)n;
    }
    VSTAT_END(start, VS_WRITE, done);
    f->wlen = 0;
    if (done && f->path_key) void_readcache_changed_key(f->path_key);
    if (rc) { errno = f->werr; void_log("[files] buffered write to %s failed: %s", f->host_path ? f->host_path : "?", strerror(f->werr)); }
    return rc;
}

/* Where the next byte goes: the descriptor's position plus the buffered bytes. */
static off_t file_logical_position(file_t *f)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return pos < 0 ? pos : pos + (off_t)f->wlen;
}

/* A seek that leaves the position where it is (a "tell", or a seek to the
 * current end of the buffered data) keeps the buffer: the game asks for the
 * position between its small writes while saving a package. Returns the
 * position, or -1 when the seek moves (the caller flushes then seeks). */
static off_t file_seek_in_place(file_t *f, int64_t distance, int whence)
{
    if (!f->wlen) return -1;
    off_t logical = file_logical_position(f);
    if (logical < 0) return -1;
    if (whence == SEEK_CUR && distance == 0) return logical;
    if (whence == SEEK_SET && distance == (int64_t)logical) return logical;
    return -1;
}

/* Before anything another handle or a path can observe. */
static void files_flush_all(void)
{
    file_t **link = &dirty_files;
    while (*link) {
        file_t *f = *link;
        file_flush(f);
        f->wlisted = 0;
        *link = f->wnext;
        f->wnext = NULL;
    }
}

static void file_unlist(file_t *f)
{
    if (!f->wlisted) return;
    for (file_t **link = &dirty_files; *link; link = &(*link)->wnext)
        if (*link == f) { *link = f->wnext; break; }
    f->wnext = NULL;
    f->wlisted = 0;
}

static void file_destroy(void_object *o)
{
    file_t *f = (file_t *)o->data;
    for (file_t **link = &live_files; *link; link = &(*link)->file_next)
        if (*link == f) { *link = f->file_next; break; }
    file_flush(f);
    if (f->cache_wrote) void_readcache_flush_key(f->path_key);
    file_unlist(f);
    free(f->wbuf);
    f->wbuf = NULL;
    if (f->fd >= 0 && !f->std_stream) { VSTAT_BEGIN(start); close(f->fd); VSTAT_END(start, VS_CLOSE, 0); }
    if (f->writable && f->host_path) dircache_writer(f->host_path, -1);
    if (f->delete_on_close && f->host_path && !unlink(f->host_path)) {
        dircache_removed(f->host_path);
        void_readcache_changed(f->host_path);
    }
    free(f->host_path);
}

static uint32_t file_handle(int fd, char *host_path, int std_stream)
{
    void_object *o = void_object_new(VOID_OBJ_FILE, sizeof(file_t));
    file_t *f = (file_t *)o->data;
    f->fd = fd;
    f->std_stream = std_stream;
    f->host_path = host_path;
    if (f->host_path) { f->file_next = live_files; live_files = f; }
    o->destroy = file_destroy;
    o->signaled = 1;
    return void_handle_new(o);
}

static file_t *file_get(uint32_t h)
{
    void_object *o = void_handle_get(h, VOID_OBJ_FILE);
    return o ? (file_t *)o->data : NULL;
}

/* File operations that yield the guest machine keep the descriptor alive through CloseHandle. */
static void release_file_operation(void_object **o)
{
    void_object_release(*o);
}

static uint64_t file_path_key(const char *path)
{
    size_t root = strlen(data_root);
    const char *name = strrchr(path, '/');
    if (root && name && !strncmp(path, data_root, root) && path[root] == '/') {
        const char *extension = strrchr(name, '.');
        /* DBPF saves and their compaction files go through the descriptor, not block write-back. */
        if ((extension && !strcasecmp(extension, ".package")) ||
            !strncasecmp(name + 1, "TempDBSegmentPackedFile_", sizeof "TempDBSegmentPackedFile_" - 1)) return 0;
    }
    return void_readcache_key(path);
}

/* For each package it uses the game opens up to four handles: one for
 * FILE_READ_ATTRIBUTES, two GENERIC_WRITE probes that only ask for the
 * position, a GENERIC_READ one it closes unused, and the read/write handle it
 * reads through. On Android's shared storage each open is a MediaProvider
 * round trip (1-2 ms; ~15 ms when OPEN_ALWAYS makes it a CREATE), ~25 s over
 * a start and the first load of a town. Handles to existing files therefore
 * open on their first read or write; times, sizes and positions come from
 * the directory cache until then. S2_LAZYOPEN=0 (or <data>/nolazyopen)
 * opens at CreateFile. */
static int files_lazy_open(void)
{
    static int state;                  /* 0 unknown, 1 on, 2 off */
    if (!state) {
        const char *env = getenv("S2_LAZYOPEN");
        char flag[1200];
        snprintf(flag, sizeof flag, "%s/nolazyopen", void_data_dir());
        state = (env && *env == '0') || !access(flag, F_OK) ? 2 : 1;
        void_log("[files] lazy opens %s", state == 1 ? "on" : "off");
    }
    return state == 1;
}

/* Open a lazy handle's descriptor (machine held; released meanwhile).
 * Returns 0, or -1 with errno set. */
static int file_open_now(file_t *f, uint32_t h)
{
    while (f->zip_entry) {
        if (!f->host_path || (f->lazy_flags & O_ACCMODE) == O_RDONLY) { errno = EACCES; return -1; }
        const zm_entry *entry = f->zip_entry;
        char *path = strdup(f->host_path);
        if (!path) { errno = ENOMEM; return -1; }
        void_blocking_begin();
        pthread_mutex_lock(&zip_copy_lock);
        struct stat st;
        int copied = 0;
        if (stat(path, &st)) {
            char parent[2048];
            snprintf(parent, sizeof parent, "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = 0; mkdirs(parent); }
            copied = s2_zip_copy(entry, path);
        }
        int error = errno;
        pthread_mutex_unlock(&zip_copy_lock);
        void_blocking_end();
        if (copied) { free(path); errno = error; return -1; }
        for (file_t *other = live_files; other; other = other->file_next)
            if (!strcmp(other->host_path, path)) other->zip_entry = NULL;
        dircache_created(path, 0);
        void_readcache_changed(path);
        free(path);
    }
    while (f->lazy) {
        if (!f->host_path) { errno = EBADF; return -1; }
        char *path = strdup(f->host_path);
        if (!path) { errno = ENOMEM; return -1; }
        int flags = f->lazy_flags;
        VSTAT_BEGIN(start);
        void_blocking_begin();
        int fd = open(path, flags, 0664);
        int e = errno;
        void_blocking_end();
        VSTAT_END(start, VS_OPEN, 0);
        IOTRACE("M %08X %08X %llu %s\n", h, (unsigned)flags,
            (unsigned long long)((void_stat_now() - start) / 1000), path);
        int same_path = f->host_path && !strcmp(path, f->host_path);
        free(path);
        if (!f->lazy || !same_path) {
            if (fd >= 0) close(fd);
            continue;
        }
        if (fd < 0) {
            VLOG("[void] deferred open of %s failed: %s", f->host_path, strerror(e));
            errno = e;
            return -1;
        }
        if (f->lazy_pos && lseek(fd, f->lazy_pos, SEEK_SET) < 0) {
            e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        f->fd = fd;
        f->lazy = 0;
        if ((flags & O_ACCMODE) != O_RDONLY) {
            f->writable = 1;
            dircache_writer(f->host_path, 1);
        }
    }
    return 0;
}

static off_t lazy_seek(file_t *f, int64_t distance, int whence);

/* SetFilePointer: the end of a file counts its dirty blocks (readcache.c). */
static off_t file_seek(file_t *f, int64_t distance, int whence)
{
    if (f->lazy) return lazy_seek(f, distance, whence);
    off_t logical;
    if (whence == SEEK_END && void_readcache_size(f->path_key, &logical)) {
        struct stat st;
        if (!fstat(f->fd, &st) && st.st_size > logical) logical = st.st_size;
        if (logical + (off_t)distance < 0) { errno = EINVAL; return -1; }
        return lseek(f->fd, logical + (off_t)distance, SEEK_SET);
    }
    return lseek(f->fd, (off_t)distance, whence);
}

/* SetFilePointer on a lazy handle. */
static off_t lazy_seek(file_t *f, int64_t distance, int whence)
{
    off_t base = 0;
    if (whence == SEEK_CUR) base = f->lazy_pos;
    else if (whence == SEEK_END) {
        struct stat st;
        if (f->zip_entry) st.st_size = (off_t)f->zip_entry->size;
        else if (dircache_stat(f->host_path, &st)) return -1;
        base = st.st_size;
        off_t logical;
        if (void_readcache_size(f->path_key, &logical) && logical > base) base = logical;
    }
    off_t pos = base + (off_t)distance;
    if (pos < 0) { errno = EINVAL; return -1; }
    f->lazy_pos = pos;
    return pos;
}

int void_file_host_fd(uint32_t h)
{
    void_object *object __attribute__((cleanup(release_file_operation))) = void_handle_acquire(h, VOID_OBJ_FILE);
    file_t *f = object ? object->data : NULL;
    if (f) file_flush(f);
    if (f && file_open_now(f, h)) return -1;
    if (f) void_readcache_flush_key(f->path_key);
    return f && void_handle_get(h, VOID_OBJ_FILE) == object ? f->fd : -1;
}

static uint32_t std_handles[3];

void void_files_std_init(void)
{
    std_handles[0] = file_handle(0, NULL, 1);
    std_handles[1] = file_handle(1, NULL, 2);
    std_handles[2] = file_handle(2, NULL, 3);
}

#define GENERIC_READ_ 0x80000000u
#define GENERIC_WRITE_ 0x40000000u
#define FILE_WRITE_DATA_ 0x0002u
#define FILE_APPEND_DATA_ 0x0004u
#define CREATE_NEW_ 1u
#define CREATE_ALWAYS_ 2u
#define OPEN_EXISTING_ 3u
#define OPEN_ALWAYS_ 4u
#define TRUNCATE_EXISTING_ 5u
#define FILE_FLAG_DELETE_ON_CLOSE_ 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS_ 0x02000000u

static uint32_t create_file_uncounted(const char *path, uint32_t access, uint32_t disposition, uint32_t flags);
static uint32_t create_file(const char *path, uint32_t access, uint32_t disposition, uint32_t flags)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    uint32_t h = create_file_uncounted(path, access, disposition, flags);
    VSTAT_END(start, VS_OPEN, 0);
    IOTRACE("O %08X %08X %u %llu %s\n", h, access, disposition, (unsigned long long)((void_stat_now() - start) / 1000), path ? path : "");
    return h;
}

static uint32_t create_file_uncounted(const char *path, uint32_t access, uint32_t disposition, uint32_t flags)
{
    if (!path || !*path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    if (!strncasecmp(path, "\\\\.\\", 4) || !strcasecmp(path, "CONOUT$") || !strcasecmp(path, "CONIN$")) {
        void_log("[void] CreateFile(%s): device paths are not supported", path);
        void_set_last_error(ERROR_FILE_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    int parent_ok = 0, type = -1;
    char *host = to_host_typed(path, &parent_ok, NULL, &type);
    if (!host) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    struct stat st;
    int exists = type != -1, is_dir = type == DT_DIR;
    if (exists && type != DT_DIR && type != DT_REG) {
        exists = !dircache_stat(host, &st);
        is_dir = exists && S_ISDIR(st.st_mode);
    }
    if (exists && is_dir) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS_)) {
            free(host);
            void_set_last_error(ERROR_ACCESS_DENIED_);
            return WIN_INVALID_HANDLE;
        }
        int fd = open(host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int virtual_dir = fd < 0 && !s2_zip_stat(host, &st) && S_ISDIR(st.st_mode);
        uint32_t h = fd >= 0 || virtual_dir ? file_handle(fd, host, 0) : WIN_INVALID_HANDLE;
        if (h != WIN_INVALID_HANDLE) { file_get(h)->is_dir = 1; file_get(h)->lazy = virtual_dir; }
        else { void_set_last_error(void_errno_to_win32(errno)); free(host); }
        return h;
    }
    int write = (access & (GENERIC_WRITE_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_ | 0x10000000u)) != 0;
    int read = (access & (GENERIC_READ_ | 0x0001u | 0x10000000u)) != 0 || !write;
    int oflags = O_CLOEXEC | (write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY);
    switch (disposition) {
    case CREATE_NEW_: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS_: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING_: break;
    case OPEN_ALWAYS_: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING_: oflags |= O_TRUNC; break;
    default:
        free(host);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return WIN_INVALID_HANDLE;
    }
    if ((oflags & O_CREAT) && !write) oflags = (oflags & ~O_ACCMODE) | O_RDWR;
    /* An existing file needs no O_CREAT: on Android's FUSE an O_CREAT open of
     * a name the kernel has not looked up yet is a CREATE request, which
     * MediaProvider answers in ~15 ms instead of ~1 ms. */
    int regular = exists && !is_dir && (type == DT_REG || (type != DT_DIR && S_ISREG(st.st_mode)));
    if (regular && (disposition == OPEN_EXISTING_ || disposition == OPEN_ALWAYS_)) {
        oflags &= ~O_CREAT;
        if (!write) oflags = (oflags & ~O_ACCMODE) | O_RDONLY;
    }
    const zm_entry *archived = s2_zip_find(host);
    if (archived && !stat(host, &st)) archived = NULL;
    if (archived && disposition == CREATE_NEW_) {
        free(host); void_set_last_error(ERROR_FILE_EXISTS_); return WIN_INVALID_HANDLE;
    }
    if (archived && !(oflags & O_TRUNC) && !(flags & FILE_FLAG_DELETE_ON_CLOSE_)) {
        uint32_t h = file_handle(-1, host, 0);
        file_t *f = file_get(h);
        f->zip_entry = archived;
        f->path_key = file_path_key(host);
        f->cached_reads = (oflags & O_ACCMODE) != O_WRONLY;
        f->lazy = 1;
        f->lazy_flags = oflags;
        void_set_last_error(disposition == OPEN_ALWAYS_ ? ERROR_ALREADY_EXISTS_ : 0);
        return h;
    }
    if (archived) {
        char parent[2048];
        snprintf(parent, sizeof parent, "%s", host);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; mkdirs(parent); }
        oflags |= O_CREAT;
    }
    int fd, lazy = 0;
    errno = 0;
    if (!exists && !(oflags & O_CREAT)) {
        /* Known absent (dircache.c): a FUSE open of a missing name costs ~0.5 ms. */
        fd = -1;
        errno = ENOENT;
    } else if (regular && !(oflags & (O_CREAT | O_TRUNC)) && !(flags & FILE_FLAG_DELETE_ON_CLOSE_) && files_lazy_open()) {
        fd = -1;                        /* see files_lazy_open */
        lazy = 1;
    } else {
        /* Before a truncation: written back blocks would land after it. */
        if (exists && (oflags & O_TRUNC)) void_readcache_flush_path(host);
        /* Opening through FUSE waits on MediaProvider; the guest state is not touched. */
        void_blocking_begin();
        fd = open(host, oflags, 0664);
        int e = errno;
        void_blocking_end();
        errno = e;
    }
    if (fd < 0 && !lazy) {
        int e = errno;
        uint32_t code = void_errno_to_win32(e);
        if (e == ENOENT && !parent_ok) code = ERROR_PATH_NOT_FOUND_;
        VLOG("[void] CreateFile(%s -> %s) failed: %s", path, host, strerror(e));
        free(host);
        void_set_last_error(code);
        return WIN_INVALID_HANDLE;
    }
    uint32_t h = file_handle(fd, host, 0);
    file_t *f = file_get(h);
    if (oflags & O_TRUNC)
        for (file_t *other = live_files; other; other = other->file_next)
            if (!strcmp(other->host_path, host)) other->zip_entry = NULL;
    if (flags & FILE_FLAG_DELETE_ON_CLOSE_) f->delete_on_close = 1;
    if (!exists) dircache_created(host, 0);
    f->path_key = file_path_key(host);
    f->cached_reads = (oflags & O_ACCMODE) != O_WRONLY;
    f->lazy = lazy;
    f->lazy_flags = oflags;
    if (!exists || (oflags & O_TRUNC)) void_readcache_changed(host);
    if (!lazy && (oflags & O_ACCMODE) != O_RDONLY) {
        f->writable = 1;
        dircache_writer(host, 1);
    }
    const char *name = strrchr(host, '/');
    if (name && !strcasecmp(name + 1, "savedProps.txt"))
        void_log("[settings] save whitelist %s -> %s", path, host);
    VLOG("[void] CreateFile(%s) -> %s handle %08X", path, host, h);
    void_set_last_error((exists && (disposition == CREATE_ALWAYS_ || disposition == OPEN_ALWAYS_)) ? ERROR_ALREADY_EXISTS_ : 0);
    return h;
}

#define K32 "kernel32.dll"

WINAPI_EXPORT(K32, CreateFileA, 7)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t h = create_file(path, ARG(1), ARG(4), ARG(5));
    free(path);
    return h;
}

WINAPI_EXPORT(K32, CreateFileW, 7)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t h = create_file(path, ARG(1), ARG(4), ARG(5));
    free(path);
    return h;
}

static void console_write(file_t *f, const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        char c = (char)data[i];
        if (c == '\n' || f->line_len == sizeof f->line - 1) {
            f->line[f->line_len] = 0;
            void_log("[guest-%s] %s", f->std_stream == 3 ? "stderr" : "stdout", f->line);
            f->line_len = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') f->line[f->line_len++] = c;
    }
}

WINAPI_EXPORT(K32, ReadFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), read_out = ARG(3), overlapped = ARG(4);
    void_object *object __attribute__((cleanup(release_file_operation))) = void_handle_acquire(h, VOID_OBJ_FILE);
    file_t *f = object ? object->data : NULL;
    if ((read_out && !void_vm_writable(read_out, 4)) || (overlapped && !void_vm_writable(overlapped, 16))) {
        void_set_last_error(998u); return 0;
    }
    if (read_out) MEM32(read_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) return 1;
    if (size && !void_vm_writable(buffer, size)) { void_set_last_error(998u); return 0; }  /* ERROR_NOACCESS */
    if (f->zip_entry) {
        const zm_entry *entry = f->zip_entry;
        uint64_t at = overlapped ? (uint64_t)MEM32(overlapped + 8) | ((uint64_t)MEM32(overlapped + 12) << 32) : (uint64_t)f->lazy_pos;
        void_blocking_begin();
        long long got = s2_zip_read(entry, GPTR(buffer), size, at);
        int error = errno;
        void_blocking_end();
        if (got < 0) { void_set_last_error(void_errno_to_win32(error)); return 0; }
        if (!overlapped) f->lazy_pos += got;
        if (read_out) MEM32(read_out) = (uint32_t)got;
        if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
        return 1;
    }
    if (f->wlen && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
    /* Another handle's buffered writes to this file come first. */
    for (file_t *d = dirty_files; d; d = d->wnext)
        if (d != f && d->wlen && d->path_key == f->path_key) file_flush(d);
    if (f->path_key && f->cached_reads && size) {
        VSTAT_BEGIN(cached_start);
        off_t at = overlapped ? (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32)
                 : f->lazy ? f->lazy_pos : lseek(f->fd, 0, SEEK_CUR);
        int cfd = f->fd;                /* -1 while lazy: blocks in memory only */
        ssize_t got = at < 0 ? -2 : void_readcache_pread(cfd, f->path_key, GPTR(buffer), size, at);
        if (got == -3) {
            if (file_open_now(f, h)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
            cfd = f->fd;
            got = void_readcache_pread(cfd, f->path_key, GPTR(buffer), size, at);
        }
        if (got != -2) {
            int e = errno;
            if (got >= 0 && !overlapped) {
                if (f->lazy) f->lazy_pos = at + got;
                else lseek(cfd, at + got, SEEK_SET);
            }
            VSTAT_END(cached_start, VS_READ, got > 0 ? (uint64_t)got : 0);
            IOTRACE("C %08X %u %lld %lld %llu\n", h, size, overlapped ? (long long)at : -1LL, (long long)(at + (got > 0 ? got : 0)),
                (unsigned long long)((void_stat_now() - cached_start) / 1000));
            if (got < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
            if (read_out) MEM32(read_out) = (uint32_t)got;
            if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
            return 1;
        }
    }
    if (file_open_now(f, h)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    void_readcache_flush_key(f->path_key);
    int fd = f->fd;
    off_t offset = overlapped ? (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32) : 0;
    ssize_t got;
    int blocking = size != 0;
    VSTAT_BEGIN(start);
    if (blocking) void_blocking_begin();
    if (overlapped) {
        got = pread(fd, GPTR(buffer), size, offset);
    } else {
        size_t total = 0;
        got = 0;
        while (total < size) {
            ssize_t r = read(fd, (uint8_t *)GPTR(buffer) + total, size - total);
            if (r < 0) { if (errno == EINTR) continue; got = -1; break; }
            if (r == 0) break;
            total += (size_t)r;
        }
        if (got >= 0) got = (ssize_t)total;
    }
    int e = errno;
    if (blocking) void_blocking_end();
    VSTAT_END(start, VS_READ, got > 0 ? (uint64_t)got : 0);
    IOTRACE("R %08X %u %lld %lld %llu\n", h, size, overlapped ? (long long)offset : -1LL,
        io_trace ? (long long)lseek(fd, 0, SEEK_CUR) : 0LL, (unsigned long long)((void_stat_now() - start) / 1000));
    if (got < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
    if (read_out) MEM32(read_out) = (uint32_t)got;
    if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
    return 1;
}

WINAPI_EXPORT(K32, WriteFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), written_out = ARG(3), overlapped = ARG(4);
    void_object *object __attribute__((cleanup(release_file_operation))) = void_handle_acquire(h, VOID_OBJ_FILE);
    file_t *f = object ? object->data : NULL;
    if ((size && !void_vm_readable(buffer, size)) || (written_out && !void_vm_writable(written_out, 4)) ||
        (overlapped && !void_vm_readable(overlapped, 16))) {
        void_set_last_error(998u); return 0;
    }
    if (written_out) MEM32(written_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) {
        console_write(f, (const uint8_t *)GPTR(buffer), size);
        if (written_out) MEM32(written_out) = size;
        return 1;
    }
    if (file_open_now(f, h)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    IOTRACE("W %08X %u %lld %lld %u\n", h, size, overlapped ? (long long)((off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32)) : -1LL,
        io_trace ? (long long)lseek(f->fd, 0, SEEK_CUR) + f->wlen : 0LL, f->wlen);
    if (f->werr) {
        /* A buffered write failed after an earlier WriteFile reported success. */
        void_set_last_error(void_errno_to_win32(f->werr));
        f->werr = 0;
        return 0;
    }
    if (f->path_key && size && (f->lazy_flags & O_ACCMODE) != O_RDONLY && void_readcache_active()) {
        off_t at = overlapped ? (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32) : lseek(f->fd, 0, SEEK_CUR);
        ssize_t put = at < 0 ? -2 : void_readcache_pwrite(f->fd, f->path_key, f->host_path, GPTR(buffer), size, at);
        if (put >= 0) {
            if (!overlapped) lseek(f->fd, at + put, SEEK_SET);
            f->cache_wrote = 1;
            if (written_out) MEM32(written_out) = (uint32_t)put;
            return 1;
        }
        if (put == -1) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
        void_readcache_flush_key(f->path_key);      /* a big write lands after the dirty blocks */
    }
    if (!overlapped && size && size < WBUF_CAP / 2 && !void_readcache_active()) {
        if (f->wlen + size > WBUF_CAP && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
        if (!f->wbuf) f->wbuf = (uint8_t *)malloc(WBUF_CAP);
        if (f->wbuf) {
            memcpy(f->wbuf + f->wlen, GPTR(buffer), size);
            f->wlen += size;
            if (!f->wlisted) { f->wnext = dirty_files; dirty_files = f; f->wlisted = 1; }
            if (written_out) MEM32(written_out) = size;
            return 1;
        }
    }
    if (f->wlen && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
    ssize_t put;
    VSTAT_BEGIN(start);
    if (overlapped) {
        off_t offset = (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32);
        put = pwrite(f->fd, GPTR(buffer), size, offset);
    } else {
        put = write(f->fd, GPTR(buffer), size);
    }
    VSTAT_END(start, VS_WRITE, put > 0 ? (uint64_t)put : 0);
    if (put > 0 && f->path_key) void_readcache_changed_key(f->path_key);
    if (put < 0) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    if (written_out) MEM32(written_out) = (uint32_t)put;
    return 1;
}

WINAPI_EXPORT(K32, SetFilePointer, 4)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    int64_t distance = ARG(2) ? (int64_t)(((uint64_t)MEM32(ARG(2)) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    if (f->std_stream) return 0;
    off_t kept = file_seek_in_place(f, distance, whence);
    if (kept >= 0) {
        if (ARG(2)) MEM32(ARG(2)) = (uint32_t)((uint64_t)kept >> 32);
        void_set_last_error(0);
        return (uint32_t)kept;
    }
    if (f->wlen && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0xFFFFFFFFu; }
    off_t pos = file_seek(f, distance, whence);
    IOTRACE("S %08X %lld %u %lld\n", ARG(0), (long long)distance, ARG(3), (long long)pos);
    if (pos < 0) { void_set_last_error(131u); return 0xFFFFFFFFu; }   /* ERROR_NEGATIVE_SEEK */
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)((uint64_t)pos >> 32);
    void_set_last_error(0);
    return (uint32_t)pos;
}

WINAPI_EXPORT(K32, SetFilePointerEx, 5)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    int64_t distance = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    int whence = ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END;
    off_t kept = file_seek_in_place(f, distance, whence);
    if (kept >= 0) {
        if (ARG(3)) { MEM32(ARG(3)) = (uint32_t)kept; MEM32(ARG(3) + 4) = (uint32_t)((uint64_t)kept >> 32); }
        return 1;
    }
    if (f->wlen && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
    off_t pos = file_seek(f, distance, whence);
    IOTRACE("S %08X %lld %u %lld\n", ARG(0), (long long)distance, ARG(4), (long long)pos);
    if (pos < 0) { void_set_last_error(131u); return 0; }
    if (ARG(3)) { MEM32(ARG(3)) = (uint32_t)pos; MEM32(ARG(3) + 4) = (uint32_t)((uint64_t)pos >> 32); }
    return 1;
}

static int file_size_of(file_t *f, struct stat *st)
{
    if (!f) return -1;
    if (f->zip_entry) return s2_zip_stat(f->host_path, st);
    if (f->lazy ? dircache_stat(f->host_path, st) : fstat(f->fd, st)) return -1;
    off_t logical;
    if (void_readcache_size(f->path_key, &logical) && logical > st->st_size) st->st_size = logical;
    if (f->wlen) {
        off_t end = file_logical_position(f);
        if (end > st->st_size) st->st_size = end;
    }
    return 0;
}

WINAPI_EXPORT(K32, GetFileSize, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || file_size_of(f, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    if (ARG(1)) MEM32(ARG(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    void_set_last_error(0);
    return (uint32_t)st.st_size;
}

WINAPI_EXPORT(K32, GetFileSizeEx, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || file_size_of(f, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    MEM32(ARG(1)) = (uint32_t)st.st_size;
    MEM32(ARG(1) + 4) = (uint32_t)((uint64_t)st.st_size >> 32);
    return 1;
}

WINAPI_EXPORT(K32, GetFileType, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    return f->std_stream ? 2u : 1u;    /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

WINAPI_EXPORT(K32, FlushFileBuffers, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) return 0;
    if (file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
    void_readcache_flush_key(f->path_key);
    return 1;
}

WINAPI_EXPORT(K32, SetEndOfFile, 1)
{
    void_object *object __attribute__((cleanup(release_file_operation))) = void_handle_acquire(ARG(0), VOID_OBJ_FILE);
    file_t *f = object ? object->data : NULL;
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->wlen && file_flush(f)) { void_set_last_error(void_errno_to_win32(f->werr)); f->werr = 0; return 0; }
    if (file_open_now(f, ARG(0))) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    /* The DBPF writer makes the file longer up to the end of each resource it
     * is about to write, ~31,000 times in the first load of a town: the cache
     * keeps that like a write (an end where the file already ends changes
     * nothing). A shorter file is truncated here. */
    int kept = f->path_key && (f->lazy_flags & O_ACCMODE) != O_RDONLY
        ? void_readcache_set_end(f->fd, f->path_key, f->host_path, pos) : 1;
    IOTRACE("E %08X %lld %d\n", ARG(0), (long long)pos, kept);
    if (!kept) { f->cache_wrote = 1; return 1; }
    void_readcache_flush_key(f->path_key);
    if (ftruncate(f->fd, pos)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    if (f->path_key) void_readcache_changed_key(f->path_key);
    return 1;
}

WINAPI_EXPORT(K32, LockFile, 5) { return 1; }
WINAPI_EXPORT(K32, UnlockFile, 5) { return 1; }
WINAPI_EXPORT(K32, LockFileEx, 6) { return 1; }

WINAPI_EXPORT(K32, GetStdHandle, 1)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: return std_handles[0];
    case 0xFFFFFFF5u: return std_handles[1];
    case 0xFFFFFFF4u: return std_handles[2];
    default: void_set_last_error(ERROR_INVALID_HANDLE_); return WIN_INVALID_HANDLE;
    }
}

WINAPI_EXPORT(K32, SetStdHandle, 2)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: std_handles[0] = ARG(1); return 1;
    case 0xFFFFFFF5u: std_handles[1] = ARG(1); return 1;
    case 0xFFFFFFF4u: std_handles[2] = ARG(1); return 1;
    default: return 0;
    }
}

static uint32_t attributes_of_uncounted(const char *path);
static uint32_t attributes_of(const char *path)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    uint32_t r = attributes_of_uncounted(path);
    VSTAT_END(start, VS_ATTR, 0);
    return r;
}

static uint32_t attributes_of_uncounted(const char *path)
{
    if (!path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return 0xFFFFFFFFu; }
    int parent_ok = 0, type = -1;
    char *host = to_host_typed(path, &parent_ok, NULL, &type);
    struct stat st;
    if (host && (type == DT_DIR || type == DT_REG)) {
        free(host);
        return type == DT_DIR ? 0x10u : 0x20u;
    }
    if (!host || type == -1 || dircache_stat(host, &st)) {
        void_set_last_error(parent_ok ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_);
        VLOG("[void] GetFileAttributes(%s): missing", path);
        free(host);
        return 0xFFFFFFFFu;
    }
    free(host);
    if (S_ISDIR(st.st_mode)) return 0x10u;
    return 0x20u;                       /* FILE_ATTRIBUTE_ARCHIVE */
}

WINAPI_EXPORT(K32, GetFileAttributesA, 1)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

WINAPI_EXPORT(K32, GetFileAttributesW, 1)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

static uint32_t delete_file(const char *path)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    char *host = to_host(path, NULL, NULL);
    int ok = host && !unlink(host);
    if (ok) { dircache_removed(host); void_readcache_changed(host); }
    VSTAT_END(start, VS_DIROP, 0);
    if (!ok) void_set_last_error(void_errno_to_win32(errno));
    free(host);
    return ok;
}

WINAPI_EXPORT(K32, DeleteFileA, 1) { char *p = void_astr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }
WINAPI_EXPORT(K32, DeleteFileW, 1) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }

static uint32_t move_file(const char *from, const char *to, int replace)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    char *a = to_host(from, NULL, NULL), *b = to_host(to, NULL, NULL);
    struct stat st;
    int ok = 0;
    struct moved_file { file_t *file; char *path; } *moved = NULL;
    size_t count = 0;
    if (a && b) {
        int exists = !lstat(b, &st);
        if (exists && (!replace || S_ISDIR(st.st_mode))) {
            void_set_last_error(ERROR_ALREADY_EXISTS_);
            goto done;
        }
        if (exists && !strcmp(a, b)) { ok = 1; goto done; }
        size_t prefix = strlen(a);
        for (file_t *f = live_files; f; f = f->file_next)
            if (!strncmp(f->host_path, a, prefix) && (!f->host_path[prefix] || f->host_path[prefix] == '/')) ++count;
        if (count && !(moved = calloc(count, sizeof *moved))) {
            void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
            goto done;
        }
        size_t i = 0;
        for (file_t *f = live_files; f; f = f->file_next) {
            if (strncmp(f->host_path, a, prefix) || (f->host_path[prefix] && f->host_path[prefix] != '/')) continue;
            moved[i].file = f;
            moved[i].path = malloc(strlen(b) + strlen(f->host_path + prefix) + 1);
            if (!moved[i].path) { void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); goto done; }
            strcpy(moved[i].path, b);
            strcat(moved[i].path, f->host_path + prefix);
            ++i;
        }
        void_readcache_flush_all();
        if (exists) {
            /* Let a deferred reader keep the old destination's inode across an atomic replace. */
            for (file_t *f = live_files; f; f = f->file_next) {
                if (!f->lazy || f->zip_entry || strcmp(f->host_path, b)) continue;
                int fd = open(b, f->lazy_flags, 0664);
                if (fd < 0 || (f->lazy_pos && lseek(fd, f->lazy_pos, SEEK_SET) < 0)) {
                    int error = errno;
                    if (fd >= 0) close(fd);
                    void_set_last_error(void_errno_to_win32(error));
                    goto done;
                }
                f->fd = fd;
                f->lazy = 0;
                if ((f->lazy_flags & O_ACCMODE) != O_RDONLY) {
                    f->writable = 1;
                    dircache_writer(b, 1);
                }
            }
        }
        if (rename(a, b)) void_set_last_error(void_errno_to_win32(errno));
        else {
            ok = 1;
            if (exists) {
                for (file_t **link = &live_files; *link;) {
                    file_t *f = *link;
                    if (strcmp(f->host_path, b)) { link = &f->file_next; continue; }
                    *link = f->file_next;
                    if (f->writable) dircache_writer(b, -1);
                    free(f->host_path);
                    f->host_path = NULL;
                    f->path_key = 0;
                    f->cached_reads = f->writable = f->delete_on_close = 0;
                }
            }
            dircache_renamed(a, b);
            /* DBPF compaction keeps both old and new package handles alive across renames. */
            for (size_t i = 0; i < count; ++i) {
                file_t *f = moved[i].file;
                free(f->host_path);
                f->host_path = moved[i].path;
                moved[i].path = NULL;
                f->path_key = file_path_key(f->host_path);
                if (f->writable) dircache_writer(f->host_path, 1);
            }
            void_readcache_clear();
        }
    } else void_set_last_error(ERROR_PATH_NOT_FOUND_);
done:
    IOTRACE("N %d %d %s -> %s\n", replace, ok, from ? from : "", to ? to : "");
    if (moved) for (size_t i = 0; i < count; ++i) free(moved[i].path);
    free(moved);
    free(a); free(b);
    VSTAT_END(start, VS_DIROP, 0);
    return ok;
}

WINAPI_EXPORT(K32, MoveFileA, 2)
{
    char *a = void_astr_utf8(ARG(0)), *b = void_astr_utf8(ARG(1));
    uint32_t r = move_file(a, b, 0);
    free(a); free(b);
    return r;
}

WINAPI_EXPORT(K32, MoveFileW, 2)
{
    char *a = void_wstr_utf8(ARG(0)), *b = void_wstr_utf8(ARG(1));
    uint32_t r = move_file(a, b, 0);
    free(a); free(b);
    return r;
}

WINAPI_EXPORT(K32, CreateDirectoryW, 2)
{
    VSTAT_BEGIN(start);
    char *path = void_wstr_utf8(ARG(0));
    char *host = to_host(path, NULL, NULL);
    uint32_t ok = 0;
    if (host) {
        if (!mkdir(host, 0775)) { ok = 1; dircache_created(host, 1); }
        else void_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS_ : void_errno_to_win32(errno));
    } else {
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
    }
    VLOG("[void] CreateDirectoryW(%s) = %u", path, ok);
    free(host); free(path);
    VSTAT_END(start, VS_DIROP, 0);
    return ok;
}

WINAPI_EXPORT(K32, CreateDirectoryA, 2)
{
    char *path = void_astr_utf8(ARG(0));
    char *host = to_host(path, NULL, NULL);
    uint32_t ok = 0;
    if (host) {
        if (!mkdir(host, 0775)) { ok = 1; dircache_created(host, 1); }
        else void_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS_ : void_errno_to_win32(errno));
    }
    free(host); free(path);
    return ok;
}

/* GetFullPathName: the normalized absolute Windows path. */
static uint32_t full_path(const char *in, uint32_t buffer, uint32_t capacity, uint32_t file_part, int wide)
{
    char norm[1024];
    if (!in || !normalize(in, norm, sizeof norm)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    /* Keep a trailing separator the caller supplied. */
    size_t in_len = strlen(in);
    if (in_len && (in[in_len - 1] == '\\' || in[in_len - 1] == '/') && strlen(norm) > 3) strcat(norm, "\\");
    uint32_t written = wide ? void_utf8_to_gw(norm, buffer, capacity) : void_utf8_to_ga(norm, buffer, capacity);
    uint32_t units = 0;
    if (wide) { if (written + 1 > capacity) return written; units = written; }
    else { if (written + 1 > capacity) return written; units = written; }
    if (file_part) {
        uint32_t last = 0;
        for (uint32_t i = 0; i < units; ++i) {
            uint32_t c = wide ? MEM16(buffer + 2 * i) : MEM8(buffer + i);
            if (c == '\\') last = i + 1;
        }
        MEM32(file_part) = last < units ? buffer + (wide ? 2 * last : last) : 0;
    }
    return units;
}

WINAPI_EXPORT(K32, GetFullPathNameA, 4)
{
    char *p = void_astr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 0);
    free(p);
    return r;
}

WINAPI_EXPORT(K32, GetFullPathNameW, 4)
{
    char *p = void_wstr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 1);
    free(p);
    return r;
}

static const char temp_dir[] = "C:\\Users\\Player\\AppData\\Local\\Temp\\";

WINAPI_EXPORT(K32, GetTempPathA, 2)
{
    uint32_t n = void_utf8_to_ga(temp_dir, ARG(1), ARG(0));
    return n;
}

WINAPI_EXPORT(K32, GetTempPathW, 2)
{
    uint32_t n = void_utf8_to_gw(temp_dir, ARG(1), ARG(0));
    return n;
}

static uint32_t temp_file_name(const char *dir, const char *prefix, uint32_t unique, char *out, size_t cap)
{
    static uint32_t counter;
    uint32_t number = unique ? unique : (uint32_t)(void_tick_ms() + ++counter) & 0xFFFFu;
    for (int attempt = 0; attempt < 65536; ++attempt) {
        if (!number) number = 1;
        snprintf(out, cap, "%s%s%.3s%X.tmp", dir, (dir[0] && dir[strlen(dir) - 1] == '\\') ? "" : "\\", prefix ? prefix : "", number);
        if (unique) return number;
        char *host = to_host(out, NULL, NULL);
        int fd = host ? open(host, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664) : -1;
        if (fd >= 0) { dircache_created(host, 0); void_readcache_changed(host); }
        free(host);
        if (fd >= 0) { close(fd); return number; }
        number = (number + 1) & 0xFFFFu;
    }
    return 0;
}

WINAPI_EXPORT(K32, GetTempFileNameA, 4)
{
    char *dir = void_astr_utf8(ARG(0)), *prefix = void_astr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_ga(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

WINAPI_EXPORT(K32, GetTempFileNameW, 4)
{
    char *dir = void_wstr_utf8(ARG(0)), *prefix = void_wstr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_gw(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

/* ------------------------------------------------------------ find files */
typedef struct {
    char **names;
    int count, next;
    int wide;                  /* WIN32_FIND_DATAW (592 bytes) or A (320) */
    char dir[2048];
} find_t;

/* FindFirstFile's name filter over a listing: the pattern case-insensitively,
 * and a trailing dot also matching names without an extension. */
typedef struct { find_t *f; const char *pattern; int all; } find_match;

static void find_gather(void *arg, const char *name, unsigned char type)
{
    (void)type;
    find_match *m = (find_match *)arg;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    size_t n = strlen(m->pattern);
    if (m->all || !fnmatch(m->pattern, name, FNM_CASEFOLD) || (n && m->pattern[n - 1] == '.' && !strchr(name, '.'))) {
        find_t *f = m->f;
        f->names = (char **)realloc(f->names, sizeof(char *) * (size_t)(f->count + 1));
        f->names[f->count++] = strdup(name);
    }
}

static void find_destroy(void_object *o)
{
    find_t *f = (find_t *)o->data;
    for (int i = 0; i < f->count; ++i) free(f->names[i]);
    free(f->names);
}

static uint64_t unix_to_filetime(time_t t, long ns)
{
    return (uint64_t)t * 10000000ull + (uint64_t)ns / 100u + 116444736000000000ull;
}

static int fill_find_data(find_t *f, const char *name, uint32_t out)
{
    char host[4096];
    snprintf(host, sizeof host, "%s/%s", f->dir, name);
    struct stat st;
    memset(GPTR(out), 0, f->wide ? 592 : 320);
    if (dircache_stat(host, &st)) return 0;
    MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    uint64_t ct = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    uint64_t at = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    uint64_t mt = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    MEM64(out + 4) = ct; MEM64(out + 12) = at; MEM64(out + 20) = mt;
    MEM32(out + 28) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(out + 32) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    if (f->wide) void_utf8_to_gw(name, out + 44, 260);
    else void_utf8_to_ga(name, out + 44, 260);
    return 1;
}

static uint32_t find_first_uncounted(char *pattern_path, uint32_t out, int wide);
static uint32_t find_first(char *pattern_path, uint32_t out, int wide)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    uint32_t h = find_first_uncounted(pattern_path, out, wide);
    VSTAT_END(start, VS_FIND, 0);
    return h;
}

static uint32_t find_first_uncounted(char *pattern_path, uint32_t out, int wide)
{
    if (!pattern_path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    char norm[1024];
    if (!normalize(pattern_path, norm, sizeof norm)) { free(pattern_path); void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    char *slash = strrchr(norm, '\\');
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s", slash ? slash + 1 : norm);
    if (slash) *slash = 0;
    if (strlen(norm) == 2) strcat(norm, "\\");
    int parent_ok = 0, dir_type = -1;
    char *host_dir = to_host_typed(norm, &parent_ok, NULL, &dir_type);
    if (!host_dir || dir_type == DT_REG || (dir_type == -1 && dircache_active())) {
        VLOG("[void] FindFirstFileW(%s): no directory", pattern_path);
        free(host_dir); free(pattern_path);
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    void_readcache_flush_dir(host_dir);
    find_t gathered = {0}, *f = &gathered;
    f->wide = wide;
    snprintf(f->dir, sizeof f->dir, "%s", host_dir);
    /* "*.*" matches every name, as on Windows. Every directory but a drive
     * root lists "." and ".." there; Android's FUSE /sdcard leaves them out,
     * and the game tests that a directory exists by FindFirstFile(dir\*.*)
     * succeeding (an empty one looked missing, so it never created its
     * Documents tree). */
    find_match match = { f, pattern, !strcmp(pattern, "*") || !strcmp(pattern, "*.*") };
    int root = strlen(norm) <= 3;
    /* Windows lists "." and ".." first; prepend them, and skip readdir's. */
    const char *dot_names[2] = {".", ".."};
    for (int i = 0; i < 2 && !root; ++i) {
        if (match.all || !fnmatch(pattern, dot_names[i], FNM_CASEFOLD)) {
            f->names = (char **)realloc(f->names, sizeof(char *) * (size_t)(f->count + 1));
            f->names[f->count++] = strdup(dot_names[i]);
        }
    }
    /* A cached listing (dircache.c) answers holding the machine; reading a
     * folder from FUSE releases it. */
    int listed = dircache_list(host_dir, find_gather, &match, 1);
    if (!listed) {
        void_blocking_begin();
        listed = dircache_list(host_dir, find_gather, &match, 0);
        if (!listed) {
            DIR *d = opendir(host_dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) find_gather(&match, e->d_name, e->d_type);
                closedir(d);
                listed = 1;
            }
        }
        void_blocking_end();
    }
    free(host_dir);
    if (!listed) {
        VLOG("[void] FindFirstFileW(%s): no directory", pattern_path);
        for (int i = 0; i < f->count; ++i) free(f->names[i]);
        free(f->names);
        free(pattern_path);
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    void_object *o = void_object_new(VOID_OBJ_FIND, sizeof(find_t));
    *(find_t *)o->data = gathered;
    f = (find_t *)o->data;
    o->destroy = find_destroy;
    VLOG("[void] FindFirstFileW(%s): %d matches", pattern_path, f->count);
    free(pattern_path);
    while (f->next < f->count && !fill_find_data(f, f->names[f->next], out)) ++f->next;
    if (f->next >= f->count) {
        void_object_release(o);
        void_set_last_error(ERROR_FILE_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    ++f->next;
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, FindFirstFileW, 2) { return find_first(void_wstr_utf8(ARG(0)), ARG(1), 1); }
WINAPI_EXPORT(K32, FindFirstFileA, 2) { return find_first(void_astr_utf8(ARG(0)), ARG(1), 0); }
/* FindExInfoStandard/Basic, FindExSearchNameMatch (or LimitToDirectories as a hint). */
WINAPI_EXPORT(K32, FindFirstFileExW, 6) { return find_first(void_wstr_utf8(ARG(0)), ARG(2), 1); }
WINAPI_EXPORT(K32, FindFirstFileExA, 6) { return find_first(void_astr_utf8(ARG(0)), ARG(2), 0); }

static uint32_t find_next(void)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_FIND);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    find_t *f = (find_t *)o->data;
    VSTAT_BEGIN(start);
    while (f->next < f->count) {
        if (fill_find_data(f, f->names[f->next++], ARG(1))) { VSTAT_END(start, VS_FINDNEXT, 0); return 1; }
    }
    VSTAT_END(start, VS_FINDNEXT, 0);
    void_set_last_error(ERROR_NO_MORE_FILES_);
    return 0;
}
WINAPI_EXPORT(K32, FindNextFileW, 2) { return find_next(); }
WINAPI_EXPORT(K32, FindNextFileA, 2) { return find_next(); }

WINAPI_EXPORT(K32, FindClose, 1)
{
    if (!void_handle_get(ARG(0), VOID_OBJ_FIND)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    void_handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------- mappings */
typedef struct { int fd; uint64_t size; uint32_t protect; uint64_t written_key; const zm_entry *zip_entry; } mapping_t;

static void mapping_destroy(void_object *o)
{
    mapping_t *m = (mapping_t *)o->data;
    if (m->fd >= 0) close(m->fd);
    if (m->written_key) void_readcache_changed_key(m->written_key);
}

WINAPI_EXPORT(K32, CreateFileMappingA, 6)
{
    uint32_t file = ARG(0), protect = ARG(2);
    uint64_t size = ((uint64_t)ARG(3) << 32) | ARG(4);
    int fd;
    uint64_t written_key = 0;
    const zm_entry *zip_entry = NULL;
    if (file == WIN_INVALID_HANDLE) {
        char path[1100];
        snprintf(path, sizeof path, "%s/Temp/.mapping-XXXXXX", data_root);
        fd = mkstemp(path);
        if (fd < 0 || !size) { if (fd >= 0) close(fd); void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
        unlink(path);
        if (ftruncate(fd, (off_t)size)) { close(fd); void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0; }
    } else {
        void_object *object __attribute__((cleanup(release_file_operation))) = void_handle_acquire(file, VOID_OBJ_FILE);
        file_t *f = object ? object->data : NULL;
        if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
        if (f->zip_entry && ((protect & 0xFFu) == 0x02u || (protect & 0xFFu) == 0x08u)) {
            zip_entry = f->zip_entry;
            if (!size) size = zip_entry->size;
            if (size > zip_entry->size) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
            fd = -1;
        } else {
            file_flush(f);
            if (f->is_dir || file_open_now(f, file)) { void_set_last_error(f->is_dir ? ERROR_ACCESS_DENIED_ : void_errno_to_win32(errno)); return 0; }
            void_readcache_flush_key(f->path_key);
            if ((protect & 0xFFu) == 0x04u || (protect & 0xFFu) == 0x40u) {
                written_key = f->path_key;
                if (written_key) void_readcache_changed_key(written_key);
            }
            fd = dup(f->fd);
            struct stat st;
            if (fd < 0 || fstat(fd, &st)) {
                int error = errno;
                if (fd >= 0) close(fd);
                void_set_last_error(void_errno_to_win32(error));
                return 0;
            }
            if (!size) size = (uint64_t)st.st_size;
            else if (size > (uint64_t)st.st_size) {
                uint32_t page = protect & 0xFFu;
                if (size > INT64_MAX || (page != 0x04u && page != 0x40u) || ftruncate(fd, (off_t)size)) {
                    close(fd);
                    void_set_last_error(ERROR_ACCESS_DENIED_);
                    return 0;
                }
            }
        }
    }
    void_object *o = void_object_new(VOID_OBJ_MAPPING, sizeof(mapping_t));
    mapping_t *m = (mapping_t *)o->data;
    m->fd = fd;
    m->size = size;
    m->protect = protect;
    m->written_key = written_key;
    m->zip_entry = zip_entry;
    o->destroy = mapping_destroy;
    void_set_last_error(0);
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, OpenFileMappingA, 3)
{
    void_set_last_error(ERROR_FILE_NOT_FOUND_);
    return 0;
}

WINAPI_EXPORT(K32, MapViewOfFile, 5)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_MAPPING);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    mapping_t *m = (mapping_t *)o->data;
    uint32_t access = ARG(1);
    uint64_t offset = ((uint64_t)ARG(2) << 32) | ARG(3);
    uint64_t size = ARG(4) ? ARG(4) : m->size - offset;
    if (offset > m->size || size > m->size - offset || size > 0x7FFF0000u || !size) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int writable = (access & 2u) != 0 || (access & 1u) != 0;
    int shared = !(access & 1u);           /* FILE_MAP_COPY is private */
    if (m->zip_entry) {
        const zm_entry *entry = m->zip_entry;
        if ((offset & (GUEST_GRANULARITY - 1)) || (writable && !(access & 1u))) {
            void_set_last_error(ERROR_ACCESS_DENIED_); return 0;
        }
        uint32_t va = void_vm_alloc(0, (uint32_t)size, 0x3000u, 0x04u);
        if (!va) return 0;
        void_blocking_begin();
        long long got = s2_zip_read(entry, GPTR(va), (size_t)size, offset);
        void_blocking_end();
        if (got != (long long)size) { void_vm_unmap_file(va); void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
        uint32_t old;
        if (!writable) void_vm_protect(va, (uint32_t)size, 0x02u, &old);
        return va;
    }
    uint32_t va = void_vm_map_file(m->fd, offset, (uint32_t)size, writable, shared);
    VLOG("[void] MapViewOfFile(%08X, off %llX, %llX) = %08X", ARG(0), (unsigned long long)offset,
        (unsigned long long)size, va);
    return va;
}

WINAPI_EXPORT(K32, UnmapViewOfFile, 1) { return (uint32_t)void_vm_unmap_file(ARG(0)); }

/* ---------------------------------------------- more file APIs (Sims 2) */
static int stat_path(const char *path, struct stat *st, int *parent_ok)
{
    files_flush_all();
    int parent = 0;
    char *host = path ? to_host(path, &parent, NULL) : NULL;
    if (host) void_readcache_flush_path(host);
    int ok = host && !dircache_stat(host, st);
    free(host);
    if (parent_ok) *parent_ok = parent;
    return ok;
}

/* WIN32_FILE_ATTRIBUTE_DATA: attributes, three FILETIMEs, size high/low. */
static uint32_t file_attributes_ex(const char *path, uint32_t out)
{
    struct stat st;
    int parent_ok = 0;
    if (!stat_path(path, &st, &parent_ok)) {
        void_set_last_error(parent_ok ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_);
        return 0;
    }
    MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    MEM64(out + 4) = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    MEM64(out + 12) = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    MEM64(out + 20) = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    MEM32(out + 28) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(out + 32) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    return 1;
}
WINAPI_EXPORT(K32, GetFileAttributesExA, 3)
{
    char *p = void_astr_utf8(ARG(0));
    uint32_t r = file_attributes_ex(p, ARG(2));
    free(p);
    return r;
}
WINAPI_EXPORT(K32, GetFileAttributesExW, 3)
{
    char *p = void_wstr_utf8(ARG(0));
    uint32_t r = file_attributes_ex(p, ARG(2));
    free(p);
    return r;
}

static int fstat_handle(uint32_t h, struct stat *st)
{
    file_t *f = file_get(h);
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->zip_entry) return !s2_zip_stat(f->host_path, st);
    file_flush(f);
    void_readcache_flush_key(f->path_key);
    if (f->lazy) {
        if (!dircache_stat(f->host_path, st)) return 1;
        void_set_last_error(void_errno_to_win32(errno));
        return 0;
    }
    if (f->fd >= 0 && !fstat(f->fd, st)) return 1;
    if (f->host_path && !stat(f->host_path, st)) return 1;
    void_set_last_error(ERROR_INVALID_HANDLE_);
    return 0;
}

WINAPI_EXPORT(K32, GetFileTime, 4)
{
    struct stat st;
    if (!fstat_handle(ARG(0), &st)) return 0;
    if (ARG(1)) MEM64(ARG(1)) = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    if (ARG(2)) MEM64(ARG(2)) = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    if (ARG(3)) MEM64(ARG(3)) = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    return 1;
}

WINAPI_EXPORT(K32, SetFileTime, 4)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    file_flush(f);
    void_readcache_flush_key(f->path_key);
    if ((f->fd >= 0 || f->lazy) && (ARG(2) || ARG(3))) {
        struct timespec ts[2];
        for (int i = 0; i < 2; ++i) {
            uint32_t ft = i == 0 ? ARG(2) : ARG(3);
            if (!ft) { ts[i].tv_sec = 0; ts[i].tv_nsec = UTIME_OMIT; continue; }
            uint64_t v = MEM64(ft) - 116444736000000000ull;
            ts[i].tv_sec = (time_t)(v / 10000000ull);
            ts[i].tv_nsec = (long)(v % 10000000ull) * 100;
        }
        if (f->lazy) utimensat(AT_FDCWD, f->host_path, ts, 0);
        else futimens(f->fd, ts);
        dircache_changed(f->host_path);
    }
    return 1;
}

/* BY_HANDLE_FILE_INFORMATION (52 bytes). */
WINAPI_EXPORT(K32, GetFileInformationByHandle, 2)
{
    struct stat st;
    if (!fstat_handle(ARG(0), &st)) return 0;
    uint32_t out = ARG(1);
    memset(GPTR(out), 0, 52);
    MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    MEM64(out + 4) = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    MEM64(out + 12) = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    MEM64(out + 20) = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    MEM32(out + 28) = (uint32_t)st.st_dev;                    /* dwVolumeSerialNumber */
    MEM32(out + 32) = (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(out + 36) = (uint32_t)st.st_size;
    MEM32(out + 40) = (uint32_t)st.st_nlink;
    MEM32(out + 44) = (uint32_t)((uint64_t)st.st_ino >> 32);
    MEM32(out + 48) = (uint32_t)st.st_ino;
    return 1;
}

static uint32_t copy_file_uncounted(const char *from, const char *to, int fail_if_exists);
static uint32_t copy_file(const char *from, const char *to, int fail_if_exists)
{
    files_flush_all();
    VSTAT_BEGIN(start);
    uint32_t r = copy_file_uncounted(from, to, fail_if_exists);
    VSTAT_END(start, VS_COPY, 0);
    return r;
}

static uint32_t copy_file_uncounted(const char *from, const char *to, int fail_if_exists)
{
    char *a = from ? to_host(from, NULL, NULL) : NULL, *b = to ? to_host(to, NULL, NULL) : NULL;
    uint32_t r = 0;
    if (!a || !b) { void_set_last_error(ERROR_PATH_NOT_FOUND_); goto done; }
    struct stat existing;
    if (fail_if_exists && !dircache_stat(b, &existing)) { void_set_last_error(ERROR_FILE_EXISTS_); goto done; }
    void_readcache_flush_path(a);
    void_readcache_flush_path(b);
    const zm_entry *archived = s2_zip_find(a);
    if (archived && stat(a, &existing)) {
        void_blocking_begin();
        int copied = s2_zip_copy(archived, b);
        int error = errno;
        void_blocking_end();
        r = copied == 0;
        if (r) { dircache_created(b, 0); void_readcache_changed(b); }
        else void_set_last_error(void_errno_to_win32(error));
        goto done;
    }
    int in = open(a, O_RDONLY);
    if (in < 0) { void_set_last_error(void_errno_to_win32(errno)); goto done; }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (out < 0) { close(in); void_set_last_error(void_errno_to_win32(errno)); goto done; }
    char buf[1 << 16];
    ssize_t n;
    r = 1;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { r = 0; break; }
    close(in);
    close(out);
    dircache_created(b, 0);
    void_readcache_changed(b);
    VLOG("[void] CopyFile(%s -> %s) = %u", from, to, r);
done:
    free(a);
    free(b);
    return r;
}
WINAPI_EXPORT(K32, CopyFileA, 3)
{
    char *a = void_astr_utf8(ARG(0)), *b = void_astr_utf8(ARG(1));
    uint32_t r = copy_file(a, b, ARG(2) != 0);
    free(a); free(b);
    return r;
}
WINAPI_EXPORT(K32, CopyFileW, 3)
{
    char *a = void_wstr_utf8(ARG(0)), *b = void_wstr_utf8(ARG(1));
    uint32_t r = copy_file(a, b, ARG(2) != 0);
    free(a); free(b);
    return r;
}

static uint32_t remove_directory(const char *path)
{
    char *host = path ? to_host(path, NULL, NULL) : NULL;
    uint32_t r = host && !rmdir(host);
    if (r) dircache_removed(host);
    if (!r) void_set_last_error(host ? void_errno_to_win32(errno) : ERROR_PATH_NOT_FOUND_);
    free(host);
    return r;
}
WINAPI_EXPORT(K32, RemoveDirectoryA, 1) { char *p = void_astr_utf8(ARG(0)); uint32_t r = remove_directory(p); free(p); return r; }
WINAPI_EXPORT(K32, RemoveDirectoryW, 1) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = remove_directory(p); free(p); return r; }

static uint32_t set_attributes(const char *path)
{
    struct stat st;
    uint32_t r = (uint32_t)stat_path(path, &st, NULL);
    if (!r) void_set_last_error(ERROR_FILE_NOT_FOUND_);
    return r;
}
WINAPI_EXPORT(K32, SetFileAttributesA, 2) { char *p = void_astr_utf8(ARG(0)); uint32_t r = set_attributes(p); free(p); return r; }
WINAPI_EXPORT(K32, SetFileAttributesW, 2) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = set_attributes(p); free(p); return r; }

static uint32_t move_file_ex(const char *from, const char *to, uint32_t flags)
{
    if (!to) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    return move_file(from, to, (flags & 1u) != 0);   /* MOVEFILE_REPLACE_EXISTING */
}
WINAPI_EXPORT(K32, MoveFileExW, 3)
{
    char *a = void_wstr_utf8(ARG(0)), *b = void_wstr_utf8(ARG(1));
    uint32_t r = move_file_ex(a, b, ARG(2));
    free(a); free(b);
    return r;
}
WINAPI_EXPORT(K32, MoveFileExA, 3)
{
    char *a = void_astr_utf8(ARG(0)), *b = void_astr_utf8(ARG(1));
    uint32_t r = move_file_ex(a, b, ARG(2));
    free(a); free(b);
    return r;
}

/* The data root's file system answers for drive C:. */
static void disk_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    struct statvfs vfs;
    if (!statvfs(data_root, &vfs)) {
        *free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        *total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    } else {
        *free_bytes = 8ull << 30;
        *total_bytes = 64ull << 30;
    }
}
static uint32_t disk_free_ex(void)
{
    uint64_t f, t;
    disk_space(&f, &t);
    if (ARG(1)) MEM64(ARG(1)) = f;
    if (ARG(2)) MEM64(ARG(2)) = t;
    if (ARG(3)) MEM64(ARG(3)) = f;
    return 1;
}
WINAPI_EXPORT(K32, GetDiskFreeSpaceExA, 4) { return disk_free_ex(); }
WINAPI_EXPORT(K32, GetDiskFreeSpaceExW, 4) { return disk_free_ex(); }
WINAPI_EXPORT(K32, GetDiskFreeSpaceA, 5)
{
    uint64_t f, t;
    disk_space(&f, &t);
    /* 4 KiB clusters of 8 sectors; the counts saturate like the real ones. */
    uint64_t free_clusters = f / 4096u, total_clusters = t / 4096u;
    if (ARG(1)) MEM32(ARG(1)) = 8;
    if (ARG(2)) MEM32(ARG(2)) = 512;
    if (ARG(3)) MEM32(ARG(3)) = free_clusters > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)free_clusters;
    if (ARG(4)) MEM32(ARG(4)) = total_clusters > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total_clusters;
    return 1;
}
static uint32_t volume_information(int wide)
{
    if (ARG(1) && ARG(2)) { if (wide) void_utf8_to_gw("ANDROID", ARG(1), ARG(2)); else void_utf8_to_ga("ANDROID", ARG(1), ARG(2)); }
    if (ARG(3)) MEM32(ARG(3)) = 0x53494D32u;         /* volume serial "SIM2" */
    if (ARG(4)) MEM32(ARG(4)) = 255;
    if (ARG(5)) MEM32(ARG(5)) = 0x0000002Fu;
    if (ARG(6) && ARG(7)) { if (wide) void_utf8_to_gw("NTFS", ARG(6), ARG(7)); else void_utf8_to_ga("NTFS", ARG(6), ARG(7)); }
    return 1;
}
WINAPI_EXPORT(K32, GetVolumeInformationA, 8) { return volume_information(0); }
WINAPI_EXPORT(K32, GetVolumeInformationW, 8) { return volume_information(1); }
static uint32_t drive_type(const char *root)
{
    /* DRIVE_FIXED for C: (and the current drive), DRIVE_NO_ROOT_DIR otherwise. */
    if (!root || ((root[0] | 0x20) == 'c' && root[1] == ':')) return 3;
    if (root[0] == '\\' || root[0] == '/') return 3;
    return 1;
}
WINAPI_EXPORT(K32, GetDriveTypeA, 1) { char *p = ARG(0) ? void_astr_utf8(ARG(0)) : NULL; uint32_t r = drive_type(p); free(p); return r; }
WINAPI_EXPORT(K32, GetDriveTypeW, 1) { char *p = ARG(0) ? void_wstr_utf8(ARG(0)) : NULL; uint32_t r = drive_type(p); free(p); return r; }
WINAPI_EXPORT(K32, GetLogicalDriveStringsA, 2)
{
    if (ARG(0) < 5) return 5;
    MEM8(ARG(1)) = 'C'; MEM8(ARG(1) + 1) = ':'; MEM8(ARG(1) + 2) = '\\'; MEM8(ARG(1) + 3) = 0; MEM8(ARG(1) + 4) = 0;
    return 4;
}
WINAPI_EXPORT(K32, GetLogicalDrives, 0) { return 1u << 2; }

static uint32_t current_directory(int wide)
{
    char dir[1024];
    void_cwd_get(dir, sizeof dir);
    uint32_t need = wide ? void_utf8_to_gw(dir, 0, 0) : void_utf8_to_ga(dir, 0, 0);
    if (ARG(0) < need) return need;
    return wide ? void_utf8_to_gw(dir, ARG(1), ARG(0)) : void_utf8_to_ga(dir, ARG(1), ARG(0));
}
WINAPI_EXPORT(K32, GetCurrentDirectoryA, 2) { return current_directory(0); }
WINAPI_EXPORT(K32, GetCurrentDirectoryW, 2) { return current_directory(1); }
static uint32_t set_current_directory(char *p)
{
    int r = p && void_cwd_set(p);
    free(p);
    if (!r) void_set_last_error(ERROR_PATH_NOT_FOUND_);
    return (uint32_t)r;
}
WINAPI_EXPORT(K32, SetCurrentDirectoryA, 1) { return set_current_directory(void_astr_utf8(ARG(0))); }
WINAPI_EXPORT(K32, SetCurrentDirectoryW, 1) { return set_current_directory(void_wstr_utf8(ARG(0))); }

/* Directory change notifications: a handle that is never signalled (the
 * game polls them with a zero timeout to notice new downloads). */
static uint32_t change_notification(void)
{
    void_object *o = void_object_new(VOID_OBJ_EVENT, 0);
    o->manual_reset = 1;
    return void_handle_new(o);
}
WINAPI_EXPORT(K32, FindFirstChangeNotificationA, 3) { return change_notification(); }
WINAPI_EXPORT(K32, FindFirstChangeNotificationW, 3) { return change_notification(); }
WINAPI_EXPORT(K32, FindNextChangeNotification, 1) { return 1; }
WINAPI_EXPORT(K32, FindCloseChangeNotification, 1) { return (uint32_t)void_handle_close(ARG(0)); }
WINAPI_EXPORT(K32, UnlockFileEx, 5) { return 1; }
