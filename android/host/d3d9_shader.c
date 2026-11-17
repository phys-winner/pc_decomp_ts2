/* D3D9 shaders through MojoShader (zlib, revision
 * cf2661bcb55d9a8732e86f79922592d7efdf5383, unmodified; built with
 * MOJOSHADER_FLIP_RENDERTARGET and MOJOSHADER_DEPTH_CLIPPING).
 *
 * The generated GLSL ES 3.00 is post-processed here:
 *   - vertex: D3D9's half-pixel rasterization offset (vpHalfPixel), applied
 *     after MojoShader's render-target flip;
 *   - pixel: the fixed-function alpha test (GLES has none) as a uniform test
 *     before every return from main.
 * D3DCOLOR inputs are swizzled by MojoShader itself (MOJOSHADER_swizzle), so a
 * vertex shader is compiled per BGRA-input mask. */
#include "d3d9_internal.h"
#include <ctype.h>
#include "d3d9_filter.h"
#include "sims2_settings.h"
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include "d3d9_thread.inc"

uint32_t d3d_shader_bytecode_size(uint32_t va)
{
    int legacy = ((MEM32(va) >> 8) & 0xFF) == 1;
    uint32_t p = va + 4;                       /* skip the version token */
    for (uint32_t guard = 0; guard < 1u << 20; ++guard) {
        uint32_t token = MEM32(p);
        if (token == 0x0000FFFFu) return p + 4 - va;
        if ((token & 0xFFFF) == 0xFFFE) { p += 4 + 4 * ((token >> 16) & 0x7FFF); continue; }
        /* SM1 has no instruction lengths; parameter modifiers occupy those bits. */
        uint32_t len = legacy ? (token == 0x0051 ? 5 : 0) : (token >> 24) & 0x0F;
        p += 4 + 4 * len;
    }
    return 0;
}

static void_com_class shader_class_vs, shader_class_ps;
static void process_destroy(d3d_shader *s);
static void programs_forget_shader(d3d_shader *s);

static void run_shader_destroy(void *args)
{
    d3d_shader *s = *(d3d_shader **)args;
    programs_forget_shader(s);
    process_destroy(s);
    if (s->gl_plain) glDeleteShader(s->gl_plain);
    if (s->parse) MOJOSHADER_freeParseData(s->parse);
    free(s->bytecode);
    free(s->parse_code);
    free(s->source);
    free(s);
}

/* After the draws recorded before its release (they may still bind it). */
static void shader_destroy(void_com_object *o) { d3d_shader *s = (d3d_shader *)o; d3d_rt_call(run_shader_destroy, &s, sizeof s); }

/* Insert `text` before the first occurrence of `anchor` (or append). */
static char *insert_before(const char *src, const char *anchor, const char *text)
{
    const char *at = strstr(src, anchor);
    size_t head = at ? (size_t)(at - src) : strlen(src), len = strlen(src), n = strlen(text);
    char *out = (char *)malloc(len + n + 1);
    if (!out) return NULL;
    memcpy(out, src, head);
    memcpy(out + head, text, n);
    strcpy(out + head + n, src + head);
    return out;
}

/* MojoShader's GLSL ES for ps_1_x reads texture coordinates through globals
 * initialized from io_ varyings it never declares (`vec4 ps_t0 = io_5_0;`),
 * and GLSL ES wants constant global initializers. Declare every io_ varying
 * the source uses and move such initializers to the start of main. */
static char *fix_pixel_varyings(const char *src)
{
    char decls[2048] = "", moved[2048] = "";
    size_t dn = 0, mn = 0;
    for (const char *p = strstr(src, "io_"); p; p = strstr(p + 3, "io_")) {
        if (p > src && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        unsigned u, i;
        if (sscanf(p, "io_%u_%u", &u, &i) != 2) continue;
        char decl[48];
        snprintf(decl, sizeof decl, "in highp vec4 io_%u_%u;", u, i);
        if (strstr(src, decl) || strstr(decls, decl)) continue;
        dn += (size_t)snprintf(decls + dn, sizeof decls - dn, "%s\n", decl);
    }
    size_t len = strlen(src);
    char *body = (char *)malloc(len + 1), *w = body;
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        char name[32];
        unsigned u, i;
        if (!strncmp(line, "vec4 ps_", 8) && sscanf(line, "vec4 %31[a-z_0-9] = io_%u_%u;", name, &u, &i) == 3) {
            w += sprintf(w, "vec4 %s;\n", name);
            mn += (size_t)snprintf(moved + mn, sizeof moved - mn, "\t%s = io_%u_%u;\n", name, u, i);
        } else {
            memcpy(w, line, n);
            w += n;
        }
        line += n;
    }
    *w = 0;
    if (!dn && !mn) return body;
    char *with_decls = insert_before(body, "void main()", decls);
    free(body);
    if (!mn) return with_decls;
    const char *mainp = strstr(with_decls, "void main()");
    const char *brace = mainp ? strchr(mainp, '{') : NULL;
    if (!brace) return with_decls;
    size_t at = (size_t)(brace + 1 - with_decls);
    char *out = (char *)malloc(strlen(with_decls) + mn + 2);
    memcpy(out, with_decls, at);
    out[at] = '\n';
    memcpy(out + at + 1, moved, mn);
    strcpy(out + at + 1 + mn, with_decls + at);
    free(with_decls);
    return out;
}

static char *patch_shader(const char *src, int pixel);

/* vs_1_x writes the address register with mov, which MojoShader emits as a
 * plain float-to-int assignment (`vs_a0.x = vs_r0.x;`) that GLSL rejects.
 * D3D floors the value there (Wine does the same); mova keeps its own
 * rounding and is already an int expression. */
static char *fix_address_moves(const char *src)
{
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 64), *w = out;
    if (!out) return NULL;
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        const char *p = line;
        while (*p == '\t' || *p == ' ') ++p;
        char mask[8];
        int prefix = 0;
        if (sscanf(p, "vs_a0.%7[xyzw] = %n", mask, &prefix) == 1 && prefix > 0 &&
            strncmp(p + prefix, "int(", 4) && strncmp(p + prefix, "ivec", 4)) {
            const char *expr = p + prefix, *semi = strchr(expr, ';');
            if (semi && semi < line + n) {
                size_t comps = strlen(mask);
                w += sprintf(w, "%.*s", (int)(p - line), line);
                if (comps == 1) w += sprintf(w, "vs_a0.%s = int(floor(%.*s));\n", mask, (int)(semi - expr), expr);
                else w += sprintf(w, "vs_a0.%s = ivec%zu(floor(%.*s));\n", mask, comps, (int)(semi - expr), expr);
                line += n;
                continue;
            }
        }
        memcpy(w, line, n);
        w += n;
        line += n;
    }
    *w = 0;
    return out;
}

/* A relatively addressed constant past the register file reads (0,0,0,0) on
 * D3D9 hardware, and RendererD3D's skinning relies on it: an unused bone slot
 * holds index 255 with weight 0, which reads c[56 + 765]. GLSL leaves an
 * out-of-bounds uniform array read undefined (Adreno returns other data,
 * NaN among it, and 0 * NaN tears vertices off), so every read indexed by
 * the address or loop register goes through vs_rel, which bounds-checks it. */
static char *guard_relative(const char *src)
{
    const char *decl = strstr(src, "uniform vec4 vs_uniforms_vec4[");
    const char *mainp = strstr(src, "void main()");
    unsigned size = 0;
    if (!decl || !mainp || sscanf(decl, "uniform vec4 vs_uniforms_vec4[%u];", &size) != 1 || !size ||
        (!strstr(mainp, "vs_a0") && !strstr(mainp, "vs_aL")))
        return strdup(src);
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 256), *w = out;
    size_t head = (size_t)(mainp - src);
    memcpy(w, src, head);
    w += head;
    w += sprintf(w, "vec4 vs_rel(int i) { return (i >= 0 && i < %u) ? vs_uniforms_vec4[i] : vec4(0.0); }\n", size);
    const char *needle = "vs_uniforms_vec4[";
    const char *p = mainp;
    for (const char *at = strstr(p, needle); at; at = strstr(p, needle)) {
        const char *open = at + strlen(needle), *q = open;
        int depth = 1;
        while (*q && depth) {
            if (*q == '[') ++depth;
            else if (*q == ']') --depth;
            if (depth) ++q;
        }
        if (!*q) break;
        memcpy(w, p, (size_t)(at - p));
        w += at - p;
        size_t n = (size_t)(q - open);
        int relative = 0;
        for (size_t i = 0; i + 5 <= n; ++i)
            if (!strncmp(open + i, "vs_a0", 5) || !strncmp(open + i, "vs_aL", 5)) relative = 1;
        if (relative) w += sprintf(w, "vs_rel(%.*s)", (int)n, open);
        else { memcpy(w, at, (size_t)(q + 1 - at)); w += q + 1 - at; }
        p = q + 1;
    }
    strcpy(w, p);
    return out;
}

static char *patch_source(const char *original, int pixel)
{
    if (!pixel) {
        char *fixed = fix_address_moves(original);
        char *guarded = guard_relative(fixed);
        char *out = patch_shader(guarded, 0);
        free(fixed);
        free(guarded);
        return out;
    }
    char *fixed = fix_pixel_varyings(original);
    char *out = patch_shader(fixed, 1);
    free(fixed);
    return out;
}

