/* A draw of the native renderers: the render thread's copy of the device
 * state resolved into an s2r_draw packet for the backend (Painkiller's model,
 * wiki note 23, as Heroes V's h5r_draw.c), with the semantics of the Direct3D
 * 9 -> GLES layer the game was verified against (android/host/d3d9_gl.c
 * d3d_gl_draw, d3d9_shader.c, d3d9_ff.c): POSITIONT vertices skip the game's
 * vertex shader, the effective alpha test, ps_1_x sampler types taken from the
 * bound textures, the HUD tags the touch layer watches, instancing by stream
 * frequency. Buffers are already up to date: the front end records every
 * write with a copy of the bytes (d3d_buffer_flush) before the draw. */
#include "s2r_internal.h"
#include "s2r_spec.h"
#include "../host/hud.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>

int s2r_trace_frame;

uint32_t s2r_prim_vertices(uint32_t type, uint32_t prims)
{
    switch (type) {
    case 1: return prims;
    case 2: return prims * 2;
    case 3: return prims + 1;
    case 4: return prims * 3;
    case 5: case 6: return prims + 2;
    default: return 0;
    }
}

s2r_image *s2r_stage_image(s2r_device *d, uint32_t stage)
{
    if (stage >= 20) return NULL;
    d3d_texture *t = (d3d_texture *)d->dev->st.textures[stage];
    if (!t) return NULL;
    s2r_image *img = s2r_texture_image(t);
    if (img && t->mips_dirty) s2r_texture_generate_mips(t);
    return img;
}

/* The most detailed level a stage may sample: the larger of D3DSAMP_MAXMIPLEVEL
 * and the texture's SetLOD (d3d9_resource.c), as d3d9_gl.c's samplers take it
 * (Direct3D samples nothing above a managed texture's LOD). */
uint32_t s2r_stage_first_level(s2r_device *d, uint32_t stage, uint32_t levels)
{
    if (levels < 2 || stage >= 20) return 0;
    const d3d_texture *t = (const d3d_texture *)d->dev->st.textures[stage];
    uint32_t first = d->dev->st.ss[stage][9];
    if (t && t->gl_lod > first) first = t->gl_lod;
    return first < levels ? first : levels - 1;
}

int s2r_targets(s2r_device *d, s2r_target color[4], s2r_target *depth)
{
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        memset(&color[i], 0, sizeof color[i]);
        if (d->dev->rt[i]) s2r_surface_target(d->dev->rt[i], &color[i]);
        if (color[i].img) n = i + 1;
    }
    memset(depth, 0, sizeof *depth);
    if (d->dev->ds) s2r_surface_target(d->dev->ds, depth);
    return n;
}

/* ---------------------------------------------------------------- fog */
/* The Direct3D 9 -> GLES layer never applied Direct3D's fog, and that is the
 * picture the game was checked with; debug.s2.fog=1 turns Direct3D 9's fog
 * model (Heroes V's, d3d9_ff.c of that line) on for a comparison. */
static int fog_enabled(void)
{
    static int value = -1;
    if (value < 0) {
        char v[92] = "";
        __system_property_get("debug.s2.fog", v);
        value = v[0] == '1';
        if (value) S2R_LOG("debug.s2.fog=1: Direct3D 9 fog is applied");
    }
    return value;
}

static float rs_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static int pretransformed(const d3d_state *st)
{
    const d3d_decl *decl = (const d3d_decl *)st->decl;
    for (int i = 0; decl && i < decl->count; ++i)
        if (decl->elements[i].usage == 9 && decl->elements[i].usage_index == 0 && decl->elements[i].type < 17) return 1;
    return 0;
}

/* The pixel formula (S2R_FOG_*), the fixed-function vertex half's coordinate
 * (S2R_FOGC_*) or, for a game vertex shader, whether table fog takes its
 * clip-space z; params (end, scale, density, 0), colour. */
