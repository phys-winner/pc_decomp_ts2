/* advapi32 registry: a small writable hive kept in memory and saved to
 * <data>/registry.txt after every change.
 *
 * Keys are full paths under HKLM/HKCU/HKCR ("HKCU\Software\..."), compared
 * case-insensitively; the 32-bit view folds Software\WOW6432Node into
 * Software. main.c seeds what the Steam install script writes for the game
 * (void_reg_seed_*); values the game writes itself persist across runs. */
#define _GNU_SOURCE
#include "host.h"
#include <ctype.h>
#include <errno.h>
#include <strings.h>

#define HKEY_CLASSES_ROOT_ 0x80000000u
#define HKEY_CURRENT_USER_ 0x80000001u
#define HKEY_LOCAL_MACHINE_ 0x80000002u
#define HKEY_USERS_ 0x80000003u
#define REG_NONE_ 0u
#define REG_SZ_ 1u
#define REG_EXPAND_SZ_ 2u
#define REG_BINARY_ 3u
#define REG_DWORD_ 4u
#define REG_MULTI_SZ_ 7u
#define REG_CREATED_NEW_KEY_ 1u
#define REG_OPENED_EXISTING_KEY_ 2u

typedef struct reg_val {
    char name[128];
    uint32_t type;
    uint32_t size;
    uint8_t *data;             /* strings are stored as UTF-8 with terminator */
} reg_val;

typedef struct reg_key {
    char path[512];
    reg_val *values;
    int count, cap;
} reg_key;

static reg_key *keys;
static int key_count, key_cap;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static char store_path[1100];
static int loading;

static const char *root_name(uint32_t root)
{
    switch (root) {
    case HKEY_CLASSES_ROOT_: return "HKCR";
    case HKEY_CURRENT_USER_: return "HKCU";
    case HKEY_LOCAL_MACHINE_: return "HKLM";
    case HKEY_USERS_: return "HKU";
    default: return NULL;
    }
}

static void normalize_path(char *path)
{
    char *wow;
    while ((wow = strcasestr(path, "\\WOW6432Node"))) memmove(wow, wow + 12, strlen(wow + 12) + 1);
    size_t n = strlen(path);
    while (n && path[n - 1] == '\\') path[--n] = 0;
    /* Collapse doubled separators. */
    char *w = path;
    for (char *r = path; *r; ++r) {
        if (*r == '\\' && w > path && w[-1] == '\\') continue;
        *w++ = *r;
    }
    *w = 0;
}

static reg_key *find_key(const char *path)
{
    for (int i = 0; i < key_count; ++i) if (!strcasecmp(keys[i].path, path)) return &keys[i];
    return NULL;
}

static reg_key *create_key(const char *path, int *created)
{
    reg_key *k = find_key(path);
    if (k) { if (created) *created = 0; return k; }
    /* Parents exist implicitly: create them too, so enumeration sees them. */
    char parent[512];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '\\');
    if (slash) { *slash = 0; if (!find_key(parent)) create_key(parent, NULL); }
    if (key_count == key_cap) {
        key_cap = key_cap ? key_cap * 2 : 64;
        keys = (reg_key *)realloc(keys, (size_t)key_cap * sizeof *keys);
    }
    k = &keys[key_count++];
    memset(k, 0, sizeof *k);
    snprintf(k->path, sizeof k->path, "%s", path);
    if (created) *created = 1;
    return k;
}

static reg_val *find_value(reg_key *k, const char *name)
{
    for (int i = 0; i < k->count; ++i) if (!strcasecmp(k->values[i].name, name ? name : "")) return &k->values[i];
    return NULL;
}

static void save_store(void);

static void set_value(reg_key *k, const char *name, uint32_t type, const void *data, uint32_t size)
{
    reg_val *v = find_value(k, name);
    if (!v) {
        if (k->count == k->cap) {
            k->cap = k->cap ? k->cap * 2 : 8;
            k->values = (reg_val *)realloc(k->values, (size_t)k->cap * sizeof *k->values);
        }
        v = &k->values[k->count++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "%s", name ? name : "");
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = (uint8_t *)malloc(size ? size : 1);
    if (size) memcpy(v->data, data, size);
    if (!loading) save_store();
}

static int is_string(uint32_t type) { return type == REG_SZ_ || type == REG_EXPAND_SZ_ || type == REG_MULTI_SZ_; }

/* ------------------------------------------------------------ persistence */
static void hex_out(FILE *f, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) fprintf(f, "%02x", data[i]);
}

