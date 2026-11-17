/* Command-line entry for adb-shell bring-up on the device:
 *   sims2-arm64 --game DIR --data DIR --images DIR [--args "-w ..."] [--selftest] [-v]
 * The APK uses the same initialization through void_game_main. */
#define _GNU_SOURCE
#include "readcache.h"
#include "host.h"
#include "input.h"
#include "iostat.h"
#include "sims2_settings.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* Sims2EP9.exe: image base 0xA0000000, AddressOfEntryPoint 0x008248A3. */
#define GAME_ENTRY 0xA08248A3u

void void_files_std_init(void);
/* The activity's display size (android_main.c); the shell build has none. */
__attribute__((weak)) void void_android_screen_size(int *w, int *h) { *w = 0; *h = 0; }
extern const char *void_command_line;
static int exiting;
__attribute__((weak)) void void_android_game_exited(int code) { (void)code; }

void void_process_exit(uint32_t code, int run_detach)
{
    if (!exiting && run_detach) {
        exiting = 1;
        void_modules_detach_all(1);
    }
    void_readcache_flush_all();
    void_android_game_exited((int)code);
    void_log("[void] process exit %u", code);
    void_log_flush();
    _exit((int)code);
}

static int self_test(void)
{
    int failures = 0;
    uint32_t h = void_module_load("ijl15.dll");
    if (!h) { void_log("[selftest] ijl15.dll: attach FAILED"); ++failures; }
    else {
        uint32_t init = void_module_proc(h, void_gstrdup("ijlInit"));
        void_log("[selftest] ijl15.dll attached at %08X, ijlInit %08X", h, init);
        if (!init) ++failures;
        if (!void_module_free(h)) { void_log("[selftest] ijl15.dll: detach FAILED"); ++failures; }
    }
    void_log("[selftest] %s: %d failures", failures ? "FAIL" : "PASS", failures);
    return failures ? 71 : 0;
}

/* What the Steam install script (3314070_install.vdf) writes for the game,
 * for the packs present under the game root: each pack's executable key
 * names its folder, plus the collection's display name and language. */
#define UC_KEY "HKCU\\Software\\Electronic Arts\\The Sims 2 Ultimate Collection 25"
static const char *packs[][2] = {
    {"Sims2EP1.exe", "EP1"}, {"Sims2EP2.exe", "EP2"}, {"Sims2EP3.exe", "EP3"}, {"Sims2SP1.exe", "SP1"},
    {"Sims2SP2.exe", "SP2"}, {"Sims2EP4.exe", "EP4"}, {"Sims2EP5.exe", "EP5"}, {"Sims2SP4.exe", "SP4"},
    {"Sims2SP5.exe", "SP5"}, {"Sims2EP6.exe", "EP6"}, {"Sims2SP6.exe", "SP6"}, {"Sims2EP7.exe", "EP7"},
    {"Sims2SP7.exe", "SP7"}, {"Sims2EP8.exe", "EP8"}, {"Sims2EP9.exe", "EP9"},
};

static int pack_present(const char *folder)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s/TSData", void_root_dir(), folder);
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static void seed_registry(uint32_t language)
{
    char key[512], value[512], installed[1024] = "";
    for (size_t i = 0; i < sizeof packs / sizeof packs[0]; ++i) {
        if (!pack_present(packs[i][1])) continue;
        snprintf(key, sizeof key, "%s\\%s", UC_KEY, packs[i][0]);
        snprintf(value, sizeof value, "C:\\Games\\The Sims 2 Legacy\\%s", packs[i][1]);
        void_reg_set_string(key, "path", value);
        void_reg_seed_dword(key, "Installed", 1);
        if (!strcmp(packs[i][0], "Sims2EP9.exe"))
            void_reg_seed_string(key, "Game Registry", "Electronic Arts\\The Sims 2 Ultimate Collection 25");
        size_t n = strlen(installed);
        snprintf(installed + n, sizeof installed - n, "%s%s", n ? "," : "", packs[i][0]);
        void_log("[void] pack %s (%s) installed", packs[i][1], packs[i][0]);
    }
    void_reg_set_string(UC_KEY, "EPsInstalled", installed);
    void_reg_seed_string(UC_KEY, "DisplayName", "The Sims 2 Legacy");
    void_reg_set_dword(UC_KEY "\\1.0", "language", language);
}

