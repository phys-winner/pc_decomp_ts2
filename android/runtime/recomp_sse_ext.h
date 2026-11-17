/*
 * Packed-integer SSE2 (and the few SSE3/SSSE3/SSE4.1 forms the game's
 * CPU-independent code uses) on the xmm_t register model, plus the SSE1
 * extensions of the MMX file (pshufw, pavg, pminub, ...). The lifter side is
 * tools/sse_ext.py; lift32_sse.py covers the float forms and the integer
 * forms it already had. Every helper takes the destination's old value and
 * the source operand and returns the new destination, so the generated code
 * stays `_xmm[d] = xmm_op(_xmm[d], src);` and clang vectorises the lane loops
 * into NEON.
 */
#ifndef RECOMP_SSE_EXT_H
#define RECOMP_SSE_EXT_H

static inline int32_t sx_sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
static inline int32_t sx_satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : v; }
static inline int32_t sx_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int32_t sx_satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : v; }

#define SX_LANES(name, n, field, type, expr)                                  \
    static inline xmm_t name(xmm_t a, xmm_t b) {                              \
        xmm_t r;                                                              \
        for (int i = 0; i < (n); i++) r.field[i] = (type)(expr);              \
        return r;                                                             \
    }

SX_LANES(xmm_paddb, 16, u8, uint8_t, a.u8[i] + b.u8[i])
SX_LANES(xmm_paddw, 8, u16, uint16_t, a.u16[i] + b.u16[i])
SX_LANES(xmm_psubb, 16, u8, uint8_t, a.u8[i] - b.u8[i])
SX_LANES(xmm_psubw, 8, u16, uint16_t, a.u16[i] - b.u16[i])
SX_LANES(xmm_paddsb, 16, u8, uint8_t, sx_sat8((int8_t)a.u8[i] + (int8_t)b.u8[i]))
SX_LANES(xmm_paddsw, 8, i16, int16_t, sx_sat16(a.i16[i] + b.i16[i]))
SX_LANES(xmm_psubsb, 16, u8, uint8_t, sx_sat8((int8_t)a.u8[i] - (int8_t)b.u8[i]))
SX_LANES(xmm_psubsw, 8, i16, int16_t, sx_sat16(a.i16[i] - b.i16[i]))
SX_LANES(xmm_paddusb, 16, u8, uint8_t, sx_satu8(a.u8[i] + b.u8[i]))
SX_LANES(xmm_paddusw, 8, u16, uint16_t, sx_satu16(a.u16[i] + b.u16[i]))
SX_LANES(xmm_psubusb, 16, u8, uint8_t, sx_satu8(a.u8[i] - b.u8[i]))
SX_LANES(xmm_psubusw, 8, u16, uint16_t, sx_satu16(a.u16[i] - b.u16[i]))
SX_LANES(xmm_pmullw, 8, u16, uint16_t, (int32_t)a.i16[i] * b.i16[i])
SX_LANES(xmm_pmulhw, 8, u16, uint16_t, ((int32_t)a.i16[i] * b.i16[i]) >> 16)
SX_LANES(xmm_pmulhuw, 8, u16, uint16_t, ((uint32_t)a.u16[i] * b.u16[i]) >> 16)
SX_LANES(xmm_pmulld, 4, u32, uint32_t, (uint32_t)a.u32[i] * b.u32[i])
SX_LANES(xmm_pavgb, 16, u8, uint8_t, (a.u8[i] + b.u8[i] + 1) >> 1)
SX_LANES(xmm_pavgw, 8, u16, uint16_t, ((uint32_t)a.u16[i] + b.u16[i] + 1) >> 1)
SX_LANES(xmm_pminub, 16, u8, uint8_t, a.u8[i] < b.u8[i] ? a.u8[i] : b.u8[i])
SX_LANES(xmm_pmaxub, 16, u8, uint8_t, a.u8[i] > b.u8[i] ? a.u8[i] : b.u8[i])
SX_LANES(xmm_pminuw, 8, u16, uint16_t, a.u16[i] < b.u16[i] ? a.u16[i] : b.u16[i])
SX_LANES(xmm_pmaxuw, 8, u16, uint16_t, a.u16[i] > b.u16[i] ? a.u16[i] : b.u16[i])
SX_LANES(xmm_pminsb, 16, u8, uint8_t, (int8_t)a.u8[i] < (int8_t)b.u8[i] ? a.u8[i] : b.u8[i])
SX_LANES(xmm_pmaxsb, 16, u8, uint8_t, (int8_t)a.u8[i] > (int8_t)b.u8[i] ? a.u8[i] : b.u8[i])
SX_LANES(xmm_pminsd, 4, i32, int32_t, a.i32[i] < b.i32[i] ? a.i32[i] : b.i32[i])
SX_LANES(xmm_pmaxsd, 4, i32, int32_t, a.i32[i] > b.i32[i] ? a.i32[i] : b.i32[i])
SX_LANES(xmm_pminud, 4, u32, uint32_t, a.u32[i] < b.u32[i] ? a.u32[i] : b.u32[i])
SX_LANES(xmm_pmaxud, 4, u32, uint32_t, a.u32[i] > b.u32[i] ? a.u32[i] : b.u32[i])
SX_LANES(xmm_pcmpeqq, 2, u64, uint64_t, a.u64[i] == b.u64[i] ? ~0ull : 0)
SX_LANES(xmm_pabsb_, 16, u8, uint8_t, (int8_t)b.u8[i] < 0 ? -(int8_t)b.u8[i] : b.u8[i])
SX_LANES(xmm_pabsw_, 8, u16, uint16_t, b.i16[i] < 0 ? -b.i16[i] : b.i16[i])
SX_LANES(xmm_pabsd_, 4, u32, uint32_t, b.i32[i] < 0 ? -(int64_t)b.i32[i] : b.i32[i])