static void save_store(void)
{
    if (!store_path[0]) return;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", store_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < key_count; ++i) {
        fprintf(f, "[%s]\n", keys[i].path);
        for (int j = 0; j < keys[i].count; ++j) {
            reg_val *v = &keys[i].values[j];
            fprintf(f, "%u|", v->type);
            hex_out(f, (const uint8_t *)v->name, (uint32_t)strlen(v->name));
            fputc('|', f);
            hex_out(f, v->data, v->size);
            fputc('\n', f);
        }
    }
    fclose(f);
    rename(tmp, store_path);
}

static uint32_t hex_in(const char *s, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) && n < cap) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

void void_registry_init(const char *data_dir)
{
    snprintf(store_path, sizeof store_path, "%s/registry.txt", data_dir);
    FILE *f = fopen(store_path, "r");
    if (!f) return;
    loading = 1;
    static char line[1 << 16];
    reg_key *k = NULL;
    static uint8_t buf[1 << 15];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0] == '[' && n > 1 && line[n - 1] == ']') {
            line[n - 1] = 0;
            k = create_key(line + 1, NULL);
            continue;
        }
        char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (!k || !a || !b) continue;
        uint32_t type = (uint32_t)strtoul(line, NULL, 10);
        char name[128];
        *b = 0;
        uint32_t nl = hex_in(a + 1, (uint8_t *)name, sizeof name - 1);
        name[nl] = 0;
        uint32_t size = hex_in(b + 1, buf, sizeof buf);
        set_value(k, name, type, buf, size);
    }
    fclose(f);
    loading = 0;
    void_log("[void] registry: %d keys from %s", key_count, store_path);
}

/* Host-side seeding (main.c): only fills values that are not there yet, so
 * whatever the game itself wrote wins. */
void void_reg_seed_string(const char *path, const char *name, const char *text)
{
    pthread_mutex_lock(&reg_lock);
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    normalize_path(p);
    reg_key *k = create_key(p, NULL);
    if (!find_value(k, name)) set_value(k, name, REG_SZ_, text, (uint32_t)strlen(text) + 1);
    pthread_mutex_unlock(&reg_lock);
}

/* Host facts that must follow the device (which packs are installed): always written. */
void void_reg_set_string(const char *path, const char *name, const char *text)
{
    pthread_mutex_lock(&reg_lock);
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    normalize_path(p);
    reg_key *k = create_key(p, NULL);
    reg_val *v = find_value(k, name);
    if (!v || v->type != REG_SZ_ || v->size != strlen(text) + 1 || memcmp(v->data, text, v->size))
        set_value(k, name, REG_SZ_, text, (uint32_t)strlen(text) + 1);
    pthread_mutex_unlock(&reg_lock);
}

void void_reg_seed_dword(const char *path, const char *name, uint32_t value)
{
    pthread_mutex_lock(&reg_lock);
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    normalize_path(p);
    reg_key *k = create_key(p, NULL);
    if (!find_value(k, name)) set_value(k, name, REG_DWORD_, &value, 4);
    pthread_mutex_unlock(&reg_lock);
}

void void_reg_set_dword(const char *path, const char *name, uint32_t value)
{
    pthread_mutex_lock(&reg_lock);
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    normalize_path(p);
    reg_key *k = create_key(p, NULL);
    reg_val *v = find_value(k, name);
    if (!v || v->type != REG_DWORD_ || v->size != 4 || memcmp(v->data, &value, 4))
        set_value(k, name, REG_DWORD_, &value, 4);
    pthread_mutex_unlock(&reg_lock);
}

/* --------------------------------------------------------------- handles */
typedef struct { char path[512]; } regkey_t;
static void regkey_destroy(void_object *o) { (void)o; }

/* The key path a handle (predefined root or opened key) names; NULL if invalid. */
static int handle_path(uint32_t handle, char *out, size_t cap)
{
    const char *root = root_name(handle);
    if (root) { snprintf(out, cap, "%s", root); return 1; }
    void_object *o = void_handle_get(handle, VOID_OBJ_OTHER);
    if (!o || o->destroy != regkey_destroy) return 0;
    snprintf(out, cap, "%s", ((regkey_t *)o->data)->path);
    return 1;
}

