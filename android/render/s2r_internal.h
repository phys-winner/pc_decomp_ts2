/* Native renderers of The Sims 2: the Direct3D 9 the game calls, drawn
 * directly with OpenGL ES 3.2 or Vulkan 1.1 (wiki notes 22-24), in place of
 * DXVK and of the Direct3D 9 -> GLES translation of d3d9_gl.c.
 *
 * Sims2EP9.exe imports only Direct3DCreate9 from d3d9.dll and talks to the
 * COM objects it hands out (GZGraphic4's "DX9 driver"): the boundary is the
 * Direct3D 9 API, as in Heroes V (note 23). The port's Direct3D 9 front end
 * (android/host/d3d9_device.c, d3d9_resource.c) stays what the game talks to:
 * it keeps the state model, the locks and guest copies, the HUD tags, the
 * touch layer's hooks, and records every GPU operation for the render thread
 * (d3d9_thread.c) with a copy of the state it needs. On the render thread
 * those operations go here instead of to d3d9_gl.c when a native backend runs
 * (s2r_on()): s2r_core.c keeps the backend objects of the front end's
 * textures, surfaces, buffers, shaders and queries, s2r_draw.c resolves the
 * render thread's copy of the device state into an s2r_draw packet, and a
 * backend (s2r_gles.c, s2r_vulkan.c, Heroes V's and Painkiller's) draws it.
 *
 * Shaders: the game's Direct3D 9 bytecode goes through one translator
 * (s2r_translate.c, MojoShader plus the text fixes of d3d9_shader.c and the
 * native conventions); a build-time library (s2r_shaderlib.h) answers the
 * known ones, the rest are translated on the device (Vulkan compiles them with
 * glslang once, s2r_glslang.cpp, and keeps the SPIR-V on disk). The fixed
 * function is two port-written shaders driven by constants (shaders/ff.*).
 *
 * Conventions shared by both backends (Painkiller's, wiki note 23):
 *   - every render target, the back buffer included, stores its rows
 *     top-down as Direct3D does: shaders negate clip-space Y (vpFlip = -1),
 *     the GL present flips once, Vulkan needs no flip at all;
 *   - D3D9's half-pixel offset is applied in the vertex stage (port_flip.yz);
 *   - textures keep Direct3D's channel order in memory and sample through a
 *     component swizzle (BGRA, XRGB, L8, A8 ...); render targets store natural
 *     RGBA (d3d_format_convert's render-target path);
 *   - Vulkan keeps Direct3D's depth range; OpenGL ES remaps z in the shader. */
#ifndef S2R_INTERNAL_H
#define S2R_INTERNAL_H

#include "../host/d3d9_internal.h"
#include "s2r_shaderlib.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2R_LOG(...) void_log("[s2r] " __VA_ARGS__)

enum { S2R_BACKEND_NONE = 0, S2R_BACKEND_GLES = 1, S2R_BACKEND_VULKAN = 2 };
/* The launcher's choice (GameActivity.nativeRenderer, s2r_core.c); NONE: the old GLES layer. */
int s2r_selected_backend(void);
const char *s2r_backend_name(int backend);
/* A native backend runs this session (decided on the render thread at the first device). */
int s2r_on(void);
uint64_t s2r_now_ns(void);

/* ------------------------------------------------------------ images */
enum { S2R_TEX_2D = 0, S2R_TEX_CUBE = 1, S2R_TEX_3D = 2 };
/* Component swizzle: what each of R,G,B,A of a shader read takes. */
enum { S2R_SW_R = 0, S2R_SW_G, S2R_SW_B, S2R_SW_A, S2R_SW_ZERO, S2R_SW_ONE };

