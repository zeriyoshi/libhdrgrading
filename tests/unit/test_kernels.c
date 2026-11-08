/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>
#include <unity.h>

#include "../test_util.h"
#include "kernels/kernels.h"
#include "hdrgrading/foundation/fmath.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"

/*
 * checkasm 型のカーネル比較: このビルドと CPU で動く階層を
 * scalar 参照と比べる。exact な階層 (FMA なし: scalar と同じ演算列) はバイト単位
 * で一致し、HDRG_SIMD_FMA の階層は L0x の許容内 (値は相対 TK_REL + 絶対 TK_ABS、
 * パックは 1 コード以内でずれるバイトが 0.1% 以下) に入る。入力はカウンタハッシュ
 * の乱数と境界値 (0、負、1 超、sRGB の折れ点の前後、PQ の下限、非正規数、
 * ショルダーとニーの分岐点の前後、LUT の格子点と対角線)、パックの幅の端数は
 * 1〜16 画素。
 */
#define TK_KEY    UINT64_C(0x544B5F4B45524E4C)
#define TK_BLOCKS 2048u
#define TK_CANARY 0xA5u
#define TK_TIERS  6u
#define TK_REL    1e-5f
#define TK_ABS    1e-6f

static const uint8_t tk_tiers[TK_TIERS] = {HDRG_SIMD_SSE41, HDRG_SIMD_AVX2, HDRG_SIMD_AVX512,
                                           HDRG_SIMD_NEON,  HDRG_SIMD_SVE,  HDRG_SIMD_WASM128};

/* the sRGB inputs: linear light around [0, 1] */
static const float tk_srgb_edges[] = {
    0.0f,           -0.0f,          -1.0f,  1.0f,      2.0f,           HDRG_FMATH_SRGB_KNEE,
    0x1.9a5c36p-9f, 0x1.9a5c3ap-9f, 1e-30f, 0x1p-140f, 0.5f,           -1e-20f,
    0.999999f,      0.18f,          1e-7f,  0x1p-126f, 0x1.fffffep-1f,
};

/* the PQ inputs: absolute nits */
static const float tk_pq_edges[] = {
    0.0f,
    -0.0f,
    -5.0f,
    1e-38f,
    0x1p-140f,
    0x1p-126f * 10000.0f,
    0x1p-125f * 10000.0f,
    1e-30f,
    0.005f,
    1.0f,
    100.0f,
    203.0f,
    1000.0f,
    9999.0f,
    10000.0f,
    10001.0f,
    1e6f,
};

/* a block of finite inputs: edge values first, then pseudo-random ones */
static void fill(bool pq, uint32_t block, float in[3][HDRG_KERNEL_BLOCK])
{
    const size_t edge_count =
        pq ? sizeof(tk_pq_edges) / sizeof(float) : sizeof(tk_srgb_edges) / sizeof(float);
    const float *edges = pq ? tk_pq_edges : tk_srgb_edges;
    uint32_t i, k;
    uint8_t c;
    float u;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
            k = (block * HDRG_KERNEL_BLOCK + i) * 3u + c;
            u = hdrg_hash_uniform(TK_KEY, i, block, c);

            if (k < edge_count) {
                in[c][i] = edges[k];
            } else if (pq) {
                /* geometric over 40 stops below 10000 nits, some above the peak */
                in[c][i] = 12000.0f * hdrg_fmath_exp2(-40.0f * u);
            } else {
                in[c][i] = -0.05f + 1.1f * u;
            }
        }
    }
}

/* exact tiers: the same bits; FMA tiers: each value within TK_REL of itself plus TK_ABS */
static void expect_floats(const char *what, const float *want, const float *got, size_t count,
                          bool exact)
{
    size_t i;
    char line[128];
    float diff, scale;

    if (exact) {
        TEST_ASSERT_EQUAL_MEMORY(want, got, count * sizeof(float));

        return;
    }

    for (i = 0; i < count; i++) {
        diff = fabsf(want[i] - got[i]);
        scale = fmaxf(fabsf(want[i]), fabsf(got[i]));

        if (!(diff <= TK_REL * scale + TK_ABS)) {
            (void)snprintf(line, sizeof(line), "%s, value %u: want %.9g, got %.9g", what,
                           (unsigned)i, (double)want[i], (double)got[i]);
            TEST_FAIL_MESSAGE(line);
        }
    }
}

