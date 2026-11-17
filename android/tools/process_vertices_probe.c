/* Replay captured VS3 fixtures through the production ProcessVertices path.
 * Usage: process_vertices_probe FIXTURE_DIRECTORY RESULT_DIRECTORY
 * Only the guest allocator and synchronous render-queue adapters are local;
 * shader translation, input binding, GPU execution, and output scatter are
 * the same code compiled into the APK. Build with tools/build_process_probe.py.
 * That tool documents abb24fae replay paths under /data/local/tmp/sims2;
 * run chmod 755 after EVERY executable push, including replacement builds.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../host/d3d9_shader.c"

static d3d_device *device;
static uint32_t guest_next;
enum { GUEST_BEGIN = 0x10000, GUEST_BYTES = 2 * 1024 * 1024 };

void void_log(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args); fputc('\n', stderr);
}

d3d_device *d3d_rt_device(void) { return device; }
void d3d_rt_sync_state(d3d_device *d) { if (d != device) abort(); }
void d3d_rt_sync(d3d_rt_fn fn, void *arg) { fn(arg); }

/* Fixtures are uploaded before entry, so no dirty guest locks remain. */
void d3d_buffer_flush(d3d_buffer *b)
{
    if (b->dirty_lo || b->dirty_hi || b->discard_pending || b->overwrite_pending) abort();
}

static void die(const char *what)
{
    fprintf(stderr, "FAIL: %s (errno=%d, EGL=%04X, GL=%04X)\n", what, errno, eglGetError(), glGetError());
    exit(1);
}

static uint8_t *read_file(const char *dir, const char *name, uint32_t *size)
{
    char path[2048];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) die("path too long");
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); die("open fixture"); }
    if (fseek(f, 0, SEEK_END)) die("seek fixture");
    long n = ftell(f);
    if (n < 0 || n > GUEST_BYTES || fseek(f, 0, SEEK_SET)) die("fixture size");
    uint8_t *data = malloc((size_t)n + 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n || fclose(f)) die("read fixture");
    *size = (uint32_t)n;
    return data;
}

static void write_file(const char *dir, const char *name, const void *data, uint32_t size)
{
    char path[2048];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) die("path too long");
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f)) die("write result");
}

static void load_buffer(d3d_buffer *b, const char *dir, const char *name)
{
    uint8_t *data = read_file(dir, name, &b->size);
    guest_next = (guest_next + 15) & ~15u;
    if (b->size > GUEST_BEGIN + GUEST_BYTES - guest_next) die("guest arena full");
    b->dev = device; b->shadow = guest_next; guest_next += b->size;
    memcpy(GPTR(b->shadow), data, b->size);
    glGenBuffers(1, &b->gl); glBindBuffer(GL_ARRAY_BUFFER, b->gl);
    glBufferData(GL_ARRAY_BUFFER, b->size, data, GL_DYNAMIC_DRAW);
    free(data);
    if (glGetError()) die("fixture GPU upload");
}

static void load_decl(d3d_decl *d, const char *dir, const char *name)
{
    uint32_t size;
    uint8_t *data = read_file(dir, name, &size);
    if (!size || size % sizeof(d3d_vertex_element) || size > sizeof d->elements) die("declaration size");
    d->dev = device; d->count = (int)(size / sizeof(d3d_vertex_element));
    memcpy(d->elements, data, size); free(data);
}

static void load_shader(d3d_shader *s, const char *dir)
{
    s->dev = device; s->bytecode = read_file(dir, "shader.vs.bin", &s->size);
    s->parse = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", s->bytecode, s->size,
                                NULL, 0, NULL, 0, NULL, NULL, NULL);
    /* Same preliminary metadata parse/relative CTAB handling as creation in
     * the APK; the production ProcessVertices linker reparses the full bank. */
    int lo = -1;
    if (s->parse && s->parse->error_count && strstr(s->parse->errors[0].error, "without a CTAB")) lo = 0;
    else if (s->parse && !s->parse->error_count) lo = relative_base(s->parse);
    uint32_t regs = lo >= 0 ? relative_regs(0, s->parse && !s->parse->error_count ? s->parse : NULL, lo) : 0;
    uint8_t *patched = lo >= 0 ? with_synthetic_ctab(s->bytecode, s->size, 0, (uint32_t)lo, regs, &s->parse_size) : NULL;
    if (patched) {
        const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", patched, s->parse_size,
                                                        NULL, 0, NULL, 0, NULL, NULL, NULL);
        if (pd && !pd->error_count) {
            MOJOSHADER_freeParseData(s->parse); s->parse = pd; s->parse_code = patched;
        } else { if (pd) MOJOSHADER_freeParseData(pd); free(patched); }
    }
    if (!s->parse || s->parse->error_count) {
        if (s->parse) void_log("parse: %s", s->parse->errors[0].error);
        die("fixture shader metadata parse");
    }
}

