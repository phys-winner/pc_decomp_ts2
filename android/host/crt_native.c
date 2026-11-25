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

/* The scene graph's material command parser (a lot's zoom-out, rotation and
 * floor-change frames: 70% material creation on the Huawei POT-LX1,
 * 2026-11-25) scans its text with these four; lifted, each byte cost a
 * dozen guest operations (the SSE2/AVX2 forms through per-instruction
 * emulation). Same contracts as the originals; ECX/EDX are scratch to every
 * caller. */

/* strncmp (ucrt strncmp.asm): 0 when equal within the count or up to the
 * terminator, else -1 or 1 by the first differing byte, unsigned (sbb/or). */
void sub_A08318B0_lifted(void);
void sub_A08318B0(void)
{
    const uint8_t *a = (const uint8_t *)GPTR(MEM32(g_esp + 4)), *b = (const uint8_t *)GPTR(MEM32(g_esp + 8));
    uint32_t count = MEM32(g_esp + 12), result = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t x = a[i], y = b[i];
        if (x != y) { result = x < y ? 0xFFFFFFFFu : 1u; break; }
        if (!x) break;
    }
    g_eax = result;
    g_esp += 4;
}

/* memchr (vcruntime memchr.asm): the first byte equal to (unsigned char)value
 * in the count, or NULL. */
void sub_A0828340_lifted(void);
void sub_A0828340(void)
{
    uint32_t buf = MEM32(g_esp + 4), count = MEM32(g_esp + 12), result = 0;
    if (count) {
        const uint8_t *p = (const uint8_t *)GPTR(buf);
        const uint8_t *hit = (const uint8_t *)memchr(p, (int)(MEM32(g_esp + 8) & 0xFFu), count);
        if (hit) result = buf + (uint32_t)(hit - p);
    }
    g_eax = result;
    g_esp += 4;
}

/* strcspn (ucrt strcspn.asm): the length of the leading span without any
 * byte of the control string (a 256-bit set, so bytes above 0x7F count). */
void sub_A08413C0_lifted(void);
void sub_A08413C0(void)
{
    g_eax = (uint32_t)strcspn((const char *)GPTR(MEM32(g_esp + 4)), (const char *)GPTR(MEM32(g_esp + 8)));
    g_esp += 4;
}

/* std::find for a byte range (__std_find_trivial_impl<_Find_traits_1,
 * unsigned char>, __stdcall first, last, value, `retn 0Ch`): the first byte
 * equal to value in [first, last), else last. */
void sub_A08212F0_lifted(void);
void sub_A08212F0(void)
{
    uint32_t first = MEM32(g_esp + 4), last = MEM32(g_esp + 8), result = last;
    if (last > first) {
        const uint8_t *p = (const uint8_t *)GPTR(first);
        const uint8_t *hit = (const uint8_t *)memchr(p, (int)(MEM32(g_esp + 12) & 0xFFu), last - first);
        if (hit) result = first + (uint32_t)(hit - p);
    }
    g_eax = result;
    g_esp += 16;
}
