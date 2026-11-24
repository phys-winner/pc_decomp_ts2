/* ARM64 implementations of hooks from LazyDuchess's TS2 Extender 1.0.0 LC.
 * Inline instruction changes are applied to the lifted C by the build. */
#define _GNU_SOURCE
#include "host.h"
#include "sims2_extender.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

unsigned s2_extender_flags;
static int enabled, skip_intro, console, single_core, ui_scale, shadow_quality, imposter_quality, design_price, uniform_fix;
static int orbit_camera;
static int custom_content_allowed;
static double ui_reference, orbit_x, orbit_y;
const char s2_extender_downloads_dir[] = "C:\\Users\\Player\\Documents\\EA Games\\The Sims 2 Legacy\\Downloads";
static const char *const package_names[] = {
    "ld_dateStoodUpFix.package", "ld_timingFix.package", "ld_WallTopFix.package", "ld_TS2Extender_uniformFix.package",
};
static char package_paths[sizeof package_names / sizeof package_names[0]][1200];

const char *s2_extender_download_name(unsigned index)
{
    return enabled && index < sizeof package_names / sizeof package_names[0] ? package_names[index] : NULL;
}

const char *s2_extender_download_path(const char *name)
{
    if (!enabled) return NULL;
    for (unsigned i = 0; i < sizeof package_names / sizeof package_names[0]; ++i)
        if (*package_paths[i] && (i != 3 || uniform_fix) && !strcasecmp(name, package_names[i])) return package_paths[i];
    return NULL;
}

static int regular_file(const char *path)
{
    struct stat file;
    return !stat(path, &file) && S_ISREG(file.st_mode);
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    return text;
}

static int boolean(const char *value)
{
    if (!strcasecmp(value, "true") || !strcmp(value, "1")) return 1;
    if (!strcasecmp(value, "false") || !strcmp(value, "0")) return 0;
    return -1;
}

static int number(const char *value, double minimum, double maximum, double *result)
{
    char *end;
    errno = 0;
    double parsed = strtod(value, &end);
    if (end == value || *end || errno || !isfinite(parsed) || parsed < minimum || parsed > maximum) return 0;
    *result = parsed;
    return 1;
}

static void read_option(const char *section, const char *key, const char *value)
{
    static const struct { const char *section, *key; unsigned flag; } flags[] = {
        {"Enhancements", "Separates4All", S2_EXTENDER_SEPARATES},
        {"Enhancements", "FreeZodiac", S2_EXTENDER_ZODIAC},
        {"Fixes", "FixRNG", S2_EXTENDER_RNG},
        {"Fixes", "FixPinkFlashing", S2_EXTENDER_PINK},
        {"Fixes", "FixMakeupLag", S2_EXTENDER_MAKEUP},
        {"Fixes", "FixPoolShadows", S2_EXTENDER_POOL},
        {"Fixes", "FixOutdoorShadows", S2_EXTENDER_OUTDOOR},
        {"Fixes", "FixSun", S2_EXTENDER_SUN},
        {"Misc", "GenderedThumbnails", S2_EXTENDER_THUMBNAILS},
        {"Legacy", "OceanReflections", S2_EXTENDER_OCEAN},
        {"Advanced", "ExtendedSimAntics", S2_EXTENDER_SIMANTICS},
        {"Advanced", "DisableGroupsCache", S2_EXTENDER_GROUPS},
        {"Advanced", "ExtendedLua", S2_EXTENDER_LUA},
    };
    int on = boolean(value);
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
        if (strcasecmp(section, flags[i].section) || strcasecmp(key, flags[i].key)) continue;
        if (on == 1) s2_extender_flags |= flags[i].flag;
        else if (on == 0) s2_extender_flags &= ~flags[i].flag;
        return;
    }
    if (!strcasecmp(section, "General") && !strcasecmp(key, "SkipIntro") && on >= 0) skip_intro = on;
    if (!strcasecmp(section, "General") && !strcasecmp(key, "Console") && on >= 0) console = on;
    if (!strcasecmp(section, "Advanced") && !strcasecmp(key, "SingleCore") && on >= 0) single_core = on;
    if (!strcasecmp(section, "Fixes") && !strcasecmp(key, "FixOFBUniform") && on >= 0) uniform_fix = on;
    double parsed;
    if (!strcasecmp(section, "Sims 3 Camera")) {
        if (!strcasecmp(key, "Enabled") && on >= 0) orbit_camera = on;
        if (!strcasecmp(key, "XSensitivity") && number(value, -16, 16, &parsed)) orbit_x = parsed;
        if (!strcasecmp(key, "YSensitivity") && number(value, -16, 16, &parsed)) orbit_y = parsed;
        return;
    }
    if (strcasecmp(section, "Enhancements")) return;
    if (!strcasecmp(key, "UIScale") && on >= 0) ui_scale = on;
    if (!strcasecmp(key, "UIScaleResolution") && number(value, 1, 16384, &parsed)) ui_reference = parsed;
    if (!strcasecmp(key, "ShadowQuality") && number(value, 0, 3, &parsed) && parsed == (int)parsed) shadow_quality = (int)parsed;
    if (!strcasecmp(key, "ImposterQuality") && number(value, 0, 3, &parsed) && parsed == (int)parsed) imposter_quality = (int)parsed;
    if (!strcasecmp(key, "DesignToolPrice") && number(value, 0, INT_MAX, &parsed) && parsed == (int)parsed) design_price = (int)parsed;
}

