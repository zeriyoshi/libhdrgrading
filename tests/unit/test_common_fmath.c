/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/math.h"

/*
 * float 多項式版 (hdrgrading/foundation/fmath.h) の精度: double で正しく丸める参照
 * (hdrgrading/foundation/math.h) に対して、log2 / exp2 は約 2 ulp、sRGB / PQ の OETF は 8bit の
 * コード値で 0.01 LSB 以下。整数の境界は厳密に一致する。
 */
#define TF_SAMPLES 1000000u
#define TF_LSB     (0.01 / 255.0)

/* 2 ulp of a float near v, at least 2^-22 */
static double two_ulp(float v)
{
    double u;

    u = fabs((double)v) * 0x1p-22;

    return (u > 0x1p-22) ? u : 0x1p-22;
}

static void test_log2_accuracy(void)
{
    uint32_t i;
    float x, ref;

    for (i = 0; i < TF_SAMPLES; i++) {
        x = hdrg_math_exp2f(-125.0f + 252.0f * (float)i / (float)TF_SAMPLES);
        ref = hdrg_math_log2f(x);
        TEST_ASSERT_TRUE(fabs((double)hdrg_fmath_log2(x) - (double)ref) <= two_ulp(ref));
    }

    TEST_ASSERT_EQUAL_FLOAT(0.0f, hdrg_fmath_log2(1.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hdrg_fmath_log2(2.0f));
    TEST_ASSERT_EQUAL_FLOAT(-14.0f, hdrg_fmath_log2(0x1p-14f));
}

static void test_exp2_accuracy(void)
{
    uint32_t i;
    float x, ref;

    for (i = 0; i <= TF_SAMPLES; i++) {
        x = -126.0f + 253.0f * (float)i / (float)TF_SAMPLES;
        ref = hdrg_math_exp2f(x);
        TEST_ASSERT_TRUE(fabs((double)hdrg_fmath_exp2(x) - (double)ref) <= 2.5e-7 * (double)ref);
    }

    TEST_ASSERT_EQUAL_FLOAT(1.0f, hdrg_fmath_exp2(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, hdrg_fmath_exp2(-1.0f));
    TEST_ASSERT_EQUAL_FLOAT(1024.0f, hdrg_fmath_exp2(10.0f));
    TEST_ASSERT_EQUAL_FLOAT(0x1p-126f, hdrg_fmath_exp2(-500.0f));
}

static void test_srgb_oetf_within_hundredth_lsb(void)
{
    uint32_t i;
    float x;

    for (i = 0; i <= TF_SAMPLES; i++) {
        x = (float)i / (float)TF_SAMPLES;
        TEST_ASSERT_TRUE(fabs((double)hdrg_fmath_srgb_oetf(x) - (double)hdrg_math_srgb_oetf(x)) <=
                         TF_LSB);
    }

    TEST_ASSERT_EQUAL_FLOAT(0.0f, hdrg_fmath_srgb_oetf(-1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, hdrg_fmath_srgb_oetf(2.0f));
}

static void test_pq_oetf_within_hundredth_lsb(void)
{
    uint32_t i;
    float nits;

    /* linear over the range, then geometric toward black */
    for (i = 0; i <= TF_SAMPLES; i++) {
        nits = (i < TF_SAMPLES / 2) ? 10000.0f * (float)i / (float)(TF_SAMPLES / 2)
                                    : hdrg_math_exp2f(-20.0f + 33.3f * (float)(i - TF_SAMPLES / 2) /
                                                                   (float)(TF_SAMPLES / 2));
        TEST_ASSERT_TRUE(fabs((double)hdrg_fmath_pq_oetf(nits) - (double)hdrg_math_pq_oetf(nits)) <=
                         TF_LSB);
    }

    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, hdrg_fmath_pq_oetf(20000.0f));
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
    RUN_TEST(test_log2_accuracy);
    RUN_TEST(test_exp2_accuracy);
    RUN_TEST(test_srgb_oetf_within_hundredth_lsb);
    RUN_TEST(test_pq_oetf_within_hundredth_lsb);

    return UNITY_END();
}
