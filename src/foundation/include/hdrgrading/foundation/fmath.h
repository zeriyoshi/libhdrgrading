/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_FMATH_H
#define HDRG_FOUNDATION_FMATH_H

#include <stdint.h>
#include <string.h>

/*
 * Float polynomial versions of the transcendental functions of the per-pixel
 * passes. Only IEEE +, -, *, / and bit operations,
 * fixed hex coefficients and Horner order, so the scalar reference is the
 * same on every compiler (-ffp-contract=off) and a SIMD kernel can run the
 * same sequence per lane. Accuracy against the double reference of
 * hdrgrading/foundation/math.h is checked by test_common_fmath: about 2 ulp, far below the
 * 0.01 LSB of an 8-bit output code.
 *
 * Domains: log2 takes positive normal floats; exp2 clamps to [-126, 127];
 * the OETFs clamp to their input range. The SIMD kernels (src/kernels/)
 * run the same sequence with these constants lane by lane.
 */
#define HDRG_FMATH_SQRT2       0x1.6a09e6p+0f
#define HDRG_FMATH_ROUND       0x1.8p+23f /* adding and subtracting it rounds to an integer */
#define HDRG_FMATH_LOG2E       0x1.715476p+0f
#define HDRG_FMATH_SRGB_KNEE   0x1.9a5c38p-9f /* 0.0031308 */
#define HDRG_FMATH_SRGB_SLOPE  0x1.9d70a4p+3f /* 12.92 */
#define HDRG_FMATH_SRGB_GAMMA  0x1.aaaaaap-2f /* 1 / 2.4 */
#define HDRG_FMATH_SRGB_SCALE  0x1.0e147ap+0f /* 1.055 */
#define HDRG_FMATH_SRGB_OFFSET 0x1.c28f5cp-5f /* 0.055 */
#define HDRG_FMATH_PQ_M1       0x1.464p-3f    /* 2610 / 16384 */
#define HDRG_FMATH_PQ_M2       0x1.3b6p+6f    /* 2523 / 4096 * 128 */
#define HDRG_FMATH_PQ_C1       0x1.acp-1f     /* 3424 / 4096 */
#define HDRG_FMATH_PQ_C2       0x1.2dap+4f    /* 2413 / 4096 * 32 */
#define HDRG_FMATH_PQ_C3       0x1.2bp+4f     /* 2392 / 4096 * 32 */
#define HDRG_FMATH_PQ_PEAK     10000.0f
#define HDRG_FMATH_PQ_MIN      0x1p-126f /* below: the PQ input is 0 */
#define HDRG_FMATH_EXP2_MIN    -126.0f
#define HDRG_FMATH_EXP2_MAX    127.0f
/* log2: s * (c1 + c3 s^2 + c5 s^4 + c7 s^6 + c9 s^8) */
#define HDRG_FMATH_LOG2_C9 0x1.484b14p-2f
#define HDRG_FMATH_LOG2_C7 0x1.a61762p-2f
#define HDRG_FMATH_LOG2_C5 0x1.2776c6p-1f
#define HDRG_FMATH_LOG2_C3 0x1.ec709ep-1f
#define HDRG_FMATH_LOG2_C1 0x1.715476p+1f
/* exp2: 1 + c1 f + ... + c7 f^7 */
#define HDRG_FMATH_EXP2_C7 0x1.ffcbfcp-17f
#define HDRG_FMATH_EXP2_C6 0x1.430912p-13f
#define HDRG_FMATH_EXP2_C5 0x1.5d87fep-10f
#define HDRG_FMATH_EXP2_C4 0x1.3b2ab6p-7f
#define HDRG_FMATH_EXP2_C3 0x1.c6b08ep-5f
#define HDRG_FMATH_EXP2_C2 0x1.ebfbe0p-3f
#define HDRG_FMATH_EXP2_C1 0x1.62e430p-1f

/*
 * log2(x) = e + log2(m), m in [sqrt(1/2), sqrt(2)): with s = (m - 1) / (m + 1),
 * log2(m) = 2 / ln 2 * atanh(s) = s * (c1 + c3 s^2 + c5 s^4 + c7 s^6 + c9 s^8)
 * (|s| < 0.172: the next term is below 4e-10)
 */
