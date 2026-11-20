// Direct3D 9 fixed-function vertex processing for the Heroes V native renderer.
// One shader for every fixed-function state: the state arrives as constants
// (s2r_spec.h, FF_*; Vulkan specialization constants, #define lines on OpenGL
// ES), so the driver folds the branches away. The semantics are the ones of
// Direct3D 9 path (android/host/d3d9_ff.c): point and spot lights, material
// colour sources, the fog coordinate of d3d_fog_state (FF_FOG_COORD; the pixel
// half applies the formula), texture coordinate generation and transforms. It writes every standard varying,
// as the game's vertex shaders do, so any pixel half links with it.
precision highp float;
precision highp int;

layout(std140, binding = 2) uniform S2R_FF {
    mat4 ff_world_view; mat4 ff_proj; vec4 ff_normal[3]; mat4 ff_view; mat4 ff_texm[8];
    vec4 ff_viewport; vec4 ff_mat_diffuse; vec4 ff_mat_ambient; vec4 ff_mat_specular; vec4 ff_mat_emissive;
    vec4 ff_mat_power; vec4 ff_global_ambient;
    vec4 ff_light_diffuse[8]; vec4 ff_light_specular[8]; vec4 ff_light_ambient[8];
    vec4 ff_light_position[8]; vec4 ff_light_direction[8]; vec4 ff_light_atten[8]; vec4 ff_light_spot[8];
    vec4 ff_fog; vec4 ff_fog_color; vec4 ff_tfactor; vec4 ff_bumpenv[8]; vec4 ff_alpha_ref;
};
layout(std140, binding = 3) uniform S2R_PORT { vec4 port_flip; vec4 port_alpha; vec4 port_fog_color; vec4 port_vpos; vec4 port_fog; };

#pragma s2r_spec in
#pragma s2r_spec ff_vs

layout(location = 0) in vec4 ff_in_pos;
layout(location = 3) in vec4 ff_in_nrm;
layout(location = 4) in vec4 ff_in_c0;
layout(location = 5) in vec4 ff_in_c1;
layout(location = 6) in vec4 ff_in_t0;
layout(location = 7) in vec4 ff_in_t1;
layout(location = 8) in vec4 ff_in_t2;
layout(location = 9) in vec4 ff_in_t3;
layout(location = 10) in vec4 ff_in_t4;
layout(location = 11) in vec4 ff_in_t5;
layout(location = 12) in vec4 ff_in_t6;
layout(location = 13) in vec4 ff_in_t7;

layout(location = 0) out highp vec4 io_10_0;
layout(location = 1) out highp vec4 io_10_1;
layout(location = 2) out highp vec4 io_5_0;
layout(location = 3) out highp vec4 io_5_1;
layout(location = 4) out highp vec4 io_5_2;
layout(location = 5) out highp vec4 io_5_3;
layout(location = 6) out highp vec4 io_5_4;
layout(location = 7) out highp vec4 io_5_5;
layout(location = 8) out highp vec4 io_5_6;
layout(location = 9) out highp vec4 io_5_7;
layout(location = 10) out highp vec4 io_11_0;

vec4 s2r_decode(vec4 v, int mode)
{
    if (mode == 1) return v.zyxw;
    if (mode == 2) return floor(v * 255.0 + 0.5);
    if (mode == 3) return vec4(floor(v.xy * 32767.0 + 0.5), 0.0, 1.0);
    if (mode == 4) return floor(v * 32767.0 + 0.5);
    if (mode == 5) return vec4(floor(v.xyz * 1023.0 + 0.5), 1.0);
    return v;
}

vec4 ff_sphere(vec3 r)
{
    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
    return vec4(r.x / m + 0.5, r.y / m + 0.5, 0.0, 1.0);
}

void ff_light(int i, int type, vec3 nrm, vec3 V, vec3 ec, inout vec3 amb, inout vec3 dif, inout vec3 spe)
{
    vec3 L;
    float att;
    if (type == 3) {
        L = -ff_light_direction[i].xyz;
        att = 1.0;
    } else {
        vec3 dl = ff_light_position[i].xyz - ec;
        float dist = length(dl);
        L = dist > 0.0 ? dl / dist : vec3(0.0, 0.0, 1.0);
        vec4 at = ff_light_atten[i];
        att = dist <= at.w ? 1.0 / max(at.x + at.y * dist + at.z * dist * dist, 1e-6) : 0.0;
        if (type == 2) {
            vec4 sp = ff_light_spot[i];
            float rho = dot(-L, normalize(ff_light_direction[i].xyz));
            att *= rho > sp.x ? 1.0 : rho <= sp.y ? 0.0 : pow(max((rho - sp.y) / max(sp.x - sp.y, 1e-6), 0.0), sp.z);
        }
    }
    float nl = max(dot(nrm, L), 0.0);
    amb += att * ff_light_ambient[i].rgb;
    dif += att * nl * ff_light_diffuse[i].rgb;
    if (FF_SPECULAR != 0 && nl > 0.0)
        spe += att * pow(max(dot(nrm, normalize(L + V)), 0.0), ff_mat_power.x) * ff_light_specular[i].rgb;
}

