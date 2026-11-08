/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>

#include "../test_util.h"
#include "io/io.h"

/*
 * 既定出力 (ハイブリッド) の構造検証: PQ プライマリ + "P3 PQ" ICC/cicp と、
 * SDR 復元用ゲインマップ (ISO 21496-1 の HDR ベース方向) の二層。
 */
static hdrg_context_t *g_ctx;
static uint8_t *g_pixels, *g_output;
static size_t g_output_size;

typedef struct {
    size_t mpf_base;
    uint32_t entry0_size, entry1_size, entry1_off;
    bool has_mpf;
} mpf_info_t;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ST 2084 EOTF, 8bit code -> diffuse-white 相対リニア (203 nits = 1.0) */
static float pq_eotf_rel(uint8_t code)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f,
                c3 = 18.6875f;
    float p, num;

    p = powf((float)code / 255.0f, 1.0f / m2);
    num = HDRG_TEST_MAX(p - c1, 0.0f);

    return powf(num / (c2 - c3 * p), 1.0f / m1) * 10000.0f / 203.0f;
}

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

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 256;
    raw.height = 192;
    raw.bit_depth = 8;
    raw.stride = 256 * 3;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_HYBRID;

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert_raw(g_ctx, &raw, &params, NULL, &encode, &g_output, &g_output_size));
}

static void parse_mpf(mpf_info_t *info)
{
    const uint8_t *seg, *entries;
    uint8_t marker;
    size_t pos, seg_len;

    memset(info, 0, sizeof(*info));

    pos = 2;
    while (pos + 4 <= g_output_size && g_output[pos] == 0xFF) {
        marker = g_output[pos + 1];
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }

        seg_len = be16(g_output + pos + 2);
        seg = g_output + pos + 4;
        if (marker == 0xE2 && seg_len >= 2 + 4 + 8 && memcmp(seg, "MPF\0", 4) == 0) {
            info->has_mpf = true;
            info->mpf_base = pos + 2 + 2 + 4;

            entries = g_output + info->mpf_base + 50;
            info->entry0_size = be32(entries + 4);
            info->entry1_size = be32(entries + 16 + 4);
            info->entry1_off = be32(entries + 16 + 8);
        }

        pos += 2 + seg_len;
    }
}

static void test_two_streams_linked_by_mpf(void)
{
    mpf_info_t mpf;

    ensure_converted();
    parse_mpf(&mpf);

    TEST_ASSERT_TRUE(mpf.has_mpf);
    TEST_ASSERT_EQUAL_size_t(g_output_size, (size_t)mpf.entry0_size + mpf.entry1_size);
    TEST_ASSERT_EQUAL_size_t((size_t)mpf.entry0_size, mpf.mpf_base + mpf.entry1_off);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_output[mpf.entry0_size]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, g_output[mpf.entry0_size + 1]);
}

static void test_primary_carries_p3_pq_icc(void)
{
    static const uint8_t cicp_block[12] = {
        'c', 'i', 'c', 'p', 0, 0, 0, 0, 12, 16, 0, 1,
    };
    mpf_info_t mpf;

    ensure_converted();
    parse_mpf(&mpf);

    /* PQ ICC (cicp 込み) はプライマリ側にあること */
    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, mpf.entry0_size, "ICC_PROFILE", 11));
    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, mpf.entry0_size, cicp_block, 12));
}