static char *patch_shader(const char *src, int pixel)
{
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 2048);
    char *w = out;
    if (!pixel) {
        const char *decl = strstr(src, "uniform float vpFlip;");
        const char *flip = strstr(src, "gl_Position.y = gl_Position.y * vpFlip;");
        if (!decl || !flip) { strcpy(out, src); return out; }
        size_t a = (size_t)(decl - src) + strlen("uniform float vpFlip;");
        memcpy(w, src, a); w += a;
        w += sprintf(w, "\nuniform vec2 vpHalfPixel;");
        size_t b = (size_t)(flip - src) + strlen("gl_Position.y = gl_Position.y * vpFlip;");
        memcpy(w, src + a, b - a); w += b - a;
        w += sprintf(w, "\n\tgl_Position.xy += vpHalfPixel * gl_Position.w;");
        strcpy(w, src + b);
        return out;
    }
    /* Pixel: find colour output 0 and main's body. */
    const char *mainp = strstr(src, "void main()");
    const char *out0 = strstr(src, "ps_oC0");
    if (!mainp || !out0) { strcpy(out, src); return out; }
    size_t head = (size_t)(mainp - src);
    memcpy(w, src, head); w += head;
    w += sprintf(w,
        "uniform int vpAlphaFunc;\nuniform float vpAlphaRef;\n"
        "void void_alpha_test() {\n"
        "\tfloat a = floor(clamp(ps_oC0.a, 0.0, 1.0) * 255.0 + 0.5);\n"
        "\tbool pass = true;\n"
        "\tif (vpAlphaFunc == 1) pass = false;\n"
        "\telse if (vpAlphaFunc == 2) pass = a < vpAlphaRef;\n"
        "\telse if (vpAlphaFunc == 3) pass = a == vpAlphaRef;\n"
        "\telse if (vpAlphaFunc == 4) pass = a <= vpAlphaRef;\n"
        "\telse if (vpAlphaFunc == 5) pass = a > vpAlphaRef;\n"
        "\telse if (vpAlphaFunc == 6) pass = a != vpAlphaRef;\n"
        "\telse if (vpAlphaFunc == 7) pass = a >= vpAlphaRef;\n"
        "\tif (!pass) discard;\n}\n");
    /* Copy main, rewriting each `return;` at any depth inside it. */
    const char *p = mainp;
    const char *brace = strchr(p, '{');
    if (!brace) { strcpy(w, p); return out; }
    memcpy(w, p, (size_t)(brace - p + 1)); w += brace - p + 1;
    int depth = 1;
    p = brace + 1;
    while (*p && depth > 0) {
        if (*p == '{') ++depth;
        else if (*p == '}') { if (--depth == 0) break; }
        if (!strncmp(p, "return;", 7) && (p == src || !isalnum((unsigned char)p[-1]))) {
            w += sprintf(w, "{ void_alpha_test(); return; }");
            p += 7;
            continue;
        }
        *w++ = *p++;
    }
    /* A main without a trailing return still needs the test. */
    w += sprintf(w, "\tvoid_alpha_test();\n");
    strcpy(w, p);
    return out;
}

/* MojoShader sizes a relatively addressed constant array from the CTAB entry
 * the base register belongs to, and cannot address constants relatively
 * without a CTAB at all. D3D9 itself just indexes the register file, and
 * RendererD3D's skinning shaders name their palette rows c56, c57 and c58 as
 * one register each while reading c[56 + 3 * bone]. Such bytecode gets a
 * synthetic CTAB instead of its own (D3DX puts that among the comments right
 * after the version token): one float4 array from register `lo` to the end of
 * the stage's register file, within what GL gives a stage (`regs`). The
 * instructions are untouched. NULL when the CTAB sits anywhere else. */
static uint8_t *with_synthetic_ctab(const uint8_t *code, uint32_t size, int pixel, uint32_t lo, uint32_t regs,
    uint32_t *out_size)
{
    uint32_t keep = 4;                                       /* the version token */
    uint32_t skip = 4;
    while (skip + 8 <= size) {
        uint32_t token, id;
        memcpy(&token, code + skip, 4);
        if ((token & 0xFFFF) != 0xFFFE) break;
        uint32_t bytes = 4 + 4 * ((token >> 16) & 0x7FFF);
        memcpy(&id, code + skip + 4, 4);
        if (id != FOURCC('C','T','A','B') || skip + bytes > size) break;
        skip += bytes;
    }
    for (uint32_t p = skip; p + 8 <= size;) {                /* a later CTAB would be parsed too */
        uint32_t token, id;
        memcpy(&token, code + p, 4);
        if ((token & 0xFFFF) != 0xFFFE) break;
        memcpy(&id, code + p + 4, 4);
        if (id == FOURCC('C','T','A','B')) return NULL;
        p += 4 + 4 * ((token >> 16) & 0x7FFF);
    }
    uint32_t count = regs > lo ? regs - lo : 1;
    uint8_t blob[128];
    memset(blob, 0, sizeof blob);
    uint32_t *w = (uint32_t *)blob;
    const char *target = pixel ? "ps_3_0" : "vs_3_0";
    /* D3DXSHADER_CONSTANTTABLE */
    w[0] = 28; w[1] = 80; memcpy(&w[2], code, 4); w[3] = 1; w[4] = 28; w[5] = 0; w[6] = 96;
    /* D3DXSHADER_CONSTANTINFO at 28: Name, RegisterSet|Index, Count|Reserved, TypeInfo, DefaultValue */
    w[7] = 72; w[8] = 2u | (lo << 16); w[9] = count; w[10] = 48; w[11] = 0;
    /* D3DXSHADER_TYPEINFO at 48: Class|Type, Rows|Columns, Elements|StructMembers, StructMemberInfo */
    w[12] = 1u | (3u << 16); w[13] = 1u | (4u << 16); w[14] = count; w[15] = 0;
    memcpy(blob + 72, "void_c", 7);
    memcpy(blob + 80, "void host", 10);
    memcpy(blob + 96, target, 7);
    uint32_t data_dwords = 104 / 4;
    uint32_t comment_dwords = 1 + data_dwords;              /* 'CTAB' + data */
    *out_size = size - (skip - keep) + 4 * (1 + comment_dwords);
    uint8_t *out = (uint8_t *)malloc(*out_size);
    if (!out) return NULL;
    memcpy(out, code, keep);
    uint32_t token = 0xFFFEu | (comment_dwords << 16), fourcc = FOURCC('C','T','A','B');
    memcpy(out + 4, &token, 4);
    memcpy(out + 8, &fourcc, 4);
    memcpy(out + 12, blob, 104);
    memcpy(out + 12 + 104, code + skip, size - skip);
    return out;
}

/* The lowest register of a constant array the shader addresses relatively,
 * -1 for none. */
static int relative_base(const MOJOSHADER_parseData *pd)
{
    int lo = -1;
    for (int i = 0; i < pd->uniform_count; ++i) {
        const MOJOSHADER_uniform *u = &pd->uniforms[i];
        if (u->type == MOJOSHADER_UNIFORM_FLOAT && u->array_count && !u->constant && (lo < 0 || u->index < lo))
            lo = u->index;
    }
    return lo;
}

/* The end of a stage's relatively addressed array from register `lo`: the
 * D3D9 register file, unless the array, the float registers the shader uses
 * below it, its int and bool constants and the port's own uniforms (vpFlip,
 * vpHalfPixel, the alpha test) would exceed GL's uniform vectors (Adreno: 256,
 * exactly the vertex register file). */
static uint32_t relative_regs(int pixel, const MOJOSHADER_parseData *pd, int lo)
{
    uint32_t regs = pixel ? 224 : 256;
    uint32_t gl = pixel ? d3d_gl_limits.max_ps_vectors : d3d_gl_limits.max_vs_vectors;
    if (!gl) return regs;
    uint32_t others = 4;
    for (int i = 0; pd && i < pd->uniform_count; ++i) {
        const MOJOSHADER_uniform *u = &pd->uniforms[i];
        if (u->constant) continue;
        if (u->type != MOJOSHADER_UNIFORM_FLOAT) others += u->array_count ? (uint32_t)u->array_count : 1;
        else if (!u->array_count && u->index < lo) others += 1;
    }
    if ((uint32_t)lo + gl < regs + others) regs = gl > others ? (uint32_t)lo + gl - others : (uint32_t)lo + 1;
    return regs;
}

d3d_shader *d3d_shader_create(d3d_device *dev, int pixel, uint32_t bytecode)
{
    uint32_t size = d3d_shader_bytecode_size(bytecode);
    if (!size) { void_log("[d3d9] shader bytecode without end token"); return NULL; }
    d3d_shader *s = (d3d_shader *)calloc(1, sizeof *s);
    s->dev = dev;
    s->pixel = pixel;
    s->size = size;
    s->bytecode = (uint8_t *)malloc(size);
    memcpy(s->bytecode, GPTR(bytecode), size);
    s->parse = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", s->bytecode, size, NULL, 0, NULL, 0, NULL, NULL, NULL);
    int lo = -1;
    if (s->parse && s->parse->error_count && strstr(s->parse->errors[0].error, "without a CTAB")) lo = 0;
    else if (s->parse && !s->parse->error_count) lo = relative_base(s->parse);
    uint32_t regs = lo >= 0 ? relative_regs(pixel, s->parse && !s->parse->error_count ? s->parse : NULL, lo) : 0;
    uint32_t patched_size = 0;
    uint8_t *patched = lo >= 0 ? with_synthetic_ctab(s->bytecode, size, pixel, (uint32_t)lo, regs, &patched_size) : NULL;
    if (patched) {
        const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", patched, patched_size,
            NULL, 0, NULL, 0, NULL, NULL, NULL);
        if (pd && !pd->error_count) {
            if (s->parse) MOJOSHADER_freeParseData(s->parse);
            s->parse = pd;
            s->parse_code = patched;
            s->parse_size = patched_size;
            VLOG("[d3d9] %s shader given a synthetic CTAB for relative addressing (c%d..c%u)", pixel ? "pixel" : "vertex",
                lo, regs - 1);
        } else {
            if (pd) MOJOSHADER_freeParseData(pd);
            free(patched);
        }
    }
    if (!s->parse || s->parse->error_count) {
        const uint32_t *w = (const uint32_t *)s->bytecode;
        uint32_t k = (g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1);   /* this COM call */
        /* RendererD3D's shader database loader (16255330) keeps its key at [ebp + 0xC]. */
        uint32_t key = MEM32(g_ebp + 0xC);
        if (void_vm_readable(key, 16))
            void_log("[d3d9] caller frame key %08X: %08X %08X %08X %08X", key, MEM32(key), MEM32(key + 4), MEM32(key + 8), MEM32(key + 12));
        else
            void_log("[d3d9] caller frame key value %08X", key);
        void_log("[d3d9] MojoShader failed on %s shader at %08X (caller %08X, %u bytes: %08X %08X %08X %08X): %s",
            pixel ? "pixel" : "vertex", bytecode, g_icall_from[k], size, w[0], size > 4 ? w[1] : 0, size > 8 ? w[2] : 0,
            size > 12 ? w[3] : 0, s->parse && s->parse->error_count ? s->parse->errors[0].error : "no parse data");
        static unsigned failed_dumped;
        if (failed_dumped < 4) {
            ++failed_dumped;
            char path[1100];
            snprintf(path, sizeof path, "%s/shaderdump", void_data_dir());
            mkdir(path, 0775);
            snprintf(path, sizeof path, "%s/shaderdump/failed_%s_%08X.bin", void_data_dir(), pixel ? "ps" : "vs", bytecode);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(s->bytecode, 1, size, f); fclose(f); void_log("[d3d9] failed shader dumped to %s", path); }
        }
        if (s->parse) MOJOSHADER_freeParseData(s->parse);
        free(s->bytecode);
        free(s);
        return NULL;
    }
    /* Diagnostics: inputs that collapse onto one varying name. */
    for (int i = 0; i < s->parse->attribute_count; ++i)
        for (int j = i + 1; j < s->parse->attribute_count; ++j)
            if (s->parse->attributes[i].usage == s->parse->attributes[j].usage &&
                s->parse->attributes[i].index == s->parse->attributes[j].index) {
                static int dumped;
                if (dumped++ < 4) {
                    char path[1100];
                    snprintf(path, sizeof path, "%s/shaderdump", void_data_dir());
                    mkdir(path, 0775);
                    snprintf(path, sizeof path, "%s/shaderdump/%s_%08X.bin", void_data_dir(), pixel ? "ps" : "vs", bytecode);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(s->bytecode, 1, size, f); fclose(f); }
                    uint32_t k = (g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1);
                    void_log("[d3d9] %s shader at %08X (caller %08X) has inputs sharing usage %d index %d: dumped to %s",
                        pixel ? "pixel" : "vertex", bytecode, g_icall_from[k], s->parse->attributes[i].usage,
                        s->parse->attributes[i].index, path);
                }
                i = j = s->parse->attribute_count;
            }
    s->source = patch_source(s->parse->output, pixel);
    return s;
}