static void read_ini(FILE *file)
{
    char section[48] = "", line[1024];
    int first = 1;
    while (fgets(line, sizeof line, file)) {
        char *start = line;
        if (first && !strncmp(start, "\xef\xbb\xbf", 3)) start += 3;
        first = 0;
        if (!strchr(start, '\n') && !feof(file)) {
            int ch;
            do { ch = fgetc(file); } while (ch != EOF && ch != '\n');
            continue;
        }
        char *comment = strpbrk(start, ";#");
        if (comment) *comment = 0;
        start = trim(start);
        if (*start == '[') {
            char *close = strchr(start + 1, ']');
            section[0] = 0;
            if (close && !close[1]) {
                *close = 0;
                snprintf(section, sizeof section, "%s", trim(start + 1));
            }
        } else {
            char *equals = strchr(start, '=');
            if (!equals || !*section) continue;
            *equals = 0;
            read_option(section, trim(start), trim(equals + 1));
        }
    }
}

static void limit_to_one_cpu(void)
{
    cpu_set_t cpus;
    if (!sched_getaffinity(0, sizeof cpus, &cpus)) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &cpus)) continue;
            CPU_ZERO(&cpus);
            CPU_SET(cpu, &cpus);
            if (!sched_setaffinity(0, sizeof cpus, &cpus)) {
                void_log("[ts2extender] guest threads use CPU %d", cpu);
                return;
            }
            break;
        }
    }
    void_log("[ts2extender] could not set the guest CPU affinity");
}

