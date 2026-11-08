/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/foundation/math.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HDRG_MATH_NAN_BITS     UINT32_C(0x7fc00000)
#define HDRG_MATH_INF_BITS     UINT32_C(0x7f800000)
#define HDRG_MATH_NEG_INF_BITS UINT32_C(0xff800000)
#define HDRG_MATH_LN2          0x1.62e42fefa39efp-1
#define HDRG_MATH_LOG2E        0x1.71547652b82fep+0
#define HDRG_MATH_CBRT2        0x1.428a2f98d728bp+0
#define HDRG_MATH_CBRT4        0x1.965fea53d6e3dp+0
#define HDRG_MATH_SQRT2_BITS   UINT64_C(0x3ff6a09e667f3bcd)
#define HDRG_MATH_EXP_MASK     UINT64_C(0x7ff0000000000000)
#define HDRG_MATH_MANT_MASK    UINT64_C(0x000fffffffffffff)
#define HDRG_MATH_ONE_BITS     UINT64_C(0x3ff0000000000000)

/* 2^t saturates here: far outside float, well inside double */
#define HDRG_MATH_EXP2_LIMIT 200u

#define HDRG_MATH_SRGB_EOTF_KNEE 0.04045
#define HDRG_MATH_SRGB_OETF_KNEE 0.0031308

/* SMPTE ST 2084; every constant is an exact binary fraction */
#define HDRG_MATH_PQ_M1   (2610.0 / 16384.0)
#define HDRG_MATH_PQ_M2   (2523.0 / 4096.0 * 128.0)
#define HDRG_MATH_PQ_C1   (3424.0 / 4096.0)
#define HDRG_MATH_PQ_C2   (2413.0 / 4096.0 * 32.0)
#define HDRG_MATH_PQ_C3   (2392.0 / 4096.0 * 32.0)
#define HDRG_MATH_PQ_PEAK 10000.0

/* 1/n!, n = 0..11: e^r on |r| <= ln2/2, truncation below 1e-14 */
static const double hdrg_math_exp_poly[12] = {
    0x1.0000000000000p+0,  0x1.0000000000000p+0,  0x1.0000000000000p-1,  0x1.5555555555555p-3,
    0x1.5555555555555p-5,  0x1.1111111111111p-7,  0x1.6c16c16c16c17p-10, 0x1.a01a01a01a01ap-13,
    0x1.a01a01a01a01ap-16, 0x1.71de3a556c734p-19, 0x1.27e4fb7789f5cp-22, 0x1.ae64567f544e4p-26,
};

/* 1/(2k+1), k = 0..8: atanh(s)/s on s^2 <= (3 - 2 sqrt2)^2, truncation below 1e-15 */
static const double hdrg_math_atanh_poly[9] = {
    0x1.0000000000000p+0, 0x1.5555555555555p-2, 0x1.999999999999ap-3,
    0x1.2492492492492p-3, 0x1.c71c71c71c71cp-4, 0x1.745d1745d1746p-4,
    0x1.3b13b13b13b14p-4, 0x1.1111111111111p-4, 0x1.e1e1e1e1e1e1ep-5,
};

/* (-1)^k pi^(2k+1) / (2k+1)!, k = 0..10: sin(pi r) / r on r^2 <= 1/4, truncation below 2e-18 */
static const double hdrg_math_sinpi_poly[11] = {
    0x1.921fb54442d18p+1,  -0x1.4abbce625be53p+2,  0x1.466bc6775aae2p+1,  -0x1.32d2cce62bd86p-1,
    0x1.50783487ee782p-4,  -0x1.e3074fde8871fp-8,  0x1.e8f434d018d63p-12, -0x1.6fadb9f155744p-16,
    0x1.aaec32af93359p-21, -0x1.8a404211f9547p-26, 0x1.2877020d52cf0p-31,
};

/* Initial cube root guess on [1, 2) (3-node Chebyshev interpolant, rel. error < 9e-4) */
static const double hdrg_math_cbrt_guess[3] = {0.625687, 0.433561, -0.058362};

static inline float hdrg_math_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));

    return value;
}

static inline double hdrg_math_from_bits64(uint64_t bits)
{
    double value;

    memcpy(&value, &bits, sizeof(value));

    return value;
}

static inline bool hdrg_math_isnan(float x)
{
    return x != x;
}