static inline xmm_t xmm_pmaddwd(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 4; i++)
        r.u32[i] = (uint32_t)((int32_t)a.i16[2 * i] * b.i16[2 * i] + (int32_t)a.i16[2 * i + 1] * b.i16[2 * i + 1]);
    return r;
}
static inline xmm_t xmm_psadbw(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int h = 0; h < 2; h++) {
        uint32_t s = 0;
        for (int i = 0; i < 8; i++) {
            int d = (int)a.u8[8 * h + i] - (int)b.u8[8 * h + i];
            s += (uint32_t)(d < 0 ? -d : d);
        }
        r.u64[h] = s;
    }
    return r;
}
static inline xmm_t xmm_packsswb(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 8; i++) { r.u8[i] = (uint8_t)sx_sat8(a.i16[i]); r.u8[8 + i] = (uint8_t)sx_sat8(b.i16[i]); }
    return r;
}
static inline xmm_t xmm_packuswb(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 8; i++) { r.u8[i] = (uint8_t)sx_satu8(a.i16[i]); r.u8[8 + i] = (uint8_t)sx_satu8(b.i16[i]); }
    return r;
}
static inline xmm_t xmm_packssdw(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 4; i++) { r.i16[i] = (int16_t)sx_sat16(a.i32[i]); r.i16[4 + i] = (int16_t)sx_sat16(b.i32[i]); }
    return r;
}
static inline int32_t sx_satu16_64(int64_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (int32_t)v; }
static inline xmm_t xmm_packusdw(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 4; i++) { r.u16[i] = (uint16_t)sx_satu16_64(a.i32[i]); r.u16[4 + i] = (uint16_t)sx_satu16_64(b.i32[i]); }
    return r;
}
/* Interleaves: the low (h = 0) or high (h = 1) halves, a's element first. */
static inline xmm_t xmm_punpckbw(xmm_t a, xmm_t b, int h) {
    xmm_t r;
    for (int i = 0; i < 8; i++) { r.u8[2 * i] = a.u8[8 * h + i]; r.u8[2 * i + 1] = b.u8[8 * h + i]; }
    return r;
}
static inline xmm_t xmm_punpckwd(xmm_t a, xmm_t b, int h) {
    xmm_t r;
    for (int i = 0; i < 4; i++) { r.u16[2 * i] = a.u16[4 * h + i]; r.u16[2 * i + 1] = b.u16[4 * h + i]; }
    return r;
}
static inline xmm_t xmm_punpckdq(xmm_t a, xmm_t b, int h) {
    xmm_t r;
    for (int i = 0; i < 2; i++) { r.u32[2 * i] = a.u32[2 * h + i]; r.u32[2 * i + 1] = b.u32[2 * h + i]; }
    return r;
}
static inline xmm_t xmm_pshufhw(xmm_t s, int imm) {
    xmm_t r = s;
    for (int i = 0; i < 4; i++) r.u16[4 + i] = s.u16[4 + ((imm >> (2 * i)) & 3)];
    return r;
}
static inline xmm_t xmm_pshuflw(xmm_t s, int imm) {
    xmm_t r = s;
    for (int i = 0; i < 4; i++) r.u16[i] = s.u16[(imm >> (2 * i)) & 3];
    return r;
}
static inline xmm_t xmm_pshufb(xmm_t a, xmm_t b) {
    xmm_t r;
    for (int i = 0; i < 16; i++) r.u8[i] = (b.u8[i] & 0x80) ? 0 : a.u8[b.u8[i] & 15];
    return r;
}
/* palignr: bytes imm.. of the 32-byte concatenation b:a (b low). */
static inline xmm_t xmm_palignr(xmm_t a, xmm_t b, int imm) {
    uint8_t cat[32];
    memcpy(cat, &b, 16);
    memcpy(cat + 16, &a, 16);
    xmm_t r;
    for (int i = 0; i < 16; i++) r.u8[i] = imm + i < 32 ? cat[imm + i] : 0;
    return r;
}
static inline xmm_t xmm_movddup(xmm_t s) { s.u64[1] = s.u64[0]; return s; }
static inline xmm_t xmm_movshdup(xmm_t s) { s.u32[0] = s.u32[1]; s.u32[2] = s.u32[3]; return s; }
static inline xmm_t xmm_movsldup(xmm_t s) { s.u32[1] = s.u32[0]; s.u32[3] = s.u32[2]; return s; }
static inline xmm_t xmm_haddps(xmm_t a, xmm_t b) {
    xmm_t r;
    r.f32[0] = a.f32[0] + a.f32[1]; r.f32[1] = a.f32[2] + a.f32[3];
    r.f32[2] = b.f32[0] + b.f32[1]; r.f32[3] = b.f32[2] + b.f32[3];
    return r;
}
static inline xmm_t xmm_hsubps(xmm_t a, xmm_t b) {
    xmm_t r;
    r.f32[0] = a.f32[0] - a.f32[1]; r.f32[1] = a.f32[2] - a.f32[3];
    r.f32[2] = b.f32[0] - b.f32[1]; r.f32[3] = b.f32[2] - b.f32[3];
    return r;
}
static inline xmm_t xmm_haddpd(xmm_t a, xmm_t b) {
    xmm_t r;
    r.f64[0] = a.f64[0] + a.f64[1]; r.f64[1] = b.f64[0] + b.f64[1];
    return r;
}
static inline xmm_t xmm_addsubps(xmm_t a, xmm_t b) {
    a.f32[0] -= b.f32[0]; a.f32[1] += b.f32[1]; a.f32[2] -= b.f32[2]; a.f32[3] += b.f32[3];
    return a;
}
static inline xmm_t xmm_addsubpd(xmm_t a, xmm_t b) { a.f64[0] -= b.f64[0]; a.f64[1] += b.f64[1]; return a; }
static inline xmm_t xmm_cvtpd2dq(xmm_t s, int chop) {
    xmm_t r;
    r.i32[0] = sse_cvt_i32(s.f64[0], chop);
    r.i32[1] = sse_cvt_i32(s.f64[1], chop);
    r.u64[1] = 0;
    return r;
}
/* SSE4.1 round{ss,sd,ps,pd}: imm bits 0-1 mode unless bit 2 (use MXCSR). */
static inline double sx_round(double v, int imm) {
    int mode = (imm & 4) ? (int)((g_mxcsr >> 13) & 3) : (imm & 3);
    switch (mode) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline xmm_t xmm_pmovzx(const uint8_t *src, int from, int to) {
    xmm_t r;
    int n = 16 / to;
    for (int i = 0; i < n; i++) {
        uint64_t v = 0;
        memcpy(&v, src + i * from, (size_t)from);
        if (to == 2) r.u16[i] = (uint16_t)v; else if (to == 4) r.u32[i] = (uint32_t)v; else r.u64[i] = v;
    }
    return r;
}
static inline xmm_t xmm_pmovsx(const uint8_t *src, int from, int to) {
    xmm_t r;
    int n = 16 / to;
    for (int i = 0; i < n; i++) {
        int64_t v;
        if (from == 1) v = (int8_t)src[i];
        else if (from == 2) { int16_t t; memcpy(&t, src + 2 * i, 2); v = t; }
        else { int32_t t; memcpy(&t, src + 4 * i, 4); v = t; }
        if (to == 2) r.u16[i] = (uint16_t)v; else if (to == 4) r.u32[i] = (uint32_t)v; else r.u64[i] = (uint64_t)v;
    }
    return r;
}

/* SSE1 additions to the MMX file. */
static inline uint64_t mmx_pshufw(uint64_t s, int imm) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= (uint64_t)(uint16_t)(s >> (16 * ((imm >> (2 * i)) & 3))) << (16 * i);
    return r;
}
#define SX_MM_LANES(name, n, bits, expr)                                      \
    static inline uint64_t name(uint64_t a, uint64_t b) {                     \
        uint64_t r = 0;                                                       \
        for (int i = 0; i < (n); i++) {                                       \
            uint64_t x = (a >> ((bits) * i)) & ((1ull << (bits)) - 1);        \
            uint64_t y = (b >> ((bits) * i)) & ((1ull << (bits)) - 1);        \
            (void)x; (void)y;                                                 \
            r |= ((uint64_t)(expr) & ((1ull << (bits)) - 1)) << ((bits) * i); \
        }                                                                     \
        return r;                                                             \
    }
