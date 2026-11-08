/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "io/io.h"

static hdrg_context_t *g_ctx;
static uint8_t *g_pixels, *g_output;
static size_t g_output_size;

/* この suite は単層 PQ (レガシー) 出力の構造を固定する */
static void ensure_converted(void)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;

    if (g_output != NULL) {
        return;
    }

    g_pixels = hdrg_test_image(256, 192);
    TEST_ASSERT_NOT_NULL(g_pixels);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&g_ctx, NULL, 0));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 256;
    raw.height = 192;
    raw.bit_depth = 8;
    raw.stride = 256 * 3;

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert_raw(g_ctx, &raw, &params, NULL, &encode, &g_output, &g_output_size));
}

static void test_output_is_jpeg(void)
{
    ensure_converted();

    TEST_ASSERT_TRUE(g_output_size > 4);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_output[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, g_output[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_output[g_output_size - 2]);
    TEST_ASSERT_EQUAL_HEX8(0xD9, g_output[g_output_size - 1]);
}

static void test_output_carries_p3_pq_icc(void)
{
    /* spec-layout cicp type block: sig + 4 reserved + {P3, PQ, RGB, full} */
    static const uint8_t cicp_block[12] = {
        'c', 'i', 'c', 'p', 0, 0, 0, 0, 12, 16, 0, 1,
    };

    ensure_converted();

    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, g_output_size, "ICC_PROFILE", 11));
    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, g_output_size, cicp_block, 12));
}

static void test_output_decodes_as_p3(void)
{
    hdrg_allocator_t alloc;
    hdrg_decoded_t decoded;

    ensure_converted();
    alloc = hdrg_test_allocator();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, g_output, g_output_size,
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT32(256, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(192, decoded.height);
    TEST_ASSERT_FALSE(hdrg_color_within(&decoded.color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&decoded.color, HDRG_GAMUT_P3));

    hdrg_decoded_free(&alloc, &decoded);
}

static void test_stats_populated(void)
{
    hdrg_stats_t stats;

    ensure_converted();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops > 0.0f);
    TEST_ASSERT_TRUE(stats.diffuse_stops >= 0.0f);
    TEST_ASSERT_TRUE(stats.light_area_ratio >= 0.0f);
}

static void test_jpeg_input_roundtrip(void)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_allocator_t alloc;
    uint8_t *pixels, *input, *output;
    size_t input_size, output_size;

    alloc = hdrg_test_allocator();

    pixels = hdrg_test_image(120, 80);
    TEST_ASSERT_NOT_NULL(pixels);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_jpeg_rgb(&alloc, pixels, 120, 80, 120 * 3, 95, NULL, 0,
                                                      NULL, 0, &input, &input_size));

    free(pixels);

    ensure_converted();

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert(g_ctx, input, input_size, &params, NULL, &encode,
                                                &output, &output_size));
    TEST_ASSERT_EQUAL_HEX8(0xFF, output[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, output[1]);

    hdrg_free(output);
    hdrg_mem_free(&alloc, input);
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
    RUN_TEST(test_output_is_jpeg);
    RUN_TEST(test_output_carries_p3_pq_icc);
    RUN_TEST(test_output_decodes_as_p3);
    RUN_TEST(test_stats_populated);
    RUN_TEST(test_jpeg_input_roundtrip);
    failures = UNITY_END();

    hdrg_free(g_output);
    hdrg_context_destroy(g_ctx);
    free(g_pixels);

    return failures;
}
