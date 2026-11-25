/* The embedded D3DX R8G8B8 row writer (format 20). Transforms and error
 * diffusion stay in the lifted body. The ordinary finite [0, 1] path keeps
 * the x87 chop stores, the ordered-dither bias, the backward row on odd x,
 * and the bytes the frame, the control word and the x87 window still hold
 * when the original returns. */
#include "host.h"

void sub_A00390A6_lifted(void);

static int rgb24_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

/* fstp dword under RC=chop. A host cast follows FPCR, so a result that
 * rounded up moves back one ULP. The products and the k/32 bias sums below
 * are exact in double. */
static float rgb24_float_rz(double v)
{
    union { float f; uint32_t u; } r = { .f = (float)v };
    if ((double)r.f > v) --r.u;
    return r.f;
}

static uint32_t rgb24_clamp(int32_t v)
{
    if (v <= 0) return 0;
    if (v >= 255) return 255u;
    return (uint32_t)v;
}

/* 1 when this row was written here. 0 leaves every guest byte and the host
 * FP flags as they were, so the caller runs the lifted body. */
static int rgb24_row(void)
{
    uint32_t self = g_ecx, sp = g_esp, in = MEM32(sp + 12);
    uint32_t n = MEM32(self + 0x68), x = MEM32(sp + 4), y = MEM32(sp + 8);
    const uint32_t frame = 0x60u;
    if ((self | sp | in) & 3u || !n || n > 4096u || sp < frame || sp > UINT32_MAX - 16u ||
        self > UINT32_MAX - 128u || MEM32(self + 4) != 20u ||
        MEM32(self + 0x54) || MEM32(self + 0x10) || MEM32(self + 0x5C) ||
        (g_fpu_cw & 0x3Fu) != 0x3Fu || (g_fpu_cw & 0x300u) == 0x100u ||
        MEM32(0xA0D9C79Cu) != 0x437F0000u) return 0;
    uintptr_t fp = (uintptr_t)g_stp;
    if ((fp & 7u) || fp < (uintptr_t)(g_stbuf + 2) || fp > (uintptr_t)(g_stbuf + X87_WINDOW - 10)) return 0;
    int depth = X87_DEPTH(g_stp);
    if (depth < 0 || depth > 6) return 0;
    uint32_t table = MEM32(self + 0x34);
    if (table != 0xA10518B0u && table != 0xA1051930u) return 0;
    uint64_t start = (uint64_t)MEM32(self + 0x20) +
        (uint64_t)MEM32(self + 0x60) * x + (uint64_t)MEM32(self + 0x64) * y;
    if (start > UINT32_MAX - n * 3u || in > UINT32_MAX - n * 16u) return 0;
    uint32_t out = (uint32_t)start, bias = table + 4u * ((y & 3u) + 8u * (x & 3u));
    uint32_t at[6] = { self, sp - frame, in, out, table, 0xA0D9C79Cu };
    uint32_t bytes[6] = { 128, frame + 16u, n * 16u, n * 3u, 128, 4 };
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (rgb24_overlap(at[i], bytes[i], at[j], bytes[j])) return 0;
    /* Integer patterns only, before any rounding or guest store. The row
     * reads three channels and at most four bias entries. */
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        uint32_t b = MEM32(bias + 4u * i);
        if (b < 0x3D000000u || b > 0x3F780000u) return 0;
        unsigned shift = 145u - (b >> 23);
        if ((b & 0x7FFFFFu) & ((1u << shift) - 1u)) return 0;
    }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < 3u; ++c) {
            uint32_t v = MEM32(in + 16u * i + 4u * c);
            if (v && (v < 0x33800000u || v > 0x3F800000u)) return 0;
        }

    uint64_t fpsr = void_host_fpsr();
    int32_t dir = (x & 1u) ? -1 : 1;
    uint32_t first = (x & 1u) ? n - 1u : 0u;
    uint8_t packed[4096 * 3];
    float scaled[3] = {0}, biased[3] = {0};
    int32_t raw[3] = {0};
    double red_sum = 0;
    uint32_t zeros = 0, bias_bits = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (x & 1u) ? n - 1u - i : i;
        bias_bits = MEM32(bias + 4u * (i & 3u));
        float d = *(float *)GPTR(bias + 4u * (i & 3u));
        for (uint32_t c = 0; c < 3u; ++c) {
            float v = *(float *)GPTR(in + 16u * j + 4u * c);
            scaled[c] = rgb24_float_rz((double)v * 255.0);
            double sum = (double)scaled[c] + (double)d;
            biased[c] = rgb24_float_rz(sum);
            raw[c] = (int32_t)biased[c];
            if (!c) red_sum = sum;
        }
        /* Memory order is B, G, R. Channel 2 is the blue byte at [edi]. */
        packed[3u * j] = (uint8_t)rgb24_clamp(raw[2]);
        packed[3u * j + 1u] = (uint8_t)rgb24_clamp(raw[1]);
        packed[3u * j + 2u] = (uint8_t)rgb24_clamp(raw[0]);
        /* A blue result of 0 takes the backward branch at 0xA00393A1 as
         * well as the loop branch, so that pixel contributes two back-edges. */
        if (raw[2] <= 0) ++zeros;
    }
    /* The blue-zero branch and the loop branch are the row's back-edges.
     * A hook yields on the edge that reaches RECOMP_YIELD_EVERY, so the
     * whole row stays in the lifted body when that edge is one of these. */
    uint32_t edges = n + zeros;
    if (recomp_yield_hook && (edges >= RECOMP_YIELD_EVERY ||
        g_backedges >= RECOMP_YIELD_EVERY - edges)) {
        void_host_fpsr_set(fpsr);
        return 0;
    }

    uint32_t _flag_k, _flag_a, _flag_b, _cf;
    RECOMP_ENTER(0xA00390A6u);
    (void)_flag_k; (void)_flag_a; (void)_flag_b; (void)_cf;
    for (uint32_t i = 0; i < n * 3u; ++i) MEM8(out + i) = packed[i];

    /* Prologue saves, the last iteration's chop results, and the epilogue's
     * control-word image. Slots the original never stores stay untouched. */
    uint32_t bp = sp - 4u;
    uint32_t saved = (y & 0xFFFF0000u) | (g_fpu_cw & 0xFFFFu);
    MEM32(bp) = g_ebp;
    MEM32(bp - 0x54u) = g_ebx;
    MEM32(bp - 0x58u) = g_esi;
    MEM32(bp - 0x5Cu) = g_edi;
    MEM32(bp - 0x50u) = MEM32(bp - 0x4Cu) = MEM32(bp - 0x48u) = 0;
    for (unsigned c = 0; c < 3; ++c) *(float *)GPTR(bp - 0x40u + 4u * c) = scaled[c];
    MEM32(bp - 0x30u) = rgb24_clamp(raw[0]);
    MEM32(bp - 0x2Cu) = rgb24_clamp(raw[1]);
    MEM32(bp - 0x28u) = (uint32_t)raw[2];
    MEM32(bp - 0x20u) = (uint32_t)(dir * 16);
    MEM32(bp - 0x1Cu) = (uint32_t)(dir * 3);
    MEM32(bp - 0x18u) = (uint32_t)raw[2];
    MEM32(bp - 0x14u) = (uint32_t)raw[1];
    MEM32(bp - 0x10u) = (uint32_t)raw[0];
    *(float *)GPTR(bp - 0xCu) = biased[2];
    MEM32(bp - 8u) = bias;
    MEM32(bp - 4u) = bias_bits;
    MEM32(bp + 8u) = n;
    MEM32(bp + 0xCu) = (first + (n - 1u) * (uint32_t)dir) * 16u;
    MEM32(bp + 0x10u) = saved;
    MEM32(self + 0x7Cu) = saved;
    /* Single precision leaves the chopped red sum; wider precision leaves
     * the exact sum. The blue fistp reloads the chopped float into the
     * other popped slot. Both shadows stay clear. */
    g_stp[-1] = (double)biased[2];
    g_stp[-2] = (g_fpu_cw & 0x300u) ? red_sum : (double)biased[0];
    X87_SHADOW(g_stp)[-1] = 0;
    X87_SHADOW(g_stp)[-2] = 0;
    void_host_fpsr_set(fpsr);
    g_eax = saved;
    g_ecx = 16u * (first + (n + 1u) * (uint32_t)dir);
    g_edx = n;
    g_esp = sp + 16u;
    g_flag_k = FK_CMP; g_flag_a = g_flag_b = n; g_flag_cf = 0;
    g_backedges += edges;
    return 1;
}

static void __attribute__((no_profile_instrument_function)) sims2_rgb24_row(void)
{
    if (!rgb24_row()) sub_A00390A6_lifted();
}
void sub_A00390A6(void) __attribute__((alias("sims2_rgb24_row")));