static void test_iso_metadata_hdr_base_direction(void)
{
    const uint8_t *body;
    static const char urn[] = "urn:iso:std:iso:ts:21496:-1";
    mpf_info_t mpf;
    int found;
    size_t pos;
    float base_hh, alt_hh, gm_min, gm_max, gamma;

    ensure_converted();
    parse_mpf(&mpf);

    found = 0;
    for (pos = mpf.entry0_size; pos + 100 < g_output_size; pos++) {
        if (memcmp(g_output + pos, urn, 27) == 0 && g_output[pos + 27] == 0) {
            body = g_output + pos + 28;
            TEST_ASSERT_EQUAL_UINT16(0, be16(body));
            TEST_ASSERT_EQUAL_UINT16(0, be16(body + 2));
            TEST_ASSERT_EQUAL_HEX8(0x40, body[4]);

            base_hh = (float)(int32_t)be32(body + 5) / 1000000.0f;
            alt_hh = (float)(int32_t)be32(body + 13) / 1000000.0f;
            gm_min = (float)(int32_t)be32(body + 21) / 1000000.0f;
            gm_max = (float)(int32_t)be32(body + 29) / 1000000.0f;
            gamma = (float)be32(body + 37) / 1000000.0f;

            /* HDR ベース: base > 0, alternate = 0, マップは [-base, 0]。
             * gm-cap は適用されない (ピークは --pq と同じ auto 上限) */
            TEST_ASSERT_TRUE(base_hh > 0.05f && base_hh <= 2.31f);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, alt_hh);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, -base_hh, gm_min);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, gm_max);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, gamma);

            found = 1;

            break;
        }
    }

    TEST_ASSERT_TRUE(found);
}

static void test_no_sdr_base_xmp_families(void)
{
    ensure_converted();

    /* Apple/hdrgm XMP は SDR ベース前提のため書かない (ISO のみ) */
    TEST_ASSERT_TRUE(
        hdrg_test_contains(g_output, g_output_size, "urn:iso:std:iso:ts:21496:-1", 27));
    TEST_ASSERT_FALSE(
        hdrg_test_contains(g_output, g_output_size, "HDRGainMap:HDRGainMapVersion", 28));
    TEST_ASSERT_FALSE(hdrg_test_contains(g_output, g_output_size, "hdrgm:Version", 13));
    TEST_ASSERT_FALSE(hdrg_test_contains(g_output, g_output_size, "Container:Directory", 19));
}

static void test_sdr_reconstruction(void)
{
    /* specular 画素: PQ プライマリ × 2^(ダウンマップ) ≈ SDR 白 (~1.0) */
    hdrg_allocator_t alloc;
    hdrg_decoded_t primary, gm;
    hdrg_stats_t stats;
    mpf_info_t mpf;
    uint32_t x, y, px, py;
    uint8_t code_max, gm_code;
    float gain, recovered;

    ensure_converted();
    parse_mpf(&mpf);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops > 0.5f);

    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, g_output, mpf.entry0_size,
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &primary));
    TEST_ASSERT_EQUAL_UINT32(256, primary.width);
    TEST_ASSERT_FALSE(hdrg_color_within(&primary.color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&primary.color, HDRG_GAMUT_P3));

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_decode_any(&alloc, NULL, g_output + mpf.entry0_size, mpf.entry1_size,
                                          UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &gm));
    TEST_ASSERT_EQUAL_UINT32(128, gm.width);

    code_max = 0;
    px = 0;
    py = 0;
    for (y = 0; y < primary.height; y++) {
        for (x = 0; x < primary.width; x++) {
            if (primary.pixels[(size_t)y * primary.stride + x * 3 + 1] > code_max) {
                code_max = primary.pixels[(size_t)y * primary.stride + x * 3 + 1];
                px = x;
                py = y;
            }
        }
    }

    /* PQ code: specular は 1000 nits 近傍 (>= 500 nits 相当) */
    TEST_ASSERT_TRUE(pq_eotf_rel(code_max) > 2.4f);

    gm_code = gm.pixels[(size_t)(py * gm.height / primary.height) * gm.stride +
                        (px * gm.width / primary.width) * 3];
    /* gamma=1: gain = gain_map_min * (1 - code/255) */
    gain = -stats.applied_max_gain_stops * (1.0f - (float)gm_code / 255.0f);
    recovered = pq_eotf_rel(code_max) * exp2f(gain);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 1.0f, recovered);

    hdrg_decoded_free(&alloc, &primary);
    hdrg_decoded_free(&alloc, &gm);
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
    RUN_TEST(test_two_streams_linked_by_mpf);
    RUN_TEST(test_primary_carries_p3_pq_icc);
    RUN_TEST(test_iso_metadata_hdr_base_direction);
    RUN_TEST(test_no_sdr_base_xmp_families);
    RUN_TEST(test_sdr_reconstruction);
    failures = UNITY_END();

    hdrg_free(g_output);
    hdrg_context_destroy(g_ctx);
    free(g_pixels);

    return failures;
}
