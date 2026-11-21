/* The native shader library: the fixed-function and present shaders, and the
 * game shaders known at build time, translated on the PC (tools/
 * build_shaderlib.py) into GLSL ES text (OpenGL ES) and SPIR-V (Vulkan), keyed
 * by the FNV-1a 64 hash of the Direct3D 9 bytecode (and, for ps_1_x, the
 * sampler types it was translated for). It travels in the APK's runtime.zip
 * (shaders/s2r_shaderlib.bin), which GameAssets installs into the app's files;
 * libsims2.so does not contain it.
 *
 * The game builds its shaders from its own material text at run time with the
 * D3DX linked into the exe; the bytecode it hands Direct3D only names a library
 * entry. A known shader needs no Direct3D shader work on the device: no
 * MojoShader, no glslang (pixel shaders carry a second SPIR-V with the UI FSR 1
 * reads, s2r_uifilter.c). Only a shader the library lacks (a mod's, a material
 * no capture saw) is still translated on the device; debug.s2.shaderlib=strict
 * refuses that, to check a capture's coverage. */
#ifndef S2R_SHADERLIB_H
#define S2R_SHADERLIB_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s2r_lib_map { uint16_t dst, reg, count; } s2r_lib_map;   /* packed element <- register */

typedef struct s2r_lib_shader {
    uint64_t hash;                /* FNV-1a 64 of the bytecode up to its end token */
    uint32_t size;
    uint32_t variant_types;       /* ps_1_x: the sampler types (2 bits per register) it was translated for */
    uint8_t pixel, major, minor, writes_fog, shadow_pcf;
    uint32_t glsl;                /* offset of the GLSL body in the text pool */
    uint32_t spv, spv_words;      /* SPIR-V in the word pool */
    uint8_t attr_count;           /* vertex inputs in location order: usage, usage index */
    uint8_t attr_usage[16], attr_index[16];
    uint16_t input_mask;          /* input locations the vertex shader declares */
    uint16_t samplers;            /* bit per sampler register */
    uint32_t sampler_types;       /* 2 bits per register: 0 2D, 1 cube, 2 3D */
    uint8_t ps_colors, ps_tex, ps_fog;   /* varyings a pixel shader reads */
    uint16_t f_total, i_total, b_total;  /* std140 block: packed registers per set */
    uint16_t map_first, fmap_count, imap_count, bmap_count;
    uint8_t texbem_count, texbem_stage[8];
    uint16_t texbem_dst[8];
    const char *name;             /* where it came from */
    /* Resolved when the entry is made (the library file or a run-time translation): */
    const char *text;             /* the GLSL body */
    const s2r_lib_map *maps;      /* fmap, then imap, then bmap */
    const uint32_t *spirv;        /* NULL: translated on the device (Vulkan compiles the text with glslang) */
    const uint32_t *spirv_ui;     /* pixel shaders: the SPIR-V with the UI FSR 1 reads; NULL: none in the library */
    uint32_t spv_ui, spv_ui_words;
    int runtime;                  /* 1: not from the library */
    uint64_t backend_module;      /* the Vulkan backend's module, made once */
    uint32_t backend_gl;          /* the OpenGL ES backend's compiled shader object(s), cached per entry */
} s2r_lib_shader;

typedef struct s2r_lib_builtin { const char *name; uint32_t gles, spv, spv_words, spv_ui, spv_ui_words; } s2r_lib_builtin;
enum { S2R_BUILTIN_FF_VS, S2R_BUILTIN_FF_PS, S2R_BUILTIN_PRESENT_VS, S2R_BUILTIN_PRESENT_PS, S2R_BUILTIN_COUNT };

typedef struct s2r_lib {
    const s2r_lib_shader *shaders; uint32_t shader_count;
    const s2r_lib_map *maps;
    const char *text;
    const uint32_t *spv;
    const s2r_lib_builtin *builtins;
} s2r_lib;

extern s2r_lib s2r_shader_library;
/* The folder GameAssets installed the library into (main.c, before the device). */
void s2r_lib_set_folder(const char *folder);
/* Loads the library once; 0 when it is missing or damaged (logged). */
int s2r_lib_load(void);

uint64_t s2r_fnv64(const void *data, uint32_t size);
/* The entry of this bytecode (and, for ps_1_x, these sampler types): the
 * library's, else - when `allow_runtime` - a translation made on the device,
 * cached by hash and types. NULL when there is none (logged once per hash and
 * types). */
const s2r_lib_shader *s2r_lib_shader_for(int pixel, const uint8_t *code, uint32_t size, uint32_t sampler_types,
    int allow_runtime);
/* debug.s2.shaderlib=strict: no translation on the device; a shader the
 * library lacks fails (logged once, dumped with shaderdump=1). */
int s2r_lib_strict(void);
/* Clears the backend modules cached on every entry (the Vulkan device went away). */
void s2r_lib_forget_modules(void);
/* The device's translations this session (the [mem] line and the exit summary). */
void s2r_lib_stats(uint32_t *library_hits, uint32_t *runtime_entries, uint64_t *runtime_ns);
/* debug.s2.shaderdump=1: the bytecode of every game shader the library lacks goes to
 * <data>/shaderdump/<hash>.{vs,ps}, and a ps_1_x sampler-type map it lacks to
 * <hash>.ps.<types>.types (tools/build_shaderlib.py capture reads them). */
void s2r_lib_dump_missing(int pixel, const uint8_t *code, uint32_t size, uint64_t hash, uint32_t types);

#ifdef __cplusplus
}
#endif
#endif