/*
 * RGB: each channel relative to the pixel's largest channel (a desaturated
 * channel is a difference of the pixel's luminance and itself)
 */
static void expect_rgb(const char *what, const float want[3][HDRG_KERNEL_BLOCK],
                       const float got[3][HDRG_KERNEL_BLOCK], bool exact)
{
    uint32_t i;
    uint8_t c;
    char line[128];
    float scale;

    if (exact) {
        TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof(float) * 3u * HDRG_KERNEL_BLOCK);

        return;
    }

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        scale = 0.0f;
        for (c = 0; c < 3; c++) {
            scale = fmaxf(scale, fmaxf(fabsf(want[c][i]), fabsf(got[c][i])));
        }

        for (c = 0; c < 3; c++) {
            if (!(fabsf(want[c][i] - got[c][i]) <= TK_REL * scale + TK_ABS)) {
                (void)snprintf(line, sizeof(line), "%s, lane %u channel %u: want %.9g, got %.9g",
                               what, (unsigned)i, (unsigned)c, (double)want[c][i],
                               (double)got[c][i]);
                TEST_FAIL_MESSAGE(line);
            }
        }
    }
}

/* one kernel against scalar over every block, width remainder, position and dither */
static void compare(hdrg_pack_fn_t reference, hdrg_pack_fn_t candidate, bool pq, bool exact)
{
    static const uint32_t positions[4] = {0, 7, 4093, 65520};
    uint32_t block, n, p, row, i, bytes, off;
    uint8_t want[3 * HDRG_KERNEL_BLOCK + 8], got[3 * HDRG_KERNEL_BLOCK + 8], d;
    bool dither;
    float in[3][HDRG_KERNEL_BLOCK];

    bytes = 0;
    off = 0;

    for (block = 0; block < TK_BLOCKS; block++) {
        fill(pq, block, in);

        n = (block % HDRG_KERNEL_BLOCK) + 1u;
        p = positions[block % 4u];
        row = hdrg_hash_row(TK_KEY, block);

        for (d = 0; d < 2; d++) {
            dither = d == 1;

            memset(want, TK_CANARY, sizeof(want));
            memset(got, TK_CANARY, sizeof(got));
            reference((const float (*)[HDRG_KERNEL_BLOCK])in, n, p, row, dither, want);
            candidate((const float (*)[HDRG_KERNEL_BLOCK])in, n, p, row, dither, got);

            /* the n pixels match, and nothing past them is written */
            TEST_ASSERT_EQUAL_HEX8(TK_CANARY, got[n * 3u]);

            if (exact) {
                TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof(got));

                continue;
            }

            for (i = 0; i < sizeof(got); i++) {
                TEST_ASSERT_TRUE(abs((int32_t)want[i] - (int32_t)got[i]) <= 1);
                off += (want[i] != got[i]) ? 1u : 0u;
            }

            bytes += n * 3u;
        }
    }

    /* L0x: at most 0.1% of the codes off by one */
    TEST_ASSERT_TRUE(off * 1000u <= bytes);
}

/*
 * The 16-bit packs over every block and width remainder: exact, or within
 * 4 codes with FMA (PQ's second power, m2 ~ 79, magnifies a float rounding;
 * a 10-bit code is 64 of these)
 */
