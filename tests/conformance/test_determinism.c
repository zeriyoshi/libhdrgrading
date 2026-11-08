/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "container.h"
#include "io/io.h"
#include "pipeline.h"
#include "priv.h"

/*
 * L0: 同一ビルドなら、スレッド数・実行回数・呼び出し元の FTZ / DAZ
 * の状態によらず出力がバイト一致する。全出力形式 (ハイブリッド / ゲインマップ /
 * 単層 PQ) で確かめる。入力の並べ方 (アルファ・行のパディング) にもよらない。
 * exact な SIMD の階層 (FMA なし) は scalar とバイト一致し、HDRG_SIMD_FMA の
 * 階層は L0x (エンコード前の符号値が 1 以内で 99.9% 以上が一致、ゲインマップの
 * セルの log 比の和が相対 1e-5 + 絶対 1e-4 以内) に入る。
 */
#if defined(__x86_64__) || defined(_M_X64) || (defined(__i386__) && defined(__SSE__)) ||           \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define TEST_HAS_FLUSH 1
#define TEST_FLUSH_X86 1
#elif defined(__aarch64__)
#define TEST_HAS_FLUSH 1
#endif

/* every output form; without lcms2 only the gain map (HYBRID / PQ need the P3 PQ profile) */
#ifdef HDRG_WITH_LCMS2
#define TEST_MODES 3u
#else
#define TEST_MODES 1u
#endif

static uint8_t *g_pixels, *g_baseline[3];
static size_t g_baseline_size[3];

#if defined(TEST_FLUSH_X86)
/* MXCSR の FTZ (bit 15) と DAZ (bit 6) を立てる。戻り値は元の値 */
static uint64_t set_flush(void)
{
    const uint64_t saved = (uint64_t)_mm_getcsr();

    _mm_setcsr((unsigned int)(saved | UINT64_C(0x8040)));

    return saved;
}

static void restore_flush(uint64_t saved)
{
    _mm_setcsr((unsigned int)saved);
}
#elif defined(TEST_HAS_FLUSH)
/* FPCR の FZ (bit 24) を立てる。戻り値は元の値 */
static uint64_t set_flush(void)
{
    uint64_t saved;

    __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved | (UINT64_C(1) << 24)) : "memory");

    return saved;
}

static void restore_flush(uint64_t saved)
{
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved) : "memory");
}
#endif

/* kernels NULL = the context's own tier */
static void convert_on(const hdrg_kernels_t *kernels, uint8_t threads, uint8_t mode,
                       uint8_t **output, size_t *output_size)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = mode;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 320;
    raw.height = 200;
    raw.bit_depth = 8;
    raw.stride = 320 * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));

    if (kernels != NULL) {
        ctx->kernels = kernels;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, output, output_size));

    hdrg_context_destroy(ctx);
}

/* the frame's 8-bit rows before encoding (320 x 200) and its gain-map sums (NULL for PQ) */
static void render_on(const hdrg_kernels_t *kernels, uint8_t mode, uint8_t *rows, float **gm,
                      size_t *gm_count)
{
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_container_t container;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_pipeline_t pipeline;
    hdrg_context_t *ctx;
    uint32_t y0, gm_h;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 4));

    ctx->kernels = kernels;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = mode;
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_container_select(ctx, &encode, &container));
    hdrg_container_caps(&container, NULL, &encode, &caps);

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.width = 320;
    raw.height = 200;
    raw.bit_depth = 8;
    raw.stride = 320 * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_open(ctx, &raw, &color, &effective, &encode, &caps,
                                                      0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_frame(ctx, &pipeline, raw.pixels));

    for (y0 = 0; y0 < 200; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_rows(ctx, &pipeline, y0,
                                                          HDRG_MIN(200u - y0, HDRG_APPLY_BAND_ROWS),
                                                          rows + (size_t)y0 * 320 * 3));
    }

    hdrg_pipeline_end(ctx, &pipeline);

    *gm = NULL;
    *gm_count = 0;

    if (pipeline.apply.gm_sum != NULL) {
        gm_h = (200u + (1u << pipeline.apply.gm_shift) - 1u) >> pipeline.apply.gm_shift;
        *gm_count = (size_t)pipeline.apply.gm_w * gm_h;
        *gm = (float *)malloc(*gm_count * sizeof(float));
        TEST_ASSERT_NOT_NULL(*gm);

        memcpy(*gm, pipeline.apply.gm_sum, *gm_count * sizeof(float));
    }

    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);
    hdrg_context_destroy(ctx);
}

