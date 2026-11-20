/* Direct3D 9 shader bytecode -> GLSL for the native renderer (s2r_translate.h).
 *
 * MojoShader (reference/mojoshader with the port's patch, GLSL ES 3 profile,
 * built with MOJOSHADER_FLIP_RENDERTARGET and MOJOSHADER_DEPTH_CLIPPING)
 * translates the bytecode. The text fixes are the ones the Direct3D 9 -> GLES
 * layer applies at run time (android/host/d3d9_shader.c): ps_1_x
 * texture-coordinate varyings, pixel precision, address-register moves,
 * relative addressing through a synthetic CTAB over the rest of the register
 * file and bounds-checked relative reads (the skinning palette); plus the ones
 * of the native conventions (Painkiller's and Heroes V's translator, wiki note
 * 23): SM1/2 colour saturation, PS1 constant range, pre-3.0 oFog naming, DEF
 * constant arrays, explicit layouts. The variants are constants of the program:
 *   vertex  S2R_IN_<n>        decode of input n (BGRA swizzle, integer formats)
 *           S2R_FOG_DEPTH     table fog: the fog coordinate is the clip-space z
 *   pixel   S2R_ALPHA_FUNC    alpha test (D3DCMP_*, 0 off)
 *           S2R_FOG           Direct3D 9 fog after the shader (0 off, 1 linear,
 *                             2 exp, 3 exp2) on the fog coordinate io_11_0.x
 * Vulkan reads them as specialization constants (#ifdef VULKAN), OpenGL ES
 * gets #define lines in front of the text. Every vertex shader declares and
 * initialises all standard varyings (colours, eight texture coordinates,
 * fog), so any pixel half links with it. */
#include "s2r_translate.h"
#include "mojoshader.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------- string helpers */
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }

static char *insert_before(const char *src, const char *anchor, const char *text)
{
    const char *at = strstr(src, anchor);
    size_t head = at ? (size_t)(at - src) : strlen(src), len = strlen(src), n = strlen(text);
    char *out = (char *)malloc(len + n + 1);
    memcpy(out, src, head);
    memcpy(out + head, text, n);
    strcpy(out + head + n, src + head);
    return out;
}

static char *replace_all(const char *src, const char *what, const char *with)
{
    size_t wl = strlen(what), nl = strlen(with), count = 0;
    for (const char *p = strstr(src, what); p; p = strstr(p + wl, what)) ++count;
    char *out = (char *)malloc(strlen(src) + count * (nl > wl ? nl - wl : 0) + 1), *w = out;
    for (const char *p = src;;) {
        const char *at = strstr(p, what);
        if (!at) { strcpy(w, p); break; }
        memcpy(w, p, (size_t)(at - p)); w += at - p;
        memcpy(w, with, nl); w += nl;
        p = at + wl;
    }
    return out;
}

typedef int (*line_fn)(const char *line, size_t n, char *out, void *ctx);
static char *map_lines(const char *src, line_fn fn, void *ctx)
{
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 8192), *w = out;
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        int written = fn(line, n, w, ctx);
        if (written >= 0) w += written;
        else { memcpy(w, line, n); w += n; }
        line += n;
    }
    *w = 0;
    return out;
}

/* Text inserted right after the opening brace of main(). */
static char *insert_after_main_brace(const char *src, const char *text)
{
    const char *mainp = strstr(src, "void main()");
    const char *brace = mainp ? strchr(mainp, '{') : NULL;
    if (!brace || !*text) return xstrdup(src);
    size_t at = (size_t)(brace + 1 - src), n = strlen(text);
    char *out = (char *)malloc(strlen(src) + n + 2);
    memcpy(out, src, at);
    out[at] = '\n';
    memcpy(out + at + 1, text, n);
    strcpy(out + at + 1 + n, src + at);
    return out;
}

/* ------------------------------------- fixes of the run-time translation */
/* ps_1_x reads texture coordinates through globals initialised from io_
 * varyings it never declares; GLSL ES wants constant global initialisers. */
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
        } else { memcpy(w, line, n); w += n; }
        line += n;
    }
    *w = 0;
    if (!dn && !mn) return body;
    char *with_decls = insert_before(body, "void main()", decls);
    free(body);
    if (!mn) return with_decls;
    char *out = insert_after_main_brace(with_decls, moved);
    free(with_decls);
    return out;
}