SX_MM_LANES(mmx_pmulhuw, 4, 16, (x * y) >> 16)
SX_MM_LANES(mmx_pavgb, 8, 8, (x + y + 1) >> 1)
SX_MM_LANES(mmx_pavgw, 4, 16, (x + y + 1) >> 1)
SX_MM_LANES(mmx_pminub, 8, 8, x < y ? x : y)
SX_MM_LANES(mmx_pmaxub, 8, 8, x > y ? x : y)
SX_MM_LANES(mmx_pminsw, 4, 16, (int16_t)x < (int16_t)y ? x : y)
SX_MM_LANES(mmx_pmaxsw, 4, 16, (int16_t)x > (int16_t)y ? x : y)
static inline uint64_t mmx_psadbw(uint64_t a, uint64_t b) {
    uint32_t s = 0;
    for (int i = 0; i < 8; i++) {
        int d = (int)(uint8_t)(a >> (8 * i)) - (int)(uint8_t)(b >> (8 * i));
        s += (uint32_t)(d < 0 ? -d : d);
    }
    return s;
}
static inline uint64_t mmx_pmuludq(uint64_t a, uint64_t b) { return (uint64_t)(uint32_t)a * (uint32_t)b; }
static inline uint32_t mmx_pmovmskb(uint64_t s) {
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) m |= (uint32_t)((s >> (8 * i + 7)) & 1) << i;
    return m;
}

/* fisttp (SSE3): store st0 truncated toward zero, x86's integer indefinite
 * when out of range, then pop. */
static inline int64_t sx_fisttp(double v, int bits) {
    double t = trunc(v);
    if (bits == 16) return (t >= -32768.0 && t <= 32767.0) ? (int64_t)t : (int64_t)(int16_t)0x8000;
    if (bits == 32) return (t >= -2147483648.0 && t <= 2147483647.0) ? (int64_t)t : (int64_t)(int32_t)0x80000000u;
    return (t >= -9223372036854775808.0 && t < 9223372036854775808.0) ? (int64_t)t : INT64_MIN;
}

#endif