static void compare16(hdrg_pack16_fn_t reference, hdrg_pack16_fn_t candidate, bool pq, bool exact)
{
    uint32_t block, n, i;
    uint16_t want[3 * HDRG_KERNEL_BLOCK + 4], got[3 * HDRG_KERNEL_BLOCK + 4];
    float in[3][HDRG_KERNEL_BLOCK];

    for (block = 0; block < TK_BLOCKS; block++) {
        fill(pq, block, in);
        n = (block % HDRG_KERNEL_BLOCK) + 1u;

        for (i = 0; i < 3 * HDRG_KERNEL_BLOCK + 4; i++) {
            want[i] = 0xA5A5u;
            got[i] = 0xA5A5u;
        }

        reference((const float (*)[HDRG_KERNEL_BLOCK])in, n, want);
        candidate((const float (*)[HDRG_KERNEL_BLOCK])in, n, got);
        TEST_ASSERT_EQUAL_HEX16(0xA5A5u, got[n * 3u]);

        if (exact) {
            TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof(got));

            continue;
        }

        for (i = 0; i < n * 3u; i++) {
            TEST_ASSERT_TRUE(abs((int32_t)want[i] - (int32_t)got[i]) <= 4);
        }
    }
}

/* edges[lane] for the first lanes when given, else a pseudo-random value in [lo, hi) */
static float sample(uint32_t block, uint32_t lane, uint32_t channel, float lo, float hi,
                    const float *edges, size_t edge_count)
{
    if (lane < edge_count) {
        return edges[lane];
    }

    return lo + (hi - lo) * hdrg_hash_uniform(TK_KEY ^ 0x9u, lane, block, channel);
}

/* the grading ratio: luminance over 40 stops, gains, clarity bases, every argument mix */
static void compare_grade(hdrg_grade_fn_t reference, hdrg_grade_fn_t candidate, bool exact)
{
    static const float luma_edges[] = {0.0f,  -0.0f, 6.1035e-5f, 6.1036e-5f, 1e-30f, 0x1p-140f,
                                       0.18f, 1.0f,  64.0f,      1e9f,       -3.0f},
                       contrasts[4] = {1.0f, 1.05f, 0.5f, 2.0f};
    hdrg_grade_args_t args;
    uint32_t block, i;
    float luma[HDRG_KERNEL_BLOCK], gain[HDRG_KERNEL_BLOCK], base[HDRG_KERNEL_BLOCK],
        want[HDRG_KERNEL_BLOCK], got[HDRG_KERNEL_BLOCK];

    for (block = 0; block < TK_BLOCKS; block++) {
        for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
            /* the edge values in every 8th block, else geometric over 40 stops */
            luma[i] = sample(block, i, 0, 0.0f, 1.0f, luma_edges,
                             (block % 8u == 0) ? sizeof(luma_edges) / sizeof(float) : 0u);
            luma[i] = (block % 8u != 0 || i >= sizeof(luma_edges) / sizeof(float))
                          ? 16.0f * hdrg_fmath_exp2(-40.0f * luma[i])
                          : luma[i];
            gain[i] = sample(block, i, 1, -0.5f, 4.0f, NULL, 0);
            base[i] = sample(block, i, 2, -12.0f, 3.0f, NULL, 0);
        }

        args.pivot = -2.473931f;
        args.floor = 6.1035e-5f;
        args.contrast = contrasts[block % 4u];
        args.clarity = (block % 3u == 0) ? 0.0f : ((block % 3u == 1) ? 0.15f : 1.0f);
        args.clarity_on = (block % 5u) != 0;

        reference(&args, luma, gain, base, want);
        candidate(&args, luma, gain, base, got);
        expect_floats("grade", want, got, HDRG_KERNEL_BLOCK, exact);
    }
}

/*
 * rendition RGB around the shoulders and knees, every argument mix, and the
 * desaturation's guide factors (1 in every third block, the pixel's own)
 */
static void fill_rgb(uint32_t block, float hi, const float *edges, size_t edge_count,
                     float rgb[3][HDRG_KERNEL_BLOCK], float guide[HDRG_KERNEL_BLOCK])
{
    uint32_t i;
    uint8_t c;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
            rgb[c][i] = sample(block, i, c, -0.01f, hi, edges, edge_count);
        }
    }

    for (i = 0; guide != NULL && i < HDRG_KERNEL_BLOCK; i++) {
        guide[i] = (block % 3u == 0) ? 1.0f : sample(block, i, 3, 0.6f, 1.4f, NULL, 0);
    }
}