/* Depth comparisons need Direct3D's float precision, the texture reads too. */
static char *fix_pixel_precision(const char *src)
{
    const char *old = "precision mediump float;", *at = strstr(src, old);
    if (!at) return xstrdup(src);
    const char *precision = "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
                            "precision highp samplerCube;\nprecision highp sampler3D;";
    size_t head = (size_t)(at - src);
    char *out = (char *)malloc(strlen(src) + strlen(precision) + 1);
    memcpy(out, src, head);
    strcpy(out + head, precision);
    strcpy(out + head + strlen(precision), at + strlen(old));
    return out;
}

/* vs_1_x writes the address register with mov as a float-to-int assignment
 * GLSL rejects; D3D floors the value. */
static int address_line(const char *line, size_t n, char *w, void *ctx)
{
    (void)ctx;
    const char *p = line;
    while (*p == '\t' || *p == ' ') ++p;
    char mask[8];
    int prefix = 0;
    if (sscanf(p, "vs_a0.%7[xyzw] = %n", mask, &prefix) == 1 && prefix > 0 &&
        strncmp(p + prefix, "int(", 4) && strncmp(p + prefix, "ivec", 4)) {
        const char *expr = p + prefix, *semi = strchr(expr, ';');
        if (semi && semi < line + n) {
            size_t comps = strlen(mask);
            int k = sprintf(w, "%.*s", (int)(p - line), line);
            if (comps == 1) k += sprintf(w + k, "vs_a0.%s = int(floor(%.*s));\n", mask, (int)(semi - expr), expr);
            else k += sprintf(w + k, "vs_a0.%s = ivec%zu(floor(%.*s));\n", mask, comps, (int)(semi - expr), expr);
            return k;
        }
    }
    return -1;
}

/* SM1/2 vertex colours are saturated before interpolation, at every exit. */
static char *fix_vertex_color_range(const char *src)
{
    char tail[160] = "";
    if (strstr(src, "#define vs_oD0 ")) strcat(tail, "\tvs_oD0 = clamp(vs_oD0, 0.0, 1.0);\n");
    if (strstr(src, "#define vs_oD1 ")) strcat(tail, "\tvs_oD1 = clamp(vs_oD1, 0.0, 1.0);\n");
    const char *mainp = strstr(src, "void main()");
    const char *brace = mainp ? strchr(mainp, '{') : NULL;
    if (!*tail || !brace) return xstrdup(src);
    size_t exits = 1;
    for (const char *p = brace; (p = strstr(p, "return;")); p += 7) ++exits;
    char *out = (char *)malloc(strlen(src) + exits * strlen(tail) + 1), *w = out;
    size_t head = (size_t)(brace + 1 - src);
    memcpy(w, src, head); w += head;
    unsigned depth = 1;
    const char *p = brace + 1;
    for (; *p && depth; ++p) {
        if (*p == '{') ++depth;
        if (*p == '}' && !--depth) { strcpy(w, tail); w += strlen(tail); }
        if (!strncmp(p, "return;", 7)) { strcpy(w, tail); w += strlen(tail); }
        *w++ = *p;
    }
    strcpy(w, p);
    return out;
}

/* PS1 constants have range [-1,1], DEF literals included. */
static int constant_line(const char *line, size_t n, char *w, void *ctx)
{
    (void)ctx;
    const char *end = memchr(line, '\n', n);
    unsigned index;
    int prefix = 0, literal = 0;
    if (sscanf(line, "#define ps_c%u %n", &index, &prefix) != 1 || !prefix || !isspace((unsigned char)line[prefix - 1])) {
        prefix = 0;
        if (sscanf(line, "const vec4 ps_c%u = %n", &index, &prefix) == 1 && prefix) literal = 1;
    }
    const char *stop = literal ? strchr(line + prefix, ';') : end;
    if (!stop && !literal) stop = line + n;
    if (prefix && stop && stop <= line + n) {
        int k = sprintf(w, "%.*sclamp(%.*s, -1.0, 1.0)%s", prefix, line, (int)(stop - line - prefix), line + prefix, literal ? ";" : "");
        if (end) w[k++] = '\n';
        return k;
    }
    return -1;
}

/* MojoShader sizes a relatively addressed constant array from the CTAB entry
 * the base register belongs to, and cannot address constants relatively
 * without a CTAB at all. Direct3D 9 just indexes the register file, and the
 * game's skinning shaders name their palette rows as one register each while
 * reading c[base + 3 * bone]. Such bytecode gets a synthetic CTAB instead of
 * its own (D3DX puts that among the comments right after the version token):
 * one float4 array from register `lo` to the end of the register file. The
 * instructions are untouched. NULL when the CTAB sits anywhere else
 * (d3d9_shader.c with_synthetic_ctab, with the whole file: the native
 * backends read constants from a uniform block, not GL uniform vectors). */