static int fog_state(s2r_device *d, int vs_major, int ps_major, uint32_t *coord, int *depth, float params[4], float color[4])
{
    const d3d_state *st = &d->dev->st;
    const uint32_t *rs = st->rs;
    *coord = S2R_FOGC_NONE;
    *depth = 0;
    if (!rs[28] || ps_major >= 3 || vs_major >= 3) return S2R_FOG_OFF;
    static int seen;
    if (!seen) { seen = 1; S2R_LOG("the game enables Direct3D fog (table %u, vertex %u)%s", rs[35], rs[140], fog_enabled() ? "" : ", not applied as the GLES layer"); }
    if (!fog_enabled()) return S2R_FOG_OFF;
    uint32_t table = rs[35], vertex = rs[140];
    int transformed = !vs_major && pretransformed(st);
    int formula, ffp = 1;
    if (table == 0) {
        if (vs_major || vertex == 0 || transformed) {
            formula = S2R_FOG_LINEAR;                       /* start 1, end 0: f = c */
            ffp = 0;
            *coord = vs_major ? S2R_FOGC_NONE : S2R_FOGC_SPECULAR;
        } else {
            formula = vertex == 1 ? S2R_FOG_EXP : vertex == 2 ? S2R_FOG_EXP2 : S2R_FOG_LINEAR;
            *coord = rs[48] ? S2R_FOGC_RANGE : S2R_FOGC_DEPTH;
        }
    } else {
        formula = table == 1 ? S2R_FOG_EXP : table == 2 ? S2R_FOG_EXP2 : S2R_FOG_LINEAR;
        const float *p = st->transforms[3];
        int ortho = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f && p[15] == 1.0f;
        if (vs_major) *depth = 1;
        else *coord = transformed ? S2R_FOGC_TRANSFORMED : ortho ? S2R_FOGC_ORTHO : S2R_FOGC_DEPTH;
    }
    float start = 1.0f, end = 0.0f;
    if (ffp) { start = rs_float(rs[36]); end = rs_float(rs[37]); }
    params[0] = ffp && start == end ? 0.0f : end;
    params[1] = ffp && start == end ? 0.0f : 1.0f / (end - start);
    params[2] = rs_float(rs[38]);
    params[3] = 0.0f;
    uint32_t c = rs[34];
    color[0] = ((c >> 16) & 255) / 255.0f; color[1] = ((c >> 8) & 255) / 255.0f; color[2] = (c & 255) / 255.0f;
    color[3] = 1.0f;
    return formula;
}

/* --------------------------------------------------------- shaders */
static uint32_t shader_major(const d3d_shader *s) { return s && s->size >= 4 ? (s->bytecode[1]) : 0; }

/* The entry of a game shader: the library's, else the device's translation. */
static const s2r_lib_shader *shader_entry(s2r_device *d, d3d_shader *s, uint32_t types)
{
    s2r_shader_info *info = s2r_shader_of(s);
    if (!info) return NULL;
    if (!types) {
        if (!info->lib && !info->failed) {
            info->lib = s2r_lib_shader_for(s->pixel, s->bytecode, s->size, 0, 1);
            if (!info->lib) info->failed = 1;
        }
        return info->lib;
    }
    for (int i = 0; i < info->variant_count; ++i) if (info->variant_types[i] == types) return info->variants[i];
    const s2r_lib_shader *e = s2r_lib_shader_for(1, s->bytecode, s->size, types, 1);
    if (!e) e = shader_entry(d, s, 0);
    if (info->variant_count < 8) {
        info->variants[info->variant_count] = e;
        info->variant_types[info->variant_count] = types;
        ++info->variant_count;
    }
    return e;
}

