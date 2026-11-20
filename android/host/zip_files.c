/* The launcher owns ZIP parsing and the descriptor; game paths stay rooted in its sidecar. */
#define _GNU_SOURCE
#include "host.h"
#include "zip_files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static zm_mount *game_zip;
static char zip_root[1024];

int s2_zip_init(const char *root)
{
    snprintf(zip_root, sizeof zip_root, "%s", root);
    game_zip = zm_from_env("game");
    if (game_zip) void_log("[zip] mounted %zu files, sidecar %s", zm_count(game_zip), root);
    else {
        const char *mounted = getenv("PORT_MOUNT_GAME");
        if (mounted && *mounted) {
            void_log("[zip] cannot open selected archive/index");
            return 0;
        }
    }
    return 1;
}

static const char *relative(const char *host)
{
    size_t n = strlen(zip_root);
    if (!game_zip || !host || strncmp(host, zip_root, n) || (host[n] && host[n] != '/')) return NULL;
    const char *p = host + n;
    if (*p == '/') ++p;
    return p;
}

int s2_zip_path(const char *host) { return relative(host) != NULL; }
const zm_entry *s2_zip_find(const char *host)
{
    const char *p = relative(host);
    const zm_entry *e = p ? zm_find(game_zip, p) : NULL;
    return e && !(e->flags & ZM_SIDECAR) ? e : NULL;
}

static int any_entry(const zm_entry *e, void *arg) { (void)e; (void)arg; return 1; }

int s2_zip_stat(const char *host, struct stat *st)
{
    char clean[2048];
    snprintf(clean, sizeof clean, "%s", host ? host : "");
    size_t n = strlen(clean);
    if (n >= 2 && !strcmp(clean + n - 2, "/.")) clean[n - 2] = 0;
    else if (n >= 3 && !strcmp(clean + n - 3, "/..")) {
        clean[n - 3] = 0;
        char *slash = strrchr(clean, '/');
        if (slash) *slash = 0;
    }
    host = clean;
    const char *p = relative(host);
    if (!p) { errno = ENOENT; return -1; }
    const zm_entry *e = zm_find(game_zip, p);
    if (!e && *p && !zm_list(game_zip, p, any_entry, NULL)) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    st->st_mode = e ? S_IFREG | 0664 : S_IFDIR | 0775;
    st->st_nlink = 1;
    /* Win32 file-information queries require a stable identity for each virtual object. */
    uint64_t identity = 1469598103934665603ull;
    for (const unsigned char *c = (const unsigned char *)p; *c; ++c)
        identity = (identity ^ (*c >= 'A' && *c <= 'Z' ? *c + ('a' - 'A') : *c)) * 1099511628211ull;
    st->st_ino = (ino_t)(identity | (1ull << 63));
    if (e) {
        st->st_ino = (ino_t)(e->offset + 1);
        st->st_size = (off_t)e->size;
        st->st_mtim.tv_sec = (time_t)(e->time / 1000u);
        st->st_mtim.tv_nsec = (long)(e->time % 1000u) * 1000000L;
        st->st_atim = st->st_ctim = st->st_mtim;
    }
    return 0;
}

typedef struct {
    size_t skip;
    void (*each)(void *, const char *, unsigned char);
    void *arg;
    char previous[1024];
} children;

static int child(const zm_entry *e, void *arg)
{
    children *c = arg;
    const char *p = e->path + c->skip;
    size_t n = e->length - c->skip;
    const char *slash = memchr(p, '/', n);
    if (slash) n = (size_t)(slash - p);
    if (!n || n >= sizeof c->previous) return 0;
    char name[1024];
    memcpy(name, p, n); name[n] = 0;
    if (strcasecmp(name, c->previous)) {
        c->each(c->arg, name, slash ? DT_DIR : DT_REG);
        snprintf(c->previous, sizeof c->previous, "%s", name);
    }
    return 0;
}

int s2_zip_list(const char *host, void (*each)(void *, const char *, unsigned char), void *arg)
{
    const char *p = relative(host);
    struct stat st;
    if (!p || s2_zip_stat(host, &st) || !S_ISDIR(st.st_mode)) return 0;
    children c = { .skip = *p ? strlen(p) + 1 : 0, .each = each, .arg = arg };
    zm_list(game_zip, p, child, &c);
    return 1;
}

long long s2_zip_read(const zm_entry *e, void *buffer, size_t size, uint64_t offset)
{
    size_t done = 0;
    while (done < size) {
        long long n = zm_pread(game_zip, e, (uint8_t *)buffer + done, size - done, offset + done);
        if (n < 0) return -1;
        if (!n) break;
        done += (size_t)n;
    }
    return (long long)done;
}

static int copy_to_fd(const zm_entry *e, int fd)
{
    uint8_t buffer[65536];
    for (uint64_t at = 0; at < e->size;) {
        size_t n = e->size - at < sizeof buffer ? (size_t)(e->size - at) : sizeof buffer;
        if (s2_zip_read(e, buffer, n, at) != (long long)n) { errno = EIO; return -1; }
        for (size_t done = 0; done < n;) {
            ssize_t put = write(fd, buffer + done, n - done);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) return -1;
            done += (size_t)put;
        }
        at += n;
    }
    return 0;
}

int s2_zip_copy(const zm_entry *e, const char *host)
{
    char temporary[2048];
    if (snprintf(temporary, sizeof temporary, "%s.zip-XXXXXX", host) >= (int)sizeof temporary) {
        errno = ENAMETOOLONG; return -1;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    int result = copy_to_fd(e, fd);
    if (!result && fsync(fd)) result = -1;
    int error = errno;
    if (close(fd) && !result) { result = -1; error = errno; }
    if (!result && rename(temporary, host)) { result = -1; error = errno; }
    if (result) unlink(temporary);
    errno = error;
    return result;
}

int s2_zip_memory_fd(const zm_entry *e)
{
    int fd = (int)syscall(__NR_memfd_create, "sims2-zip-view", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (copy_to_fd(e, fd) || lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    return fd;
}

uint8_t *s2_read_file(const char *host, size_t *size)
{
    FILE *file = fopen(host, "rb");
    const zm_entry *e = !file && (errno == ENOENT || errno == ENOTDIR) ? s2_zip_find(host) : NULL;
    struct stat st;
    if ((!file && !e) || (file && fstat(fileno(file), &st))) { if (file) fclose(file); return NULL; }
    uint64_t n = e ? e->size : (uint64_t)st.st_size;
    if (n > SIZE_MAX - 1) { if (file) fclose(file); return NULL; }
    uint8_t *data = malloc((size_t)n + 1);
    int ok = data && (e ? s2_zip_read(e, data, (size_t)n, 0) == (long long)n : fread(data, 1, (size_t)n, file) == n);
    if (file) fclose(file);
    if (!ok) { free(data); return NULL; }
    *size = (size_t)n;
    return data;
}