#define FOURCC_CTAB 0x42415443u
static uint8_t *with_synthetic_ctab(const uint8_t *code, uint32_t size, int pixel, uint32_t lo, uint32_t *out_size)
{
    uint32_t regs = pixel ? 224 : 256;
    uint32_t keep = 4, skip = 4;                             /* the version token */
    while (skip + 8 <= size) {
        uint32_t token, id;
        memcpy(&token, code + skip, 4);
        if ((token & 0xFFFF) != 0xFFFE) break;
        uint32_t bytes = 4 + 4 * ((token >> 16) & 0x7FFF);
        memcpy(&id, code + skip + 4, 4);
        if (id != FOURCC_CTAB || skip + bytes > size) break;
        skip += bytes;
    }
    for (uint32_t p = skip; p + 8 <= size;) {                /* a later CTAB would be parsed too */
        uint32_t token, id;
        memcpy(&token, code + p, 4);
        if ((token & 0xFFFF) != 0xFFFE) break;
        memcpy(&id, code + p + 4, 4);
        if (id == FOURCC_CTAB) return NULL;
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
    uint32_t comment_dwords = 1 + 104 / 4;                  /* 'CTAB' + data */
    *out_size = size - (skip - keep) + 4 * (1 + comment_dwords);
    uint8_t *out = (uint8_t *)malloc(*out_size);
    if (!out) return NULL;
    memcpy(out, code, keep);
    uint32_t token = 0xFFFEu | (comment_dwords << 16), fourcc = FOURCC_CTAB;
    memcpy(out + 4, &token, 4);
    memcpy(out + 8, &fourcc, 4);
    memcpy(out + 12, blob, 104);
    memcpy(out + 12 + 104, code + skip, size - skip);
    return out;
}

/* The lowest register of a constant array the shader addresses relatively, -1 for none. */
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

/* A relatively addressed constant past the register file reads (0,0,0,0) on
 * Direct3D 9 hardware, and the skinning relies on it: an unused bone slot
 * holds index 255 with weight 0. A uniform block read out of bounds is
 * undefined (NaN on Adreno, and 0 * NaN tears vertices off), so every read
 * indexed by the address or loop register goes through s2r_rel, which bounds
 * checks it (d3d9_shader.c guard_relative). Runs on the final text: the array
 * is a member of the S2R_VS_CONST block there. */
static char *guard_relative(const char *src, int size)
{
    const char *mainp = strstr(src, "void main()");
    if (size <= 0 || !mainp || (!strstr(mainp, "vs_a0") && !strstr(mainp, "vs_aL"))) return xstrdup(src);
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 256), *w = out;
    size_t head = (size_t)(mainp - src);
    memcpy(w, src, head);
    w += head;
    w += sprintf(w, "vec4 s2r_rel(int i) { return (i >= 0 && i < %d) ? vs_uniforms_vec4[i] : vec4(0.0); }\n", size);
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
        if (relative) w += sprintf(w, "s2r_rel(%.*s)", (int)n, open);
        else { memcpy(w, at, (size_t)(q + 1 - at)); w += q + 1 - at; }
        p = q + 1;
    }
    strcpy(w, p);
    return out;
}

/* ---------------------------------------------------- layouts */
static int varying_location(unsigned usage, unsigned index)
{
    if (usage == 10 && index < 2) return (int)index;          /* COLOR0/1 */
    if (usage == 5 && index < 8) return 2 + (int)index;       /* TEXCOORD0-7 */
    if (usage == 11 && index == 0) return 10;                 /* FOG */
    return 11 + (int)((usage * 3 + index) % 5);               /* the rest (rare) */
}

typedef struct layout_ctx { const MOJOSHADER_parseData *pd; int pixel; } layout_ctx;

