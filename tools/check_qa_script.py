"""Execute the production script entry point: player profiles never read QA commands."""
import _ctypes
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    source = function((ROOT/'android/host/script.c').read_text(), 'void void_input_script_start(')
    prelude = r'''
typedef unsigned long long size_t;
typedef void FILE;
#define NULL ((void *)0)
#define F_OK 0
typedef struct { int action; } script_event;
static const char *data;
static int reads, starts, environment;
const char *void_data_dir(void) { return data; }
static char *strrchr(const char *s, int c) { const char *last=0; for (;*s;s++) if (*s==c) last=s; return (char *)last; }
static int strncmp(const char *a,const char *b,size_t n) { while(n--){if(*a!=*b)return 1;if(!*a)return 0;a++;b++;}return 0; }
static char *getenv(const char *s) { (void)s; ++environment; return "qa-command"; }
static char *strdup(const char *s) { return (char *)s; }
static int access(const char *s,int f) { (void)s;(void)f;++reads;return 0; }
static int snprintf(char *b,size_t n,const char *f,...) { (void)n;(void)f;b[0]=0;return 0; }
static FILE *fopen(const char *s,const char *m) { (void)s;(void)m;++reads;return 0; }
static void *calloc(size_t n,size_t s) { (void)n;(void)s;return 0; }
static char *fgets(char *b,int n,FILE *f) { (void)b;(void)n;(void)f;return 0; }
static int fclose(FILE *f) { (void)f;return 0; }
static int parse_line(char *s,script_event *e,int timed) { (void)s;(void)e;(void)timed;return 0; }
static void void_log(const char *s,...) { (void)s; }
static void *live_thread(void *p) { return p; }
static void *script_thread(void *p) { return p; }
static void start_detached(void *(*fn)(void *),void *p) { (void)fn;(void)p;++starts; }
'''
    wrapper = r'''
__declspec(dllexport) int check(const char *profile) {
    data=profile; reads=starts=environment=0; void_input_script_start();
    return (reads?1:0)|(starts?2:0)|(environment?4:0);
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-qa-script-') as directory:
        folder = Path(directory)
        code, obj, dll = folder/'check.c', folder/'check.obj', folder/'check.dll'
        code.write_text(prelude + source + wrapper)
        subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-builtin',
                        '-fno-stack-protector','-c',str(code),'-o',str(obj)],check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry',
                        '/out:'+str(dll),str(obj)],check=True)
        library = ctypes.CDLL(str(dll))
        library.check.argtypes = [ctypes.c_char_p]
        try:
            for name in ('/sdcard/Sims2/userdata','userdata','/sdcard/Sims2/userdata-player',
                         '/sdcard/Sims2/userdata-qa-','/sdcard/Sims2/userdata-qa-old/userdata',''):
                assert library.check(name.encode()) == 0, name
            for name in ('/sdcard/Sims2/userdata-qa-audit','userdata-qa-test'):
                assert library.check(name.encode()) == 7, name
        finally:
            _ctypes.FreeLibrary(library._handle)
    print('PASS: normal/empty/non-QA profiles cannot read files, environment or start command threads; isolated QA still works')


if __name__ == '__main__':
    main()