/* 2^t for any non-NaN t, accurate to about 1e-14 relative */
static inline double hdrg_math_exp2_kernel(double t)
{
    uint32_t n;
    uint8_t i;
    double r, p;

    if (t > (double)HDRG_MATH_EXP2_LIMIT) {
        t = (double)HDRG_MATH_EXP2_LIMIT;
    }

    if (t < -(double)HDRG_MATH_EXP2_LIMIT) {
        t = -(double)HDRG_MATH_EXP2_LIMIT;
    }

    /* t = k + f with k = n - LIMIT = round(t), |f| <= 1/2; t - k is exact */
    n = (uint32_t)(t + ((double)HDRG_MATH_EXP2_LIMIT + 0.5));
    r = (t - ((double)n - (double)HDRG_MATH_EXP2_LIMIT)) * HDRG_MATH_LN2;
    p = hdrg_math_exp_poly[11];
    for (i = 11; i > 0; i--) {
        p = p * r + hdrg_math_exp_poly[i - 1];
    }

    return p * hdrg_math_from_bits64((uint64_t)(n + 1023u - HDRG_MATH_EXP2_LIMIT) << 52);
}

/* log2(x) for finite x > 0 that is normal as a double (every positive float is) */
static inline double hdrg_math_log2_kernel(double x)
{
    uint64_t bits;
    uint32_t biased;
    uint8_t i;
    double m, s, z, p;

    memcpy(&bits, &x, sizeof(bits));
    biased = (uint32_t)((bits & HDRG_MATH_EXP_MASK) >> 52);
    bits = (bits & HDRG_MATH_MANT_MASK) | HDRG_MATH_ONE_BITS;

    /* m in [sqrt(1/2), sqrt(2)) keeps |s| <= 3 - 2 sqrt2 */
    if (bits > HDRG_MATH_SQRT2_BITS) {
        bits -= UINT64_C(1) << 52;
        biased++;
    }

    m = hdrg_math_from_bits64(bits);
    s = (m - 1.0) / (m + 1.0);
    z = s * s;
    p = hdrg_math_atanh_poly[8];
    for (i = 8; i > 0; i--) {
        p = p * z + hdrg_math_atanh_poly[i - 1];
    }

    /* ln(m) = 2 atanh(s) */
    return ((double)biased - 1023.0) + 2.0 * s * p * HDRG_MATH_LOG2E;
}

/* x^y for finite x > 0 */
static inline double hdrg_math_pow_kernel(double x, double y)
{
    return hdrg_math_exp2_kernel(y * hdrg_math_log2_kernel(x));
}

/* cbrt(a) for finite a > 0 that is normal as a double */
static inline double hdrg_math_cbrt_kernel(double a)
{
    uint64_t bits;
    uint32_t biased;
    uint8_t i;
    double m, y;

    memcpy(&bits, &a, sizeof(bits));
    biased = (uint32_t)((bits & HDRG_MATH_EXP_MASK) >> 52);
    m = hdrg_math_from_bits64((bits & HDRG_MATH_MANT_MASK) | HDRG_MATH_ONE_BITS);

    /* Newton on y^3 = m: the guess error 9e-4 squares to 8e-7, 6e-13, then double rounding */
    y = hdrg_math_cbrt_guess[0] + m * (hdrg_math_cbrt_guess[1] + m * hdrg_math_cbrt_guess[2]);
    for (i = 0; i < 3; i++) {
        y = y - (y * y * y - m) / (3.0 * y * y);
    }

    /* a = m 2^(biased - 1023) with 1023 = 3 * 341: cbrt(a) = cbrt(m 2^(biased % 3)) 2^q */
    if (biased % 3 == 1) {
        y *= HDRG_MATH_CBRT2;
    } else if (biased % 3 == 2) {
        y *= HDRG_MATH_CBRT4;
    }

    return y * hdrg_math_from_bits64((uint64_t)(biased / 3 + (1023 - 341)) << 52);
}

