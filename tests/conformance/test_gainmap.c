/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>

#include "../test_util.h"
#include "io/io.h"

/* ゲインマップの形の構造検証: HDRG_OUTPUT_GAINMAP (SDR ベース二層 JPEG) */
static hdrg_context_t *g_ctx;
static uint8_t *g_pixels, *g_output;
static size_t g_output_size;

typedef struct {
    size_t mpf_base; /* MP endian フィールドのファイル内位置 */
    size_t primary_len, gainmap_off;
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
    encode.output_mode = HDRG_OUTPUT_GAINMAP;

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

/* プライマリのセグメントを歩き MPF を読む */
static void parse_mpf_of(const uint8_t *data, size_t size, mpf_info_t *info)
{
    const uint8_t *seg, *entries;
    uint8_t marker;
    size_t pos, seg_len;

    memset(info, 0, sizeof(*info));

    pos = 2;
    while (pos + 4 <= size && data[pos] == 0xFF) {
        marker = data[pos + 1];
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }

        seg_len = be16(data + pos + 2);
        seg = data + pos + 4;
        if (marker == 0xE2 && seg_len >= 2 + 4 + 8 && memcmp(seg, "MPF\0", 4) == 0) {
            info->has_mpf = true;
            info->mpf_base = pos + 2 + 2 + 4;

            /* MM..IFD(3)..: エントリ値は endian フィールド + 50 */
            entries = data + info->mpf_base + 50;
            info->entry0_size = be32(entries + 4);
            info->entry1_size = be32(entries + 16 + 4);
            info->entry1_off = be32(entries + 16 + 8);
        }

        pos += 2 + seg_len;
    }
}

static void parse_mpf(mpf_info_t *info)
{
    parse_mpf_of(g_output, g_output_size, info);
}

static void test_mpf_self_consistency(void)
{
    mpf_info_t mpf;

    ensure_converted();
    parse_mpf(&mpf);

    TEST_ASSERT_TRUE(mpf.has_mpf);

    /* entry1.offset + base == len(primary), sizes の合計 == filesize */
    TEST_ASSERT_EQUAL_size_t(g_output_size, (size_t)mpf.entry0_size + mpf.entry1_size);
    TEST_ASSERT_EQUAL_size_t((size_t)mpf.entry0_size, mpf.mpf_base + mpf.entry1_off);

    /* ゲインマップの位置に SOI があること */
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_output[mpf.entry0_size]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, g_output[mpf.entry0_size + 1]);
}

static void test_iso_metadata_roundtrip(void)
{
    const uint8_t *body;
    static const char urn[] = "urn:iso:std:iso:ts:21496:-1";
    mpf_info_t mpf;
    int found;
    size_t pos;
    float alt_headroom, gm_max, gamma;

    ensure_converted();
    parse_mpf(&mpf);

    /* ゲインマップ側ストリーム内の ISO セグメント (61 バイト本体) を探す */
    found = 0;
    for (pos = mpf.entry0_size; pos + 100 < g_output_size; pos++) {
        if (memcmp(g_output + pos, urn, 27) == 0 && g_output[pos + 27] == 0) {
            body = g_output + pos + 28;
            TEST_ASSERT_EQUAL_UINT16(0, be16(body));     /* minimum_version */
            TEST_ASSERT_EQUAL_UINT16(0, be16(body + 2)); /* writer_version */
            TEST_ASSERT_EQUAL_HEX8(0x40, body[4]);       /* 1ch + base cs */
            TEST_ASSERT_EQUAL_UINT32(0, be32(body + 5)); /* base headroom */

            alt_headroom = (float)(int32_t)be32(body + 13) / 1000000.0f;
            gm_max = (float)(int32_t)be32(body + 29) / 1000000.0f;
            gamma = (float)be32(body + 37) / 1000000.0f;

            /*
             * 宣言は既定の上限 3.0 段以下 (表示の余裕が小さいビューアは ISO の重み
             * w = display / declared で全体を縮める)
             */
            TEST_ASSERT_TRUE(alt_headroom > 0.05f && alt_headroom <= 3.0f + 1e-4f);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, alt_headroom, gm_max);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, gamma);
            /* gain_map_min = 0 */
            TEST_ASSERT_EQUAL_UINT32(0, be32(body + 21));

            found = 1;

            break;
        }
    }

    TEST_ASSERT_TRUE(found);
}

