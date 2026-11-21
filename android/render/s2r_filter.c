/* The spatial filters of the native renderers, as GLSL for both backends:
 *
 *   - the present's scene filters of the Direct3D 9 layer (d3d9_filter.c
 *     d3d_filter_present): FSR 1 EASU then RCAS (the "FSR 1" render
 *     upscaler), a bilinear resample (the "Bilinear" upscaler when FXAA
 *     follows), FXAA at the output size (render antialiasing), and the copy a
 *     multisampled Vulkan attachment takes back its resolved image with;
 *   - UI FSR 1 (d3d9_filter.h d3d_ui_filter_source): the interface's 2D
 *     texture reads go through EASU while the game draws its UI, so the
 *     panels the port's UI scale magnifies stay sharp.
 *
 * The same text serves OpenGL ES (#version 310 es, plain uniforms) and Vulkan
 * (#version 450, glslang on the device: binding 0 and push constants). FSR 1
 * is fsr1.h (AMD FidelityFX FSR 1.0, MIT), FXAA d3d9_filter.h. Every pass
 * reads its input with texelFetch or normalized coordinates from
 * gl_FragCoord, so the images keep the rows in memory order in both APIs
 * (the stored rows are Direct3D's, top-down); only the window pass flips. */
#include "s2r_internal.h"
#include "../host/fsr1.h"
#include "../host/d3d9_filter.h"
#include "../host/sims2_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A strip of four vertices from the vertex index, covering the viewport. */
static const char vertex_gles[] =
    "#version 310 es\n"
    "void main(){vec2 p=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char vertex_vulkan[] =
    "#version 450\n"
    "void main(){vec2 p=vec2(float(gl_VertexIndex&1),float(gl_VertexIndex>>1));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";

const char *s2r_filter_vertex(int vulkan) { return vulkan ? vertex_vulkan : vertex_gles; }

/* s2r_filter: xy the output size, z the RCAS sharpness (fsr1_rcas_con). */
static const char common_gles[] =
    "#version 310 es\nprecision highp float;precision highp int;\n"
    "uniform highp sampler2D image;uniform highp vec4 s2r_filter;\n"
    "layout(location=0) out vec4 color;\n";
static const char common_vulkan[] =
    "#version 450\nprecision highp float;precision highp int;\n"
    "layout(binding=0) uniform highp sampler2D image;\n"
    "layout(push_constant) uniform S2R_FILTER { vec4 v; } s2r_push;\n#define s2r_filter s2r_push.v\n"
    "layout(location=0) out vec4 color;\n";
static const char macros[] =
    "#define target (s2r_filter.xy)\n#define sharp (s2r_filter.z)\n"
    "#define FSR_ALPHA 0\n#define FSR_EASU_BASE(fp) ivec2 base=ivec2(fp);\n"
    "#define T(x,y) texelFetch(image,clamp(base+ivec2(x,y),ivec2(0),textureSize(image,0)-1),0)\n"
    "#define FSR_RCAS_BASE ivec2 base=ivec2(gl_FragCoord.xy);\n"
    "#define L(x,y) texelFetch(image,clamp(base+ivec2(x,y),ivec2(0),textureSize(image,0)-1),0)\n";

char *s2r_filter_fragment(int which, int vulkan)
{
    const char *lib = "", *body;
    switch (which) {
    case S2R_FILTER_EASU:
        lib = fsr1_easu_glsl;
        body = "void main(){color=FsrEasuH(gl_FragCoord.xy*vec2(textureSize(image,0))/target);color.a=1.0;}\n";
        break;
    case S2R_FILTER_RCAS:
        lib = fsr1_rcas_glsl;
        body = "void main(){color=FsrRcasH(sharp);color.a=1.0;}\n";
        break;
    case S2R_FILTER_FXAA:
        body = d3d_fxaa_glsl;
        break;
    case S2R_FILTER_LINEAR:
        body = "void main(){color=vec4(texture(image,gl_FragCoord.xy/target).rgb,1.0);}\n";
        break;
    default:   /* S2R_FILTER_COPY: texel for pixel, alpha kept */
        body = "void main(){color=texelFetch(image,ivec2(gl_FragCoord.xy),0);}\n";
        break;
    }
    const char *common = vulkan ? common_vulkan : common_gles;
    size_t n = strlen(common) + strlen(macros) + strlen(lib) + strlen(body) + 1;
    char *out = (char *)malloc(n);
    if (out) snprintf(out, n, "%s%s%s%s", common, macros, lib, body);
    return out;
}

const char *s2r_filter_name(int which)
{
    static const char *const names[S2R_FILTER_COUNT] = { "EASU", "RCAS", "FXAA", "bilinear", "copy" };
    return which >= 0 && which < S2R_FILTER_COUNT ? names[which] : "?";
}

/* The present's chain for the launcher's choices: up to three passes into images of the output
 * size, the last one shown. FSR 1: EASU, RCAS; FXAA after it, or after a bilinear resample when
 * the scene has another size (the Direct3D 9 layer's order). Returns the pass count. */
int s2r_filter_chain(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, int passes[3])
{
    int upscaler = s2_render_upscaler(), aa = s2_render_aa(), n = 0;
    if (upscaler) { passes[n++] = S2R_FILTER_EASU; passes[n++] = S2R_FILTER_RCAS; }
    if (aa) {
        if (!upscaler && (in_w != out_w || in_h != out_h)) passes[n++] = S2R_FILTER_LINEAR;
        passes[n++] = S2R_FILTER_FXAA;
    }
    return n;
}
