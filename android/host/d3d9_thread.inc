/* Query completion and optional GL ownership checks shared by the GLES bridge. */
#ifndef S2_D3D9_THREAD_INC
#define S2_D3D9_THREAD_INC
#include <stdatomic.h>

void d3d_rt_set_idle(int (*poll)(int idle));
void d3d_rt_apply_cores(void);
void d3d_query_log(uint32_t frames);

#ifdef VOID_RT_CHECK
static inline void d3d_rt_gl_check(const char *name)
{
    if (!d3d_rt_on_render_thread()) void_fatal("[rt] CHECK FAILED: %s outside the render thread", name);
}
#include "d3d9_thread_check.inc"
#endif

#endif
