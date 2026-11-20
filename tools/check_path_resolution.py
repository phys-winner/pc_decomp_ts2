"""Build a real-POSIX check of the actual files.c path resolver, without adb.

    python tools/check_path_resolution.py

The resulting Android executable makes/removes its own temporary fixture
under /data/local/tmp/sims2; run it on the target after loading captures finish.
"""
from pathlib import Path
import subprocess

import build_android as B

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/path-resolution'
OUT.mkdir(parents=True, exist_ok=True)
text = (ROOT / 'android/host/files.c').read_text()
globals_start = text.index('typedef struct { const char *win;')
globals_end = text.index('const char *void_root_dir(', globals_start)
overlay_start = text.index('typedef struct { char win[600];')
overlay_end = text.index('\n', text.index('static int overlay_count;', overlay_start))
start = text.index('const char *void_overlay_path(')
end = text.index('char *void_path_to_host(', start)
code = r'''
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define WIN_MAX_PATH 260
#include "dircache.h"
#include "iostat.h"
/* dircache.c and iostat.c are not linked: no listing is cached, so every
 * lookup takes the resolver's own file-system path, and nothing is counted. */
int dircache_lookup(const char *dir, const char *name, char *actual, size_t cap, unsigned char *type, int cached_only)
{ (void)dir; (void)name; (void)actual; (void)cap; (void)type; return cached_only ? DC_NOT_LOADED : DC_UNKNOWN; }
int dircache_stat(const char *path, struct stat *st) { return stat(path, st); }
int dircache_active(void) { return 0; }
uint64_t void_stat_now(void) { return 0; }
void void_stat_add(int kind, uint64_t ns, uint64_t bytes) { (void)kind; (void)ns; (void)bytes; }
static void void_blocking_begin(void) {}
static void void_blocking_end(void) {}
static unsigned probes, scans;
static int counted_lstat(const char *path, struct stat *st) { ++probes; return lstat(path, st); }
static DIR *counted_opendir(const char *path) { ++scans; return opendir(path); }
#define lstat counted_lstat
#define opendir counted_opendir
''' + text[globals_start:globals_end] + text[overlay_start:overlay_end] + '\n' + text[start:end] + r'''
#undef lstat
#undef opendir
static int failures;
static char fixture[1024], expected[2048];
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); ++failures; } } while (0)
static void host_path(char *out, size_t cap, const char *relative) { snprintf(out,cap,"%s/%s",fixture,relative); }
static void touch(const char *relative) {
    char path[1200]; host_path(path,sizeof path,relative);
    FILE *f=fopen(path,"wb"); CHECK(f!=NULL); if(f) { fputs("fixture",f); fclose(f); }
}
static void check(const char *input, const char *relative, int parent, const char *normal) {
    char norm[1024]={0}; int got_parent=-1;
    char *got=to_host(input,&got_parent,norm);
    host_path(expected,sizeof expected,relative);
    if(!got || strcmp(got,expected) || got_parent!=parent || (normal && strcmp(norm,normal))) {
        fprintf(stderr,"FAIL path %s -> %s / parent %d / norm %s; expected %s / parent %d\n",
                input,got?got:"NULL",got_parent,norm,expected,parent);
        ++failures;
    }
    free(got);
}
static void check_error(const char *input, int expected_errno, int expected_parent) {
    int parent=-1;
    char *path=to_host(input,&parent,NULL);
    CHECK(path!=NULL);
    if(!path) return;
    errno=0;
    int fd=open(path,O_RDONLY);
    int actual_errno=errno;
    CHECK(fd==-1);
    CHECK(actual_errno==expected_errno);
    CHECK(parent==expected_parent);
    if(fd>=0) close(fd);
    free(path);
}
int main(void) {
    char template[]="/data/local/tmp/sims2/s2-path-check-XXXXXX";
    char *created=mkdtemp(template); if(!created) { perror("mkdtemp"); return 2; }
    snprintf(fixture,sizeof fixture,"%s",created);
    static const char *directories[]={"game","game/EP9","game/EP9/TSData","game/EP9/TSData/Res",
        "game/EP9/TSData/Res/UI","game/EP9/TSData/Res/Config","drive_c","drive_c/Temp","overlay"};
    char path[1200], other[1200];
    for(size_t i=0;i<sizeof directories/sizeof *directories;++i) {
        host_path(path,sizeof path,directories[i]); CHECK(mkdir(path,0700)==0);
    }
    touch("game/EP9/TSData/Res/UI/ui.package");
    touch("game/EP9/TSData/Res/Config/savedProps.txt");
    touch("game/EP9/TSData/Res/UI/Caf\xC3\xA9.package");
    touch("game/EP9/not-a-directory");
    touch("overlay/ui.package");
    host_path(path,sizeof path,"game/EP9/Link"); CHECK(symlink("TSData",path)==0);
    host_path(path,sizeof path,"game/EP9/Dangling"); CHECK(symlink("absent",path)==0);
    mount_count=2;
    mounts[0].win="C:\\Games\\The Sims 2 Legacy"; host_path(mounts[0].host,sizeof mounts[0].host,"game");
    mounts[1].win="C:"; host_path(mounts[1].host,sizeof mounts[1].host,"drive_c");
    snprintf(cwd,sizeof cwd,"C:\\Games\\The Sims 2 Legacy\\EP9\\TSData\\Res");
    probes=scans=0;
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\TSData\\Res\\UI\\ui.package",
          "game/EP9/TSData/Res/UI/ui.package",1,NULL);
    printf("exact existing path: %u lstat probes, %u directory scans\n",probes,scans);
    check("c:/games/the sims 2 legacy/ep9/tsdata/res/ui/UI.PACKAGE",
          "game/EP9/TSData/Res/UI/ui.package",1,NULL);
    check("UI\\ui.package","game/EP9/TSData/Res/UI/ui.package",1,
          "C:\\Games\\The Sims 2 Legacy\\EP9\\TSData\\Res\\UI\\ui.package");
    check("UI\\.\\unused\\..\\ui.package","game/EP9/TSData/Res/UI/ui.package",1,NULL);
    check("UI//ui.package.  ","game/EP9/TSData/Res/UI/ui.package",1,NULL);
    check("C:UI\\ui.package","game/EP9/TSData/Res/UI/ui.package",1,NULL);
    check("\\\\?\\C:\\Games\\The Sims 2 Legacy\\EP9\\TSData\\Res\\UI\\ui.package",
          "game/EP9/TSData/Res/UI/ui.package",1,NULL);
    check("UI\\new.package","game/EP9/TSData/Res/UI/new.package",1,NULL);
    check("Missing\\new.package","game/EP9/TSData/Res/Missing/new.package",0,NULL);
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\not-a-directory\\leaf",
          "game/EP9/not-a-directory/leaf",1,NULL); /* preserve legacy parent flag; open reports ENOTDIR */
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\Link\\Res\\UI\\ui.package",
          "game/EP9/Link/Res/UI/ui.package",1,NULL);
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\lInK\\rEs\\uI\\ui.package",
          "game/EP9/Link/Res/UI/ui.package",1,NULL);
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\Dangling","game/EP9/Dangling",1,NULL);
    check("C:\\Games\\The Sims 2 Legacy\\EP9\\Dangling\\leaf","game/EP9/Dangling/leaf",1,NULL);
    check_error("UI\\new.package",ENOENT,1);
    check_error("Missing\\new.package",ENOENT,0);
    check_error("C:\\Games\\The Sims 2 Legacy\\EP9\\not-a-directory\\leaf",ENOTDIR,1);
    check_error("C:\\Games\\The Sims 2 Legacy\\EP9\\Dangling\\leaf",ENOENT,1);
    check("C:\\Temp\\new.file","drive_c/Temp/new.file",1,NULL);
    check("C:\\Games\\The Sims 2 Legacy","game",1,NULL);
    check("C:\\","drive_c",1,NULL);
    check("UI\\Caf\xC3\xA9.package","game/EP9/TSData/Res/UI/Caf\xC3\xA9.package",1,NULL);
    overlay_count=1;
    snprintf(overlays[0].win,sizeof overlays[0].win,"C:\\Games\\The Sims 2 Legacy\\EP9\\TSData\\Res\\UI\\ui.package");
    host_path(overlays[0].host,sizeof overlays[0].host,"overlay/ui.package");
    check("ui\\UI.PACKAGE","overlay/ui.package",1,NULL);
    overlay_count=0;
    check("UI\\Renew.pkg","game/EP9/TSData/Res/UI/Renew.pkg",1,NULL);
    touch("game/EP9/TSData/Res/UI/Renew.pkg");
    check("UI\\Renew.pkg","game/EP9/TSData/Res/UI/Renew.pkg",1,NULL);
    host_path(path,sizeof path,"game/EP9/TSData/Res/UI/Renew.pkg");
    host_path(other,sizeof other,"game/EP9/TSData/Res/UI/NEW.pkg"); CHECK(rename(path,other)==0);
    check("UI\\new.PKG","game/EP9/TSData/Res/UI/NEW.pkg",1,NULL);
    CHECK(unlink(other)==0);
    check("UI\\new.PKG","game/EP9/TSData/Res/UI/new.PKG",1,NULL);
    static const char *files[]={"overlay/ui.package","game/EP9/Link","game/EP9/Dangling",
        "game/EP9/not-a-directory","game/EP9/TSData/Res/UI/ui.package",
        "game/EP9/TSData/Res/UI/Caf\xC3\xA9.package","game/EP9/TSData/Res/Config/savedProps.txt"};
    for(size_t i=0;i<sizeof files/sizeof *files;++i) {
        host_path(path,sizeof path,files[i]); CHECK(unlink(path)==0);
    }
    for(size_t i=sizeof directories/sizeof *directories;i;--i) {
        host_path(path,sizeof path,directories[i-1]); CHECK(rmdir(path)==0);
    }
    CHECK(rmdir(fixture)==0);
    printf("%s: actual-C normalization, case fallback, symlinks, missing paths, overlays, fresh create/rename/delete (%d failures)\n",
           failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
'''
source=OUT/'check.c'
source.write_text(code)
binary=OUT/'check-path-resolution'
subprocess.run([str(B.CLANG)]+B.COMMON+['-Iandroid/host','-O2','-std=gnu11','-Wall','-Wextra','-Wno-unused-function',
                '-Wno-unused-variable',str(source),'-o',str(binary)],cwd=ROOT,check=True)
print(binary)
print('Ready for a target-side run; this script does not use adb or change the game build.')
