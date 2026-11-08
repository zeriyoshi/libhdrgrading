/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"

/* 参照実装は libm の double 版 (テストでは libm を使ってよい) */
#define TSM_PI 3.14159265358979323846 /* M_PI is not C11 */
#define PQ_M1  (2610.0 / 16384.0)
#define PQ_M2  (2523.0 / 4096.0 * 128.0)
#define PQ_C1  (3424.0 / 4096.0)
#define PQ_C2  (2413.0 / 4096.0 * 32.0)
#define PQ_C3  (2392.0 / 4096.0 * 32.0)

#define NAN_BITS UINT32_C(0x7fc00000)

/* 正規数の結果に対する上限: 0.5 ulp (正しい丸め) + 内部 double 計算の誤差分 */
#define MAX_ULP 0.5000005f
#define MAX_REL 6.0e-8f

/* コード値で 0.01 LSB 以下 */
#define MAX_LSB 0.01f

typedef struct {
    uint64_t count, misrounded;
    double max_ulp, max_rel;
} err_stat_t;

/* powf の特殊値: {x, y, 期待値} と NaN になる {x, y} */
static const float pow_cases[][3] = {
    {NAN, 0.0f, 1.0f},           {NAN, -0.0f, 1.0f},          {1.0f, NAN, 1.0f},
    {1.0f, INFINITY, 1.0f},      {1.0f, -INFINITY, 1.0f},     {INFINITY, 0.0f, 1.0f},
    {0.0f, 0.0f, 1.0f},          {0.0f, 2.0f, 0.0f},          {-0.0f, 2.0f, 0.0f},
    {0.0f, -1.0f, INFINITY},     {-0.0f, -0.5f, INFINITY},    {0.0f, INFINITY, 0.0f},
    {0.0f, -INFINITY, INFINITY}, {INFINITY, 2.0f, INFINITY},  {INFINITY, -2.0f, 0.0f},
    {0.5f, INFINITY, 0.0f},      {0.5f, -INFINITY, INFINITY}, {2.0f, INFINITY, INFINITY},
    {2.0f, -INFINITY, 0.0f},     {4.0f, 0.5f, 2.0f},          {2.0f, 10.0f, 1024.0f},
    {2.0f, 128.0f, INFINITY},    {0.25f, -1.5f, 8.0f},        {3.0f, 2.0f, 9.0f},
};

static const float pow_nan_cases[][2] = {
    {NAN, 1.0f}, {2.0f, NAN}, {NAN, NAN}, {-1.0f, 2.0f}, {-2.0f, 0.5f}, {-INFINITY, 1.0f},
};

static inline uint32_t bits_of(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));

    return bits;
}

static inline float from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));

    return value;
}

/* 参照値 ref の 2 進桁での float の ulp (サブノーマル域は 2^-149) */
static double ulp_of(double ref)
{
    int exponent;

    if (ref == 0.0) {
        return ldexp(1.0, -149);
    }

    (void)frexp(fabs(ref), &exponent);

    return ldexp(1.0, (exponent - 24 < -149) ? -149 : exponent - 24);
}

static void err_add(err_stat_t *stat, float got, double ref)
{
    double diff;

    if (!isfinite(ref) || fabs(ref) > FLT_MAX) {
        return;
    }

    stat->count++;

    if (got != (float)ref) {
        stat->misrounded++;
    }

    diff = fabs((double)got - ref);
    stat->max_ulp = fmax(stat->max_ulp, diff / ulp_of(ref));

    if (fabs(ref) >= FLT_MIN) {
        stat->max_rel = fmax(stat->max_rel, diff / fabs(ref));
    }
}

static void err_check(const char *name, const err_stat_t *stat)
{
    char message[192];

    (void)snprintf(message, sizeof(message),
                   "%s: %llu points, max %.9f ulp, max rel %.3e, %llu not correctly rounded", name,
                   (unsigned long long)stat->count, stat->max_ulp, stat->max_rel,
                   (unsigned long long)stat->misrounded);
    TEST_MESSAGE(message);

    TEST_ASSERT_TRUE(stat->count > 0);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_ULP, (float)stat->max_ulp);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_REL, (float)stat->max_rel);
}

