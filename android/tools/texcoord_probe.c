/* Read back ps_1_1..1_3 TEXCOORD through the actual production FF/PS pipeline.
 * Microsoft: https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texcoord---ps
 * Build: python tools/build_texcoord_probe.py
 * An optional argv[1] is an actual failed shader dump; parse, patch and compile
 * that fragment shader as well. Root owns all device operations.
 */
#define main vertex_color_regression_main
#include "vertex_color_probe.c"
#undef main

void void_fatal(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args); fputc('\n', stderr); exit(2);
}

static d3d_shader coordinate_shader(unsigned model, unsigned stage)
{
    char assembly[160];
    /* MojoShader's assembler calls opcode 64 TEXCRD for both SM1 forms. */
    snprintf(assembly, sizeof assembly, "ps_1_%u\ntexcrd t%u\nmov r0,t%u\n", model, stage, stage);
    const MOJOSHADER_parseData *a = MOJOSHADER_assemble(NULL, assembly, strlen(assembly), NULL, 0, NULL, 0,
        NULL, 0, NULL, NULL, NULL, NULL, NULL);
    if (a && a->error_count) fprintf(stderr, "assemble: %s\n", a->errors[0].error);
    require(a && !a->error_count, "assemble TEXCOORD");
    d3d_shader s = {0};
    s.pixel = 1; s.bytecode = (uint8_t *)a->output; s.size = a->output_len;
    s.parse = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", s.bytecode, s.size,
        NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (s.parse && s.parse->error_count) fprintf(stderr, "parse: %s\n", s.parse->errors[0].error);
    require(s.parse && !s.parse->error_count && s.parse->major_ver == 1 && s.parse->minor_ver == model &&
        s.parse->sampler_count == 0, "old-version TEXCOORD parses without a sampler");
    char expected[96];
    snprintf(expected, sizeof expected, "vec4(clamp(io_5_%u.xyz, vec3(0.0), vec3(1.0)), 1.0)", stage);
    require(strstr(s.parse->output, expected) != NULL, "TEXCOORD reads original interpolator, clamps XYZ, forces W");
    s.source = patch_source(s.parse->output, 1);
    return s; /* The assembled bytecode stays alive for the whole probe. */
}

static void rejected_14(void)
{
    static const char assembly[] = "ps_1_4\ntexcrd r0.rgb,t0\nmov r0.a,c0\n";
    const MOJOSHADER_parseData *a = MOJOSHADER_assemble(NULL, assembly, sizeof assembly - 1,
        NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL);
    require(a && !a->error_count, "assemble separate ps_1_4 TEXCRD form");
    const MOJOSHADER_parseData *p = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", (const unsigned char *)a->output, a->output_len,
        NULL, 0, NULL, 0, NULL, NULL, NULL);
    require(p && p->error_count && strstr(p->errors[0].error, "TEXCRD unimplemented"), "ps_1_4 remains unsupported");
    MOJOSHADER_freeParseData(p); MOJOSHADER_freeParseData(a);
}

static void captured_shader(const char *path)
{
    FILE *f = fopen(path, "rb"); require(f != NULL, "open actual shader dump");
    require(!fseek(f, 0, SEEK_END), "size actual shader dump");
    long size = ftell(f); require(size >= 8 && size <= 1048576, "bounded actual shader size");
    rewind(f); uint8_t *bytes = malloc((size_t)size); require(bytes != NULL, "allocate actual shader");
    require(fread(bytes, 1, (size_t)size, f) == (size_t)size, "read complete actual shader"); fclose(f);
    const MOJOSHADER_parseData *p = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3, "main", bytes, (unsigned)size,
        NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (p && p->error_count) fprintf(stderr, "actual shader parse: %s\n", p->errors[0].error);
    require(p && !p->error_count, "actual game shader translation");
    char *source = patch_source(p->output, 1);
    GLuint shader = compile(GL_FRAGMENT_SHADER, source);
    require(shader != 0, "actual game shader GPU compilation");
    printf("PASS actual game fragment shader: %ld bytes, ps_%u_%u, %u samplers, translated and GPU-compiled\n",
        size, p->major_ver, p->minor_ver, p->sampler_count);
    glDeleteShader(shader); free(source); MOJOSHADER_freeParseData(p); free(bytes);
}

