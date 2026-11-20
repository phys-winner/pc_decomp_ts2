/* Direct3D 9 shader bytecode -> GLSL with the native renderer's conventions.
 *
 * One translator for two callers: tools/shaderlib/s2_shaderc.c runs it on the
 * PC over the game shaders the build knows (the library in the APK), and
 * the renderer runs it on the device for a shader or a sampler
 * variant the library lacks. Both get the same text and facts. Plain C over
 * MojoShader; no renderer state. */
#ifndef S2R_TRANSLATE_H
#define S2R_TRANSLATE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s2r_translation {
    int pixel, major, minor, synthetic_ctab;
    int f_total, i_total, b_total;            /* packed registers per set */
    int map[3][64][3];                        /* fmap/imap/bmap: dst, register, count */
    int map_count[3];
    int texbem[8][2], texbem_count;           /* stage, destination register */
    int attributes[16][2], attribute_count;   /* usage, usage index in location order */
    int samplers[16][2], sampler_count;       /* register, type (0 2D, 1 cube, 2 3D) */
    unsigned ps_colors, ps_tex, ps_fog;       /* varyings a pixel half reads */
    int writes_fog;
    int shadow_pcf;                           /* unused (Heroes V terrain shadows) */
    unsigned input_mask;                      /* vertex input locations */
    char *glsl;                               /* the body, without #version */
} s2r_translation;

/* `sampler_types`: 2 bits per sampler register (0 2D, 1 cube, 2 3D) for a
 * ps_1_x shader, whose bytecode does not declare them (the bound texture
 * decides); ignored for other shaders. 1 on success; on failure 0 with a
 * reason in `error`. */
int s2r_translate(int pixel, const uint8_t *code, uint32_t size, uint32_t sampler_types, s2r_translation *out,
    char *error, size_t error_cap);
void s2r_translation_free(s2r_translation *t);

#ifdef __cplusplus
}
#endif
#endif
