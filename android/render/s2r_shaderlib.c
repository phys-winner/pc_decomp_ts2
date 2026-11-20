/* The shader library file (shaders/s2r_shaderlib.bin of the APK's runtime.zip).
 *
 * tools/build_shaderlib.py writes it on the PC (the fixed-function and present
 * shaders, and the game shaders captured from the pinned game data) and
 * tools/build_game_apk.py packs it; GameAssets installs it with the port's
 * other resources and main.c names the folder; the renderer reads it once,
 * before the device is created. It is not compiled into libsims2.so. The file is a header (magic, format, the SHA-256
 * of the exe it came from) and five sections of packed little-endian records;
 * the record layouts are the struct formats in build_shaderlib.py. Every
 * offset and range is checked here, so a damaged or foreign file is refused
 * with a log line, not read past.
 *
 * What the library lacks (a material the capture never saw, a ps_1_x
 * sampler-type map it was not built for, a mod) is translated on the device by
 * the same translator (s2r_translate.c): OpenGL ES compiles the text, Vulkan
 * compiles it to SPIR-V once with glslang (s2r_vulkan.c, cached on disk). */
#include "s2r_internal.h"
#include "s2r_shaderlib.h"
#include "s2r_translate.h"
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LIB_FILE "s2r_shaderlib.bin"
#define LIB_FORMAT 1u

enum { S_SHADERS, S_MAPS, S_TEXT, S_SPV, S_BUILTINS, S_COUNT };
static const uint32_t record_size[S_COUNT] = { 120, 6, 1, 4, 16 };

s2r_lib s2r_shader_library;

static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static int loaded;
static char folder[512];

void s2r_lib_set_folder(const char *path) { snprintf(folder, sizeof folder, "%s", path ? path : ""); }

/* ------------------------------------------------------------ reading */
typedef struct reader { const uint8_t *p; } reader;
static uint8_t r8(reader *r) { return *r->p++; }
static uint16_t r16(reader *r) { uint16_t v = (uint16_t)(r->p[0] | r->p[1] << 8); r->p += 2; return v; }
static uint32_t r32(reader *r) { uint32_t v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24; r->p += 4; return v; }
static uint64_t r64(reader *r) { uint64_t lo = r32(r); return lo | (uint64_t)r32(r) << 32; }

typedef struct lib_file {
    const uint8_t *data;
    size_t size;
    uint32_t offset[S_COUNT], count[S_COUNT];
    uint32_t text_size;
    const char *text;
    int bad;
} lib_file;

/* A string of the text pool; NULL for the "none" offset, "" (and bad) when out of range. */
static const char *text_at(lib_file *f, uint32_t at, int allow_none)
{
    if (at == 0xFFFFFFFFu && allow_none) return NULL;
    if (at >= f->text_size) { f->bad = 1; return ""; }
    return f->text + at;
}

