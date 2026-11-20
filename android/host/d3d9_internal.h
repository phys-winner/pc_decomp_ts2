/* Direct3D 9 on OpenGL ES 3.2: shared definitions.
 *
 * Orientation: every render target, the back buffer included, is a GL
 * texture rendered with Y flipped, so its rows are stored top-down exactly as
 * D3D stores them. Viewports, scissors, ReadPixels rows and texture uploads
 * then use D3D coordinates unchanged, and sampling a render target sees the
 * same image D3D would. Only the final present flips to the window. */
#ifndef VOID_D3D9_INTERNAL_H
#define VOID_D3D9_INTERNAL_H

#include "com.h"
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include "../../reference/mojoshader/mojoshader.h"
#include "../render/s2r.h"   /* the native renderers under this front end (android/render) */

/* Interface IDs (d3d9.h), checked against the system d3d9.dll. */
static const uint8_t IID_IUnknown[16] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};   /* {00000000-0000-0000-C000-000000000046} */
static const uint8_t IID_IDirect3D9[16] = {0xCA,0xCB,0xBD,0x81,0xD4,0x64,0x6D,0x42,0xAE,0x8D,0xAD,0x01,0x47,0xF4,0x27,0x5C};   /* {81BDCBCA-64D4-426D-AE8D-AD0147F4275C} */
static const uint8_t IID_IDirect3DDevice9[16] = {0x96,0x3B,0x22,0xD0,0x7A,0xBF,0xFD,0x43,0x92,0xBD,0xA4,0x3B,0x0D,0x82,0xB9,0xEB};   /* {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB} */
static const uint8_t IID_IDirect3DResource9[16] = {0x5D,0xC0,0xEE,0x05,0x7D,0x8F,0x62,0x43,0xB9,0x99,0xD1,0xBA,0xF3,0x57,0xC7,0x04};   /* {05EEC05D-8F7D-4362-B999-D1BAF357C704} */
static const uint8_t IID_IDirect3DBaseTexture9[16] = {0x7E,0xA8,0x0C,0x58,0x3C,0x1D,0x54,0x4D,0x99,0x1D,0xB7,0xD3,0xE3,0xC2,0x98,0xCE};   /* {580CA87E-1D3C-4D54-991D-B7D3E3C298CE} */
static const uint8_t IID_IDirect3DTexture9[16] = {0x27,0x12,0xC3,0x85,0xE5,0x3D,0x00,0x4F,0x9B,0x3A,0xF1,0x1A,0xC3,0x8C,0x18,0xB5};   /* {85C31227-3DE5-4F00-9B3A-F11AC38C18B5} */
static const uint8_t IID_IDirect3DCubeTexture9[16] = {0x81,0x2F,0xF3,0xFF,0x53,0xD9,0x3A,0x47,0x92,0x23,0x93,0xD6,0x52,0xAB,0xA9,0x3F};   /* {FFF32F81-D953-473A-9223-93D652ABA93F} */
static const uint8_t IID_IDirect3DVolumeTexture9[16] = {0x6C,0x52,0x18,0x25,0x89,0xE7,0x11,0x41,0xA7,0xB9,0x47,0xEF,0x32,0x8D,0x13,0xE6};   /* {2518526C-E789-4111-A7B9-47EF328D13E6} */
static const uint8_t IID_IDirect3DSurface9[16] = {0x3A,0xAF,0xFB,0x0C,0xF6,0x9F,0x9A,0x42,0x99,0xB3,0xA2,0x79,0x6A,0xF8,0xB8,0x9B};   /* {0CFBAF3A-9FF6-429A-99B3-A2796AF8B89B} */
static const uint8_t IID_IDirect3DVolume9[16] = {0xE6,0x16,0xF4,0x24,0x67,0x1F,0xA7,0x4A,0xB8,0x8E,0xD3,0x3F,0x6F,0x31,0x28,0xA1};   /* {24F416E6-1F67-4AA7-B88E-D33F6F3128A1} */
static const uint8_t IID_IDirect3DVertexBuffer9[16] = {0xB5,0xB1,0x4B,0xB6,0x70,0xFD,0xF6,0x4D,0xBF,0x91,0x19,0xD0,0xA1,0x24,0x55,0xE3};   /* {B64BB1B5-FD70-4DF6-BF91-19D0A12455E3} */
static const uint8_t IID_IDirect3DIndexBuffer9[16] = {0x5E,0xD6,0x9D,0x7C,0xF7,0xD3,0x29,0x45,0xAC,0xEE,0x78,0x58,0x30,0xAC,0xDE,0x35};   /* {7C9DD65E-D3F7-4529-ACEE-785830ACDE35} */
static const uint8_t IID_IDirect3DSwapChain9[16] = {0xF2,0x50,0x49,0x79,0xFC,0xAD,0x8A,0x45,0x90,0x5E,0x10,0xA1,0x0B,0x0B,0x50,0x3B};   /* {794950F2-ADFC-458A-905E-10A10B0B503B} */
static const uint8_t IID_IDirect3DQuery9[16] = {0x60,0x14,0x77,0xD9,0x95,0xA6,0x26,0x4F,0xBB,0xD3,0x27,0xB8,0x40,0xB5,0x41,0xCC};   /* {D9771460-A695-4F26-BBD3-27B840B541CC} */
static const uint8_t IID_IDirect3DStateBlock9[16] = {0xE5,0x4F,0x7C,0xB0,0x0D,0x31,0xA8,0x4B,0xA2,0x3C,0x4F,0x0F,0x20,0x6F,0x21,0x8B};   /* {B07C4FE5-310D-4BA8-A23C-4F0F206F218B} */
static const uint8_t IID_IDirect3DVertexShader9[16] = {0x7E,0x55,0xC5,0xEF,0x65,0x62,0x13,0x46,0x8A,0x94,0x43,0x85,0x78,0x89,0xEB,0x36};   /* {EFC5557E-6265-4613-8A94-43857889EB36} */
static const uint8_t IID_IDirect3DPixelShader9[16] = {0xDC,0xDB,0x3B,0x6D,0x02,0x5B,0x15,0x44,0xB8,0x52,0xCE,0x5E,0x8B,0xCC,0xB2,0x89};   /* {6D3BDBDC-5B02-4415-B852-CE5E8BCCB289} */
static const uint8_t IID_IDirect3DVertexDeclaration9[16] = {0x9C,0xC5,0x13,0xDD,0xFA,0x36,0x98,0x40,0xA8,0xFB,0xC7,0xED,0x39,0xDC,0x85,0x46};   /* {DD13C59C-36FA-4098-A8FB-C7ED39DC8546} */

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_MOREDATA 0x88760867u
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define D3DERR_WASSTILLDRAWING 0x8876021Cu
#define D3DOK_NOAUTOGEN 0x0876086Fu
#define S_FALSE_D3D 1u

