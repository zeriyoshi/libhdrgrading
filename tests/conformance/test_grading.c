/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "io/io.h"

/*
 * Grading behaviour on a synthetic scene: a dark strip and a bright strip.
 * All checks compare decoded PQ codes, which are monotone in luminance.
 */
static hdrg_allocator_t g_alloc;
static hdrg_context_t *g_ctx;
static uint8_t *g_pixels;

static void convert_and_decode(const hdrg_params_t *params, hdrg_decoded_t *out)
{
    hdrg_raw_image_t raw;
    uint8_t *jpeg;
    size_t jpeg_size;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 128;
    raw.height = 128;
    raw.bit_depth = 8;
    raw.stride = 128 * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(g_ctx, &raw, params, NULL, NULL, &jpeg, &jpeg_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, jpeg, jpeg_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, out));

    hdrg_free(jpeg);
}

static float area_mean(const hdrg_decoded_t *d, uint32_t y0, uint32_t y1, uint8_t ch)
{
    uint32_t x, y, count;
    float sum;

    sum = 0.0f;
    count = 0;
    for (y = y0; y < y1; y++) {
        for (x = 8; x < d->width - 8; x++) {
            sum += (float)d->pixels[(size_t)y * d->stride + x * 3 + ch];
            count++;
        }
    }

    return sum / (float)count;
}

/* rows 0..63: dark gray (40), rows 64..127: bright gray (200), with a red
 * patch inside the bright strip */
static void build_scene(void)
{
    uint32_t x, y;
    size_t idx;

    if (g_pixels != NULL) {
        return;
    }

    g_alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&g_ctx, NULL, 0));

    g_pixels = (uint8_t *)malloc(128 * 128 * 3);
    TEST_ASSERT_NOT_NULL(g_pixels);

    for (y = 0; y < 128; y++) {
        for (x = 0; x < 128; x++) {
            idx = ((size_t)y * 128 + x) * 3;
            g_pixels[idx + 0] = (y < 64) ? 40 : 200;
            g_pixels[idx + 1] = (y < 64) ? 40 : 200;
            g_pixels[idx + 2] = (y < 64) ? 40 : 200;
        }
    }

    for (y = 80; y < 112; y++) {
        for (x = 80; x < 112; x++) {
            idx = ((size_t)y * 128 + x) * 3;
            g_pixels[idx + 0] = 220;
            g_pixels[idx + 1] = 60;
            g_pixels[idx + 2] = 60;
        }
    }
}

static void test_contrast_deepens_shadows_expands_highlights(void)
{
    hdrg_params_t neutral, punchy;
    hdrg_decoded_t out_neutral, out_punchy;
    float dark_n, dark_p, bright_n, bright_p;

    build_scene();

    hdrg_params_init(&neutral, HDRG_PRESET_PHOTO);
    neutral.contrast = 1.0f;
    neutral.clarity = 0.0f;
    neutral.saturation = 1.0f;
    /* 拡散面の倍率 (全面一様の持ち上げ) でシャドウフロアから離し、contrast を単離する */
    neutral.diffuse_max = 1.74f;

    punchy = neutral;
    punchy.contrast = 1.5f;

    convert_and_decode(&neutral, &out_neutral);
    convert_and_decode(&punchy, &out_punchy);

    dark_n = area_mean(&out_neutral, 8, 56, 1);
    dark_p = area_mean(&out_punchy, 8, 56, 1);
    bright_n = area_mean(&out_neutral, 66, 76, 1);
    bright_p = area_mean(&out_punchy, 66, 76, 1);

    TEST_ASSERT_TRUE(dark_p < dark_n - 2.0f);
    TEST_ASSERT_TRUE(bright_p > bright_n - 1.0f);
    /* コントラスト = 明部と暗部の差の拡大 */
    TEST_ASSERT_TRUE((bright_p - dark_p) > (bright_n - dark_n) + 4.0f);

    hdrg_decoded_free(&g_alloc, &out_neutral);
    hdrg_decoded_free(&g_alloc, &out_punchy);
}

static void test_saturation_increases_chroma(void)
{
    hdrg_params_t neutral, vivid;
    hdrg_decoded_t out_neutral, out_vivid;
    float green_n, green_v, red_n, red_v;

    build_scene();

    hdrg_params_init(&neutral, HDRG_PRESET_PHOTO);
    neutral.contrast = 1.0f;
    neutral.clarity = 0.0f;
    neutral.saturation = 1.0f;
    neutral.highlight_desat = 0.0f; /* 彩度効果を単離する */

    vivid = neutral;
    vivid.saturation = 1.5f;

    convert_and_decode(&neutral, &out_neutral);
    convert_and_decode(&vivid, &out_vivid);

    /* 赤パッチ: 彩度上昇で G チャネルが下がり R は同等以上 */
    green_n = area_mean(&out_neutral, 84, 108, 1);
    green_v = area_mean(&out_vivid, 84, 108, 1);
    red_n = area_mean(&out_neutral, 84, 108, 0);
    red_v = area_mean(&out_vivid, 84, 108, 0);
    TEST_ASSERT_TRUE(green_v < green_n - 2.0f);
    TEST_ASSERT_TRUE(red_v > red_n - 1.0f);

    hdrg_decoded_free(&g_alloc, &out_neutral);
    hdrg_decoded_free(&g_alloc, &out_vivid);
}

static void test_clarity_amplifies_local_step(void)
{
    hdrg_params_t neutral, clear;
    hdrg_decoded_t out_neutral, out_clear;
    float edge_dark_n, edge_dark_c, edge_bright_n, edge_bright_c;

    build_scene();

    hdrg_params_init(&neutral, HDRG_PRESET_PHOTO);
    neutral.contrast = 1.0f;
    neutral.clarity = 0.0f;
    neutral.saturation = 1.0f;
    /* 拡散面の倍率 (全面一様の持ち上げ) だけにして clarity を単離する */
    neutral.diffuse_max = 1.32f;

    clear = neutral;
    clear.clarity = 0.5f;

    convert_and_decode(&neutral, &out_neutral);
    convert_and_decode(&clear, &out_clear);

    /* 明暗境界近傍の段差が clarity で拡大する */
    edge_dark_n = area_mean(&out_neutral, 58, 63, 1);
    edge_dark_c = area_mean(&out_clear, 58, 63, 1);
    edge_bright_n = area_mean(&out_neutral, 65, 70, 1);
    edge_bright_c = area_mean(&out_clear, 65, 70, 1);
    TEST_ASSERT_TRUE((edge_bright_c - edge_dark_c) > (edge_bright_n - edge_dark_n) + 2.0f);

    hdrg_decoded_free(&g_alloc, &out_neutral);
    hdrg_decoded_free(&g_alloc, &out_clear);
}

void setUp(void)
{
#ifndef HDRG_WITH_LCMS2
    /* the whole suite checks PQ-encoded output, which needs the lcms2 P3 PQ profile */
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

void tearDown(void)
{
}

int main(void)
{
    int failures;

    UNITY_BEGIN();
    RUN_TEST(test_contrast_deepens_shadows_expands_highlights);
    RUN_TEST(test_saturation_increases_chroma);
    RUN_TEST(test_clarity_amplifies_local_step);
    failures = UNITY_END();

    hdrg_context_destroy(g_ctx);
    free(g_pixels);

    return failures;
}