static int parse(lib_file *f, s2r_lib *lib)
{
    if (f->size < 48 + 12 * S_COUNT || memcmp(f->data, "S2SHLIB", 8)) { S2R_LOG("shader library: not a library file"); return 0; }
    reader h = { f->data + 8 };
    uint32_t format = r32(&h), sections = r32(&h);
    if (format != LIB_FORMAT || sections != S_COUNT) {
        S2R_LOG("shader library: format %u with %u sections, this build reads format %u", format, sections, LIB_FORMAT);
        return 0;
    }
    char exe[17];
    for (int i = 0; i < 8; ++i) snprintf(exe + 2 * i, 3, "%02x", f->data[16 + i]);
    h.p = f->data + 48;
    for (int s = 0; s < S_COUNT; ++s) {
        uint32_t offset = r32(&h), count = r32(&h), bytes = r32(&h);
        if ((uint64_t)offset + bytes > f->size || (uint64_t)count * record_size[s] != bytes || (offset & 3)) {
            S2R_LOG("shader library: section %d is damaged", s);
            return 0;
        }
        f->offset[s] = offset;
        f->count[s] = count;
    }
    f->text = (const char *)f->data + f->offset[S_TEXT];
    f->text_size = f->count[S_TEXT];
    if (!f->text_size || f->text[f->text_size - 1]) { S2R_LOG("shader library: the text pool is damaged"); return 0; }
    uint32_t words = f->count[S_SPV], maps = f->count[S_MAPS], shaders = f->count[S_SHADERS];

    s2r_lib_shader *sh = (s2r_lib_shader *)calloc(shaders + 1, sizeof *sh);
    reader r = { f->data + f->offset[S_SHADERS] };
    for (uint32_t i = 0; i < shaders; ++i) {
        s2r_lib_shader *s = &sh[i];
        s->hash = r64(&r); s->size = r32(&r); s->variant_types = r32(&r);
        s->pixel = r8(&r); s->major = r8(&r); s->minor = r8(&r); s->writes_fog = r8(&r); s->shadow_pcf = r8(&r);
        s->glsl = r32(&r); s->spv = r32(&r); s->spv_words = r32(&r);
        s->attr_count = r8(&r);
        memcpy(s->attr_usage, r.p, 16); r.p += 16;
        memcpy(s->attr_index, r.p, 16); r.p += 16;
        s->input_mask = r16(&r); s->samplers = r16(&r); s->sampler_types = r32(&r);
        s->ps_colors = r8(&r); s->ps_tex = r8(&r); s->ps_fog = r8(&r);
        s->f_total = r16(&r); s->i_total = r16(&r); s->b_total = r16(&r);
        s->map_first = r16(&r); s->fmap_count = r16(&r); s->imap_count = r16(&r); s->bmap_count = r16(&r);
        s->texbem_count = r8(&r);
        memcpy(s->texbem_stage, r.p, 8); r.p += 8;
        for (int k = 0; k < 8; ++k) s->texbem_dst[k] = r16(&r);
        s->name = text_at(f, r32(&r), 0);
        if (s->glsl >= f->text_size || (uint64_t)s->spv + s->spv_words > words || s->attr_count > 16 || s->texbem_count > 8 ||
            (uint32_t)s->map_first + s->fmap_count + s->imap_count + s->bmap_count > maps) f->bad = 1;
    }
    s2r_lib_map *mp = (s2r_lib_map *)calloc(maps + 1, sizeof *mp);
    r.p = f->data + f->offset[S_MAPS];
    for (uint32_t i = 0; i < maps; ++i) { mp[i].dst = r16(&r); mp[i].reg = r16(&r); mp[i].count = r16(&r); }
    s2r_lib_builtin *bi = (s2r_lib_builtin *)calloc(S2R_BUILTIN_COUNT, sizeof *bi);
    if (f->count[S_BUILTINS] != S2R_BUILTIN_COUNT) f->bad = 1;
    r.p = f->data + f->offset[S_BUILTINS];
    for (uint32_t i = 0; i < f->count[S_BUILTINS] && i < S2R_BUILTIN_COUNT; ++i) {
        bi[i].name = text_at(f, r32(&r), 0);
        bi[i].gles = r32(&r); bi[i].spv = r32(&r); bi[i].spv_words = r32(&r);
        if ((bi[i].gles != 0xFFFFFFFFu && bi[i].gles >= f->text_size) || (uint64_t)bi[i].spv + bi[i].spv_words > words) f->bad = 1;
    }
    if (f->bad) { S2R_LOG("shader library: a record points outside the file"); free(sh); free(mp); free(bi); return 0; }
    lib->shaders = sh; lib->shader_count = shaders;
    lib->maps = mp;
    lib->text = f->text;
    lib->spv = (const uint32_t *)(f->data + f->offset[S_SPV]);
    lib->builtins = bi;
    for (uint32_t i = 0; i < shaders; ++i) {
        sh[i].text = f->text + sh[i].glsl;
        sh[i].maps = mp + sh[i].map_first;
        sh[i].spirv = lib->spv + sh[i].spv;
    }
    S2R_LOG("shader library %s/%s: %u bytes, %u game shaders, %u bytes of SPIR-V, inputs %s... (mapped)",
        folder, LIB_FILE, (unsigned)f->size, shaders, 4u * words, exe);
    return 1;
}

static void load(void)
{
    if (!folder[0]) { S2R_LOG("shader library: no folder (the APK did not install it)"); return; }
    char path[600];
    snprintf(path, sizeof path, "%s/%s", folder, LIB_FILE);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { S2R_LOG("shader library %s is missing: every shader is translated on the device", path); return; }
    struct stat st;
    void *data = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (data == MAP_FAILED) { S2R_LOG("shader library %s cannot be read", path); return; }
    lib_file f;
    memset(&f, 0, sizeof f);
    f.data = (const uint8_t *)data;
    f.size = (size_t)st.st_size;
    /* The text pool and the SPIR-V stay mapped for the process's lifetime: clean file pages, read
       when a shader is first used (the file is not copied into the heap). */
    if (parse(&f, &s2r_shader_library)) loaded = 1;
    else munmap(data, (size_t)st.st_size);
}