/* ps_1_x declares no sampler types: the bound textures decide (d3d9_shader.c legacy_sampler_types). */
static const s2r_lib_shader *pixel_entry(s2r_device *d, d3d_shader *ps)
{
    const s2r_lib_shader *base = shader_entry(d, ps, 0);
    if (shader_major(ps) != 1 || !base) return base;
    uint32_t types = 0, used = base->samplers;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (!(used & (1u << slot))) continue;
        d3d_texture *t = (d3d_texture *)d->dev->st.textures[slot];
        uint32_t type = t && t->rtype == D3DRTYPE_CUBETEXTURE ? 1u : t && t->rtype == D3DRTYPE_VOLUMETEXTURE ? 2u : 0u;
        types |= type << (slot * 2);
    }
    return types ? shader_entry(d, ps, types) : base;
}

/* Packed constants of a stage for this shader: float block then ints then
 * bools (std140: 16 bytes each). Returns the byte size written to `out`. */
uint32_t s2r_pack_constants(const s2r_lib_shader *x, const d3d_state *st, uint8_t *out, uint32_t cap)
{
    uint32_t total = 16u * (uint32_t)(x->f_total + x->i_total + x->b_total);
    if (!total || total > cap) return 0;
    memset(out, 0, total);
    const s2r_lib_map *map = x->maps;
    const float (*f)[4] = x->pixel ? (const float (*)[4])st->ps_f : (const float (*)[4])st->vs_f;
    uint32_t fmax = x->pixel ? 224 : 256;
    for (int k = 0; k < x->fmap_count; ++k, ++map)
        for (int r = 0; r < map->count; ++r) {
            uint32_t reg = (uint32_t)map->reg + (uint32_t)r, dst = (uint32_t)map->dst + (uint32_t)r;
            if (reg < fmax && dst < x->f_total) memcpy(out + 16 * dst, f[reg], 16);
        }
    for (int k = 0; k < x->texbem_count; ++k) {
        uint32_t stage = x->texbem_stage[k], dst = x->texbem_dst[k];
        if (stage >= 8 || dst + 1 >= x->f_total) continue;
        float *m = (float *)(out + 16 * dst);
        memcpy(&m[0], &st->tss[stage][7], 4); memcpy(&m[1], &st->tss[stage][8], 4);
        memcpy(&m[2], &st->tss[stage][9], 4); memcpy(&m[3], &st->tss[stage][10], 4);
        memcpy(&m[4], &st->tss[stage][22], 4); memcpy(&m[5], &st->tss[stage][23], 4);
    }
    uint8_t *ip = out + 16u * x->f_total;
    const int32_t (*iv)[4] = x->pixel ? st->ps_i : st->vs_i;
    for (int k = 0; k < x->imap_count; ++k, ++map)
        for (int r = 0; r < map->count; ++r) {
            uint32_t reg = (uint32_t)map->reg + (uint32_t)r, dst = (uint32_t)map->dst + (uint32_t)r;
            if (reg < 16 && dst < x->i_total) memcpy(ip + 16 * dst, iv[reg], 16);
        }
    uint8_t *bp = ip + 16u * x->i_total;
    const int32_t *bv = x->pixel ? st->ps_b : st->vs_b;
    for (int k = 0; k < x->bmap_count; ++k, ++map)
        for (int r = 0; r < map->count; ++r) {
            uint32_t reg = (uint32_t)map->reg + (uint32_t)r, dst = (uint32_t)map->dst + (uint32_t)r;
            if (reg < 16 && dst < x->b_total) { int32_t v = bv[reg] != 0; memcpy(bp + 16 * dst, &v, 4); }
        }
    return total;
}

/* How a vertex input of this element type reaches the shader: OpenGL ES
 * converts integer types itself and has no BGRA attribute; Vulkan reads the
 * integer types through UNORM/SNORM formats (USCALED/SSCALED are optional,
 * Adreno has none) and the shader scales them back. */
uint8_t s2r_decode_mode(uint8_t type, int vulkan)
{
    if (type == 4) return S2R_DECODE_BGRA;
    if (!vulkan) return S2R_DECODE_NONE;
    switch (type) {
    case 5: return S2R_DECODE_UBYTE4;
    case 6: return S2R_DECODE_SHORT2;
    case 7: return S2R_DECODE_SHORT4;
    case 13: return S2R_DECODE_UDEC3;
    default: return S2R_DECODE_NONE;
    }
}