/* D3DRESOURCETYPE */
enum { D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME, D3DRTYPE_TEXTURE, D3DRTYPE_VOLUMETEXTURE,
       D3DRTYPE_CUBETEXTURE, D3DRTYPE_VERTEXBUFFER, D3DRTYPE_INDEXBUFFER };

/* D3DUSAGE */
#define D3DUSAGE_RENDERTARGET 0x1u
#define D3DUSAGE_DEPTHSTENCIL 0x2u
#define D3DUSAGE_WRITEONLY 0x8u
#define D3DUSAGE_DYNAMIC 0x200u
#define D3DUSAGE_AUTOGENMIPMAP 0x400u

/* D3DPOOL */
enum { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3 };

/* D3DLOCK */
#define D3DLOCK_READONLY 0x10u
#define D3DLOCK_DISCARD 0x2000u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DLOCK_NOSYSLOCK 0x800u
#define D3DLOCK_DONOTWAIT 0x4000u
#define D3DLOCK_NO_DIRTY_UPDATE 0x8000u

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Format conversions applied on upload (guest layout -> GL layout). */
enum {
    CONV_NONE,          /* bytes upload unchanged */
    CONV_DXT1, CONV_DXT3, CONV_DXT5, CONV_ATI1, CONV_ATI2,  /* decoded to RGBA8 / R8 / RG8 */
    CONV_X1R5G5B5,      /* rotate to RGB5_A1 with alpha set */
    CONV_A1R5G5B5,
    CONV_A4R4G4B4,      /* rotate to RGBA4 */
    CONV_L16,           /* to R32F */
    CONV_G16R16,        /* to RG32F */
    CONV_A16B16G16R16,  /* to RGBA32F */
    CONV_BGRA_TO_RGBA,  /* byte swap for render-target textures that cannot swizzle */
    CONV_V8U8,          /* signed RG8 */
    CONV_Q8W8V8U8,
};

