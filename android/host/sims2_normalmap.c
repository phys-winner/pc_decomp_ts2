/* D3DXComputeNormalMap's pixel loop, native.
 *
 * Sims2EP9.exe links D3DX statically; its D3DXComputeNormalMap (0xA001D884)
 * turns the bump maps of materials into normal maps while a lot's objects
 * are first drawn: at the first frame of a lot, at the first zoom-out and at
 * a floor change. Each row's height samples sit in three padded float rows
 * ([ebp-0xC] the row above, [ebp-0x10] this row, [ebp-0x18] the row below;
 * element 0 is the wrapped last texel, element w+1 the wrapped first). The
 * x87 loop at 0xA001DD67-0xA001E0F6 then derives each texel's slopes, crosses
 * the two tangents, normalizes through D3DX's CPU-selected D3DXVec3Normalize
 * (the SSE2 body 0xA00BA930), optionally darkens by occlusion, and packs a
 * float4 row that the guest's row writer stores. Lifted, that loop was the
 * single hottest function of a lot's long frames (a fifth of the game thread
 * in the first full lot frame on the Huawei POT-LX1, about half of the first
 * zoom-out on the Honor 9 Lite).
 *
 * tools/build_android.py calls s2_normal_map_row at the loop's entry with the
 * registers published. It computes the row with the lifted code's arithmetic
 * (x87 as double in the same order, fstp roundings, the lifter's fdivp/fdivrp
 * divide-by-zero rule, the SSE2 normalize in float with rsqrtss as
 * 1/sqrtf), then leaves what the loop leaves: the frame's locals, the callee
 * frame of the last normalize call, the x87 slots under the stack top and the
 * SSE registers of that call, the scratch registers and the back-edge count.
 * Anything it does not recognise returns 0 and the lifted loop runs. */
#include "host.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define NM_NORMALIZE_SLOT 0xA105012Cu     /* D3DX's function table, D3DXVec3Normalize */
#define NM_NORMALIZE_SSE2 0xA00BA930u
#define NM_FRAME 0x1F0u                   /* ebp - esp inside the row loop */

typedef struct {
    const float *p, *c, *n;               /* padded rows above, at, below */
    float *out;
    int w, h, y, mu, mv, occ, inv, unorm;
    float amp, thr, three, half;
} nm_row;

typedef struct {
    double t1, t2, t3, t4;                /* x87 slots ST-1..ST-4 under the loop's top */
    double dx;
    uint32_t mem4;                        /* [ebp-4]: the dy count, or the occlusion term */
    float dxf, dyf, c[3], v[3], d[8];
    float sq, s1, s2, x0, x1, x2, f1, t6, t7;
    int full;                             /* the normalize took its rsqrt path */
    uint32_t comi;                        /* its comiss result */
} nm_pixel_state;

static inline uint32_t nm_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* The lifted fdivp by the sample count: 0 leaves the dividend (the lifter's
 * rule), 2 and 4 scale by an exact power of two (the same single rounding as
 * the division), 3 divides. */
static inline double nm_average(double v, int count)
{
    if (count == 4) return v * 0.25;
    if (count == 2) return v * 0.5;
    if (count == 3) return v / 3.0;
    return v;   /* 1: no division; 0: the lifter keeps the dividend */
}