static void convert_with(uint8_t threads, uint8_t mode, uint8_t **output, size_t *output_size)
{
    convert_on(NULL, threads, mode, output, output_size);
}

static void ensure_baseline(void)
{
    uint8_t mode;

    if (g_pixels != NULL) {
        return;
    }

    g_pixels = hdrg_test_image(320, 200);
    TEST_ASSERT_NOT_NULL(g_pixels);

    for (mode = 0; mode < TEST_MODES; mode++) {
        convert_with(1, mode, &g_baseline[mode], &g_baseline_size[mode]);
    }
}

static void assert_matches_baseline(uint8_t threads)
{
    uint8_t *output, mode;
    size_t output_size;

    ensure_baseline();

    for (mode = 0; mode < TEST_MODES; mode++) {
        convert_with(threads, mode, &output, &output_size);

        TEST_ASSERT_EQUAL_size_t(g_baseline_size[mode], output_size);
        TEST_ASSERT_EQUAL_MEMORY(g_baseline[mode], output, g_baseline_size[mode]);

        hdrg_free(output);
    }
}

static void test_thread_counts_identical(void)
{
    assert_matches_baseline(2);
    assert_matches_baseline(4);
    assert_matches_baseline(8);
    assert_matches_baseline(16);
}

static void test_auto_threads_identical(void)
{
    assert_matches_baseline(0);
}

/* exact tiers: the scalar bytes; FMA tiers: L0x against scalar before encoding */
static void expect_l0x(const hdrg_kernels_t *kernels, uint8_t mode)
{
    const size_t count = (size_t)320 * 200 * 3;
    uint8_t *want, *got;
    size_t want_gm_count, got_gm_count, i, off;
    float *want_gm, *got_gm;

    want = (uint8_t *)malloc(count);
    got = (uint8_t *)malloc(count);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_NOT_NULL(got);

    render_on(&hdrg_kernels_scalar, mode, want, &want_gm, &want_gm_count);
    render_on(kernels, mode, got, &got_gm, &got_gm_count);

    off = 0;
    for (i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(abs((int32_t)want[i] - (int32_t)got[i]) <= 1);
        off += (want[i] != got[i]) ? 1u : 0u;
    }

    TEST_ASSERT_TRUE(off * 1000u <= count);
    TEST_ASSERT_EQUAL_size_t(want_gm_count, got_gm_count);

    for (i = 0; i < want_gm_count; i++) {
        TEST_ASSERT_TRUE(fabsf(want_gm[i] - got_gm[i]) <=
                         1e-5f * fmaxf(fabsf(want_gm[i]), fabsf(got_gm[i])) + 1e-4f);
    }

    free(want);
    free(got);
    free(want_gm);
    free(got_gm);
}

/* このビルドと CPU で動くすべての SIMD の階層 (scalar を含む) を scalar と比べる */
static void test_simd_tiers_match(void)
{
    const hdrg_kernels_t *kernels;
    uint8_t *reference[3], *output, tier, mode;
    size_t reference_size[3], output_size;

    ensure_baseline();

    for (mode = 0; mode < TEST_MODES; mode++) {
        convert_on(&hdrg_kernels_scalar, 4, mode, &reference[mode], &reference_size[mode]);
    }

    for (tier = HDRG_SIMD_SCALAR; tier <= HDRG_SIMD_WASM128; tier++) {
        kernels = hdrg_kernels_get(tier);
        if (kernels == NULL) {
            continue;
        }

        for (mode = 0; mode < TEST_MODES; mode++) {
            if (!kernels->exact) {
                expect_l0x(kernels, mode);

                continue;
            }

            convert_on(kernels, 4, mode, &output, &output_size);

            TEST_ASSERT_EQUAL_size_t(reference_size[mode], output_size);
            TEST_ASSERT_EQUAL_MEMORY(reference[mode], output, reference_size[mode]);

            hdrg_free(output);
        }
    }

    for (mode = 0; mode < TEST_MODES; mode++) {
        hdrg_free(reference[mode]);
    }
}

static void test_rerun_identical(void)
{
    assert_matches_baseline(1);
}