static double ref_srgb_eotf(double v)
{
    return (v <= 0.04045) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double ref_srgb_oetf(double l)
{
    return (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

static double ref_pq_eotf(double e)
{
    double p;

    e = fmin(fmax(e, 0.0), 1.0);
    p = pow(e, 1.0 / PQ_M2);

    return 10000.0 * pow(fmax(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

static double ref_pq_oetf(double nits)
{
    double y, p;

    y = fmin(fmax(nits / 10000.0, 0.0), 1.0);
    p = pow(y, PQ_M1);

    return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

/* 折れ点 knee 以下 (線形区間) で最大の float のビット列 */
static uint32_t knee_bits(double knee)
{
    uint32_t bits = bits_of((float)knee);

    if ((double)from_bits(bits) > knee) {
        bits--;
    }

    return bits;
}

static void test_exp2f_sweep(void)
{
    const uint32_t steps = UINT32_C(1) << 21;
    err_stat_t stat = {0};
    uint32_t i, bits;
    float x;

    /* 一様格子: 結果がサブノーマル・オーバーフローになる端まで含める */
    for (i = 0; i < steps; i++) {
        x = (float)(-151.0 + 280.0 * (double)i / (double)(steps - 1));
        err_add(&stat, hdrg_math_exp2f(x), exp2((double)x));
    }

    /* |x| < 1 はビット列で走査 (0 近傍の対数間隔) */
    for (bits = 0; bits < UINT32_C(0x3f800000); bits += 1597) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_exp2f(x), exp2((double)x));
        err_add(&stat, hdrg_math_exp2f(-x), exp2(-(double)x));
    }

    err_check("exp2f", &stat);
}

static void test_expf_sweep(void)
{
    const uint32_t steps = UINT32_C(1) << 21;
    err_stat_t stat = {0};
    uint32_t i, bits;
    float x;

    for (i = 0; i < steps; i++) {
        x = (float)(-105.0 + 195.0 * (double)i / (double)(steps - 1));
        err_add(&stat, hdrg_math_expf(x), exp((double)x));
    }

    for (bits = 0; bits < UINT32_C(0x3f800000); bits += 1597) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_expf(x), exp((double)x));
        err_add(&stat, hdrg_math_expf(-x), exp(-(double)x));
    }

    err_check("expf", &stat);
}

static void test_log2f_sweep(void)
{
    err_stat_t stat = {0};
    uint32_t bits;
    float x;

    /* 正の有限 float 全域 (サブノーマルを含む) を対数間隔で */
    for (bits = 1; bits < UINT32_C(0x7f800000); bits += 1021) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_log2f(x), log2((double)x));
    }

    /* 1 の近傍は全 float: 結果が 0 に近く相対誤差が最も厳しい */
    for (bits = bits_of(1.0f - 0x1p-9f); bits <= bits_of(1.0f + 0x1p-9f); bits++) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_log2f(x), log2((double)x));
    }

    err_check("log2f", &stat);
}

static void test_powf_sweep(void)
{
    const float exponents[6] = {
        2.4f, 1.0f / 2.4f, (float)PQ_M1, (float)PQ_M2, (float)(1.0 / PQ_M1), (float)(1.0 / PQ_M2),
    };
    err_stat_t stat = {0};
    uint32_t i, j;
    uint8_t k;
    float x, y;

    for (i = 0; i < 1024; i++) {
        x = (float)exp2(-40.0 + 80.0 * (double)i / 1023.0);
        for (j = 0; j < 1024; j++) {
            y = (float)(-8.0 + 16.0 * (double)j / 1023.0);
            err_add(&stat, hdrg_math_powf(x, y), pow((double)x, (double)y));
        }
    }

    /* sRGB / PQ が使う指数を 16 bit のコード値全点で */
    for (k = 0; k < 6; k++) {
        for (i = 0; i <= 65535; i++) {
            x = (float)i / 65535.0f;
            err_add(&stat, hdrg_math_powf(x, exponents[k]), pow((double)x, (double)exponents[k]));
        }
    }

    err_check("powf", &stat);
}

static void test_cbrtf_sweep(void)
{
    err_stat_t stat = {0};
    uint32_t bits;
    float x, r;

    for (bits = 1; bits < UINT32_C(0x7f800000); bits += 1021) {
        x = from_bits(bits);
        r = hdrg_math_cbrtf(x);
        err_add(&stat, r, cbrt((double)x));
        TEST_ASSERT_EQUAL_HEX32(bits_of(-r), bits_of(hdrg_math_cbrtf(-x)));
    }

    err_check("cbrtf", &stat);
}

