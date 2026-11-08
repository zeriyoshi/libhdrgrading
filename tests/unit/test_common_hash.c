/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hdrgrading/foundation/hash.h"

#define KEY  UINT64_C(0x0123456789abcdef)
#define GRID 512u

typedef struct {
    uint64_t key;
    uint32_t x, y, sample, expected;
} hash_kat_t;

/* tests の外で Python (任意精度整数) で独立に計算した値。構成を変えたらここも作り直す */
static const hash_kat_t kat[] = {
    {UINT64_C(0x0000000000000000), 0, 0, 0, UINT32_C(0xc7f0bdfe)},
    {UINT64_C(0x0000000000000000), 1, 0, 0, UINT32_C(0xf1c002d4)},
    {UINT64_C(0x0000000000000000), 0, 1, 0, UINT32_C(0xb439f37c)},
    {UINT64_C(0x0000000000000000), 0, 0, 1, UINT32_C(0x59418bcf)},
    {UINT64_C(0x0000000000000001), 0, 0, 0, UINT32_C(0x250a4100)},
    {UINT64_C(0x0000000100000000), 0, 0, 0, UINT32_C(0x74dcdfd5)},
    {UINT64_C(0x0123456789abcdef), 1920, 1080, 2, UINT32_C(0x265053dd)},
    {UINT64_C(0xffffffffffffffff), 4294967295u, 4294967295u, 4294967295u, UINT32_C(0x0850764c)},
    {UINT64_C(0xfedcba9876543210), 7, 3, 5, UINT32_C(0xe430145d)},
};

/* 試験入力の生成用 (splitmix64)。被試験側とは独立した生成器にする */
static uint64_t next_input(uint64_t *state)
{
    uint64_t z;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);

    return z ^ (z >> 31);
}

/* words = {key 下位, key 上位, y, x, sample} */
static uint32_t hash_words(const uint32_t words[5])
{
    return hdrg_hash_u32(((uint64_t)words[1] << 32) | words[0], words[3], words[2], words[4]);
}

static int compare_u32(const void *a, const void *b)
{
    const uint32_t lhs = *(const uint32_t *)a, rhs = *(const uint32_t *)b;

    return (lhs > rhs) - (lhs < rhs);
}

static void test_known_answers(void)
{
    uint32_t i;

    TEST_ASSERT_EQUAL_HEX32(0, hdrg_hash_mix32(0));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x688990c0), hdrg_hash_mix32(1));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xcc4b4124), hdrg_hash_mix32(UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0xe628c683), hdrg_hash_mix32(UINT32_C(0xdeadbeef)));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x6768824a), hdrg_hash_mix32(UINT32_C(0xffffffff)));

    for (i = 0; i < sizeof(kat) / sizeof(kat[0]); i++) {
        TEST_ASSERT_EQUAL_HEX32(kat[i].expected,
                                hdrg_hash_u32(kat[i].key, kat[i].x, kat[i].y, kat[i].sample));
    }
}

static void test_stages_match_full(void)
{
    const uint64_t keys[3] = {0, KEY, UINT64_MAX};
    uint32_t x, y, s, row, pixel;
    uint8_t k;

    for (k = 0; k < 3; k++) {
        for (y = 0; y < 16; y++) {
            row = hdrg_hash_row(keys[k], y);
            for (x = 0; x < 16; x++) {
                pixel = hdrg_hash_pixel(row, x);
                for (s = 0; s < 4; s++) {
                    TEST_ASSERT_EQUAL_HEX32(hdrg_hash_u32(keys[k], x, y, s),
                                            hdrg_hash_sample(pixel, s));
                    TEST_ASSERT_TRUE(hdrg_hash_unit(hdrg_hash_sample(pixel, s)) ==
                                     hdrg_hash_uniform(keys[k], x, y, s));
                    TEST_ASSERT_TRUE(hdrg_hash_tri(hdrg_hash_sample(pixel, 2 * s),
                                                   hdrg_hash_sample(pixel, 2 * s + 1)) ==
                                     hdrg_hash_triangular(keys[k], x, y, s));
                }
            }
        }
    }
}

