/* The Vulkan driver of the native Vulkan renderer: the system's (libvulkan.so, the Android loader)
 * or an app-local Mesa driver through adrenotools, as the launcher chose (port.json modes "turnip"
 * and "panvk"; the player's own driver from the launcher's import, PORT_TURNIP_DRIVER and
 * PORT_PANVK_DRIVER, else the one the APK carries).
 *
 * adrenotools (BSD-2-Clause, the DXVK builds' helper and hooks) loads the driver into a linker
 * namespace of its own, so the system driver stays untouched and the app-local one is called
 * directly: no Android loader, the driver's own WSI presents (Mesa's Android surface). Its version
 * is the driver's, not the system's: a phone whose system driver offers Vulkan 1.0 still runs a
 * Turnip of Vulkan 1.3. The renderer (s2r_vulkan.c) resolves every function through the returned
 * vkGetInstanceProcAddr.
 *
 * Mesa's variables before the driver loads: Turnip renders without GMEM tiles on Adreno 6xx
 * (TU_DEBUG=sysmem: with tiles Turnip 26.0 hung or faulted an Adreno 650 within 10-100 s of a lot,
 * sysmem was faster; STATE.md, 2026-11-07); PanVK uses linear gralloc buffers on the two Mali
 * phones whose frames were corrupted without (begonia, TB373FU) and keeps its shader cache in the
 * app cache. <data>/../vulkan.env (/sdcard/Sims2/vulkan.env, also the older turnip.env) holds
 * NAME=VALUE lines for TU_, FD_, IR3_, PAN_, PANVK_, MESA_ and VK_ variables (diagnostics) and wins. */
#define VK_NO_PROTOTYPES 1
#include "s2r_internal.h"
#include <vulkan/vulkan.h>
#include "s2r_vkdriver.h"
#include "../launcher/native/panvk_identity.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOGVD(...) void_log("[s2r-vk] " __VA_ARGS__)

#define S2R_TURNIP_LIBRARY "libs2turnip.so"

static int driver_kind = S2R_VK_SYSTEM;
static char library_dir[600];

void s2r_set_vk_driver(int kind, const char *native_dir)
{
    driver_kind = kind;
    snprintf(library_dir, sizeof library_dir, "%s", native_dir ? native_dir : "");
}

void s2r_set_vk_kind(int kind) { driver_kind = kind; }

int s2r_vk_driver(void) { return driver_kind; }

const char *s2r_vk_driver_name(int kind)
{
    return kind == S2R_VK_TURNIP ? "Turnip" : kind == S2R_VK_PANVK ? "PanVK" : "system";
}

/* The player's own driver for the kind; NULL for the one the APK carries. */
static const char *custom_driver(int kind)
{
    const char *path = getenv(kind == S2R_VK_PANVK ? "PORT_PANVK_DRIVER" : "PORT_TURNIP_DRIVER");
    return path && path[0] ? path : NULL;
}

int s2r_vk_custom(void) { return driver_kind != S2R_VK_SYSTEM && custom_driver(driver_kind) != NULL; }