int ff_dims(int set)
{
    return set == 0 ? FF_DIMS_0 : set == 1 ? FF_DIMS_1 : set == 2 ? FF_DIMS_2 : set == 3 ? FF_DIMS_3 :
           set == 4 ? FF_DIMS_4 : set == 5 ? FF_DIMS_5 : set == 6 ? FF_DIMS_6 : FF_DIMS_7;
}

vec4 ff_input(int set, vec4 t0, vec4 t1, vec4 t2, vec4 t3, vec4 t4, vec4 t5, vec4 t6, vec4 t7)
{
    return set == 0 ? t0 : set == 1 ? t1 : set == 2 ? t2 : set == 3 ? t3 :
           set == 4 ? t4 : set == 5 ? t5 : set == 6 ? t6 : t7;
}

// The texture coordinate of stage n: generated (camera-space normal, position,
// reflection, sphere map) or the vertex's set, padded as Direct3D pads it for
// the texture transform, then that transform.
vec4 ff_texcoord(int n, int set, int mode, int count, vec3 ec, vec3 nrm, vec4 t)
{
    vec4 v;
    if (mode == 1) v = vec4(nrm, 1.0);
    else if (mode == 2) v = vec4(ec, 1.0);
    else if (mode == 3) v = vec4(reflect(normalize(ec), nrm), 1.0);
    else if (mode == 4) v = ff_sphere(reflect(normalize(ec), nrm));
    else {
        int dims = ff_dims(set);
        if (dims == 0) v = vec4(0.0, 0.0, 0.0, 1.0);
        else if (count == 0) v = t;
        else if (dims == 1) v = vec4(t.x, 1.0, 0.0, 0.0);
        else if (dims == 2) v = vec4(t.xy, 1.0, 0.0);
        else if (dims == 3) v = vec4(t.xyz, 1.0);
        else v = t;
    }
    return count != 0 && mode != 4 ? ff_texm[n] * v : v;
}