typedef struct d3d_format {
    uint32_t d3d;
    uint32_t bits;              /* bits per texel in guest layout (block formats: per texel) */
    uint32_t block_bytes;       /* bytes per 4x4 block, 0 for linear formats */
    GLenum internal, format, type;
    int conv;
    GLint swizzle[4];           /* sampling swizzle (GL_RED.. / GL_ZERO / GL_ONE) */
    int depth, stencil;
    int render_target;          /* usable as colour attachment */
    int is_float;
    uint32_t gl_bits;           /* bits per texel after conversion */
} d3d_format;

const d3d_format *d3d_format_find(uint32_t d3d);
/* Guest pitch of one row (or block row) of `width` texels. */
uint32_t d3d_format_pitch(const d3d_format *f, uint32_t width);
uint32_t d3d_format_rows(const d3d_format *f, uint32_t height);
/* Convert `rows` guest rows to GL layout. Returns a malloc'ed buffer (or src
 * itself when no conversion is needed) and the GL byte size. */
const void *d3d_format_convert(const d3d_format *f, const uint8_t *src, uint32_t width, uint32_t height,
    uint32_t pitch, void **owned, size_t *gl_size, int render_target);

typedef struct d3d_device d3d_device;
typedef struct d3d_texture d3d_texture;
typedef struct d3d_surface d3d_surface;

/* d3d9_texcache.c: 1 when a DXT level went up from the converted texture cache (the old layer; the
 * native renderers: s2r_texcache.c). */
int d3d_texcache_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch);

/* A surface: a standalone render target / depth buffer / offscreen plain
 * surface, or one level (and face) of a texture. */
struct d3d_surface {
    void_com_object com;
    d3d_device *dev;
    uint32_t width, height, format, usage, pool, multisample;
    const d3d_format *fmt;
    d3d_texture *container;       /* owning texture (NULL for standalone) */
    uint32_t face, level;
    GLuint gl;                    /* standalone surfaces: own GL texture */
    GLuint gl_msaa_depth;         /* persistent screen depth/stencil for optional MSAA */
    uint32_t gl_msaa_samples;
    float scale;                  /* standalone: GL storage per D3D pixel (render scale; 1 elsewhere) */
    int screen_sized;             /* follows the render scale (d3d_scaled_*) */
    uint32_t sysmem;              /* SYSTEMMEM/SCRATCH: persistent guest storage */
    uint32_t pitch;
    /* Lock state */
    int locked;
    uint32_t lock_mem, lock_flags;
    uint32_t lock_rect[4];
    int lock_whole;
    uint64_t gl_bytes;            /* estimated GL storage ([mem] report); render thread */
    void *s2r;                    /* the native renderer's image (standalone surfaces); render thread */
};

struct d3d_texture {
    void_com_object com;
    d3d_device *dev;
    uint32_t rtype;               /* D3DRTYPE_TEXTURE / CUBETEXTURE / VOLUMETEXTURE */
    uint32_t width, height, depth, levels, usage, format, pool;
    const d3d_format *fmt;
    GLuint gl;
    GLenum target;
    uint32_t lod;
    uint32_t autogen_filter;
    int faces;
    d3d_surface **surfaces;       /* faces * levels, created on demand */
    uint32_t *sysmem;             /* SYSTEMMEM/SCRATCH textures: per surface guest storage */
    int mips_dirty;
    /* Sampler parameters currently applied to the GL texture object. */
    uint32_t applied_base_level;
    uint32_t hud_tag;             /* a HUD image the touch layer reacts to (hud.h) */
    float scale;                  /* GL storage per D3D pixel (render scale; 1 elsewhere) */
    int screen_sized;             /* follows the render scale (d3d_scaled_*) */
    int has_storage;              /* GL storage was asked for (the game's thread's view of `gl`) */
    uint32_t gl_lod;              /* SetLOD as the render thread applies it */
    GLenum gl_internal;           /* storage format other than fmt->internal (converted or ATC blocks); render thread */
    int cache_state;              /* D3D_TEXCACHE_* (d3d9_texcache.h): undecided, RGBA8, converted, ATC; render thread */
    uint64_t gl_bytes;            /* estimated GL storage ([mem] report); render thread */
    void *s2r;                    /* the native renderer's image; render thread */
};