/* One texel; `record` (constant at each call) keeps the state the loop leaves. */
static inline __attribute__((always_inline)) void nm_pixel(const nm_row *r, int x, nm_pixel_state *s, const int record)
{
    const float *p = r->p + x, *c = r->c + x, *n = r->n + x;   /* [0] texel x-1, [1] x, [2] x+1 */
    const int w = r->w, h = r->h, y = r->y, mu = r->mu, mv = r->mv;
    const int left = x != 0 || !mu, right = x < w - 1 || !mu;
    const int up = y != 0 || !mv, down = y < h - 1 || !mv;
    /* dx (0xA001DDA2-0xA001DE55): the rows above and below. */
    double t1 = 0.0, t2;
    int count = 0;
    if (left) {
        if (up) { t1 = (double)p[1]; count = 1; t1 += (double)p[0]; }
        if (down) { t2 = t1; t2 += (double)n[1]; ++count; t2 += (double)n[0]; t1 = t2; }
    }
    if (right) {
        if (up) { t2 = (double)p[2]; ++count; t2 += (double)p[1]; t1 -= t2; }
        if (down) { t2 = (double)n[2]; ++count; t2 += (double)n[1]; t1 -= t2; }
    }
    t1 *= (double)r->amp;
    t1 = nm_average(t1, count);
    const double dx = t1;
    /* dy (0xA001DE57-0xA001DEED): this row against the rows around it. */
    count = 0;
    t2 = 0.0;
    if (up) {
        if (left) { t2 = (double)c[0]; count = 1; t2 += (double)p[0]; }
        if (right) { double t3 = (double)c[2]; ++count; t3 += (double)p[2]; t2 += t3; }
    }
    if (down) {
        if (left) { double t3 = (double)n[0]; ++count; t3 += (double)c[0]; t2 -= t3; }
        if (right) { double t3 = (double)n[2]; ++count; t3 += (double)c[2]; t2 -= t3; }
    }
    t2 *= (double)r->amp;
    t2 = nm_average(t2, count);
    const float dxf = (float)dx, dyf = (float)t2;
    /* The tangents A = (-1, 0, dx) and B = (0, -1, dy) and their cross product
     * (0xA001DEEF-0xA001DF6C); every product of two floats is exact in double. */
    const double ax = -1.0, ay = 0.0, bx = 0.0, by = -1.0;
    const float cx = (float)((double)dyf * ay - by * (double)dxf);
    const float cy = (float)((double)dxf * bx - (double)dyf * ax);
    const double z1 = by * ax, z2 = ay * bx;
    const float cz = (float)(z1 - z2);
    /* D3DXVec3Normalize, SSE2 (0xA00BA930), as lifted: rsqrtss is 1/sqrtf. */
    float x0 = cx, x1 = cy, x2 = cz;
    float sq = x0 * x0, s1 = x1 * x1, s2 = x2 * x2;
    sq = sq + s1;
    sq = sq + s2;
    uint32_t comi = sq < r->thr ? 0x01u : sq > r->thr ? 0x00u : sq == r->thr ? 0x40u : 0x45u;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f, f1 = 0.0f, t6 = 0.0f, t7 = 0.0f;
    int full = !(comi & 1u);
    if (full) {
        float rs = 1.0f / sqrtf(sq);
        t6 = rs * sq;
        t6 = t6 * rs;
        t7 = rs * r->half;
        f1 = r->three - t6;
        f1 = f1 * t7;
        vx = x0 * f1; vy = x1 * f1; vz = x2 * f1;
    }
    /* Occlusion (0xA001DF81-0xA001E046). */
    double occ = 1.0, t3 = dx, t4 = 0.0, val = 0.0, q = 0.0;
    uint32_t mem4 = (uint32_t)count;
    float d[8];
    if (r->occ) {
        d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = c[0];
        d[4] = c[2]; d[5] = n[0]; d[6] = n[1]; d[7] = n[2];
        const double c0 = (double)c[1];
        double sum = 0.0;
        for (int i = 0; i < 8; ++i) {
            double diff = (double)d[i] - c0;
            d[i] = (float)diff;
            if (diff < 0.0) d[i] = 0.0f;
            sum = sum + (double)d[i];
        }
        t4 = sum;
        t3 = c0;
        val = sum * (double)r->amp;
        val = val * 0.125;
        const float o = (float)val;
        mem4 = nm_bits(o);
        t2 = val;
        if (val > 0.0) {
            q = (double)o * (double)o;
            q = q + 1.0;
            q = sqrt(q);
            double num = q - (double)o;
            t2 = num;
            occ = q == 0.0 ? num : num / q;
        }
    } else {
        t2 = z2;
    }
    /* Packing (0xA001E048-0xA001E0EA). */
    float *out = r->out + 4 * x;
    if (r->unorm) {
        double ox, oy, oz;
        if (r->inv) {
            ox = 0.5 - (double)vx * 0.5; oy = 0.5 - (double)vy * 0.5; oz = 0.5 - (double)vz * 0.5;
        } else {
            ox = ((double)vx + 1.0) * 0.5; oy = ((double)vy + 1.0) * 0.5; oz = ((double)vz + 1.0) * 0.5;
        }
        out[0] = (float)ox; out[1] = (float)oy; out[2] = (float)oz; out[3] = (float)occ;
        t1 = occ; t2 = 0.5; t3 = oz;
    } else {
        out[0] = vx; out[1] = vy; out[2] = vz; out[3] = (float)occ;
        t1 = occ;
        if (r->inv) {
            out[0] = (float)((double)vx * -1.0);
            out[1] = (float)((double)vy * -1.0);
            double nz = (double)vz * -1.0;
            out[2] = (float)nz;
            t1 = -1.0; t2 = nz;
        }
    }
    s->full = full;
    if (full) { s->t6 = t6; s->t7 = t7; }
    if (!record) return;
    s->t1 = t1; s->t2 = t2; s->t3 = t3; s->t4 = t4;
    s->dx = dx; s->mem4 = mem4; s->dxf = dxf; s->dyf = dyf;
    s->c[0] = cx; s->c[1] = cy; s->c[2] = cz;
    s->v[0] = vx; s->v[1] = vy; s->v[2] = vz;
    if (r->occ) memcpy(s->d, d, sizeof d);
    s->sq = sq; s->s1 = s1; s->s2 = s2; s->x0 = x0; s->x1 = x1; s->x2 = x2;
    s->comi = comi;
    if (full) s->f1 = f1;
    (void)val; (void)q;
}

/* The game's case (no mirror, occlusion or invert flags, an unsigned
 * destination): every texel takes all four sample pairs, so the texels
 * before the last need no branches and no recorded state. The same
 * operations in the same order as nm_pixel; the normalize's threshold test is
 * a select. */