/* What a backend stores (s2r_format.c maps a D3D format to one). */
enum s2r_gpu_format {
    S2R_GPU_NONE = 0,
    S2R_GPU_RGBA8, S2R_GPU_RG8, S2R_GPU_R8, S2R_GPU_RG8_SNORM, S2R_GPU_RGBA8_SNORM,
    S2R_GPU_RGB565, S2R_GPU_RGBA4, S2R_GPU_RGB5A1, S2R_GPU_RGB10A2,
    S2R_GPU_R16F, S2R_GPU_RG16F, S2R_GPU_RGBA16F, S2R_GPU_R32F, S2R_GPU_RG32F, S2R_GPU_RGBA32F,
    S2R_GPU_D16, S2R_GPU_D24S8, S2R_GPU_D32F, S2R_GPU_D24X8,
    S2R_GPU_ETC2_RGB, S2R_GPU_ETC2_RGBA, S2R_GPU_ASTC_4x4, S2R_GPU_ASTC_5x5, S2R_GPU_ASTC_6x6,
    S2R_GPU_BC1, S2R_GPU_BC2, S2R_GPU_BC3,
    S2R_GPU_COUNT
};

/* A GPU image: a texture's storage, a standalone surface's, or the back buffer. */
typedef struct s2r_image {
    int kind;                         /* S2R_TEX_* */
    uint32_t width, height, depth, levels, layers;
    uint32_t d3dfmt;
    const d3d_format *fmt;            /* the D3D format (android/host/d3d9_format.c) */
    uint32_t native_internal;         /* converted blocks (the launcher's ETC2/ASTC): their GL format, else 0 */
    uint8_t gpu;                      /* s2r_gpu_format */
    uint8_t swizzle[4];               /* S2R_SW_* of a shader read */
    int render_target, depth_stencil, has_stencil;
    int compressed_dxt;               /* DXT blocks stored as BC (the device samples them) */
    uint32_t samples;
    int autogen;                      /* levels 1.. are made from level 0 */
    int is_backbuffer;
    /* The back buffer and the targets of its size follow the render scale (d3d_scaled_*):
       `scale` stored pixels per Direct3D pixel (width/height are the stored size), `screen_sized`
       such a target, `d3d_width`/`d3d_height` its size as the game knows it. */
    float scale;
    int screen_sized;
    uint32_t d3d_width, d3d_height;
    uint64_t bytes;
    void *backend;
} s2r_image;

/* s2r_format.c */
int s2r_image_describe(s2r_image *img, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t d3dfmt,
    uint32_t usage, uint32_t native_internal);
uint32_t s2r_mip_levels(uint32_t w, uint32_t h, uint32_t d);
void s2r_level_size(const s2r_image *img, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *d);
/* Bytes per row (or block row) of `w` texels as the backend stores them, and the row count. */
uint32_t s2r_gpu_pitch(const s2r_image *img, uint32_t w);
uint32_t s2r_gpu_rows(const s2r_image *img, uint32_t h);
int s2r_gpu_block(uint8_t gpu, uint32_t *bw, uint32_t *bh, uint32_t *bytes);   /* compressed formats */
uint32_t s2r_gpu_texel_bytes(uint8_t gpu);
const char *s2r_gpu_name(uint8_t gpu);
/* What the running backend samples natively: BC1-3 (DXT) per format (S2R_DXT* bits: the launcher's
   converted cache then serves only the formats missing here), ETC2, ASTC LDR. */
enum { S2R_DXT1 = 1, S2R_DXT3 = 2, S2R_DXT5 = 4, S2R_DXT_ALL = 7 };
void s2r_set_native_support(int dxt, int etc2, int astc);
int s2r_bc_supported(void);        /* the S2R_DXT* mask */
const char *s2r_dxt_names(int mask);   /* "yes" (all three), "no", else "DXT1+DXT5 only" and the like */
int s2r_etc2_supported(void);
int s2r_astc_supported(void);

/* --------------------------------------------- backend objects of the front end */
typedef struct s2r_buffer {           /* of a d3d_buffer */
    int index;
    uint32_t size, usage, format;
    uint8_t *mirror;                  /* the render thread's copy of the contents, made when a backend needs it (Vulkan:
                                         the first in-place write while a draw in flight reads it); NULL until then */
    void *backend;
} s2r_buffer;