/* アルファ付き・行パディング付きの同じ画素は、RGB の詰めた入力とバイト一致する */
static void test_rgba_padded_layout_identical(void)
{
    hdrg_params_t params;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    uint32_t x, y;
    uint8_t *rgba, *output, c;
    size_t stride, output_size;

    ensure_baseline();

    stride = (size_t)320 * 4 + 12;
    rgba = (uint8_t *)malloc(stride * 200);
    TEST_ASSERT_NOT_NULL(rgba);

    memset(rgba, 0xA5, stride * 200);

    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            for (c = 0; c < 3; c++) {
                rgba[(size_t)y * stride + (size_t)x * 4 + c] =
                    g_pixels[((size_t)y * 320 + x) * 3 + c];
            }

            rgba[(size_t)y * stride + (size_t)x * 4 + 3] = (uint8_t)(x * 7u + y);
        }
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    memset(&raw, 0, sizeof(raw));
    raw.pixels = rgba;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 320;
    raw.height = 200;
    raw.bit_depth = 8;
    raw.stride = stride;
    raw.has_alpha = true;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, NULL, NULL, &output, &output_size));

    TEST_ASSERT_EQUAL_size_t(g_baseline_size[0], output_size);
    TEST_ASSERT_EQUAL_MEMORY(g_baseline[0], output, g_baseline_size[0]);

    hdrg_free(output);
    hdrg_context_destroy(ctx);
    free(rgba);
}

/* 16bit 入力 (コード値 v * 257) は 8bit 入力とほぼ同じ SDR ベースになる */
static void test_16bit_input_matches_8bit(void)
{
    hdrg_allocator_t alloc;
    hdrg_params_t params;
    hdrg_raw_image_t raw;
    hdrg_decoded_t wide, narrow;
    hdrg_context_t *ctx;
    uint16_t *pixels16;
    uint8_t *output;
    size_t i, total, output_size;
    double diff;

    ensure_baseline();

    alloc = hdrg_test_allocator();
    total = (size_t)320 * 200 * 3;
    pixels16 = (uint16_t *)malloc(total * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(pixels16);

    for (i = 0; i < total; i++) {
        pixels16[i] = (uint16_t)(g_pixels[i] * 257u);
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)pixels16;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 320;
    raw.height = 200;
    raw.bit_depth = 16;
    raw.stride = (size_t)320 * 3 * sizeof(uint16_t);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, NULL, NULL, &output, &output_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, output, output_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &wide));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, g_baseline[0], g_baseline_size[0],
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &narrow));
    TEST_ASSERT_EQUAL_UINT32(narrow.width, wide.width);
    TEST_ASSERT_EQUAL_UINT32(narrow.height, wide.height);

    diff = 0.0;
    for (i = 0; i < total; i++) {
        diff += fabs((double)wide.pixels[i] - (double)narrow.pixels[i]);
    }

    TEST_ASSERT_TRUE(diff / (double)total < 1.0);

    hdrg_decoded_free(&alloc, &wide);
    hdrg_decoded_free(&alloc, &narrow);
    hdrg_free(output);
    hdrg_context_destroy(ctx);
    free(pixels16);
}

/* 呼び出し元が FTZ / DAZ を立てていても、ライブラリは自分の FP 環境で計算する */
static void test_caller_flush_to_zero_identical(void)
{
#if defined(TEST_HAS_FLUSH)
    uint64_t saved;

    ensure_baseline();

    saved = set_flush();

    assert_matches_baseline(1);
    assert_matches_baseline(4);

    restore_flush(saved);
#else
    TEST_IGNORE_MESSAGE("no FTZ / DAZ control on this target");
#endif
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    uint8_t mode;
    int failures;

    UNITY_BEGIN();
    RUN_TEST(test_thread_counts_identical);
    RUN_TEST(test_auto_threads_identical);
    RUN_TEST(test_simd_tiers_match);
    RUN_TEST(test_rerun_identical);
    RUN_TEST(test_caller_flush_to_zero_identical);
    RUN_TEST(test_rgba_padded_layout_identical);
    RUN_TEST(test_16bit_input_matches_8bit);
    failures = UNITY_END();

    for (mode = 0; mode < TEST_MODES; mode++) {
        hdrg_free(g_baseline[mode]);
    }

    free(g_pixels);

    return failures;
}
