/* Wall-time counters per host service family (iostat.c). */
#ifndef VOID_IOSTAT_H
#define VOID_IOSTAT_H

#include <stdint.h>

enum {
    VS_PATH, VS_OPEN, VS_READ, VS_WRITE, VS_ATTR, VS_FIND, VS_FINDNEXT, VS_COPY, VS_DIROP, VS_CLOSE,
    VS_SLEEP, VS_WAIT, VS_MACHINE, VS_COUNT
};

uint64_t void_stat_now(void);
void void_stat_add(int kind, uint64_t ns, uint64_t bytes);
void void_stat_main_thread(void);   /* the calling thread is the game's main guest thread */
void void_stat_start(void);
int void_is_main_guest_thread(void);  /* threads.c */

#define VSTAT_BEGIN(name) uint64_t name = void_stat_now()
#define VSTAT_END(name, kind, bytes) void_stat_add((kind), void_stat_now() - (name), (bytes))

#endif
