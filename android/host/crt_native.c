/* Native replacements for the static VS2022 CRT's memory and string
 * primitives in Sims2EP9.exe (found by their opening bytes, tools notes in
 * STATE.md). The lifted bodies go through per-instruction SSE2 emulation and
 * jump tables for the tail bytes; bionic's versions are NEON. Same contracts:
 * cdecl, the result in EAX, callee-saved registers untouched.
 *
 * tools/build_android.py NATIVE_OVERRIDES renames the lifted bodies to
 * <name>_lifted; every call and the dispatch table bind to these. */
#include "host.h"
#include <string.h>

/* memcpy and memmove share one body (vcruntime memcpy.asm):
 * 57 56 8B 74 24 10 8B 4C 24 14 8B 7C 24 0C = push edi; push esi; mov esi,[esp+10h]; ... */
void sub_A08264A0_lifted(void);
void sub_A08264A0(void)
{
    uint32_t dst = MEM32(g_esp + 4), src = MEM32(g_esp + 8), count = MEM32(g_esp + 12);
    if (count) memmove(GPTR(dst), GPTR(src), count);
    g_eax = dst;
    g_esp += 4;   /* ret: the caller pops the arguments */
}

/* memset (vcruntime memset.asm): 8B 4C 24 0C 0F B6 44 24 08 8B D7 8B 7C 24 04. */
void sub_A08281E0_lifted(void);
void sub_A08281E0(void)
{
    uint32_t dst = MEM32(g_esp + 4), value = MEM32(g_esp + 8), count = MEM32(g_esp + 12);
    if (count) memset(GPTR(dst), (int)(value & 0xFFu), count);
    g_eax = dst;
    g_esp += 4;
}

/* strlen (ucrt strlen.asm): 8B 4C 24 04 F7 C1 03 00 00 00 74 ... */
void sub_A0831280_lifted(void);
void sub_A0831280(void)
{
    g_eax = (uint32_t)strlen((const char *)GPTR(MEM32(g_esp + 4)));
    g_esp += 4;
}
