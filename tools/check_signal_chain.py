"""Build a probe that faults while holding the log mutex and reaches the prior handler."""
import subprocess

import build_android as B
from check_virtual_query import function


def main():
    text = (B.ROOT / 'android/host/core.c').read_text()
    prelude = r'''
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#define VOID_GUEST_BASE 0x2000000000ull
#define VOID_GUEST_SPAN 0x100000000ull
static pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static atomic_int log_in_signal,dumping;
static struct sigaction previous_actions[SIGSYS+1];
static void void_dump_state(const char *why){(void)why;}
'''
    functions = '\n'.join(function(text, name) for name in (
        'static void log_va(', 'void void_log(const char *fmt, ...)', 'void void_log_flush(',
        'static void on_signal(', 'void void_install_fault_handlers('))
    checks = r'''
static void previous(int signal){_exit(signal==SIGABRT?71:72);}
int main(void){
 signal(SIGABRT,previous);void_install_fault_handlers();
 pthread_mutex_lock(&log_lock);raise(SIGABRT);return 73;
}
'''
    out = B.ROOT / 'build/test/signal-chain'
    out.mkdir(parents=True, exist_ok=True)
    source, binary = out/'check.c', out/'check-signal-chain'
    source.write_text(prelude + functions + checks)
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-Wl,-z,max-page-size=16384',
                    str(source), '-llog', '-o', str(binary)], cwd=B.ROOT, check=True)
    print('PASS signal probe compilation; selected-device execution must return 71 without hanging:', binary)


if __name__ == '__main__':
    main()
