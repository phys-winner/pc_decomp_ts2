/* Optional spatial reconstruction. All intermediates stay on the GL render thread. */
#include "d3d9_internal.h"
#include "sims2_settings.h"
#include "fsr1.h"

typedef struct { GLuint texture; int width,height; } filter_target;
static filter_target targets[3];
static GLuint programs[6],framebuffer,sampler,filter_vao;
static unsigned failed;
static const char vertex[]="void main(){vec2 p=vec2(gl_VertexID&1,gl_VertexID>>1);gl_Position=vec4(p*2.0-1.0,0,1);}";
static const char common[]="precision highp float;precision highp int;\n"
    "uniform highp sampler2D image;uniform highp vec2 target;layout(location=0)out vec4 color;\n"
    "#define FSR_ALPHA 0\n#define FSR_EASU_BASE(fp) ivec2 base=ivec2(fp);\n"
    "#define T(x,y) texelFetch(image,clamp(base+ivec2(x,y),ivec2(0),textureSize(image,0)-1),0)\n"
    "#define FSR_RCAS_BASE ivec2 base=ivec2(gl_FragCoord.xy);\n"
    "#define L(x,y) texelFetch(image,clamp(base+ivec2(x,y),ivec2(0),textureSize(image,0)-1),0)\n";
static const char fxaa[]=
    "vec3 tap(vec2 uv){return texture(image,uv).rgb;}\n"
    "void main(){vec2 px=1.0/vec2(textureSize(image,0)),uv=gl_FragCoord.xy/target;vec3 luma=vec3(.299,.587,.114);\n"
    "vec3 m=tap(uv);float nw=dot(tap(uv+vec2(-1,-1)*px),luma),ne=dot(tap(uv+vec2(1,-1)*px),luma);\n"
    "float sw=dot(tap(uv+vec2(-1,1)*px),luma),se=dot(tap(uv+vec2(1,1)*px),luma),mid=dot(m,luma);\n"
    "float lo=min(mid,min(min(nw,ne),min(sw,se))),hi=max(mid,max(max(nw,ne),max(sw,se)));\n"
    "if(hi-lo<max(.0312,hi*.125)){color=vec4(m,1);return;}\n"
    "vec2 dir=vec2(-((nw+ne)-(sw+se)),(nw+sw)-(ne+se));float reduce=max((nw+ne+sw+se)/32.0,1.0/128.0);\n"
    "dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8),vec2(8))*px;\n"
    "vec3 a=.5*(tap(uv-dir/6.0)+tap(uv+dir/6.0));vec3 b=a*.5+.25*(tap(uv-dir*.5)+tap(uv+dir*.5));\n"
    "float lb=dot(b,luma);color=vec4(lb<lo||lb>hi?a:b,1);}";