/* 鍵を固定すると row は y の、pixel は x の全単射 (同じ鍵の中で行・画素の状態が衝突しない) */
static void test_stage_bijective(void)
{
    uint32_t *states, i;

    states = (uint32_t *)malloc(sizeof(uint32_t) * 65536);
    TEST_ASSERT_NOT_NULL(states);

    for (i = 0; i < 65536; i++) {
        states[i] = hdrg_hash_row(KEY, i);
    }

    qsort(states, 65536, sizeof(uint32_t), compare_u32);

    for (i = 1; i < 65536; i++) {
        TEST_ASSERT_TRUE(states[i - 1] != states[i]);
    }

    for (i = 0; i < 65536; i++) {
        states[i] = hdrg_hash_pixel(hdrg_hash_row(KEY, 7), i);
    }

    qsort(states, 65536, sizeof(uint32_t), compare_u32);

    for (i = 1; i < 65536; i++) {
        TEST_ASSERT_TRUE(states[i - 1] != states[i]);
    }

    free(states);
}

static void test_uniform_distribution(void)
{
    uint64_t ones[32] = {0}, count = 0;
    uint32_t x, y, c, h;
    uint8_t b;
    double sum = 0.0, sum_sq = 0.0, mean, var;
    float u;

    for (y = 0; y < GRID; y++) {
        for (x = 0; x < GRID; x++) {
            for (c = 0; c < 3; c++) {
                h = hdrg_hash_u32(KEY, x, y, c);
                u = hdrg_hash_unit(h);
                TEST_ASSERT_TRUE(u >= 0.0f && u < 1.0f);
                TEST_ASSERT_TRUE(floorf(u * 0x1p24f) == u * 0x1p24f);

                for (b = 0; b < 32; b++) {
                    ones[b] += (h >> b) & 1u;
                }

                sum += (double)u;
                sum_sq += (double)u * (double)u;
                count++;
            }
        }
    }

    mean = sum / (double)count;
    var = sum_sq / (double)count - mean * mean;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.5f, (float)mean);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f / 12.0f, (float)var);

    for (b = 0; b < 32; b++) {
        TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.5f, (float)((double)ones[b] / (double)count));
    }

    TEST_ASSERT_TRUE(hdrg_hash_unit(0) == 0.0f);
    TEST_ASSERT_TRUE(hdrg_hash_unit(UINT32_MAX) == 1.0f - 0x1p-24f);
}

static void test_triangular_distribution(void)
{
    uint64_t bins[20] = {0}, count = 0;
    uint32_t x, y, c;
    uint8_t i;
    double sum = 0.0, sum_sq = 0.0, mean, var, lo, hi, expected;
    float t;

    for (y = 0; y < GRID; y++) {
        for (x = 0; x < GRID; x++) {
            for (c = 0; c < 3; c++) {
                t = hdrg_hash_triangular(KEY, x, y, c);
                TEST_ASSERT_TRUE(t > -1.0f && t < 1.0f);

                sum += (double)t;
                sum_sq += (double)t * (double)t;
                bins[(uint32_t)(((double)t + 1.0) * 10.0)]++;
                count++;
            }
        }
    }

    mean = sum / (double)count;
    var = sum_sq / (double)count - mean * mean;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.0f, (float)mean);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.0f / 6.0f, (float)var);

    /* 密度 1 - |t| の三角分布: 幅 0.1 の各 bin の確率と比べる */
    for (i = 0; i < 20; i++) {
        lo = -1.0 + 0.1 * (double)i;
        hi = lo + 0.1;
        expected =
            (lo < 0.0) ? (hi - lo) * (1.0 + (lo + hi) / 2.0) : (hi - lo) * (1.0 - (lo + hi) / 2.0);
        TEST_ASSERT_FLOAT_WITHIN(0.002f, (float)expected, (float)((double)bins[i] / (double)count));
    }

    TEST_ASSERT_TRUE(hdrg_hash_tri(UINT32_C(0x12345678), UINT32_C(0x12345678)) == 0.0f);
    TEST_ASSERT_TRUE(hdrg_hash_tri(UINT32_MAX, 0) == 1.0f - 0x1p-24f);
    TEST_ASSERT_TRUE(hdrg_hash_tri(0, UINT32_MAX) == -(1.0f - 0x1p-24f));
}

