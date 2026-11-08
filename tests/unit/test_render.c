/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "core/analyze.h"
#include "core/apply.h"
#include "core/plan.h"
#include "core/render.h"

/*
 * render (表示依存): 1 つのプランを表示能力だけ変えて描画し直せる。解析・プランは
 * 作り直さない。ヘッドルーム・SDR 白・全白上限・色域が結果に効く。
 */
#define TR_W 128u
#define TR_H 96u

static hdrg_allocator_t g_alloc;
static hdrg_color_t g_color;
static hdrg_params_t g_params;
static hdrg_encode_params_t g_encode;
static hdrg_source_t g_src;
static hdrg_plan_t g_plan;
static uint8_t *g_pixels;

static void ensure_plan(void)
{
    hdrg_raw_image_t raw;
    hdrg_analysis_t analysis;

    if (g_pixels != NULL) {
        return;
    }

    g_alloc = hdrg_test_allocator();
    g_pixels = hdrg_test_image(TR_W, TR_H);
    TEST_ASSERT_NOT_NULL(g_pixels);

    hdrg_params_init(&g_params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&g_encode);
    g_encode.output_mode = HDRG_OUTPUT_PQ;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TR_W;
    raw.height = TR_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TR_W * 3;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &g_color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &g_color, &g_src));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_analyze(&g_alloc, NULL, &g_src, NULL, &analysis));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_plan_build(&g_alloc, NULL, NULL, &g_src, &analysis, 0, &g_params, &g_plan));

    hdrg_analysis_free(&g_alloc, &analysis);
}

/* 既定の想定視聴プロファイル (単層 PQ: ヘッドルームの制限なし、P3) */
static void file_caps(hdrg_display_caps_t *caps)
{
    hdrg_display_caps_for_file(NULL, &g_encode, true, caps);
}

/* 単層 PQ で全画面を描画する。rows は TR_W * TR_H * 3 バイト */
static void render_rows(const hdrg_display_caps_t *caps, hdrg_render_t *render, uint8_t *rows)
{
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    hdrg_apply_t apply;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, caps,
                                                     &g_color, render));

    hdrg_gamut_map_init(&g_params, render->gamut, &map);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_color_lut_build(&g_alloc, NULL, &g_color, g_src.lut, 255, &map, &lut));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_apply_setup(&g_alloc, NULL, &g_src, false, &lut, NULL,
                                                    &g_params, &g_encode, &apply));
    hdrg_apply_bind(&apply, &g_plan, render, &g_params);
    hdrg_apply_rows(NULL, &apply, 0, TR_H, rows);
    hdrg_apply_free(&g_alloc, &apply);
    hdrg_color_lut_free(&g_alloc, &lut);
}

static double mean_code(const uint8_t *rows)
{
    size_t i;
    double sum;

    sum = 0.0;
    for (i = 0; i < (size_t)TR_W * TR_H * 3; i++) {
        sum += (double)rows[i];
    }

    return sum / (double)((size_t)TR_W * TR_H * 3);
}

static void test_render_only_rerender(void)
{
    hdrg_display_caps_t caps;
    hdrg_render_t wide, narrow, again;
    uint8_t *rows_wide, *rows_narrow, *rows_again;
    size_t size;

    ensure_plan();

    size = (size_t)TR_W * TR_H * 3;
    rows_wide = (uint8_t *)malloc(size);
    rows_narrow = (uint8_t *)malloc(size);
    rows_again = (uint8_t *)malloc(size);
    TEST_ASSERT_NOT_NULL(rows_wide);
    TEST_ASSERT_NOT_NULL(rows_narrow);
    TEST_ASSERT_NOT_NULL(rows_again);

    file_caps(&caps);
    render_rows(&caps, &wide, rows_wide);

    /* 1 stop しか出せない表示: ピークが 2 倍の SDR 白で頭打ちになり、明部が下がる */
    caps.headroom_stops = 1.0f;
    render_rows(&caps, &narrow, rows_narrow);

    TEST_ASSERT_TRUE(wide.peak_nits > narrow.peak_nits);
    TEST_ASSERT_TRUE(narrow.peak_nits <= 2.0f * HDRG_SDR_WHITE_NITS + 0.01f);
    TEST_ASSERT_TRUE(memcmp(rows_wide, rows_narrow, size) != 0);
    TEST_ASSERT_TRUE(mean_code(rows_narrow) < mean_code(rows_wide));

    /* プランは描画で変わらない: 同じ表示能力なら同じ画素に戻る */
    file_caps(&caps);
    render_rows(&caps, &again, rows_again);
    TEST_ASSERT_EQUAL_MEMORY(rows_wide, rows_again, size);

    free(rows_wide);
    free(rows_narrow);
    free(rows_again);
}

