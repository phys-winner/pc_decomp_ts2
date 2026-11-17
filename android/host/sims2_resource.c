/* DBPF group enumeration: reject a whole constant-group bucket natively.
 * The separately compiled original owns allocation, sorting and output records. */
#include "host.h"
void sub_A01F3D30(void);
void sub_A09AC040(void), sub_A09AC040_native(void);
void sims2_resource_enumerate(void);
int void_native_call_allowed(uint32_t va, recomp_func_t expected);
static int skip_enabled = 1;
static uint64_t skip_rows, skip_batches;

void s2_resource_skip_enable(int enabled)
{
    skip_enabled = enabled != 0;
    void_log("[resource-skip] %s rows %llu batches %llu dispatch %s", skip_enabled ? "on" : "off",
        (unsigned long long)skip_rows, (unsigned long long)skip_batches,
        void_dispatch_find(0xA09AC040u) == sims2_resource_enumerate ? "native wrapper" : "original/unknown");
}

void s2_resource_skip_stats(uint64_t *rows, uint64_t *batches) { *rows = skip_rows; *batches = skip_batches; }
static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return (uint64_t)a + an > b && (uint64_t)b + bn > a; }

uint32_t s2_resource_skip(uint32_t sp, uint32_t bucket, uint32_t at, uint32_t end)
{
    if (!skip_enabled || end <= at || end - at < 64 || !void_native_call_allowed(0xA01F3D30u, sub_A01F3D30) ||
        sp < 8 || sp > 0xFFFFFF9Fu || end <= at || ((end - at) & 15) || ((sp | bucket | at | end) & 3) ||
        !void_vm_writable(sp - 8, 92) || !void_vm_readable(sp + 0x5C, 4) || !void_vm_readable(bucket, 8)) return 0;
    uint32_t predicate = MEM32(sp + 0x5C), bytes = end - at;
    if (!predicate || !void_vm_readable(predicate, 12) || overlap(predicate, 12, sp - 8, 92) ||
        overlap(bucket, 8, sp - 8, 92) || overlap(at, bytes, sp - 8, 92)) return 0;
    uint32_t vtable = MEM32(predicate);
    if (!void_vm_readable(vtable, 16) || overlap(vtable, 16, sp - 8, 92) || MEM32(vtable + 12) != 0xA01F3D30u) return 0;
    uint32_t group = MEM32(bucket + 4), wanted = MEM32(predicate + 8);
    if (group == wanted || !void_vm_readable(at, bytes)) return 0;
    uint32_t skipped = bytes >> 4;
    if (recomp_yield_hook) {
        if (g_backedges >= RECOMP_YIELD_EVERY - 1) return 0;
        uint32_t room = RECOMP_YIELD_EVERY - g_backedges - 1;
        if (skipped > room) skipped = room;
    }
    uint32_t last = at + (skipped - 1) * 16;
    MEM32(sp + 0x44) = MEM32(bucket); MEM32(sp + 0x48) = group;
    MEM32(sp + 0x4C) = MEM32(last); MEM32(sp + 0x50) = MEM32(last + 4);
    MEM32(sp - 4) = sp + 0x44; MEM32(sp - 8) = RECOMP_RETADDR;
    /* The original callback's global lazy flags differ from the loop's locals. */
    g_flag_k = FK_CMP; g_flag_a = group; g_flag_b = wanted; g_flag_cf = group < wanted;
    g_icall_trace[ICALL_TRACE_SIZE - 1] = 0xA01F3D30u; g_icall_from[ICALL_TRACE_SIZE - 1] = 0xA09AC040u;
    g_backedges += skipped;
    skip_rows += skipped; ++skip_batches;
    return skipped;
}

/* OFF executes the unchanged, PGO-named v9 function, without a per-row probe. */
void sims2_resource_enumerate(void)
{
    uint32_t sp = g_esp;
    if (!skip_enabled || !void_native_call_allowed(0xA01F3D30u, sub_A01F3D30) ||
        sp > 0xFFFFFFF4u || !void_vm_readable(sp + 8, 4)) goto original;
    uint32_t predicate = MEM32(sp + 8);
    if (!predicate || !void_vm_readable(predicate, 12)) goto original;
    uint32_t vtable = MEM32(predicate);
    if (!void_vm_readable(vtable, 16) || MEM32(vtable + 12) != 0xA01F3D30u) goto original;
    sub_A09AC040_native();
    return;
original:
    sub_A09AC040();
}