static int shader_type_ok(const d3d_shader *s, int pixel) { return s && s->pixel == pixel; }

COM_METHOD(shd, QueryInterface, 3)
{
    d3d_shader *s = (d3d_shader *)void_com_host(THIS_);
    if (void_guid_equal(ARG(1), s->pixel ? IID_IDirect3DPixelShader9 : IID_IDirect3DVertexShader9) ||
        void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&s->com); MEM32(ARG(2)) = THIS_; return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(shd, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(shd, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(shd, GetDevice, 2)
{
    d3d_shader *s = (d3d_shader *)void_com_host(THIS_);
    void_com_addref(&s->dev->com);
    MEM32(ARG(1)) = s->dev->com.guest;
    return D3D_OK;
}
COM_METHOD(shd, GetFunction, 3)
{
    d3d_shader *s = (d3d_shader *)void_com_host(THIS_);
    if (ARG(1)) memcpy(GPTR(ARG(1)), s->bytecode, s->size);
    MEM32(ARG(2)) = s->size;
    return D3D_OK;
}
static const void_fn shader_methods[] = { shd_QueryInterface, shd_AddRef, shd_Release, shd_GetDevice, shd_GetFunction };
static void_com_class shader_class_vs = { "IDirect3DVertexShader9", 5, shader_methods, 0 };
static void_com_class shader_class_ps = { "IDirect3DPixelShader9", 5, shader_methods, 0 };

uint32_t d3d_wrap_shader(d3d_shader *s)
{
    uint32_t guest = void_com_wrap(&s->com, s->pixel ? &shader_class_ps : &shader_class_vs, 0);
    s->com.destroy = shader_destroy;
    return guest;
}

/* ------------------------------------------------------------- programs */
typedef struct { int type, index, count; } umap_t;

typedef struct program {
    d3d_shader *vs, *ps;          /* either may be NULL: a generated fixed-function half stands in */
    uint32_t bgra_mask;
    uint32_t ff_id;               /* the generated half (d3d9_ff.c), 0 for none */
    GLuint gl, vs_gl, ps_gl;
    int own_vs, own_ps;           /* variants; plain and fixed-function halves are borrowed */
    GLint loc_vs_f, loc_vs_i, loc_vs_b, loc_ps_f, loc_ps_i, loc_ps_b;
    GLint loc_flip, loc_half, loc_alpha_func, loc_alpha_ref;
    umap_t vs_map[256], ps_map[256];
    int vs_map_count, ps_map_count;
    int vs_f_count, vs_i_count, vs_b_count, ps_f_count, ps_i_count, ps_b_count;
    float *vs_f, *ps_f;           /* what the GL program holds (GL starts uniforms at zero, as calloc does) */
    int32_t *vs_i, *ps_i, *vs_b, *ps_b;
    GLint *vs_f_loc, *ps_f_loc;   /* element locations, queried on first use (-2: not yet) */
    int attr_count;
    int attr_usage[16], attr_index[16];
    uint32_t vs_sampler_mask, ps_sampler_mask;
    int ps_sampler_type[16], vs_sampler_type[4];
    float last_half[2];
    int last_alpha_func;
    float last_alpha_ref;
    uint32_t uploaded_generation;
    struct program *next;
} program_t;

static program_t *programs[1024];
static program_t *current_program;
uint32_t d3d_constant_generation = 1;

/* The [mem] report (iostat.c): live programs, and the GLSL compiles and links
 * so far with the time they took (render thread; read racily, as statistics). */
static uint64_t stat_programs, stat_links, stat_link_ns, stat_compiles, stat_compile_ns;

/* The fixed-function pipeline's programs and halves (d3d9_ff.c) count too. */
void d3d_shader_count(int programs, uint64_t link_ns, int compiles, uint64_t compile_ns)
{
    stat_programs += (uint64_t)(int64_t)programs;
    if (programs > 0) { stat_links += (uint64_t)programs; stat_link_ns += link_ns; }
    stat_compiles += (uint64_t)compiles;
    stat_compile_ns += compile_ns;
}

void d3d_shader_stats(uint64_t *programs_live, uint64_t *links, uint64_t *link_ns, uint64_t *compiles, uint64_t *compile_ns)
{
    *programs_live = stat_programs; *links = stat_links; *link_ns = stat_link_ns;
    *compiles = stat_compiles; *compile_ns = stat_compile_ns;
}

static void program_destroy(program_t *p)
{
    if (p->gl) --stat_programs;
    if (current_program == p) current_program = NULL;
    if (p->gl) { d3d_ff_forget_program(p->gl); d3d_gl_delete_program(p->gl); }
    if (p->own_vs && p->vs_gl) glDeleteShader(p->vs_gl);
    if (p->own_ps && p->ps_gl) glDeleteShader(p->ps_gl);
    free(p->vs_f); free(p->ps_f);
    free(p->vs_i); free(p->ps_i);
    free(p->vs_b); free(p->ps_b);
    free(p->vs_f_loc); free(p->ps_f_loc);
    free(p);
}

/* Runs after the queued draws that held the shader, before its address can
 * be reused for another shader. Remove identities from every bucket. */
static void programs_forget_shader(d3d_shader *s)
{
    for (uint32_t i = 0; i < 1024; ++i) {
        program_t **slot = &programs[i];
        while (*slot) {
            program_t *p = *slot;
            if (p->vs == s || p->ps == s) {
                *slot = p->next;
                program_destroy(p);
            } else slot = &p->next;
        }
    }
}

/* Malloc aligns shader pointers. Masking their low bits directly put many
 * shader pairs in the same bucket; mix the full addresses before masking. */
static uint32_t program_bucket(d3d_shader *vs, d3d_shader *ps, uint32_t mask, uint32_t ff_id)
{
    uint64_t vertex = (uintptr_t)vs, pixel = (uintptr_t)ps;
    uint64_t h = vertex ^ ((pixel << 21) | (pixel >> 43)) ^ ((uint64_t)mask << 32) ^ ff_id;
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    return (uint32_t)h & 1023u;
}

static program_t *find_program(d3d_shader *vs, d3d_shader *ps, uint32_t mask, uint32_t ff_id, uint32_t *bucket)
{
    program_t *p = current_program;
    if (p && p->vs == vs && p->ps == ps && p->bgra_mask == mask && p->ff_id == ff_id) return p;
    *bucket = program_bucket(vs, ps, mask, ff_id);
    for (p = programs[*bucket]; p; p = p->next)
        if (p->vs == vs && p->ps == ps && p->bgra_mask == mask && p->ff_id == ff_id) return p;
    return NULL;
}

static GLuint compile(GLenum type, const char *source)
{
    GLuint sh = glCreateShader(type);
    char *filtered = type == GL_FRAGMENT_SHADER && s2_ui_fsr() ? d3d_ui_filter_source(source) : NULL;
    const char *active = filtered ? filtered : source;
    glShaderSource(sh, 1, &active, NULL);
    free(filtered);
    uint64_t started = void_monotonic_ns();
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    stat_compile_ns += void_monotonic_ns() - started;
    ++stat_compiles;
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        void_log("[d3d9] GLSL compile failed: %s", log);
        void_log("[d3d9] source:\n%s", source);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Program binaries on disk (<app cache>/glprog): a program the driver linked
 * once loads with glProgramBinary in later sessions instead of compiling and
 * linking again (a first lot visit spent 2.4 s in links, as hitches). The key
 * is a 128-bit hash of the exact GLSL both halves compile from, the vertex
 * attribute bindings and the driver's renderer and version strings, so any
 * change to the translation or the driver misses instead of loading a stale
 * binary; one the driver refuses is deleted and the program built from
 * source. S2_PROGRAM_CACHE=0 (or a file <cache>/glprog/off) turns it off. */
__attribute__((weak)) const char *void_app_cache_dir(void) { return ""; }   /* d3d9_dxvk.c in the APK */

#define PROGRAM_CACHE_MAGIC 0x42503253u   /* "S2PB" */
#define PROGRAM_CACHE_VERSION 1u
#define PROGRAM_CACHE_LIMIT (256u << 20)  /* bytes written per session at most */
typedef struct { uint32_t magic, version; uint64_t a, b; uint32_t format, length; } program_cache_header;
static int program_cache_state;           /* 0 untried, 1 on, -1 off */
static char program_cache_dir[600];
static uint64_t stat_cache_hits, stat_cache_misses, stat_cache_bytes;

void d3d_program_cache_stats(uint64_t *hits, uint64_t *misses, uint64_t *bytes)
{
    *hits = stat_cache_hits; *misses = stat_cache_misses; *bytes = stat_cache_bytes;
}

/* The folder used to only grow: a driver update changes every key, and
 * nothing removed the old driver's files. glprog/driver now names the driver
 * the files belong to (a hash of GL_RENDERER and GL_VERSION); when it
 * differs, the first use of the cache in a session removes them all. Above
 * PROGRAM_FOLDER_LIMIT the oldest files go too, down to PROGRAM_FOLDER_KEEP,
 * and a .tmp a session left behind always goes. Only regular files named the
 * way program_cache_path names them (or their .tmp) are ever removed, and the
 * first session with this code keeps the files it finds. This is the UT3
 * port's binary_housekeeping (2026-11-04) with this cache's names;
 * tools/check_program_housekeeping.py runs it on a device. */
#define PROGRAM_FOLDER_LIMIT (512ull << 20)
#define PROGRAM_FOLDER_KEEP (384ull << 20)

/* program_cache_path's names: 32 lowercase hex digits, then ".bin" or ".bin.tmp". */
static int program_file_name(const char *name)
{
    for (int i = 0; i < 32; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return !strcmp(name + 32, ".bin") || !strcmp(name + 32, ".bin.tmp");
}

typedef struct { char name[48]; uint64_t size; int64_t mtime; } program_file;

static int program_file_older(const void *a, const void *b)
{
    int64_t x = ((const program_file *)a)->mtime, y = ((const program_file *)b)->mtime;
    return (x > y) - (x < y);
}

/* Once per session, before the first load or store (see the block comment). */
static void program_cache_housekeeping(const char *dir, uint64_t driver)
{
    char marker[700], partial[720], seen[40] = "", want[40];
    snprintf(marker, sizeof marker, "%s/driver", dir);
    snprintf(want, sizeof want, "%016llx\n", (unsigned long long)driver);
    FILE *f = fopen(marker, "rb");
    int known = f != NULL;
    if (f) { seen[fread(seen, 1, sizeof seen - 1, f)] = 0; fclose(f); }
    int wipe = known && strcmp(seen, want) != 0;
    DIR *d = opendir(dir);
    if (!d) return;
    program_file *files = NULL;
    size_t count = 0, capacity = 0, removed = 0;
    uint64_t kept = 0, freed = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        struct stat st;
        if (!program_file_name(e->d_name) || fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode))
            continue;
        if (wipe || strlen(e->d_name) != 36) {     /* the old driver's, or a store cut short */
            if (!unlinkat(dirfd(d), e->d_name, 0)) { ++removed; freed += (uint64_t)st.st_size; }
            continue;
        }
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 256;
            program_file *more = (program_file *)realloc(files, grown * sizeof *files);
            if (!more) break;
            files = more;
            capacity = grown;
        }
        snprintf(files[count].name, sizeof files[count].name, "%s", e->d_name);
        files[count].size = (uint64_t)st.st_size;
        files[count].mtime = (int64_t)st.st_mtime;
        kept += files[count++].size;
    }
    if (kept > PROGRAM_FOLDER_LIMIT) {
        qsort(files, count, sizeof *files, program_file_older);
        for (size_t i = 0; i < count && kept > PROGRAM_FOLDER_KEEP; ++i)
            if (!unlinkat(dirfd(d), files[i].name, 0)) { kept -= files[i].size; freed += files[i].size; ++removed; }
    }
    closedir(d);
    free(files);
    if (!known || wipe) {
        snprintf(partial, sizeof partial, "%s.tmp", marker);
        f = fopen(partial, "wb");
        int ok = f && fputs(want, f) >= 0;
        if (f && fclose(f)) ok = 0;
        if (!ok || rename(partial, marker)) unlink(partial);
    }
    if (removed)
        void_log("[d3d9] program cache: removed %u file(s), %.1f MiB (%s); %.1f MiB kept", (unsigned)removed,
                 freed / 1048576.0, wipe ? "driver changed" : "over the limit or unfinished", kept / 1048576.0);
}

int d3d_program_cache_on(void)
{
    if (program_cache_state) return program_cache_state > 0;
    program_cache_state = -1;
    const char *env = getenv("S2_PROGRAM_CACHE"), *base = void_app_cache_dir();
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    if ((env && *env == '0') || !base || !*base || formats <= 0) {
        void_log("[d3d9] program binary cache off (%s)", env && *env == '0' ? "S2_PROGRAM_CACHE=0" :
            formats <= 0 ? "no binary formats" : "no cache folder");
        return 0;
    }
    snprintf(program_cache_dir, sizeof program_cache_dir, "%s/glprog", base);
    mkdir(program_cache_dir, 0700);
    char off[640];
    snprintf(off, sizeof off, "%s/off", program_cache_dir);
    if (access(off, F_OK) == 0) { void_log("[d3d9] program binary cache off (%s)", off); return 0; }
    const char *renderer = (const char *)glGetString(GL_RENDERER), *version = (const char *)glGetString(GL_VERSION);
    uint64_t driver = 0xCBF29CE484222325ull;                     /* FNV-1a of "renderer\nversion" */
    for (const char *s = renderer ? renderer : ""; *s; ++s) driver = (driver ^ (uint8_t)*s) * 0x100000001B3ull;
    driver = (driver ^ (uint8_t)'\n') * 0x100000001B3ull;
    for (const char *s = version ? version : ""; *s; ++s) driver = (driver ^ (uint8_t)*s) * 0x100000001B3ull;
    program_cache_housekeeping(program_cache_dir, driver);
    program_cache_state = 1;
    void_log("[d3d9] program binary cache in %s (%d formats)", program_cache_dir, formats);
    return 1;
}

static uint64_t key_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void key_add(d3d_program_key *k, const void *data, size_t n)
{
    const uint8_t *s = (const uint8_t *)data;
    uint64_t a = k->a, b = k->b;
    for (size_t i = 0; i < n; ++i) {
        a = (a ^ s[i]) * 0x100000001B3ull;                       /* FNV-1a */
        b = (b + s[i] + 1) * 0x9E3779B97F4A7C15ull; b ^= b >> 29;  /* an unrelated multiply-xorshift */
    }
    k->a = a; k->b = b;
}

static void key_text(d3d_program_key *k, const char *text)
{
    uint64_t n = text ? strlen(text) : 0;
    key_add(k, &n, sizeof n);
    if (n) key_add(k, text, (size_t)n);
}

d3d_program_key d3d_program_key_of(const char *vs_text, const char *ps_text, const MOJOSHADER_parseData *attributes)
{
    static const char *renderer, *version;
    if (!renderer) { renderer = (const char *)glGetString(GL_RENDERER); version = (const char *)glGetString(GL_VERSION); }
    d3d_program_key k = { 0xCBF29CE484222325ull, 0x6A09E667F3BCC908ull };
    key_text(&k, "S2PB1");
    key_text(&k, renderer);
    key_text(&k, version);
    key_text(&k, vs_text);
    key_text(&k, ps_text);
    for (int i = 0; attributes && i < attributes->attribute_count && i < 16; ++i) {
        uint32_t index = (uint32_t)i;
        key_add(&k, &index, sizeof index);
        key_text(&k, attributes->attributes[i].name);
    }
    k.a = key_mix(k.a ^ 0x5851F42D4C957F2Dull);
    k.b = key_mix(k.b ^ k.a);
    return k;
}

static void program_cache_path(char *out, size_t cap, d3d_program_key k)
{
    snprintf(out, cap, "%s/%016llx%016llx.bin", program_cache_dir, (unsigned long long)k.a, (unsigned long long)k.b);
}

/* A linked program from the binary filed under key, or 0. */
GLuint d3d_program_cache_load(d3d_program_key key)
{
    if (!d3d_program_cache_on()) return 0;
    char path[700];
    program_cache_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) { ++stat_cache_misses; return 0; }
    program_cache_header h;
    void *data = NULL;
    GLuint gl = 0;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == PROGRAM_CACHE_MAGIC && h.version == PROGRAM_CACHE_VERSION &&
        h.a == key.a && h.b == key.b && h.length && h.length <= (64u << 20) && (data = malloc(h.length)) &&
        fread(data, 1, h.length, f) == h.length) {
        gl = glCreateProgram();
        glProgramBinary(gl, (GLenum)h.format, data, (GLsizei)h.length);
        GLint ok = 0;
        glGetProgramiv(gl, GL_LINK_STATUS, &ok);
        if (!ok) { d3d_gl_delete_program(gl); gl = 0; }
    }
    free(data);
    fclose(f);
    if (!gl) {
        while (glGetError() != GL_NO_ERROR) {}   /* a refused binary must not read as a draw's error */
        unlink(path);
        ++stat_cache_misses;
        return 0;
    }
    ++stat_cache_hits;
    return gl;
}