static void compare_layer(hdrg_layer_fn_t reference, hdrg_layer_fn_t candidate, bool exact)
{
    static const float peaks[4] = {1000.0f, 811.0f, 300.0f, 4000.0f},
                       knees[3] = {0.75f, 0.5f, 0.99f}, desats[3] = {0.0f, 0.4f, 1.0f};
    hdrg_layer_args_t args;
    uint32_t block;
    float edges[12], rgb[3][HDRG_KERNEL_BLOCK], guide[HDRG_KERNEL_BLOCK],
        want[3][HDRG_KERNEL_BLOCK], got[3][HDRG_KERNEL_BLOCK], want_lr[HDRG_KERNEL_BLOCK],
        got_lr[HDRG_KERNEL_BLOCK];

    for (block = 0; block < TK_BLOCKS; block++) {
        args.coeffs[0] = 0.2289746f;
        args.coeffs[1] = 0.6917385f;
        args.coeffs[2] = 0.0792869f;
        args.white = 203.0f;
        args.start = 0.75f * (peaks[block % 4u] / args.white);
        args.range = 0.25f * (peaks[block % 4u] / args.white);
        args.knee = knees[block % 3u];
        args.desat = desats[(block / 3u) % 3u];
        args.hybrid = (block % 2u) == 1;

        /* the branch points themselves */
        edges[0] = 0.0f;
        edges[1] = -0.0f;
        edges[2] = args.start;
        edges[3] = nextafterf(args.start, 0.0f);
        edges[4] = nextafterf(args.start, 100.0f);
        edges[5] = args.knee;
        edges[6] = nextafterf(args.knee, 2.0f);
        edges[7] = args.start + args.range;
        edges[8] = 1e-30f;
        edges[9] = 0x1p-140f;
        edges[10] = 1.0f;
        edges[11] = 50.0f;

        fill_rgb(block, 1.4f * (args.start + args.range), edges, (block % 7u == 0) ? 12u : 0u, rgb,
                 guide);

        reference(&args, (const float (*)[HDRG_KERNEL_BLOCK])rgb, guide, want, want_lr);
        candidate(&args, (const float (*)[HDRG_KERNEL_BLOCK])rgb, guide, got, got_lr);
        expect_rgb("layer", (const float (*)[HDRG_KERNEL_BLOCK])want,
                   (const float (*)[HDRG_KERNEL_BLOCK])got, exact);
        expect_floats("layer lr", want_lr, got_lr, HDRG_KERNEL_BLOCK, exact);
    }
}

static void compare_shoulder(hdrg_shoulder_fn_t reference, hdrg_shoulder_fn_t candidate, bool exact)
{
    static const float peaks[4] = {1000.0f, 400.0f, 4000.0f, 203.0f}, whites[2] = {203.0f, 100.0f},
                       desats[2] = {0.0f, 0.4f};
    hdrg_shoulder_args_t args;
    uint32_t block;
    float edges[8], rgb[3][HDRG_KERNEL_BLOCK], guide[HDRG_KERNEL_BLOCK], want[3][HDRG_KERNEL_BLOCK],
        got[3][HDRG_KERNEL_BLOCK];

    for (block = 0; block < TK_BLOCKS; block++) {
        args.coeffs[0] = 0.2289746f;
        args.coeffs[1] = 0.6917385f;
        args.coeffs[2] = 0.0792869f;
        args.white = whites[block % 2u];
        args.knee = peaks[(block / 2u) % 4u] * 0.75f;
        args.range = peaks[(block / 2u) % 4u] - args.knee;
        args.desat = desats[(block / 8u) % 2u];

        edges[0] = 0.0f;
        edges[1] = -0.0f;
        edges[2] = args.knee / args.white;
        edges[3] = nextafterf(args.knee / args.white, 0.0f);
        edges[4] = nextafterf(args.knee / args.white, 1e6f);
        edges[5] = 1e-30f;
        edges[6] = 1.0f;
        edges[7] = 1e4f;

        fill_rgb(block, 1.5f * (args.knee + args.range) / args.white, edges,
                 (block % 7u == 0) ? 8u : 0u, rgb, guide);

        reference(&args, (const float (*)[HDRG_KERNEL_BLOCK])rgb, guide, want);
        candidate(&args, (const float (*)[HDRG_KERNEL_BLOCK])rgb, guide, got);
        expect_rgb("shoulder", (const float (*)[HDRG_KERNEL_BLOCK])want,
                   (const float (*)[HDRG_KERNEL_BLOCK])got, exact);
    }
}