static void test_both_streams_decode(void)
{
    hdrg_allocator_t alloc;
    hdrg_decoded_t base, gm;
    mpf_info_t mpf;

    ensure_converted();
    parse_mpf(&mpf);
    alloc = hdrg_test_allocator();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, g_output, mpf.entry0_size,
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &base));
    TEST_ASSERT_EQUAL_UINT32(256, base.width);
    TEST_ASSERT_EQUAL_UINT32(192, base.height);
#ifdef HDRG_WITH_LCMS2
    /* 既定の expand_strength によりベースは P3 ICC つきになる */
    TEST_ASSERT_NOT_NULL(base.icc);
    TEST_ASSERT_FALSE(hdrg_color_within(&base.color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&base.color, HDRG_GAMUT_P3));
#else
    /* sRGB のベースは ICC を持たない */
    TEST_ASSERT_NULL(base.icc);
    TEST_ASSERT_TRUE(base.color.srgb_curve);
    TEST_ASSERT_TRUE(hdrg_color_within(&base.color, HDRG_GAMUT_SRGB));
#endif

    hdrg_decoded_free(&alloc, &base);

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_decode_any(&alloc, NULL, g_output + mpf.entry0_size, mpf.entry1_size,
                                          UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &gm));
    TEST_ASSERT_EQUAL_UINT32(128, gm.width);
    TEST_ASSERT_EQUAL_UINT32(96, gm.height);

    hdrg_decoded_free(&alloc, &gm);
}

static void test_gainmap_stream_is_monochrome(void)
{
    const uint8_t *seg;
    mpf_info_t mpf;
    uint8_t marker;
    int components;
    size_t pos, seg_len;

    ensure_converted();
    parse_mpf(&mpf);

    components = 0;
    pos = mpf.entry0_size + 2;
    while (pos + 4 <= g_output_size && g_output[pos] == 0xFF) {
        marker = g_output[pos + 1];
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }

        seg_len = be16(g_output + pos + 2);
        seg = g_output + pos + 4;

        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC && seg_len >= 7) {
            components = seg[5];
        }

        pos += 2 + seg_len;
    }

    TEST_ASSERT_EQUAL_INT(1, components);
}

static void test_metadata_families_present(void)
{
    ensure_converted();

    /* ISO + Apple XMP + hdrgm XMP + GContainer が既定で併記される */
    TEST_ASSERT_TRUE(
        hdrg_test_contains(g_output, g_output_size, "urn:iso:std:iso:ts:21496:-1", 27));
    TEST_ASSERT_TRUE(
        hdrg_test_contains(g_output, g_output_size, "HDRGainMap:HDRGainMapVersion", 28));
    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, g_output_size, "hdrgm:Version", 13));
    TEST_ASSERT_TRUE(hdrg_test_contains(g_output, g_output_size, "Container:Directory", 19));

    /* ベースに PQ ICC が残っていないこと (二重適用防止) */
    TEST_ASSERT_FALSE(hdrg_test_contains(g_output, g_output_size, "P3 PQ (libhdrgrading)", 21));
}

static void test_reconstruction_headroom(void)
{
    /* 明部 (specular) の再構成: base ~1.0 × 2^headroom がピークに一致する */
    hdrg_allocator_t alloc;
    hdrg_decoded_t base, gm;
    hdrg_stats_t stats;
    mpf_info_t mpf;
    uint32_t x, y;
    uint8_t base_max, gm_max;
    float headroom, recon;

    ensure_converted();
    parse_mpf(&mpf);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    headroom = stats.applied_max_gain_stops;
    TEST_ASSERT_TRUE(headroom > 0.5f);

    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, g_output, mpf.entry0_size,
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &base));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_decode_any(&alloc, NULL, g_output + mpf.entry0_size, mpf.entry1_size,
                                          UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &gm));

    base_max = 0;
    for (y = 0; y < base.height; y++) {
        for (x = 0; x < base.width; x++) {
            base_max =
                (uint8_t)HDRG_TEST_MAX(base_max, base.pixels[(size_t)y * base.stride + x * 3]);
        }
    }

    gm_max = 0;
    for (y = 0; y < gm.height; y++) {
        for (x = 0; x < gm.width; x++) {
            gm_max = (uint8_t)HDRG_TEST_MAX(gm_max, gm.pixels[(size_t)y * gm.stride + x * 3]);
        }
    }

    /* specular 領域: base はほぼ白、ゲインマップはほぼ最大 */
    TEST_ASSERT_TRUE(base_max >= 250);
    TEST_ASSERT_TRUE(gm_max >= 250);

    /* 再構成ピーク = 1.0 × 2^(g/255 × max) ≈ 2^headroom */
    recon = exp2f((float)gm_max / 255.0f * headroom);
    TEST_ASSERT_FLOAT_WITHIN(0.15f * exp2f(headroom), exp2f(headroom), recon);

    hdrg_decoded_free(&alloc, &base);
    hdrg_decoded_free(&alloc, &gm);
}