static inline __attribute__((always_inline)) void nm_plain_texel(const float *p, const float *c, const float *n, double amp,
                                                                 float thr, float three, float half, float *sq_out,
                                                                 float *t6_out, float *t7_out, float v[3])
{
    const double p0 = p[0], p1 = p[1], p2 = p[2];
    const double c0 = c[0], c2 = c[2];
    const double n0 = n[0], n1 = n[1], n2 = n[2];
    double t1 = p1 + p0;
    t1 = t1 + n1;
    t1 = t1 + n0;
    t1 = t1 - (p2 + p1);
    t1 = t1 - (n2 + n1);
    t1 = (t1 * amp) * 0.25;
    double t2 = c0 + p0;
    t2 = t2 + (c2 + p2);
    t2 = t2 - (n0 + c0);
    t2 = t2 - (n2 + c2);
    t2 = (t2 * amp) * 0.25;
    const float dxf = (float)t1, dyf = (float)t2;
    const float cx = (float)((double)dyf * 0.0 - -1.0 * (double)dxf);
    const float cy = (float)((double)dxf * 0.0 - (double)dyf * -1.0);
    const float cz = (float)(-1.0 * -1.0 - 0.0 * 0.0);
    float sq = cx * cx, s1 = cy * cy, s2 = cz * cz;
    sq = sq + s1;
    sq = sq + s2;
    const int full = sq >= thr;                       /* comiss/jae: ordered and not below */
    const float rs = 1.0f / sqrtf(sq);
    float t6 = rs * sq;
    t6 = t6 * rs;
    const float t7 = rs * half;
    float f1 = three - t6;
    f1 = f1 * t7;
    v[0] = full ? cx * f1 : 0.0f; v[1] = full ? cy * f1 : 0.0f; v[2] = full ? cz * f1 : 0.0f;
    *sq_out = full ? sq : -1.0f;                      /* -1: the zero path */
    *t6_out = t6; *t7_out = t7;
}

/* Texels [0, count); returns 1 if any took the rsqrt path, with the last such texel's t6/t7. */
static int nm_plain(const nm_row *r, int count, float *t6_out, float *t7_out)
{
    const float *__restrict p = r->p, *__restrict c = r->c, *__restrict n = r->n;
    float *__restrict out = r->out;
    const double amp = (double)r->amp;
    const float thr = r->thr, three = r->three, half = r->half;
    for (int x = 0; x < count; ++x) {
        float sq, t6, t7, v[3];
        nm_plain_texel(p + x, c + x, n + x, amp, thr, three, half, &sq, &t6, &t7, v);
        float *o = out + 4 * x;
        o[0] = (float)(((double)v[0] + 1.0) * 0.5);
        o[1] = (float)(((double)v[1] + 1.0) * 0.5);
        o[2] = (float)(((double)v[2] + 1.0) * 0.5);
        o[3] = 1.0f;
    }
    /* xmm6/xmm7 keep the last texel that took the rsqrt path: usually the last one. */
    for (int x = count - 1; x >= 0; --x) {
        float sq, t6, t7, v[3];
        nm_plain_texel(p + x, c + x, n + x, amp, thr, three, half, &sq, &t6, &t7, v);
        if (sq != -1.0f) { *t6_out = t6; *t7_out = t7; return 1; }
    }
    return 0;
}

static int nm_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

static void nm_store(uint32_t address, float value) { MEM32(address) = nm_bits(value); }