static int layout_line(const char *line, size_t n, char *w, void *opaque)
{
    layout_ctx *c = (layout_ctx *)opaque;
    const char *stage = c->pixel ? "ps" : "vs";
    char name[64];
    unsigned u, i, size;
    if (sscanf(line, "uniform vec4 %63[a-z_0-9][%u];", name, &size) == 2 && strstr(name, "_uniforms_vec4"))
        return sprintf(w, "/*s2r:f*/vec4 %s[%u];\n", name, size);
    if (sscanf(line, "uniform ivec4 %63[a-z_0-9][%u];", name, &size) == 2 && strstr(name, "_uniforms_ivec4"))
        return sprintf(w, "/*s2r:i*/ivec4 %s[%u];\n", name, size);
    if (sscanf(line, "uniform bool %63[a-z_0-9][%u];", name, &size) == 2 && strstr(name, "_uniforms_bool"))
        return sprintf(w, "/*s2r:b*/bool %s[%u];\n", name, size);
    if (!strncmp(line, "uniform float vpFlip;", 21)) return 0;
    if (!strncmp(line, "uniform vec2 vposFlip;", 22)) return sprintf(w, "#define vposFlip port_vpos.xy\n");
    char type[32];
    unsigned reg;
    if (sscanf(line, "uniform %31s %63[a-z_0-9];", type, name) == 2 && !strncmp(type, "sampler", 7)) {
        char want[8];
        snprintf(want, sizeof want, "%s_s", stage);
        if (!strncmp(name, want, 4) && sscanf(name + 4, "%u", &reg) == 1)
            return sprintf(w, "layout(binding = %u) uniform highp %s %s;\n", (c->pixel ? 4u : 20u) + reg, type, name);
    }
    if (!c->pixel && sscanf(line, "in vec4 %63[a-z_0-9];", name) == 1) {
        for (int a = 0; a < c->pd->attribute_count; ++a)
            if (c->pd->attributes[a].name && !strcmp(c->pd->attributes[a].name, name))
                return sprintf(w, "layout(location = %d) in vec4 %s;\n", a, name);
    }
    const char *q = line;
    while (*q == ' ' || *q == '\t') ++q;
    int is_out = !strncmp(q, "out highp vec4 io_", 18), is_in = !strncmp(q, "in highp vec4 io_", 17);
    int is_outf = !strncmp(q, "out highp float io_", 19);
    if ((is_out || is_in || is_outf) && sscanf(strstr(q, "io_"), "io_%u_%u", &u, &i) == 2) {
        int loc = varying_location(u, i);
        if (is_outf) return sprintf(w, "layout(location = %d) out highp vec4 io_%u_%u;\n", loc, u, i);
        return sprintf(w, "layout(location = %d) %.*s", loc, (int)(n - (size_t)(q - line)), q);
    }
    return -1;
}

#define S2R_PORT_BLOCK \
    "layout(std140, binding = 3) uniform S2R_PORT { vec4 port_flip; vec4 port_alpha; vec4 port_fog_color; " \
    "vec4 port_vpos; vec4 port_fog; };\n"

static char *gather_blocks(const char *src, int pixel)
{
    char members[4096] = "";
    size_t mn = 0;
    size_t len = strlen(src);
    char *body = (char *)malloc(len + 1), *w = body;
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        if (!strncmp(line, "/*s2r:", 6) && n > 9)
            mn += (size_t)snprintf(members + mn, sizeof members - mn, "\t%.*s", (int)(n - 9), line + 9);
        else { memcpy(w, line, n); w += n; }
        line += n;
    }
    *w = 0;
    char block[4800];
    int k = snprintf(block, sizeof block, S2R_PORT_BLOCK "#define vpFlip port_flip.x\n");
    if (mn) snprintf(block + k, sizeof block - (size_t)k, "layout(std140, binding = %d) uniform S2R_%s_CONST {\n%s};\n",
        pixel ? 1 : 0, pixel ? "PS" : "VS", members);
    char *out = insert_before(body, "void main()", block);
    free(body);
    return out;
}

/* D3D9's half-pixel offset after the flip, and the table-fog coordinate
 * (Direct3D's clip-space z, before the flip and the GL depth remap). */
static char *add_half_pixel(const char *src)
{
    const char *flip = "gl_Position.y = gl_Position.y * vpFlip;";
    return replace_all(src, flip, "if (S2R_FOG_DEPTH != 0) io_11_0 = vec4(gl_Position.z);\n"
                                  "\tgl_Position.y = gl_Position.y * vpFlip;\n\tgl_Position.xy += port_flip.yz * gl_Position.w;");
}

/* Relative addressing into DEF constants: MojoShader declares a uniform array
 * the GL glue is supposed to fill at link time; it becomes a constant array
 * with the DEF values (formerly it read zeros, and Vulkan rejected it). */
