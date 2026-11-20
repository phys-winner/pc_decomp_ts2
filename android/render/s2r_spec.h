/* Variant constants of the native shaders: Vulkan specialization constants
 * (constant_id = the number) and, for OpenGL ES, #define lines in front of
 * the same GLSL. One list feeds both sides: tools/build_shaderlib.py writes the
 * Vulkan declarations from it where a shader says "#pragma s2r_spec <list>",
 * and the backends fill the values from it.
 *
 * Game vertex shaders: S2R_IN_<location> (ids 0..15) decode an input:
 *   0 as fetched, 1 D3DCOLOR (BGRA), 2 UBYTE4, 3 SHORT2, 4 SHORT4, 5 UDEC3
 * (Vulkan reads the integer formats as UNORM/SNORM and scales them back);
 * S2R_FOG_DEPTH (16): table fog, the fog coordinate is the clip-space z.
 * Game pixel shaders: S2R_ALPHA_FUNC (20) D3DCMP_* or 0, S2R_FOG (21) the fog
 * formula (0 off, 1 linear, 2 exp, 3 exp2) on io_11_0.x with port_fog. */
#ifndef S2R_SPEC_H
#define S2R_SPEC_H

enum { S2R_DECODE_NONE = 0, S2R_DECODE_BGRA, S2R_DECODE_UBYTE4, S2R_DECODE_SHORT2, S2R_DECODE_SHORT4, S2R_DECODE_UDEC3 };
/* The fog formula (S2R_FOG, FF_FOG) and the fixed-function vertex half's fog
 * coordinate (FF_FOG_COORD), as d3d9_ff.c names them (FOG_*, FFOG_*). */
enum { S2R_FOG_OFF = 0, S2R_FOG_LINEAR, S2R_FOG_EXP, S2R_FOG_EXP2 };
enum { S2R_FOGC_NONE = 0, S2R_FOGC_SPECULAR, S2R_FOGC_DEPTH, S2R_FOGC_RANGE, S2R_FOGC_ORTHO, S2R_FOGC_TRANSFORMED };

#define S2R_SPEC_IN(X) \
    X(0, S2R_IN_0) X(1, S2R_IN_1) X(2, S2R_IN_2) X(3, S2R_IN_3) X(4, S2R_IN_4) X(5, S2R_IN_5) X(6, S2R_IN_6) \
    X(7, S2R_IN_7) X(8, S2R_IN_8) X(9, S2R_IN_9) X(10, S2R_IN_10) X(11, S2R_IN_11) X(12, S2R_IN_12) \
    X(13, S2R_IN_13) X(14, S2R_IN_14) X(15, S2R_IN_15)

#define S2R_SPEC_VS(X) X(16, S2R_FOG_DEPTH)

#define S2R_SPEC_PS(X) X(20, S2R_ALPHA_FUNC) X(21, S2R_FOG)

/* Fixed-function vertex half (shaders/ff.vert); its inputs use S2R_IN_<n> too. */
#define S2R_SPEC_FF_VS(X) \
    X(32, FF_POST) X(33, FF_NORMAL) X(34, FF_C0) X(35, FF_C1) \
    X(36, FF_DIMS_0) X(37, FF_DIMS_1) X(38, FF_DIMS_2) X(39, FF_DIMS_3) \
    X(40, FF_DIMS_4) X(41, FF_DIMS_5) X(42, FF_DIMS_6) X(43, FF_DIMS_7) \
    X(44, FF_LIGHTING) X(45, FF_SPECULAR) X(46, FF_LOCALVIEWER) \
    X(47, FF_MAT_DIFFUSE) X(48, FF_MAT_SPECULAR) X(49, FF_MAT_AMBIENT) X(50, FF_MAT_EMISSIVE) \
    X(51, FF_LIGHT_0) X(52, FF_LIGHT_1) X(53, FF_LIGHT_2) X(54, FF_LIGHT_3) \
    X(55, FF_LIGHT_4) X(56, FF_LIGHT_5) X(57, FF_LIGHT_6) X(58, FF_LIGHT_7) \
    X(59, FF_FOG_COORD) X(60, FF_NORMALIZE) X(61, FF_OUT_TEX) \
    X(62, FF_TC_SET_0) X(63, FF_TC_SET_1) X(64, FF_TC_SET_2) X(65, FF_TC_SET_3) \
    X(66, FF_TC_SET_4) X(67, FF_TC_SET_5) X(68, FF_TC_SET_6) X(69, FF_TC_SET_7) \
    X(70, FF_TC_MODE_0) X(71, FF_TC_MODE_1) X(72, FF_TC_MODE_2) X(73, FF_TC_MODE_3) \
    X(74, FF_TC_MODE_4) X(75, FF_TC_MODE_5) X(76, FF_TC_MODE_6) X(77, FF_TC_MODE_7) \
    X(78, FF_TC_COUNT_0) X(79, FF_TC_COUNT_1) X(80, FF_TC_COUNT_2) X(81, FF_TC_COUNT_3) \
    X(82, FF_TC_COUNT_4) X(83, FF_TC_COUNT_5) X(84, FF_TC_COUNT_6) X(85, FF_TC_COUNT_7)

/* Fixed-function pixel half (shaders/ff.frag): per stage s the ten values
 * at 40 + 10 s are colour op/arg1/arg2, alpha op/arg1/arg2 (D3DTOP_*,
 * D3DTA_*), sampler kind (0 2D, 1 cube, 2 3D), projection divisor
 * component (0 none, 2..4), texture bound, result to temp. */
#define S2R_FF_STAGE_SPEC(X, s, b) \
    X(b + 0, FF_COP_##s) X(b + 1, FF_CA1_##s) X(b + 2, FF_CA2_##s) X(b + 3, FF_AOP_##s) X(b + 4, FF_AA1_##s) \
    X(b + 5, FF_AA2_##s) X(b + 6, FF_KIND_##s) X(b + 7, FF_PROJ_##s) X(b + 8, FF_TEX_##s) X(b + 9, FF_TEMP_##s)
#define S2R_SPEC_FF_PS(X) \
    X(32, FF_STAGES) X(33, FF_ADD_SPECULAR) X(34, FF_FOG) X(35, FF_ALPHA_FUNC) \
    S2R_FF_STAGE_SPEC(X, 0, 40) S2R_FF_STAGE_SPEC(X, 1, 50) S2R_FF_STAGE_SPEC(X, 2, 60) S2R_FF_STAGE_SPEC(X, 3, 70) \
    S2R_FF_STAGE_SPEC(X, 4, 80) S2R_FF_STAGE_SPEC(X, 5, 90) S2R_FF_STAGE_SPEC(X, 6, 100) S2R_FF_STAGE_SPEC(X, 7, 110)

#endif