static int nm_run(int may_yield)
{
    static int checked, usable, logged;
    if (!checked) {
        checked = 1;
        /* The loop's x87 constants (-1, 0, 1/8, 1, 1/2) and the SSE2 body's
         * (threshold, 3, 1/2) are .rdata; a different build of the exe keeps
         * its lifted loop. */
        usable = MEM32(0xA0DA03E8u) == 0xBF800000u && MEM32(0xA0D9E82Cu) == 0 &&
                 MEM32(0xA0DA74F4u) == 0x3E000000u && MEM32(0xA0D9C698u) == 0x3F800000u &&
                 MEM32(0xA0D9C694u) == 0x3F000000u && MEM32(0xA0E2E710u) == 0x40400000u &&
                 MEM32(0xA0E2E720u) == 0x3F000000u && MEM32(0xA0E2E730u) == 0x28800000u;
        if (!usable) void_log("[normalmap] constants differ: lifted pixel loop kept");
    }
    if (!usable || MEM32(NM_NORMALIZE_SLOT) != NM_NORMALIZE_SSE2) return 0;
    const uint32_t bp = g_ebp, sp = g_esp;
    if (sp != bp - NM_FRAME || sp < 0x10000u || bp > 0xFFFF0000u || !void_vm_writable(sp - 16u, NM_FRAME + 16u + 0x20u))
        return 0;
    const int32_t w = (int32_t)MEM32(bp - 0x4Cu), h = (int32_t)MEM32(bp - 0x48u), y = (int32_t)MEM32(bp - 8u);
    if (w <= 0 || w > 32768 || h <= 0 || y < 0 || y >= h) return 0;
    const uint32_t flags = MEM32(bp + 0x14u), codec = MEM32(bp - 0x30u);
    const uint32_t prev = MEM32(bp - 0xCu), cur = MEM32(bp - 0x10u), next = MEM32(bp - 0x18u), out = MEM32(bp - 0x20u);
    const uint32_t padded = 4u * (uint32_t)w + 8u, row = 16u * (uint32_t)w;
    if (!void_vm_readable(prev, padded) || !void_vm_readable(cur, padded) || !void_vm_readable(next, padded) ||
        !void_vm_writable(out, row) || !void_vm_readable(codec + 8u, 4)) return 0;
    const uint32_t frame = sp - 16u, frame_size = NM_FRAME + 16u + 0x20u;
    if (nm_overlap(out, row, prev, padded) || nm_overlap(out, row, cur, padded) || nm_overlap(out, row, next, padded) ||
        nm_overlap(out, row, frame, frame_size) || nm_overlap(prev, padded, frame, frame_size) ||
        nm_overlap(cur, padded, frame, frame_size) || nm_overlap(next, padded, frame, frame_size)) return 0;
    const int occ = (flags & 0x100000u) != 0;
    /* The lifted loop's back edges: one a texel, eight more with occlusion.
     * A row that would reach a yield keeps the lifted loop, which yields at
     * its exact place. */
    const uint32_t edges = (uint32_t)w * (occ ? 9u : 1u);
    if (recomp_yield_hook && (edges >= RECOMP_YIELD_EVERY || g_backedges >= RECOMP_YIELD_EVERY - edges)) return 0;
    /* The loop pushes at most four slots and pops back to its top: no re-centring. */
    double *top = g_stp;
    if (top - g_stbuf < 6 || top >= g_stbuf + X87_WINDOW - 9) return 0;

    nm_row r = {
        .p = (const float *)GPTR(prev), .c = (const float *)GPTR(cur), .n = (const float *)GPTR(next),
        .out = (float *)GPTR(out), .w = w, .h = h, .y = y,
        .mu = (flags & 0x10000u) != 0, .mv = (flags & 0x20000u) != 0, .occ = occ,
        .inv = (flags & 0x80000u) != 0, .unorm = MEM32(codec + 8u) == 1,
        .thr = 1.4210854715202004e-14f, .three = 3.0f, .half = 0.5f,
    };
    memcpy(&r.amp, GPTR(bp + 0x1Cu), 4);
    if (!logged) {
        logged = 1;
        const uint32_t reader = MEM32(bp - 0x28u);
        void_log("[normalmap] native pixel loop: %dx%d, flags %08X, channel %u, amplitude %g, %s output; "
                 "writer %08X (format %u, diffusion %08X, pre %08X/%08X), reader %08X",
                 w, h, flags, MEM32(bp + 0x18u), (double)r.amp, r.unorm ? "unsigned" : "signed",
                 MEM32(codec), MEM32(codec + 4u), MEM32(codec + 0x5Cu), MEM32(codec + 0x54u), MEM32(codec + 0x10u),
                 void_vm_readable(reader, 4) ? MEM32(reader) : 0);
    }
    static uint64_t native_ns, texels;
    static unsigned images;
    const uint64_t started = void_monotonic_ns();
    nm_pixel_state s;
    float full_t6 = 0.0f, full_t7 = 0.0f;
    int any_full = 0;
    if (!r.mu && !r.mv && !r.occ && !r.inv && r.unorm) {
        any_full = nm_plain(&r, w - 1, &full_t6, &full_t7);
        nm_pixel(&r, w - 1, &s, 1);
        if (s.full) { any_full = 1; full_t6 = s.t6; full_t7 = s.t7; }
    } else {
        for (int x = 0; x < w; ++x) {
            if (x < w - 1) nm_pixel(&r, x, &s, 0);
            else nm_pixel(&r, x, &s, 1);
            if (s.full) { any_full = 1; full_t6 = s.t6; full_t7 = s.t7; }
        }
    }
    native_ns += void_monotonic_ns() - started;
    texels += (uint32_t)w;
    if (y == h - 1 && (++images <= 4 || !(images & 63)))
        void_log("[normalmap] %u images (last %dx%d, flags %08X), %llu texels native in %.1f ms", images, w, h, flags,
                 (unsigned long long)texels, native_ns / 1e6);
    const int x = w - 1;
    const float *p = r.p + x, *n = r.n + x;
    (void)p;

    /* The frame as the loop leaves it: its constants, then the last texel's
     * temporaries (the movsd copies of A and B, the cross product, the unit
     * normal, the occlusion differences). */
    MEM32(bp - 0xA8u) = 0xBF800000u; MEM32(bp - 0xA4u) = 0;
    MEM32(bp - 0xB4u) = 0;           MEM32(bp - 0xB0u) = 0xBF800000u;
    MEM32(bp - 0x90u) = flags & 0x100000u;
    MEM32(bp - 0x6Cu) = flags & 0x80000u;
    MEM32(bp - 4u) = s.mem4;
    MEM32(bp - 0x70u) = (uint32_t)(w - 1);
    nm_store(bp - 0xA0u, s.dxf);
    nm_store(bp - 0xACu, s.dyf);
    MEM32(bp - 0x7Cu) = 0xBF800000u; MEM32(bp - 0x78u) = 0; nm_store(bp - 0x74u, s.dxf);
    MEM32(bp - 0x88u) = 0; MEM32(bp - 0x84u) = 0xBF800000u; nm_store(bp - 0x80u, s.dyf);
    for (int i = 0; i < 3; ++i) {
        nm_store(bp - 0x9Cu + 4u * i, s.c[i]);
        nm_store(bp - 0x44u + 4u * i, s.v[i]);
    }
    if (occ)
        for (int i = 0; i < 8; ++i) nm_store(bp - 0xD4u + 4u * i, s.d[i]);
    /* The last normalize call's frame: two pointer arguments, the lifted
     * call's return address, the callee's saved ebp. */
    MEM32(sp - 4u) = bp - 0x44u;
    MEM32(sp - 8u) = bp - 0x44u;
    MEM32(sp - 12u) = RECOMP_RETADDR;
    MEM32(sp - 16u) = bp;
    /* x87 slots under the top, each written by a push (shadow 0). */
    top[-1] = s.t1; top[-2] = s.t2; top[-3] = s.t3;
    X87_SHADOW(top)[-1] = 0; X87_SHADOW(top)[-2] = 0; X87_SHADOW(top)[-3] = 0;
    if (occ) { top[-4] = s.t4; X87_SHADOW(top)[-4] = 0; }
    /* SSE registers of the last normalize; xmm6/xmm7 change only on its
     * rsqrt path, so they hold the last such texel's values. */
    for (int i = 0; i < 6; ++i) g_xmm[i].u64[0] = g_xmm[i].u64[1] = 0;
    g_xmm[0].f32[0] = s.sq;
    g_xmm[1].f32[0] = s.full ? s.f1 : s.s1;
    g_xmm[2].f32[0] = s.s2;
    g_xmm[3].f32[0] = s.full ? s.v[0] : s.x0;
    g_xmm[4].f32[0] = s.full ? s.v[1] : s.x1;
    g_xmm[5].f32[0] = s.full ? s.v[2] : s.x2;
    if (any_full) {
        g_xmm[6].f32[0] = full_t6;
        g_xmm[7] = g_xmm[6];
        g_xmm[7].f32[0] = full_t7;
    }
    /* Scratch registers, the flags the last normalize returned with, and the
     * loop's back edges. */
    g_eax = out + 16u * (uint32_t)x;
    g_ebx = (uint32_t)w;
    g_ecx = r.unorm ? out : nm_bits(s.v[2]);
    g_edx = occ ? 8u : next;
    g_esi = occ ? nm_bits(n[1]) : bp - 0x90u;
    g_edi = bp - 0x38u;
    if (s.full) { g_flag_k = FK_EFLAGS; g_flag_a = s.comi; }
    else { g_flag_k = FK_TEST; g_flag_a = 0; }
    g_flag_b = 0; g_flag_cf = 0;
    /* An urgent waiter (threads.c machine_poke) may have set the counter to
     * the threshold while the row ran: answer it now, as the lifted loop
     * would have at its next back edge. No guest thread can see this row's
     * buffers, so offering the machine after the row instead of within it
     * changes no result. */
    const uint32_t total = __atomic_load_n(&g_backedges, __ATOMIC_RELAXED) + edges;
    if (may_yield && total >= RECOMP_YIELD_EVERY && recomp_yield_hook) {
        g_backedges = 0;
        recomp_yield_hook();
    } else {
        g_backedges = total;
    }
    return 1;
}