static int const_array_line(const char *line, size_t n, char *w, void *opaque)
{
    const MOJOSHADER_parseData *pd = (const MOJOSHADER_parseData *)opaque;
    char name[64];
    unsigned base, size, count;
    (void)n;
    if (sscanf(line, "uniform vec4 %63[a-z_0-9][%u];", name, &count) != 2 || !strstr(name, "_const_array_")) return -1;
    const char *tail = strstr(name, "_const_array_") + 13;
    if (sscanf(tail, "%u_%u", &base, &size) != 2 || size != count) return -1;
    int k = sprintf(w, "const vec4 %s[%u] = vec4[%u](", name, count, count);
    for (unsigned i = 0; i < count; ++i) {
        const float *f = NULL;
        for (int c = 0; c < pd->constant_count; ++c)
            if (pd->constants[c].type == MOJOSHADER_UNIFORM_FLOAT && pd->constants[c].index == (int)(base + i))
                f = pd->constants[c].value.f;
        static const float zero[4] = {0, 0, 0, 0};
        if (!f) f = zero;
        k += sprintf(w + k, "%svec4(", i ? ", " : "");
        for (int j = 0; j < 4; ++j) {
            char num[32];
            snprintf(num, sizeof num, "%.9g", (double)f[j]);
            if (!strpbrk(num, ".eni")) strcat(num, ".0");
            k += sprintf(w + k, "%s%s", j ? ", " : "", num);
        }
        k += sprintf(w + k, ")");
    }
    k += sprintf(w + k, ");\n");
    return k;
}

static char *patch_common(const char *src_in, const MOJOSHADER_parseData *pd, int pixel, int *shadow)
{
    char *a;
    char *src = map_lines(src_in, const_array_line, (void *)pd);
    if (!pixel) {
        char *ranged = pd->major_ver >= 1 && pd->major_ver < 3 ? fix_vertex_color_range(src) : xstrdup(src);
        if (pd->major_ver < 3) {
            /* MojoShader names a pre-3.0 oFog after its RASTOUT register (1): io_11_1.
             * Every other half uses FOG index 0, so the linker matches one name. */
            char *renamed = replace_all(ranged, "io_11_1", "io_11_0");
            free(ranged);
            ranged = renamed;
        }
        a = map_lines(ranged, address_line, NULL);
        free(ranged);
    } else {
        char *ranged = pd->major_ver == 1 ? map_lines(src, constant_line, NULL) : xstrdup(src);
        char *precise = fix_pixel_precision(ranged);
        free(ranged);
        char *varyings = fix_pixel_varyings(precise);
        free(precise);
        a = varyings;
        (void)shadow;
    }
    layout_ctx c = { pd, pixel };
    char *b = map_lines(a, layout_line, &c);
    free(a);
    char *blocks = gather_blocks(b, pixel);
    free(b);
    char *out;
    if (pixel) {
        /* VPOS in Direct3D pixels when the render scale stores the target larger or smaller
           (port_vpos.z: Direct3D pixels per stored pixel, 1 without a scale) */
        out = replace_all(blocks, "vec4(gl_FragCoord.x, (gl_FragCoord.y * vposFlip.x) + vposFlip.y,",
                          "vec4(gl_FragCoord.x * port_vpos.z, (gl_FragCoord.y * port_vpos.z * vposFlip.x) + vposFlip.y,");
    } else out = add_half_pixel(blocks);
    free(blocks);
    free(src);
    return out;
}

/* ------------------------------------------------ variant constants */
/* Vertex inputs: "layout(location = a) in vec4 NAME;" -> a raw input and a
 * decoded global; the standard varyings: declared and given D3D's defaults;
 * the clip-space depth remap only for OpenGL ES. */
