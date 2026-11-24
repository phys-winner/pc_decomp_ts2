/* D3DX's BC1 (DXT1) colour-block encoder, natively.
 *
 * Opening the Families bin makes a DXT1 thumbnail of every family portrait
 * through D3DX: half of the main thread's time in its 11 s freeze was this
 * encoder, lifted (x87 on a memory-backed stack, every local in guest
 * memory). sub_A0084173 is DirectXTex's EncodeBC1: the colours (16 x float4)
 * in EAX; pBC, bColorKey and the dither flag on the stack; ret 0Ch. It uses
 * OptimizeRGB (sub_A0083BF3), Encode565 (sub_A00837C8) and Decode565
 * (sub_A00836C6).
 *
 * The result is the lifted function's, bit for bit (tools/check_bc1_native.py
 * runs both on the same blocks): the x87 model of recomp_types.h (the stack
 * holds doubles, a dword load widens, a dword store rounds to float, fistp
 * truncates: the encoder sets RC=chop), with every value kept where the
 * original keeps it (on the stack or stored to a dword) and the same order of
 * operations; the build has -ffp-contract=off. OptimizeRGB has no lower bound
 * on its step index: a point that projects more than 1.5 steps before the
 * first endpoint indexes below its step table, into words the original left
 * on its stack. Such a block is encoded by the lifted function, as is one
 * whose buffers overlap each other or the frame the original would use.
 *
 * The lifted body had dropped `fst st(1)` at 0xA00847A6 (lift32 lifts only
 * the memory forms; tools/sse_ext.py now handles it): its colour axis took
 * the red delta for the green one. This file follows the original. */
#include "host.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

/* The constants the original reads (.rdata), by their bits. */
#define F_HALF      0x3F000000u   /* 0xA0D9C694 */
#define F_ONE       0x3F800000u   /* 0xA0D9C698 */
#define F_ZERO      0x00000000u   /* 0xA0D9E82C */
#define F_MINUS_ONE 0xBF800000u   /* 0xA0DA03E8 */
#define F_EIGHTH    0x3E000000u   /* 0xA0DA74F4 */
#define F_16TH      0x3D800000u   /* 0xA0DA7698: 1/16 */
#define F_3_16      0x3E400000u   /* 0xA0DB1DB4 */
#define F_5_16      0x3EA00000u   /* 0xA0DB1DBC */
#define F_7_16      0x3EE00000u   /* 0xA0E2AE78 */
#define F_TWO_3     0x3F2AAAABu   /* 0xA0DB7854 */
#define F_ONE_3     0x3EAAAAABu   /* 0xA0DB8548 */
#define F_31        0x41F80000u   /* 0xA0DCC3C0 */
#define F_63        0x427C0000u   /* 0xA0E1DF70 */
#define F_INV31     0x3D042108u   /* 0xA0E1DF74 */
#define F_INV63     0x3C820821u   /* 0xA0E1DF6C */
#define F_FLT_MIN   0x00800000u   /* 0xA0E1D4C4 */
#define F_EPSILON   0x37800000u   /* 0xA0E2AE1C: (0.25/64)^2 */
#define F_4096TH    0x39800000u   /* 0xA0E2AE20 */
#define F_TWO_32    0x4F800000u   /* 0xA0E19214 */
#define LUM_VA      0xA10583C0u   /* g_Luminance r, g, b */
#define LUM_INV_VA  0xA10583D0u   /* g_LuminanceInv r, g, b */

