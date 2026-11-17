/* HUD state for the touch layer (hud.h): which of the game's cursor and
 * helper images the last frame drew. The images are found by name in the
 * running release's data/Textures.vfs (LP2C container, the layout
 * tools/build_vfs_index.py reads) and recognised by size and FNV-1a 64 of
 * the file bytes when D3DX creates a texture from them. */
#include "host.h"
#include "hud.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct { const char *name; uint32_t tag; } wanted[] = {
    {"textures\\cursor_paint.tex", HUD_CURSOR},
    {"textures\\cursor_paint_with_limfa.tex", HUD_CURSOR_LYMPHA},
    {"textures\\ui\\helper\\ui_helper_use.tex", HUD_USE},
    {"textures\\ui\\helper\\ui_helper_use_arrow.tex", HUD_USE_ARROW},
    {"textures\\ui\\helper\\ui_helper_paint.tex", HUD_PAINT},
    {"textures\\ui\\helper\\ui_helper_trap.tex", HUD_TRAP},
    {"textures\\ui\\helper\\ui_helper_wait.tex", HUD_WAIT},
    {"textures\\ui\\helper\\ui_helper_wait_wheel.tex", HUD_WAIT},
    {"textures\\ui\\helper\\ui_helper_death.tex", HUD_DEATH},
    {"textures\\ui\\helper\\ui_helper_not_enough_limfa.tex", HUD_NO_LYMPHA},
    {"textures\\ui\\helper\\ui_helper_not_enough_limfa_col.tex", HUD_NO_LYMPHA},
    {"textures\\ui\\helper\\ui_helper_questlog.tex", HUD_QUESTLOG},
};
#define WANTED (int)(sizeof wanted / sizeof wanted[0])

static struct { uint32_t offset, size, tag; uint64_t hash; } known[WANTED];
static int known_count;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint32_t frame_tags;
static _Atomic uint32_t shown_tags, shown_frame;

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* One directory level: its files, then its subdirectories. */
static int read_dir(FILE *f, char *path, size_t len, uint32_t dirs, uint32_t files, int depth)
{
    if (depth > 32) return 0;
    for (uint32_t i = 0; i < files; ++i) {
        int n = fgetc(f);
        char name[256];
        uint32_t meta[6];
        if (n < 0 || fread(name, 1, (size_t)n, f) != (size_t)n || fread(meta, 4, 6, f) != 6) return 0;
        name[n] = 0;
        if (len + 1 + (size_t)n + 1 > 1024) continue;
        snprintf(path + len, 1024 - len, "\\%s", name);
        for (int w = 0; w < WANTED; ++w) {
            if (strcasecmp(path, wanted[w].name)) continue;
            if (known_count < WANTED) {
                known[known_count].size = meta[0];
                known[known_count].offset = meta[2];
                known[known_count].tag = wanted[w].tag;
                ++known_count;
            }
        }
        path[len] = 0;
    }
    for (uint32_t i = 0; i < dirs; ++i) {
        int n = fgetc(f);
        char name[256];
        uint32_t counts[2];
        if (n < 0 || fread(name, 1, (size_t)n, f) != (size_t)n || fread(counts, 4, 2, f) != 2) return 0;
        name[n] = 0;
        if (len + 1 + (size_t)n + 1 > 1024) return 0;
        snprintf(path + len, 1024 - len, "\\%s", name);
        if (!read_dir(f, path, len + 1 + (size_t)n, counts[0], counts[1], depth + 1)) return 0;
        path[len] = 0;
    }
    return 1;
}

static void load_table(void)
{
    char dir[1100], file[1400];
    snprintf(dir, sizeof dir, "%s/data", void_root_dir());
    file[0] = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, "Textures.vfs")) { snprintf(file, sizeof file, "%s/%s", dir, e->d_name); break; }
        closedir(d);
    }
    FILE *f = file[0] ? fopen(file, "rb") : NULL;
    if (!f) { void_log("[hud] no data/Textures.vfs: cursor and helpers are not recognised"); return; }
    char magic[4];
    uint32_t counts[2];
    char path[1024] = "textures";
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "LP2C", 4) || fread(counts, 4, 2, f) != 2 ||
        !read_dir(f, path, strlen(path), counts[0], counts[1], 0))
        void_log("[hud] %s: unreadable directory", file);
    for (int i = 0; i < known_count; ++i) {
        uint8_t *data = malloc(known[i].size);
        if (data && fseek(f, (long)known[i].offset, SEEK_SET) == 0 && fread(data, 1, known[i].size, f) == known[i].size)
            known[i].hash = fnv64(data, known[i].size);
        else
            known[i].tag = 0;
        free(data);
    }
    fclose(f);
    void_log("[hud] %d cursor/helper images recognised by content", known_count);
}

uint32_t void_hud_tag_for(const uint8_t *data, uint32_t size)
{
    pthread_once(&once, load_table);
    uint64_t h = 0;
    int hashed = 0;
    uint32_t tag = 0;
    for (int i = 0; i < known_count; ++i) {
        if (known[i].size != size || !known[i].tag) continue;
        if (!hashed) { h = fnv64(data, size); hashed = 1; }
        if (h == known[i].hash) tag |= known[i].tag;
    }
    if (tag) VLOG("[hud] texture of %u bytes tagged %X", size, tag);
    return tag;
}

/* Draws and presents happen on the rendering thread. */
void void_hud_draw(uint32_t tags) { frame_tags |= tags; }

void void_hud_present(void)
{
    atomic_store(&shown_tags, frame_tags);
    atomic_fetch_add(&shown_frame, 1);
    frame_tags = 0;
}

uint32_t void_hud_state(uint32_t *frame)
{
    if (frame) *frame = atomic_load(&shown_frame);
    return atomic_load(&shown_tags);
}