static void test_sdr_white_moves_pq_codes(void)
{
    hdrg_display_caps_t caps;
    hdrg_render_t ref, dim;
    uint8_t *rows_ref, *rows_dim;
    size_t size;

    ensure_plan();

    size = (size_t)TR_W * TR_H * 3;
    rows_ref = (uint8_t *)malloc(size);
    rows_dim = (uint8_t *)malloc(size);
    TEST_ASSERT_NOT_NULL(rows_ref);
    TEST_ASSERT_NOT_NULL(rows_dim);

    file_caps(&caps);
    render_rows(&caps, &ref, rows_ref);

    /* SDR 白を 100 nits に置く表示: PQ の符号値が全体に下がる */
    caps.sdr_white_nits = 100.0f;
    render_rows(&caps, &dim, rows_dim);

    TEST_ASSERT_EQUAL_FLOAT(100.0f, dim.sdr_white_nits);
    TEST_ASSERT_TRUE(mean_code(rows_dim) + 5.0 < mean_code(rows_ref));

    free(rows_ref);
    free(rows_dim);
}

static void test_full_frame_limit_fits_apl(void)
{
    hdrg_display_caps_t caps;
    hdrg_render_t free_apl, limited;
    float budget;

    ensure_plan();

    g_params.apl_budget_nits = 0.0f;
    g_params.apl_peak_fraction = 0.0f;

    file_caps(&caps);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &free_apl));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, free_apl.apl_scale);
    TEST_ASSERT_TRUE(free_apl.apl_nits > free_apl.sdr_apl_nits);

    /* ABL: 全白の上限が SDR と描画の中間なら、ゲインを共通に縮めて収める */
    budget = 0.5f * (free_apl.sdr_apl_nits + free_apl.apl_nits);
    caps.max_full_frame_nits = budget;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &limited));
    TEST_ASSERT_TRUE(limited.apl_scale < 1.0f);
    TEST_ASSERT_TRUE(limited.apl_nits <= budget * 1.001f);
    TEST_ASSERT_TRUE(limited.max_gain_stops < free_apl.max_gain_stops);

    hdrg_params_init(&g_params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&g_encode);
    g_encode.output_mode = HDRG_OUTPUT_PQ;
}

/*
 * 描画の色域: 表示が P3 を運べて、HDR 主体の出力・色域拡張あり・ソースが sRGB を
 * 超える、のいずれかなら P3。それ以外は sRGB (拡張しないなら P3 にする意味がない)
 */
static void test_rendition_gamut(void)
{
    hdrg_display_caps_t caps;
    hdrg_render_t render;
    hdrg_color_t adobe;

    ensure_plan();
    TEST_ASSERT_TRUE(
        hdrg_color_from_codes(HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB, &adobe));

    file_caps(&caps);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &render));
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_P3, render.gamut);

    /* sRGB しか運べない出力 (lcms2 なしのコンテナ) */
    caps.gamut = HDRG_GAMUT_SRGB;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &render));
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_SRGB, render.gamut);

    g_encode.output_mode = HDRG_OUTPUT_GAINMAP;
    caps.gamut = HDRG_GAMUT_P3;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &render));
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_P3, render.gamut);

    /* 拡張なしの sRGB ソースは sRGB のまま */
    g_params.expand_strength = 0.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &g_color, &render));
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_SRGB, render.gamut);

    /* sRGB を超えるソースは拡張なしでも sRGB へ潰さない */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_render_build(&g_alloc, &g_plan, &g_params, &g_encode, &caps,
                                                     &adobe, &render));
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_P3, render.gamut);

    hdrg_params_init(&g_params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&g_encode);
    g_encode.output_mode = HDRG_OUTPUT_PQ;
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    int failures;

    UNITY_BEGIN();
    RUN_TEST(test_render_only_rerender);
    RUN_TEST(test_sdr_white_moves_pq_codes);
    RUN_TEST(test_full_frame_limit_fits_apl);
    RUN_TEST(test_rendition_gamut);
    failures = UNITY_END();

    hdrg_plan_free(&g_alloc, &g_plan);
    hdrg_source_free(&g_alloc, &g_src);
    free(g_pixels);

    return failures;
}