static inline float hdrg_fmath_log2(float x)
{
    uint32_t bits;
    int32_t e;
    float m, s, s2, p;

    memcpy(&bits, &x, sizeof(bits));

    e = (int32_t)((bits >> 23) & 0xFFu) - 127;
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    memcpy(&m, &bits, sizeof(m));

    if (m > HDRG_FMATH_SQRT2) {
        m *= 0.5f;
        e++;
    }

    s = (m - 1.0f) / (m + 1.0f);
    s2 = s * s;
    p = HDRG_FMATH_LOG2_C9;
    p = p * s2 + HDRG_FMATH_LOG2_C7;
    p = p * s2 + HDRG_FMATH_LOG2_C5;
    p = p * s2 + HDRG_FMATH_LOG2_C3;
    p = p * s2 + HDRG_FMATH_LOG2_C1;

    return (float)e + s * p;
}

/*
 * 2^x = 2^n * 2^f, n = round(x), |f| <= 1/2: 2^f by its Taylor series in f
 * to degree 7 (remainder below 6e-9)
 */
static inline float hdrg_fmath_exp2(float x)
{
    uint32_t bits;
    float n, f, p, scale;

    x = (x < HDRG_FMATH_EXP2_MIN) ? HDRG_FMATH_EXP2_MIN
                                  : ((x > HDRG_FMATH_EXP2_MAX) ? HDRG_FMATH_EXP2_MAX : x);
    n = (x + HDRG_FMATH_ROUND) - HDRG_FMATH_ROUND;
    f = x - n;

    p = HDRG_FMATH_EXP2_C7;
    p = p * f + HDRG_FMATH_EXP2_C6;
    p = p * f + HDRG_FMATH_EXP2_C5;
    p = p * f + HDRG_FMATH_EXP2_C4;
    p = p * f + HDRG_FMATH_EXP2_C3;
    p = p * f + HDRG_FMATH_EXP2_C2;
    p = p * f + HDRG_FMATH_EXP2_C1;
    p = p * f + 1.0f;

    bits = (uint32_t)((int32_t)n + 127) << 23;
    memcpy(&scale, &bits, sizeof(scale));

    return p * scale;
}

static inline float hdrg_fmath_exp(float x)
{
    return hdrg_fmath_exp2(x * HDRG_FMATH_LOG2E);
}

/* sRGB encoding of linear light in [0, 1] */
static inline float hdrg_fmath_srgb_oetf(float linear)
{
    linear = (linear < 0.0f) ? 0.0f : ((linear > 1.0f) ? 1.0f : linear);

    if (linear <= HDRG_FMATH_SRGB_KNEE) {
        return linear * HDRG_FMATH_SRGB_SLOPE;
    }

    return HDRG_FMATH_SRGB_SCALE *
               hdrg_fmath_exp2(hdrg_fmath_log2(linear) * HDRG_FMATH_SRGB_GAMMA) -
           HDRG_FMATH_SRGB_OFFSET;
}

/* SMPTE ST 2084 inverse EOTF: absolute nits (clamped to [0, 10000]) -> signal */
static inline float hdrg_fmath_pq_oetf(float nits)
{
    float y, ym, v;

    y = nits / HDRG_FMATH_PQ_PEAK;
    y = (y > 1.0f) ? 1.0f : y;
    ym = (y >= HDRG_FMATH_PQ_MIN) ? hdrg_fmath_exp2(HDRG_FMATH_PQ_M1 * hdrg_fmath_log2(y)) : 0.0f;
    v = (HDRG_FMATH_PQ_C1 + HDRG_FMATH_PQ_C2 * ym) / (1.0f + HDRG_FMATH_PQ_C3 * ym);

    return hdrg_fmath_exp2(HDRG_FMATH_PQ_M2 * hdrg_fmath_log2(v));
}

#endif /* HDRG_FOUNDATION_FMATH_H */