static char *vertex_variants(const char *src, unsigned *input_mask)
{
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + 8192), *w = out;
    char prologue[8192] = "";
    size_t pn = 0;
    *input_mask = 0;
    int declared[16] = {0};
    for (const char *line = src; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        unsigned loc;
        char name[64], one[512];
        int used = 0;
        snprintf(one, sizeof one, "%.*s", (int)(n < sizeof one - 1 ? n : sizeof one - 1), line);
        if (sscanf(line, "layout(location = %u) in vec4 %63[a-z_0-9];", &loc, name) == 2 && loc < 16) {
            w += sprintf(w, "layout(location = %u) in vec4 s2r_in_%u;\nvec4 %s;\n", loc, loc, name);
            pn += (size_t)snprintf(prologue + pn, sizeof prologue - pn, "\t%s = s2r_decode(s2r_in_%u, S2R_IN_%u);\n", name, loc, loc);
            *input_mask |= 1u << loc;
            used = 1;
        } else if (sscanf(line, "layout(location = %u)", &loc) == 1 && strstr(one, ") out highp vec4 io_") && loc < 16) {
            declared[loc] = 1;
        } else if (strstr(one, "gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;")) {
            w += sprintf(w, "#ifndef VULKAN\n%.*s#endif\n", (int)n, line);
            used = 1;
        }
        if (!used) { memcpy(w, line, n); w += n; }
        line += n;
    }
    *w = 0;
    char decls[4096] = "";
    size_t dn = 0;
    dn += (size_t)snprintf(decls + dn, sizeof decls - dn, "#ifdef VULKAN\n");
    for (unsigned a = 0; a < 16; ++a)
        if (*input_mask & (1u << a))
            dn += (size_t)snprintf(decls + dn, sizeof decls - dn, "layout(constant_id = %u) const int S2R_IN_%u = 0;\n", a, a);
    dn += (size_t)snprintf(decls + dn, sizeof decls - dn, "layout(constant_id = 16) const int S2R_FOG_DEPTH = 0;\n#endif\n"
        "vec4 s2r_decode(vec4 v, int mode)\n{\n"
        "\tif (mode == 1) return v.zyxw;\n"
        "\tif (mode == 2) return floor(v * 255.0 + 0.5);\n"
        "\tif (mode == 3) return vec4(floor(v.xy * 32767.0 + 0.5), 0.0, 1.0);\n"
        "\tif (mode == 4) return floor(v * 32767.0 + 0.5);\n"
        "\tif (mode == 5) return vec4(floor(v.xyz * 1023.0 + 0.5), 1.0);\n"
        "\treturn v;\n}\n");
    static const char *const std_names[11] = { "io_10_0", "io_10_1", "io_5_0", "io_5_1", "io_5_2", "io_5_3",
        "io_5_4", "io_5_5", "io_5_6", "io_5_7", "io_11_0" };
    static const char *const std_defaults[11] = { "vec4(1.0)", "vec4(0.0)", "vec4(0.0, 0.0, 0.0, 1.0)",
        "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0, 0.0, 0.0, 1.0)",
        "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0, 0.0, 0.0, 1.0)", "vec4(0.0)" };
    char defaults[2048] = "";
    size_t fn = 0;
    for (int l = 0; l < 11; ++l) {
        if (!declared[l]) dn += (size_t)snprintf(decls + dn, sizeof decls - dn, "layout(location = %d) out highp vec4 %s;\n", l, std_names[l]);
        fn += (size_t)snprintf(defaults + fn, sizeof defaults - fn, "\t%s = %s;\n", std_names[l], std_defaults[l]);
    }
    char *with_decls = insert_before(out, "void main()", decls);
    free(out);
    char both[10240];
    snprintf(both, sizeof both, "%s%s", defaults, prologue);
    char *final = insert_after_main_brace(with_decls, both);
    free(with_decls);
    return final;
}

/* The pixel epilogue: Direct3D 9 fog after the shader (the formula on the fog
 * coordinate, d3d9_ff.c:d3d_fog_state's port_fog = end, scale, density), then
 * the alpha test on the clamped 8-bit alpha, at every exit of main. */