static inline float fb(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline double db(uint32_t bits) { return (double)fb(bits); }
static inline float gf(uint32_t va) { float f; memcpy(&f, (const void *)ADDR(va), 4); return f; }

/* fistp dword with RC=chop, as the lifted fp_to_int: NaN or out of range
 * stores the integer indefinite. */
static inline int32_t chop_i32(double v)
{
    double t = trunc(v);
    return t >= -2147483648.0 && t <= 2147483647.0 ? (int32_t)t : INT32_MIN;
}

/* fild dword of an unsigned count: the original adds 2^32 to a negative one. */
static inline double count_f(uint32_t n)
{
    double v = (double)(int32_t)n;
    return (int32_t)n < 0 ? v + db(F_TWO_32) : v;
}

typedef struct { float r, g, b; } rgb_t;

/* Encode565 (sub_A00837C8). */
static uint32_t encode565(const rgb_t *c)
{
    float v[3] = { c->r, c->g, c->b };
    for (int k = 0; k < 3; ++k) {
        double x = v[k];
        if (x < db(F_ZERO)) v[k] = 0.0f;
        else if (x > db(F_ONE)) v[k] = 1.0f;
    }
    int32_t r = chop_i32((double)(float)((double)v[0] * db(F_31) + db(F_HALF)));
    int32_t g = chop_i32((double)(float)((double)v[1] * db(F_63) + db(F_HALF)));
    int32_t b = chop_i32((double)(float)((double)v[2] * db(F_31) + db(F_HALF)));
    return ((((uint32_t)r << 6) | (uint32_t)g) << 5) | (uint32_t)b;
}

/* Decode565 (sub_A00836C6). */
static void decode565(rgb_t *c, uint32_t w)
{
    c->r = (float)((double)(int32_t)((w & 0xFFFFu) >> 11) * db(F_INV31));
    c->g = (float)((double)(int32_t)((w >> 5) & 0x3Fu) * db(F_INV63));
    c->b = (float)((double)(int32_t)(w & 0x1Fu) * db(F_INV31));
}

/* OptimizeRGB (sub_A0083BF3). 0, or -1 when a step index falls below the
 * step table (the caller encodes the block with the lifted function). */
static int optimize_rgb(rgb_t *pX, rgb_t *pY, const float pts[16][4], uint32_t steps, const float lum[3])
{
    static const uint32_t c3[3] = { F_ONE, F_HALF, F_ZERO }, d3[3] = { F_ZERO, F_HALF, F_ONE };
    static const uint32_t c4[4] = { F_ONE, F_TWO_3, F_ONE_3, F_ZERO }, d4[4] = { F_ZERO, F_ONE_3, F_TWO_3, F_ONE };
    const uint32_t *pc = steps == 3 ? c3 : c4, *pd = steps == 3 ? d3 : d4;
    float xr = lum[0], xg = lum[1], xb = lum[2], yr = 0.0f, yg = 0.0f, yb = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float r = pts[i][0], g = pts[i][1], b = pts[i][2];
        if (r < xr) xr = r;
        if (g < xg) xg = g;
        if (b < xb) xb = b;
        if (r > yr) yr = r;
        if (yg < g) yg = g;                       /* kept on the stack, compared to the point */
        if (b > yb) yb = b;
    }
    float ab_r = (float)((double)yr - (double)xr);
    float ab_g = (float)((double)yg - (double)xg);
    double ab_b = (double)yb - (double)xb;        /* on the stack */
    double ab = ab_b * ab_b;
    ab = ab + (double)ab_g * (double)ab_g;
    ab = ab + (double)ab_r * (double)ab_r;
    float ab_f = (float)ab;
    if (ab < db(F_FLT_MIN)) goto done;            /* a single colour */
    double inv = 1.0 / ab;
    float dir_r = (float)((double)ab_r * inv), dir_g = (float)((double)ab_g * inv), dir_b = (float)(inv * ab_b);
    float mid_r = (float)(((double)yr + (double)xr) * db(F_HALF));
    float mid_g = (float)(((double)yg + (double)xg) * db(F_HALF));
    float mid_b = (float)(((double)yb + (double)xb) * db(F_HALF));
    double sum0 = 0.0;                            /* fDir[0] stays on the stack */
    float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 16; ++i) {
        float pr = (float)(((double)pts[i][0] - (double)mid_r) * (double)dir_r);
        float pg = (float)(((double)pts[i][1] - (double)mid_g) * (double)dir_g);
        double pb = ((double)pts[i][2] - (double)mid_b) * (double)dir_b;
        double f = ((double)pg + pb) + (double)pr;
        sum0 = sum0 + f * f;
        f = ((double)pg + (double)pr) - pb;
        sum[1] = (float)(f * f + (double)sum[1]);
        double d = (double)pr - (double)pg;
        f = d + pb;
        sum[2] = (float)(f * f + (double)sum[2]);
        f = d - pb;
        sum[3] = (float)(f * f + (double)sum[3]);
    }
    sum[0] = (float)sum0;
    double best = sum0;
    unsigned dir = 0;
    for (unsigned k = 1; k < 4; ++k)
        if (best < (double)sum[k]) { best = sum[k]; dir = k; }
    if (dir & 2) { float t = xg; xg = yg; yg = t; }
    if (dir & 1) { float t = xb; xb = yb; yb = t; }
    if ((double)ab_f < db(F_4096TH)) goto done;   /* two colours */

    float fsteps = (float)count_f(steps - 1);
    double eps = db(F_EPSILON);
    for (int iteration = 0; iteration < 8; ++iteration) {
        float st[4][3];
        for (uint32_t s = 0; s < steps; ++s) {
            double c = db(pc[s]), e = db(pd[s]);
            st[s][0] = (float)((double)xr * c + (double)yr * e);
            st[s][1] = (float)((double)xg * c + (double)yg * e);
            st[s][2] = (float)((double)xb * c + (double)yb * e);
        }
        double dr = (double)yr - (double)xr;
        float dg = (float)((double)yg - (double)xg);
        double dbl = (double)yb - (double)xb;
        double len = dbl * dbl;
        len = len + (double)dg * (double)dg;
        len = len + dr * dr;
        if (len < db(F_4096TH)) break;
        double scale = (double)fsteps / len;
        dir_r = (float)(scale * dr);
        dir_g = (float)((double)dg * scale);
        dir_b = (float)(scale * dbl);
        float d2x = 0, d2y = 0, dxr = 0, dxg = 0, dxb = 0, dyr = 0, dyg = 0, dyb = 0;
        for (int i = 0; i < 16; ++i) {
            double pr = pts[i][0], pg = pts[i][1], pb = pts[i][2];
            double dot = (pg - (double)xg) * (double)dir_g;
            dot = dot + (pr - (double)xr) * (double)dir_r;
            dot = dot + (pb - (double)xb) * (double)dir_b;
            int32_t step;
            if (!(dot >= (double)fsteps)) {
                float t = (float)((double)(float)dot + db(F_HALF));
                step = chop_i32((double)t);
                if (step < 0) return -1;
            } else {
                step = (int32_t)(steps - 1);
            }
            double diff_r = (double)st[step][0] - pr;
            float diff_g = (float)((double)st[step][1] - pg);
            double diff_b = (double)st[step][2] - pb;
            double c = db(pc[step]), e = db(pd[step]);
            double fc = c * db(F_EIGHTH), fd = e * db(F_EIGHTH);
            d2x = (float)(fc * c + (double)d2x);
            dxr = (float)(fc * diff_r + (double)dxr);
            dxg = (float)(fc * (double)diff_g + (double)dxg);
            dxb = (float)(fc * diff_b + (double)dxb);
            d2y = (float)(fd * e + (double)d2y);
            dyr = (float)(fd * diff_r + (double)dyr);
            dyg = (float)((double)diff_g * fd + (double)dyg);
            dyb = (float)(fd * diff_b + (double)dyb);
        }
        if ((double)d2x > db(F_ZERO)) {
            double f = db(F_MINUS_ONE) / (double)d2x;
            xr = (float)((double)dxr * f + (double)xr);
            xg = (float)((double)dxg * f + (double)xg);
            xb = (float)(f * (double)dxb + (double)xb);
        }
        if ((double)d2y > db(F_ZERO)) {
            double f = db(F_MINUS_ONE) / (double)d2y;
            yr = (float)((double)dyr * f + (double)yr);
            yg = (float)((double)dyg * f + (double)yg);
            yb = (float)(f * (double)dyb + (double)yb);
        }
        if ((double)dxr * dxr < eps && (double)dxg * dxg < eps && (double)dxb * dxb < eps &&
            (double)dyr * dyr < eps && (double)dyg * dyg < eps && (double)dyb * dyb < eps) break;
    }