/* File a freshly linked program's binary under key (written aside, then renamed into place). */
void d3d_program_cache_store(d3d_program_key key, GLuint program)
{
    if (program_cache_state <= 0 || stat_cache_bytes > PROGRAM_CACHE_LIMIT) return;
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0 || length > (64 << 20)) return;
    void *data = malloc((size_t)length);
    GLenum format = 0;
    GLsizei got = 0;
    if (data) glGetProgramBinary(program, length, &got, &format, data);
    if (got > 0) {
        program_cache_header h = { PROGRAM_CACHE_MAGIC, PROGRAM_CACHE_VERSION, key.a, key.b, (uint32_t)format, (uint32_t)got };
        char path[700], temp[720];
        program_cache_path(path, sizeof path, key);
        snprintf(temp, sizeof temp, "%s.tmp", path);
        FILE *f = fopen(temp, "wb");
        if (f) {
            int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
            ok = (fclose(f) == 0) && ok;
            if (ok && rename(temp, path) == 0) stat_cache_bytes += (uint64_t)got + sizeof h;
            else unlink(temp);
        }
    }
    free(data);
    while (glGetError() != GL_NO_ERROR) {}
}

/* A shader object's source (malloc'd), as compiled: what a program key hashes for a generated half. */
char *d3d_shader_text(GLuint shader)
{
    GLint length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    char *text = (char *)malloc(length > 0 ? (size_t)length + 1 : 1);
    if (!text) return NULL;
    text[0] = 0;
    if (length > 0) glGetShaderSource(shader, length + 1, NULL, text);
    return text;
}

static void build_map(const MOJOSHADER_parseData *pd, umap_t *map, int *map_count, int *fc, int *ic, int *bc)
{
    *map_count = *fc = *ic = *bc = 0;
    for (int i = 0; i < pd->uniform_count; ++i) {
        const MOJOSHADER_uniform *u = &pd->uniforms[i];
        if (u->constant) continue;
        int count = u->array_count ? u->array_count : 1;
        map[*map_count].type = u->type;
        map[*map_count].index = u->index;
        map[*map_count].count = count;
        ++*map_count;
        if (u->type == MOJOSHADER_UNIFORM_FLOAT) *fc += count;
        else if (u->type == MOJOSHADER_UNIFORM_INT) *ic += count;
        else *bc += count;
    }
}

/* The vertex shader variant whose D3DCOLOR inputs are swizzled to RGBA. */
/* Patched GLSL of the vertex shader variant for a BGRA-input mask. */
static char *vertex_source(d3d_shader *vs, uint32_t mask)
{
    if (!mask) return strdup(vs->source);
    MOJOSHADER_swizzle swiz[16];
    unsigned n = 0;
    for (int i = 0; i < vs->parse->attribute_count && i < 16; ++i) {
        if (!(mask & (1u << i))) continue;
        swiz[n].usage = vs->parse->attributes[i].usage;
        swiz[n].index = (unsigned)vs->parse->attributes[i].index;
        /* MojoShader's SM1 swizzle lookup uses the conventional v-register
         * semantics even when an explicit DCL assigns a different one.
         * The game's UI declares COLOR0 in v1 (normally BLENDWEIGHT), so
         * address that lookup by the actual input register, not its DCL. */
        if (vs->parse->major_ver < 2) {
            static const MOJOSHADER_usage implicit[] = {
                MOJOSHADER_USAGE_POSITION, MOJOSHADER_USAGE_BLENDWEIGHT, MOJOSHADER_USAGE_BLENDINDICES,
                MOJOSHADER_USAGE_NORMAL, MOJOSHADER_USAGE_POINTSIZE, MOJOSHADER_USAGE_COLOR, MOJOSHADER_USAGE_COLOR,
                MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD,
                MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD, MOJOSHADER_USAGE_TEXCOORD,
                MOJOSHADER_USAGE_POSITION, MOJOSHADER_USAGE_NORMAL
            };
            unsigned reg;
            if (sscanf(vs->parse->attributes[i].name, "vs_v%u", &reg) == 1 && reg < sizeof implicit / sizeof implicit[0]) {
                swiz[n].usage = implicit[reg];
                swiz[n].index = reg == 6 || reg >= 15 ? 1u : reg >= 7 ? reg - 7 : 0u;
            }
        }
        swiz[n].swizzles[0] = 2; swiz[n].swizzles[1] = 1; swiz[n].swizzles[2] = 0; swiz[n].swizzles[3] = 3;
        ++n;
    }
    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main",
        vs->parse_code ? vs->parse_code : vs->bytecode, vs->parse_code ? vs->parse_size : vs->size,
        swiz, n, NULL, 0, NULL, NULL, NULL);
    char *src = NULL;
    if (pd && !pd->error_count) src = patch_source(pd->output, 0);
    if (pd) MOJOSHADER_freeParseData(pd);
    return src;
}

