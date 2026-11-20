"""Execute production buffer uploads with GL spies for PowerVR, Mali and Adreno."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/d3d9_resource.c').read_text(encoding='utf-8')
    declaration = source[source.index('typedef struct { d3d_buffer *b; uint32_t lo, length, whole, unsynchronized; }'):
                         source.index('static void run_buffer_upload(')]
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#define memory_order_relaxed 0
#define atomic_load_explicit(p,order) __c11_atomic_load(p,order)
#define atomic_store_explicit(p,value,order) __c11_atomic_store(p,value,order)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLenum;
#define GL_COPY_WRITE_BUFFER 1u
#define GL_RENDERER 2u
#define GL_DYNAMIC_DRAW 3u
#define GL_STATIC_DRAW 4u
#define GL_STREAM_DRAW 5u
#define GL_MAP_WRITE_BIT 8u
#define GL_MAP_UNSYNCHRONIZED_BIT 16u
#define D3DUSAGE_DYNAMIC 1u
typedef struct {uint32_t gl,size,usage;void *storage;} d3d_buffer;
/* the native renderers are off in these probes (s2r.h) */
static int s2r_on(void){return 0;}
__attribute__((unused)) static void s2r_buffer_upload(d3d_buffer *b,uint32_t lo,uint32_t n,int whole,int unsync,const uint8_t *src){(void)b;(void)lo;(void)n;(void)whole;(void)unsync;(void)src;assert(0);}
__attribute__((unused)) static void s2r_buffer_create(d3d_buffer *b){(void)b;assert(0);}
__attribute__((unused)) static void s2r_buffer_release(d3d_buffer *b){(void)b;assert(0);}
static _Atomic int buffer_subdata=-1;
static int buffer_storage_upload(d3d_buffer *b,int whole,int unsynchronized,uint32_t lo,uint32_t length,const uint8_t *src){
 (void)b;(void)whole;(void)unsynchronized;(void)lo;(void)length;(void)src;assert(0);return 0;
}
static uint8_t gpu[128];
static const char *fixture_renderer;
static unsigned strings,binds,stores,maps,unmaps,subdata,map_failure;
static unsigned expected_offset,expected_length,expected_usage;
static const unsigned char *glGetString(GLenum key){
 assert(key==GL_RENDERER);++strings;return (const unsigned char *)fixture_renderer;
}
static void glBindBuffer(GLenum target,unsigned name){assert(target==GL_COPY_WRITE_BUFFER&&name==77);++binds;}
static void glBufferData(GLenum target,uint32_t bytes,const void *data,GLenum usage){
 assert(target==GL_COPY_WRITE_BUFFER&&bytes==sizeof gpu&&expected_length==bytes&&expected_offset==0);
 assert(data&&usage==expected_usage);++stores;memcpy(gpu,data,bytes);
}
static void glBufferSubData(GLenum target,uint32_t offset,uint32_t bytes,const void *data){
 assert(target==GL_COPY_WRITE_BUFFER&&offset==expected_offset&&bytes==expected_length&&data);
 assert(offset<=sizeof gpu&&bytes<=sizeof gpu-offset);++subdata;memcpy(gpu+offset,data,bytes);
}
static void *glMapBufferRange(GLenum target,uint32_t offset,uint32_t bytes,unsigned flags){
 assert(target==GL_COPY_WRITE_BUFFER&&offset==expected_offset&&bytes==expected_length);
 assert(flags==(GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT));
 assert(offset<=sizeof gpu&&bytes<=sizeof gpu-offset);++maps;return map_failure?NULL:gpu+offset;
}
static int glUnmapBuffer(GLenum target){assert(target==GL_COPY_WRITE_BUFFER);++unmaps;return 1;}
'''
    checks = r'''
static void upload(unsigned whole,unsigned nooverwrite,unsigned dynamic,int expected_map,int fail){
 d3d_buffer buffer={77,sizeof gpu,dynamic?D3DUSAGE_DYNAMIC:0,NULL};
 struct {buffer_upload_args a;uint8_t bytes[128];} call;
 memset(gpu,0xCD,sizeof gpu);
 expected_offset=whole?0:17;expected_length=whole?sizeof gpu:23;
 expected_usage=whole==2?GL_STREAM_DRAW:dynamic?GL_DYNAMIC_DRAW:GL_STATIC_DRAW;
 call.a=(buffer_upload_args){&buffer,expected_offset,expected_length,whole,nooverwrite};
 for(unsigned i=0;i<sizeof call.bytes;++i)call.bytes[i]=(uint8_t)(i+1);
 binds=stores=maps=unmaps=subdata=0;map_failure=(unsigned)fail;
 run_buffer_upload(&call);
 assert(binds==1&&maps==(unsigned)expected_map&&unmaps==(unsigned)(expected_map&&!fail));
 assert(stores==(unsigned)(whole!=0)&&subdata==(unsigned)(!whole&&(!expected_map||fail)));
 for(unsigned i=0;i<sizeof gpu;++i){
  uint8_t expected=i>=expected_offset&&i<expected_offset+expected_length?call.bytes[i-expected_offset]:0xCD;
  assert(gpu[i]==expected);
 }
}
static void driver(const char *renderer,int prefer_subdata){
 fixture_renderer=renderer;buffer_subdata=-1;strings=0;
 upload(0,1,1,!prefer_subdata,0);assert(strings==1);
 upload(0,1,1,!prefer_subdata,1);assert(strings==1);
 upload(0,0,1,0,0);assert(strings==1);
 for(unsigned dynamic=0;dynamic<2;++dynamic){
  upload(1,0,dynamic,0,0);upload(1,1,dynamic,0,0);assert(strings==1);
 }
 upload(2,1,1,0,0);assert(strings==1);
 /* The detected driver is immutable for this context, not queried per upload. */
 fixture_renderer=prefer_subdata?"Mali-G715":"PowerVR D-Series";
 upload(0,1,1,!prefer_subdata,0);assert(strings==1);
}
int main(void){
 driver("PowerVR D-Series DXT-48-1536",1);
 driver("Mali-G715",0);driver("Adreno (TM) 735",0);
 driver("Unknown GLES",0);driver(NULL,0);
 puts("PASS production buffer upload: PowerVR SubData, Mali/Adreno NOOVERWRITE map, map-failure fallback, unchanged static/dynamic DISCARD, one renderer query, exact range bytes");
 return 0;
}
'''
    compile_probe('buffer-upload', prelude + declaration + function(source, 'static void run_buffer_upload(') + checks,
                  {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