void s2_extender_init(const char *profile)
{
#ifdef VOID_RELEASE
    (void)profile;
#else
    const char *option = getenv("PORT_OPTION_TS2_EXTENDER");
    enabled = option && boolean(option) == 1;
    s2_extender_flags = 0;
    memset(package_paths, 0, sizeof package_paths);
    shadow_quality = imposter_quality = single_core = ui_scale = skip_intro = orbit_camera = console = 0;
    design_price = 15;
    uniform_fix = 1;
    ui_reference = 1080;
    orbit_x = orbit_y = 1;
    if (!enabled) {
        void_log("[ts2extender] off");
        return;
    }
    s2_extender_flags = S2_EXTENDER_RNG | S2_EXTENDER_PINK | S2_EXTENDER_MAKEUP | S2_EXTENDER_POOL |
        S2_EXTENDER_OUTDOOR | S2_EXTENDER_SUN | S2_EXTENDER_THUMBNAILS | S2_EXTENDER_SIMANTICS |
        S2_EXTENDER_GROUPS | S2_EXTENDER_LUA;
    skip_intro = 1;
    const char *assets = getenv("S2_TS2_EXTENDER_ASSETS");
    char path[1200];
    int len = snprintf(path, sizeof path, "%s/TS2Extender.ini", profile);
    FILE *file = len >= 0 && (size_t)len < sizeof path ? fopen(path, "rb") : NULL;
    const char *ini_source = file ? "profile" : "defaults";
    if (!file && assets && *assets) {
        len = snprintf(path, sizeof path, "%s/TS2Extender.ini", assets);
        if (len >= 0 && (size_t)len < sizeof path) file = fopen(path, "rb");
        if (file) ini_source = "ZIP";
    }
    if (file) {
        read_ini(file);
        fclose(file);
    }
    if (assets && *assets) {
        for (unsigned i = 0; i < sizeof package_names / sizeof package_names[0]; ++i) {
            len = snprintf(package_paths[i], sizeof package_paths[i], "%s/%s", assets, package_names[i]);
            if (len < 0 || (size_t)len >= sizeof package_paths[i] || !regular_file(package_paths[i]))
                package_paths[i][0] = 0;
        }
    }
    void_log("[ts2extender] on (LC 1.0.0), ini=%s, flags=%X, intro skip=%d, shadows=%d, imposters=%d, design price=%d",
             ini_source, s2_extender_flags, skip_intro, shadow_quality, imposter_quality, design_price);
    if (console) void_verbose = 1;
    if (single_core) limit_to_one_cpu();
#endif
}

void s2_extender_custom_content(int allowed) { custom_content_allowed = allowed; }

void s2_extender_note_package_open(const char *path)
{
    if (!enabled || !path) return;
    const char *name = strrchr(path, '\\');
    if (!name) name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (unsigned i = 0; i < sizeof package_names / sizeof package_names[0]; ++i) {
        if (strcasecmp(name, package_names[i])) continue;
        if (i == 3 && uniform_fix && custom_content_allowed) s2_extender_flags |= S2_EXTENDER_OFB;
        static unsigned logged;
        if (!(logged & (1u << i))) {
            logged |= 1u << i;
            void_log("[ts2extender] loaded %s", name);
        }
        return;
    }
}

void s2_extender_guest_init(void)
{
    if (s2_extender_flags & S2_EXTENDER_SIMANTICS) MEM32(0xA1016914u) = 500000;
}

int s2_extender_skips_intro(void) { return enabled && skip_intro; }
double s2_extender_ui_reference(void) { return ui_scale ? ui_reference : 0; }

void s2_extender_camera_sensitivity(float *yaw, float *pitch)
{
    if (orbit_camera) {
        *yaw *= (float)orbit_x;
        *pitch *= (float)orbit_y;
    }
}

void sub_A0C78B10_lifted(void);
void sub_A0C78B10(void)
{
    if (s2_extender_flags & S2_EXTENDER_LUA) MEM32(g_esp + 4) |= 0x3Eu;
    sub_A0C78B10_lifted();
}

void sub_A01B8D75_lifted(void);
void sub_A01B8D75(void)
{
    if ((s2_extender_flags & S2_EXTENDER_RNG) && !MEM32(g_ecx)) {
        struct timespec now = {0};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t seed = (uint32_t)((uint64_t)now.tv_sec * 1000000000 + now.tv_nsec);
        MEM32(g_ecx) = seed ? seed : 1;
    }
    sub_A01B8D75_lifted();
}

void sub_A02E63A0_lifted(void);
void sub_A02E63A0(void)
{
    uint32_t self = g_ecx;
    sub_A02E63A0_lifted();
    if (shadow_quality) {
        MEM32(self + 0x34) = MEM32(self + 0x38) = 128u << shadow_quality;
        MEM32(self + 0x2C) = shadow_quality == 1 ? 6 : 12;
    }
}