static GLuint vertex_variant(d3d_shader *vs, uint32_t mask)
{
    if (!mask) {
        if (!vs->gl_plain) vs->gl_plain = compile(GL_VERTEX_SHADER, vs->source);
        return vs->gl_plain;
    }
    char *src = vertex_source(vs, mask);
    GLuint sh = src ? compile(GL_VERTEX_SHADER, src) : 0;
    free(src);
    return sh;
}

/* D3D links a pixel input to a vertex output by semantic even when the
 * vertex shader never writes it; GLSL refuses. A POSITION input reads the
 * clip-space position the vertex shader wrote (before the GL fixups), an
 * unwritten colour reads 1 and anything else 0. Returns the patched sources
 * (NULL when that stage needs no change). */
static void reconcile_varyings(const char *vs_src, const char *ps_src, char **vs_out, char **ps_out)
{
    *vs_out = *ps_out = NULL;
    const char *needle = "in highp vec4 io_";
    char *ps = NULL, *vs = NULL;
    for (const char *p = strstr(ps_src, needle); p; p = strstr(p + 1, needle)) {
        unsigned u, i;
        if (sscanf(p + strlen(needle), "%u_%u", &u, &i) != 2) continue;
        char out_decl[48];
        snprintf(out_decl, sizeof out_decl, "out highp vec4 io_%u_%u;", u, i);
        if (strstr(vs_src, out_decl)) continue;
        if (u == 0 && i == 0 && strstr(vs_src, "gl_Position.y = gl_Position.y * vpFlip;")) {
            const char *base = vs ? vs : vs_src;
            char *a = insert_before(base, "void main()", "out highp vec4 io_0_0;\n");
            char *b = insert_before(a, "\tgl_Position.y = gl_Position.y * vpFlip;", "\tio_0_0 = gl_Position;\n");
            free(a);
            free(vs);
            vs = b;
            continue;
        }
        char in_decl[48], constant[80];
        snprintf(in_decl, sizeof in_decl, "in highp vec4 io_%u_%u;", u, i);
        snprintf(constant, sizeof constant, "const highp vec4 io_%u_%u = vec4(%s);", u, i, u == 10 ? "1.0" : "0.0");
        const char *base = ps ? ps : ps_src;
        const char *at = strstr(base, in_decl);
        if (!at) continue;
        size_t head = (size_t)(at - base), len = strlen(base);
        char *next = (char *)malloc(len + strlen(constant) + 1);
        memcpy(next, base, head);
        strcpy(next + head, constant);
        strcat(next, base + head + strlen(in_decl));
        free(ps);
        ps = next;
    }
    *vs_out = vs;
    *ps_out = ps;
}

/* d3d9_ff.c */
void d3d_ff_attributes(int *attr_count, int *usage, int *index);
GLuint d3d_ff_vertex_stage(d3d_device *d, d3d_shader *ps, uint32_t *id);
GLuint d3d_ff_pixel_stage(d3d_device *d, d3d_shader *vs, uint32_t *id, uint32_t *stage_mask);
void d3d_ff_uniforms(d3d_device *d, GLuint program, int vertex, int pixel);

/* Link vs and ps; a missing one is replaced by the generated shader object
 * ff_gl (named by ff_id), whose samplers are the stages in ff_samplers. */