static GLuint shader(GLenum type,const char *const *source,int count)
{
    GLuint result=glCreateShader(type);glShaderSource(result,count,source,NULL);glCompileShader(result);
    GLint ok=0;glGetShaderiv(result,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(result,sizeof log,NULL,log);void_log("[filter] compile failed: %s",log);glDeleteShader(result);return 0;}
    return result;
}
static GLuint program(unsigned kind)
{
    if(programs[kind]||(failed&(1u<<kind)))return programs[kind];
    const char *body=kind==0?"void main(){color=texture(image,gl_FragCoord.xy/target);}":
        kind==1?"void main(){color=FsrEasuH(gl_FragCoord.xy*vec2(textureSize(image,0))/target);color.a=1.0;}":
        kind==2?"void main(){color=FsrRcasH(.757858283);color.a=1.0;}":kind==3?fxaa:
        kind==4?"void main(){uint p=floatBitsToUint(texture(image,gl_FragCoord.xy/target).r);color=vec4(uvec4(p,p>>8u,p>>16u,p>>24u)&uvec4(255))/255.0;}":
        "uniform highp vec2 origin;void main(){vec2 uv=(gl_FragCoord.xy-origin)/target;color=vec4(texture(image,vec2(uv.x,1.0-uv.y)).rgb,1.0);}";
    const char *version=kind==5?"#version 300 es\n":"#version 310 es\n";
    const char *fragment[]={version,common,kind==1?fsr1_easu_glsl:kind==2?fsr1_rcas_glsl:"",body};
    const char *v[]={version,vertex};GLuint vs=shader(GL_VERTEX_SHADER,v,2),fs=shader(GL_FRAGMENT_SHADER,fragment,4),p=0;
    if(vs&&fs){p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){char log[2048];glGetProgramInfoLog(p,sizeof log,NULL,log);void_log("[filter] link failed: %s",log);d3d_gl_delete_program(p);p=0;}}
    if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);
    if(!p)failed|=1u<<kind;else void_log("[filter] program %u ready",kind);
    return programs[kind]=p;
}
static int storage(filter_target *t,int width,int height)
{
    if(t->texture&&t->width==width&&t->height==height)return 1;
    if(t->texture){d3d_gl_forget_texture(t->texture);glDeleteTextures(1,&t->texture);t->texture=0;}
    glGenTextures(1,&t->texture);glActiveTexture(GL_TEXTURE31);glBindTexture(GL_TEXTURE_2D,t->texture);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,width,height);t->width=width;t->height=height;
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->texture,0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}
static int draw(GLuint input,filter_target *output,int width,int height,unsigned kind)
{
    GLuint p=program(kind);if(!p||!storage(output,width,height))return 0;
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output->texture,0);
    glActiveTexture(GL_TEXTURE31);glBindTexture(GL_TEXTURE_2D,input);glBindSampler(31,sampler);
    glViewport(0,0,width,height);d3d_gl_use_program(p);glUniform1i(glGetUniformLocation(p,"image"),31);glUniform2f(glGetUniformLocation(p,"target"),width,height);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);return 1;
}

#include "d3d9_fsr3.inc"

static void filter_setup(void)
{
    if(!filter_vao)glGenVertexArrays(1,&filter_vao);
    if(!sampler){glGenSamplers(1,&sampler);
        glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);}
    glBindVertexArray(filter_vao);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);glDisable(GL_POLYGON_OFFSET_FILL);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
}

/* Some GLES drivers mishandle flipped framebuffer blits into Android windows. */
int d3d_filter_output(GLuint input,int x,int y,int width,int height)
{
    if(!input||width<1||height<1||width>8192||height>8192)return 0;
    static unsigned checked;
    int diagnose=checked++<32;
    GLenum prior=diagnose?glGetError():GL_NO_ERROR;
    GLuint p=program(5);if(!p)return 0;
    filter_setup();glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(x,y,width,height);
    glActiveTexture(GL_TEXTURE31);glBindTexture(GL_TEXTURE_2D,input);glBindSampler(31,sampler);
    d3d_gl_use_program(p);glUniform1i(glGetUniformLocation(p,"image"),31);
    glUniform2f(glGetUniformLocation(p,"target"),width,height);glUniform2f(glGetUniformLocation(p,"origin"),x,y);
    GLenum setup=diagnose?glGetError():GL_NO_ERROR;
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    GLenum drawn=diagnose?glGetError():GL_NO_ERROR;
    if(prior||setup||drawn)void_log("[filter] window errors before/setup/draw: %04X/%04X/%04X",prior,setup,drawn);
    return 1;
}

GLuint d3d_filter_present(GLuint input,int width,int height,int output_width,int output_height)
{
    int upscale=s2_render_upscaler(),aa=s2_render_aa();if(!upscale&&!aa)return 0;
    if(width<1||height<1||output_width<1||output_height<1||output_width>8192||output_height>8192)return 0;
    if(!framebuffer)glGenFramebuffers(1,&framebuffer);
    filter_setup();
    GLuint result=upscale==3?fsr3_present(input,width,height,output_width,output_height):0;
    glBindVertexArray(filter_vao);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    if(!result&&draw(input,targets,output_width,output_height,upscale?1:0)){
        result=targets[0].texture;
        if(upscale&&draw(result,targets+1,output_width,output_height,2))result=targets[1].texture;
    }
    if(result&&aa&&draw(result,targets+2,output_width,output_height,3))result=targets[2].texture;
    return result;
}
