"""Actual file-path/enumeration C: slow host I/O yields, guest state stays owned.

Build the ARM64 fixture here, then execute it on the explicitly selected device.
"""
import subprocess
from build_android import ROOT, CLANG
from check_virtual_query import function

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define WIN_INVALID_HANDLE UINT32_MAX
#define ERROR_PATH_NOT_FOUND_ 3
#define ERROR_FILE_NOT_FOUND_ 2
#define ERROR_INVALID_HANDLE_ 6
#define WIN_MAX_PATH 260
#define VLOG(...) ((void)0)
#define VOID_OBJ_FIND 1
#include "dircache.h"
#include "iostat.h"
#include "readcache.h"
#include "zip_files.h"
static long long s2_zip_read_stub(const zm_entry *e,void *b,size_t n,uint64_t at){(void)e;(void)b;(void)n;(void)at;assert(0);return -1;}
#define s2_zip_read s2_zip_read_stub
/* dircache.c, iostat.c and readcache.c are not linked: nothing is cached, so
 * every lookup and listing takes the file system with the machine released,
 * and every read goes past the block cache. */
int dircache_lookup(const char *d,const char *n,char *a,size_t c,unsigned char *t,int cached_only)
{(void)d;(void)n;(void)a;(void)c;(void)t;return cached_only?DC_NOT_LOADED:DC_UNKNOWN;}
int dircache_stat(const char *p,struct stat *st){return stat(p,st);}
int dircache_list(const char *d,void (*each)(void *,const char *,unsigned char),void *arg,int cached_only)
{(void)d;(void)each;(void)arg;(void)cached_only;return 0;}
int dircache_active(void){return 0;}
uint64_t void_stat_now(void){return 0;}
void void_stat_add(int k,uint64_t ns,uint64_t b){(void)k;(void)ns;(void)b;}
void void_readcache_flush_dir(const char *d){(void)d;}
void void_readcache_flush_key(uint64_t k){(void)k;}
ssize_t void_readcache_pread(int fd,uint64_t k,void *o,size_t n,off_t at){(void)fd;(void)k;(void)o;(void)n;(void)at;return -2;}
static FILE *path_trace,*io_trace;
#define IOTRACE(...) do{if(0)fprintf(io_trace,__VA_ARGS__);}while(0)
static void files_flush_all(void){}
typedef struct {const char *win;char host[1024];} mount_t;
static mount_t mounts[8];static int mount_count=1;
typedef struct {char win[600];char host[1100];} overlay_t;
static overlay_t overlays[64];static int overlay_count;
static char cwd[260]="C:\\";
static pthread_mutex_t cwd_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char guest_memory[1024];
static int owned=1,begins,ends,error;
static void void_blocking_begin(void){assert(owned);owned=0;++begins;}
static void void_blocking_end(void){assert(!owned);owned=1;++ends;}
static void void_set_last_error(unsigned e){assert(owned);error=e;}
static void *guest(uint32_t p){assert(owned&&p<sizeof guest_memory);return guest_memory+p;}
#define GPTR(p) (guest_memory+(p))
#define MEM32(p) (*(uint32_t*)guest(p))
#define MEM64(p) (*(uint64_t*)guest(p))
static void void_utf8_to_ga(const char *s,uint32_t p,unsigned n){snprintf(guest(p),n,"%s",s);}
static void void_utf8_to_gw(const char *s,uint32_t p,unsigned n){assert(owned);uint16_t *d=guest(p);unsigned i;for(i=0;i+1<n&&s[i];++i)d[i]=(uint8_t)s[i];d[i]=0;}
typedef struct void_object {void *data;void (*destroy)(struct void_object*);} void_object;
static void_object *current;
static void_object *void_object_new(int kind,size_t size){assert(owned&&kind==VOID_OBJ_FIND);current=calloc(1,sizeof *current);current->data=calloc(1,size);return current;}
static void void_object_release(void_object *o){assert(owned);o->destroy(o);free(o->data);free(o);current=NULL;}
static unsigned void_handle_new(void_object *o){assert(owned&&o==current);return 7;}
typedef struct file_t {int fd,is_dir,std_stream;uint32_t wlen;int werr;struct file_t *wnext;
 uint64_t path_key;int cached_reads,lazy;off_t lazy_pos;const zm_entry *zip_entry;} file_t;