static void coordinate_attributes(d3d_device *d, int count, const int *usage, const int *index, unsigned stride)
{
    for (int i = 0; i < 16; ++i) glDisableVertexAttribArray((GLuint)i);
    for (int i = 0; i < count; ++i) {
        const d3d_vertex_element *e = find_element(d, usage[i], index[i]);
        if (!e) {
            float value = usage[i] == 10 ? 1.0f : 0.0f;
            glVertexAttrib4f((GLuint)i, value, value, value, 1);
            continue;
        }
        const attr_format *f = attr_formats + e->type;
        glVertexAttribPointer((GLuint)i, f->size, f->type, f->norm, (GLsizei)stride, (void *)(uintptr_t)e->offset);
        glEnableVertexAttribArray((GLuint)i);
    }
}

int main(int argc, char **argv)
{
    require(vertex_color_regression_main() == 0, "existing 105-pixel vertex-color regression");
    rejected_14();
    static const float positions[4][4] = {{0,0,.5,1},{16,0,.5,1},{0,16,.5,1},{16,16,.5,1}};
    static const float values[][4] = {{-.5f,1.5f,.25f,-8}, {0,0,0,0}, {1,1,1,.2f},
        {.125f,.5f,.875f,0}, {2,-1,2,2}, {.9999f,.0001f,.5f,-1}, {-2,-3,-4,8}, {.25f,.75f,.0625f,.5f}};
    static d3d_shader shaders[3][4];
    unsigned passed = 0;
    for (unsigned model = 1; model <= 3; ++model) for (unsigned stage = 0; stage < 4; ++stage) {
        d3d_shader *s = &shaders[model - 1][stage];
        *s = coordinate_shader(model, stage);
        for (unsigned dims = 2; dims <= 4; ++dims) {
            d3d_device d = {0}; d3d_decl decl = {0};
            decl.count = 2;
            decl.elements[0] = (d3d_vertex_element){.type=3,.usage=9};
            decl.elements[1] = (d3d_vertex_element){.offset=16,.type=(uint8_t)(dims-1),.usage=5,.usage_index=(uint8_t)stage};
            d.st.decl = &decl; d.st.viewport[2] = d.st.viewport[3] = 16; d.st.tss[stage][11] = stage;
            uint32_t ps_mask, vs_mask; int count;
            const int *usage, *index, *ps_types, *vs_types;
            require(d3d_shader_bind2(&d, NULL, s, probe_bgra_of, &count, &usage, &index, &ps_mask, &vs_mask,
                &ps_types, &vs_types), "production fixed vertex + TEXCOORD pixel program");
            require(ps_mask == 0 && vs_mask == 0, "TEXCOORD does not bind or sample a texture");
            d3d_shader_upload(&d, 0, 0, 8, 0);
            for (unsigned test = 0; test < sizeof values / sizeof values[0]; ++test) {
                float data[4][8]; uint8_t expected[4], actual[4];
                for (unsigned v = 0; v < 4; ++v) {
                    memcpy(data[v], positions[v], 16); memcpy(data[v] + 4, values[test], 16);
                }
                for (unsigned k = 0; k < 3; ++k) {
                    float value = k < dims ? values[test][k] : 0;
                    expected[k] = (uint8_t)floorf(fminf(1, fmaxf(0, value)) * 255 + .5f);
                }
                expected[3] = 255;
                glBufferData(GL_ARRAY_BUFFER, sizeof data, data, GL_STREAM_DRAW);
                coordinate_attributes(&d, count, usage, index, sizeof data[0]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual);
                require(glGetError() == GL_NO_ERROR, "TEXCOORD draw/readback");
                for (unsigned k = 0; k < 4; ++k) if (abs((int)actual[k] - expected[k]) > 1) {
                    fprintf(stderr, "FAIL ps_1_%u t%u dims%u case%u: expected %u,%u,%u,%u actual %u,%u,%u,%u\n",
                        model, stage, dims, test, expected[0], expected[1], expected[2], expected[3], actual[0], actual[1], actual[2], actual[3]);
                    return 1;
                }
                ++passed;
            }
        }
    }
    printf("PASS %u TEXCOORD GPU pixels: ps_1_1/1_2/1_3, t0..t3, FLOAT2/3/4, clamp edges, missing Z, forced W=1, no sampler; ps_1_4 rejection preserved\n", passed);
    if (argc > 1) captured_shader(argv[1]);
    return 0;
}