static program_t *link_program(d3d_shader *vs, d3d_shader *ps, uint32_t bgra_mask, GLuint ff_gl, uint32_t ff_id,
    uint32_t ff_samplers)
{
    program_t *p = (program_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->vs = vs; p->ps = ps; p->bgra_mask = bgra_mask; p->ff_id = ff_id;
    /* The exact text each half compiles from: a BGRA variant or a varying fix of
     * the vertex shader is its own (vs_text), a fixed pixel shader too
     * (ps_fixed); the plain ones are the shaders' sources. */
    char *vs_fixed = NULL, *ps_fixed = NULL, *vs_text = NULL;
    if (vs && ps) reconcile_varyings(vs->source, ps->source, &vs_fixed, &ps_fixed);
    if (vs_fixed) {
        /* The same fix applied to the BGRA variant's source. */
        char *variant = vertex_source(vs, bgra_mask), *ignored = NULL;
        if (variant) reconcile_varyings(variant, ps->source, &vs_text, &ignored);
        free(variant); free(ignored); free(vs_fixed);
        if (!vs_text) { free(ps_fixed); program_destroy(p); return NULL; }
    } else if (vs && bgra_mask) {
        vs_text = vertex_source(vs, bgra_mask);
        if (!vs_text) { free(ps_fixed); program_destroy(p); return NULL; }
    }
    /* A binary filed under these texts (a generated half's read back from its
     * shader object; a pixel half as compile() filters it) skips both compiles
     * and the link. */
    d3d_program_key key = { 0, 0 };
    int cache = d3d_program_cache_on();
    if (cache) {
        char *ff_text = !vs || !ps ? d3d_shader_text(ff_gl) : NULL;
        const char *ps_plain = ps_fixed ? ps_fixed : ps ? ps->source : NULL;
        char *ps_filtered = ps_plain && s2_ui_fsr() ? d3d_ui_filter_source(ps_plain) : NULL;
        key = d3d_program_key_of(vs ? (vs_text ? vs_text : vs->source) : ff_text,
            ps ? (ps_filtered ? ps_filtered : ps_plain) : ff_text, vs ? vs->parse : NULL);
        free(ps_filtered);
        free(ff_text);
        p->gl = d3d_program_cache_load(key);
    }
    int cached = p->gl != 0;
    if (!cached) {
        if (vs_text) { p->vs_gl = compile(GL_VERTEX_SHADER, vs_text); p->own_vs = 1; }
        else p->vs_gl = vs ? vertex_variant(vs, 0) : ff_gl;
        if (ps_fixed) { p->ps_gl = compile(GL_FRAGMENT_SHADER, ps_fixed); p->own_ps = 1; }
        else {
            if (ps && !ps->gl_plain) ps->gl_plain = compile(GL_FRAGMENT_SHADER, ps->source);
            p->ps_gl = ps ? ps->gl_plain : ff_gl;
        }
    }
    free(vs_text);
    free(ps_fixed);
    if (!cached && (!p->vs_gl || !p->ps_gl)) { program_destroy(p); return NULL; }
    if (!cached) {
        p->gl = glCreateProgram();
        glAttachShader(p->gl, p->vs_gl);
        glAttachShader(p->gl, p->ps_gl);
        if (cache) glProgramParameteri(p->gl, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    if (vs) {
        const MOJOSHADER_parseData *vpd = vs->parse;
        p->attr_count = vpd->attribute_count < 16 ? vpd->attribute_count : 16;
        for (int i = 0; i < p->attr_count; ++i) {
            if (!cached) glBindAttribLocation(p->gl, (GLuint)i, vpd->attributes[i].name);
            p->attr_usage[i] = vpd->attributes[i].usage;
            p->attr_index[i] = vpd->attributes[i].index;
        }
    } else {
        d3d_ff_attributes(&p->attr_count, p->attr_usage, p->attr_index);   /* fixed layout locations */
    }
    ++stat_programs;
    if (!cached) {
        uint64_t started = void_monotonic_ns();
        glLinkProgram(p->gl);
        GLint ok = 0;
        glGetProgramiv(p->gl, GL_LINK_STATUS, &ok);
        stat_link_ns += void_monotonic_ns() - started;
        ++stat_links;
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(p->gl, sizeof log, NULL, log);
            void_log("[d3d9] program link failed: %s", log);
            void_log("[d3d9] vs:\n%s\nps:\n%s", vs ? vs->source : "(fixed function)", ps ? ps->source : "(fixed function)");
            program_destroy(p);
            return NULL;
        }
        if (cache) d3d_program_cache_store(key, p->gl);
    }
    d3d_gl_use_program(p->gl);
    p->loc_vs_f = glGetUniformLocation(p->gl, "vs_uniforms_vec4");
    p->loc_vs_i = glGetUniformLocation(p->gl, "vs_uniforms_ivec4");
    p->loc_vs_b = glGetUniformLocation(p->gl, "vs_uniforms_bool");
    p->loc_ps_f = glGetUniformLocation(p->gl, "ps_uniforms_vec4");
    p->loc_ps_i = glGetUniformLocation(p->gl, "ps_uniforms_ivec4");
    p->loc_ps_b = glGetUniformLocation(p->gl, "ps_uniforms_bool");
    p->loc_flip = glGetUniformLocation(p->gl, "vpFlip");
    p->loc_half = glGetUniformLocation(p->gl, "vpHalfPixel");
    p->loc_alpha_func = glGetUniformLocation(p->gl, "vpAlphaFunc");
    p->loc_alpha_ref = glGetUniformLocation(p->gl, "vpAlphaRef");
    if (p->loc_flip >= 0) glUniform1f(p->loc_flip, -1.0f);
    p->last_alpha_func = -1;
    if (vs) build_map(vs->parse, p->vs_map, &p->vs_map_count, &p->vs_f_count, &p->vs_i_count, &p->vs_b_count);
    if (ps) build_map(ps->parse, p->ps_map, &p->ps_map_count, &p->ps_f_count, &p->ps_i_count, &p->ps_b_count);
    p->vs_f = (float *)calloc((size_t)p->vs_f_count * 4 + 4, sizeof(float));
    p->ps_f = (float *)calloc((size_t)p->ps_f_count * 4 + 4, sizeof(float));
    p->vs_i = (int32_t *)calloc((size_t)p->vs_i_count * 4 + 4, sizeof(int32_t));
    p->ps_i = (int32_t *)calloc((size_t)p->ps_i_count * 4 + 4, sizeof(int32_t));
    p->vs_b = (int32_t *)calloc((size_t)p->vs_b_count + 1, sizeof(int32_t));
    p->ps_b = (int32_t *)calloc((size_t)p->ps_b_count + 1, sizeof(int32_t));
    p->vs_f_loc = (GLint *)malloc(((size_t)p->vs_f_count + 1) * sizeof(GLint));
    p->ps_f_loc = (GLint *)malloc(((size_t)p->ps_f_count + 1) * sizeof(GLint));
    for (int i = 0; i <= p->vs_f_count; ++i) p->vs_f_loc[i] = -2;
    for (int i = 0; i <= p->ps_f_count; ++i) p->ps_f_loc[i] = -2;
    /* Samplers: pixel sampler n -> unit n, vertex sampler n -> unit 16 + n. */
    if (!ps) p->ps_sampler_mask = ff_samplers;
    for (int i = 0; ps && i < ps->parse->sampler_count; ++i) {
        const MOJOSHADER_sampler *sm = &ps->parse->samplers[i];
        GLint loc = glGetUniformLocation(p->gl, sm->name);
        if (loc >= 0 && sm->index < 16) {
            glUniform1i(loc, sm->index);
            p->ps_sampler_mask |= 1u << sm->index;
            p->ps_sampler_type[sm->index] = sm->type;
        }
    }
    for (int i = 0; vs && i < vs->parse->sampler_count; ++i) {
        const MOJOSHADER_sampler *sm = &vs->parse->samplers[i];
        GLint loc = glGetUniformLocation(p->gl, sm->name);
        if (loc >= 0 && sm->index < 4) {
            glUniform1i(loc, 16 + sm->index);
            p->vs_sampler_mask |= 1u << sm->index;
            p->vs_sampler_type[sm->index] = sm->type;
        }
    }
    return p;
}

static void pack(const umap_t *map, int count, const float (*f)[4], const int32_t (*iv)[4], const int32_t *b,
    float *of, int32_t *oi, int32_t *ob)
{
    for (int k = 0; k < count; ++k) {
        const umap_t *m = &map[k];
        if (m->type == MOJOSHADER_UNIFORM_FLOAT) { memcpy(of, f[m->index], 16u * (uint32_t)m->count); of += 4 * m->count; }
        else if (m->type == MOJOSHADER_UNIFORM_INT) { memcpy(oi, iv[m->index], 16u * (uint32_t)m->count); oi += 4 * m->count; }
        else { for (int i = 0; i < m->count; ++i) ob[i] = b[m->index + i] != 0; ob += m->count; }
    }
}

/* Uploads the vec4 range of `fresh` that differs from what the program holds
 * (`held`, updated): a draw usually changes a matrix or two of the hundreds
 * of constants its program declares. A range that starts past element 0 goes
 * to that element's own location (GLSL ES does not promise consecutive ones);
 * when the compiler dropped that element, the whole array goes from the base. */
static void upload_vec4(GLuint prog, const char *name, GLint base, GLint *locs, float *held, const float *fresh,
    int count)
{
    int first = 0, last = count - 1;
    while (first < count && !memcmp(held + 4 * first, fresh + 4 * first, 16)) ++first;
    if (first == count) return;
    while (last > first && !memcmp(held + 4 * last, fresh + 4 * last, 16)) --last;
    memcpy(held + 4 * first, fresh + 4 * first, 16u * (uint32_t)(last - first + 1));
    if (first && locs[first] == -2) {
        char element[64];
        snprintf(element, sizeof element, "%s[%d]", name, first);
        locs[first] = glGetUniformLocation(prog, element);
    }
    if (!first) glUniform4fv(base, last + 1, held);
    else if (locs[first] >= 0) glUniform4fv(locs[first], last - first + 1, held + 4 * first);
    else glUniform4fv(base, last + 1, held);
}

static void upload_ints(GLint loc, int32_t *held, const int32_t *fresh, int count, int vec4)
{
    size_t bytes = (size_t)count * (vec4 ? 16u : 4u);
    if (!memcmp(held, fresh, bytes)) return;
    memcpy(held, fresh, bytes);
    if (vec4) glUniform4iv(loc, count, held);
    else glUniform1iv(loc, count, held);
}

void d3d_shader_upload(d3d_device *dev, float half_x, float half_y, int alpha_func, float alpha_ref)
{
    program_t *p = current_program;
    if (!p) return;
    if (p->uploaded_generation != d3d_constant_generation) {
        /* D3D9 limits: 256 vertex and 224 pixel float registers, 16 int and 16 bool each. */
        static float vs_f[256 * 4 + 4], ps_f[256 * 4 + 4];
        static int32_t vs_i[16 * 4 + 4], ps_i[16 * 4 + 4], vs_b[16 + 1], ps_b[16 + 1];
        if (p->vs_f_count > 256 || p->ps_f_count > 256 || p->vs_i_count > 16 || p->ps_i_count > 16 ||
            p->vs_b_count > 16 || p->ps_b_count > 16)
            void_fatal("shader constants beyond the D3D9 register files (%d/%d/%d, %d/%d/%d)",
                p->vs_f_count, p->vs_i_count, p->vs_b_count, p->ps_f_count, p->ps_i_count, p->ps_b_count);
        pack(p->vs_map, p->vs_map_count, (const float (*)[4])dev->st.vs_f, (const int32_t (*)[4])dev->st.vs_i, dev->st.vs_b, vs_f, vs_i, vs_b);
        pack(p->ps_map, p->ps_map_count, (const float (*)[4])dev->st.ps_f, (const int32_t (*)[4])dev->st.ps_i, dev->st.ps_b, ps_f, ps_i, ps_b);
        if (p->loc_vs_f >= 0 && p->vs_f_count)
            upload_vec4(p->gl, "vs_uniforms_vec4", p->loc_vs_f, p->vs_f_loc, p->vs_f, vs_f, p->vs_f_count);
        if (p->loc_vs_i >= 0 && p->vs_i_count) upload_ints(p->loc_vs_i, p->vs_i, vs_i, p->vs_i_count, 1);
        if (p->loc_vs_b >= 0 && p->vs_b_count) upload_ints(p->loc_vs_b, p->vs_b, vs_b, p->vs_b_count, 0);
        if (p->loc_ps_f >= 0 && p->ps_f_count)
            upload_vec4(p->gl, "ps_uniforms_vec4", p->loc_ps_f, p->ps_f_loc, p->ps_f, ps_f, p->ps_f_count);
        if (p->loc_ps_i >= 0 && p->ps_i_count) upload_ints(p->loc_ps_i, p->ps_i, ps_i, p->ps_i_count, 1);
        if (p->loc_ps_b >= 0 && p->ps_b_count) upload_ints(p->loc_ps_b, p->ps_b, ps_b, p->ps_b_count, 0);
        p->uploaded_generation = d3d_constant_generation;
    }
    if (p->loc_half >= 0 && (p->last_half[0] != half_x || p->last_half[1] != half_y)) {
        glUniform2f(p->loc_half, half_x, half_y);
        p->last_half[0] = half_x; p->last_half[1] = half_y;
    }
    if (p->loc_alpha_func >= 0 && (p->last_alpha_func != alpha_func || p->last_alpha_ref != alpha_ref)) {
        glUniform1i(p->loc_alpha_func, alpha_func);
        glUniform1f(p->loc_alpha_ref, alpha_ref);
        p->last_alpha_func = alpha_func; p->last_alpha_ref = alpha_ref;
    }
}

/* Bind the program for (vs, ps, D3DCOLOR inputs). bgra_of(usage, index) says
 * whether that input is fed by a D3DCOLOR element. */
int d3d_shader_bind2(d3d_device *dev, d3d_shader *vs, d3d_shader *ps, int (*bgra_of)(d3d_device *, int, int),
    int *attr_count, const int **attr_usage, const int **attr_index, uint32_t *ps_samplers, uint32_t *vs_samplers,
    const int **ps_types, const int **vs_types)
{
    if ((vs && !shader_type_ok(vs, 0)) || (ps && !shader_type_ok(ps, 1)) || (!vs && !ps)) return 0;
    uint32_t mask = 0, ff_id = 0, ff_samplers = 0;
    GLuint ff_gl = 0;
    for (int i = 0; vs && i < vs->parse->attribute_count && i < 16; ++i)
        if (bgra_of(dev, vs->parse->attributes[i].usage, vs->parse->attributes[i].index)) mask |= 1u << i;
    /* A missing shader is replaced by the fixed-function half for the current state. */
    if (!vs) ff_gl = d3d_ff_vertex_stage(dev, ps, &ff_id);
    else if (!ps) ff_gl = d3d_ff_pixel_stage(dev, vs, &ff_id, &ff_samplers);
    if ((!vs || !ps) && !ff_gl) return 0;
    uint32_t h = 0;
    program_t *p = find_program(vs, ps, mask, ff_id, &h);
    if (!p) {
        p = link_program(vs, ps, mask, ff_gl, ff_id, ff_samplers);
        if (!p) return 0;
        p->next = programs[h];
        programs[h] = p;
    }
    d3d_gl_use_program(p->gl);
    current_program = p;
    if (!vs || !ps) d3d_ff_uniforms(dev, p->gl, !vs, !ps);
    *attr_count = p->attr_count;
    *attr_usage = p->attr_usage;
    *attr_index = p->attr_index;
    *ps_samplers = p->ps_sampler_mask;
    *vs_samplers = p->vs_sampler_mask;
    *ps_types = p->ps_sampler_type;
    *vs_types = p->vs_sampler_type;
    return 1;
}

int d3d_shader_bind(d3d_device *dev, d3d_shader *vs, d3d_shader *ps, uint32_t *attribute_mask,
    int *attribute_usage, int *attribute_index, int *attribute_count, int *needs_bgra)
{
    (void)dev; (void)vs; (void)ps; (void)attribute_mask; (void)attribute_usage; (void)attribute_index;
    (void)attribute_count; (void)needs_bgra;
    return 0;
}

void d3d_shader_forget_current(void) { current_program = NULL; }

/* Frame tracing (d3d9_gl.c): the current program's vertex GLSL, once per
 * program, and the packed vertex constants it was given. */
void d3d_shader_trace(void)
{
    program_t *p = current_program;
    if (!p) return;
    void_log("[trace]   shader program %u BGRA mask %04X", p->gl, p->bgra_mask);
    static program_t *shown[256];
    static int shown_count;
    int seen = 0;
    for (int i = 0; i < shown_count; ++i) if (shown[i] == p) seen = 1;
    if (!seen && shown_count < 256) {
        shown[shown_count++] = p;
        /* The log truncates long lines: the whole sources go to <data>/glsl. */
        char path[1100];
        snprintf(path, sizeof path, "%s/glsl", void_data_dir());
        mkdir(path, 0775);
        for (int stage = 0; stage < 2; ++stage) {
            GLuint shader = stage ? p->ps_gl : p->vs_gl;
            GLint length = 0;
            glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
            if (length <= 0) continue;
            char *source = malloc((size_t)length);
            if (!source) continue;
            glGetShaderSource(shader, length, NULL, source);
            void_log("[trace]   compiled %s GLSL of program %u:\n%s", stage ? "pixel" : "vertex", p->gl, source);
            snprintf(path, sizeof path, "%s/glsl/p%u_%s.glsl", void_data_dir(), p->gl, stage ? "ps" : "vs");
            FILE *f = fopen(path, "wb");
            if (f) { fputs(source, f); fclose(f); }
            free(source);
        }
    }
    char line[1024];
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < p->vs_map_count && n < sizeof line - 64; ++i)
        n += (size_t)snprintf(line + n, sizeof line - n, " c%d x%d", p->vs_map[i].index, p->vs_map[i].count);
    void_log("[trace]   vs uniform map:%s", line);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < p->vs_f_count && i < 8; ++i)
        n += (size_t)snprintf(line + n, sizeof line - n, " [%d](%g %g %g %g)", i, p->vs_f[4 * i], p->vs_f[4 * i + 1],
            p->vs_f[4 * i + 2], p->vs_f[4 * i + 3]);
    void_log("[trace]   vs uniforms:%s", line);
    /* Skinned shaders keep their bone palette low and the transforms at
     * c200 and up (RendererD3D's skinning effects): show those too. */
    n = 0;
    line[0] = 0;
    for (int i = 200; i < p->vs_f_count && i < 216 && n < sizeof line - 96; ++i)
        n += (size_t)snprintf(line + n, sizeof line - n, " [%d](%g %g %g %g)", i, p->vs_f[4 * i], p->vs_f[4 * i + 1],
            p->vs_f[4 * i + 2], p->vs_f[4 * i + 3]);
    if (n) void_log("[trace]   vs uniforms c200+:%s", line);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < p->ps_map_count && n < sizeof line - 64; ++i)
        n += (size_t)snprintf(line + n, sizeof line - n, " c%d x%d", p->ps_map[i].index, p->ps_map[i].count);
    void_log("[trace]   ps uniform map:%s", line);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < p->ps_f_count && i < 8; ++i)
        n += (size_t)snprintf(line + n, sizeof line - n, " [%d](%g %g %g %g)", i, p->ps_f[4 * i], p->ps_f[4 * i + 1],
            p->ps_f[4 * i + 2], p->ps_f[4 * i + 3]);
    void_log("[trace]   ps uniforms:%s", line);
}
/* -------------------------------------------------- software vertices
 * ProcessVertices runs the original programmable vertex shader without the
 * rasterizer epilogue. Software shaders have much larger register files than
 * GLES uniforms; packed constants live in texelFetch-only textures instead.
 * The result is read back once, then scattered according to the D3D output
 * declaration. This path is synchronous and used for preprocessing/portraits,
 * while normal draws keep their small uniform files and asynchronous queue. */