static file_t fixture_file,*dirty_files;
static int file_flush(file_t *f){(void)f;return 0;}
static int file_open_now(file_t *f,uint32_t h){(void)h;return f->fd>=0?0:-1;}
static uint32_t args[5];
#define ARG(i) args[i]
#define WINAPI_EXPORT(d,n,c) static uint32_t n##_impl(void)
static file_t *file_get(uint32_t h){assert(owned);return h==8?&fixture_file:NULL;}
static int void_vm_writable(uint32_t p,uint32_t n){assert(owned);return (uint64_t)p+n<=sizeof guest_memory;}
static unsigned void_errno_to_win32(int e){return e==EBADF?ERROR_INVALID_HANDLE_:5;}
static DIR *checked_opendir(const char *p){assert(!owned);return opendir(p);}
static struct dirent *checked_readdir(DIR *d){assert(!owned);return readdir(d);}
static int checked_lstat(const char *p,struct stat *s){assert(!owned);return lstat(p,s);}
#define opendir checked_opendir
#define readdir checked_readdir
#define lstat checked_lstat
'''
TEST = r'''
int main(void){
 char folder[]="/data/local/tmp/sims2/file-yield-XXXXXX";assert(mkdtemp(folder));
 char dir[1200],file[1300];snprintf(dir,sizeof dir,"%s/Dir",folder);assert(!mkdir(dir,0700));
 snprintf(file,sizeof file,"%s/Alpha.TXT",dir);FILE *out=fopen(file,"w");assert(out);fputs("audio",out);assert(!fclose(out));
 mounts[0].win="C:";snprintf(mounts[0].host,sizeof mounts[0].host,"%s",folder);
 int parent=0;char *path=to_host("c:\\dir\\alpha.txt",&parent,NULL);assert(parent&&!strcmp(path,file));free(path);
 assert(begins==1&&ends==1&&owned);
 path=to_host("C:\\Dir\\missing.txt",&parent,NULL);assert(parent&&strstr(path,"/missing.txt"));free(path);
 path=to_host("C:\\Absent\\missing.txt",&parent,NULL);assert(!parent);free(path);
 for(int wide=0;wide<2;++wide){
  assert(find_first(strdup("c:\\dir\\*.txt"),64,wide)==7&&owned&&begins==ends);
  assert(MEM32(64)==0x20&&MEM32(64+32)==5);
  if(wide)assert(*(uint16_t*)guest(64+44)=='A');else assert(!strcmp(guest(64+44),"Alpha.TXT"));
  void_object_release(current);
  assert(find_first(strdup("C:\\Dir\\*.none"),64,wide)==WIN_INVALID_HANDLE&&error==ERROR_FILE_NOT_FOUND_&&!current);
 }
 assert(find_first(strdup("C:\\Absent\\*.*"),64,0)==WIN_INVALID_HANDLE&&error==ERROR_PATH_NOT_FOUND_);
 fixture_file.fd=open(file,O_RDONLY);assert(fixture_file.fd>=0);
 args[0]=8;args[1]=768;args[2]=3;args[3]=16;args[4]=0;
 int before=begins;assert(ReadFile_impl()==1&&MEM32(16)==3&&!memcmp(GPTR(768),"aud",3)&&begins==before+1);
 args[2]=0;before=begins;assert(ReadFile_impl()==1&&MEM32(16)==0&&begins==before);
 args[2]=2;args[4]=32;MEM32(40)=3;MEM32(44)=0;
 assert(ReadFile_impl()==1&&MEM32(16)==2&&!memcmp(GPTR(768),"io",2)&&MEM32(36)==2);
 args[2]=2048;before=begins;assert(!ReadFile_impl()&&error==998&&begins==before);
 assert(!close(fixture_file.fd));
 assert(owned&&begins==ends);assert(!unlink(file)&&!rmdir(dir)&&!rmdir(folder));
 puts("PASS file audio yield: case-insensitive paths, ANSI/wide enumeration/failures, small/zero/overlapped/invalid reads; host I/O unlocked, guest state locked");
 return 0;
}
'''

def main():
    source=(ROOT/'android/host/files.c').read_text(encoding='utf-8')
    find_type=source[source.index('typedef struct {\n    char **names;'):source.index('static void find_destroy')]
    # A forward declaration starts each two-function slice (it runs to the
    # end of the function after it); the definitions follow by full header.
    names=('static int normalize(', 'static int resolve_component(', 'static char *to_host_uncounted(',
           'static char *to_host(', 'static int walk_components(',
           'static char *to_host_uncounted(const char *windows_path, int *parent_exists, char *normalized_out, int *type)\n{',
           'static void find_destroy(', 'static uint64_t unix_to_filetime(', 'static int fill_find_data(',
           'static uint32_t find_first_uncounted(',
           'static uint32_t find_first_uncounted(char *pattern_path, uint32_t out, int wide)\n{',
           'WINAPI_EXPORT(K32, ReadFile, 5)')
    out=ROOT/'build/test/file-audio-yield'
    out.mkdir(parents=True,exist_ok=True)
    fixture=out/'probe.c'
    fixture.write_text(PRELUDE+find_type+'\n'.join(function(source,s) for s in names)+TEST,encoding='utf-8')
    subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT/'android/host'),
                    '-Wl,-z,max-page-size=16384',str(fixture),'-o',str(out/'check-file-audio-yield')],check=True)
    print(out/'check-file-audio-yield')

if __name__=='__main__':main()