const d3d_vertex_element *s2r_input_element(s2r_device *d, const s2r_lib_shader *vs, uint32_t loc)
{
    const d3d_decl *decl = (const d3d_decl *)d->dev->st.decl;
    if (!decl) return NULL;
    if (!vs) {
        for (int i = 0; i < decl->count; ++i)
            if (decl->elements[i].type < 17 &&
                s2r_ff_input_location(decl->elements[i].usage, decl->elements[i].usage_index) == (int)loc)
                return &decl->elements[i];
        return NULL;
    }
    if (loc >= vs->attr_count) return NULL;
    uint8_t u = vs->attr_usage[loc], ui = vs->attr_index[loc];
    for (int i = 0; i < decl->count; ++i)
        if (decl->elements[i].type < 17 && decl->elements[i].usage == u && decl->elements[i].usage_index == ui)
            return &decl->elements[i];
    /* POSITION and POSITIONT stand in for each other, as Direct3D links them. */
    if (u == 0 || u == 9)
        for (int i = 0; i < decl->count; ++i)
            if (decl->elements[i].type < 17 && (decl->elements[i].usage == 0 || decl->elements[i].usage == 9) &&
                decl->elements[i].usage_index == ui)
                return &decl->elements[i];
    return NULL;
}

uint64_t s2r_decode_modes(s2r_device *d, const s2r_lib_shader *vs, int vulkan, uint8_t modes[16])
{
    uint64_t key = 0;
    for (uint32_t loc = 0; loc < 16; ++loc) {
        const d3d_vertex_element *e = s2r_input_element(d, vs, loc);
        modes[loc] = e ? s2r_decode_mode(e->type, vulkan) : 0;
        key |= (uint64_t)modes[loc] << (3 * loc);
    }
    return key;
}

char *s2r_gles_program_text(const char *body, const char *defines)
{
    size_t n = strlen(body) + strlen(defines) + 32;
    char *out = (char *)malloc(n);
    snprintf(out, n, "#version 310 es\n%s%s", defines, body);
    return out;
}

void s2r_shader_defines(char *out, size_t cap, int pixel, const uint8_t modes[16], int alpha_func, int fog, int fog_depth)
{
    size_t n = 0;
    if (pixel) {
        snprintf(out, cap, "#define S2R_ALPHA_FUNC %d\n#define S2R_FOG %d\n", alpha_func, fog);
        return;
    }
    out[0] = 0;
    for (int loc = 0; loc < 16 && n < cap; ++loc)
        n += (size_t)snprintf(out + n, cap - n, "#define S2R_IN_%d %d\n", loc, modes ? modes[loc] : 0);
    if (n < cap) snprintf(out + n, cap - n, "#define S2R_FOG_DEPTH %d\n", fog_depth);
}

void s2r_port_block_fill(s2r_device *d, const s2r_draw *draw, float target_scale, uint32_t target_h, s2r_port_block *p)
{
    const d3d_state *st = &d->dev->st;
    memset(p, 0, sizeof *p);
    float vw = (float)(st->viewport[2] ? st->viewport[2] : 1) * target_scale;
    float vh = (float)(st->viewport[3] ? st->viewport[3] : 1) * target_scale;
    /* D3D9 pixel centres sit on integer coordinates: half a pixel right and down after the flip. */
    p->flip[0] = -1.0f; p->flip[1] = 1.0f / vw; p->flip[2] = 1.0f / vh;
    p->alpha[0] = draw->alpha_ref;
    memcpy(p->fog_color, draw->fog_color, 16);
    /* VPOS: Direct3D's pixel coordinates from the top-left of the target (rows are stored top-down),
       in Direct3D pixels of a target the render scale stores larger or smaller. */
    p->vpos[0] = 1.0f; p->vpos[1] = 0.0f;
    p->vpos[2] = target_scale > 0.0f ? 1.0f / target_scale : 1.0f;
    /* UI FSR 1: the interface's 2D reads through EASU (s2r_ui_filter_source) */
    p->vpos[3] = draw->ui && s2r_ui_fsr() ? 3.0f : -1.0f;
    (void)target_h;
    memcpy(p->fog, draw->fog_params, 16);
}