int s2r_lib_load(void)
{
    pthread_once(&load_once, load);
    return loaded;
}

uint64_t s2r_fnv64(const void *data, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ------------------------------------------------- run-time translation */
typedef struct rt_shader {
    s2r_lib_shader lib;                /* first: the entry callers see */
    struct rt_shader *next;
} rt_shader;

static rt_shader *rt_shaders;
static pthread_mutex_t rt_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t stat_hits, stat_runtime;
static uint64_t stat_runtime_ns;

void s2r_lib_stats(uint32_t *library_hits, uint32_t *runtime_entries, uint64_t *runtime_ns)
{
    *library_hits = stat_hits; *runtime_entries = stat_runtime; *runtime_ns = stat_runtime_ns;
}

static int dump_on(void)
{
    static int value = -1;
    if (value < 0) {
        char v[92] = "";
        __system_property_get("debug.s2.shaderdump", v);
        value = v[0] == '1';
        if (value) S2R_LOG("debug.s2.shaderdump=1: shaders the library lacks go to %s/shaderdump", void_data_dir());
    }
    return value;
}

void s2r_lib_dump_missing(int pixel, const uint8_t *code, uint32_t size, uint64_t hash)
{
    if (!dump_on()) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/shaderdump", void_data_dir());
    mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/shaderdump/%016llx.%s", void_data_dir(), (unsigned long long)hash, pixel ? "ps" : "vs");
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(code, 1, size, f);
    fclose(f);
}

/* debug.s2.shaderlib=runtime: translate every game shader on the device, as
 * for a shader the library lacks (checks that path). */
static int runtime_only(void)
{
    static int value = -1;
    if (value < 0) {
        char v[92] = "";
        __system_property_get("debug.s2.shaderlib", v);
        value = !strcmp(v, "runtime");
        if (value) S2R_LOG("debug.s2.shaderlib=runtime: game shaders are translated on the device");
    }
    return value;
}

/* The library entry of a hash: the exact sampler-type map of a ps_1_x shader. */
static const s2r_lib_shader *library_entry(int pixel, uint64_t h, uint32_t size, uint32_t types, int sm1_ps)
{
    const s2r_lib_shader *list = s2r_shader_library.shaders;
    uint32_t lo = 0, hi = s2r_shader_library.shader_count;
    while (lo < hi) {                                   /* entries are sorted by hash */
        uint32_t mid = (lo + hi) / 2;
        if (list[mid].hash < h) lo = mid + 1; else hi = mid;
    }
    for (uint32_t i = lo; i < s2r_shader_library.shader_count && list[i].hash == h; ++i) {
        const s2r_lib_shader *s = &list[i];
        if (s->size != size || s->pixel != (pixel != 0)) continue;
        if (!sm1_ps || s->variant_types == types) return s;
    }
    return NULL;
}

static rt_shader *runtime_entry(int pixel, const uint8_t *code, uint32_t size, uint64_t h, uint32_t types)
{
    pthread_mutex_lock(&rt_lock);
    for (rt_shader *r = rt_shaders; r; r = r->next)
        if (r->lib.hash == h && r->lib.size == size && r->lib.pixel == (pixel != 0) && r->lib.variant_types == types) {
            pthread_mutex_unlock(&rt_lock);
            return r;
        }
    pthread_mutex_unlock(&rt_lock);
    uint64_t start = s2r_now_ns();
    s2r_translation t;
    char error[512];
    if (!s2r_translate(pixel, code, size, types, &t, error, sizeof error)) {
        S2R_LOG("%s shader %016llx (%u bytes, sampler types %X) cannot be translated: %s", pixel ? "pixel" : "vertex",
            (unsigned long long)h, size, types, error);
        return NULL;
    }
    rt_shader *r = (rt_shader *)calloc(1, sizeof *r);
    s2r_lib_shader *s = &r->lib;
    s->hash = h; s->size = size; s->pixel = (uint8_t)(pixel != 0); s->variant_types = types;
    s->major = (uint8_t)t.major; s->minor = (uint8_t)t.minor; s->writes_fog = (uint8_t)t.writes_fog;
    s->shadow_pcf = (uint8_t)t.shadow_pcf;
    s->attr_count = (uint8_t)t.attribute_count;
    for (int a = 0; a < t.attribute_count; ++a) { s->attr_usage[a] = (uint8_t)t.attributes[a][0]; s->attr_index[a] = (uint8_t)t.attributes[a][1]; }
    s->input_mask = (uint16_t)t.input_mask;
    for (int k = 0; k < t.sampler_count; ++k) {
        int reg = t.samplers[k][0];
        if (reg < 0 || reg >= 16) continue;
        s->samplers |= (uint16_t)(1u << reg);
        s->sampler_types |= (uint32_t)t.samplers[k][1] << (2 * reg);
    }
    s->ps_colors = (uint8_t)t.ps_colors; s->ps_tex = (uint8_t)t.ps_tex; s->ps_fog = (uint8_t)t.ps_fog;
    s->f_total = (uint16_t)t.f_total; s->i_total = (uint16_t)t.i_total; s->b_total = (uint16_t)t.b_total;
    s->fmap_count = (uint16_t)t.map_count[0]; s->imap_count = (uint16_t)t.map_count[1]; s->bmap_count = (uint16_t)t.map_count[2];
    s2r_lib_map *maps = (s2r_lib_map *)calloc((size_t)(t.map_count[0] + t.map_count[1] + t.map_count[2]) + 1, sizeof *maps);
    int n = 0;
    for (int set = 0; set < 3; ++set)
        for (int k = 0; k < t.map_count[set]; ++k, ++n) {
            maps[n].dst = (uint16_t)t.map[set][k][0]; maps[n].reg = (uint16_t)t.map[set][k][1]; maps[n].count = (uint16_t)t.map[set][k][2];
        }
    s->maps = maps;
    s->texbem_count = (uint8_t)t.texbem_count;
    for (int k = 0; k < t.texbem_count; ++k) { s->texbem_stage[k] = (uint8_t)t.texbem[k][0]; s->texbem_dst[k] = (uint16_t)t.texbem[k][1]; }
    s->name = "translated on the device";
    if (dump_on()) {
        /* development (debug.s2.shaderdump=1): the GLSL and the constant packing beside the bytecode */
        char path[1200];
        snprintf(path, sizeof path, "%s/shaderdump", void_data_dir());
        mkdir(path, 0775);
        snprintf(path, sizeof path, "%s/shaderdump/%016llx.%s.native.glsl", void_data_dir(), (unsigned long long)h,
            pixel ? "ps" : "vs");
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "// f_total %d i_total %d b_total %d synthetic_ctab %d\n", t.f_total, t.i_total, t.b_total, t.synthetic_ctab);
            for (int set = 0; set < 3; ++set)
                for (int k = 0; k < t.map_count[set]; ++k)
                    fprintf(f, "// map %c dst %d reg %d count %d\n", "fib"[set], t.map[set][k][0], t.map[set][k][1], t.map[set][k][2]);
            fputs(t.glsl, f);
            fclose(f);
        }
    }
    s->text = t.glsl;                  /* kept */
    s->runtime = 1;
    t.glsl = NULL;
    s2r_translation_free(&t);
    pthread_mutex_lock(&rt_lock);
    r->next = rt_shaders;
    rt_shaders = r;
    pthread_mutex_unlock(&rt_lock);
    uint64_t took = s2r_now_ns() - start;
    ++stat_runtime;
    stat_runtime_ns += took;
    if (stat_runtime <= 16 || took > 20000000u)
        S2R_LOG("%s shader %016llx (%u.%u, sampler types %X) translated on the device in %.1f ms (%u so far)",
            pixel ? "pixel" : "vertex", (unsigned long long)h, s->major, s->minor, types, (double)took / 1e6, stat_runtime);
    s2r_lib_dump_missing(pixel, code, size, h);
    return r;
}