typedef struct s2r_query {            /* of a d3d_query */
    uint32_t type;
    int issued, ended;
    void *backend;
} s2r_query;

typedef struct s2r_shader_info {      /* of a d3d_shader */
    const s2r_lib_shader *lib;        /* the entry without sampler types (all 2D); NULL: none */
    const s2r_lib_shader *variants[8];   /* ps_1_x: entries by sampler-type map, found on demand */
    uint32_t variant_types[8];
    int variant_count;
    int failed;                       /* logged once */
} s2r_shader_info;

/* ------------------------------------------------------- draw packet */
typedef struct s2r_ff_key {           /* fixed-function program identity (s2r_ff.c), compared bytewise */
    uint8_t vs[64];
    uint8_t ps[96];
    uint32_t vs_hash, ps_hash;        /* 0 when that half is a shader */
} s2r_ff_key;

typedef struct s2r_target {           /* an attachment: an image, its face and level */
    s2r_image *img;
    uint32_t face, level;
} s2r_target;

/* Render scale: a Direct3D pixel coordinate in the stored pixels of an image of scale k
 * (d3d9_gl.c scaled_px; edges round alike, so neighbouring rects keep no cracks). */
static inline int32_t s2r_scaled(int32_t v, float k) { return k == 1.0f || k <= 0.0f ? v : (int32_t)((float)v * k + 0.5f); }
static inline float s2r_scale_of(const s2r_image *img) { return img && img->scale > 0.0f ? img->scale : 1.0f; }

typedef struct s2r_draw {
    uint32_t primitive;               /* D3DPRIMITIVETYPE */
    int indexed;
    uint32_t start;                   /* first vertex or first index */
    uint32_t count;                   /* vertices or indices */
    int32_t base_vertex;
    uint32_t min_index, num_vertices;
    uint32_t instances;               /* 0: not instanced */
    const s2r_lib_shader *vs, *ps;    /* NULL: fixed function for that stage */
    s2r_ff_key ff;                    /* valid for the fixed-function stages */
    int alpha_func;                   /* 0 none, else D3DCMP_* applied in the pixel stage */
    float alpha_ref;
    int fog;                          /* S2R_FOG_* formula of the pixel half */
    int fog_depth;                    /* the game vertex shader writes its clip z as the fog coordinate */
    float fog_params[4], fog_color[4];
    /* DrawPrimitiveUP / DrawIndexedPrimitiveUP: host copies of the data */
    const void *up_vertices, *up_indices;
    uint32_t up_stride, up_vertex_bytes, up_index_bytes;
    int up_index32;
    int ui;                           /* the game draws its interface (UI FSR 1 filters its 2D reads) */
} s2r_draw;

/* ----------------------------------------------------------- backend */
enum { S2R_WRITE_PLAIN = 0, S2R_WRITE_DISCARD = 1, S2R_WRITE_NOOVERWRITE = 2 };

typedef struct s2r_device s2r_device;