/*
 * the plan pass's highlight boost: La over 14 stops around the ramp's ends,
 * spans down to none (lo at or above hi), boosts of 0 to 1
 */
static void compare_boost(hdrg_boost_fn_t reference, hdrg_boost_fn_t candidate, bool exact)
{
    hdrg_boost_args_t args;
    uint32_t block, i;
    float luma[HDRG_KERNEL_BLOCK], boost[HDRG_KERNEL_BLOCK], lo[HDRG_KERNEL_BLOCK],
        hi[HDRG_KERNEL_BLOCK], want[HDRG_KERNEL_BLOCK], got[HDRG_KERNEL_BLOCK],
        want_log[HDRG_KERNEL_BLOCK], got_log[HDRG_KERNEL_BLOCK];

    for (block = 0; block < TK_BLOCKS; block++) {
        args.floor = 6.1035e-5f;

        for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
            lo[i] = sample(block, i, 0, -12.0f, 0.0f, NULL, 0);
            hi[i] = (block % 5u == 0) ? lo[i] + 0.01f * (float)(i % 3u)
                                      : lo[i] + sample(block, i, 1, 0.0f, 4.0f, NULL, 0);
            boost[i] = (block % 6u == 0) ? 0.0f : sample(block, i, 2, 0.0f, 1.0f, NULL, 0);
            luma[i] = exp2f(sample(block, i, 3, -14.0f, 0.4f, NULL, 0));
        }

        /* the ramp's ends and their neighbors, and the floor */
        if (block % 7u == 0) {
            luma[0] = exp2f(lo[0]);
            luma[1] = nextafterf(exp2f(lo[1]), 0.0f);
            luma[2] = nextafterf(exp2f(lo[2]), 2.0f);
            luma[3] = exp2f(hi[3]);
            luma[4] = nextafterf(exp2f(hi[4]), 0.0f);
            luma[5] = nextafterf(exp2f(hi[5]), 2.0f);
            luma[6] = 0x1p-140f;
            luma[7] = 0.0f;
        }

        reference(&args, luma, boost, lo, hi, want, want_log);
        candidate(&args, luma, boost, lo, hi, got, got_log);
        expect_floats("boost", want, got, HDRG_KERNEL_BLOCK, exact);
        expect_floats("boost log", want_log, got_log, HDRG_KERNEL_BLOCK, exact);
    }
}

/*
 * A synthetic LUT over 8- and 16-bit codes: smooth nonlinear nodes (a
 * channel mix with a saturation-like bend) and a convex EOTF table
 */
static void make_lut(uint32_t max_code, float *nodes, float *eotf, hdrg_lut_t *lut)
{
    const uint32_t n = HDRG_LUT_SIZE;
    uint32_t r, g, b, i;
    uint8_t c;
    float v[3], mean, t;

    for (r = 0; r < n; r++) {
        for (g = 0; g < n; g++) {
            for (b = 0; b < n; b++) {
                v[0] = (float)r / (float)(n - 1u);
                v[1] = (float)g / (float)(n - 1u);
                v[2] = (float)b / (float)(n - 1u);
                mean = (v[0] + v[1] + v[2]) / 3.0f;

                for (c = 0; c < 3; c++) {
                    t = mean + 1.2f * (v[c] - mean) + 0.05f * v[(c + 1u) % 3u] * v[c];
                    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
                    nodes[((r * n + g) * n + b) * 3u + c] = t * (float)max_code;
                }
            }
        }
    }

    for (c = 0; c < 3; c++) {
        for (i = 0; i <= max_code; i++) {
            t = (float)i / (float)max_code;
            eotf[(size_t)c * (max_code + 1u) + i] = t * t * (0.8f + 0.2f * t) * (1.0f + 0.1f * c);
        }
    }

    lut->nodes = nodes;
    lut->eotf = eotf;
    lut->entries = max_code + 1u;
    lut->max_code = (float)max_code;
}