void main()
{
    vec4 pos = s2r_decode(ff_in_pos, S2R_IN_0);
    vec4 t0 = s2r_decode(ff_in_t0, S2R_IN_6), t1 = s2r_decode(ff_in_t1, S2R_IN_7);
    vec4 t2 = s2r_decode(ff_in_t2, S2R_IN_8), t3 = s2r_decode(ff_in_t3, S2R_IN_9);
    vec4 t4 = s2r_decode(ff_in_t4, S2R_IN_10), t5 = s2r_decode(ff_in_t5, S2R_IN_11);
    vec4 t6 = s2r_decode(ff_in_t6, S2R_IN_12), t7 = s2r_decode(ff_in_t7, S2R_IN_13);
    vec3 ec = vec3(0.0), nrm = vec3(0.0);
    if (FF_POST != 0) {
        float w = pos.w != 0.0 ? 1.0 / pos.w : 1.0;
        gl_Position = vec4((pos.x - ff_viewport.x) / ff_viewport.z * 2.0 - 1.0,
                           1.0 - (pos.y - ff_viewport.y) / ff_viewport.w * 2.0, pos.z, 1.0) * w;
    } else {
        vec4 eye = ff_world_view * vec4(pos.xyz, 1.0);
        ec = eye.xyz;
        gl_Position = ff_proj * eye;
        if (FF_NORMAL != 0) {
            nrm = mat3(ff_normal[0].xyz, ff_normal[1].xyz, ff_normal[2].xyz) * s2r_decode(ff_in_nrm, S2R_IN_3).xyz;
            if (FF_NORMALIZE != 0) nrm = length(nrm) > 0.0 ? normalize(nrm) : nrm;
        }
    }
    float clip_z = gl_Position.z;
    gl_Position.y = gl_Position.y * port_flip.x;
#ifndef VULKAN
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;
#endif
    gl_Position.xy += port_flip.yz * gl_Position.w;

    vec4 c0 = FF_C0 != 0 ? s2r_decode(ff_in_c0, S2R_IN_4) : vec4(1.0);
    vec4 c1 = FF_C1 != 0 ? s2r_decode(ff_in_c1, S2R_IN_5) : vec4(0.0);
    vec4 diffuse = c0, specular = c1;
    if (FF_LIGHTING != 0) {
        vec4 md = FF_MAT_DIFFUSE == 1 ? c0 : FF_MAT_DIFFUSE == 2 ? c1 : ff_mat_diffuse;
        vec4 ms = FF_MAT_SPECULAR == 1 ? c0 : FF_MAT_SPECULAR == 2 ? c1 : ff_mat_specular;
        vec4 ma = FF_MAT_AMBIENT == 1 ? c0 : FF_MAT_AMBIENT == 2 ? c1 : ff_mat_ambient;
        vec4 me = FF_MAT_EMISSIVE == 1 ? c0 : FF_MAT_EMISSIVE == 2 ? c1 : ff_mat_emissive;
        vec3 amb = ff_global_ambient.rgb, dif = vec3(0.0), spe = vec3(0.0);
        if (FF_NORMAL != 0) {
            vec3 V = FF_LOCALVIEWER != 0 ? normalize(-ec) : vec3(0.0, 0.0, -1.0);
            if (FF_LIGHT_0 != 0) ff_light(0, FF_LIGHT_0, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_1 != 0) ff_light(1, FF_LIGHT_1, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_2 != 0) ff_light(2, FF_LIGHT_2, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_3 != 0) ff_light(3, FF_LIGHT_3, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_4 != 0) ff_light(4, FF_LIGHT_4, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_5 != 0) ff_light(5, FF_LIGHT_5, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_6 != 0) ff_light(6, FF_LIGHT_6, nrm, V, ec, amb, dif, spe);
            if (FF_LIGHT_7 != 0) ff_light(7, FF_LIGHT_7, nrm, V, ec, amb, dif, spe);
        } else {
            if (FF_LIGHT_0 != 0) amb += ff_light_ambient[0].rgb;
            if (FF_LIGHT_1 != 0) amb += ff_light_ambient[1].rgb;
            if (FF_LIGHT_2 != 0) amb += ff_light_ambient[2].rgb;
            if (FF_LIGHT_3 != 0) amb += ff_light_ambient[3].rgb;
            if (FF_LIGHT_4 != 0) amb += ff_light_ambient[4].rgb;
            if (FF_LIGHT_5 != 0) amb += ff_light_ambient[5].rgb;
            if (FF_LIGHT_6 != 0) amb += ff_light_ambient[6].rgb;
            if (FF_LIGHT_7 != 0) amb += ff_light_ambient[7].rgb;
        }
        diffuse = clamp(vec4(me.rgb + ma.rgb * amb + md.rgb * dif, md.a), 0.0, 1.0);
        specular = clamp(vec4(ms.rgb * spe, ms.a), 0.0, 1.0);
    }
    io_10_0 = diffuse;
    io_10_1 = specular;

    // The fog coordinate (d3d9_ff.c FFOG_*): the pixel half applies the formula.
    if (FF_FOG_COORD == 1) io_11_0 = vec4(c1.a);
    else if (FF_FOG_COORD == 2) io_11_0 = vec4(abs(ec.z));
    else if (FF_FOG_COORD == 3) io_11_0 = vec4(length(ec));
    else if (FF_FOG_COORD == 4) io_11_0 = vec4(clip_z);
    else if (FF_FOG_COORD == 5) io_11_0 = vec4(pos.z);
    else io_11_0 = vec4(0.0);

    const vec4 none = vec4(0.0, 0.0, 0.0, 1.0);
    io_5_0 = (FF_OUT_TEX & 1) != 0 ? ff_texcoord(0, FF_TC_SET_0, FF_TC_MODE_0, FF_TC_COUNT_0, ec, nrm,
        ff_input(FF_TC_SET_0, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_1 = (FF_OUT_TEX & 2) != 0 ? ff_texcoord(1, FF_TC_SET_1, FF_TC_MODE_1, FF_TC_COUNT_1, ec, nrm,
        ff_input(FF_TC_SET_1, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_2 = (FF_OUT_TEX & 4) != 0 ? ff_texcoord(2, FF_TC_SET_2, FF_TC_MODE_2, FF_TC_COUNT_2, ec, nrm,
        ff_input(FF_TC_SET_2, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_3 = (FF_OUT_TEX & 8) != 0 ? ff_texcoord(3, FF_TC_SET_3, FF_TC_MODE_3, FF_TC_COUNT_3, ec, nrm,
        ff_input(FF_TC_SET_3, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_4 = (FF_OUT_TEX & 16) != 0 ? ff_texcoord(4, FF_TC_SET_4, FF_TC_MODE_4, FF_TC_COUNT_4, ec, nrm,
        ff_input(FF_TC_SET_4, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_5 = (FF_OUT_TEX & 32) != 0 ? ff_texcoord(5, FF_TC_SET_5, FF_TC_MODE_5, FF_TC_COUNT_5, ec, nrm,
        ff_input(FF_TC_SET_5, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_6 = (FF_OUT_TEX & 64) != 0 ? ff_texcoord(6, FF_TC_SET_6, FF_TC_MODE_6, FF_TC_COUNT_6, ec, nrm,
        ff_input(FF_TC_SET_6, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
    io_5_7 = (FF_OUT_TEX & 128) != 0 ? ff_texcoord(7, FF_TC_SET_7, FF_TC_MODE_7, FF_TC_COUNT_7, ec, nrm,
        ff_input(FF_TC_SET_7, t0, t1, t2, t3, t4, t5, t6, t7)) : none;
}