static char *pixel_variants(const char *src)
{
    const char *mainp = strstr(src, "void main()");
    if (!mainp || !strstr(src, "ps_oC0")) return xstrdup(src);
    char helper[2560];
    snprintf(helper, sizeof helper,
        "%s#ifdef VULKAN\nlayout(constant_id = 20) const int S2R_ALPHA_FUNC = 0;\nlayout(constant_id = 21) const int S2R_FOG = 0;\n#endif\n"
        "void s2r_epilogue()\n{\n"
        "\tif (S2R_FOG != 0) {\n"
        "\t\tfloat c = io_11_0.x;\n"
        "\t\tfloat f = S2R_FOG == 2 ? exp(-port_fog.z * c) : S2R_FOG == 3 ? exp(-(port_fog.z * c) * (port_fog.z * c)) :\n"
        "\t\t\t(port_fog.x - c) * port_fog.y;\n"
        "\t\tps_oC0.rgb = mix(port_fog_color.rgb, ps_oC0.rgb, clamp(f, 0.0, 1.0));\n"
        "\t}\n"
        "\tif (S2R_ALPHA_FUNC != 0) {\n"
        "\t\tfloat a = floor(clamp(ps_oC0.a, 0.0, 1.0) * 255.0 + 0.5), r = port_alpha.x;\n"
        "\t\tbool pass = S2R_ALPHA_FUNC == 2 ? a < r : S2R_ALPHA_FUNC == 3 ? a == r : S2R_ALPHA_FUNC == 4 ? a <= r :\n"
        "\t\t\tS2R_ALPHA_FUNC == 5 ? a > r : S2R_ALPHA_FUNC == 6 ? a != r : S2R_ALPHA_FUNC == 7 ? a >= r : false;\n"
        "\t\tif (!pass) discard;\n\t}\n}\n",
        strstr(src, "in highp vec4 io_11_0;") ? "" : "layout(location = 10) in highp vec4 io_11_0;\n");
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 2 + sizeof helper + 64), *w = out;
    size_t head = (size_t)(mainp - src);
    memcpy(w, src, head); w += head;
    strcpy(w, helper); w += strlen(helper);
    const char *p = mainp, *brace = strchr(p, '{');
    if (!brace) { strcpy(w, p); return out; }
    memcpy(w, p, (size_t)(brace - p + 1)); w += brace - p + 1;
    int depth = 1;
    p = brace + 1;
    while (*p && depth > 0) {
        if (*p == '{') ++depth;
        else if (*p == '}') { if (--depth == 0) break; }
        if (!strncmp(p, "return;", 7) && !isalnum((unsigned char)p[-1])) {
            w += sprintf(w, "{ s2r_epilogue(); return; }");
            p += 7;
            continue;
        }
        *w++ = *p++;
    }
    w += sprintf(w, "\ts2r_epilogue();\n");
    strcpy(w, p);
    return out;
}

static int array_size(const char *src, const char *name)
{
    const char *p = strstr(src, name);
    unsigned n = 0;
    if (p && sscanf(p + strlen(name), "[%u]", &n) == 1) return (int)n;
    return 0;
}

static const MOJOSHADER_parseData *parse(const uint8_t *code, uint32_t size, int pixel, int sm1, uint32_t types)
{
    MOJOSHADER_samplerMap map[8];
    unsigned count = 0;
    if (pixel && sm1) {
        for (unsigned i = 0; i < 8; ++i) {
            map[i].index = (int)i;
            map[i].type = (MOJOSHADER_samplerType)((types >> (2 * i)) & 3u);
        }
        count = 8;
    }
    return MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", code, size, NULL, 0, count ? map : NULL, count,
                            NULL, NULL, NULL);
}

static void fail(char *error, size_t cap, const char *what)
{
    if (error && cap) snprintf(error, cap, "%s", what);
}