static uint32_t open_key(uint32_t parent, const char *sub, uint32_t out, int create, uint32_t disposition)
{
    char path[512];
    if (!handle_path(parent, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    if (sub && *sub) {
        size_t n = strlen(path);
        snprintf(path + n, sizeof path - n, "\\%s", sub);
    }
    normalize_path(path);
    pthread_mutex_lock(&reg_lock);
    reg_key *k = find_key(path);
    int created = 0;
    if (!k && create) {
        k = create_key(path, &created);
        save_store();
    }
    pthread_mutex_unlock(&reg_lock);
    if (!k && !root_name(parent) && !(sub && *sub)) k = NULL;
    if (!k) {
        /* The roots and their first level always exist. */
        if (!strchr(path, '\\') || !strcasecmp(path, "HKLM\\Software") || !strcasecmp(path, "HKCU\\Software")) {
            pthread_mutex_lock(&reg_lock);
            k = create_key(path, NULL);
            pthread_mutex_unlock(&reg_lock);
        } else {
            VLOG("[void] RegOpenKey(%s): not found", path);
            return ERROR_FILE_NOT_FOUND_;
        }
    }
    void_object *o = void_object_new(VOID_OBJ_OTHER, sizeof(regkey_t));
    o->destroy = regkey_destroy;
    snprintf(((regkey_t *)o->data)->path, sizeof ((regkey_t *)o->data)->path, "%s", path);
    MEM32(out) = void_handle_new(o);
    if (disposition) MEM32(disposition) = created ? REG_CREATED_NEW_KEY_ : REG_OPENED_EXISTING_KEY_;
    VLOG("[void] RegOpenKey(%s) = %08X%s", path, MEM32(out), created ? " (created)" : "");
    return ERROR_SUCCESS_;
}

/* Copy a value to the guest: strings convert to ANSI (cp1252) or UTF-16. */
static uint32_t query_value(uint32_t key, const char *name, uint32_t type_out, uint32_t data, uint32_t size_ptr, int wide)
{
    char path[512];
    if (!handle_path(key, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    pthread_mutex_lock(&reg_lock);
    reg_key *k = find_key(path);
    reg_val *v = k ? find_value(k, name) : NULL;
    if (!v) {
        pthread_mutex_unlock(&reg_lock);
        VLOG("[void] RegQueryValue(%s\\%s): not found", path, name ? name : "");
        return ERROR_FILE_NOT_FOUND_;
    }
    uint32_t type = v->type, need = v->size;
    char *text = NULL;
    if (is_string(type)) {
        text = (char *)malloc(v->size + 1);
        memcpy(text, v->data, v->size);
        text[v->size] = 0;
        need = wide ? 2u * (void_utf8_to_gw(text, 0, 0)) : void_utf8_to_ga(text, 0, 0);
        if (type == REG_MULTI_SZ_) need = wide ? 2u * v->size : v->size;   /* ASCII lists */
    }
    uint8_t *copy = (uint8_t *)malloc(v->size ? v->size : 1);
    memcpy(copy, v->data, v->size);
    uint32_t raw_size = v->size;
    pthread_mutex_unlock(&reg_lock);
    uint32_t result = ERROR_SUCCESS_;
    if (type_out) MEM32(type_out) = type;
    if (size_ptr) {
        uint32_t cap = MEM32(size_ptr);
        MEM32(size_ptr) = need;
        if (data) {
            if (cap < need) result = ERROR_MORE_DATA_;
            else if (type == REG_MULTI_SZ_) {
                for (uint32_t i = 0; i < raw_size; ++i) {
                    if (wide) MEM16(data + 2 * i) = copy[i]; else MEM8(data + i) = copy[i];
                }
            } else if (text) {
                if (wide) void_utf8_to_gw(text, data, need / 2);
                else void_utf8_to_ga(text, data, need);
            } else {
                memcpy(GPTR(data), copy, raw_size);
            }
        }
    } else if (data) {
        result = ERROR_INVALID_PARAMETER_;
    }
    VLOG("[void] RegQueryValue(%s\\%s) type %u size %u -> %u", path, name ? name : "", type, need, result);
    free(text);
    free(copy);
    return result;
}

static uint32_t store_value(uint32_t key, const char *name, uint32_t type, uint32_t data, uint32_t size, int wide)
{
    char path[512];
    if (!handle_path(key, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    uint8_t *bytes = NULL;
    uint32_t n = size;
    if (is_string(type) && type != REG_MULTI_SZ_) {
        char *s = wide ? void_wstr_utf8(data) : void_astr_utf8(data);
        if (!s) s = strdup("");
        bytes = (uint8_t *)s;
        n = (uint32_t)strlen(s) + 1;
    } else {
        bytes = (uint8_t *)malloc(size ? size : 1);
        if (size) memcpy(bytes, GPTR(data), size);
        if (type == REG_MULTI_SZ_ && wide) {   /* keep ASCII lists as bytes */
            for (uint32_t i = 0; i < size / 2; ++i) bytes[i] = (uint8_t)MEM16(data + 2 * i);
            n = size / 2;
        }
    }
    pthread_mutex_lock(&reg_lock);
    reg_key *k = create_key(path, NULL);
    set_value(k, name, type, bytes, n);
    pthread_mutex_unlock(&reg_lock);
    VLOG("[void] RegSetValue(%s\\%s) type %u size %u", path, name ? name : "", type, n);
    free(bytes);
    return ERROR_SUCCESS_;
}

#define ADV "advapi32.dll"
WINAPI_EXPORT(ADV, RegOpenKeyExW, 5)
{
    char *sub = void_wstr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(4), 0, 0);
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegOpenKeyExA, 5)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(4), 0, 0);
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegOpenKeyA, 3)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(2), 0, 0);
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegCreateKeyExA, 9)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(7), 1, ARG(8));
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegCreateKeyExW, 9)
{
    char *sub = void_wstr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(7), 1, ARG(8));
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegCreateKeyA, 3)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = open_key(ARG(0), sub, ARG(2), 1, 0);
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegQueryValueExW, 6)
{
    char *name = void_wstr_utf8(ARG(1));
    uint32_t r = query_value(ARG(0), name, ARG(3), ARG(4), ARG(5), 1);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegQueryValueExA, 6)
{
    char *name = void_astr_utf8(ARG(1));
    uint32_t r = query_value(ARG(0), name, ARG(3), ARG(4), ARG(5), 0);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegSetValueExA, 6)
{
    char *name = void_astr_utf8(ARG(1));
    uint32_t r = store_value(ARG(0), name, ARG(3), ARG(4), ARG(5), 0);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegSetValueExW, 6)
{
    char *name = void_wstr_utf8(ARG(1));
    uint32_t r = store_value(ARG(0), name, ARG(3), ARG(4), ARG(5), 1);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegDeleteValueA, 2)
{
    char path[512];
    if (!handle_path(ARG(0), path, sizeof path)) return ERROR_INVALID_HANDLE_;
    char *name = void_astr_utf8(ARG(1));
    uint32_t r = ERROR_FILE_NOT_FOUND_;
    pthread_mutex_lock(&reg_lock);
    reg_key *k = find_key(path);
    reg_val *v = k ? find_value(k, name) : NULL;
    if (v) {
        free(v->data);
        *v = k->values[--k->count];
        save_store();
        r = ERROR_SUCCESS_;
    }
    pthread_mutex_unlock(&reg_lock);
    free(name);
    return r;
}
static uint32_t delete_key(uint32_t parent, const char *sub)
{
    char path[512];
    if (!handle_path(parent, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    if (sub && *sub) { size_t n = strlen(path); snprintf(path + n, sizeof path - n, "\\%s", sub); }
    normalize_path(path);
    uint32_t r = ERROR_FILE_NOT_FOUND_;
    pthread_mutex_lock(&reg_lock);
    reg_key *k = find_key(path);
    if (k) {
        for (int i = 0; i < k->count; ++i) free(k->values[i].data);
        free(k->values);
        *k = keys[--key_count];
        save_store();
        r = ERROR_SUCCESS_;
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}
WINAPI_EXPORT(ADV, RegDeleteKeyA, 2)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = delete_key(ARG(0), sub);
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegDeleteKeyW, 2)
{
    char *sub = void_wstr_utf8(ARG(1));
    uint32_t r = delete_key(ARG(0), sub);
    free(sub);
    return r;
}

/* Direct subkeys of a path, in creation order. */
static int subkey_at(const char *path, uint32_t index, char *name, size_t cap)
{
    size_t n = strlen(path);
    uint32_t seen = 0;
    for (int i = 0; i < key_count; ++i) {
        const char *p = keys[i].path;
        if (strncasecmp(p, path, n) || p[n] != '\\' || strchr(p + n + 1, '\\')) continue;
        if (seen++ == index) { snprintf(name, cap, "%s", p + n + 1); return 1; }
    }
    return 0;
}

static uint32_t enum_key(uint32_t key, uint32_t index, uint32_t name, uint32_t name_size, int wide)
{
    char path[512], sub[256];
    if (!handle_path(key, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    pthread_mutex_lock(&reg_lock);
    int found = subkey_at(path, index, sub, sizeof sub);
    pthread_mutex_unlock(&reg_lock);
    if (!found) return ERROR_NO_MORE_ITEMS_;
    uint32_t cap = name_size ? MEM32(name_size) : 0;
    uint32_t len = (uint32_t)strlen(sub);
    if (len + 1 > cap) return ERROR_MORE_DATA_;
    if (wide) void_utf8_to_gw(sub, name, cap); else void_utf8_to_ga(sub, name, cap);
    MEM32(name_size) = len;
    return ERROR_SUCCESS_;
}
WINAPI_EXPORT(ADV, RegEnumKeyExA, 8) { return enum_key(ARG(0), ARG(1), ARG(2), ARG(3), 0); }
WINAPI_EXPORT(ADV, RegEnumKeyExW, 8) { return enum_key(ARG(0), ARG(1), ARG(2), ARG(3), 1); }
WINAPI_EXPORT(ADV, RegEnumKeyA, 4)
{
    uint32_t size_slot = void_galloc(4);
    MEM32(size_slot) = ARG(3);
    uint32_t r = enum_key(ARG(0), ARG(1), ARG(2), size_slot, 0);
    void_gfree(size_slot);
    return r;
}

static uint32_t enum_value(uint32_t key, uint32_t index, uint32_t name, uint32_t name_size, uint32_t type_out,
                           uint32_t data, uint32_t data_size, int wide)
{
    char path[512];
    if (!handle_path(key, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    pthread_mutex_lock(&reg_lock);
    reg_key *k = find_key(path);
    if (!k || index >= (uint32_t)k->count) { pthread_mutex_unlock(&reg_lock); return ERROR_NO_MORE_ITEMS_; }
    char value_name[128];
    snprintf(value_name, sizeof value_name, "%s", k->values[index].name);
    pthread_mutex_unlock(&reg_lock);
    uint32_t cap = name_size ? MEM32(name_size) : 0;
    uint32_t len = (uint32_t)strlen(value_name);
    if (len + 1 > cap) return ERROR_MORE_DATA_;
    if (wide) void_utf8_to_gw(value_name, name, cap); else void_utf8_to_ga(value_name, name, cap);
    MEM32(name_size) = len;
    if (!type_out && !data && !data_size) return ERROR_SUCCESS_;
    return query_value(key, value_name, type_out, data, data_size, wide);
}
WINAPI_EXPORT(ADV, RegEnumValueA, 8) { return enum_value(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5), ARG(6), ARG(7), 0); }
WINAPI_EXPORT(ADV, RegEnumValueW, 8) { return enum_value(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5), ARG(6), ARG(7), 1); }

static uint32_t query_info(uint32_t key, uint32_t subkeys, uint32_t max_subkey, uint32_t values, uint32_t max_name,
                           uint32_t max_data)
{
    char path[512], sub[256];
    if (!handle_path(key, path, sizeof path)) return ERROR_INVALID_HANDLE_;
    pthread_mutex_lock(&reg_lock);
    uint32_t count = 0, longest = 0;
    while (subkey_at(path, count, sub, sizeof sub)) {
        if (strlen(sub) > longest) longest = (uint32_t)strlen(sub);
        ++count;
    }
    reg_key *k = find_key(path);
    uint32_t nvalues = k ? (uint32_t)k->count : 0, name_len = 0, data_len = 0;
    for (uint32_t i = 0; i < nvalues; ++i) {
        if (strlen(k->values[i].name) > name_len) name_len = (uint32_t)strlen(k->values[i].name);
        if (2 * k->values[i].size > data_len) data_len = 2 * k->values[i].size;
    }
    pthread_mutex_unlock(&reg_lock);
    if (subkeys) MEM32(subkeys) = count;
    if (max_subkey) MEM32(max_subkey) = longest;
    if (values) MEM32(values) = nvalues;
    if (max_name) MEM32(max_name) = name_len;
    if (max_data) MEM32(max_data) = data_len;
    return ERROR_SUCCESS_;
}
WINAPI_EXPORT(ADV, RegQueryInfoKeyA, 12) { return query_info(ARG(0), ARG(4), ARG(5), ARG(7), ARG(8), ARG(9)); }
WINAPI_EXPORT(ADV, RegQueryInfoKeyW, 12) { return query_info(ARG(0), ARG(4), ARG(5), ARG(7), ARG(8), ARG(9)); }
WINAPI_EXPORT(ADV, RegFlushKey, 1) { return ERROR_SUCCESS_; }
WINAPI_EXPORT(ADV, RegCloseKey, 1)
{
    if (root_name(ARG(0))) return ERROR_SUCCESS_;
    return void_handle_close(ARG(0)) ? ERROR_SUCCESS_ : ERROR_INVALID_HANDLE_;
}