typedef struct process_program {
    struct process_program *next;
    const MOJOSHADER_parseData *pd;
    GLuint gl, vertex, fragment, constants[2];
    uint32_t bgra;
    umap_t *map;
    int map_count, fc, ic, bc, outputs, words;
    int usage[16], index[16], offset[16], components[16];
    uint8_t masks[16];
} process_program;

static void process_destroy(d3d_shader *s)
{
    process_program *p = s->process;
    while (p) {
        process_program *next = p->next;
        if (p->gl) d3d_gl_delete_program(p->gl);
        if (p->vertex) glDeleteShader(p->vertex);
        if (p->fragment) glDeleteShader(p->fragment);
        glDeleteTextures(2, p->constants);
        if (p->pd) MOJOSHADER_freeParseData(p->pd);
        free(p->map);
        free(p);
        p = next;
    }
    s->process = NULL;
}

/* The bracket expressions Mojo emits contain no nested array subscripts.
 * Retain DEF literals and array-base macros, replacing only packed uniforms.
 * In particular do not apply flip, half-pixel, or clip-depth conversion to
 * the camera-space POSITION that this shader writes. */
static char *process_source(const char *src, int fc, int ic, int bc)
{
    size_t size = strlen(src);
    char *body = malloc(size * 2 + 2048), *w = body;
    if (!body) return NULL;
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        const char *p = line;
        while (*p == '\t' || *p == ' ') ++p;
#define PV_PREFIX(text) (!strncmp(p, (text), sizeof(text) - 1))
        int drop = PV_PREFIX("uniform vec4 vs_uniforms_vec4[") ||
            PV_PREFIX("uniform ivec4 vs_uniforms_ivec4[") ||
            PV_PREFIX("uniform bool vs_uniforms_bool[") || PV_PREFIX("uniform float vpFlip;") ||
            PV_PREFIX("gl_Position.y = gl_Position.y * vpFlip;") ||
            PV_PREFIX("gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;");
#undef PV_PREFIX
        if (!drop) { memcpy(w, line, n); w += n; }
        line += n;
    }
    *w = 0;
    char *converted = malloc(size * 2 + 2048); w = converted;
    if (!converted) { free(body); return NULL; }
    const char *names[] = {"vs_uniforms_vec4[", "vs_uniforms_ivec4[", "vs_uniforms_bool["};
    const char *funcs[] = {"pv_c(", "pv_i(", "pv_b("};
    for (const char *p = body; *p;) {
        int match = -1;
        for (int k = 0; k < 3; ++k) if (!strncmp(p, names[k], strlen(names[k]))) { match = k; break; }
        if (match < 0) { *w++ = *p++; continue; }
        p += strlen(names[match]);
        const char *end = strchr(p, ']');
        if (!end) { free(body); free(converted); return NULL; }
        w += sprintf(w, "%s%.*s)", funcs[match], (int)(end - p), p);
        p = end + 1;
    }
    *w = 0; free(body);
    char helpers[1536];
    snprintf(helpers, sizeof helpers,
        "uniform highp sampler2D pv_float;\n"
        "uniform highp isampler2D pv_integer;\n"
        "vec4 pv_c(int i) { return (i >= 0 && i < %d) ? texelFetch(pv_float, ivec2(i & 127, i >> 7), 0) : vec4(0.0); }\n"
        "ivec4 pv_i(int i) { return (i >= 0 && i < %d) ? texelFetch(pv_integer, ivec2(i & 127, i >> 7), 0) : ivec4(0); }\n"
        "bool pv_b(int i) { int j = %d + i; int t = j >> 2; return (i >= 0 && i < %d) && texelFetch(pv_integer, ivec2(t & 127, t >> 7), 0)[j & 3] != 0; }\n",
        fc, ic, ic * 4, bc);
    /* Definitions/macros below this point can call the helpers. */
    char *out = insert_before(converted, "void main()", helpers);
    free(converted);
    if (!out) return NULL;
    char *fixed = fix_address_moves(out); free(out);
    return fixed;
}

/* Track the actual written components of each vs_3 output register; DCL's
 * mask declares a semantic, it does not write it. Unwritten destination
 * components must survive ProcessVertices. Output registers are write-only,
 * so an OUTPUT first operand outside DCL is a destination. */
static void process_masks(const d3d_shader *s, process_program *p)
{
    uint8_t masks[16] = {0};
    int usages[16], indices[16];
    for (int i = 0; i < 16; ++i) usages[i] = indices[i] = -1;
    const uint32_t *code = (const uint32_t *)s->bytecode;
    uint32_t words = s->size / 4;
    for (uint32_t at = 1; at < words;) {
        uint32_t op = code[at], id = op & 0xFFFF;
        if (id == 0xFFFF) break;
        if (id == 0xFFFE) { at += 1 + ((op >> 16) & 0x7FFF); continue; }
        uint32_t len = (op >> 24) & 15;
        if (at + len >= words) break;
        if (id == 31 && len >= 2) {
            uint32_t d = code[at + 2], type = ((d >> 28) & 7) | ((d >> 8) & 0x18), reg = d & 0x7FF;
            if (type == 6 && reg < 16) { usages[reg] = code[at + 1] & 15; indices[reg] = (code[at + 1] >> 16) & 15; }
        } else if (len) {
            uint32_t d = code[at + 1 + ((op >> 28) & 1)], type = ((d >> 28) & 7) | ((d >> 8) & 0x18), reg = d & 0x7FF;
            if (type == 6 && reg < 16) masks[reg] |= (d >> 16) & 15;
        }
        at += 1 + len;
    }
    for (int i = 0; i < p->outputs; ++i)
        for (int j = 0; j < 16; ++j)
            if (p->usage[i] == usages[j] && p->index[i] == indices[j]) p->masks[i] = masks[j];
}