typedef struct d3d_buffer {
    void_com_object com;
    d3d_device *dev;
    int index;                    /* index buffer */
    uint32_t size, usage, pool, format;
    GLuint gl;
    uint32_t shadow;              /* guest copy, what Lock returns */
    struct d3d_buffer_storage *storage; /* render-thread persistent stores, when supported */
    int locks;
    uint32_t dirty_lo, dirty_hi;
    int discard_pending;
    int overwrite_pending;        /* a lock without D3DLOCK_NOOVERWRITE since the last flush */
    void *s2r;                    /* the native renderer's buffer; render thread */
} d3d_buffer;

typedef struct d3d_vertex_element {
    uint16_t stream, offset;
    uint8_t type, method, usage, usage_index;
} d3d_vertex_element;

typedef struct d3d_decl {
    void_com_object com;
    d3d_device *dev;
    int count;
    d3d_vertex_element elements[64];
    uint32_t fvf;
} d3d_decl;

#define D3D_SOFTWARE_VS_CONSTANTS 8192u
#define D3D_SOFTWARE_VS_INT_CONSTANTS 2048u
#define D3D_SOFTWARE_VS_BOOL_CONSTANTS 2048u

typedef struct d3d_shader {
    void_com_object com;
    d3d_device *dev;
    int pixel;
    uint8_t *bytecode;
    uint32_t size;
    const MOJOSHADER_parseData *parse;
    uint8_t *parse_code;          /* bytecode MojoShader parses, if patched */
    uint32_t parse_size;
    GLuint gl_plain;              /* compiled without variants */
    char *source;                 /* patched GLSL */
    void *process;                /* raw software vertex programs (GL thread) */
    uint32_t sampler_types;       /* 2 bits per sampler from the bytecode */
    int ff_io_ready;              /* ff_io holds the io_ varyings of `source` (d3d9_ff.c) */
    uint32_t ff_io_count;
    uint32_t ff_io[32];           /* usage << 8 | index: what a generated other half must match */
    void *s2r;                    /* the native renderer's library entries; render thread */
} d3d_shader;

typedef struct d3d_query {
    void_com_object com;
    d3d_device *dev;
    uint32_t type;
    GLuint gl;
    int issued, ended;
    GLsync fence;
    void *s2r;                    /* the native renderer's query; render thread */
} d3d_query;

/* Everything a state block can capture. */
typedef struct d3d_state {
    uint32_t rs[256];
    uint32_t ss[20][14];          /* samplers 0-15 and vertex samplers (D3DDMAPSAMPLER, 257-260) */
    uint32_t tss[8][33];
    float transforms[512][16];    /* D3DTS_VIEW(2), PROJECTION(3), TEXTUREn(16-23), WORLDn(256+n) */
    float vs_f[D3D_SOFTWARE_VS_CONSTANTS][4];
    int32_t vs_i[D3D_SOFTWARE_VS_INT_CONSTANTS][4];
    int32_t vs_b[D3D_SOFTWARE_VS_BOOL_CONSTANTS];
    float ps_f[224][4];
    int32_t ps_i[16][4];
    int32_t ps_b[16];
    uint32_t viewport[4];
    float viewport_z[2];
    uint32_t scissor[4];
    void *textures[20];
    void *streams[16];
    uint32_t stream_offset[16], stream_stride[16], stream_freq[16];
    void *indices;
    void *decl;
    uint32_t fvf;
    void *vs, *ps;
    float material[17];
    float lights[8][26];
    int light_enable[8];
    float clip_planes[6][4];
} d3d_state;

typedef struct d3d_stateblock {
    void_com_object com;
    d3d_device *dev;
    d3d_state state;
    /* What the block records: a byte per category item. */
    uint8_t rec_rs[256], rec_ss[20][14], rec_tss[8][33], rec_tex[20], rec_stream[16];
    uint8_t rec_vs_f[D3D_SOFTWARE_VS_CONSTANTS], rec_ps_f[224], rec_vs_i[D3D_SOFTWARE_VS_INT_CONSTANTS], rec_ps_i[16], rec_vs_b[D3D_SOFTWARE_VS_BOOL_CONSTANTS], rec_ps_b[16];
    uint8_t rec_transform[512];
    int rec_viewport, rec_scissor, rec_indices, rec_decl, rec_fvf, rec_vs, rec_ps, rec_material;
    uint8_t rec_light[8], rec_clip[6];
} d3d_stateblock;

