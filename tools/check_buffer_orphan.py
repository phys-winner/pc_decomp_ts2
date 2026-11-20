"""Execute production buffer Lock/flush/upload/destroy across a delayed GL queue."""
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
#define atomic_store(p,value) __c11_atomic_store(p,value,5)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLenum,GLuint;
#define GL_COPY_WRITE_BUFFER 1u
#define GL_RENDERER 2u
#define GL_DYNAMIC_DRAW 3u
#define GL_STATIC_DRAW 4u
#define GL_STREAM_DRAW 5u
#define GL_MAP_WRITE_BIT 8u
#define GL_MAP_UNSYNCHRONIZED_BIT 16u
#define D3DUSAGE_DYNAMIC 0x200u
#define D3DLOCK_READONLY 0x10u
#define D3DLOCK_DISCARD 0x2000u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DERR_INVALIDCALL 0x8876086cu
#define D3D_OK 0u
typedef struct {unsigned placeholder;} void_com_object;
typedef struct {
 void_com_object com;
 uint32_t gl,size,usage,shadow;
 int locks;
 uint32_t dirty_lo,dirty_hi;
 int discard_pending,overwrite_pending;
 void *storage;
} d3d_buffer;
/* the native renderers are off in these probes (s2r.h) */
static int s2r_on(void){return 0;}
__attribute__((unused)) static void s2r_buffer_upload(d3d_buffer *b,uint32_t lo,uint32_t n,int whole,int unsync,const uint8_t *src){(void)b;(void)lo;(void)n;(void)whole;(void)unsync;(void)src;assert(0);}
__attribute__((unused)) static void s2r_buffer_create(d3d_buffer *b){(void)b;assert(0);}
__attribute__((unused)) static void s2r_buffer_release(d3d_buffer *b){(void)b;assert(0);}

static _Atomic int buffer_subdata=-1,buffer_persistent=0;
static void buffer_storage_free(d3d_buffer *b){(void)b;assert(0);}
static int buffer_storage_upload(d3d_buffer *b,int whole,int unsynchronized,uint32_t lo,uint32_t length,const uint8_t *src){
 (void)b;(void)whole;(void)unsynchronized;(void)lo;(void)length;(void)src;assert(0);return 0;
}
static unsigned char memory[131072],gpu[65537];
#define GPTR(a) (memory+(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
static uint32_t args[5];
#define ARG(n) args[n]
static d3d_buffer *active;
#define BUF active
#define COM_METHOD(group,name,count) static uint32_t group##_##name(void)
static uint64_t stat_buffers,stat_buffer_bytes;
uint64_t d3d_buffer_bytes,d3d_buffer_spare_bytes,d3d_buffer_uploads;
static const char *fixture_renderer;
static unsigned strings,stores,subdata,maps,deleted,freed;
static unsigned expected_size,expected_offset,expected_length,expected_usage;
static const unsigned char *glGetString(GLenum key){
 assert(key==GL_RENDERER);++strings;return (const unsigned char *)fixture_renderer;
}
static void glBindBuffer(GLenum target,GLuint name){assert(target==GL_COPY_WRITE_BUFFER&&name==77);}
static void glBufferData(GLenum target,uint32_t bytes,const void *data,GLenum usage){
 assert(target==GL_COPY_WRITE_BUFFER&&bytes==expected_size&&expected_offset==0&&expected_length==bytes);
 assert(bytes<=sizeof gpu&&data&&usage==expected_usage);++stores;memcpy(gpu,data,bytes);
}
static void glBufferSubData(GLenum target,uint32_t offset,uint32_t bytes,const void *data){
 assert(target==GL_COPY_WRITE_BUFFER&&offset==expected_offset&&bytes==expected_length&&data);
 assert(offset<=expected_size&&bytes<=expected_size-offset);++subdata;memcpy(gpu+offset,data,bytes);
}
static void *glMapBufferRange(GLenum target,uint32_t offset,uint32_t bytes,unsigned flags){
 assert(target==GL_COPY_WRITE_BUFFER&&offset==expected_offset&&bytes==expected_length);
 assert(flags==(GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT));
 assert(offset<=expected_size&&bytes<=expected_size-offset);++maps;return gpu+offset;
}
static int glUnmapBuffer(GLenum target){assert(target==GL_COPY_WRITE_BUFFER);return 1;}
static void d3d_gl_forget_buffer(GLuint name){assert(name==77);}
static void glDeleteBuffers(unsigned count,const GLuint *name){assert(count==1&&*name==77);++deleted;}
static void void_gfree(uint32_t address){assert(address==256);memset(GPTR(address),0xDD,expected_size);++freed;}
typedef void (*render_fn)(void *);
static struct {render_fn fn;void *payload;uint32_t bytes;} queue[8];
static unsigned queued,open_record;
static void *d3d_rt_begin(render_fn fn,uint32_t bytes){
 assert(!open_record&&queued<8);open_record=1;
 queue[queued].fn=fn;queue[queued].bytes=bytes;queue[queued].payload=malloc(bytes);
 assert(queue[queued].payload);return queue[queued].payload;
}
static void d3d_rt_commit(void){assert(open_record);open_record=0;++queued;}
static void d3d_rt_call(render_fn fn,const void *args_,uint32_t bytes){
 memcpy(d3d_rt_begin(fn,bytes),args_,bytes);d3d_rt_commit();
}
static void consume(unsigned i){assert(i<queued);queue[i].fn(queue[i].payload);free(queue[i].payload);queue[i].payload=NULL;}
'''
    actual = declaration + '\n'.join(function(source, signature) for signature in (
        'static void run_buffer_upload(', 'void d3d_buffer_flush(',
        'COM_METHOD(buf, Lock, 5)', 'COM_METHOD(buf, Unlock, 1)',
        'static void run_buffer_destroy(', 'static void buffer_destroy(',
    ))
    checks = r'''