int s2r_translate(int pixel, const uint8_t *code, uint32_t size, uint32_t sampler_types, s2r_translation *out,
    char *error, size_t error_cap)
{
    memset(out, 0, sizeof *out);
    if (size < 8) { fail(error, error_cap, "too short"); return 0; }
    uint32_t version;
    memcpy(&version, code, 4);
    int sm1 = ((version >> 8) & 255u) == 1;
    uint8_t *parse_code = NULL;
    uint32_t parse_size = 0;
    const MOJOSHADER_parseData *pd = parse(code, size, pixel, sm1, sampler_types);
    int lo = -1;
    if (pd && pd->error_count && strstr(pd->errors[0].error, "without a CTAB")) lo = 0;
    else if (pd && !pd->error_count) lo = relative_base(pd);
    if (lo >= 0) {
        parse_code = with_synthetic_ctab(code, size, pixel, (uint32_t)lo, &parse_size);
        const MOJOSHADER_parseData *synthetic = parse_code ? parse(parse_code, parse_size, pixel, sm1, sampler_types) : NULL;
        if (synthetic && !synthetic->error_count) {
            if (pd) MOJOSHADER_freeParseData(pd);
            pd = synthetic;
            out->synthetic_ctab = 1;
        } else {
            if (synthetic) MOJOSHADER_freeParseData(synthetic);
            free(parse_code);
            parse_code = NULL;
        }
    }
    if (!pd || pd->error_count) {
        fail(error, error_cap, pd && pd->error_count ? pd->errors[0].error : "no parse data");
        if (pd) MOJOSHADER_freeParseData(pd);
        free(parse_code);
        return 0;
    }
    out->pixel = pixel;
    out->major = pd->major_ver;
    out->minor = pd->minor_ver;
    char *glsl = patch_common(pd->output, pd, pixel, &out->shadow_pcf);
    /* Array sizes from MojoShader's own text, where the declaration comes
     * before the register #defines (the patched text moves it into a block). */
    const char *st = pixel ? "ps" : "vs";
    char name[48];
    snprintf(name, sizeof name, "uniform vec4 %s_uniforms_vec4", st); out->f_total = array_size(pd->output, name);
    snprintf(name, sizeof name, "uniform ivec4 %s_uniforms_ivec4", st); out->i_total = array_size(pd->output, name);
    snprintf(name, sizeof name, "uniform bool %s_uniforms_bool", st); out->b_total = array_size(pd->output, name);
    char *final = pixel ? pixel_variants(glsl) : vertex_variants(glsl, &out->input_mask);
    if (!pixel) {
        char *guarded = guard_relative(final, out->f_total);
        free(final);
        final = guarded;
    }
    /* The callers put their own #version (310 es / 450) and the variant lines first. */
    if (!strncmp(final, "#version", 8)) {
        const char *eol = strchr(final, '\n');
        memmove(final, eol ? eol + 1 : final + strlen(final), strlen(eol ? eol + 1 : "") + 1);
    }
    /* constant packing: dst, register, count per set */
    static const MOJOSHADER_uniformType types[3] = { MOJOSHADER_UNIFORM_FLOAT, MOJOSHADER_UNIFORM_INT, MOJOSHADER_UNIFORM_BOOL };
    for (int set = 0; set < 3; ++set) {
        int dst = 0;
        for (int k = 0; k < pd->uniform_count; ++k) {
            const MOJOSHADER_uniform *u = &pd->uniforms[k];
            if (u->constant || u->type != types[set] || out->map_count[set] >= 64) continue;
            int count = u->array_count ? u->array_count : 1;
            int *m = out->map[set][out->map_count[set]++];
            m[0] = dst; m[1] = u->index; m[2] = count;
            dst += count;
        }
    }
    /* texbem: stage -> destination register of its bump matrix */
    for (const char *p = strstr(pd->output, "_texbem ps_uniforms_vec4["); p && out->texbem_count < 8;
         p = strstr(p + 8, "_texbem ps_uniforms_vec4[")) {
        const char *def = p;
        while (def > pd->output && def[-1] != '\n') --def;
        unsigned stage, dst;
        if (sscanf(def, "#define ps_s%u_texbem ps_uniforms_vec4[%u]", &stage, &dst) == 2) {
            out->texbem[out->texbem_count][0] = (int)stage;
            out->texbem[out->texbem_count][1] = (int)dst;
            ++out->texbem_count;
        }
    }
    for (int a = 0; a < pd->attribute_count && a < 16; ++a) {
        out->attributes[a][0] = (int)pd->attributes[a].usage;
        out->attributes[a][1] = pd->attributes[a].index;
        out->attribute_count = a + 1;
    }
    for (int k = 0; k < pd->sampler_count && out->sampler_count < 16; ++k) {
        int t = pd->samplers[k].type == MOJOSHADER_SAMPLER_CUBE ? 1 : pd->samplers[k].type == MOJOSHADER_SAMPLER_VOLUME ? 2 : 0;
        out->samplers[out->sampler_count][0] = pd->samplers[k].index;
        out->samplers[out->sampler_count][1] = t;
        ++out->sampler_count;
    }
    if (pixel) {
        for (const char *p = strstr(final, "in highp vec4 io_"); p; p = strstr(p + 1, "in highp vec4 io_")) {
            unsigned u, i;
            if (sscanf(p + 17, "%u_%u", &u, &i) != 2) continue;
            if (u == 10 && i < 2) out->ps_colors |= 1u << i;
            else if (u == 5 && i < 8) out->ps_tex |= 1u << i;
        }
        out->ps_fog = strstr(pd->output, "io_11_") != NULL;
    }
    /* oFog after the io_11_1 -> io_11_0 rename of pre-3.0 shaders, or dcl_fog of vs_3_0 */
    out->writes_fog = !pixel && strstr(pd->output, "io_11_") != NULL;
    out->glsl = final;
    free(glsl);
    free(parse_code);
    MOJOSHADER_freeParseData(pd);
    return 1;
}

void s2r_translation_free(s2r_translation *t)
{
    free(t->glsl);
    memset(t, 0, sizeof *t);
}