/* ------------------------------------------------------------- tracing */
/* debug.s2.trace=N (a new N each time): the draws of the next frame. */
static uint32_t trace_seen_frame = 0xFFFFFFFFu;
static char trace_last[92];

static void trace_poll(s2r_device *d)
{
    if (d->frame == trace_seen_frame) return;
    trace_seen_frame = d->frame;
    s2r_trace_frame = 0;
    if (d->frame % 30) return;
    char v[92] = "";
    __system_property_get("debug.s2.trace", v);
    if (v[0] && strcmp(v, trace_last)) {
        strcpy(trace_last, v);
        s2r_trace_frame = 1;
        S2R_LOG("trace: frame %u", d->frame);
    }
}

static void trace_draw(s2r_device *d, const s2r_draw *dr)
{
    const d3d_state *s = &d->dev->st;
    const d3d_decl *decl = (const d3d_decl *)s->decl;
    char text[300];
    int n = 0;
    for (int i = 0; decl && i < decl->count && n < 240; ++i)
        n += snprintf(text + n, sizeof text - (size_t)n, " %u:%u/%u/%u@%u", decl->elements[i].stream, decl->elements[i].usage,
            decl->elements[i].usage_index, decl->elements[i].type, decl->elements[i].offset);
    s2r_target c[4], z;
    s2r_targets(d, c, &z);
    S2R_LOG("draw prim %u count %u idx %d base %d inst %u | vs %s ps %s fvf %X decl[%s ] | rt %ux%u fmt %u depth %s | "
        "vp %u,%u %ux%u | op0 %u/%u/%u a %u/%u/%u op1 %u | blend %u %u/%u/%u z %u/%u/%u cull %u | fog %d atest %d",
        dr->primitive, dr->count, dr->indexed, dr->base_vertex, dr->instances, dr->vs ? dr->vs->name : "ff",
        dr->ps ? dr->ps->name : "ff", s->fvf, text, c[0].img ? c[0].img->width : 0, c[0].img ? c[0].img->height : 0,
        c[0].img ? c[0].img->d3dfmt : 0, z.img ? "yes" : "no", s->viewport[0], s->viewport[1], s->viewport[2],
        s->viewport[3], s->tss[0][1], s->tss[0][2], s->tss[0][3], s->tss[0][4], s->tss[0][5], s->tss[0][6], s->tss[1][1],
        s->rs[27], s->rs[171], s->rs[19], s->rs[20], s->rs[7], s->rs[14], s->rs[23], s->rs[22], dr->fog, dr->alpha_func);
    for (uint32_t i = 0; i < 8; ++i) {
        d3d_texture *t = (d3d_texture *)s->textures[i];
        s2r_image *img = t ? s2r_texture_image(t) : NULL;
        if (t) S2R_LOG("  sampler %u: %ux%u fmt %u (%s) levels %u lod %u kind %d rt %d", i, t->width, t->height, t->format,
            img ? s2r_gpu_name(img->gpu) : "no storage", t->levels, t->gl_lod, img ? img->kind : -1, img ? img->render_target : 0);
    }
}

/* ---------------------------------------------------------------- draw */
static int instancing_logged;