static void test_exp2f_expf_special(void)
{
    uint32_t k;

    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_exp2f(from_bits(UINT32_C(0xffc12345)))));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_exp2f(-INFINITY)));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_exp2f(INFINITY));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_exp2f(128.0f));
    TEST_ASSERT_TRUE(hdrg_math_exp2f(0x1.fffffep+6f) <= FLT_MAX);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hdrg_math_exp2f(-0.0f));

    /* 2^-150 は 0 と 2^-149 のちょうど中点: 偶数丸めで +0 */
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_exp2f(-150.0f)));
    TEST_ASSERT_EQUAL_HEX32(1, bits_of(hdrg_math_exp2f(-149.0f)));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_exp2f(-200.0f)));

    /* 整数では厳密 */
    for (k = 0; k <= 276; k++) {
        TEST_ASSERT_EQUAL_HEX32(bits_of(ldexpf(1.0f, (int)k - 149)),
                                bits_of(hdrg_math_exp2f((float)k - 149.0f)));
    }

    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_expf(NAN)));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_expf(-INFINITY)));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_expf(INFINITY));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_expf(88.73f));
    TEST_ASSERT_TRUE(hdrg_math_expf(88.72f) <= FLT_MAX);
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_expf(-104.0f)));
    TEST_ASSERT_EQUAL_HEX32(1, bits_of(hdrg_math_expf(-103.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(hdrg_math_expf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x402df854), bits_of(hdrg_math_expf(1.0f)));
}

static void test_log2f_special(void)
{
    uint32_t k;

    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_log2f(NAN)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_log2f(-1.0f)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_log2f(-INFINITY)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_log2f(-from_bits(1))));
    TEST_ASSERT_FLOAT_IS_NEG_INF(hdrg_math_log2f(0.0f));
    TEST_ASSERT_FLOAT_IS_NEG_INF(hdrg_math_log2f(-0.0f));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_log2f(INFINITY));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_log2f(1.0f)));

    /* 2 のべき乗 (サブノーマルを含む) では厳密 */
    for (k = 0; k <= 276; k++) {
        TEST_ASSERT_EQUAL_HEX32(bits_of((float)k - 149.0f),
                                bits_of(hdrg_math_log2f(ldexpf(1.0f, (int)k - 149))));
    }

    TEST_ASSERT_EQUAL_HEX32(bits_of(-149.0f), bits_of(hdrg_math_log2f(from_bits(1))));
}

static void test_powf_special(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(pow_cases) / sizeof(pow_cases[0]); i++) {
        TEST_ASSERT_EQUAL_HEX32(bits_of(pow_cases[i][2]),
                                bits_of(hdrg_math_powf(pow_cases[i][0], pow_cases[i][1])));
    }

    for (i = 0; i < sizeof(pow_nan_cases) / sizeof(pow_nan_cases[0]); i++) {
        TEST_ASSERT_EQUAL_HEX32(NAN_BITS,
                                bits_of(hdrg_math_powf(pow_nan_cases[i][0], pow_nan_cases[i][1])));
    }

    TEST_ASSERT_EQUAL_HEX32(1, bits_of(hdrg_math_powf(2.0f, -149.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(0x1p-140f), bits_of(hdrg_math_powf(0x1p-70f, 2.0f)));
}

static void test_cbrtf_special(void)
{
    uint32_t k, n;

    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_cbrtf(NAN)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(0.0f), bits_of(hdrg_math_cbrtf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(-0.0f), bits_of(hdrg_math_cbrtf(-0.0f)));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_cbrtf(INFINITY));
    TEST_ASSERT_FLOAT_IS_NEG_INF(hdrg_math_cbrtf(-INFINITY));

    /* 2 の 3k 乗 (サブノーマルを含む) と 2^24 以下の整数の立方は厳密 */
    for (k = 0; k <= 91; k++) {
        TEST_ASSERT_EQUAL_HEX32(bits_of(ldexpf(1.0f, (int)k - 49)),
                                bits_of(hdrg_math_cbrtf(ldexpf(1.0f, 3 * ((int)k - 49)))));
    }

    for (n = 1; n <= 255; n++) {
        TEST_ASSERT_EQUAL_HEX32(bits_of((float)n), bits_of(hdrg_math_cbrtf((float)(n * n * n))));
        TEST_ASSERT_EQUAL_HEX32(bits_of(-(float)n), bits_of(hdrg_math_cbrtf(-(float)(n * n * n))));
    }
}