typedef struct s2r_backend {
    const char *name;
    int (*init)(s2r_device *d);                       /* creates the context; waits for the window */
    void (*shutdown)(s2r_device *d);
    int (*image_create)(s2r_device *d, s2r_image *img);
    void (*image_destroy)(s2r_device *d, s2r_image *img);
    /* `data`: GPU-layout rows (d3d_format_convert, converted blocks) of a w x h x depth box. */
    void (*image_upload)(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, uint32_t x, uint32_t y,
        uint32_t w, uint32_t h, uint32_t depth, const void *data, uint32_t pitch, uint32_t slice);
    void (*image_mips)(s2r_device *d, s2r_image *img);
    /* A rect (x0,y0,x1,y1) of a level in the GPU layout into `out` rows of `pitch`; 0 when it cannot. */
    int (*image_read)(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, const uint32_t *rect, void *out,
        uint32_t pitch);
    void *(*buffer_create)(s2r_device *d, s2r_buffer *b);
    void (*buffer_write)(s2r_device *d, s2r_buffer *b, uint32_t offset, uint32_t size, const void *data, int mode);
    void (*buffer_destroy)(s2r_device *d, s2r_buffer *b);
    /* Clear of the bound targets: rects in render-target pixels (count 0: the viewport and scissor). */
    void (*clear)(s2r_device *d, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z,
        uint32_t stencil);
    void (*draw)(s2r_device *d, const s2r_draw *draw);
    /* StretchRect: rects (x0,y0,x1,y1); `linear` D3DTEXF_LINEAR. */
    int (*blit)(s2r_device *d, const s2r_target *src, const uint32_t *sr, const s2r_target *dst, const uint32_t *dr,
        int linear);
    void (*fill)(s2r_device *d, const s2r_target *dst, const uint32_t *rect, uint32_t color);
    void (*present)(s2r_device *d);
    void *(*query_create)(s2r_device *d, uint32_t type);
    void (*query_issue)(s2r_device *d, s2r_query *q, uint32_t flags);
    /* 1 with *value when the answer is there (occlusion: visible samples, event: 1); `flush` D3DGETDATA_FLUSH. */
    int (*query_result)(s2r_device *d, s2r_query *q, uint32_t *value, int flush);
    void (*query_destroy)(s2r_device *d, s2r_query *q);
    void (*backbuffer)(s2r_device *d);                /* the back buffer image changed (CreateDevice, Reset) */
    void (*finish)(s2r_device *d);                    /* everything recorded so far has completed on the GPU */
} s2r_backend;

extern const s2r_backend s2r_gles_backend;
extern const s2r_backend s2r_vulkan_backend;
/* The Vulkan driver (s2r_vkdriver.c; S2R_VK_* of s2r_vkdriver.h): the launcher's choice, the one running. */
enum { S2R_VK_SYSTEM = 0, S2R_VK_TURNIP = 1, S2R_VK_PANVK = 2 };
void s2r_set_vk_driver(int kind, const char *native_dir);
void s2r_set_vk_kind(int kind);
int s2r_vk_driver(void);           /* the chosen one */
int s2r_vk_active_driver(void);    /* the one running (the chosen one once the device exists; never another) */

/* The launcher's renderer failure policy (launcher/native/replacements/renderer_failure.h, owner's decision
   of 24 November 2026): a renderer that cannot start, or loses its GPU device, stops the game; no other
   renderer or driver takes over. s2r_fail_note keeps the first cause seen while a backend starts (printf
   format), which s2r_init reports when init fails. s2r_renderer_lost reports VK_ERROR_DEVICE_LOST /
   EGL_CONTEXT_LOST, s2r_renderer_failed any failure; both record it for the launcher, hand it to
   GameActivity (Launch.rendererFailed) and end the process. s2r_renderer_presented: a frame reached the
   display (the first one starts the launcher's failure window). */
void s2r_fail_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
void s2r_renderer_failed(const char *reason, int device_lost) __attribute__((noreturn));
void s2r_renderer_lost(const char *what);
void s2r_renderer_presented(void);
int s2r_renderer_has_presented(void);

/* ------------------------------------------------------------ device */
/* The render thread's native renderer: one for the session; `dev` follows the
 * device the game draws with (it creates a probe device first). */
