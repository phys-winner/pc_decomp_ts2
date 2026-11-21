/* UI FSR 1 (d3d9_filter.h d3d_ui_filter_source for the native renderers): a
 * pixel shader's plain 2D texture() reads go through EASU while the game draws
 * its interface. A text transform without any renderer state, so
 * tools/shaderlib/s2_shaderc.c runs the same code on the PC: the shader
 * library carries the filtered SPIR-V of every game pixel shader, and Vulkan
 * compiles nothing on the device for it (s2r_vulkan.c library_module). */
#include "../host/fsr1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *s2r_ui_filter_source(const char *src);

static int ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* A pixel shader whose plain texture() reads become vpTexture(): EASU on 2D
 * textures while port_vpos.w is 3 (the draw is the interface's), the plain
 * read otherwise (d3d_ui_filter_source's functions, premultiplied alpha). The
 * functions go right after the port block, the first thing every pixel
 * shader declares before any function; textureLod, textureGrad, textureProj
 * and shadow reads keep their Direct3D meaning. malloc'ed; NULL: no port
 * block (the text is used as it is). */
char *s2r_ui_filter_source(const char *src)
{
    const char *block = strstr(src, "uniform S2R_PORT");
    const char *after = block ? strchr(block, '\n') : NULL;
    const char *signature = "vec4 FsrEasuH(highp vec2 src)";
    const char *at = strstr(fsr1_easu_glsl, signature);
    if (!after || !at) return NULL;
    ++after;
    static const char head[] =
        "#define vpUiFilter int(port_vpos.w)\n"
        "#define FSR_ALPHA 1\n"
        "#define FSR_EASU_BASE(fp) ivec2 vpBase=ivec2(fp);\n"
        "#define T(x,y) vpPremulTexel(image,vpBase+ivec2(x,y))\n"
        "vec4 vpPremulTexel(highp sampler2D image,ivec2 p){vec4 c=texelFetch(image,clamp(p,ivec2(0),textureSize(image,0)-1),0);c.rgb*=c.a;return c;}\n";
    static const char replacement[] = "vec4 vpEasu(highp sampler2D image,highp vec2 src)";
    static const char tail[] =
        "\n#undef T\n#undef FSR_EASU_BASE\n#undef FSR_ALPHA\n"
        "vec4 vpTexture(highp sampler2D image,highp vec2 uv){\n"
        " if(vpUiFilter!=3)return texture(image,uv);\n"
        " vec2 size=vec2(textureSize(image,0));\n"
        " vec4 c=vpEasu(image,uv*size);c.rgb=c.a>0.00001?clamp(c.rgb/c.a,0.0,1.0):vec3(0.0);return c;}\n"
        "vec4 vpTexture(highp sampler2D image,highp vec2 uv,float bias){return vpUiFilter!=3?texture(image,uv,bias):vpTexture(image,uv);}\n"
        "vec4 vpTexture(highp samplerCube image,highp vec3 uv){return texture(image,uv);}\n"
        "vec4 vpTexture(highp samplerCube image,highp vec3 uv,float bias){return texture(image,uv,bias);}\n"
        "vec4 vpTexture(highp sampler3D image,highp vec3 uv){return texture(image,uv);}\n"
        "vec4 vpTexture(highp sampler3D image,highp vec3 uv,float bias){return texture(image,uv,bias);}\n"
        "float vpTexture(highp sampler2DShadow image,highp vec3 uv){return texture(image,uv);}\n";
    size_t easu_head = (size_t)(at - fsr1_easu_glsl);
    size_t len = strlen(src);
    size_t cap = len + len / 4 + 64 + sizeof head + strlen(fsr1_easu_glsl) + sizeof replacement + sizeof tail;
    char *out = (char *)malloc(cap), *w = out;
    if (!out) return NULL;
    /* the text up to the port block's end as it is, then the functions */
    size_t prefix = (size_t)(after - src);
    memcpy(w, src, prefix); w += prefix;
    w += sprintf(w, "%s%.*s%s%s%s", head, (int)easu_head, fsr1_easu_glsl, replacement, at + strlen(signature), tail);
    /* the rest with its texture( reads renamed */
    for (const char *p = after; *p;) {
        if (!strncmp(p, "texture(", 8) && (p == src || !ident_char(p[-1]))) {
            if ((size_t)(w - out) + 16 > cap) break;
            memcpy(w, "vpTexture(", 10); w += 10; p += 8;
        } else {
            if ((size_t)(w - out) + 2 > cap) break;
            *w++ = *p++;
        }
    }
    *w = 0;
    return out;
}