done:
    pX->r = xr; pX->g = xg; pX->b = xb;
    pY->r = yr; pY->g = yg; pY->b = yb;
    return 0;
}

/* Error diffusion of one pixel's difference (Floyd-Steinberg on the 4x4
 * block), as the original writes it: r, g and b of the 1/16 share from
 * the given (stored) values, the other shares from the exact ones. */
typedef struct { double r, g, b; float g_stored, b_stored; } diff_t;

static void diffuse_first(float err[16][4], int i, const diff_t *d)
{
    int k = i & 3;
    if (k != 3) {
        double w = db(F_7_16);
        err[i + 1][0] = (float)(d->r * w + (double)err[i + 1][0]);
        err[i + 1][1] = (float)(d->g * w + (double)err[i + 1][1]);
        err[i + 1][2] = (float)(d->b * w + (double)err[i + 1][2]);
    }
    if (i >= 12) return;
    if (k != 0) {
        double w = db(F_3_16);
        err[i + 3][0] = (float)(d->r * w + (double)err[i + 3][0]);
        err[i + 3][1] = (float)(d->g * w + (double)err[i + 3][1]);
        err[i + 3][2] = (float)(d->b * w + (double)err[i + 3][2]);
    }
    double w = db(F_5_16);
    err[i + 4][0] = (float)(d->r * w + (double)err[i + 4][0]);
    err[i + 4][1] = (float)(d->g * w + (double)err[i + 4][1]);
    err[i + 4][2] = (float)(d->b * w + (double)err[i + 4][2]);
    if (k != 3) {
        w = db(F_16TH);
        err[i + 5][0] = (float)(d->r * w + (double)err[i + 5][0]);
        err[i + 5][1] = (float)((double)d->g_stored * w + (double)err[i + 5][1]);
        err[i + 5][2] = (float)((double)d->b_stored * w + (double)err[i + 5][2]);
    }
}