struct s2r_device {
    d3d_device *dev;                  /* the render thread's copy of the Direct3D device (d3d_rt_device()) */
    const s2r_backend *be;
    int backend_kind;
    uint32_t frame;
    /* the present's gamma table (Sims 2 sets no ramp: identity, the backends skip it) */
    uint8_t gamma_lut[1024];
    int gamma_identity;
    uint32_t gamma_serial;
    /* frame statistics */
    uint32_t frame_draws, frame_prims, frame_streamed;
    uint32_t frame_passes, frame_loads;    /* backend render passes begun, attachments they loaded */
    uint64_t query_wait_ns;                /* time the render thread waited for query answers */
    uint64_t gpu_ns, gpu_frames;           /* GPU time the backend measured (GL timer queries, Vulkan timestamps) */
    uint64_t upload_bytes;            /* all of them; and of those: */
    uint64_t upload_texture, upload_stream;   /* texture levels, draws' vertices and indices */
    uint64_t program_links, program_link_ns;  /* programs the backend compiled and linked (not from a cache) */
    uint64_t async_links, async_link_ns;      /* of them, by background linkers (totals, read at present) */
    uint32_t frame_skipped;                   /* draws left out of this frame: their program was still linking */
    double present_ms;                /* the backend's last present call */
};

extern s2r_device *s2r_dev;

/* s2r_core.c: the front end's objects as backend objects (render thread) */
s2r_image *s2r_texture_image(d3d_texture *t);          /* NULL: no storage (yet) */
s2r_image *s2r_surface_image(d3d_surface *s);          /* a standalone surface's own image, NULL for a level */
int s2r_surface_target(d3d_surface *s, s2r_target *out);
s2r_image *s2r_backbuffer_image(s2r_device *d);
s2r_buffer *s2r_buffer_of(d3d_buffer *b);
s2r_shader_info *s2r_shader_of(d3d_shader *s);
void s2r_texture_generate_mips(d3d_texture *t);

/* s2r_texcache.c: a DXT level (or block-aligned rect) of `t` from the launcher's converted textures;
 * 1 when it went up here, 0 leaves it to the decode (the texture's image may have been created again). */
int s2r_texcache_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch);

/* s2r_draw.c: the draw packet from the device state */
void s2r_draw_primitive(s2r_device *d, uint32_t type, uint32_t start, uint32_t vertex_count, uint32_t prims, int indexed,
    int32_t base_vertex, uint32_t min_index, const void *up_vertices, uint32_t up_stride, const void *up_indices,
    int up_index32);
uint32_t s2r_prim_vertices(uint32_t type, uint32_t prims);
/* The image bound to a sampler stage (NULL: none) and its texture kind. */
s2r_image *s2r_stage_image(s2r_device *d, uint32_t stage);
uint32_t s2r_stage_first_level(s2r_device *d, uint32_t stage, uint32_t levels);
/* The bound colour targets (rt[0..3], NULL entries skipped) and depth. */
int s2r_targets(s2r_device *d, s2r_target color[4], s2r_target *depth);
/* Constants of a game shader packed for its block (std140 vec4s); returns bytes. */
uint32_t s2r_pack_constants(const s2r_lib_shader *s, const d3d_state *st, uint8_t *out, uint32_t cap);
/* Vertex input decoding (s2r_spec.h S2R_DECODE_*) of an element type. */
uint8_t s2r_decode_mode(uint8_t type, int vulkan);
/* The declaration element a vertex input location reads (game shader or FF half); NULL: none. */
const d3d_vertex_element *s2r_input_element(s2r_device *d, const s2r_lib_shader *vs, uint32_t loc);
uint64_t s2r_decode_modes(s2r_device *d, const s2r_lib_shader *vs, int vulkan, uint8_t modes[16]);
/* The vertex-shader input locations of the fixed-function half (ff.vert): 0, 3, 4, 5, 6..13 */
#define S2R_FF_INPUTS 0x3FF9u
int s2r_ff_input_location(uint8_t usage, uint8_t usage_index);
/* GLSL for OpenGL ES: version, the variant #defines, the body. malloc'ed. */
char *s2r_gles_program_text(const char *body, const char *defines);
void s2r_shader_defines(char *out, size_t cap, int pixel, const uint8_t modes[16], int alpha_func, int fog, int fog_depth);
extern int s2r_trace_frame;