static d3d_buffer *make_buffer(unsigned size,unsigned dynamic){
 d3d_buffer *b=calloc(1,sizeof *b);assert(b);
 b->gl=77;b->size=size;b->usage=dynamic?D3DUSAGE_DYNAMIC:0;b->shadow=256;b->dirty_lo=UINT32_MAX;
 for(unsigned i=0;i<size;++i)memory[256+i]=(uint8_t)(i*37u+11u);
 memset(gpu,0xCD,sizeof gpu);active=b;expected_size=size;
 stores=subdata=maps=deleted=freed=queued=open_record=0;
 d3d_buffer_bytes=d3d_buffer_spare_bytes=d3d_buffer_uploads=0;
 stat_buffers=1;stat_buffer_bytes=size;
 return b;
}
static void write_lock(unsigned offset,unsigned length,unsigned flags,unsigned byte){
 args[1]=offset;args[2]=length;args[3]=4;args[4]=flags;
 assert(buf_Lock()==D3D_OK&&MEM32(4)==256+offset);
 if(!(flags&D3DLOCK_READONLY))memset(GPTR(MEM32(4)),(int)byte,length);
 assert(buf_Unlock()==D3D_OK&&active->locks==0);
}
static void matrix(unsigned size,unsigned dynamic,int driver,unsigned flags,unsigned whole){
 d3d_buffer *b=make_buffer(size,dynamic);atomic_store(&buffer_subdata,driver);
 fixture_renderer=driver?"PowerVR D-Series DXT-48-1536":"Mali-G715";
 unsigned old_strings=strings;
 write_lock(17,23,flags,0xA6);
 if(flags&D3DLOCK_READONLY){
  assert(queued==0&&d3d_buffer_bytes==0&&d3d_buffer_spare_bytes==0&&d3d_buffer_uploads==0);
  assert(b->dirty_lo==UINT32_MAX);free(b);return;
 }
 assert(queued==1&&!open_record);
 buffer_upload_args *a=queue[0].payload;
 expected_offset=whole?0:17;expected_length=whole?size:23;
 expected_usage=whole==2?GL_STREAM_DRAW:dynamic?GL_DYNAMIC_DRAW:GL_STATIC_DRAW;
 assert(a->whole==whole&&a->lo==expected_offset&&a->length==expected_length);
 assert(queue[0].bytes==sizeof *a+expected_length);
 assert(d3d_buffer_uploads==1&&d3d_buffer_bytes==expected_length);
 assert(d3d_buffer_spare_bytes==(whole?size-23:0));
 assert(b->dirty_lo==UINT32_MAX&&b->dirty_hi==0&&!b->discard_pending&&!b->overwrite_pending);
 /* A queued upload owns its bytes; later guest writes cannot alter it. */
 uint8_t expected[65537];memcpy(expected,GPTR(256),size);memset(GPTR(256),0xEE,size);
 consume(0);
 assert(stores==(unsigned)(whole!=0));
 assert(maps==(unsigned)(!whole&&driver==0&&(flags&D3DLOCK_NOOVERWRITE)!=0));
 assert(subdata==(unsigned)(!whole&&!maps));
 assert(strings==old_strings+(driver<0));
 for(unsigned i=0;i<size;++i)
  assert(gpu[i]==((whole||(i>=17&&i<40))?expected[i]:0xCD));
 free(b);
}
static void ordered_lifetime(void){
 d3d_buffer *b=make_buffer(128,1);atomic_store(&buffer_subdata,1);
 uint8_t first[128],second[128];
 write_lock(17,23,D3DLOCK_NOOVERWRITE,0x31);memcpy(first,GPTR(256),128);
 write_lock(64,16,D3DLOCK_NOOVERWRITE,0x72);memcpy(second,GPTR(256),128);
 buffer_destroy(&b->com);assert(queued==3&&freed==1&&!b->shadow&&!deleted);
 expected_offset=0;expected_length=128;expected_usage=GL_STREAM_DRAW;
 consume(0);assert(memcmp(gpu,first,128)==0&&!deleted);
 consume(1);assert(memcmp(gpu,second,128)==0&&!deleted);
 assert(d3d_buffer_bytes==256&&d3d_buffer_spare_bytes==(128-23)+(128-16));
 consume(2);assert(deleted==1&&stat_buffers==0&&stat_buffer_bytes==0);
}
int main(void){
 for(unsigned dynamic=0;dynamic<2;++dynamic)
  for(int driver=0;driver<2;++driver){
   matrix(128,dynamic,driver,D3DLOCK_NOOVERWRITE,dynamic&&driver?2:0);
   matrix(128,dynamic,driver,0,0);
   matrix(128,dynamic,driver,D3DLOCK_READONLY,0);
   matrix(128,dynamic,driver,D3DLOCK_DISCARD,1);
  }
 matrix(65536,1,1,D3DLOCK_NOOVERWRITE,2);
 matrix(65537,1,1,D3DLOCK_NOOVERWRITE,0);
 /* Persistent-capable contexts keep the narrow enqueue-time snapshot. */
 atomic_store(&buffer_persistent,1);
 matrix(128,1,1,D3DLOCK_NOOVERWRITE,0);
 atomic_store(&buffer_persistent,0);
 /* First upload may precede driver detection; it must remain a valid narrow record. */
 matrix(128,1,-1,D3DLOCK_NOOVERWRITE,0);
 ordered_lifetime();
 puts("PASS production PowerVR orphan: exact preserved bytes, enqueue-time snapshots, FIFO lifetime through guest free, 64KiB boundary, READONLY, static/other-driver/overwrite/DISCARD unchanged, extra-byte accounting");
 return 0;
}
'''
    compile_probe('buffer-orphan', prelude + actual + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