struct d3d_device {
    void_com_object com;
    int ui_draw;
    float projection[3];
    uint32_t d3d9;                /* parent IDirect3D9 guest object */
    uint32_t window, behavior;
    uint32_t pp[14];              /* D3DPRESENT_PARAMETERS */
    d3d_surface *backbuffer;
    d3d_surface *auto_depth;
    d3d_surface *rt[4];
    d3d_surface *ds;
    d3d_state st;
    d3d_stateblock *recording;
    GLuint fbo, read_fbo, draw_fbo;
    int fbo_dirty;
    int in_scene;
    uint32_t software_vp;
    uint32_t frame;
    /* Shader constants touched since the last draw (for upload). */
    int constants_dirty;
    /* Scratch GL objects for UP draws. */
    GLuint up_vbo, up_ibo;
    GLuint vao;
    /* Applied GL state cache. */
    struct gl_cache *gl;
    uint32_t swapchain;           /* guest IDirect3DSwapChain9 */
    uint32_t vs_sampler_base;     /* first GL unit used for vertex samplers */
    /* 64-byte pieces touched since the render thread's copy last followed. */
    uint32_t state_dirty[(sizeof(d3d_state) + 2047u) / 2048u];
    /* One bit per dirty bitmap word: draws usually touch only a few of the
     * software register file's words, so sync need not visit the rest. */
    uint32_t state_dirty_active[(sizeof(d3d_state) + 65535u) / 65536u];
    /* What each bound slot's object is for the guest (d3d9_device.c bind_guest): the
     * texture, stream, index, declaration and shader slots in that order. */
    struct { void *host; uint32_t guest; } bound[40];
};

static inline void d3d_dirty_range(d3d_device *d, uint32_t offset, uint32_t size)
{
    if (!size) return;
    uint32_t first = offset / 64u, last = (offset + size - 1u) / 64u;
    for (uint32_t word = first >> 5; word <= (last >> 5); ++word) {
        uint32_t lo = word == (first >> 5) ? first & 31u : 0;
        uint32_t hi = word == (last >> 5) ? last & 31u : 31;
        d->state_dirty[word] |= (UINT32_MAX << lo) & (UINT32_MAX >> (31u - hi));
        d->state_dirty_active[word >> 5] |= 1u << (word & 31);
    }
}
#define d3d_dirty_field(d, field) d3d_dirty_range((d), (uint32_t)((uint8_t *)&(d)->st.field - (uint8_t *)&(d)->st), sizeof((d)->st.field))
#define d3d_dirty_transform(d, slot) d3d_dirty_field((d), transforms[slot])
static inline void d3d_dirty_all(d3d_device *d)
{
    memset(d->state_dirty, 0xFF, sizeof d->state_dirty);
    memset(d->state_dirty_active, 0xFF, sizeof d->state_dirty_active);
}

/* d3d9_shader.c: linked program binaries kept in the app's cache folder, keyed
 * by the exact GLSL of both halves (render thread). */
typedef struct { uint64_t a, b; } d3d_program_key;
int d3d_program_cache_on(void);
d3d_program_key d3d_program_key_of(const char *vs_text, const char *ps_text, const MOJOSHADER_parseData *attributes);
GLuint d3d_program_cache_load(d3d_program_key key);
void d3d_program_cache_store(d3d_program_key key, GLuint program);
char *d3d_shader_text(GLuint shader);

/* d3d9_thread.c: the render thread. A call records a function and a copy of
 * its arguments; the thread runs them in order (S2_RENDER_THREAD=0: inline). */
