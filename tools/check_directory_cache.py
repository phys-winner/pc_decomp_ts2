"""Build an actual directory-cache regression for empty directories on first launch."""
import subprocess

import build_android as B


def main():
    out = B.ROOT/'build/test/directory-cache'
    out.mkdir(parents=True, exist_ok=True)
    source, binary = out/'check.c', out/'check-directory-cache'
    source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#define VOID_HOST_H
static char root[]="/data/local/tmp/sims2/cache-audit-XXXXXX";
const char *void_data_dir(void){return root;}
void void_log(const char *fmt,...){(void)fmt;}
_Noreturn void void_fatal(const char *fmt,...){(void)fmt;abort();}
''' + '#include "' + (B.ROOT/'android/host/dircache.c').as_posix() + '"\n' + r'''
static unsigned names;
static void each(void *arg,const char *name,unsigned char type){(void)arg;(void)type;assert(strcmp(name,".")&&strcmp(name,".."));++names;}
int main(void){
 assert(mkdtemp(root));char dir[1200],path[1400];struct stat actual,expected;
 snprintf(dir,sizeof dir,"%s/Empty",root);assert(!mkdir(dir,0700));
 assert(dircache_list(dir,each,NULL,0)&&!names);
 snprintf(path,sizeof path,"%s/.",dir);
 if(dircache_stat(path,&actual)){puts("FAIL cached empty-directory dot entry: first-run profile creation stops here");return 20;}
 assert(!stat(dir,&expected)&&actual.st_ino==expected.st_ino&&S_ISDIR(actual.st_mode));
 snprintf(path,sizeof path,"%s/..",dir);assert(!dircache_stat(path,&actual)&&!stat(root,&expected)&&actual.st_ino==expected.st_ino);
 snprintf(path,sizeof path,"%s/",dir);assert(!dircache_stat(path,&actual)&&S_ISDIR(actual.st_mode));
 snprintf(path,sizeof path,"%s/missing",dir);assert(dircache_stat(path,&actual)&&errno==ENOENT);
 snprintf(path,sizeof path,"%s/...",dir);assert(dircache_stat(path,&actual)&&errno==ENOENT);
 snprintf(path,sizeof path,"%s/new.txt",dir);FILE *f=fopen(path,"w");assert(f);assert(fputs("fixture",f)>=0&&!fclose(f));dircache_created(path,0);
 assert(!dircache_stat(path,&actual)&&actual.st_size==7);assert(dircache_list(dir,each,NULL,1)&&names==1);
 assert(!unlink(path));dircache_removed(path);assert(dircache_stat(path,&actual)&&errno==ENOENT);
 assert(!rmdir(dir)&&!rmdir(root));puts("PASS production directory cache: dot/parent/trailing slash, empty profile folders, negative entries and creation/removal notifications");return 0;
}
''')
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-Wl,-z,max-page-size=16384', str(source), str(B.ROOT/'android/host/zip_files.c'), str(B.ROOT/'android/launcher/native/zipmount.c'),
                    '-o', str(binary)], cwd=B.ROOT, check=True)
    print('Compiled first-run directory-cache probe:', binary)


if __name__ == '__main__':
    main()