void sub_A0362A10_lifted(void);
void sub_A0362A10(void)
{
    uint32_t self = g_ecx;
    sub_A0362A10_lifted();
    if (imposter_quality) {
        MEM32(self + 0xA4) = MEM32(self + 0xA8) = imposter_quality == 3 ? 1024 : 512;
        MEM32(self + 0x138) = imposter_quality == 1 ? 32 : 64;
        MEM32(self + 0x14C) = imposter_quality == 1 ? 24 : 64;
    }
}

void sub_A06B1356_lifted(void);
void sub_A06B1356(void)
{
    if (enabled && (int32_t)MEM32(g_ecx + 0x54) > 0)
        MEM32(g_ecx + 0x54) = design_price;
    sub_A06B1356_lifted();
}

void sub_A01EA190_lifted(void);
void sub_A01EA190(void)
{
    if (s2_extender_flags & S2_EXTENDER_GROUPS) g_esp += 4;
    else sub_A01EA190_lifted();
}

void sub_A0BC6360_lifted(void);
void sub_A0BC6360(void)
{
    static int last_celestial;
    if (s2_extender_flags & S2_EXTENDER_SUN) {
        char *effect = void_astr_utf8(MEM32(g_esp + 4));
        if (effect) {
            int celestial = !strcmp(effect, "LotSunHolder") ? 1 : !strcmp(effect, "LotMoonHolder") ? 2 : 0;
            int duplicate = celestial && celestial == last_celestial;
            if (celestial || !strcmp(effect, "lotfx") || !strcmp(effect, "CASfx")) last_celestial = celestial;
            free(effect);
            if (duplicate) {
                g_eax &= 0xFFFFFF00u;
                g_esp += 12;
                return;
            }
        }
    }
    sub_A0BC6360_lifted();
}

/* Keep the CAS overlays on the original refresh path after a user's change. */
static int overlays_dirty;

void sub_A024FB30_lifted(void);
void sub_A024FB30(void)
{
    sub_A024FB30_lifted();
    overlays_dirty = 0;
}

void sub_A05F982C_lifted(void);
void sub_A05F982C(void)
{
    overlays_dirty = 1;
    sub_A05F982C_lifted();
}

void sub_A05FA6E6_lifted(void);
void sub_A05FA6E6(void)
{
    uint32_t message = MEM32(g_esp + 4);
    if ((s2_extender_flags & S2_EXTENDER_MAKEUP) && void_vm_readable(message, 8) && MEM32(message) == 3) {
        uint32_t control = MEM32(message + 4);
        if (control == 0x287259F6u || control == 0x028759F7u || control == 0x028759F8u) overlays_dirty = 1;
    }
    sub_A05FA6E6_lifted();
}

void s2_extender_note_cas_tab(void) { overlays_dirty = 1; }

void sub_A05FB453_lifted(void);
void sub_A05FB453(void)
{
    if ((s2_extender_flags & S2_EXTENDER_MAKEUP) && !overlays_dirty) {
        g_eax = (g_eax & 0xFFFFFF00u) | 1;
        g_esp += 8;
        return;
    }
    sub_A05FB453_lifted();
}

static int cancel_next_clothes;
static uint32_t hidden_clothes;

void sub_A06EFF84_lifted(void);
void sub_A06EFF84(void)
{
    if (s2_extender_flags & S2_EXTENDER_OFB) cancel_next_clothes = 1;
    sub_A06EFF84_lifted();
}

void sub_A06CF346_lifted(void);
void sub_A06CF346(void)
{
    uint32_t dialog = g_ecx, window = MEM32(g_esp + 4);
    int cancel = cancel_next_clothes;
    cancel_next_clothes = 0;
    sub_A06CF346_lifted();
    if (cancel) {
        void_regs saved;
        void_regs_save(&saved);
        void_call_guest_this(0xA06CDD5Au, dialog, 0, NULL);
        void_regs_load(&saved);
        if (void_vm_readable(window, 0xEC) && void_vm_writable(window + 0xE8, 4)) {
            MEM32(window + 0xE8) = 0;
            hidden_clothes = window;
        }
        g_eax = 0;
    }
}

int s2_extender_hidden_clothes(unsigned window)
{
    return window && window == hidden_clothes && MEM32(window + 0xE8) == 0;
}