typedef void (*d3d_rt_fn)(void *args);
void d3d_rt_start(void);
int d3d_rt_threaded(void);
int d3d_rt_on_render_thread(void);
void *d3d_rt_begin(d3d_rt_fn fn, uint32_t size);   /* where to write `size` bytes of arguments */
void d3d_rt_commit(void);
void d3d_rt_commit_size(uint32_t used);            /* fewer bytes than begun */
void d3d_rt_call(d3d_rt_fn fn, const void *args, uint32_t size);
void d3d_rt_sync(d3d_rt_fn fn, void *arg);         /* run fn(arg) there and wait for it */
uint32_t d3d_process_vertices(d3d_device *d, uint32_t first, uint32_t dest, uint32_t count, d3d_buffer *buffer, d3d_decl *decl, uint32_t flags);
int d3d_process_inputs(d3d_device *d, const MOJOSHADER_parseData *pd, uint32_t first, uint32_t count, int bind);
void d3d_process_inputs_done(void);
void d3d_rt_finish(void);
void d3d_rt_flush(void);       /* hand over what is recorded now */
int d3d_rt_render_tid(void);   /* 0 until it runs */
void d3d_rt_wait_stats(uint64_t *syncs, uint64_t *waited_ns);
void d3d_rt_wait_begin(void);   /* around GL work that sleeps (vsync, frame limiter) */
void d3d_rt_wait_end(void);
void d3d_rt_frame_queued(void);
void d3d_rt_frame_wait(int ahead);
void d3d_rt_frame_done(void);
/* The render thread's device: the game's state as of the last call that
 * needed it (d3d_rt_sync_state before each). */
d3d_device *d3d_rt_device(void);
void d3d_rt_sync_state(d3d_device *d);
void d3d_rt_device_reset(d3d_device *d);
/* Bumped when shader constants change (d3d9_shader.c re-uploads). */
extern uint32_t d3d_constant_generation;

/* d3d9_device.c */
extern d3d_device *g_d3d_device;
uint32_t d3d_wrap_surface(d3d_surface *s);
d3d_surface *d3d_surface_new(d3d_device *dev, uint32_t w, uint32_t h, uint32_t format, uint32_t usage,
    uint32_t pool, uint32_t multisample);
void d3d_surface_upload(d3d_surface *s);
/* The object holding a surface's reference count: its texture, if it has one. */
void_com_object *d3d_surface_owner(d3d_surface *s);
GLuint d3d_surface_gl_texture(d3d_surface *s, GLenum *target, GLint *level, GLint *layer);
uint32_t d3d_texture_surface(d3d_texture *t, uint32_t face, uint32_t level);
d3d_texture *d3d_texture_new(d3d_device *dev, uint32_t rtype, uint32_t w, uint32_t h, uint32_t d,
    uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool);
uint32_t d3d_wrap_texture(d3d_texture *t);
void d3d_texture_level_size(d3d_texture *t, uint32_t level, uint32_t *w, uint32_t *h);
void d3d_texture_upload_level(d3d_texture *t, uint32_t face, uint32_t level, const uint8_t *guest_data,
    uint32_t pitch);
void d3d_generate_mips(d3d_texture *t);           /* render thread */
void d3d_generate_mips_later(d3d_texture *t);     /* the game's thread: recorded */
/* The game's thread's ways to the render thread's GL work (d3d9_device.c). */
void d3d_present(d3d_device *d);
void d3d_read_surface(d3d_device *d, d3d_surface *src, uint32_t mem, uint32_t pitch);
int d3d_read_rgba(d3d_surface *s, const uint32_t *rect, float *out);
void d3d_blit(d3d_device *d, d3d_surface *src, const uint32_t *sr, d3d_surface *dst, const uint32_t *dr, uint32_t filter);
void d3d_free_later(void_com_object *o);

void d3d_stateblock_capture(d3d_stateblock *sb);
/* Point a slot at a COM object, holding a reference on it (textures, buffers,
 * shaders, declarations: objects that own their count). */
void d3d_ref_set(void **slot, void *object);
void d3d_stateblock_apply(d3d_stateblock *sb);

/* Render scale (sims2_settings.c): the back buffer and every render target
 * or depth surface of its size are stored at d3d_screen_scale() GL pixels per
 * D3D pixel; the D3D side never sees it (sizes, locks, viewports stay in D3D
 * pixels). d3d9_resource.c allocates, d3d9_gl.c scales rectangles. */
float d3d_screen_scale(uint32_t backbuffer_width);
float d3d_scale_of(const d3d_surface *s);
uint32_t d3d_scaled_size(uint32_t size, float scale);
void d3d_scaled_add(d3d_surface *s, d3d_texture *t);
void d3d_scaled_remove(void *object);
/* Every registered target at scale k (storage again, no GL of the old layer): their count, 0 when
   the scale did not change. The native renderers call it after a present (s2r_scale_follow). */