static void compare(const char *name, const uint8_t *actual, const uint8_t *expected,
                    uint32_t size, uint32_t begin, uint32_t end)
{
    if (memcmp(actual, expected, begin) || memcmp(actual + end, expected + end, size - end)) die("destination canaries changed");
    for (uint32_t at = begin; at < end; at += 4) {
        float got, want; memcpy(&got, actual + at, 4); memcpy(&want, expected + at, 4);
        if (!isfinite(got) || fabsf(got - want) > 2e-5f * fmaxf(1.0f, fabsf(want))) {
            fprintf(stderr, "%s byte %u: got %.9g, expected %.9g\n", name, at, got, want);
            die("raw POSITION/NORMAL/UV mismatch");
        }
    }
}

static void replay(const char *fixtures, const char *results, const char *name)
{
    char path[2048], filename[128]; uint32_t size;
    if (snprintf(path, sizeof path, "%s/%s", fixtures, name) >= (int)sizeof path) die("path too long");
    uint8_t *params = read_file(path, "case.params.bin", &size);
    if (size != 48) die("fixture parameter size");
    uint32_t p[12]; memcpy(p, params, sizeof p); free(params);
    if (p[0] != 0x31525650 || p[5] != 32 || p[11] != D3D_SOFTWARE_VS_CONSTANTS ||
        ((uint64_t)p[2] + p[3]) * p[5] > p[6]) die("fixture parameter header");
    memset(&device->st, 0, sizeof device->st); guest_next = GUEST_BEGIN;
    d3d_buffer streams[2] = {{0}}, output = {0};
    d3d_decl input_decl = {0}, output_decl = {0}; d3d_shader shader = {0};
    load_buffer(streams, path, "stream0.bin"); load_buffer(streams + 1, path, "stream1.bin");
    load_buffer(&output, path, "destination.before.bin");
    if (output.size != p[6]) die("destination size");
    load_decl(&input_decl, path, "input.decl.bin"); load_decl(&output_decl, path, "output.decl.bin");
    load_shader(&shader, path);
    uint8_t *constants = read_file(path, "constants.f32.bin", &size);
    if (size != sizeof device->st.vs_f) die("constant bank size");
    memcpy(device->st.vs_f, constants, size); free(constants);
    device->st.vs = &shader; device->st.decl = &input_decl;
    for (int i = 0; i < 2; ++i) {
        device->st.streams[i] = &streams[i];
        device->st.stream_stride[i] = p[7 + i * 2]; device->st.stream_offset[i] = p[8 + i * 2];
    }
    uint32_t hr = d3d_process_vertices(device, p[1], p[2], p[3], &output, &output_decl, p[4]);
    snprintf(filename, sizeof filename, "%s.bin", name);
    write_file(results, filename, GPTR(output.shadow), output.size);
    if (hr != D3D_OK) { fprintf(stderr, "%s HRESULT=%08X\n", name, hr); die("ProcessVertices"); }
    glBindBuffer(GL_COPY_READ_BUFFER, output.gl);
    const void *gpu = glMapBufferRange(GL_COPY_READ_BUFFER, 0, output.size, GL_MAP_READ_BIT);
    if (!gpu) die("read destination GPU buffer");
    snprintf(filename, sizeof filename, "%s.gpu.bin", name); write_file(results, filename, gpu, output.size);
    if (memcmp(gpu, GPTR(output.shadow), output.size)) die("CPU shadow and GPU destination differ");
    if (!glUnmapBuffer(GL_COPY_READ_BUFFER)) die("destination GPU map invalid");
    uint8_t *expected = read_file(path, "expected.buffer.bin", &size);
    if (size != output.size) die("expected destination size");
    compare(name, GPTR(output.shadow), expected, size, p[2] * p[5], (p[2] + p[3]) * p[5]); free(expected);
    printf("PASS %s: %u vertices, raw POSITION/NORMAL/UV, CPU/GPU match, canaries intact\n", name, p[3]); fflush(stdout);
    process_destroy(&shader); MOJOSHADER_freeParseData(shader.parse); free(shader.parse_code); free(shader.bytecode);
    glDeleteBuffers(1, &output.gl); for (int i = 0; i < 2; ++i) glDeleteBuffers(1, &streams[i].gl);
    if (glGetError()) die("end-of-case GL error");
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s FIXTURE_DIRECTORY RESULT_DIRECTORY\n", argv[0]); return 2; }
    if (mkdir(argv[2], 0775) && errno != EEXIST) die("create result directory");
    void *wanted = GPTR(GUEST_BEGIN);
    void *mapped = mmap(wanted, GUEST_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped != wanted) die("guest arena address unavailable");
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) die("eglInitialize");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint n;
    if (!eglChooseConfig(display, attrs, &config, 1, &n) || !n) die("ES3 pbuffer config");
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pb);
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) die("ES3.2 context");
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, (GLint *)&d3d_gl_limits.max_vs_vectors);
    device = calloc(1, sizeof *device); if (!device) die("allocate device state");
    const char *cases[] = {"blend", "high-bone", "unused-bones", "offsets"};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) replay(argv[1], argv[2], cases[i]);
    puts("ProcessVertices GPU regression: 4/4 PASS");
    free(device); eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    munmap(mapped, GUEST_BYTES);
    return 0;
}