static process_program *process_link(d3d_device *d, d3d_shader *s)
{
    MOJOSHADER_swizzle swiz[16]; unsigned ns = 0;
    uint32_t bgra = 0;
    d3d_decl *decl = d->st.decl;
    for (int i = 0; i < s->parse->attribute_count && i < 16; ++i)
        for (int j = 0; decl && j < decl->count; ++j)
            if (decl->elements[j].type == 4 && decl->elements[j].usage == s->parse->attributes[i].usage &&
                decl->elements[j].usage_index == s->parse->attributes[i].index) {
                bgra |= 1u << i;
                swiz[ns].usage = s->parse->attributes[i].usage; swiz[ns].index = s->parse->attributes[i].index;
                swiz[ns].swizzles[0] = 2; swiz[ns].swizzles[1] = 1; swiz[ns].swizzles[2] = 0; swiz[ns].swizzles[3] = 3; ++ns;
                break;
            }
    for (process_program *p = s->process; p; p = p->next) if (p->bgra == bgra) return p;
    process_program *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->bgra = bgra;
    uint32_t size = s->size;
    int lo = relative_base(s->parse);
    uint8_t *patched = lo >= 0 ? with_synthetic_ctab(s->bytecode, s->size, 0, (uint32_t)lo, D3D_SOFTWARE_VS_CONSTANTS, &size) : NULL;
    if (lo >= 0 && !patched) goto failed;
    p->pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", patched ? patched : s->bytecode, size,
        swiz, ns, NULL, 0, NULL, NULL, NULL);
    free(patched);
    if (!p->pd || p->pd->error_count || p->pd->output_count > 16 || p->pd->attribute_count > 16) goto failed;
    p->outputs = p->pd->output_count;
    if (!p->outputs) goto failed;
    p->map = calloc((size_t)p->pd->uniform_count + 1, sizeof *p->map);
    if (!p->map) goto failed;
    build_map(p->pd, p->map, &p->map_count, &p->fc, &p->ic, &p->bc);
    if (p->fc > D3D_SOFTWARE_VS_CONSTANTS || p->ic > D3D_SOFTWARE_VS_INT_CONSTANTS || p->bc > D3D_SOFTWARE_VS_BOOL_CONSTANTS) goto failed;
    for (int i = 0; i < p->map_count; ++i) {
        umap_t *m = &p->map[i];
        uint32_t limit = m->type == MOJOSHADER_UNIFORM_FLOAT ? D3D_SOFTWARE_VS_CONSTANTS :
            m->type == MOJOSHADER_UNIFORM_INT ? D3D_SOFTWARE_VS_INT_CONSTANTS : D3D_SOFTWARE_VS_BOOL_CONSTANTS;
        if (m->index < 0 || m->count < 0 || (uint32_t)m->index > limit || (uint32_t)m->count > limit - (uint32_t)m->index) goto failed;
    }
    char *source = process_source(p->pd->output, p->fc, p->ic, p->bc);
    p->vertex = source ? compile(GL_VERTEX_SHADER, source) : 0; free(source);
    p->fragment = compile(GL_FRAGMENT_SHADER, "#version 300 es\nprecision highp float;\nout vec4 c; void main(){ c = vec4(0.0); }\n");
    if (!p->vertex || !p->fragment) goto failed;
    p->gl = glCreateProgram(); glAttachShader(p->gl, p->vertex); glAttachShader(p->gl, p->fragment);
    for (int i = 0; i < p->pd->attribute_count; ++i) glBindAttribLocation(p->gl, (GLuint)i, p->pd->attributes[i].name);
    char names[16][40]; const char *varyings[16];
    for (int i = 0; i < p->outputs; ++i) {
        p->usage[i] = p->pd->outputs[i].usage; p->index[i] = p->pd->outputs[i].index;
        if (p->usage[i] == 0 && !p->index[i]) strcpy(names[i], "gl_Position");
        else if (p->usage[i] == 4 && !p->index[i]) strcpy(names[i], "gl_PointSize");
        else snprintf(names[i], sizeof names[i], "io_%d_%d", p->usage[i], p->index[i]);
        varyings[i] = names[i];
    }
    glTransformFeedbackVaryings(p->gl, p->outputs, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p->gl);
    GLint ok = 0; glGetProgramiv(p->gl, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p->gl, sizeof log, NULL, log); void_log("[pv] link failed: %s", log); goto failed; }
    for (int i = 0; i < p->outputs; ++i) {
        GLsizei n; GLenum type; GLint count; char name[64];
        glGetTransformFeedbackVarying(p->gl, (GLuint)i, sizeof name, &n, &count, &type, name);
        p->components[i] = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3 : type == GL_FLOAT_VEC4 ? 4 : 0;
        if (!p->components[i] || count != 1) goto failed;
        p->offset[i] = p->words; p->words += p->components[i];
    }
    process_masks(s, p);
    glGenTextures(2, p->constants);
    p->next = s->process; s->process = p;
    void_log("[pv] linked raw VS: %d inputs, %d outputs, constants %d/%d/%d", p->pd->attribute_count, p->outputs, p->fc, p->ic, p->bc);
    return p;
failed:
    if (p->pd && p->pd->error_count) void_log("[pv] translation failed: %s", p->pd->errors[0].error);
    /* Use the common release path without disturbing existing variants. */
    { d3d_shader dead = {0}; dead.process = p; process_destroy(&dead); }
    return NULL;
}

typedef struct {
    uint32_t first, dest, count, stride, result, flags;
    d3d_buffer *buffer;
    d3d_decl *decl;
} process_args;

static int process_output(const d3d_decl *decl, uint32_t *stride)
{
    if (!decl || decl->count < 1 || decl->count > 64) return 0;
    uint32_t end = 0;
    for (int i = 0; i < decl->count; ++i) {
        const d3d_vertex_element *e = &decl->elements[i];
        uint32_t bytes = e->type <= 3 ? 4u * (e->type + 1) : e->type == 4 ? 4u : 0;
        if (e->stream || e->method || !bytes || e->offset < end) return 0;
        end = e->offset + bytes;
    }
    *stride = end;
    return end != 0;
}

static void run_process(void *args)
{
    process_args *a = args;
    d3d_device *d = d3d_rt_device();
    d3d_shader *s = d->st.vs;
    process_program *p = process_link(d, s);
    if (!p || !d3d_process_inputs(d, p->pd, a->first, a->count, 0)) return;
    for (int i = 0; i < p->outputs; ++i) if (p->masks[i]) {
        int found = 0;
        for (int j = 0; j < a->decl->count; ++j)
            if (a->decl->elements[j].usage == p->usage[i] && a->decl->elements[j].usage_index == p->index[i]) found = 1;
        if (!found) return;
    }
    /* Report earlier renderer errors separately from this operation. */
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR;) void_log("[pv] earlier GL error %X", error);

    size_t ftexels = ((size_t)p->fc + 127) / 128 * 128;
    size_t itexels = ((size_t)p->ic + ((size_t)p->bc + 3) / 4 + 127) / 128 * 128;
    if (!ftexels) ftexels = 128; if (!itexels) itexels = 128;
    float *floats = calloc(ftexels * 4, 4);
    int32_t *integers = calloc(itexels * 4, 4);
    if (!floats || !integers) { free(floats); free(integers); a->result = D3DERR_OUTOFVIDEOMEMORY; return; }
    pack(p->map, p->map_count, d->st.vs_f, d->st.vs_i, d->st.vs_b, floats, integers, integers + p->ic * 4);
    GLint old_vao, old_array, old_program, old_tf, old_tf_buffer, old_copy, old_active, old_texture[2], old_sampler[2];
    GLint old_unpack, unpack_alignment, unpack_row, unpack_x, unpack_y;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao); glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program); glGetIntegerv(GL_TRANSFORM_FEEDBACK_BINDING, &old_tf);
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, &old_tf_buffer); glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &old_copy);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active); glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_unpack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_x); glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_y);
    GLboolean discarded = glIsEnabled(GL_RASTERIZER_DISCARD);
    GLuint vao, tf, buffer;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenTransformFeedbacks(1, &tf); glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, tf);
    glGenBuffers(1, &buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    size_t bytes = (size_t)a->count * (size_t)p->words * 4;
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    d3d_gl_use_program(p->gl);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0); glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    for (int i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + 30 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture[i]); glGetIntegerv(GL_SAMPLER_BINDING, &old_sampler[i]);
        glBindSampler((GLuint)(30 + i), 0); glBindTexture(GL_TEXTURE_2D, p->constants[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, i ? GL_RGBA32I : GL_RGBA32F, 128, (GLsizei)((i ? itexels : ftexels) / 128), 0,
            i ? GL_RGBA_INTEGER : GL_RGBA, i ? GL_INT : GL_FLOAT, i ? (void *)integers : (void *)floats);
        glUniform1i(glGetUniformLocation(p->gl, i ? "pv_integer" : "pv_float"), 30 + i);
    }
    free(floats); free(integers);
    if (d3d_process_inputs(d, p->pd, a->first, a->count, 1)) {
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, (GLsizei)a->count); glEndTransformFeedback();
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
        const float *data = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
        if (data) {
            size_t output_bytes = (size_t)a->count * a->stride;
            uint8_t *dst = malloc(output_bytes);
            if (!dst) { glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER); a->result = D3DERR_OUTOFVIDEOMEMORY; goto restore; }
            memcpy(dst, GPTR(a->buffer->shadow + a->dest * a->stride), output_bytes);
            for (uint32_t v = 0; v < a->count; ++v) {
                for (int i = 0; i < a->decl->count; ++i) {
                    const d3d_vertex_element *e = &a->decl->elements[i];
                    for (int j = 0; j < p->outputs; ++j) if (e->usage == p->usage[j] && e->usage_index == p->index[j]) {
                        const float *value = data + (size_t)v * p->words + p->offset[j];
                        for (int k = 0; k < (e->type <= 3 ? e->type + 1 : 4) && k < p->components[j]; ++k) {
                            if (!(p->masks[j] & (1u << k))) continue;
                            if (e->type <= 3) memcpy(dst + (size_t)v * a->stride + e->offset + k * 4, value + k, 4);
                            else {
                                float x = fminf(1.0f, fmaxf(0.0f, value[k]));
                                dst[(size_t)v * a->stride + e->offset + (k == 0 ? 2 : k == 2 ? 0 : k)] = (uint8_t)floorf(x * 255.0f + 0.5f);
                            }
                        }
                        break;
                    }
                }
            }
            GLboolean valid = glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
            GLenum error = glGetError();
            if (valid && error == GL_NO_ERROR) {
                glBindBuffer(GL_COPY_WRITE_BUFFER, a->buffer->gl);
                glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)((size_t)a->dest * a->stride), (GLsizeiptr)((size_t)a->count * a->stride), dst);
                error = glGetError();
                if (error == GL_NO_ERROR) {
                    memcpy(GPTR(a->buffer->shadow + a->dest * a->stride), dst, output_bytes);
                    a->result = D3D_OK;
                }
            }
            if (error != GL_NO_ERROR) void_log("[pv] GL transform/readback error %X", error);
            free(dst);
        }
    }
restore:
    if (!discarded) glDisable(GL_RASTERIZER_DISCARD);
    glBindVertexArray((GLuint)old_vao); glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, (GLuint)old_tf); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)old_tf_buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)old_copy); d3d_gl_use_program((GLuint)old_program);
    for (int i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + 30 + i); glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture[i]); glBindSampler((GLuint)(30 + i), (GLuint)old_sampler[i]);
    }
    glActiveTexture((GLenum)old_active); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)old_unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment); glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_x); glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_y);
    glDeleteBuffers(1, &buffer); glDeleteTransformFeedbacks(1, &tf); glDeleteVertexArrays(1, &vao);
}

void d3d_buffer_flush(d3d_buffer *b);
uint32_t d3d_process_vertices(d3d_device *d, uint32_t first, uint32_t dest, uint32_t count, d3d_buffer *buffer, d3d_decl *decl, uint32_t flags)
{
    uint32_t stride;
    d3d_shader *s = d->st.vs;
    uint32_t allowed = 1u | D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK;
    if (!buffer || buffer->index || buffer->dev != d || !s || s->pixel || s->parse->major_ver != 3 ||
        !process_output(decl, &stride) || decl->dev != d || (flags & ~allowed) || count > INT32_MAX ||
        ((uint64_t)dest + count) * stride > buffer->size || !d3d_process_inputs(d, s->parse, first, count, 0)) return D3DERR_INVALIDCALL;
    if (!count) return D3D_OK;
    for (int i = 0; i < 16; ++i) if (d->st.streams[i]) d3d_buffer_flush(d->st.streams[i]);
    d3d_buffer_flush(buffer);
    d3d_rt_sync_state(d);
    process_args a = { first, dest, count, stride, D3DERR_INVALIDCALL, flags, buffer, decl };
    d3d_rt_sync(run_process, &a);
    static uint32_t logged;
    if (a.result || logged++ < 8) void_log("[pv] processed %u vertices, stride %u: %08X", count, stride, a.result);
    return a.result;
}