/* The encoding pass keeps r and g exact and b stored throughout. */
static void diffuse_final(float err[16][4], int i, const diff_t *d)
{
    int k = i & 3;
    double bs = d->b_stored;
    if (k != 3) {
        double w = db(F_7_16);
        err[i + 1][0] = (float)(d->r * w + (double)err[i + 1][0]);
        err[i + 1][1] = (float)(w * d->g + (double)err[i + 1][1]);
        err[i + 1][2] = (float)(bs * w + (double)err[i + 1][2]);
    }
    if (i >= 12) return;
    if (k != 0) {
        double w = db(F_3_16);
        err[i + 3][0] = (float)(d->r * w + (double)err[i + 3][0]);
        err[i + 3][1] = (float)(w * d->g + (double)err[i + 3][1]);
        err[i + 3][2] = (float)(bs * w + (double)err[i + 3][2]);
    }
    double w = db(F_5_16);
    err[i + 4][0] = (float)(d->r * w + (double)err[i + 4][0]);
    err[i + 4][1] = (float)(d->g * w + (double)err[i + 4][1]);
    err[i + 4][2] = (float)(bs * w + (double)err[i + 4][2]);
    if (k != 3) {
        w = db(F_16TH);
        err[i + 5][0] = (float)(d->r * w + (double)err[i + 5][0]);
        err[i + 5][1] = (float)((double)d->g_stored * w + (double)err[i + 5][1]);
        err[i + 5][2] = (float)(bs * w + (double)err[i + 5][2]);
    }
}

