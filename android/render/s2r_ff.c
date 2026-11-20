/* Direct3D 9 fixed function for the native renderers: the device state as the
 * constants of two port-written shaders (shaders/ff.vert, shaders/ff.frag;
 * Painkiller's, wiki note 23). No GLSL is generated on the device: the build
 * compiles both once (SPIR-V for Vulkan, whose specialization constants take
 * these values; GLSL ES text that OpenGL ES compiles with them as #define
 * lines), and the driver folds the branches. The halves pair with the game's
 * shaders through the standard varyings every vertex half writes.
 *
 * The fog model is the Direct3D 9 -> GLES layer's (d3d9_ff.c d3d_fog_state):
 * the vertex half writes the fog coordinate (FF_FOG_COORD), the pixel half
 * applies the formula (FF_FOG) with port_fog = (end, scale, density). */
#include "s2r_internal.h"
#include "s2r_spec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ff_vkey {
    uint8_t valid, post, normal, lighting;
    uint8_t color[2], specular, localviewer;
    uint8_t tex_dims[8];
    uint8_t mat_src[4];           /* diffuse, specular, ambient, emissive: 0 material, 1 colour 0, 2 colour 1 */
    uint8_t light_type[8];        /* 0 off, 1 point, 2 spot, 3 directional */
    uint8_t fog_coord, normalize, out_tex_mask, pad;
    uint8_t tc_set[8], tc_mode[8], tc_count[8];
} ff_vkey;

typedef struct ff_pstage {
    uint8_t colorop, carg1, carg2, alphaop, aarg1, aarg2, sampler, project, has_tex, result_temp;
} ff_pstage;

typedef struct ff_pkey {
    uint8_t valid, stages, specular, fog, alpha_func, pad[3];
    ff_pstage st[8];
} ff_pkey;

_Static_assert(sizeof(ff_vkey) <= sizeof(((s2r_ff_key *)0)->vs), "vertex key size");
_Static_assert(sizeof(ff_pkey) <= sizeof(((s2r_ff_key *)0)->ps), "pixel key size");

static uint32_t hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h ? h : 1;
}

int s2r_ff_input_location(uint8_t usage, uint8_t usage_index)
{
    switch (usage) {
    case 0: case 9: return usage_index == 0 ? 0 : -1;       /* POSITION, POSITIONT */
    case 3: return usage_index == 0 ? 3 : -1;               /* NORMAL */
    case 10: return usage_index < 2 ? 4 + usage_index : -1; /* COLOR */
    case 5: return usage_index < 8 ? 6 + usage_index : -1;  /* TEXCOORD */
    default: return -1;
    }
}

static uint8_t decl_dims(uint8_t type)
{
    switch (type) {
    case 0: return 1;
    case 1: case 6: case 9: case 11: case 15: return 2;
    case 2: return 3;
    default: return 4;
    }
}

static uint8_t material_source(const s2r_device *d, uint32_t rs, const ff_vkey *k)
{
    if (!d->dev->st.rs[141]) return 0;                 /* COLORVERTEX off */
    uint32_t src = d->dev->st.rs[rs];
    if (src == 1 && k->color[0]) return 1;
    if (src == 2 && k->color[1]) return 2;
    return 0;
}

static uint32_t light_type(const s2r_device *d, int i)
{
    uint32_t t;
    memcpy(&t, &d->dev->st.lights[i][0], 4);
    return t >= 1 && t <= 3 ? t : 0;
}

