// Direct3D 9 fixed-function texture stages for the Heroes V native renderer.
// One shader for every stage setup: the eight stages' operations, arguments,
// sampler kinds and projections arrive as constants (s2r_spec.h, FF_*), so the
// driver folds the branches away. The semantics are the former generator's
// (s2r_ff.c): D3DTOP_* operations with D3DTA_* arguments, the result to
// CURRENT or TEMP, unbound textures reading (0,0,0,1), the specular add, the
// fog blend and the alpha test on the clamped 8-bit alpha. Vulkan declares the
// three sampler kinds of a stage on one binding (only the one its constant
// selects is ever read); OpenGL ES declares only that one.
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp samplerCube;
precision highp sampler3D;

layout(std140, binding = 2) uniform S2R_FF {
    mat4 ff_world_view; mat4 ff_proj; vec4 ff_normal[3]; mat4 ff_view; mat4 ff_texm[8];
    vec4 ff_viewport; vec4 ff_mat_diffuse; vec4 ff_mat_ambient; vec4 ff_mat_specular; vec4 ff_mat_emissive;
    vec4 ff_mat_power; vec4 ff_global_ambient;
    vec4 ff_light_diffuse[8]; vec4 ff_light_specular[8]; vec4 ff_light_ambient[8];
    vec4 ff_light_position[8]; vec4 ff_light_direction[8]; vec4 ff_light_atten[8]; vec4 ff_light_spot[8];
    vec4 ff_fog; vec4 ff_fog_color; vec4 ff_tfactor; vec4 ff_bumpenv[8]; vec4 ff_alpha_ref;
};
layout(std140, binding = 3) uniform S2R_PORT { vec4 port_flip; vec4 port_alpha; vec4 port_fog_color; vec4 port_vpos; vec4 port_fog; };

#pragma s2r_spec ff_ps

layout(location = 0) out highp vec4 ff_color;
layout(location = 0) in highp vec4 io_10_0;
layout(location = 1) in highp vec4 io_10_1;
layout(location = 2) in highp vec4 io_5_0;
layout(location = 3) in highp vec4 io_5_1;
layout(location = 4) in highp vec4 io_5_2;
layout(location = 5) in highp vec4 io_5_3;
layout(location = 6) in highp vec4 io_5_4;
layout(location = 7) in highp vec4 io_5_5;
layout(location = 8) in highp vec4 io_5_6;
layout(location = 9) in highp vec4 io_5_7;
layout(location = 10) in highp vec4 io_11_0;