static void test_curve_special(void)
{
    const float pq_floor = (float)ref_pq_oetf(0.0);

    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_srgb_eotf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(hdrg_math_srgb_eotf(1.0f)));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_srgb_oetf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(hdrg_math_srgb_oetf(1.0f)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_srgb_eotf(NAN)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_srgb_oetf(NAN)));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_srgb_eotf(INFINITY));
    TEST_ASSERT_FLOAT_IS_INF(hdrg_math_srgb_oetf(INFINITY));
    TEST_ASSERT_EQUAL_HEX32(bits_of((float)(-0.5 / 12.92)), bits_of(hdrg_math_srgb_eotf(-0.5f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of((float)(-0.5 * 12.92)), bits_of(hdrg_math_srgb_oetf(-0.5f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of((float)ref_srgb_eotf(2.0)), bits_of(hdrg_math_srgb_eotf(2.0f)));

    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_pq_eotf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_pq_eotf(-1.0f)));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_pq_eotf(pq_floor * 0.5f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(10000.0f), bits_of(hdrg_math_pq_eotf(1.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(10000.0f), bits_of(hdrg_math_pq_eotf(INFINITY)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_pq_eotf(NAN)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(hdrg_math_pq_oetf(10000.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(hdrg_math_pq_oetf(INFINITY)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(pq_floor), bits_of(hdrg_math_pq_oetf(0.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(pq_floor), bits_of(hdrg_math_pq_oetf(-5.0f)));
    TEST_ASSERT_EQUAL_HEX32(bits_of(pq_floor), bits_of(hdrg_math_pq_oetf(-INFINITY)));
    TEST_ASSERT_EQUAL_HEX32(NAN_BITS, bits_of(hdrg_math_pq_oetf(NAN)));

    /* BT.2408 の基準: 100 nit = 0.5081, 203 nit = 0.5807 (小数 4 桁) */
    TEST_ASSERT_FLOAT_WITHIN(5e-5f, 0.5081f, hdrg_math_pq_oetf(100.0f));
    TEST_ASSERT_FLOAT_WITHIN(5e-5f, 0.5807f, hdrg_math_pq_oetf(203.0f));
}

/*
 * 8 / 10 / 12 / 16 bit の全コード値で、誤差をコード値の LSB 単位で測る。
 * EOTF (信号 -> 線形) は出力を参照 OETF で信号に戻した差、OETF (線形 -> 信号) は同じ float 入力に
 * 対する参照値との差。入力の float 量子化は誤差に含めない。
 */
static void test_code_values(void)
{
    const uint8_t depths[4] = {8, 10, 12, 16};
    uint32_t c;
    uint8_t d;
    char message[160];
    double max, err[4];
    float v, linear, nits;

    for (d = 0; d < 4; d++) {
        max = (double)((UINT32_C(1) << depths[d]) - 1);
        err[0] = err[1] = err[2] = err[3] = 0.0;
        for (c = 0; c <= (uint32_t)max; c++) {
            v = (float)((double)c / max);
            err[0] = fmax(err[0], fabs(ref_srgb_oetf((double)hdrg_math_srgb_eotf(v)) -
                                       ref_srgb_oetf(ref_srgb_eotf((double)v))) *
                                      max);
            err[2] = fmax(err[2], fabs(ref_pq_oetf((double)hdrg_math_pq_eotf(v)) -
                                       ref_pq_oetf(ref_pq_eotf((double)v))) *
                                      max);

            linear = (float)ref_srgb_eotf((double)c / max);
            nits = (float)ref_pq_eotf((double)c / max);
            err[1] = fmax(
                err[1],
                fabs((double)hdrg_math_srgb_oetf(linear) - ref_srgb_oetf((double)linear)) * max);
            err[3] = fmax(err[3],
                          fabs((double)hdrg_math_pq_oetf(nits) - ref_pq_oetf((double)nits)) * max);
        }

        (void)snprintf(message, sizeof(message),
                       "%2u-bit: sRGB EOTF %.2e / OETF %.2e LSB, PQ EOTF %.2e / OETF %.2e LSB",
                       (unsigned int)depths[d], err[0], err[1], err[2], err[3]);
        TEST_MESSAGE(message);

        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_LSB, (float)err[0]);
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_LSB, (float)err[1]);
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_LSB, (float)err[2]);
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(MAX_LSB, (float)err[3]);
    }
}

static void test_curve_roundtrip(void)
{
    uint32_t c;
    float v;

    for (c = 0; c <= 65535; c++) {
        v = (float)c / 65535.0f;
        TEST_ASSERT_FLOAT_WITHIN(0.01f / 65535.0f, v, hdrg_math_srgb_oetf(hdrg_math_srgb_eotf(v)));

        if (v >= 0.001f) {
            TEST_ASSERT_FLOAT_WITHIN(0.01f / 65535.0f, v, hdrg_math_pq_oetf(hdrg_math_pq_eotf(v)));
        }
    }
}

static void test_curve_monotonic(void)
{
    const uint32_t oetf_knee = knee_bits(0.0031308), eotf_knee = knee_bits(0.04045);
    uint32_t bits, c;
    uint8_t k;
    float x, prev[4], cur[4];

    /* [0, 1] の float をビット列の刻み 1021 で */
    prev[0] = prev[1] = prev[2] = prev[3] = -1.0f;
    for (bits = 0; bits <= bits_of(1.0f); bits += 1021) {
        x = from_bits(bits);
        cur[0] = hdrg_math_srgb_eotf(x);
        cur[1] = hdrg_math_srgb_oetf(x);
        cur[2] = hdrg_math_pq_eotf(x);
        cur[3] = hdrg_math_pq_oetf(x * 10000.0f);
        for (k = 0; k < 4; k++) {
            if (k != 1 || bits <= oetf_knee || bits - 1021 > oetf_knee) {
                TEST_ASSERT_TRUE(cur[k] >= prev[k]);
            }

            prev[k] = cur[k];
        }
    }

    /*
     * IEC 61966-2-1 の定数は OETF の折れ点に 1.7e-6 の下向きの段差を残す (8 bit で 0.0004 LSB)。
     * 参照式と同じ段差であることを確かめ、それ以外の区間は単調であることを全 float で確かめる
     */
    x = hdrg_math_srgb_oetf(from_bits(oetf_knee + 1)) - hdrg_math_srgb_oetf(from_bits(oetf_knee));
    TEST_ASSERT_TRUE(x < 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-8f,
                             (float)(ref_srgb_oetf((double)from_bits(oetf_knee + 1)) -
                                     ref_srgb_oetf((double)from_bits(oetf_knee))),
                             x);

    for (bits = oetf_knee - 4096; bits < oetf_knee + 4096; bits++) {
        if (bits != oetf_knee) {
            TEST_ASSERT_TRUE(hdrg_math_srgb_oetf(from_bits(bits + 1)) >=
                             hdrg_math_srgb_oetf(from_bits(bits)));
        }
    }

    for (bits = eotf_knee - 4096; bits < eotf_knee + 4096; bits++) {
        TEST_ASSERT_TRUE(hdrg_math_srgb_eotf(from_bits(bits + 1)) >=
                         hdrg_math_srgb_eotf(from_bits(bits)));
    }

    /* 16 bit のコード値の列では狭義単調増加 */
    for (c = 1; c <= 65535; c++) {
        TEST_ASSERT_TRUE(hdrg_math_srgb_eotf((float)c / 65535.0f) >
                         hdrg_math_srgb_eotf((float)(c - 1) / 65535.0f));
        TEST_ASSERT_TRUE(hdrg_math_pq_eotf((float)c / 65535.0f) >
                         hdrg_math_pq_eotf((float)(c - 1) / 65535.0f));
        TEST_ASSERT_TRUE(hdrg_math_srgb_oetf((float)ref_srgb_eotf((double)c / 65535.0)) >
                         hdrg_math_srgb_oetf((float)ref_srgb_eotf((double)(c - 1) / 65535.0)));
        TEST_ASSERT_TRUE(hdrg_math_pq_oetf((float)ref_pq_eotf((double)c / 65535.0)) >
                         hdrg_math_pq_oetf((float)ref_pq_eotf((double)(c - 1) / 65535.0)));
    }
}

/* sin(pi x) の参照: 被検査側と同じ厳密な剰余 r = x - round(x) から libm の sin */
static double ref_sinpi(float x)
{
    double n, r;

    n = floor((double)x + 0.5);
    r = (double)x - n;

    return (fmod(n, 2.0) != 0.0) ? -sin(TSM_PI * r) : sin(TSM_PI * r);
}

static void test_sinpif_sweep(void)
{
    const uint32_t steps = UINT32_C(1) << 21;
    err_stat_t stat = {0};
    uint32_t i, bits;
    float x;

    for (i = 0; i < steps; i++) {
        x = (float)(-8.0 + 16.0 * (double)i / (double)(steps - 1));
        err_add(&stat, hdrg_math_sinpif(x), ref_sinpi(x));
    }

    /* |x| < 1 はビット列で走査 (0 近傍の対数間隔) */
    for (bits = 0; bits < UINT32_C(0x3f800000); bits += 1597) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_sinpif(x), ref_sinpi(x));
        err_add(&stat, hdrg_math_sinpif(-x), ref_sinpi(-x));
    }

    /* 大きな x (剰余は厳密) */
    for (bits = UINT32_C(0x3f800000); bits < UINT32_C(0x4b000000); bits += 4099) {
        x = from_bits(bits);
        err_add(&stat, hdrg_math_sinpif(x), ref_sinpi(x));
    }

    err_check("sinpif", &stat);
}

static void test_sinpif_special(void)
{
    int32_t n;

    for (n = -40; n <= 40; n++) {
        TEST_ASSERT_TRUE(hdrg_math_sinpif((float)n) == 0.0f);
        TEST_ASSERT_TRUE((signbit(hdrg_math_sinpif((float)n)) != 0) == (n < 0));
        TEST_ASSERT_EQUAL_FLOAT((n % 2 == 0) ? 1.0f : -1.0f, hdrg_math_sinpif((float)n + 0.5f));
    }

    TEST_ASSERT_TRUE(signbit(hdrg_math_sinpif(-0.0f)));
    TEST_ASSERT_TRUE(hdrg_math_sinpif(0x1p23f) == 0.0f && hdrg_math_sinpif(1e30f) == 0.0f);
    TEST_ASSERT_TRUE(isnan(hdrg_math_sinpif(NAN)) && isnan(hdrg_math_sinpif(INFINITY)) &&
                     isnan(hdrg_math_sinpif(-INFINITY)));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x7fc00000), bits_of(hdrg_math_sinpif(INFINITY)));
}

static void test_transcendental_monotonic(void)
{
    uint32_t bits;
    float x, next;

    /* [1, 1.125) の全 float (連続する入力) で広義単調 */
    for (bits = bits_of(1.0f); bits < bits_of(1.125f); bits++) {
        x = from_bits(bits);
        next = from_bits(bits + 1);
        TEST_ASSERT_TRUE(hdrg_math_exp2f(next) >= hdrg_math_exp2f(x));
        TEST_ASSERT_TRUE(hdrg_math_log2f(next) >= hdrg_math_log2f(x));
        TEST_ASSERT_TRUE(hdrg_math_cbrtf(next) >= hdrg_math_cbrtf(x));
    }
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exp2f_sweep);
    RUN_TEST(test_expf_sweep);
    RUN_TEST(test_log2f_sweep);
    RUN_TEST(test_powf_sweep);
    RUN_TEST(test_cbrtf_sweep);
    RUN_TEST(test_exp2f_expf_special);
    RUN_TEST(test_log2f_special);
    RUN_TEST(test_powf_special);
    RUN_TEST(test_cbrtf_special);
    RUN_TEST(test_curve_special);
    RUN_TEST(test_code_values);
    RUN_TEST(test_curve_roundtrip);
    RUN_TEST(test_curve_monotonic);
    RUN_TEST(test_transcendental_monotonic);
    RUN_TEST(test_sinpif_sweep);
    RUN_TEST(test_sinpif_special);

    return UNITY_END();
}
