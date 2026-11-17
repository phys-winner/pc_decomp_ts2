"""Exercise production GLES setters: unchanged values and state-block recording."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_device.c').read_text()
    constants = source[source.index('#define CONST_SET('):source.index('#define CONST_GET(')]
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3D_SOFTWARE_VS_CONSTANTS 8192u
#define D3D_SOFTWARE_VS_INT_CONSTANTS 2048u
#define D3D_SOFTWARE_VS_BOOL_CONSTANTS 2048u
typedef struct {
 uint32_t rs[256],tss[8][33],ss[20][14],light_enable[8];
 uint32_t vs_f[8192][4],vs_i[2048][4],vs_b[2048];
 uint32_t ps_f[224][4],ps_i[16][4],ps_b[16];
} d3d_state;
typedef struct {
 d3d_state state;
 uint8_t rec_rs[256],rec_tss[8][33],rec_ss[20][14],rec_light[8];
 uint8_t rec_vs_f[8192],rec_vs_i[2048],rec_vs_b[2048];
 uint8_t rec_ps_f[224],rec_ps_i[16],rec_ps_b[16];
} block;
typedef struct { d3d_state st; block *recording; unsigned constants_dirty; } d3d_device;
static d3d_device device;
static block recorded;
static uint32_t args[5],input[16],dirty_calls,dirty_offset,dirty_bytes;
#define DEV (&device)
#define ARG(i) args[i]
#define GPTR(at) ((void)assert((at)==16), (void *)input)
#define COM_METHOD(p,n,c) static uint32_t p##_##n(void)
static void d3d_dirty_range(d3d_device *d,size_t offset,size_t bytes){
 assert(d==&device&&!d->recording);++dirty_calls;dirty_offset=(uint32_t)offset;dirty_bytes=(uint32_t)bytes;
}
#define d3d_dirty_field(d,field) d3d_dirty_range(d,offsetof(d3d_state,field),sizeof((d)->st.field))
#define DIRTY(field) do { if(!d->recording)d3d_dirty_field(d,field); } while(0)
static void void_log(const char *format,...){assert(format);}
static int d3d_trace_active(void){return 0;}
#define ICALL_TRACE_SIZE 64
static uint32_t g_icall_trace_idx,g_icall_from[ICALL_TRACE_SIZE];
'''
    def line(s):
        result = next(r for r in source.splitlines() if r.startswith(s))
        assert result.endswith('}')
        return result

    actual = line('static d3d_state *target_state(') + '\n'
    actual += '\n'.join(function(source, s) for s in (
        'static int sampler_slot(',
        'COM_METHOD(dev, SetRenderState, 3)', 'COM_METHOD(dev, SetTextureStageState, 4)',
        'COM_METHOD(dev, SetSamplerState, 4)', 'COM_METHOD(dev, LightEnable, 3)'))
    actual += '\n' + constants
    actual += '\n' + '\n'.join(line(s) for s in (
        'COM_METHOD(dev, SetVertexShaderConstantF, 4)', 'COM_METHOD(dev, SetVertexShaderConstantI, 4)',
        'COM_METHOD(dev, SetVertexShaderConstantB, 4)', 'COM_METHOD(dev, SetPixelShaderConstantI, 4)',
        'COM_METHOD(dev, SetPixelShaderConstantB, 4)'))
    actual += '\n' + function(source, 'COM_METHOD(dev, SetPixelShaderConstantF, 4)')
    checks = r'''
static void clean(void){dirty_calls=0;device.constants_dirty=0;}
int main(void){
 args[1]=7;args[2]=0;assert(!dev_SetRenderState()&&!dirty_calls);
 args[2]=99;assert(!dev_SetRenderState()&&dirty_calls==1&&device.st.rs[7]==99);
 assert(dirty_offset==offsetof(d3d_state,rs[7])&&dirty_bytes==4);
 clean();for(unsigned i=0;i<10000;++i)assert(!dev_SetRenderState()&&!dirty_calls);
 args[1]=256;assert(!dev_SetRenderState()&&!dirty_calls);
 args[1]=3;args[2]=32;args[3]=0;assert(!dev_SetTextureStageState()&&!dirty_calls);
 args[3]=UINT32_MAX;assert(!dev_SetTextureStageState()&&dirty_calls==1&&device.st.tss[3][32]==UINT32_MAX);
 clean();assert(!dev_SetTextureStageState()&&!dirty_calls);
 args[1]=8;assert(!dev_SetTextureStageState()&&!dirty_calls);
 args[1]=0;args[2]=33;assert(!dev_SetTextureStageState()&&!dirty_calls);
 args[1]=257;args[2]=13;args[3]=0;assert(!dev_SetSamplerState()&&!dirty_calls);
 args[3]=17;assert(!dev_SetSamplerState()&&dirty_calls==1&&device.st.ss[16][13]==17);
 clean();assert(!dev_SetSamplerState()&&!dirty_calls);
 args[1]=255;assert(!dev_SetSamplerState()&&!dirty_calls);
 args[1]=15;args[2]=14;assert(!dev_SetSamplerState()&&!dirty_calls);
 args[1]=7;args[2]=2;assert(!dev_LightEnable()&&dirty_calls==1&&device.st.light_enable[7]==1);
 clean();args[2]=128;assert(!dev_LightEnable()&&!dirty_calls);
 args[1]=8;assert(!dev_LightEnable()&&!dirty_calls);

 /* A repeated value must still be captured into an explicitly recorded block. */
 device.recording=&recorded;recorded.state=device.st;
 args[1]=7;args[2]=99;assert(!dev_SetRenderState()&&recorded.rec_rs[7]&&!dirty_calls);
 args[1]=3;args[2]=32;args[3]=UINT32_MAX;
 assert(!dev_SetTextureStageState()&&recorded.rec_tss[3][32]&&!dirty_calls);
 args[1]=257;args[2]=13;args[3]=17;
 assert(!dev_SetSamplerState()&&recorded.rec_ss[16][13]&&!dirty_calls);
 args[1]=7;args[2]=2;assert(!dev_LightEnable()&&recorded.rec_light[7]&&!dirty_calls);
 device.recording=NULL;

 uint32_t (*setters[])(void)={dev_SetVertexShaderConstantF,dev_SetVertexShaderConstantI,
     dev_SetVertexShaderConstantB,dev_SetPixelShaderConstantF,dev_SetPixelShaderConstantI,dev_SetPixelShaderConstantB};
 uint32_t *dest[]={&device.st.vs_f[0][0],&device.st.vs_i[0][0],device.st.vs_b,
     &device.st.ps_f[0][0],&device.st.ps_i[0][0],device.st.ps_b};
 uint8_t *rec[]={recorded.rec_vs_f,recorded.rec_vs_i,recorded.rec_vs_b,recorded.rec_ps_f,recorded.rec_ps_i,recorded.rec_ps_b};
 unsigned width[]={4,4,1,4,4,1},limit[]={8192,2048,2048,224,16,16};
 for(unsigned n=0;n<6;++n){
  args[1]=limit[n]-1;args[2]=16;args[3]=1;memset(input,0,sizeof input);clean();
  assert(!setters[n]()&&!dirty_calls&&!device.constants_dirty);
  /* Float comparisons must retain payload bits, signed zero and NaNs. */
  input[0]=0x80000000;input[1]=0x7FC01234;input[2]=0xFFFFFFFF;input[3]=0x3F800000;
  assert(!setters[n]()&&dirty_calls==1&&device.constants_dirty);
  assert(!memcmp(dest[n]+(limit[n]-1)*width[n],input,width[n]*4));
  assert(dirty_bytes==width[n]*4);clean();
  assert(!setters[n]()&&!dirty_calls&&!device.constants_dirty);
  device.recording=&recorded;recorded.state=device.st;
  assert(!setters[n]()&&rec[n][limit[n]-1]&&!dirty_calls&&device.constants_dirty);
  device.recording=NULL;clean();
  args[3]=2;assert(setters[n]()==D3DERR_INVALIDCALL&&!dirty_calls&&!device.constants_dirty);
  args[1]=limit[n]+1;args[3]=0;assert(setters[n]()==D3DERR_INVALIDCALL&&!dirty_calls);
  args[1]=limit[n];assert(!setters[n]()&&!dirty_calls&&!device.constants_dirty);
 }
 puts("PASS production state setters: 10000 repeats skip dirty work; changed values, bounds, vertex sampler slots, normalized booleans, exact float bits and unchanged state-block capture");
 return 0;
}
'''
    compile_probe('redundant-state', prelude + actual + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