const s2r_lib_shader *s2r_lib_shader_for(int pixel, const uint8_t *code, uint32_t size, uint32_t types, int allow_runtime)
{
    int loaded_lib = s2r_lib_load();
    uint32_t version = size >= 4 ? (uint32_t)code[0] | (uint32_t)code[1] << 8 | (uint32_t)code[2] << 16 | (uint32_t)code[3] << 24 : 0;
    int sm1_ps = pixel && ((version >> 8) & 255u) == 1;
    if (!sm1_ps) types = 0;
    uint64_t h = s2r_fnv64(code, size);
    const s2r_lib_shader *s = loaded_lib && !(allow_runtime && runtime_only()) ? library_entry(pixel, h, size, types, sm1_ps) : NULL;
    if (s) { ++stat_hits; return s; }
    if (allow_runtime) {
        rt_shader *r = runtime_entry(pixel, code, size, h, types);
        return r ? &r->lib : NULL;
    }
    /* Logged once per hash and map. */
    static struct { uint64_t h; uint32_t types; } missing[64];
    static int missing_count;
    for (int i = 0; i < missing_count; ++i) if (missing[i].h == h && missing[i].types == types) return NULL;
    if (missing_count < 64) { missing[missing_count].h = h; missing[missing_count].types = types; ++missing_count; }
    S2R_LOG("%s shader %016llx (%u bytes%s%X) is not in the library", pixel ? "pixel" : "vertex", (unsigned long long)h, size,
        sm1_ps ? ", ps_1_x sampler types " : ", types ", types);
    return NULL;
}