/* `fog_coord`: S2R_FOGC_* the pixel half needs; `out_tex`: the texture coordinates it reads. */
static int vertex_key(const s2r_device *d, ff_vkey *k, uint32_t fog_coord, uint32_t out_tex)
{
    memset(k, 0, sizeof *k);
    const d3d_decl *decl = (const d3d_decl *)d->dev->st.decl;
    int pos = 0;
    for (int i = 0; decl && i < decl->count; ++i) {
        const d3d_vertex_element *e = &decl->elements[i];
        if (e->type > 16) continue;
        switch (e->usage) {
        case 0: if (!e->usage_index) pos = 1; break;
        case 9: if (!e->usage_index) { pos = 1; k->post = 1; } break;
        case 3: if (!e->usage_index) k->normal = 1; break;
        case 10: if (e->usage_index < 2) k->color[e->usage_index] = 1; break;
        case 5: if (e->usage_index < 8) k->tex_dims[e->usage_index] = decl_dims(e->type); break;
        default: break;
        }
    }
    if (!pos) return 0;
    if (k->post) k->normal = 0;
    const uint32_t *rs = d->dev->st.rs;
    k->lighting = rs[137] && !k->post;
    if (k->lighting) {
        k->specular = rs[29] != 0;
        k->localviewer = rs[142] != 0;
        k->normalize = rs[143] != 0;
        k->mat_src[0] = material_source(d, 145, k);
        k->mat_src[1] = material_source(d, 146, k);
        k->mat_src[2] = material_source(d, 147, k);
        k->mat_src[3] = material_source(d, 148, k);
        for (int i = 0; i < 8; ++i)
            if (d->dev->st.light_enable[i]) k->light_type[i] = (uint8_t)light_type(d, i);
    }
    k->fog_coord = (uint8_t)fog_coord;
    k->out_tex_mask = (uint8_t)out_tex;
    for (uint32_t n = 0; n < 8; ++n) {
        if (!(out_tex & (1u << n))) continue;
        uint32_t tci = d->dev->st.tss[n][11], flags = d->dev->st.tss[n][24];
        k->tc_set[n] = (uint8_t)(tci & 7u);
        uint32_t mode = k->post ? 0 : (tci >> 16) & 15u;
        if (mode > 4) mode = 0;
        if (mode && mode != 2 && !k->normal) mode = 0;
        k->tc_mode[n] = (uint8_t)mode;
        uint32_t count = flags & 0xFFu;
        k->tc_count[n] = (uint8_t)((k->post || count < 2 || count > 4) ? 0 : count);
    }
    k->valid = 1;
    return 1;
}

static void pixel_key(const s2r_device *d, ff_pkey *k, int vertex_shader, int alpha_func, int fog)
{
    memset(k, 0, sizeof *k);
    k->specular = d->dev->st.rs[29] != 0;
    k->fog = (uint8_t)fog;
    k->alpha_func = (uint8_t)alpha_func;
    for (uint32_t s = 0; s < 8; ++s) {
        const uint32_t *t = d->dev->st.tss[s];
        if (t[1] == 1) break;
        ff_pstage *st = &k->st[s];
        st->colorop = (uint8_t)t[1]; st->carg1 = (uint8_t)t[2]; st->carg2 = (uint8_t)t[3];
        st->alphaop = (uint8_t)t[4]; st->aarg1 = (uint8_t)t[5]; st->aarg2 = (uint8_t)t[6];
        st->result_temp = t[28] == 5;
        s2r_image *img = s2r_stage_image((s2r_device *)d, s);
        st->has_tex = img != NULL;
        st->sampler = (uint8_t)(!img ? 0 : img->kind == S2R_TEX_CUBE ? 1 : img->kind == S2R_TEX_3D ? 2 : 0);
        uint32_t flags = t[24], count = flags & 0xFFu;
        if ((flags & 0x100u) && count >= 2 && count <= 4) st->project = (uint8_t)count;
        else if ((flags & 0x100u) && vertex_shader) st->project = 4;
        k->stages = (uint8_t)(s + 1);
    }
    k->valid = 1;
}

void s2r_ff_key_build(s2r_device *d, const s2r_draw *draw, s2r_ff_key *key, int need_vs, int need_ps, uint32_t fog_coord)
{
    memset(key, 0, sizeof *key);
    ff_vkey *vk = (ff_vkey *)key->vs;
    ff_pkey *pk = (ff_pkey *)key->ps;
    if (need_vs && need_ps) {
        /* Both halves fixed: the pixel half decides which coordinates it reads. */
        ff_pkey probe;
        pixel_key(d, &probe, 0, draw->alpha_func, draw->fog);
        uint32_t tex = probe.stages ? (1u << probe.stages) - 1u : 0;
        if (!vertex_key(d, vk, draw->fog ? fog_coord : S2R_FOGC_NONE, tex)) { vk->valid = 0; return; }
        *pk = probe;
    } else if (need_vs) {
        const s2r_lib_shader *ps = draw->ps;
        if (!vertex_key(d, vk, draw->fog ? fog_coord : S2R_FOGC_NONE, ps ? ps->ps_tex : 0)) { vk->valid = 0; return; }
    } else if (need_ps) {
        pixel_key(d, pk, 1, draw->alpha_func, draw->fog);
    }
    if (need_vs) key->vs_hash = hash_bytes(key->vs, sizeof(ff_vkey));
    if (need_ps) key->ps_hash = hash_bytes(key->ps, sizeof(ff_pkey));
}