/* EncodeBC1 (sub_A0084173) into out[8]. 0, or -1: encode with the lifted body. */
static int encode_bc1(uint8_t out[8], const float px[16][4], int color_key, int dither)
{
    uint32_t steps = 4;
    if (color_key) {
        unsigned keyed = 0;
        for (int i = 0; i < 16; ++i)
            if ((double)px[i][3] < db(F_HALF)) ++keyed;
        if (keyed == 16) {
            memset(out, 0, 2);
            memset(out + 2, 0xFF, 6);
            return 0;
        }
        steps = keyed ? 3 : 4;
    }
    const float lum[3] = { gf(LUM_VA), gf(LUM_VA + 4), gf(LUM_VA + 8) };
    const float lum_inv[3] = { gf(LUM_INV_VA), gf(LUM_INV_VA + 4), gf(LUM_INV_VA + 8) };
    float err[16][4];
    memset(err, 0, sizeof err);
    float color[16][4];
    for (int i = 0; i < 16; ++i) {
        float r = px[i][0], g = px[i][1], b = px[i][2];
        if (dither) {
            r = (float)((double)r + (double)err[i][0]);
            g = (float)((double)g + (double)err[i][1]);
            b = (float)((double)b + (double)err[i][2]);
        }
        float qr = (float)((double)chop_i32((double)(float)((double)r * db(F_31) + db(F_HALF))) * db(F_INV31));
        float qg = (float)((double)chop_i32((double)(float)((double)g * db(F_63) + db(F_HALF))) * db(F_INV63));
        double qb = (double)chop_i32((double)(float)((double)b * db(F_31) + db(F_HALF))) * db(F_INV31);
        color[i][3] = 1.0f;
        if (dither) {
            diff_t d;
            d.r = (double)r - (double)qr;
            d.g = (double)g - (double)qg;
            d.g_stored = (float)d.g;
            d.b = (double)b - qb;
            d.b_stored = (float)d.b;
            diffuse_first(err, i, &d);
        }
        color[i][0] = (float)((double)qr * (double)lum[0]);
        color[i][1] = (float)((double)qg * (double)lum[1]);
        color[i][2] = (float)(qb * (double)lum[2]);
    }
    rgb_t a, b;
    if (optimize_rgb(&a, &b, (const float (*)[4])color, steps, lum)) return -1;
    rgb_t c = { (float)((double)a.r * (double)lum_inv[0]), (float)((double)a.g * (double)lum_inv[1]),
                (float)((double)a.b * (double)lum_inv[2]) };
    rgb_t d = { (float)((double)b.r * (double)lum_inv[0]), (float)((double)b.g * (double)lum_inv[1]),
                (float)((double)b.b * (double)lum_inv[2]) };
    uint32_t wa = encode565(&c), wb = encode565(&d);
    if (steps == 4 && (uint16_t)wa == (uint16_t)wb) {
        out[0] = (uint8_t)wa; out[1] = (uint8_t)(wa >> 8);
        out[2] = (uint8_t)wb; out[3] = (uint8_t)(wb >> 8);
        memset(out + 4, 0, 4);
        return 0;
    }
    decode565(&c, wa);
    decode565(&d, wb);
    a.r = (float)((double)c.r * (double)lum[0]);
    a.g = (float)((double)c.g * (double)lum[1]);
    a.b = (float)((double)c.b * (double)lum[2]);
    b.r = (float)((double)d.r * (double)lum[0]);
    b.g = (float)((double)d.g * (double)lum[1]);
    b.b = (float)((double)d.b * (double)lum[2]);
    rgb_t step[4];
    int in_order = (steps == 3) == ((uint16_t)wa <= (uint16_t)wb);
    uint32_t first = in_order ? wa : wb, second = in_order ? wb : wa;
    out[0] = (uint8_t)first; out[1] = (uint8_t)(first >> 8);
    out[2] = (uint8_t)second; out[3] = (uint8_t)(second >> 8);
    step[0] = in_order ? a : b;
    step[1] = in_order ? b : a;
    static const uint32_t order3[3] = { 0, 2, 1 }, order4[4] = { 0, 2, 3, 1 };
    const uint32_t *order = steps == 3 ? order3 : order4;
    double dr = (double)step[1].r - (double)step[0].r;
    double dg = (double)step[1].g - (double)step[0].g;
    float db_stored = (float)((double)step[1].b - (double)step[0].b);
    if (steps == 3) {
        double h = db(F_HALF);
        step[2].r = (float)(h * dr + (double)step[0].r);
        step[2].g = (float)(dg * h + (double)step[0].g);
        step[2].b = (float)(((double)step[1].b - (double)step[0].b) * h + (double)step[0].b);
    } else {
        double t = db(F_ONE_3), u = db(F_TWO_3);
        double dbl = (double)step[1].b - (double)step[0].b;
        step[2].r = (float)(t * dr + (double)step[0].r);
        step[2].g = (float)(t * dg + (double)step[0].g);
        step[2].b = (float)(t * dbl + (double)step[0].b);
        step[3].r = (float)(dr * u + (double)step[0].r);
        step[3].g = (float)(dg * u + (double)step[0].g);
        step[3].b = (float)(dbl * u + (double)step[0].b);
    }
    float dir_r = (float)dr;
    float fsteps = (float)count_f(steps - 1);
    double scale = 0.0;
    if ((uint16_t)wb != (uint16_t)wa) {
        double len = (double)db_stored * (double)db_stored;
        len = len + dg * dg;
        len = len + (double)dir_r * (double)dir_r;
        scale = (double)fsteps / len;
    }
    dir_r = (float)((double)dir_r * scale);
    float dir_g = (float)(scale * dg);
    float dir_b = (float)(scale * (double)db_stored);
    if (dither) memset(err, 0, sizeof err);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        if (steps == 3 && (double)px[i][3] < db(F_HALF)) {
            bits = (bits >> 2) | 0xC0000000u;
            continue;
        }
        float r = (float)((double)lum[0] * (double)px[i][0]);
        float g = (float)((double)lum[1] * (double)px[i][1]);
        float b = (float)((double)lum[2] * (double)px[i][2]);
        if (dither) {
            r = (float)((double)r + (double)err[i][0]);
            g = (float)((double)g + (double)err[i][1]);
            b = (float)((double)b + (double)err[i][2]);
        }
        double dot = ((double)b - (double)step[0].b) * (double)dir_b;
        dot = dot + ((double)g - (double)step[0].g) * (double)dir_g;
        dot = dot + ((double)r - (double)step[0].r) * (double)dir_r;
        uint32_t index;
        if (dot < db(F_ZERO) || dot == db(F_ZERO)) index = 0;
        else if (!(dot >= (double)fsteps)) {
            int32_t at = chop_i32((double)(float)(dot + db(F_HALF)));
            if (at < 0 || (uint32_t)at >= steps) return -1;
            index = order[at];
        } else index = 1;
        bits = (index << 30) | (bits >> 2);
        if (dither) {
            diff_t d;
            d.r = ((double)r - (double)step[index].r) * (double)color[i][3];
            d.g = ((double)g - (double)step[index].g) * (double)color[i][3];
            d.g_stored = (float)d.g;
            d.b = ((double)b - (double)step[index].b) * (double)color[i][3];
            d.b_stored = (float)d.b;
            diffuse_final(err, i, &d);
        }
    }
    memcpy(out + 4, &bits, 4);
    return 0;
}

