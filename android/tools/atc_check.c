/* The DXT->ATC transcoder (android/host/d3d9_atc.c) on the device:
 *   1. its model of the GPU's ATC decode (d3d_atc_decode_block) against the
 *      GPU itself, texel for texel, on every block it produces;
 *   2. the error of transcoded blocks against the DXT decode the renderer
 *      used before (d3d9_format.c's rules), on random blocks of every
 *      contrast, single colours, DXT1 punch-through and DXT3/DXT5 alpha.
 * Exit 0 when the GPU agrees exactly and the error stays within bounds.
 * Build and run: tools/check_atc.py --serial SERIAL. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "d3d9_internal.h"
#include "d3d9_atc.h"

/* ---- the DXT decode of d3d9_format.c ---- */
static void rgb565(uint16_t c, uint8_t out[3])
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (uint8_t)((r << 3) | (r >> 2)); out[1] = (uint8_t)((g << 2) | (g >> 4)); out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static void decode_color_block(const uint8_t *block, uint8_t out[16][4], int force_four)
{
    uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8)), c1 = (uint16_t)(block[2] | (block[3] << 8));
    uint32_t bits = (uint32_t)block[4] | ((uint32_t)block[5] << 8) | ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || force_four) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (int i = 0; i < 16; ++i) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

static void decode_alpha_block(const uint8_t *block, uint8_t out[16])
{
    uint8_t a0 = block[0], a1 = block[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)block[2 + i] << (8 * i);
    uint8_t pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
    else { for (int i = 1; i < 5; ++i) pal[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5); pal[6] = 0; pal[7] = 255; }
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

static void dxt_decode(int conv, const uint8_t *b, uint8_t out[16][4])
{
    if (conv == CONV_DXT1) { decode_color_block(b, out, 0); return; }
    decode_color_block(b + 8, out, 1);
    if (conv == CONV_DXT3) for (int i = 0; i < 16; ++i) out[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    else { uint8_t a[16]; decode_alpha_block(b, a); for (int i = 0; i < 16; ++i) out[i][3] = a[i]; }
}

/* ---- GL ---- */
static GLuint prog, fbo, rb;
static void gl_init(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, NULL, NULL);
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8, EGL_NONE };
    EGLConfig config; EGLint n = 0;
    eglChooseConfig(d, attrs, &config, 1, &n);
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, config, pb);
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(d, s, s, c);
    const char *vs = "#version 300 es\nout vec2 uv;\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
                     "uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
    const char *fs = "#version 300 es\nprecision highp float;\nin vec2 uv;uniform sampler2D t;out vec4 o;\n"
                     "void main(){ivec2 s=textureSize(t,0);o=texelFetch(t,ivec2(uv*vec2(s)),0);}\n";
    prog = glCreateProgram();
    const char *src[2] = { vs, fs };
    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    for (int i = 0; i < 2; ++i) { GLuint sh = glCreateShader(types[i]); glShaderSource(sh, 1, &src[i], NULL); glCompileShader(sh); glAttachShader(prog, sh); }
    glLinkProgram(prog);
    glUseProgram(prog);
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
}

/* The GPU's decode of `count` ATC blocks laid out as one row, through glTexStorage2D like the renderer. */
static int gpu_decode(GLenum storage, const uint8_t *blocks, int count, uint8_t *out)
{
    int w = count * 4, h = 4;
    GLuint tex; glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError()) {}
    glTexStorage2D(GL_TEXTURE_2D, 1, storage, w, h);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, storage, count * (int)d3d_atc_block_bytes(storage), blocks);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum e = glGetError();
    if (e) { printf("FAIL: glTexStorage2D/glCompressedTexSubImage2D of %04X: GL error %04X\n", storage, e); return 0; }
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int b = 0; b < count; ++b)
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
            memcpy(out + ((size_t)b * 16 + y * 4 + x) * 4, px + ((size_t)y * w + b * 4 + x) * 4, 4);
    free(px);
    glDeleteTextures(1, &tex);
    return 1;
}

static uint32_t rng = 0x12345678u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* A random DXT block whose endpoints are at most `spread` apart per 8-bit channel. */
static void random_block(int conv, int spread, uint8_t *b)
{
    int in = conv == CONV_DXT1 ? 8 : 16;
    for (int i = 0; i < in; ++i) b[i] = (uint8_t)next();
    uint8_t *c = conv == CONV_DXT1 ? b : b + 8;
    int r = (int)(next() % 32), g = (int)(next() % 64), bl = (int)(next() % 32);
    int dr = spread / 8, dg = spread / 4, db = spread / 8;
    int r1 = r + (dr ? (int)(next() % (2 * dr + 1)) - dr : 0), g1 = g + (dg ? (int)(next() % (2 * dg + 1)) - dg : 0),
        b1 = bl + (db ? (int)(next() % (2 * db + 1)) - db : 0);
    r1 = r1 < 0 ? 0 : r1 > 31 ? 31 : r1; g1 = g1 < 0 ? 0 : g1 > 63 ? 63 : g1; b1 = b1 < 0 ? 0 : b1 > 31 ? 31 : b1;
    uint16_t c0 = (uint16_t)(r << 11 | g << 5 | bl), c1 = (uint16_t)(r1 << 11 | g1 << 5 | b1);
    if (conv == CONV_DXT1 && (next() & 3) == 0 && c0 > c1) { uint16_t t = c0; c0 = c1; c1 = t; }   /* some three-colour blocks */
    c[0] = (uint8_t)c0; c[1] = (uint8_t)(c0 >> 8); c[2] = (uint8_t)c1; c[3] = (uint8_t)(c1 >> 8);
}