/* s2r_ff.c: fixed function as the constants of shaders/ff.vert and ff.frag */
void s2r_ff_key_build(s2r_device *d, const s2r_draw *draw, s2r_ff_key *key, int need_vs, int need_ps, uint32_t fog_coord);
int s2r_ff_spec(const s2r_ff_key *key, int pixel, uint32_t *ids, int32_t *values, int cap);
int s2r_ff_valid(const s2r_ff_key *key, int pixel);
uint32_t s2r_ff_sampler_mask(const s2r_ff_key *key);
void s2r_ff_defines(const s2r_ff_key *key, int pixel, char *out, size_t cap);
typedef struct s2r_ff_uniforms {
    float world_view[16];             /* column-major for GLSL: world * view, then projection */
    float proj[16];
    float normal[12];                 /* inverse transpose of world*view (3 vec4 rows) */
    float view[16];
    float texm[8][16];
    float viewport[4];                /* x, y, w, h of the D3D viewport (for XYZRHW) */
    float material_diffuse[4], material_ambient[4], material_specular[4], material_emissive[4];
    float material_power[4];
    float global_ambient[4];
    float light_diffuse[8][4], light_specular[8][4], light_ambient[8][4];
    float light_position[8][4], light_direction[8][4];
    float light_atten[8][4];          /* att0, att1, att2, range */
    float light_spot[8][4];           /* cos(theta/2), cos(phi/2), falloff, type */
    float fog[4];
    float fog_color[4];
    float tfactor[4];
    float bumpenv[8][4];
    float alpha_ref[4];
} s2r_ff_uniforms;
void s2r_ff_uniforms_fill(s2r_device *d, const s2r_draw *draw, s2r_ff_uniforms *u);

/* The port uniform block of every program (binding 3): flip and half pixel,
 * alpha reference, fog colour, VPOS fix, fog parameters. */
typedef struct s2r_port_block { float flip[4], alpha[4], fog_color[4], vpos[4], fog[4]; } s2r_port_block;
/* port_vpos: xy MojoShader's vposFlip, z Direct3D pixels per stored pixel (VPOS under the render
   scale), w the UI filter (3: EASU, -1: none). */
void s2r_port_block_fill(s2r_device *d, const s2r_draw *draw, float target_scale, uint32_t target_h, s2r_port_block *p);

/* s2r_filter.c: the present's scene filters and UI FSR 1 as GLSL for both backends */
enum { S2R_FILTER_EASU, S2R_FILTER_RCAS, S2R_FILTER_FXAA, S2R_FILTER_LINEAR, S2R_FILTER_COPY, S2R_FILTER_COUNT };
const char *s2r_filter_vertex(int vulkan);
char *s2r_filter_fragment(int which, int vulkan);          /* malloc'ed */
const char *s2r_filter_name(int which);
/* The passes the launcher's choices ask for at the present (into output-size images). */
int s2r_filter_chain(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, int passes[3]);
/* A pixel shader with its 2D reads through EASU in interface draws (malloc'ed; NULL: unchanged). */
char *s2r_ui_filter_source(const char *src);
/* The launcher's UI FSR 1 and MSAA (sims2_settings.c), for the backends. */
int s2r_ui_fsr(void);
int s2r_msaa_samples(void);
/* The render scale's targets: the stored size changed (contents lost, as after a reset). */
void s2r_scale_follow(s2r_device *d);

/* s2r_present.c: pacing and statistics shared by the backends */
void s2r_frame_presented(s2r_device *d, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns);
/* The Frame Rate Limit (sims2_settings.c): waits for the frame's slot before the swap (d3d9_gl.c pace_frame). */
void s2r_pace(void);
/* The builtin shaders (s2r_shaderlib.h S2R_BUILTIN_*): GLSL ES body, SPIR-V words; NULL when missing. */
const char *s2r_builtin_gles(int which);
const uint32_t *s2r_builtin_spirv(int which, uint32_t *words);
void s2r_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
/* The app cache folder (Vulkan pipeline cache, SPIR-V of device translations). */
const char *s2r_cache_dir(void);

#ifdef __cplusplus
}
#endif
#endif