/* the test image converted again, the encode and display changed (NULL: the defaults) */
static void convert_with(const hdrg_encode_params_t *encode, const hdrg_display_caps_t *display,
                         uint8_t **output, size_t *output_size)
{
    hdrg_raw_image_t raw;

    ensure_converted();

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 256;
    raw.height = 192;
    raw.bit_depth = 8;
    raw.stride = 256 * 3;

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert_raw(g_ctx, &raw, NULL, display, encode, output, output_size));
}

/* the gain map stream's size (decoded) */
static void gainmap_size(const uint8_t *data, size_t size, uint32_t *width, uint32_t *height)
{
    hdrg_allocator_t alloc;
    hdrg_decoded_t gm;
    mpf_info_t mpf;

    parse_mpf_of(data, size, &mpf);
    TEST_ASSERT_TRUE(mpf.has_mpf);
    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_decode_any(&alloc, NULL, data + mpf.entry0_size, mpf.entry1_size,
                                          UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &gm));
    *width = gm.width;
    *height = gm.height;
    hdrg_decoded_free(&alloc, &gm);
}

/* both streams open with a JFIF APP0 (the iPhone's files have it) */
static void test_jfif_first(void)
{
    mpf_info_t mpf;

    ensure_converted();
    parse_mpf(&mpf);

    TEST_ASSERT_EQUAL_MEMORY("\xFF\xD8\xFF\xE0", g_output, 4);
    TEST_ASSERT_EQUAL_MEMORY("JFIF\0", g_output + 6, 5);
    TEST_ASSERT_EQUAL_MEMORY("\xFF\xD8\xFF\xE0", g_output + mpf.entry0_size, 4);
    TEST_ASSERT_EQUAL_MEMORY("JFIF\0", g_output + mpf.entry0_size + 6, 5);
}

/* the stored gain map's resolution: 1 / 2^n of the image's, n 0 its own */
static void test_gainmap_resolution(void)
{
    hdrg_encode_params_t encode;
    uint32_t width, height;
    uint8_t *output;
    size_t output_size;

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP;
    encode.gainmap_scale_shift = 0;
    convert_with(&encode, NULL, &output, &output_size);
    gainmap_size(output, output_size, &width, &height);
    TEST_ASSERT_EQUAL_UINT32(256, width);
    TEST_ASSERT_EQUAL_UINT32(192, height);
    hdrg_free(output);

    encode.gainmap_scale_shift = 2;
    convert_with(&encode, NULL, &output, &output_size);
    gainmap_size(output, output_size, &width, &height);
    TEST_ASSERT_EQUAL_UINT32(64, width);
    TEST_ASSERT_EQUAL_UINT32(48, height);
    hdrg_free(output);
}