#ifdef VULKAN
layout(binding = 4) uniform highp sampler2D ff_2d0;
layout(binding = 4) uniform highp samplerCube ff_cube0;
layout(binding = 4) uniform highp sampler3D ff_vol0;
vec4 ff_tex0(vec4 tc) { return FF_KIND_0 == 1 ? texture(ff_cube0, tc.xyz) : FF_KIND_0 == 2 ? texture(ff_vol0, tc.xyz) : texture(ff_2d0, tc.xy); }
#elif FF_TEX_0 != 0 && FF_KIND_0 == 1
layout(binding = 4) uniform highp samplerCube ff_cube0;
vec4 ff_tex0(vec4 tc) { return texture(ff_cube0, tc.xyz); }
#elif FF_TEX_0 != 0 && FF_KIND_0 == 2
layout(binding = 4) uniform highp sampler3D ff_vol0;
vec4 ff_tex0(vec4 tc) { return texture(ff_vol0, tc.xyz); }
#elif FF_TEX_0 != 0
layout(binding = 4) uniform highp sampler2D ff_2d0;
vec4 ff_tex0(vec4 tc) { return texture(ff_2d0, tc.xy); }
#else
vec4 ff_tex0(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 5) uniform highp sampler2D ff_2d1;
layout(binding = 5) uniform highp samplerCube ff_cube1;
layout(binding = 5) uniform highp sampler3D ff_vol1;
vec4 ff_tex1(vec4 tc) { return FF_KIND_1 == 1 ? texture(ff_cube1, tc.xyz) : FF_KIND_1 == 2 ? texture(ff_vol1, tc.xyz) : texture(ff_2d1, tc.xy); }
#elif FF_TEX_1 != 0 && FF_KIND_1 == 1
layout(binding = 5) uniform highp samplerCube ff_cube1;
vec4 ff_tex1(vec4 tc) { return texture(ff_cube1, tc.xyz); }
#elif FF_TEX_1 != 0 && FF_KIND_1 == 2
layout(binding = 5) uniform highp sampler3D ff_vol1;
vec4 ff_tex1(vec4 tc) { return texture(ff_vol1, tc.xyz); }
#elif FF_TEX_1 != 0
layout(binding = 5) uniform highp sampler2D ff_2d1;
vec4 ff_tex1(vec4 tc) { return texture(ff_2d1, tc.xy); }
#else
vec4 ff_tex1(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 6) uniform highp sampler2D ff_2d2;
layout(binding = 6) uniform highp samplerCube ff_cube2;
layout(binding = 6) uniform highp sampler3D ff_vol2;
vec4 ff_tex2(vec4 tc) { return FF_KIND_2 == 1 ? texture(ff_cube2, tc.xyz) : FF_KIND_2 == 2 ? texture(ff_vol2, tc.xyz) : texture(ff_2d2, tc.xy); }
#elif FF_TEX_2 != 0 && FF_KIND_2 == 1
layout(binding = 6) uniform highp samplerCube ff_cube2;
vec4 ff_tex2(vec4 tc) { return texture(ff_cube2, tc.xyz); }
#elif FF_TEX_2 != 0 && FF_KIND_2 == 2
layout(binding = 6) uniform highp sampler3D ff_vol2;
vec4 ff_tex2(vec4 tc) { return texture(ff_vol2, tc.xyz); }
#elif FF_TEX_2 != 0
layout(binding = 6) uniform highp sampler2D ff_2d2;
vec4 ff_tex2(vec4 tc) { return texture(ff_2d2, tc.xy); }
#else
vec4 ff_tex2(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 7) uniform highp sampler2D ff_2d3;
layout(binding = 7) uniform highp samplerCube ff_cube3;
layout(binding = 7) uniform highp sampler3D ff_vol3;
vec4 ff_tex3(vec4 tc) { return FF_KIND_3 == 1 ? texture(ff_cube3, tc.xyz) : FF_KIND_3 == 2 ? texture(ff_vol3, tc.xyz) : texture(ff_2d3, tc.xy); }
#elif FF_TEX_3 != 0 && FF_KIND_3 == 1
layout(binding = 7) uniform highp samplerCube ff_cube3;
vec4 ff_tex3(vec4 tc) { return texture(ff_cube3, tc.xyz); }
#elif FF_TEX_3 != 0 && FF_KIND_3 == 2
layout(binding = 7) uniform highp sampler3D ff_vol3;
vec4 ff_tex3(vec4 tc) { return texture(ff_vol3, tc.xyz); }
#elif FF_TEX_3 != 0
layout(binding = 7) uniform highp sampler2D ff_2d3;
vec4 ff_tex3(vec4 tc) { return texture(ff_2d3, tc.xy); }
#else
vec4 ff_tex3(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 8) uniform highp sampler2D ff_2d4;
layout(binding = 8) uniform highp samplerCube ff_cube4;
layout(binding = 8) uniform highp sampler3D ff_vol4;
vec4 ff_tex4(vec4 tc) { return FF_KIND_4 == 1 ? texture(ff_cube4, tc.xyz) : FF_KIND_4 == 2 ? texture(ff_vol4, tc.xyz) : texture(ff_2d4, tc.xy); }
#elif FF_TEX_4 != 0 && FF_KIND_4 == 1
layout(binding = 8) uniform highp samplerCube ff_cube4;
vec4 ff_tex4(vec4 tc) { return texture(ff_cube4, tc.xyz); }
#elif FF_TEX_4 != 0 && FF_KIND_4 == 2
layout(binding = 8) uniform highp sampler3D ff_vol4;
vec4 ff_tex4(vec4 tc) { return texture(ff_vol4, tc.xyz); }
#elif FF_TEX_4 != 0
layout(binding = 8) uniform highp sampler2D ff_2d4;
vec4 ff_tex4(vec4 tc) { return texture(ff_2d4, tc.xy); }
#else
vec4 ff_tex4(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 9) uniform highp sampler2D ff_2d5;
layout(binding = 9) uniform highp samplerCube ff_cube5;
layout(binding = 9) uniform highp sampler3D ff_vol5;
vec4 ff_tex5(vec4 tc) { return FF_KIND_5 == 1 ? texture(ff_cube5, tc.xyz) : FF_KIND_5 == 2 ? texture(ff_vol5, tc.xyz) : texture(ff_2d5, tc.xy); }
#elif FF_TEX_5 != 0 && FF_KIND_5 == 1
layout(binding = 9) uniform highp samplerCube ff_cube5;
vec4 ff_tex5(vec4 tc) { return texture(ff_cube5, tc.xyz); }
#elif FF_TEX_5 != 0 && FF_KIND_5 == 2
layout(binding = 9) uniform highp sampler3D ff_vol5;
vec4 ff_tex5(vec4 tc) { return texture(ff_vol5, tc.xyz); }
#elif FF_TEX_5 != 0
layout(binding = 9) uniform highp sampler2D ff_2d5;
vec4 ff_tex5(vec4 tc) { return texture(ff_2d5, tc.xy); }
#else
vec4 ff_tex5(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 10) uniform highp sampler2D ff_2d6;
layout(binding = 10) uniform highp samplerCube ff_cube6;
layout(binding = 10) uniform highp sampler3D ff_vol6;
vec4 ff_tex6(vec4 tc) { return FF_KIND_6 == 1 ? texture(ff_cube6, tc.xyz) : FF_KIND_6 == 2 ? texture(ff_vol6, tc.xyz) : texture(ff_2d6, tc.xy); }
#elif FF_TEX_6 != 0 && FF_KIND_6 == 1
layout(binding = 10) uniform highp samplerCube ff_cube6;
vec4 ff_tex6(vec4 tc) { return texture(ff_cube6, tc.xyz); }
#elif FF_TEX_6 != 0 && FF_KIND_6 == 2
layout(binding = 10) uniform highp sampler3D ff_vol6;
vec4 ff_tex6(vec4 tc) { return texture(ff_vol6, tc.xyz); }
#elif FF_TEX_6 != 0
layout(binding = 10) uniform highp sampler2D ff_2d6;
vec4 ff_tex6(vec4 tc) { return texture(ff_2d6, tc.xy); }
#else
vec4 ff_tex6(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif
#ifdef VULKAN
layout(binding = 11) uniform highp sampler2D ff_2d7;
layout(binding = 11) uniform highp samplerCube ff_cube7;
layout(binding = 11) uniform highp sampler3D ff_vol7;
vec4 ff_tex7(vec4 tc) { return FF_KIND_7 == 1 ? texture(ff_cube7, tc.xyz) : FF_KIND_7 == 2 ? texture(ff_vol7, tc.xyz) : texture(ff_2d7, tc.xy); }
#elif FF_TEX_7 != 0 && FF_KIND_7 == 1
layout(binding = 11) uniform highp samplerCube ff_cube7;
vec4 ff_tex7(vec4 tc) { return texture(ff_cube7, tc.xyz); }
#elif FF_TEX_7 != 0 && FF_KIND_7 == 2
layout(binding = 11) uniform highp sampler3D ff_vol7;
vec4 ff_tex7(vec4 tc) { return texture(ff_vol7, tc.xyz); }
#elif FF_TEX_7 != 0
layout(binding = 11) uniform highp sampler2D ff_2d7;
vec4 ff_tex7(vec4 tc) { return texture(ff_2d7, tc.xy); }
#else
vec4 ff_tex7(vec4 tc) { return vec4(0.0, 0.0, 0.0, 1.0); }
#endif

vec4 ff_project(vec4 tc, int divisor)
{
    if (divisor == 2) tc.xyz /= tc.y;
    else if (divisor == 3) tc.xyz /= tc.z;
    else if (divisor == 4) tc.xyz /= tc.w;
    return tc;
}

// D3DTA_*: diffuse, current (diffuse on stage 0), texture, factor, specular,
// temp; then the alpha replicate and the complement.
vec4 ff_arg(int arg, bool first, vec4 t, vec4 diffuse, vec4 specular, vec4 current, vec4 temp)
{
    int sel = arg & 15;
    vec4 base = sel == 0 ? diffuse : sel == 2 ? t : sel == 3 ? ff_tfactor : sel == 4 ? specular :
                sel == 5 ? temp : first ? diffuse : current;
    if ((arg & 32) != 0) base = vec4(base.a);
    if ((arg & 16) != 0) base = vec4(1.0) - base;
    return base;
}

// D3DTOP_*
vec4 ff_op(int op, vec4 a1, vec4 a2, vec4 t, vec4 diffuse, vec4 cur)
{
    if (op == 2) return a1;
    if (op == 3) return a2;
    if (op == 4) return a1 * a2;
    if (op == 5) return a1 * a2 * 2.0;
    if (op == 6) return a1 * a2 * 4.0;
    if (op == 7) return a1 + a2;
    if (op == 8) return a1 + a2 - 0.5;
    if (op == 9) return (a1 + a2 - 0.5) * 2.0;
    if (op == 10) return a1 - a2;
    if (op == 11) return a1 + a2 * (vec4(1.0) - a1);
    if (op == 12) return mix(a2, a1, diffuse.a);
    if (op == 13) return mix(a2, a1, t.a);
    if (op == 14) return mix(a2, a1, ff_tfactor.a);
    if (op == 15) return a1 + a2 * (1.0 - t.a);
    if (op == 16) return mix(a2, a1, cur.a);
    if (op == 18) return a1 + vec4(a1.a) * a2;
    if (op == 19) return vec4((a1 * a2).rgb, a1.a + a2.a);
    if (op == 20) return a1 + vec4(1.0 - a1.a) * a2;
    if (op == 21) return vec4(((vec4(1.0) - a1) * a2).rgb, a1.a + a2.a);
    if (op == 24) return vec4(clamp(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4.0, 0.0, 1.0));
    return a1 * a2;
}

void ff_stage(bool first, int cop, int ca1, int ca2, int aop, int aa1, int aa2, int to_temp, vec4 t,
              vec4 diffuse, vec4 specular, inout vec4 current, inout vec4 temp)
{
    vec4 cur = first ? diffuse : current;
    vec4 c = ff_op(cop, ff_arg(ca1, first, t, diffuse, specular, current, temp),
                   ff_arg(ca2, first, t, diffuse, specular, current, temp), t, diffuse, cur);
    vec4 a = aop == 1 ? cur : ff_op(aop, ff_arg(aa1, first, t, diffuse, specular, current, temp),
                                    ff_arg(aa2, first, t, diffuse, specular, current, temp), t, diffuse, cur);
    vec4 r = clamp(vec4(c.rgb, a.a), 0.0, 1.0);
    if (to_temp != 0) temp = r; else current = r;
}

void main()
{
    vec4 v_diffuse = io_10_0, v_specular = io_10_1;
    vec4 current = v_diffuse, temp = vec4(0.0), t = vec4(0.0, 0.0, 0.0, 1.0);
    if (FF_STAGES > 0) {
        t = FF_TEX_0 != 0 ? ff_tex0(ff_project(io_5_0, FF_PROJ_0)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(true, FF_COP_0, FF_CA1_0, FF_CA2_0, FF_AOP_0, FF_AA1_0, FF_AA2_0, FF_TEMP_0, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 1) {
        t = FF_TEX_1 != 0 ? ff_tex1(ff_project(io_5_1, FF_PROJ_1)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_1, FF_CA1_1, FF_CA2_1, FF_AOP_1, FF_AA1_1, FF_AA2_1, FF_TEMP_1, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 2) {
        t = FF_TEX_2 != 0 ? ff_tex2(ff_project(io_5_2, FF_PROJ_2)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_2, FF_CA1_2, FF_CA2_2, FF_AOP_2, FF_AA1_2, FF_AA2_2, FF_TEMP_2, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 3) {
        t = FF_TEX_3 != 0 ? ff_tex3(ff_project(io_5_3, FF_PROJ_3)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_3, FF_CA1_3, FF_CA2_3, FF_AOP_3, FF_AA1_3, FF_AA2_3, FF_TEMP_3, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 4) {
        t = FF_TEX_4 != 0 ? ff_tex4(ff_project(io_5_4, FF_PROJ_4)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_4, FF_CA1_4, FF_CA2_4, FF_AOP_4, FF_AA1_4, FF_AA2_4, FF_TEMP_4, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 5) {
        t = FF_TEX_5 != 0 ? ff_tex5(ff_project(io_5_5, FF_PROJ_5)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_5, FF_CA1_5, FF_CA2_5, FF_AOP_5, FF_AA1_5, FF_AA2_5, FF_TEMP_5, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 6) {
        t = FF_TEX_6 != 0 ? ff_tex6(ff_project(io_5_6, FF_PROJ_6)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_6, FF_CA1_6, FF_CA2_6, FF_AOP_6, FF_AA1_6, FF_AA2_6, FF_TEMP_6, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_STAGES > 7) {
        t = FF_TEX_7 != 0 ? ff_tex7(ff_project(io_5_7, FF_PROJ_7)) : vec4(0.0, 0.0, 0.0, 1.0);
        ff_stage(false, FF_COP_7, FF_CA1_7, FF_CA2_7, FF_AOP_7, FF_AA1_7, FF_AA2_7, FF_TEMP_7, t, v_diffuse, v_specular, current, temp);
    }
    if (FF_ADD_SPECULAR != 0) current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);
    if (FF_FOG != 0) {
        // d3d9_ff.c: f = (end - c) * scale (linear), exp(-d c), exp(-(d c)^2) on the fog coordinate.
        float c = io_11_0.x;
        float f = FF_FOG == 2 ? exp(-port_fog.z * c) : FF_FOG == 3 ? exp(-(port_fog.z * c) * (port_fog.z * c)) :
                  (port_fog.x - c) * port_fog.y;
        current.rgb = mix(port_fog_color.rgb, current.rgb, clamp(f, 0.0, 1.0));
    }
    if (FF_ALPHA_FUNC != 0) {
        float a = floor(clamp(current.a, 0.0, 1.0) * 255.0 + 0.5), r = port_alpha.x;
        bool pass = FF_ALPHA_FUNC == 2 ? a < r : FF_ALPHA_FUNC == 3 ? a == r : FF_ALPHA_FUNC == 4 ? a <= r :
                    FF_ALPHA_FUNC == 5 ? a > r : FF_ALPHA_FUNC == 6 ? a != r : FF_ALPHA_FUNC == 7 ? a >= r : false;
        if (!pass) discard;
    }
    ff_color = current;
}