/* debug.s2.normalmap: "off" keeps the lifted loop; "check" computes each row
 * natively, records the result, restores the state and lets the lifted loop
 * run, then compares everything the loop leaves at 0xA001E0F7 (the frame and
 * the callee frame below it, the output row, registers, both flag sets, the
 * x87 slots and their shadows, the SSE file, the back-edge count). */
typedef struct {
    uint32_t sp, bp, out, row, frame, frame_size;
    uint32_t regs[8], flags[4], backedges;
    double slots[4];
    int64_t shadows[4];
    xmm_t xmm[8];
    uint8_t *frame_bytes, *out_bytes;
} nm_snapshot;

int s2_normal_map_pending;
static nm_snapshot nm_before, nm_native;
static unsigned nm_checked_rows, nm_mismatched_rows, nm_yielded_rows;

static int nm_mode(void)
{
    static int mode = -1;   /* 0 off, 1 native, 2 check */
    if (mode < 0) {
        mode = 1;
#ifdef __ANDROID__
        char value[92] = "";
        __system_property_get("debug.s2.normalmap", value);
        if (!strcmp(value, "off") || !strcmp(value, "0")) mode = 0;
        else if (!strcmp(value, "check")) mode = 2;
#endif
        if (mode != 1) void_log("[normalmap] mode %s", mode ? "check" : "off");
    }
    return mode;
}