/* the LUT kernel: random codes, the lattice points, the diagonals (tetrahedron ties), the ends */
static void compare_lut(hdrg_lut_fn_t reference, hdrg_lut_fn_t candidate, bool exact)
{
    static const uint32_t depths[2] = {255u, 65535u};
    hdrg_lut_t lut;
    uint32_t d, block, i, max_code, codes[3][HDRG_KERNEL_BLOCK], lattice;
    uint8_t c;
    float *nodes, *eotf, ratio[HDRG_KERNEL_BLOCK], want[3][HDRG_KERNEL_BLOCK],
        got[3][HDRG_KERNEL_BLOCK];

    nodes = (float *)malloc(sizeof(float) * 3u * HDRG_LUT_SIZE * HDRG_LUT_SIZE * HDRG_LUT_SIZE);
    eotf = (float *)malloc(sizeof(float) * 3u * 65536u);
    TEST_ASSERT_NOT_NULL(nodes);
    TEST_ASSERT_NOT_NULL(eotf);

    for (d = 0; d < 2; d++) {
        max_code = depths[d];
        make_lut(max_code, nodes, eotf, &lut);

        for (block = 0; block < TK_BLOCKS; block++) {
            for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
                for (c = 0; c < 3; c++) {
                    codes[c][i] = (uint32_t)(hdrg_hash_uniform(TK_KEY ^ 0x5u, i, block, c) *
                                             (float)(max_code + 1u));
                    codes[c][i] = (codes[c][i] > max_code) ? max_code : codes[c][i];
                }

                /* ties: two or three channels equal; codes on the lattice's planes, rounded */
                if (block % 4u == 1) {
                    codes[1][i] = codes[0][i];
                } else if (block % 4u == 2) {
                    codes[1][i] = codes[0][i];
                    codes[2][i] = codes[0][i];
                } else if (block % 4u == 3) {
                    for (c = 0; c < 3; c++) {
                        lattice = (uint32_t)(hdrg_hash_uniform(TK_KEY ^ 0x7u, i, block, c) *
                                             (float)HDRG_LUT_SIZE);
                        lattice = (lattice >= HDRG_LUT_SIZE) ? HDRG_LUT_SIZE - 1u : lattice;
                        codes[c][i] = lattice * max_code / (HDRG_LUT_SIZE - 1u);
                    }
                }

                ratio[i] = sample(block, i, 0, 0.5f, 8.0f, NULL, 0);
            }

            if (block == 0) {
                for (c = 0; c < 3; c++) {
                    codes[c][0] = 0;
                    codes[c][1] = max_code;
                    codes[c][2] = (c == 0) ? max_code : 0;
                    codes[c][3] = (c == 2) ? 0 : max_code;
                }
            }

            reference(&lut, (const uint32_t (*)[HDRG_KERNEL_BLOCK])codes, ratio, want);
            candidate(&lut, (const uint32_t (*)[HDRG_KERNEL_BLOCK])codes, ratio, got);
            expect_rgb("lut", (const float (*)[HDRG_KERNEL_BLOCK])want,
                       (const float (*)[HDRG_KERNEL_BLOCK])got, exact);
        }
    }

    free(nodes);
    free(eotf);
}

