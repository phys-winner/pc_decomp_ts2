/* Read back fixed-function and programmable vertex colors on an Android GPU.
 * The original UI VS assembly string at PE A0D9C248 has an explicit SM1
 * COLOR0 v1 mapping, exposing MojoShader's implicit-register swizzle lookup
 * mismatch. Validate that mapping rather than bytecode identity: assembler
 * comments and register-mask encodings may differ from the runtime shader.
 * Build: python tools/build_vertex_color_probe.py
 * After every adb push, chmod 755 the executable before running it.
 * No app restart, game data, or graphics assets are needed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "../host/d3d9_ff.c"
#include "../host/d3d9_shader.c"
#include "../host/d3d9_gl.c"

void void_log(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args); fputc('\n', stderr);
}

static void require(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s EGL=%x GL=%x\n", what, eglGetError(), glGetError()); exit(1); }
}

int probe_bgra_of(d3d_device *, int, int);
void probe_bind_attributes(d3d_device *, int, const int *, const int *, unsigned);
static d3d_device probe_device;
static d3d_decl probe_decl;
static d3d_texture probe_texture = { .rtype=D3DRTYPE_TEXTURE,.width=1,.height=1,.format=26 };
static int probe_attr_count;
static const int *probe_attr_usage, *probe_attr_index;

static GLuint color_program(uint8_t type, unsigned model)
{
    d3d_device dev = {0};
    d3d_decl decl = {0};
    decl.count = 2;
    decl.elements[0] = (d3d_vertex_element){ .type=3, .usage=9 };
    decl.elements[1] = (d3d_vertex_element){ .offset=16, .type=type, .usage=10 };
    dev.st.decl = &decl;
    uint32_t output = 10u << 8;
    ff_vkey vk;
    require(vertex_key(&dev, &vk, &output, 1), "production vertex key");
    require(vk.bgra == (type == 4), "D3DCOLOR declaration selects BGRA");
    ff_fkey fk = { .available=1 };
    GLuint vs = 0, fs = pixel_shader(&fk);
    const MOJOSHADER_parseData *assembled = NULL;
    d3d_shader *shader = NULL;
    if (!model) vs = vertex_shader(&vk);
    else {
        static const char *asm_vs[] = {
            NULL,
            "vs_1_1\nmul r0,v0,c0\nadd oPos,r0,c1\nmov oD0,v5\nmov oT0.xy,v7.xy\n",
            "vs_2_0\ndcl_position v0\ndcl_color v1\ndcl_texcoord v2\nmul r0,v0,c0\nadd oPos,r0,c1\nmov oD0,v1\nmov oT0.xy,v2.xy\n",
            "vs_3_0\ndcl_position v0\ndcl_color v1\ndcl_texcoord v2\ndcl_position o0\ndcl_color o1\ndcl_texcoord o2\nmul r0,v0,c0\nadd o0,r0,c1\nmov o1,v1\nmov o2.xy,v2.xy\n",
            "vs_1_1\ndcl_position0 v0\ndcl_color0    v1\ndcl_texcoord0 v2\nmul r0, v0, c0\nadd r0, r0, c1\nmov oPos,   r0\nmov oD0,    v1\nmov oT0.xy, v2\n"
        };
        assembled = MOJOSHADER_assemble(NULL, asm_vs[model], strlen(asm_vs[model]), NULL, 0, NULL, 0,
                                        NULL, 0, NULL, NULL, NULL, NULL, NULL);
        if (assembled && assembled->error_count) fprintf(stderr,"assemble: %s\n",assembled->errors[0].error);
        require(assembled && !assembled->error_count,"assemble color VS");
        if (model==4) {
            uint32_t tag = 2166136261u;
            for (int i=0;i<assembled->output_len;++i) tag = (tag ^ (uint8_t)assembled->output[i]) * 16777619u;
            printf("original UI assembly fixture bytecode hash: %08X\n",tag); fflush(stdout);
        }
        shader = calloc(1,sizeof *shader);
        shader->bytecode = (uint8_t *)assembled->output; shader->size = assembled->output_len;
        shader->parse = MOJOSHADER_parse(MOJOSHADER_PROFILE_GLSLES3,"main",shader->bytecode,shader->size,
                                       NULL,0,NULL,0,NULL,NULL,NULL);
        if (shader->parse && shader->parse->error_count) fprintf(stderr,"parse: %s\n",shader->parse->errors[0].error);
        require(shader->parse && !shader->parse->error_count,"production color VS metadata");
        if (model==4) {
            require(shader->parse->major_ver==1 && shader->parse->attribute_count==3,"original UI SM1 declaration count");
            static const MOJOSHADER_usage expected[] = {
                MOJOSHADER_USAGE_POSITION, MOJOSHADER_USAGE_COLOR, MOJOSHADER_USAGE_TEXCOORD
            };
            for (int i=0;i<3;++i) {
                char name[16]; snprintf(name,sizeof name,"vs_v%d",i);
                const MOJOSHADER_attribute *a = shader->parse->attributes+i;
                require(a->usage==expected[i] && a->index==0 && !strcmp(a->name,name),"original UI explicit DCL semantic/register mapping");
            }
        }
        probe_decl.count = 3;
        probe_decl.elements[0] = (d3d_vertex_element){ .offset=0,.type=type,.usage=10 };
        probe_decl.elements[1] = (d3d_vertex_element){ .offset=type==3?16:4,.type=1,.usage=5 };
        probe_decl.elements[2] = (d3d_vertex_element){ .offset=type==3?24:12,.type=1,.usage=0 };
        probe_device.st.decl = &probe_decl;
        probe_device.st.textures[0] = &probe_texture;
        probe_device.st.tss[0][1] = probe_device.st.tss[0][4] = 4;
        probe_device.st.tss[0][2] = probe_device.st.tss[0][5] = 2;
        probe_device.st.tss[0][3] = probe_device.st.tss[0][6] = 0;
        probe_device.st.tss[1][1] = 1;
        uint32_t mask = 0;
        for (int i=0;i<shader->parse->attribute_count;++i)
            if (probe_bgra_of(&probe_device,shader->parse->attributes[i].usage,shader->parse->attributes[i].index)) mask |= 1u<<i;
        require((mask!=0)==(type==4),"programmable D3DCOLOR mask");
        shader->source = patch_source(shader->parse->output,0);
        char *source = vertex_source(shader,mask);
        require(source!=NULL,"production BGRA vertex variant");
        vs = compile(GL_VERTEX_SHADER,source); free(source);
    }
    require(vs && fs, "production shaders compile");
    GLuint program = glCreateProgram();
    if (model) for (int i=0;i<shader->parse->attribute_count;++i)
        glBindAttribLocation(program,(GLuint)i,shader->parse->attributes[i].name);
    glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "production shaders link");
    glDeleteShader(vs); glDeleteShader(fs);
    d3d_gl_use_program(program);
    glUniform4f(glGetUniformLocation(program, "ff_vp"), 0, 0, 16, 16);
    glUniform2f(glGetUniformLocation(program, "ff_half"), 0, 0);
    if (model) {
        d3d_gl_delete_program(program);
        uint32_t ps_samplers, vs_samplers;
        const int *ps_types, *vs_types;
        d3d_shader_forget_current();
        require(d3d_shader_bind2(&probe_device,shader,NULL,probe_bgra_of,&probe_attr_count,&probe_attr_usage,
                                &probe_attr_index,&ps_samplers,&vs_samplers,&ps_types,&vs_types),"production exact UI declaration link/bind");
        require(current_program->bgra_mask == (type==4?2u:0u),"COLOR declaration first maps to VS color input index 1");
        program = current_program->gl;
        float constants[8] = {0.125f,-0.125f,0,0,-1,1,0,1};
        glUniform4fv(glGetUniformLocation(program,"vs_uniforms_vec4"),2,constants);
        glUniform1f(glGetUniformLocation(program,"vpFlip"),1);
        glUniform2f(glGetUniformLocation(program,"vpHalfPixel"),0,0);
    }
    return program;
}

int main(void)
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    require(eglInitialize(display, NULL, NULL), "EGL init");
    EGLint attrs[] = { EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE };
    EGLConfig config; EGLint count;
    require(eglChooseConfig(display, attrs, &config, 1, &count) && count, "RGBA8 config");
    EGLint pb[] = { EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE }, ca[] = { EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pb);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
    require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT, "pbuffer/context");
    require(eglMakeCurrent(display,surface,surface,context), "current context");
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    glViewport(0,0,16,16);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
    GLuint vao, buffer; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&buffer); glBindBuffer(GL_ARRAY_BUFFER,buffer);
    GLuint texture; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    uint16_t white = 0xFFFF;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA4,1,1,0,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,&white);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    static const uint32_t colors[] = { 0xFF000000,0xFF0000FF,0xFFFF0000,0xFF050671,0xFF710605,0xFF7F7F7F,0xB13D9BC7 };
    static const uint8_t types[] = {4,8,3};
    static const float pos[4][4] = {{0,0,0.5,1},{16,0,0.5,1},{0,16,0.5,1},{16,16,0.5,1}};
    unsigned passed = 0;
    for (unsigned model=0;model<5;++model) for (unsigned t=0;t<sizeof types;++t) {
        GLuint program = color_program(types[t],model);
        for (unsigned c=0;c<sizeof colors/sizeof colors[0];++c) {
            uint32_t argb = colors[c];
            uint8_t expected[4] = {argb>>16,argb>>8,argb,argb>>24};
            uint8_t data[4*32] = {0};
            unsigned stride = types[t] == 3 ? 32 : 20;
            for (unsigned v=0;v<4;++v) {
                unsigned color_at = model?0:16;
                if (model) memcpy(data+v*stride+(types[t]==3?24:12),pos[v],8);
                else memcpy(data+v*stride,pos[v],16);
                if (types[t] == 4) memcpy(data+v*stride+color_at,&argb,4);
                else if (types[t] == 8) memcpy(data+v*stride+color_at,expected,4);
                else {
                    float rgba[4]; for (unsigned k=0;k<4;++k) rgba[k]=expected[k]/255.0f;
                    memcpy(data+v*stride+color_at,rgba,16);
                }
            }
            glBufferData(GL_ARRAY_BUFFER,stride*4,data,GL_STREAM_DRAW);
            if (model) probe_bind_attributes(&probe_device,probe_attr_count,probe_attr_usage,probe_attr_index,stride);
            else {
                glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,stride,(void*)0);
                glVertexAttribPointer(2,4,types[t]==3?GL_FLOAT:GL_UNSIGNED_BYTE,types[t]==3?GL_FALSE:GL_TRUE,stride,(void*)16);
                glEnableVertexAttribArray(0); glEnableVertexAttribArray(2);
            }
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            uint8_t actual[4]; glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual);
            require(glGetError()==GL_NO_ERROR,"draw/readback");
            for (unsigned k=0;k<4;++k) {
                if (abs((int)actual[k]-expected[k])>1) {
                    fprintf(stderr,"FAIL VS=%u type=%u ARGB=%08X expected=%u,%u,%u,%u actual=%u,%u,%u,%u\n",model,types[t],argb,
                        expected[0],expected[1],expected[2],expected[3],actual[0],actual[1],actual[2],actual[3]);
                    return 1;
                }
            }
            ++passed;
        }
        if (!model) d3d_gl_delete_program(program);
    }
    printf("PASS %u real-GPU pixels: production FF + original UI assembly SM1 DCL COLOR0v1 + COLOR/UV/POS declaration order through d3d_shader_bind2 + VS1/2/3, D3DCOLOR/UBYTE4N/FLOAT4, black/red/blue/OptionsText/gray/alpha\n",passed);
    return 0;
}



int probe_bgra_of(d3d_device *d, int usage, int index) { return bgra_of(d, usage, index); }

void probe_bind_attributes(d3d_device *d, int count, const int *usage, const int *index, unsigned stride)
{
    for (int i=0;i<16;++i) glDisableVertexAttribArray((GLuint)i);
    for (int i=0;i<count;++i) {
        const d3d_vertex_element *e = find_element(d,usage[i],index[i]);
        if (!e) abort();
        const attr_format *f = attr_formats+e->type;
        glVertexAttribPointer((GLuint)i,f->size,f->type,f->norm,(GLsizei)stride,(void *)(uintptr_t)e->offset);
        glEnableVertexAttribArray((GLuint)i);
    }
}
