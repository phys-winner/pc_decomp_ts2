/* ProcessVertices under the native renderers.
 *
 * The Direct3D 9 layer runs a vs_3_0 shader over a vertex range with GL
 * transform feedback and writes the results into the destination buffer
 * (d3d9_shader.c run_process: the game uses it for preprocessing and
 * portraits). The native renderers keep that path: under OpenGL ES it runs in
 * the backend's own context (run_process saves and restores the GL state it
 * touches), under Vulkan in a small OpenGL ES context of its own, made current
 * on the render thread only for the call (Vulkan has no transform feedback on
 * these phones). The inputs come from the buffers' guest copies, the results
 * go into the guest copy and through s2r_buffer_upload to the backend. */
#include "s2r_internal.h"
#include "s2r.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

static EGLDisplay own_display = EGL_NO_DISPLAY;
static EGLContext own_context = EGL_NO_CONTEXT;
static EGLSurface own_surface = EGL_NO_SURFACE;
static int own_failed, own_current;

/* A GLES 3 context current on this thread for the call: the backend's (OpenGL ES), else ours. */
int s2r_process_begin(void)
{
    own_current = 0;
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) return 1;
    if (own_failed) return 0;
    if (own_context == EGL_NO_CONTEXT) {
        own_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint n = 0;
        EGLConfig config;
        const EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        const EGLint ctx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
        const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        if (own_display == EGL_NO_DISPLAY || !eglInitialize(own_display, NULL, NULL) ||
            !eglChooseConfig(own_display, attrs, &config, 1, &n) || n < 1 ||
            (own_context = eglCreateContext(own_display, config, EGL_NO_CONTEXT, ctx)) == EGL_NO_CONTEXT ||
            (own_surface = eglCreatePbufferSurface(own_display, config, pb)) == EGL_NO_SURFACE) {
            S2R_LOG("ProcessVertices: no OpenGL ES context (%04X): the calls fail", eglGetError());
            own_failed = 1;
            return 0;
        }
        S2R_LOG("ProcessVertices: an OpenGL ES 3.1 context of its own (transform feedback)");
    }
    if (!eglMakeCurrent(own_display, own_surface, own_surface, own_context)) return 0;
    own_current = 1;
    return 1;
}

void s2r_process_end(void)
{
    if (!own_current) return;
    own_current = 0;
    eglMakeCurrent(own_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