static void test_tiers_match_scalar(void)
{
    const hdrg_kernels_t *kernels;
    uint8_t t, ran;
    char line[64];

    ran = 0;
    for (t = 0; t < TK_TIERS; t++) {
        kernels = hdrg_kernels_get(tk_tiers[t]);
        if (kernels == NULL) {
            continue;
        }

        TEST_ASSERT_EQUAL_UINT8(tk_tiers[t], kernels->tier);

        /* SSE has no fused multiply-add */
        if (kernels->tier == HDRG_SIMD_SSE41) {
            TEST_ASSERT_TRUE(kernels->exact);
        }

        compare(hdrg_kernels_scalar.pack_srgb, kernels->pack_srgb, false, kernels->exact);
        compare(hdrg_kernels_scalar.pack_pq, kernels->pack_pq, true, kernels->exact);
        compare16(hdrg_kernels_scalar.pack_srgb16, kernels->pack_srgb16, false, kernels->exact);
        compare16(hdrg_kernels_scalar.pack_pq16, kernels->pack_pq16, true, kernels->exact);
        compare_boost(hdrg_kernels_scalar.boost, kernels->boost, kernels->exact);
        compare_grade(hdrg_kernels_scalar.grade, kernels->grade, kernels->exact);
        compare_lut(hdrg_kernels_scalar.lut, kernels->lut, kernels->exact);
        compare_layer(hdrg_kernels_scalar.layer, kernels->layer, kernels->exact);
        compare_shoulder(hdrg_kernels_scalar.shoulder, kernels->shoulder, kernels->exact);

        (void)snprintf(line, sizeof(line), "tier %s matches scalar (%s)",
                       hdrg_simd_name(kernels->tier), kernels->exact ? "exact" : "L0x");
        TEST_MESSAGE(line);
        ran++;
    }

    if (ran == 0) {
        TEST_MESSAGE("no SIMD tier in this build / CPU: scalar only");
    }
}

/* the scalar kernel is the fmath reference with the triangular dither of the hash */
static void test_scalar_reference(void)
{
    uint32_t i, row, pixel;
    uint8_t c, out[3 * HDRG_KERNEL_BLOCK];
    float in[3][HDRG_KERNEL_BLOCK], code;

    fill(false, 5, in);
    row = hdrg_hash_row(TK_KEY, 9);
    hdrg_kernels_scalar.pack_srgb((const float (*)[HDRG_KERNEL_BLOCK])in, HDRG_KERNEL_BLOCK, 100,
                                  row, true, out);

    for (i = 0; i < HDRG_KERNEL_BLOCK; i++) {
        pixel = hdrg_hash_pixel(row, 100 + i);

        for (c = 0; c < 3; c++) {
            code = hdrg_fmath_srgb_oetf(in[c][i]) * 255.0f +
                   hdrg_hash_tri(hdrg_hash_sample(pixel, c * 2u),
                                 hdrg_hash_sample(pixel, c * 2u + 1u));
            code = (code < 0.0f) ? 0.0f : ((code > 255.0f) ? 255.0f : code);
            HDRG_TEST_NEAR(0.5, code, out[i * 3u + c]);
        }
    }
}

/*
 * The PQ table regenerates from hdrg_math_pq_oetf bit for bit (each grid
 * point's nits are exact in float), and the interpolated signal stays within
 * 0.3 of a 16-bit code of it from 10^-10 nits to past the peak
 */
static void test_pq_table(void)
{
    uint32_t i, table_bits, reference_bits;
    double worst;
    float y, reference, nits;

    for (i = 0; i < HDRG_PQ_TABLE_ENTRIES; i++) {
        y = ldexpf(1.0f + (float)(i % 64u) / 64.0f, (int32_t)(i / 64u) - 45);
        y = (y > 1.0f) ? 1.0f : y;
        reference = hdrg_math_pq_oetf(y * 10000.0f);
        memcpy(&table_bits, &hdrg_pq_table[i], sizeof(table_bits));
        memcpy(&reference_bits, &reference, sizeof(reference_bits));
        TEST_ASSERT_EQUAL_HEX32(reference_bits, table_bits);
    }

    worst = 0.0;
    for (nits = 1e-10f; nits < 12000.0f; nits *= 1.0001f) {
        worst = fmax(worst, fabs((double)hdrg_pq_signal(nits) - (double)hdrg_math_pq_oetf(nits)) *
                                65535.0);
    }

    printf("PQ table: within %.3f of a 16-bit code\n", worst);

    TEST_ASSERT_TRUE(worst <= 0.3);
    TEST_ASSERT_TRUE(hdrg_pq_signal(0.0f) * 65535.0f < 0.5f);
    TEST_ASSERT_TRUE(hdrg_pq_signal(NAN) * 65535.0f < 0.5f);
    TEST_ASSERT_TRUE(hdrg_pq_signal(10000.0f) == 1.0f && hdrg_pq_signal(1e9f) == 1.0f);
}