int void_game_main(const char *game, const char *data, const char *images, int selftest)
{
    void_install_fault_handlers();
    mkdir(data, 0775);
    char log_path[1024];
    snprintf(log_path, sizeof log_path, "%s/sims2.log", data);
    void_log_open(log_path);
    void_log("[void] The Sims 2 ARM64 host: game=%s data=%s images=%s", game, data, images);
    void_log("[void] command line: %s", void_command_line);
    void_stat_start();
    if (!void_guest_reserve()) return 65;
    void_threads_init();
    void_dispatch_init();
    if (!void_files_init(game, data)) return 66;
    void_files_std_init();
    char overlay[1100];
    snprintf(overlay, sizeof overlay, "%s/overlay", images);
    void_files_overlay(overlay);
    void_registry_init(data);
    /* The screen the game is shown on: the activity's window, or S2_SCREEN
     * (WxH) for a shell run; the UI scale turns it into the game's size. */
    int screen_w = 0, screen_h = 0;
    if (getenv("S2_SCREEN")) sscanf(getenv("S2_SCREEN"), "%dx%d", &screen_w, &screen_h);
    else void_android_screen_size(&screen_w, &screen_h);
    s2_settings_init(data, screen_w, screen_h);
    seed_registry(s2_language_init(data));
    if (getenv("VOID_TIME_SCALE")) void_set_time_scale(strtod(getenv("VOID_TIME_SCALE"), NULL));
    char binaries[1100];
    snprintf(binaries, sizeof binaries, "%s/EP9/TSBin", game);
    if (!void_modules_init(binaries)) return 67;
    void_machine_enter();
    int code;
    if (getenv("S2_PROBE_MEMCMP")) {
        /* Differential probe of the lifted CRT memcmp (0xA0826B4B) against
         * the host's, over equal and differing buffers of every length 0..80. */
        uint32_t a = void_galloc(128), b = void_galloc(128);
        int bad = 0;
        for (uint32_t n = 0; n <= 80; ++n) {
            for (int diff = -1; diff < (int)n && diff < 40; diff += (diff < 0 ? 1 : 7)) {
                for (uint32_t i = 0; i < 128; ++i) MEM8(a + i) = MEM8(b + i) = (uint8_t)('A' + i % 26);
                if (diff >= 0) MEM8(b + (uint32_t)diff) = (uint8_t)(MEM8(b + (uint32_t)diff) + 1);
                uint32_t args[3] = { a, b, n };
                int32_t got = (int32_t)void_call_guest(0xA0826B4Bu, 3, args, 0);
                int want = memcmp(GPTR(a), GPTR(b), n);
                if ((got < 0) != (want < 0) || (got > 0) != (want > 0)) {
                    void_log("[probe] memcmp n=%u diff=%d: lifted %d, host %d", n, diff, got, want);
                    ++bad;
                }
            }
        }
        void_log("[probe] memcmp: %d mismatches", bad);
    }
    if (selftest) {
        code = self_test();
    } else {
        void_input_script_start();
        void_log("[void] starting lifted Sims2EP9.exe at %08X", GAME_ENTRY);
        code = (int)void_call_guest(GAME_ENTRY, 0, NULL, 1);
        void_log("[void] Sims2EP9.exe entry returned %d", code);
    }
    void_machine_leave();
    void_log_flush();
    return code;
}

#ifndef VOID_ANDROID_APP
int main(int argc, char **argv)
{
    const char *game = "/sdcard/Sims2/game", *data = "/sdcard/Sims2/userdata";
    const char *images = ".";
    const char *args = NULL;
    int selftest = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) images = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) args = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "-v")) void_verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 64; }
    }
    if (args && *args) {
        static char line[2048];
        snprintf(line, sizeof line, "\"C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin\\Sims2EP9.exe\" %s", args);
        void_command_line = line;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    int code = void_game_main(game, data, images, selftest);
    void_process_exit((uint32_t)code, 0);
    return code;
}
#endif