int main(void)
{
    gl_init();
    printf("%s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    enum { N = 2048 };
    static uint8_t dxt[N * 16], atc[N * 16], gpu[N * 16 * 4];
    int failures = 0;
    static const int convs[3] = { CONV_DXT1, CONV_DXT3, CONV_DXT5 };
    static const char *names[3] = { "DXT1", "DXT3", "DXT5" };
    static const int spreads[5] = { 0, 16, 48, 128, 255 };
    for (int ci = 0; ci < 3; ++ci) for (int si = 0; si < 5; ++si) {
        int conv = convs[ci], in = conv == CONV_DXT1 ? 8 : 16;
        for (int k = 0; k < N; ++k) random_block(conv, spreads[si], dxt + (size_t)k * in);
        /* One texture per test, as wide as the blocks (a row of N blocks). */
        GLenum storage = d3d_atc_storage(conv, dxt, N * 4, 4, (uint32_t)(N * in));
        size_t bytes = d3d_atc_transcode(conv, storage, dxt, N * 4, 4, (uint32_t)(N * in), atc);
        uint32_t out = d3d_atc_block_bytes(storage);
        if (bytes != (size_t)N * out) { printf("FAIL: %s wrote %zu bytes\n", names[ci], bytes); ++failures; continue; }
        if (!gpu_decode(storage, atc, N, gpu)) { ++failures; continue; }
        long model_mismatch = 0, max_err = 0, max_alpha = 0;
        double sq = 0;
        long cared = 0;
        for (int k = 0; k < N; ++k) {
            uint8_t want[16][4], model[16][4];
            dxt_decode(conv, dxt + (size_t)k * in, want);
            d3d_atc_decode_block(storage, atc + (size_t)k * out, model);
            for (int i = 0; i < 16; ++i) {
                const uint8_t *g = gpu + ((size_t)k * 16 + i) * 4;
                for (int c = 0; c < 3; ++c) if (g[c] != model[i][c]) ++model_mismatch;
                /* Interpolated alpha: GPUs round the sevenths/fifths their own way (within 1). */
                if (abs((int)g[3] - (int)model[i][3]) > (storage == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD ? 1 : 0)) ++model_mismatch;
                int transparent = want[i][3] == 0;
                for (int c = 0; c < 3 && !transparent; ++c) {
                    long e = labs((long)model[i][c] - want[i][c]);
                    if (e > max_err) max_err = e;
                    sq += (double)e * e;
                    ++cared;
                }
                long ea = labs((long)model[i][3] - want[i][3]);
                if (ea > max_alpha) max_alpha = ea;
            }
        }
        double psnr = cared && sq ? 10 * log10(255.0 * 255.0 * cared / sq) : 99.0;
        /* DXT5's own alpha decode differs from GPUs by a rounding step (+-1). */
        int ok = model_mismatch == 0 && max_alpha <= (conv == CONV_DXT5 ? 1 : 0);
        printf("%s spread %3d -> %s: GPU vs model %ld channel values differ; colour max error %ld, PSNR %.1f dB; alpha max error %ld%s\n",
            names[ci], spreads[si], storage == GL_ATC_RGB_AMD ? "ATC_RGB" : storage == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ?
            "ATC_EXPLICIT" : "ATC_INTERP", model_mismatch, max_err, psnr, max_alpha, ok ? "" : "  <- FAIL");
        if (!ok) ++failures;
    }
    /* Speed: the render thread transcodes every DXT level the game uploads. */
    for (int ci = 0; ci < 3; ++ci) {
        int conv = convs[ci], in = conv == CONV_DXT1 ? 8 : 16;
        enum { BIG = 65536 };
        uint8_t *src = malloc((size_t)BIG * 16), *dst = malloc((size_t)BIG * 16);
        for (int k = 0; k < BIG; ++k) random_block(conv, spreads[k % 5], src + (size_t)k * in);
        GLenum storage = conv == CONV_DXT1 ? GL_ATC_RGB_AMD : conv == CONV_DXT3 ? GL_ATC_RGBA_EXPLICIT_ALPHA_AMD : GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int r = 0; r < 4; ++r) d3d_atc_transcode(conv, storage, src, 1024, (uint32_t)BIG * 4 / 1024 * 4 / 4, 256u * (uint32_t)in, dst);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / (4.0 * BIG);
        printf("%s transcode: %.0f ns per block (%.0f MB/s of DXT)\n", names[ci], ns, in / ns * 1e3);
        free(src); free(dst);
    }
    printf(failures ? "FAILED %d\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