static int nm_capture(nm_snapshot *s, uint32_t sp, uint32_t bp, uint32_t out, uint32_t row, uint32_t kflag[4])
{
    s->sp = sp; s->bp = bp; s->out = out; s->row = row;
    s->frame = sp - 16u; s->frame_size = NM_FRAME + 16u + 0x20u;
    if (!s->frame_bytes) s->frame_bytes = (uint8_t *)malloc(NM_FRAME + 16u + 0x20u);
    s->out_bytes = (uint8_t *)realloc(s->out_bytes, row ? row : 1u);
    if (!s->frame_bytes || !s->out_bytes) return 0;
    memcpy(s->frame_bytes, GPTR(s->frame), s->frame_size);
    memcpy(s->out_bytes, GPTR(out), row);
    uint32_t regs[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    memcpy(s->regs, regs, sizeof regs);
    if (kflag) memcpy(s->flags, kflag, sizeof s->flags);
    s->backedges = g_backedges;
    for (int i = 0; i < 4; ++i) { s->slots[i] = g_stp[-1 - i]; s->shadows[i] = X87_SHADOW(g_stp)[-1 - i]; }
    memcpy(s->xmm, g_xmm, sizeof s->xmm);
    return 1;
}

static void nm_restore(const nm_snapshot *s)
{
    memcpy(GPTR(s->frame), s->frame_bytes, s->frame_size);
    memcpy(GPTR(s->out), s->out_bytes, s->row);
    g_eax = s->regs[0]; g_ecx = s->regs[1]; g_edx = s->regs[2]; g_ebx = s->regs[3];
    g_esp = s->regs[4]; g_ebp = s->regs[5]; g_esi = s->regs[6]; g_edi = s->regs[7];
    g_backedges = s->backedges;
    for (int i = 0; i < 4; ++i) { g_stp[-1 - i] = s->slots[i]; X87_SHADOW(g_stp)[-1 - i] = s->shadows[i]; }
    memcpy(g_xmm, s->xmm, sizeof s->xmm);
}

int s2_normal_map_row(void)
{
    int mode = nm_mode();
    if (mode == 0) return 0;
    if (mode == 1) return nm_run(1);
    const uint32_t sp = g_esp, bp = g_ebp;
    const int32_t w = (int32_t)MEM32(bp - 0x4Cu);
    const uint32_t out = MEM32(bp - 0x20u), row = w > 0 && w <= 32768 ? 16u * (uint32_t)w : 0;
    if (!row || sp != bp - NM_FRAME || !void_vm_writable(sp - 16u, NM_FRAME + 16u + 0x20u) || !void_vm_writable(out, row))
        return 0;
    uint32_t flags[4] = { g_flag_k, g_flag_a, g_flag_b, g_flag_cf };
    if (!nm_capture(&nm_before, sp, bp, out, row, flags)) return 0;
    if (!nm_run(0)) return 0;
    uint32_t after[4] = { g_flag_k, g_flag_a, g_flag_b, g_flag_cf };
    if (!nm_capture(&nm_native, sp, bp, out, row, after)) { nm_restore(&nm_before); return 0; }
    nm_restore(&nm_before);
    g_flag_k = flags[0]; g_flag_a = flags[1]; g_flag_b = flags[2]; g_flag_cf = flags[3];
    s2_normal_map_pending = 1;
    return 0;
}

/* At 0xA001E0F7 after a checked row: the lifted loop's state against the native one. */
void s2_normal_map_check(uint32_t flag_k, uint32_t flag_a, uint32_t flag_b, uint32_t cf)
{
    s2_normal_map_pending = 0;
    const nm_snapshot *n = &nm_native;
    char what[256] = "";
    size_t used = 0;
#define NM_DIFF(...) (used += (size_t)snprintf(what + used, used < sizeof what ? sizeof what - used : 0, __VA_ARGS__))
    if (g_esp != n->sp || g_ebp != n->bp) { NM_DIFF(" frame moved"); goto report; }
    for (uint32_t i = 0; i < n->frame_size; i += 4)
        if (memcmp(GPTR(n->frame + i), n->frame_bytes + i, 4)) {
            NM_DIFF(" [ebp%+d] %08X/%08X", (int)(n->frame + i - n->bp), MEM32(n->frame + i), *(uint32_t *)(n->frame_bytes + i));
            if (used > 160) break;
        }
    for (uint32_t i = 0; i < n->row; i += 4)
        if (memcmp(GPTR(n->out + i), n->out_bytes + i, 4)) {
            NM_DIFF(" out[%u].%u %08X/%08X", i / 16u, (i / 4u) & 3u, MEM32(n->out + i), *(uint32_t *)(n->out_bytes + i));
            break;
        }
    {
        uint32_t regs[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
        static const char *names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
        for (int i = 0; i < 8; ++i)
            if (regs[i] != n->regs[i]) NM_DIFF(" %s %08X/%08X", names[i], regs[i], n->regs[i]);
    }
    if (g_flag_k != n->flags[0] || g_flag_a != n->flags[1] || g_flag_b != n->flags[2] || g_flag_cf != n->flags[3])
        NM_DIFF(" gflags %u,%X,%X,%u/%u,%X,%X,%u", g_flag_k, g_flag_a, g_flag_b, g_flag_cf,
                n->flags[0], n->flags[1], n->flags[2], n->flags[3]);
    if (flag_k != FK_CMP || flag_a != n->regs[3] || flag_b != n->regs[3] || cf)
        NM_DIFF(" lflags %u,%X,%X,%u", flag_k, flag_a, flag_b, cf);
    /* A lifted loop that met an urgent poke (threads.c) yielded inside the
     * row: other guest threads then moved the back-edge count and used the
     * x87 window's free slots. Those rows compare everything else. */
    const int yielded = g_backedges != n->backedges;
    nm_yielded_rows += yielded;
    for (int i = 0; i < 4 && !yielded; ++i) {
        uint64_t a, b;
        memcpy(&a, &g_stp[-1 - i], 8); memcpy(&b, &n->slots[i], 8);
        if (a != b || X87_SHADOW(g_stp)[-1 - i] != n->shadows[i])
            NM_DIFF(" st-%d %016llX/%016llX", i + 1, (unsigned long long)a, (unsigned long long)b);
    }
    for (int i = 0; i < 8; ++i)
        if (memcmp(&g_xmm[i], &n->xmm[i], sizeof g_xmm[i]))
            NM_DIFF(" xmm%d %08X.%08X.%08X.%08X/%08X.%08X.%08X.%08X", i, g_xmm[i].u32[0], g_xmm[i].u32[1], g_xmm[i].u32[2],
                    g_xmm[i].u32[3], n->xmm[i].u32[0], n->xmm[i].u32[1], n->xmm[i].u32[2], n->xmm[i].u32[3]);
report:
#undef NM_DIFF
    ++nm_checked_rows;
    if (used) {
        if (++nm_mismatched_rows <= 20)
            void_log("[normalmap] check row %d of %d (w %d): native differs:%s", (int)MEM32(n->bp - 8u),
                     (int)MEM32(n->bp - 0x48u), (int)MEM32(n->bp - 0x4Cu), what);
    }
    if (nm_checked_rows <= 2 || !(nm_checked_rows & 1023))
        void_log("[normalmap] checked %u rows, %u differ (%u yielded inside the lifted loop)", nm_checked_rows,
                 nm_mismatched_rows, nm_yielded_rows);
}

/* The height row before the pixel loop (0xA001DD0E-0xA001DD5C): the row
 * below, just decoded to float4 at esi, goes through the channel selector
 * 0xA001D839 once per texel into the padded row at [ebp-0x18] (the last
 * texel wrapped into element 0, the first into element w+1). The game asks
 * for the luminance channel (16): ((b*0.0721 + g*0.7154) + r*0.2125) in x87
 * double, stored as float. Native for that channel: the same stores, the
 * loop's registers (eax the last texel, ecx 0 from the selector's
 * subtractions, edx past the row, ebx 0, esi past the source, edi the width),
 * the selector's returned flags (sub ecx, 8), its last two x87 slots, the
 * lifted call's return address under esp and the back edges. */
int s2_normal_map_heights_pending;
static uint32_t nm_heights_expected_regs[8], nm_heights_rows, nm_heights_bad;
static double nm_heights_expected_slots[2];
static uint32_t nm_heights_expected_edges;
static uint8_t *nm_heights_expected;
static uint32_t nm_heights_out, nm_heights_size;

static int nm_heights(int may_yield, uint8_t *into)
{
    const uint32_t bp = g_ebp, sp = g_esp;
    if (sp != bp - NM_FRAME || sp < 0x10000u || bp > 0xFFFF0000u || !void_vm_writable(sp - 4u, NM_FRAME + 4u + 0x20u)) return 0;
    if (MEM32(bp + 0x18u) != 16u) return 0;
    const int32_t w = (int32_t)MEM32(bp - 0x4Cu);
    const uint32_t row = g_esi, dst = MEM32(bp - 0x18u);
    if (w <= 0 || w > 32768 || row != MEM32(bp - 0x34u)) return 0;
    const uint32_t src_size = 16u * (uint32_t)w, dst_size = 4u * (uint32_t)w + 8u;
    if (!void_vm_readable(row, src_size) || !void_vm_writable(dst, dst_size) ||
        nm_overlap(row, src_size, dst, dst_size) || nm_overlap(dst, dst_size, sp - 4u, NM_FRAME + 4u + 0x20u) ||
        nm_overlap(row, src_size, sp - 4u, NM_FRAME + 4u + 0x20u)) return 0;
    const uint32_t edges = (uint32_t)w;
    if (recomp_yield_hook && (edges >= RECOMP_YIELD_EVERY || g_backedges >= RECOMP_YIELD_EVERY - edges)) return 0;
    double *top = g_stp;
    if (top - g_stbuf < 4 || top >= g_stbuf + X87_WINDOW - 9) return 0;
    float kb, kg, kr;
    memcpy(&kb, GPTR(0xA0E19BB4u), 4); memcpy(&kg, GPTR(0xA0E19BB0u), 4); memcpy(&kr, GPTR(0xA0E19BACu), 4);
    const float *s = (const float *)GPTR(row);
    float *d = (float *)(into ? into : (uint8_t *)GPTR(dst));
    const double b = kb, g = kg, r = kr;
    for (int32_t i = 0; i < w; ++i) {
        const float *t = s + 4 * i;
        double l = (double)t[2] * b;
        l = l + (double)t[1] * g;
        l = l + (double)t[0] * r;
        d[1 + i] = (float)l;
    }
    d[0] = d[w];
    d[w + 1] = d[1];
    const float *last = s + 4 * (w - 1);
    double l = (double)last[2] * b;
    l = l + (double)last[1] * g;
    const double rr = (double)last[0] * r;
    l = l + rr;
    if (into) {   /* check mode: the expected state, nothing changed */
        nm_heights_expected_slots[0] = l; nm_heights_expected_slots[1] = rr;
        uint32_t regs[8] = { row + 16u * (uint32_t)(w - 1), 0, dst + 4u + 4u * (uint32_t)w, 0, sp, bp, row + 16u * (uint32_t)w,
                             (uint32_t)w };
        memcpy(nm_heights_expected_regs, regs, sizeof regs);
        nm_heights_expected_edges = g_backedges + edges;
        return 1;
    }
    MEM32(sp - 4u) = RECOMP_RETADDR;
    top[-1] = l; top[-2] = rr;
    X87_SHADOW(top)[-1] = 0; X87_SHADOW(top)[-2] = 0;
    g_eax = row + 16u * (uint32_t)(w - 1);
    g_ecx = 0;
    g_edx = dst + 4u + 4u * (uint32_t)w;
    g_ebx = 0;
    g_esi = row + 16u * (uint32_t)w;
    g_edi = (uint32_t)w;
    g_flag_k = FK_CMP; g_flag_a = 8; g_flag_b = 8; g_flag_cf = 0;
    const uint32_t total = __atomic_load_n(&g_backedges, __ATOMIC_RELAXED) + edges;
    if (may_yield && total >= RECOMP_YIELD_EVERY && recomp_yield_hook) {
        g_backedges = 0;
        recomp_yield_hook();
    } else {
        g_backedges = total;
    }
    return 1;
}

int s2_normal_map_heights(void)
{
    const int mode = nm_mode();
    if (mode == 0) return 0;
    if (mode == 1) return nm_heights(1, NULL);
    const int32_t w = (int32_t)MEM32(g_ebp - 0x4Cu);
    if (w <= 0 || w > 32768) return 0;
    const uint32_t size = 4u * (uint32_t)w + 8u;
    uint8_t *buffer = (uint8_t *)realloc(nm_heights_expected, size);
    if (!buffer) return 0;
    nm_heights_expected = buffer;
    if (!nm_heights(0, buffer)) return 0;
    nm_heights_out = MEM32(g_ebp - 0x18u);
    nm_heights_size = size;
    s2_normal_map_heights_pending = 1;
    return 0;
}

/* At 0xA001DD5E after a checked height row: the lifted row against the native one. */
void s2_normal_map_heights_check(void)
{
    s2_normal_map_heights_pending = 0;
    uint32_t regs[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    int bad = memcmp(GPTR(nm_heights_out), nm_heights_expected, nm_heights_size) != 0 ||
              memcmp(regs, nm_heights_expected_regs, sizeof regs) != 0 ||
              g_flag_k != FK_CMP || g_flag_a != 8 || g_flag_b != 8 || g_flag_cf || MEM32(g_esp - 4u) != RECOMP_RETADDR;
    int yielded = g_backedges != nm_heights_expected_edges;
    uint64_t a0, b0, a1, b1;
    memcpy(&a0, &g_stp[-1], 8); memcpy(&b0, &nm_heights_expected_slots[0], 8);
    memcpy(&a1, &g_stp[-2], 8); memcpy(&b1, &nm_heights_expected_slots[1], 8);
    if (!yielded && (a0 != b0 || a1 != b1 || X87_SHADOW(g_stp)[-1] || X87_SHADOW(g_stp)[-2])) bad = 1;
    ++nm_heights_rows;
    if (bad && ++nm_heights_bad <= 10) {
        uint32_t i = 0;
        while (i < nm_heights_size && MEM8(nm_heights_out + i) == nm_heights_expected[i]) ++i;
        void_log("[normalmap] height check differs: byte %u of %u, eax %08X/%08X ecx %08X edx %08X/%08X esi %08X/%08X edi %08X, "
                 "flags %u,%X,%X,%u, st %016llX/%016llX", i, nm_heights_size, regs[0], nm_heights_expected_regs[0], regs[1],
                 regs[2], nm_heights_expected_regs[2], regs[6], nm_heights_expected_regs[6], regs[7], g_flag_k, g_flag_a, g_flag_b,
                 g_flag_cf, (unsigned long long)a0, (unsigned long long)b0);
    }
    if (nm_heights_rows <= 2 || !(nm_heights_rows & 1023))
        void_log("[normalmap] height rows checked %u, %u differ", nm_heights_rows, nm_heights_bad);
}