void s2r_draw_primitive(s2r_device *d, uint32_t type, uint32_t start, uint32_t vertex_count, uint32_t prims, int indexed,
    int32_t base_vertex, uint32_t min_index, const void *up_vertices, uint32_t up_stride, const void *up_indices,
    int up_index32)
{
    const d3d_state *st = &d->dev->st;
    if (!prims || !st->decl) return;
    trace_poll(d);
    s2r_draw draw;
    memset(&draw, 0, sizeof draw);
    draw.primitive = type;
    draw.indexed = indexed;
    draw.start = start;
    draw.count = vertex_count;
    draw.base_vertex = base_vertex;
    draw.min_index = min_index;
    draw.num_vertices = 0;
    draw.ui = d->dev->ui_draw;
    if (indexed && !up_indices && !st->indices) return;
    if (st->stream_freq[0] & 0x40000000u) {
        draw.instances = st->stream_freq[0] & 0x3FFFFFFFu;
        if (!instancing_logged) { instancing_logged = 1; S2R_LOG("instanced draws (%u instances)", draw.instances); }
    }
    d3d_shader *vs = (d3d_shader *)st->vs, *ps = (d3d_shader *)st->ps;
    /* POSITIONT vertices are already transformed: Direct3D 9 runs no vertex
     * shader for them, only the fixed-function pass-through (d3d_gl_draw). */
    if (vs && pretransformed(st)) vs = NULL;
    draw.vs = vs ? shader_entry(d, vs, 0) : NULL;
    draw.ps = ps ? pixel_entry(d, ps) : NULL;
    if ((vs && !draw.vs) || (ps && !draw.ps)) return;     /* a shader without a program: logged at translation */
    const uint32_t *rs = st->rs;
    uint32_t func = rs[25], ref = rs[24] & 255u;
    /* Clamped 8-bit alpha makes GEQUAL 0 and LEQUAL 255 pass for every output. */
    if (rs[15] && func != 8 && !(func == 7 && ref == 0) && !(func == 4 && ref == 255) && func >= 1 && func <= 7) {
        draw.alpha_func = (int)func;
        draw.alpha_ref = (float)ref;
    }
    uint32_t fog_coord;
    draw.fog = fog_state(d, vs ? (int)shader_major(vs) : 0, ps ? (int)shader_major(ps) : 0, &fog_coord, &draw.fog_depth,
                         draw.fog_params, draw.fog_color);
    if (!draw.vs || !draw.ps) {
        s2r_ff_key_build(d, &draw, &draw.ff, !draw.vs, !draw.ps, fog_coord);
        if ((!draw.vs && !s2r_ff_valid(&draw.ff, 0)) || (!draw.ps && !s2r_ff_valid(&draw.ff, 1))) return;
    }
    if (up_vertices) {
        draw.up_vertices = up_vertices;
        draw.up_stride = up_stride;
        uint32_t vertices = vertex_count;
        if (indexed && up_indices) {
            uint32_t max = 0;
            for (uint32_t i = 0; i < vertex_count; ++i) {
                uint32_t v = up_index32 ? ((const uint32_t *)up_indices)[i] : ((const uint16_t *)up_indices)[i];
                if (v > max) max = v;
            }
            vertices = max + 1;
            draw.up_indices = up_indices;
            draw.up_index32 = up_index32;
            draw.up_index_bytes = vertex_count * (up_index32 ? 4u : 2u);
        }
        draw.up_vertex_bytes = vertices * up_stride;
        draw.num_vertices = vertices;
        d->upload_bytes += draw.up_vertex_bytes + draw.up_index_bytes;
        d->upload_stream += draw.up_vertex_bytes + draw.up_index_bytes;
        ++d->frame_streamed;
    }
    /* The touch layer reacts to HUD images on screen (hud.h), as d3d_gl_draw reports them. */
    uint32_t hud = 0, used = draw.ps ? draw.ps->samplers : s2r_ff_sampler_mask(&draw.ff);
    int picture = 0;
    for (int s = 0; s < 16; ++s) {
        d3d_texture *t = (d3d_texture *)st->textures[s];
        if (!t || !(used & (1u << s))) continue;
        hud |= t->hud_tag;
        if (t->width >= 128 && t->height >= 128) picture = 1;
    }
    void_hud_draw(hud | (picture && d->dev->rt[0] == d->dev->backbuffer ? 0x80000000u : 0));
    if (s2r_trace_frame) trace_draw(d, &draw);
    d->be->draw(d, &draw);
    ++d->frame_draws;
    d->frame_prims += prims;
}