extern float hdrg_math_exp2f(float x)
{
    if (hdrg_math_isnan(x)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    return (float)hdrg_math_exp2_kernel((double)x);
}

extern float hdrg_math_expf(float x)
{
    if (hdrg_math_isnan(x)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    return (float)hdrg_math_exp2_kernel((double)x * HDRG_MATH_LOG2E);
}

extern float hdrg_math_log2f(float x)
{
    if (hdrg_math_isnan(x) || x < 0.0f) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (x == 0.0f) {
        return hdrg_math_from_bits(HDRG_MATH_NEG_INF_BITS);
    }

    if (x > FLT_MAX) {
        return x;
    }

    return (float)hdrg_math_log2_kernel((double)x);
}

extern float hdrg_math_powf(float x, float y)
{
    if (y == 0.0f || x == 1.0f) {
        return 1.0f;
    }

    if (hdrg_math_isnan(x) || hdrg_math_isnan(y) || x < 0.0f) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (x == 0.0f) {
        return (y > 0.0f) ? 0.0f : hdrg_math_from_bits(HDRG_MATH_INF_BITS);
    }

    if (x > FLT_MAX) {
        return (y > 0.0f) ? x : 0.0f;
    }

    return (float)hdrg_math_pow_kernel((double)x, (double)y);
}

extern float hdrg_math_cbrtf(float x)
{
    double r;

    if (hdrg_math_isnan(x)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (x == 0.0f || x > FLT_MAX || x < -FLT_MAX) {
        return x;
    }

    r = hdrg_math_cbrt_kernel((x < 0.0f) ? -(double)x : (double)x);

    return (float)((x < 0.0f) ? -r : r);
}

extern float hdrg_math_sinpif(float x)
{
    const double v = (double)x;
    uint8_t k;
    int64_t n;
    double r, r2, p;

    if (hdrg_math_isnan(x) || x > FLT_MAX || x < -FLT_MAX) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (x >= 0x1p23f || x <= -0x1p23f) {
        return x * 0.0f;
    }

    /* the nearest integer (halves up) and the exact remainder in [-1/2, 1/2) */
    n = (int64_t)(v + 0.5);
    if ((double)n > v + 0.5) {
        n--;
    }

    r = v - (double)n;
    r2 = r * r;
    p = hdrg_math_sinpi_poly[10];

    for (k = 10; k > 0; k--) {
        p = p * r2 + hdrg_math_sinpi_poly[k - 1u];
    }

    p *= r;

    if (r == 0.0) {
        return x * 0.0f;
    }

    return (float)((((uint64_t)n & 1u) != 0) ? -p : p);
}

extern float hdrg_math_srgb_eotf(float encoded)
{
    const double v = (double)encoded;

    if (hdrg_math_isnan(encoded)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (v <= HDRG_MATH_SRGB_EOTF_KNEE) {
        return (float)(v / 12.92);
    }

    if (encoded > FLT_MAX) {
        return encoded;
    }

    return (float)hdrg_math_pow_kernel((v + 0.055) / 1.055, 2.4);
}

extern float hdrg_math_srgb_oetf(float linear)
{
    const double v = (double)linear;

    if (hdrg_math_isnan(linear)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (v <= HDRG_MATH_SRGB_OETF_KNEE) {
        return (float)(v * 12.92);
    }

    if (linear > FLT_MAX) {
        return linear;
    }

    return (float)(1.055 * hdrg_math_exp2_kernel(hdrg_math_log2_kernel(v) / 2.4) - 0.055);
}

extern float hdrg_math_pq_eotf(float signal)
{
    const double e = (double)signal;
    double p, num;

    if (hdrg_math_isnan(signal)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (e <= 0.0) {
        return 0.0f;
    }

    if (e >= 1.0) {
        return (float)HDRG_MATH_PQ_PEAK;
    }

    p = hdrg_math_exp2_kernel(hdrg_math_log2_kernel(e) / HDRG_MATH_PQ_M2);
    num = p - HDRG_MATH_PQ_C1;
    if (num <= 0.0) {
        return 0.0f;
    }

    return (float)(HDRG_MATH_PQ_PEAK *
                   hdrg_math_exp2_kernel(
                       hdrg_math_log2_kernel(num / (HDRG_MATH_PQ_C2 - HDRG_MATH_PQ_C3 * p)) /
                       HDRG_MATH_PQ_M1));
}

extern float hdrg_math_pq_oetf(float nits)
{
    const double y = (double)nits / HDRG_MATH_PQ_PEAK;
    double p;

    if (hdrg_math_isnan(nits)) {
        return hdrg_math_from_bits(HDRG_MATH_NAN_BITS);
    }

    if (y >= 1.0) {
        return 1.0f;
    }

    p = (y > 0.0) ? hdrg_math_pow_kernel(y, HDRG_MATH_PQ_M1) : 0.0;

    return (float)hdrg_math_pow_kernel(
        (HDRG_MATH_PQ_C1 + HDRG_MATH_PQ_C2 * p) / (1.0 + HDRG_MATH_PQ_C3 * p), HDRG_MATH_PQ_M2);
}