int d3d_scaled_retarget(float k);
float d3d_scaled_applied(void);
void d3d_scaled_follow(d3d_device *dev);   /* at a present: apply a changed scale */
void d3d_texture_storage(d3d_texture *t);
void d3d_surface_storage(d3d_surface *s);
void d3d_gl_upload_scaled(d3d_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, GLenum format, GLenum type,
    const void *data);
void d3d_gl_state_forget(void);

/* d3d9_gl.c */
/* GL implementation limits, filled by d3d_gl_init (0 before). */
typedef struct { uint32_t max_texture, max_3d_texture, max_draw_buffers, max_vs_vectors, max_ps_vectors; } d3d_gl_limits_t;
extern d3d_gl_limits_t d3d_gl_limits;
int d3d_gl_init(void);
void d3d_gl_present(d3d_device *dev, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns);
void d3d_gl_bind_targets(d3d_device *dev);
void d3d_gl_forget_texture(GLuint tex);
void d3d_gl_forget_buffer(GLuint buffer);
void d3d_gl_upload_unit(void);
void d3d_gl_use_program(GLuint program);
void d3d_gl_delete_program(GLuint program);
GLuint d3d_filter_present(GLuint input,int width,int height,int output_width,int output_height);
int d3d_filter_output(GLuint input,int x,int y,int width,int height);
void d3d_gl_apply_state(d3d_device *dev);
int d3d_gl_draw(d3d_device *dev, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count,
    uint32_t prim_count, int indexed, uint32_t base_vertex, uint32_t start_index, uint32_t min_index,
    const void *up_vertices, uint32_t up_stride, const void *up_indices, int up_index32);
void d3d_gl_clear(d3d_device *dev, uint32_t count, const uint32_t *rects, uint32_t flags, uint32_t color, float z,
    uint32_t stencil);
void d3d_gl_blit(d3d_surface *src, const uint32_t *src_rect, d3d_surface *dst, const uint32_t *dst_rect, uint32_t filter);
void d3d_gl_read_surface(d3d_surface *src, uint32_t dst_mem, uint32_t dst_pitch);
void d3d_gl_fill(d3d_surface *dst, const uint32_t *rect, uint32_t color);
void d3d_gl_dump_frame(d3d_device *dev, const char *path);

/* d3d9_shader.c */
d3d_shader *d3d_shader_create(d3d_device *dev, int pixel, uint32_t bytecode);
uint32_t d3d_wrap_shader(d3d_shader *s);
int d3d_shader_bind(d3d_device *dev, d3d_shader *vs, d3d_shader *ps, uint32_t *attribute_mask,
    int *attribute_usage, int *attribute_index, int *attribute_count, int *needs_bgra);
uint32_t d3d_shader_bytecode_size(uint32_t va);
void d3d_ff_forget_program(GLuint program);

/* d3d9_texel.c: logical RGBA texels */
float d3d_half_to_float(uint16_t h);
uint16_t d3d_float_to_half(float f);
void d3d_gl_layout_to_rgba(GLenum format, GLenum type, const void *data, uint32_t count, float *out);
void d3d_swizzle_rgba(const GLint sw[4], float *rgba, uint32_t count);
void d3d_storage_swizzle(const d3d_format *f, uint32_t usage, GLint sw[4]);
int d3d_format_decode(const d3d_format *f, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch, float *rgba);
int d3d_format_encode(uint32_t d3d, const float *rgba, uint32_t count, uint8_t *out);
void d3d_rgba_resample(const float *src, uint32_t w, uint32_t h, float *dst, uint32_t dw, uint32_t dh);
/* d3d9_gl.c / d3d9_resource.c: a rect (x0, y0, x1, y1) of a surface as logical RGBA */
int d3d_gl_read_rgba(d3d_surface *s, const uint32_t *rect, float *out);
int d3d_surface_read_rgba(d3d_surface *s, const uint32_t *rect, float *out);
int d3d_surface_write_rgba(d3d_surface *s, const uint32_t *rect, const float *rgba);
void d3d_texture_upload_rect(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch);

int d3d_trace_active(void);

/* d3d9_ff.c */
int d3d_ff_bind(d3d_device *dev, int *attr_count, int *usage, int *index, uint32_t *stage_mask);

#endif