static uint64_t bc1_native, bc1_lifted;
static struct {
    float pixels[16][4], luminance[6];
    uint8_t block[8];
    uint32_t color_key,dither;
    uint64_t fpcr;
    int valid;
} bc1_last;
static uint64_t bc1_reused;

static int spans_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

void sub_A0084173_lifted(void);
void sub_A0084173(void)
{
    uint32_t sp = g_esp, colors = g_eax;
    /* The original's frame: 0x2DC bytes of locals, three saved registers and
     * the calls below it, under its return address and three arguments. */
    if (sp < 0x400u || sp > UINT32_MAX - 16u || !void_vm_readable(sp, 16)) goto lifted;
    uint32_t out = MEM32(sp + 4), color_key = MEM32(sp + 8), dither = MEM32(sp + 12);
    if (colors > UINT32_MAX - 256u || out > UINT32_MAX - 8u || !void_vm_readable(colors, 256) ||
        !void_vm_writable(out, 8) || spans_overlap(out, 8, colors, 256) ||
        spans_overlap(colors, 256, sp - 0x400u, 0x410u) || spans_overlap(out, 8, sp - 0x400u, 0x410u)) goto lifted;
    float px[16][4];
    memcpy(px, (const void *)ADDR(colors), sizeof px);
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 4; ++k)
            if (!isfinite(px[i][k])) goto lifted;
    uint8_t block[8];
    float luminance[6];
    memcpy(luminance,GPTR(LUM_VA),12);memcpy(luminance+3,GPTR(LUM_INV_VA),12);
    uint64_t fpcr = void_host_fpcr();
    /* Adjacent portrait blocks repeat the same background; reuse only an exact input. */
    if(bc1_last.valid && bc1_last.color_key==color_key && bc1_last.dither==dither && bc1_last.fpcr==fpcr &&
        !memcmp(px,bc1_last.pixels,sizeof px) && !memcmp(luminance,bc1_last.luminance,sizeof luminance)) {
        memcpy(block,bc1_last.block,8);++bc1_reused;
    } else {
        if (encode_bc1(block, (const float (*)[4])px, color_key != 0, dither != 0)) goto lifted;
        memcpy(bc1_last.pixels,px,sizeof px);memcpy(bc1_last.luminance,luminance,sizeof luminance);
        memcpy(bc1_last.block,block,8);bc1_last.color_key=color_key;bc1_last.dither=dither;bc1_last.fpcr=fpcr;bc1_last.valid=1;
    }
    uint32_t _flag_k, _flag_a, _flag_b, _cf;
    RECOMP_ENTER(0xA0084173u);
    (void)_flag_k; (void)_flag_a; (void)_flag_b; (void)_cf;
    memcpy((void *)ADDR(out), block, 8);
    if (!bc1_native++) void_log("[bc1] D3DX BC1 blocks are encoded natively");
    g_eax = 0;                                    /* S_OK, after xor eax, eax */
    g_ecx = out;
    g_flag_k = FK_TEST; g_flag_a = 0; g_flag_b = 0; g_flag_cf = 0;
    g_esp = sp + 16;                              /* ret 0Ch */
    return;
lifted:
    ++bc1_lifted;
    if (!(bc1_lifted & (bc1_lifted - 1)))
        void_log("[bc1] %llu blocks through the lifted encoder, %llu native", (unsigned long long)bc1_lifted,
                 (unsigned long long)bc1_native);
    sub_A0084173_lifted();
}

void s2_bc1_stats(uint64_t *native, uint64_t *lifted)
{
    *native = bc1_native;
    *lifted = bc1_lifted;
}