/* The sampler stages the fixed-function pixel half reads. */
uint32_t s2r_ff_sampler_mask(const s2r_ff_key *key)
{
    const ff_pkey *k = (const ff_pkey *)key->ps;
    uint32_t mask = 0;
    for (int s = 0; s < k->stages && s < 8; ++s) if (k->st[s].has_tex) mask |= 1u << s;
    return mask;
}

int s2r_ff_valid(const s2r_ff_key *key, int pixel)
{
    return pixel ? ((const ff_pkey *)key->ps)->valid : ((const ff_vkey *)key->vs)->valid;
}

/* ------------------------------------------------------------ constants */
typedef struct spec_name { uint16_t id; const char *name; } spec_name;
#define SPEC_ENTRY(id, name) { id, #name },
static const spec_name vs_names[] = { S2R_SPEC_FF_VS(SPEC_ENTRY) };
static const spec_name ps_names[] = { S2R_SPEC_FF_PS(SPEC_ENTRY) };
#undef SPEC_ENTRY

static void vs_values(const ff_vkey *k, int32_t v[128])
{
    memset(v, 0, 128 * sizeof *v);
    v[32] = k->post; v[33] = k->normal; v[34] = k->color[0]; v[35] = k->color[1];
    for (int n = 0; n < 8; ++n) v[36 + n] = k->tex_dims[n];
    v[44] = k->lighting; v[45] = k->specular; v[46] = k->localviewer;
    for (int n = 0; n < 4; ++n) v[47 + n] = k->mat_src[n];
    for (int n = 0; n < 8; ++n) v[51 + n] = k->light_type[n];
    v[59] = k->fog_coord; v[60] = k->normalize; v[61] = k->out_tex_mask;
    for (int n = 0; n < 8; ++n) { v[62 + n] = k->tc_set[n]; v[70 + n] = k->tc_mode[n]; v[78 + n] = k->tc_count[n]; }
}

static void ps_values(const ff_pkey *k, int32_t v[128])
{
    memset(v, 0, 128 * sizeof *v);
    v[32] = k->stages; v[33] = k->specular; v[34] = k->fog; v[35] = k->alpha_func;
    for (int s = 0; s < 8 && s < k->stages; ++s) {
        const ff_pstage *st = &k->st[s];
        int32_t *p = &v[40 + 10 * s];
        p[0] = st->colorop; p[1] = st->carg1; p[2] = st->carg2; p[3] = st->alphaop; p[4] = st->aarg1;
        p[5] = st->aarg2; p[6] = st->sampler; p[7] = st->project; p[8] = st->has_tex; p[9] = st->result_temp;
    }
}

int s2r_ff_spec(const s2r_ff_key *key, int pixel, uint32_t *ids, int32_t *values, int cap)
{
    int32_t v[128];
    const spec_name *names = pixel ? ps_names : vs_names;
    int count = pixel ? (int)(sizeof ps_names / sizeof ps_names[0]) : (int)(sizeof vs_names / sizeof vs_names[0]);
    if (pixel) ps_values((const ff_pkey *)key->ps, v); else vs_values((const ff_vkey *)key->vs, v);
    int n = 0;
    for (int i = 0; i < count && n < cap; ++i, ++n) { ids[n] = names[i].id; values[n] = v[names[i].id]; }
    return n;
}

void s2r_ff_defines(const s2r_ff_key *key, int pixel, char *out, size_t cap)
{
    int32_t v[128];
    const spec_name *names = pixel ? ps_names : vs_names;
    int count = pixel ? (int)(sizeof ps_names / sizeof ps_names[0]) : (int)(sizeof vs_names / sizeof vs_names[0]);
    if (pixel) ps_values((const ff_pkey *)key->ps, v); else vs_values((const ff_vkey *)key->vs, v);
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < count && n < cap; ++i)
        n += (size_t)snprintf(out + n, cap - n, "#define %s %d\n", names[i].name, (int)v[names[i].id]);
}

/* ------------------------------------------------------------ uniforms */
static void mul44(const float *a, const float *b, float *r)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

static void argb(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 255) / 255.0f;
    out[1] = (float)((c >> 8) & 255) / 255.0f;
    out[2] = (float)(c & 255) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}

/* Matrices are uploaded as Direct3D stores them (row-major, row vectors); the
 * GLSL side multiplies M * v with a column-major mat4, i.e. by the transpose,
 * which is the same product for row vectors. */