/* Apple's XMP and hdrgm's (with the GContainer) each left out alone */
static void test_metadata_families_separately(void)
{
    static const char *const no_apple[] = {"apple_xmp=0"}, *const no_hdrgm[] = {"hdrgm_xmp=0"};
    hdrg_encode_params_t encode;
    uint8_t *output;
    size_t output_size;

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP;
    ensure_converted();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(g_ctx, "hdrg.jpeg", no_apple, 1));
    convert_with(&encode, NULL, &output, &output_size);
    TEST_ASSERT_FALSE(hdrg_test_contains(output, output_size, "HDRGainMap:HDRGainMapVersion", 28));
    TEST_ASSERT_TRUE(hdrg_test_contains(output, output_size, "hdrgm:Version", 13));
    TEST_ASSERT_TRUE(hdrg_test_contains(output, output_size, "urn:iso:std:iso:ts:21496:-1", 27));
    hdrg_free(output);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(g_ctx, "hdrg.jpeg", no_hdrgm, 1));
    convert_with(&encode, NULL, &output, &output_size);
    TEST_ASSERT_TRUE(hdrg_test_contains(output, output_size, "HDRGainMap:HDRGainMapVersion", 28));
    TEST_ASSERT_FALSE(hdrg_test_contains(output, output_size, "hdrgm:Version", 13));
    TEST_ASSERT_FALSE(hdrg_test_contains(output, output_size, "Container:Directory", 19));
    hdrg_free(output);

    /* options without an id: the default writer's (hdrg.jpeg) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(g_ctx, NULL, no_apple, 1));
    convert_with(&encode, NULL, &output, &output_size);
    TEST_ASSERT_FALSE(hdrg_test_contains(output, output_size, "HDRGainMap:HDRGainMapVersion", 28));
    TEST_ASSERT_TRUE(hdrg_test_contains(output, output_size, "hdrgm:Version", 13));
    hdrg_free(output);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_writer(g_ctx, NULL, NULL));
}

/* without the dither: another 8-bit gain map, still both streams */
static void test_without_dither(void)
{
    hdrg_encode_params_t encode;
    uint32_t width, height;
    uint8_t *output;
    size_t output_size;

    ensure_converted();
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP;
    encode.dither = false;
    convert_with(&encode, NULL, &output, &output_size);
    gainmap_size(output, output_size, &width, &height);
    TEST_ASSERT_EQUAL_UINT32(128, width);
    TEST_ASSERT_FALSE(output_size == g_output_size && memcmp(output, g_output, output_size) == 0);
    hdrg_free(output);
}

/*
 * The display's headroom caps the rendition the gain map carries (and declares); without the
 * cap (0) an unknown display gets the default peak's, less than the cap's 3 stops; a display
 * of SDR white 100 nits and no peak given: its peak from the headroom
 */
static void test_display_headroom(void)
{
    hdrg_encode_params_t encode;
    hdrg_display_caps_t display;
    hdrg_stats_t stats;
    uint8_t *output;
    size_t output_size;
    float capped;

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP;
    hdrg_display_caps_init(&display);
    display.headroom_stops = 1.5f;
    convert_with(&encode, &display, &output, &output_size);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops > 0.5f &&
                     stats.applied_max_gain_stops <= 1.5f + 1e-3f);
    hdrg_free(output);

    convert_with(&encode, NULL, &output, &output_size);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    capped = stats.applied_max_gain_stops;
    hdrg_free(output);

    encode.gainmap_headroom_cap_stops = 0.0f;
    convert_with(&encode, NULL, &output, &output_size);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops > 0.5f);
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops < capped);
    hdrg_free(output);

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP;
    display.headroom_stops = 1.0f;
    display.sdr_white_nits = 100.0f;
    display.peak_nits = 0.0f;
    convert_with(&encode, &display, &output, &output_size);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(stats.applied_max_gain_stops > 0.3f &&
                     stats.applied_max_gain_stops <= 1.0f + 1e-3f);
    hdrg_free(output);
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
    RUN_TEST(test_mpf_self_consistency);
    RUN_TEST(test_iso_metadata_roundtrip);
    RUN_TEST(test_both_streams_decode);
    RUN_TEST(test_gainmap_stream_is_monochrome);
    RUN_TEST(test_metadata_families_present);
    RUN_TEST(test_reconstruction_headroom);
    RUN_TEST(test_jfif_first);
    RUN_TEST(test_gainmap_resolution);
    RUN_TEST(test_metadata_families_separately);
    RUN_TEST(test_without_dither);
    RUN_TEST(test_display_headroom);
    failures = UNITY_END();

    hdrg_free(g_output);
    hdrg_context_destroy(g_ctx);
    free(g_pixels);

    return failures;
}