static void gpu_model(char *out, size_t size)
{
    out[0] = 0;
    FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (!f) return;
    if (!fgets(out, (int)size, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

static int mesa_name(const char *name)
{
    static const char *const prefixes[] = { "TU_", "FD_", "IR3_", "PAN_", "PANVK_", "MESA_", "VK_" };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; ++i)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}

static void environment_file(const char *path)
{
    FILE *in = fopen(path, "r");
    if (!in) return;
    char line[512];
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        char *value = strchr(line, '=');
        if (line[0] == '#' || !value || value == line) continue;
        *value++ = 0;
        if (!mesa_name(line)) continue;
        setenv(line, value, 1);
        LOGVD("%s: %s=%s", path, line, value);
    }
    fclose(in);
}

static void driver_environment(int kind)
{
    if (kind == S2R_VK_TURNIP) {
        char model[64];
        gpu_model(model, sizeof model);
        /* The launcher's switch (port.json "adreno6_sysmem", Renderer -> advanced settings, on by
           default); 0 lets Turnip choose GMEM tiles again. vulkan.env still wins either way. */
        const char *option = getenv("PORT_OPTION_ADRENO6_SYSMEM");
        int wanted = !option || strcmp(option, "0");
        if (!strncmp(model, "Adreno6", 7) && wanted) {
            setenv("TU_DEBUG", "sysmem", 0);
            LOGVD("%s: Turnip renders without GMEM tiles (TU_DEBUG=%s)", model, getenv("TU_DEBUG"));
        } else if (!strncmp(model, "Adreno6", 7)) {
            LOGVD("%s: the sysmem workaround is off in the launcher: Turnip may use GMEM tiles", model);
        }
    } else if (kind == S2R_VK_PANVK) {
        char product[PROP_VALUE_MAX] = "";
        __system_property_get("ro.product.device", product);
        if (!strcmp(product, "TB373FU") || !strcmp(product, "begonia")) setenv("MESA_GRALLOC_LINEAR", "1", 0);
    }
    const char *cache = s2r_cache_dir();
    if (cache && cache[0]) {
        char folder[700];
        snprintf(folder, sizeof folder, "%s/mesa", cache);
        mkdir(folder, 0700);
        setenv("MESA_SHADER_CACHE_DIR", folder, 0);
    }
    /* The profile's folder is <storage>/userdata...; the files sit beside it. */
    char base[600];
    snprintf(base, sizeof base, "%s", void_data_dir());
    char *slash = strrchr(base, '/');
    if (!slash) return;
    static const char *const names[] = { "/turnip.env", "/vulkan.env" };
    for (int i = 0; i < 2; ++i) {
        if (i == 0 && kind != S2R_VK_TURNIP) continue;
        snprintf(slash, sizeof base - (size_t)(slash - base), "%s", names[i]);
        environment_file(base);
    }
}

typedef void *(*adrenotools_open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);

/* The system loader, or adrenotools with the chosen driver. `error` says why none opened. */
PFN_vkGetInstanceProcAddr s2r_vk_open(int kind, char *error, size_t error_size)
{
    if (kind == S2R_VK_SYSTEM) {
        void *vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        PFN_vkGetInstanceProcAddr get = vulkan ? (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr") : NULL;
        if (!get) snprintf(error, error_size, "libvulkan.so: %s", vulkan ? "no vkGetInstanceProcAddr" : dlerror());
        return get;
    }
    const char *label = s2r_vk_driver_name(kind);
    if (!library_dir[0]) { snprintf(error, error_size, "%s: no native library folder", label); return NULL; }
    const char *custom = custom_driver(kind);
    if (kind == S2R_VK_PANVK) {
        const char *page = port_panvk_page_size_error(custom == NULL);
        if (page) { snprintf(error, error_size, "%s", page); return NULL; }
    }
    char hooks[700], helper_path[700], folder[700], file[256];
    snprintf(hooks, sizeof hooks, "%s/", library_dir);
    snprintf(helper_path, sizeof helper_path, "%s/libadrenotools.so", library_dir);
    if (custom) {
        const char *slash = strrchr(custom, '/');
        if (custom[0] != '/' || !slash || !slash[1]) { snprintf(error, error_size, "%s: %s is no file path", label, custom); return NULL; }
        snprintf(folder, sizeof folder, "%.*s", (int)(slash - custom + 1), custom);
        snprintf(file, sizeof file, "%s", slash + 1);
        if (access(custom, R_OK)) { snprintf(error, error_size, "%s: %s cannot be read", label, custom); return NULL; }
    } else {
        snprintf(folder, sizeof folder, "%s", hooks);
        snprintf(file, sizeof file, "%s", kind == S2R_VK_PANVK ? PORT_PANVK_DRIVER : S2R_TURNIP_LIBRARY);
    }
    driver_environment(kind);
    void *helper = dlopen(helper_path, RTLD_NOW | RTLD_LOCAL);
    adrenotools_open_fn open = helper ? (adrenotools_open_fn)dlsym(helper, "adrenotools_open_libvulkan") : NULL;
    if (!open) { snprintf(error, error_size, "%s: %s", label, helper ? "adrenotools_open_libvulkan missing" : dlerror()); return NULL; }
    /* 1 = ADRENOTOOLS_DRIVER_CUSTOM; no temporary folder (memfd on API 29+), no file redirection,
       no GPU mapping import. */
    void *driver = open(RTLD_NOW | RTLD_LOCAL, 1, NULL, hooks, folder, file, NULL, NULL);
    PFN_vkGetInstanceProcAddr get = driver ? (PFN_vkGetInstanceProcAddr)dlsym(driver, "vkGetInstanceProcAddr") : NULL;
    if (!get) {
        const char *detail = dlerror();
        snprintf(error, error_size, "%s: adrenotools loaded no driver from %s%s (%s)", label, folder, file, detail ? detail : "no library");
        return NULL;
    }
    LOGVD("%s driver %s%s (%s) through adrenotools", label, folder, file, custom ? "the player's" : "the APK's");
    return get;
}

/* PanVK: the driver the renderer talks to is the app's (its file is mapped), not a stand-in. */
int s2r_vk_panvk_mapped(void)
{
    const char *custom = custom_driver(S2R_VK_PANVK);
    char path[900];
    if (custom) snprintf(path, sizeof path, "%s", custom);
    else snprintf(path, sizeof path, "%s/%s", library_dir, PORT_PANVK_DRIVER);
    return port_panvk_mapped(path);
}