void s2r_ff_uniforms_fill(s2r_device *d, const s2r_draw *draw, s2r_ff_uniforms *u)
{
    (void)draw;
    const d3d_state *s = &d->dev->st;
    const float *world = s->transforms[256], *view = s->transforms[2], *proj = s->transforms[3];
    mul44(world, view, u->world_view);
    memcpy(u->proj, proj, 64);
    /* Inverse transpose of the world-view 3x3, as GLSL columns. */
    const float *m = u->world_view;
    float a = m[0], b = m[1], c = m[2], dd = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float det = a * (e * i - f * h) - b * (dd * i - f * g) + c * (dd * h - e * g);
    float inv = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    float r[9] = {
        (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
        (f * g - dd * i) * inv, (a * i - c * g) * inv, (c * dd - a * f) * inv,
        (dd * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * dd) * inv,
    };
    for (int col = 0; col < 3; ++col) {
        u->normal[col * 4 + 0] = r[0 * 3 + col];
        u->normal[col * 4 + 1] = r[1 * 3 + col];
        u->normal[col * 4 + 2] = r[2 * 3 + col];
        u->normal[col * 4 + 3] = 0;
    }
    memcpy(u->view, view, 64);
    for (int k = 0; k < 8; ++k) memcpy(u->texm[k], s->transforms[16 + k], 64);
    u->viewport[0] = (float)s->viewport[0];
    u->viewport[1] = (float)s->viewport[1];
    u->viewport[2] = s->viewport[2] ? (float)s->viewport[2] : 1.0f;
    u->viewport[3] = s->viewport[3] ? (float)s->viewport[3] : 1.0f;
    /* D3DMATERIAL9: diffuse, ambient, specular, emissive, power */
    memcpy(u->material_diffuse, &s->material[0], 16);
    memcpy(u->material_ambient, &s->material[4], 16);
    memcpy(u->material_specular, &s->material[8], 16);
    memcpy(u->material_emissive, &s->material[12], 16);
    u->material_power[0] = s->material[16]; u->material_power[1] = u->material_power[2] = u->material_power[3] = 0;
    argb(s->rs[139], u->global_ambient);
    for (int l = 0; l < 8; ++l) {
        /* D3DLIGHT9: type, diffuse[1..4], specular[5..8], ambient[9..12], position[13..15],
           direction[16..18], range, falloff, att0, att1, att2, theta, phi */
        const float *lt = s->lights[l];
        memcpy(u->light_diffuse[l], &lt[1], 16);
        memcpy(u->light_specular[l], &lt[5], 16);
        memcpy(u->light_ambient[l], &lt[9], 16);
        float p[4] = {lt[13], lt[14], lt[15], 1.0f}, q[4] = {0};
        float dir[4] = {lt[16], lt[17], lt[18], 0.0f}, v[4] = {0};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) { q[col] += p[row] * view[row * 4 + col]; v[col] += dir[row] * view[row * 4 + col]; }
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; ++k) { u->light_position[l][k] = q[k]; u->light_direction[l][k] = len > 0 ? v[k] / len : 0; }
        u->light_position[l][3] = 1; u->light_direction[l][3] = 0;
        u->light_atten[l][0] = lt[21]; u->light_atten[l][1] = lt[22]; u->light_atten[l][2] = lt[23];
        u->light_atten[l][3] = lt[19] > 0 ? lt[19] : 1e30f;
        u->light_spot[l][0] = cosf(lt[24] * 0.5f); u->light_spot[l][1] = cosf(lt[25] * 0.5f);
        u->light_spot[l][2] = lt[20]; u->light_spot[l][3] = (float)light_type(d, l);
    }
    memcpy(u->fog, draw->fog_params, 16);
    memcpy(u->fog_color, draw->fog_color, 16);
    argb(s->rs[60], u->tfactor);
    for (int k = 0; k < 8; ++k) {
        memcpy(&u->bumpenv[k][0], &s->tss[k][7], 4); memcpy(&u->bumpenv[k][1], &s->tss[k][8], 4);
        memcpy(&u->bumpenv[k][2], &s->tss[k][9], 4); memcpy(&u->bumpenv[k][3], &s->tss[k][10], 4);
    }
    u->alpha_ref[0] = (float)(s->rs[24] & 255u); u->alpha_ref[1] = u->alpha_ref[2] = u->alpha_ref[3] = 0;
}