void s2r_lib_forget_modules(void)
{
    for (uint32_t i = 0; i < s2r_shader_library.shader_count; ++i) {
        ((s2r_lib_shader *)&s2r_shader_library.shaders[i])->backend_module = 0;
        ((s2r_lib_shader *)&s2r_shader_library.shaders[i])->backend_gl = 0;
    }
    pthread_mutex_lock(&rt_lock);
    for (rt_shader *r = rt_shaders; r; r = r->next) { r->lib.backend_module = 0; r->lib.backend_gl = 0; }
    pthread_mutex_unlock(&rt_lock);
}

/* --------------------------------------------------------------- builtins */
/* The fixed-function and present shaders: the library's (GLSL ES text and
 * SPIR-V the build compiled with glslc), else the copy tools/build_android.py
 * embeds (android/render/shaders), whose Vulkan form glslang compiles once. */
#include "s2r_builtins.inc"
extern int s2r_glslang_compile(int pixel, const char *source, uint32_t **words, uint32_t *count, char *log, size_t log_cap);

const char *s2r_builtin_gles(int which)
{
    if (which < 0 || which >= S2R_BUILTIN_COUNT) return NULL;
    if (s2r_lib_load()) {
        const s2r_lib_builtin *b = &s2r_shader_library.builtins[which];
        if (b->gles != 0xFFFFFFFFu) return s2r_shader_library.text + b->gles;
    }
    return s2r_builtin_gles_text[which];
}

const uint32_t *s2r_builtin_spirv(int which, uint32_t *words)
{
    static uint32_t *compiled[S2R_BUILTIN_COUNT], counts[S2R_BUILTIN_COUNT];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    *words = 0;
    if (which < 0 || which >= S2R_BUILTIN_COUNT) return NULL;
    /* UI FSR 1: the fixed-function pixel shader with its 2D reads through EASU (no library SPIR-V) */
    int ui = which == S2R_BUILTIN_FF_PS && s2r_ui_fsr();
    if (!ui && s2r_lib_load()) {
        const s2r_lib_builtin *b = &s2r_shader_library.builtins[which];
        if (b->spv_words) { *words = b->spv_words; return s2r_shader_library.spv + b->spv; }
    }
    pthread_mutex_lock(&lock);
    if (!compiled[which]) {
        char log[4096];
        uint64_t start = s2r_now_ns();
        char *filtered = ui ? s2r_ui_filter_source(s2r_builtin_vulkan_text[which]) : NULL;
        int ok = s2r_glslang_compile(s2r_builtin_pixel[which], filtered ? filtered : s2r_builtin_vulkan_text[which],
            &compiled[which], &counts[which], log, sizeof log);
        free(filtered);
        if (!ok) {
            S2R_LOG("builtin shader %d: GLSL -> SPIR-V failed: %s", which, log);
            compiled[which] = NULL;
        } else S2R_LOG("builtin shader %d compiled on the device in %.1f ms", which, (double)(s2r_now_ns() - start) / 1e6);
    }
    pthread_mutex_unlock(&lock);
    *words = counts[which];
    return compiled[which];
}