/* 隣接する画素・行・チャネル・鍵の値の相関係数 (N = 511^2 * 3 で 1 sigma = 0.0011) */
static void test_neighbor_correlation(void)
{
    const char *names[6] = {"x+1", "y+1", "x+1,y+1", "sample+1", "key+1", "key+2^32"};
    uint64_t count = 0;
    uint32_t x, y, c;
    uint8_t k;
    char message[96];
    double sum = 0.0, sum_sq = 0.0, cross[6] = {0.0}, mean, var, rho;
    float u, v[6];

    for (y = 0; y + 1 < GRID; y++) {
        for (x = 0; x + 1 < GRID; x++) {
            for (c = 0; c < 3; c++) {
                u = hdrg_hash_uniform(KEY, x, y, c) - 0.5f;
                v[0] = hdrg_hash_uniform(KEY, x + 1, y, c);
                v[1] = hdrg_hash_uniform(KEY, x, y + 1, c);
                v[2] = hdrg_hash_uniform(KEY, x + 1, y + 1, c);
                v[3] = hdrg_hash_uniform(KEY, x, y, c + 1);
                v[4] = hdrg_hash_uniform(KEY + 1, x, y, c);
                v[5] = hdrg_hash_uniform(KEY + (UINT64_C(1) << 32), x, y, c);
                for (k = 0; k < 6; k++) {
                    cross[k] += (double)u * (double)(v[k] - 0.5f);
                }

                sum += (double)u;
                sum_sq += (double)u * (double)u;
                count++;
            }
        }
    }

    mean = sum / (double)count;
    var = sum_sq / (double)count - mean * mean;
    for (k = 0; k < 6; k++) {
        rho = cross[k] / (double)count / var;
        (void)snprintf(message, sizeof(message), "correlation %-8s %+.5f", names[k], rho);
        TEST_MESSAGE(message);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, (float)rho);
    }
}

/*
 * 入力語 (key 下位/上位, y, x, sample) の各ビットを反転したとき、出力の各ビットが反転する確率が
 * 1/2 に近いこと (4096 試行で 1 sigma = 0.0078)
 */
static void test_avalanche(void)
{
    uint64_t state = 1;
    uint32_t base[5], flipped[5], flips[32], h, d, n;
    uint8_t word, bit, out, i;
    char message[96];
    double worst, p;

    for (word = 0; word < 5; word++) {
        worst = 0.0;
        for (bit = 0; bit < 32; bit++) {
            for (out = 0; out < 32; out++) {
                flips[out] = 0;
            }

            for (n = 0; n < 4096; n++) {
                for (i = 0; i < 5; i++) {
                    base[i] = (uint32_t)next_input(&state);
                    flipped[i] = base[i];
                }

                flipped[word] ^= UINT32_C(1) << bit;
                h = hash_words(base);
                d = h ^ hash_words(flipped);
                for (out = 0; out < 32; out++) {
                    flips[out] += (d >> out) & 1u;
                }
            }

            for (out = 0; out < 32; out++) {
                p = (double)flips[out] / 4096.0;
                worst = fmax(worst, fabs(p - 0.5));
            }
        }

        (void)snprintf(message, sizeof(message), "avalanche word %u: worst |p - 0.5| = %.4f",
                       (unsigned int)word, worst);
        TEST_MESSAGE(message);
        TEST_ASSERT_TRUE(worst < 0.05);
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
    RUN_TEST(test_known_answers);
    RUN_TEST(test_stages_match_full);
    RUN_TEST(test_stage_bijective);
    RUN_TEST(test_uniform_distribution);
    RUN_TEST(test_triangular_distribution);
    RUN_TEST(test_neighbor_correlation);
    RUN_TEST(test_avalanche);

    return UNITY_END();
}
