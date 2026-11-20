/* The native renderers as the port's Direct3D 9 front end sees them
 * (android/host/d3d9_*.c). Every function runs on the render thread, from the
 * calls the game's thread recorded, while s2r_on() (s2r_core.c); the front
 * end's d3d_gl_* path runs otherwise. */
#ifndef S2R_H
#define S2R_H
#include <stdint.h>

typedef struct d3d_device d3d_device;
typedef struct d3d_texture d3d_texture;
typedef struct d3d_surface d3d_surface;
typedef struct d3d_buffer d3d_buffer;
typedef struct d3d_query d3d_query;
typedef struct d3d_shader d3d_shader;

int s2r_on(void);
/* The device's context on the render thread (CreateDevice); 0: the old GLES layer renders. */
int s2r_init(d3d_device *mirror);
void s2r_set_mode(int backend);                 /* the launcher's choice (JNI), before the game starts */
int s2r_selected_backend(void);

void s2r_texture_storage(d3d_texture *t);
void s2r_texture_release(d3d_texture *t);
void s2r_texture_upload(d3d_texture *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const uint8_t *data, uint32_t pitch);
void s2r_texture_generate_mips(d3d_texture *t);
void s2r_surface_storage(d3d_surface *s);
void s2r_surface_release(d3d_surface *s);
void s2r_surface_upload(d3d_surface *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const uint8_t *data,
    uint32_t pitch);
void s2r_buffer_create(d3d_buffer *b);
void s2r_buffer_upload(d3d_buffer *b, uint32_t lo, uint32_t length, int whole, int unsynchronized, const uint8_t *src);
void s2r_buffer_release(d3d_buffer *b);
void s2r_shader_release(d3d_shader *s);
void s2r_query_create_rt(d3d_query *q);
void s2r_query_issue_rt(d3d_query *q, uint32_t flags);
/* 1 answered (*visible: samples > 0), 0 not yet, -1 failed; `flush`: hand the GPU the work so far. */
int s2r_query_answer(d3d_query *q, int flush, int *visible);
void s2r_query_release(d3d_query *q);
void s2r_flush(void);
/* ProcessVertices (d3d9_shader.c run_process): an OpenGL ES context current for the call, the
   backend's or one of its own (Vulkan); 0 when there is none. */
int s2r_process_begin(void);
void s2r_process_end(void);

void s2r_draw_call(d3d_device *d, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count, uint32_t prim_count,
    int indexed, uint32_t base_vertex, uint32_t start_index, uint32_t min_index, const void *up_vertices,
    uint32_t up_stride, const void *up_indices, int up_index32);
void s2r_clear(d3d_device *d, uint32_t count, const uint32_t *rects, uint32_t flags, uint32_t color, float z,
    uint32_t stencil);
void s2r_blit(d3d_surface *src, const uint32_t *sr, d3d_surface *dst, const uint32_t *dr, uint32_t filter);
void s2r_fill(d3d_surface *dst, const uint32_t *rect, uint32_t color);
void s2r_read_surface(d3d_surface *src, uint32_t dst_mem, uint32_t dst_pitch);
int s2r_read_rgba(d3d_surface *s, const uint32_t *rect, float *out);
void s2r_present(d3d_device *d, uint64_t game_cpu_ns, uint64_t game_wait_ns, uint64_t syncs, uint64_t rt_wait_ns);
/* The [mem] report's GPU objects (iostat.c through d3d_gpu_stats). */
void s2r_gpu_stats(uint64_t *textures, uint64_t *texture_bytes, uint64_t *surfaces, uint64_t *surface_bytes,
    uint64_t *buffers, uint64_t *buffer_bytes, uint64_t *programs);

#endif
