/* Actual block cache, with a writer scheduled while the reader yields for pread. */
#define RC_SLOTS 8u
#include "../android/host/readcache.c"
#include <assert.h>
#include <stdarg.h>

static const char *test_path;
static int test_fd, replacement_fd = -1, mutation;
static uint64_t test_key;
static const off_t test_offset = RC_BLOCK + 123;

const char *void_data_dir(void) { return "/data/local/tmp"; }
void void_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}
uint64_t void_stat_now(void) { return 1; }
void void_stat_add(int kind, uint64_t ns, uint64_t bytes) { (void)kind; (void)ns; (void)bytes; }
void dircache_changed(const char *path) { (void)path; }
void void_blocking_begin(void) {}
void void_blocking_end(void)
{
    int mode = mutation;
    mutation = 0;
    char replacement[32];
    memset(replacement, 'B', sizeof replacement);
    if (mode == 1) {
        assert(void_readcache_pwrite(test_fd, test_key, test_path, replacement, sizeof replacement, test_offset) == sizeof replacement);
        void_readcache_flush_key(test_key);
    } else if (mode == 2) {
        assert(pwrite(test_fd, replacement, sizeof replacement, test_offset) == sizeof replacement);
        void_readcache_changed_key(test_key);
    } else if (mode == 3 || mode == 4) {
        assert(!unlink(test_path));
        replacement_fd = open(test_path, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(replacement_fd >= 0);
        assert(!ftruncate(replacement_fd, RC_BLOCK * 2));
        assert(pwrite(replacement_fd, replacement, sizeof replacement, test_offset) == sizeof replacement);
        if (mode == 4) void_readcache_clear();
        else void_readcache_changed_key(test_key);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    test_path = argv[1];
    int mode = atoi(argv[2]);
    assert(mode >= 1 && mode <= 4);
    test_fd = open(test_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(test_fd >= 0);
    char original[RC_BLOCK * 2];
    memset(original, 'A', sizeof original);
    assert(write(test_fd, original, sizeof original) == sizeof original);
    test_key = void_readcache_key(test_path);
    rc_state = 1;
    flusher_started = 1;
    mutation = mode;
    char readback[32];
    ssize_t result = void_readcache_pread(test_fd, test_key, readback, sizeof readback, test_offset);
    if (mode == 1) assert(result == sizeof readback);
    else {
        assert(result == -2 && find(test_key, 1) < 0);
        /* ReadFile's uncached fallback keeps the original handle's inode. */
        assert(pread(test_fd, readback, sizeof readback, test_offset) == sizeof readback);
    }
    for (size_t i = 0; i < sizeof readback; ++i) assert(readback[i] == (mode >= 3 ? 'A' : 'B'));
    if (mode >= 3) {
        assert(void_readcache_pread(replacement_fd, test_key, readback, sizeof readback, test_offset) == sizeof readback);
        for (size_t i = 0; i < sizeof readback; ++i) assert(readback[i] == 'B');
        close(replacement_fd);
    }
    assert(rc_misses == (mode == 2 ? 1 : 2));
    void_readcache_clear();
    close(test_fd);
    assert(!unlink(test_path));
    printf("PASS: in-flight read coherency: %s\n", mode == 1 ? "partial write-back retries" :
           mode == 2 ? "generation change falls back to a fresh read" :
           mode == 3 ? "replaced inode never poisons the new file's cache" : "rename cache clear rejects in-flight reads from old paths");
}