static void test_selection(void)
{
    const hdrg_kernels_t *best;
    uint8_t t;

    TEST_ASSERT_EQUAL_PTR(&hdrg_kernels_scalar, hdrg_kernels_select(HDRG_SIMD_SCALAR));
    TEST_ASSERT_EQUAL_PTR(&hdrg_kernels_scalar, hdrg_kernels_get(HDRG_SIMD_SCALAR));
    TEST_ASSERT_NULL(hdrg_kernels_get(0));
    TEST_ASSERT_NULL(hdrg_kernels_get(200));

    TEST_ASSERT_TRUE(hdrg_kernels_scalar.exact);

    /* no cap: the best tier of this architecture the CPU runs, never a provisional one */
    best = hdrg_kernels_select(0);
    TEST_ASSERT_NOT_NULL(best);
    TEST_ASSERT_TRUE(best->tier != HDRG_SIMD_AVX512 && best->tier != HDRG_SIMD_SVE);

    for (t = 0; t < TK_TIERS; t++) {
        if (hdrg_kernels_get(tk_tiers[t]) != NULL) {
            TEST_ASSERT_TRUE(best->tier != HDRG_SIMD_SCALAR);
        }
    }

    /* a cap selects at most that tier; another architecture's tier caps to scalar */
    for (t = 0; t < TK_TIERS; t++) {
        best = hdrg_kernels_select(tk_tiers[t]);

        if (hdrg_kernels_get(tk_tiers[t]) != NULL) {
            TEST_ASSERT_EQUAL_UINT8(tk_tiers[t], best->tier);
        }
    }

#if defined(__aarch64__) || defined(_M_ARM64)
    TEST_ASSERT_EQUAL_PTR(&hdrg_kernels_scalar, hdrg_kernels_select(HDRG_SIMD_AVX2));
#elif defined(__x86_64__) || defined(_M_X64)
    TEST_ASSERT_EQUAL_PTR(&hdrg_kernels_scalar, hdrg_kernels_select(HDRG_SIMD_NEON));

    if (hdrg_kernels_get(HDRG_SIMD_AVX512) == NULL && hdrg_kernels_get(HDRG_SIMD_AVX2) != NULL) {
        TEST_ASSERT_EQUAL_UINT8(HDRG_SIMD_AVX2, hdrg_kernels_select(HDRG_SIMD_AVX512)->tier);
    }
#endif
}

static void test_names(void)
{
    uint8_t t;

    for (t = HDRG_SIMD_SCALAR; t <= HDRG_SIMD_WASM128; t++) {
        TEST_ASSERT_NOT_NULL(hdrg_simd_name(t));
        TEST_ASSERT_EQUAL_UINT8(t, hdrg_kernels_parse(hdrg_simd_name(t)));
    }

    TEST_ASSERT_NULL(hdrg_simd_name(0));
    TEST_ASSERT_EQUAL_UINT8(0, hdrg_kernels_parse(NULL));
    TEST_ASSERT_EQUAL_UINT8(0, hdrg_kernels_parse("AVX2"));
    TEST_ASSERT_EQUAL_UINT8(0, hdrg_kernels_parse(""));
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
    RUN_TEST(test_tiers_match_scalar);
    RUN_TEST(test_scalar_reference);
    RUN_TEST(test_pq_table);
    RUN_TEST(test_selection);
    RUN_TEST(test_names);

    return UNITY_END();
}
