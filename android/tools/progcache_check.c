/* The GL program cache's folder housekeeping (program_cache_housekeeping in
 * android/host/d3d9_shader.c, extracted to housekeeping.inc by
 * tools/check_program_housekeeping.py) on a scratch folder on the device, with
 * a 64 KiB limit and 48 KiB to keep instead of 512/384 MiB:
 *   1. the first session with the code keeps the programs it finds, removes a
 *      .tmp left behind and writes the driver marker;
 *   2. the same driver again removes nothing;
 *   3. another driver removes every program and rewrites the marker;
 *   4. above the limit the oldest programs go, down to what is kept;
 * and in every step a file with another name, a symlink and a folder with a
 * program's name, and the symlink's target outside the folder, stay.
 * Build and run: tools/check_program_housekeeping.py --serial SERIAL. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM_FOLDER_LIMIT (64ull << 10)
#define PROGRAM_FOLDER_KEEP (48ull << 10)
static int logged;
void void_log(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
    ++logged;
}
#include "housekeeping.inc"

static const char *dir;
static char target[600];

static void path_of(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

static void put(const char *name, size_t bytes, time_t mtime)
{
    char path[700];
    path_of(path, sizeof path, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot create %s\n", path); exit(2); }
    for (size_t i = 0; i < bytes; ++i) fputc((int)(i & 0xFF), f);
    fclose(f);
    struct timespec times[2] = { { mtime, 0 }, { mtime, 0 } };
    utimensat(AT_FDCWD, path, times, 0);
}

static int present(const char *name)
{
    char path[700];
    struct stat st;
    path_of(path, sizeof path, name);
    return lstat(path, &st) == 0;
}

static int expect(int ok, const char *what)
{
    if (!ok) printf("FAIL: %s\n", what);
    return !ok;
}

static int marker_is(const char *want)
{
    char path[700], seen[64] = "";
    path_of(path, sizeof path, "driver");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    seen[fread(seen, 1, sizeof seen - 1, f)] = 0;
    fclose(f);
    return !strcmp(seen, want);
}

#define PA "0123456789abcdef0123456789abcde0.bin"
#define PB "0123456789abcdef0123456789abcde1.bin"
#define PC "0123456789abcdef0123456789abcde2.bin"
#define PA_TMP PA ".tmp"
#define LINK "0123456789abcdef0123456789abcdee.bin"
#define FOLDER "0123456789abcdef0123456789abcdef.bin"
#define OTHER_NAMES { "notes.txt", "0123456789ABCDEF0123456789abcdef.bin", "0123.bin", "off" }

/* The files that are not the cache's own must survive every step. */
static int strangers_stay(const char *step)
{
    static const char *names[] = OTHER_NAMES;
    int failures = 0;
    char what[200];
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        snprintf(what, sizeof what, "%s: %s was removed", step, names[i]);
        failures += expect(present(names[i]), what);
    }
    snprintf(what, sizeof what, "%s: the symlink with a program's name was removed", step);
    failures += expect(present(LINK), what);
    snprintf(what, sizeof what, "%s: the folder with a program's name was removed", step);
    failures += expect(present(FOLDER), what);
    struct stat st;
    snprintf(what, sizeof what, "%s: the symlink's target outside the folder was removed", step);
    failures += expect(lstat(target, &st) == 0, what);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: progcache_check FOLDER (created, must not exist)\n"); return 2; }
    dir = argv[1];
    if (mkdir(dir, 0700)) { printf("FAIL: cannot create %s\n", dir); return 2; }
    snprintf(target, sizeof target, "%s.target", dir);
    FILE *t = fopen(target, "wb");
    if (!t) { printf("FAIL: cannot create %s\n", target); return 2; }
    fputs("not the cache's\n", t);
    fclose(t);
    int failures = 0;
    static const char *names[] = OTHER_NAMES;
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) put(names[i], 10, 50);
    char path[700];
    path_of(path, sizeof path, LINK);
    if (symlink(target, path)) { printf("FAIL: symlink\n"); return 2; }
    path_of(path, sizeof path, FOLDER);
    mkdir(path, 0700);

    /* 1. The first session with the code. */
    put(PA, 1000, 100); put(PB, 1000, 200); put(PC, 1000, 300); put(PA_TMP, 10, 400);
    program_cache_housekeeping(dir, 0x1111u);
    failures += expect(present(PA) && present(PB) && present(PC), "first session: a program was removed");
    failures += expect(!present(PA_TMP), "first session: the .tmp left behind stayed");
    failures += expect(marker_is("0000000000001111\n"), "first session: no driver marker");
    failures += strangers_stay("first session");

    /* 2. The same driver. */
    int before = logged;
    program_cache_housekeeping(dir, 0x1111u);
    failures += expect(present(PA) && present(PB) && present(PC), "same driver: a program was removed");
    failures += expect(logged == before, "same driver: something was reported removed");
    failures += strangers_stay("same driver");

    /* 3. Another driver. */
    program_cache_housekeeping(dir, 0x2222u);
    failures += expect(!present(PA) && !present(PB) && !present(PC), "driver changed: a program stayed");
    failures += expect(marker_is("0000000000002222\n"), "driver changed: the marker was not rewritten");
    failures += strangers_stay("driver changed");

    /* 4. Above the limit: six programs of 16 KiB, oldest first; 96 KiB > 64, keep 48. */
    static const char *aged[] = { "00000000000000000000000000000001.bin", "00000000000000000000000000000002.bin",
        "00000000000000000000000000000003.bin", "00000000000000000000000000000004.bin",
        "00000000000000000000000000000005.bin", "00000000000000000000000000000006.bin" };
    for (int i = 0; i < 6; ++i) put(aged[i], 16 << 10, 1000 * (i + 1));
    program_cache_housekeeping(dir, 0x2222u);
    failures += expect(!present(aged[0]) && !present(aged[1]) && !present(aged[2]), "over the limit: an old program stayed");
    failures += expect(present(aged[3]) && present(aged[4]) && present(aged[5]), "over the limit: a recent program was removed");
    failures += strangers_stay("over the limit");

    printf(failures ? "FAILED %d\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
