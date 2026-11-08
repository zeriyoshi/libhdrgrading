/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/video.h"
#include "io/io.h"

/* a PNG past the limit: refused by its reader, or by none without it (HDRG_PLUGINS_EXCLUDE) */
#define PNG_LIMIT ((HDRG_BUILTIN_PNG != 0) ? HDRG_ERR_LIMIT : HDRG_ERR_UNSUPPORTED_FORMAT)

static const uint8_t g_gif_colors[6] = {0, 0, 0, 255, 255, 255};
static hdrg_params_t params;
static hdrg_encode_params_t encode;
static hdrg_display_caps_t display;

static hdrg_status_t convert_color_status(const hdrg_params_t *p, uint8_t primaries,
                                          uint8_t transfer)
{
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t *pixels, *output;
    size_t output_size;

    pixels = hdrg_test_image(64, 64);
    TEST_ASSERT_NOT_NULL(pixels);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = primaries;
    raw.transfer = transfer;
    raw.width = 64;
    raw.height = 64;
    raw.bit_depth = 8;
    raw.stride = 64 * 3;

    status = hdrg_convert_raw(ctx, &raw, p, &display, &encode, &output, &output_size);
    if (status == HDRG_OK) {
        hdrg_free(output);
    }

    hdrg_context_destroy(ctx);
    free(pixels);

    return status;
}

static hdrg_status_t convert_status(const hdrg_params_t *p)
{
    return convert_color_status(p, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB);
}

/* a still's conversion with these prefilters: UNSUPPORTED for one the build left out */
static hdrg_status_t prefilter_status(const hdrg_params_t *p)
{
    const bool missing = (p->deblock_strength > 0.0f && HDRG_BUILTIN_DEBLOCK == 0) ||
                         (p->dering_strength > 0.0f && HDRG_BUILTIN_DERING == 0) ||
                         (p->defringe_strength > 0.0f && HDRG_BUILTIN_DEFRINGE == 0) ||
                         (p->deband_strength > 0.0f && HDRG_BUILTIN_DEBAND == 0);

    return missing ? HDRG_ERR_UNSUPPORTED : HDRG_OK;
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t png_crc(const uint8_t *data, size_t size)
{
    uint32_t crc, k;
    uint8_t b;
    size_t i;

    crc = 0xFFFFFFFFu;

    for (i = 0; i < size; i++) {
        crc ^= data[i];

        for (b = 0; b < 8u; b++) {
            k = ((crc & 1u) != 0) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ k;
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/* a chunk at p (length, type, data, the CRC of the type and data); returns its size */
static size_t png_chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t length)
{
    put_be32(p, length);
    memcpy(p + 4, type, 4);

    if (length > 0) {
        memcpy(p + 8, data, length);
    }

    put_be32(p + 8 + length, png_crc(p + 4, length + 4u));

    return 12u + length;
}

/* a width x height RGB PNG's signature and IHDR, then an IDAT and IEND: the header reads */
static size_t png_header(uint8_t *out, uint32_t width, uint32_t height)
{
    const uint8_t idat[2] = {0x78, 0x9C};
    uint8_t ihdr[13];
    size_t n;

    memset(ihdr, 0, sizeof(ihdr));
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;
    ihdr[9] = 2;

    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    n = 8;
    n += png_chunk(out + n, "IHDR", ihdr, sizeof(ihdr));
    n += png_chunk(out + n, "IDAT", idat, sizeof(idat));
    n += png_chunk(out + n, "IEND", NULL, 0);

    return n;
}

/* a 3-bit LZW code into the packed bytes (least significant bit first) */
static void gif_code(uint8_t *lzw, size_t *length, uint32_t *acc, uint32_t *bits, uint32_t code)
{
    *acc |= code << *bits;
    *bits += 3;

    while (*bits >= 8) {
        lzw[(*length)++] = (uint8_t)(*acc & 0xFFu);
        *acc >>= 8;
        *bits -= 8;
    }
}

/*
 * A width x height GIF of two colors and the first pixels of it, alternating:
 * 3-bit LZW codes with a clear code before every two pixels (the table never
 * outgrows them)
 */
static size_t gif_image(uint8_t *out, uint32_t width, uint32_t height, uint32_t pixels)
{
    uint32_t i, acc, bits;
    uint8_t lzw[128];
    size_t n, length;

    memcpy(out, "GIF89a", 6);
    put_le16(out + 6, width);
    put_le16(out + 8, height);
    out[10] = 0x80;
    out[11] = 0;
    out[12] = 0;
    memcpy(out + 13, g_gif_colors, sizeof(g_gif_colors));
    n = 19;

    out[n] = 0x2C;
    put_le16(out + n + 1, 0);
    put_le16(out + n + 3, 0);
    put_le16(out + n + 5, width);
    put_le16(out + n + 7, height);
    out[n + 9] = 0;
    out[n + 10] = 2;
    n += 11;

    acc = 0;
    bits = 0;
    length = 0;

    for (i = 0; i < pixels; i++) {
        if ((i % 2u) == 0) {
            gif_code(lzw, &length, &acc, &bits, 4);
        }

        gif_code(lzw, &length, &acc, &bits, i & 1u);
    }

    gif_code(lzw, &length, &acc, &bits, 5);

    if (bits > 0) {
        lzw[length++] = (uint8_t)acc;
    }

    out[n++] = (uint8_t)length;
    memcpy(out + n, lzw, length);
    n += length;
    out[n++] = 0;
    out[n++] = 0x3B;

    return n;
}

/* sets the size in the frame header of a JPEG, walking its segments */
static bool jpeg_set_size(uint8_t *jpeg, size_t size, uint32_t width, uint32_t height)
{
    size_t i;

    i = 2;

    while (i + 9 < size && jpeg[i] == 0xFF) {
        if (jpeg[i + 1] >= 0xC0 && jpeg[i + 1] <= 0xC2) {
            jpeg[i + 5] = (uint8_t)(height >> 8);
            jpeg[i + 6] = (uint8_t)(height & 0xFFu);
            jpeg[i + 7] = (uint8_t)(width >> 8);
            jpeg[i + 8] = (uint8_t)(width & 0xFFu);

            return true;
        }

        i += 2u + (((size_t)jpeg[i + 2] << 8) | jpeg[i + 3]);
    }

    return false;
}

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_UINT32(sizeof(params), params.struct_size);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, params.global_strength);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, params.diffuse_max);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, params.apl_budget_nits);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, params.apl_peak_fraction);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, params.contrast);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, params.clarity);
    TEST_ASSERT_EQUAL_FLOAT(1.05f, params.saturation);
    TEST_ASSERT_EQUAL_FLOAT(0.4f, params.highlight_desat);
    TEST_ASSERT_EQUAL_FLOAT(0.7f, params.expand_strength);
    TEST_ASSERT_EQUAL_FLOAT(0.85f, params.expand_reach);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, params.expand_skin);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, params.expand_memory);
    TEST_ASSERT_EQUAL_UINT8(HDRG_PRESET_PHOTO, params.preset);
    TEST_ASSERT_EQUAL_UINT8(HDRG_SDR_EOTF_AUTO, params.sdr_eotf);
    TEST_ASSERT_EQUAL_UINT32(sizeof(encode), encode.struct_size);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, encode.tonemap_knee);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, encode.gainmap_headroom_cap_stops);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, encode.gainmap_gamma);
    TEST_ASSERT_EQUAL_UINT8(HDRG_OUTPUT_AUTO, encode.output_mode);
    TEST_ASSERT_EQUAL_UINT8(1, encode.gainmap_scale_shift);
    TEST_ASSERT_TRUE(encode.dither);
    TEST_ASSERT_EQUAL_UINT32(sizeof(display), display.struct_size);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, display.peak_nits);
    TEST_ASSERT_TRUE(display.headroom_stops < 0.0f);
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_P3, display.gamut);
}

/* プリセットは同じパイプラインの別の出発点 */
static void test_presets(void)
{
    hdrg_params_t anime, game, fallback;

    hdrg_params_init(&anime, HDRG_PRESET_ANIME);
    TEST_ASSERT_EQUAL_UINT8(HDRG_PRESET_ANIME, anime.preset);
    TEST_ASSERT_EQUAL_UINT8(HDRG_SDR_EOTF_GAMMA22, anime.sdr_eotf);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, anime.expand_skin);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, anime.expand_memory);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, anime.saturation);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, anime.apl_budget_nits);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&anime));

    hdrg_params_init(&game, HDRG_PRESET_GAME);
    TEST_ASSERT_EQUAL_UINT8(HDRG_PRESET_GAME, game.preset);
    TEST_ASSERT_EQUAL_UINT8(HDRG_SDR_EOTF_GAMMA22, game.sdr_eotf);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, game.expand_strength);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&game));

    /* 未知のプリセットは PHOTO */
    hdrg_params_init(&fallback, 200);
    TEST_ASSERT_EQUAL_UINT8(HDRG_PRESET_PHOTO, fallback.preset);
    TEST_ASSERT_EQUAL_MEMORY(&params, &fallback, sizeof(params));
}

/* 生画素の色は原色 + 伝達特性の組で指定し、未知の組は拒否される */
static void test_raw_color_codes(void)
{
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT709));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_PROPHOTO, HDRG_TRANSFER_PROPHOTO));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_P3_D65, HDRG_TRANSFER_LINEAR));
    /* SD video's BT.601 and the 10 / 12-bit BT.2020 curves (BT.1886 as BT.709), gamma 2.8 */
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          convert_color_status(&params, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_BT601));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT2020_10));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT2020_12));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, convert_color_status(&params, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_GAMMA28));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          convert_color_status(&params, 0, HDRG_TRANSFER_SRGB));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          convert_color_status(&params, HDRG_PRIMARIES_BT709, 0));
    /* PQ (16) と HLG (18) の入力は扱わない */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          convert_color_status(&params, HDRG_PRIMARIES_BT2020, 16));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          convert_color_status(&params, HDRG_PRIMARIES_BT2020, 18));
}

static void test_null_params_uses_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(NULL));
}

static void test_explicit_defaults_accepted(void)
{
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));
}

static void test_bad_struct_size_rejected(void)
{
    params.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    encode.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_encode_params_init(&encode);
    display.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

static void test_out_of_domain_strength(void)
{
    params.global_strength = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

static void test_out_of_domain_scale_shift(void)
{
    encode.gainmap_scale_shift = 9;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

/* the JPEG codec's options: their defaults, and a value outside the range refused */
static void test_out_of_domain_quality(void)
{
    static const char *const keys[6] = {"quality",  "gainmap_quality", "hagc",
                                        "icc_lumi", "apple_xmp",       "hdrgm_xmp"},
                             *const zero[] = {"quality=0"}, *const top[] = {"quality=100"};
    static const double values[6] = {98.0, 90.0, 1.0, 0.0, 1.0, 1.0};
    hdrg_codec_info_t info;
    hdrg_option_info_t option;
    hdrg_context_t *ctx;
    uint32_t i, j, jpeg;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 1));

    jpeg = UINT32_MAX;
    info.struct_size = sizeof(info);

    for (i = 0; i < hdrg_context_codec_count(ctx); i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_codec_info(ctx, i, &info));

        if (strcmp(info.id, "hdrg.jpeg") == 0) {
            jpeg = i;

            break;
        }
    }

    TEST_ASSERT_TRUE(jpeg != UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING("jpg,jpeg", info.extensions);
    TEST_ASSERT_EQUAL_UINT32(HDRG_CODEC_READ | HDRG_CODEC_WRITE, info.roles);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OUTPUT_GAINMAP, info.default_form);
    TEST_ASSERT_EQUAL_UINT32(6, info.option_count);

    for (i = 0; i < 6u; i++) {
        option.struct_size = sizeof(option);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_codec_option(ctx, jpeg, i, &option));

        for (j = 0; j < 6u && strcmp(option.key, keys[j]) != 0; j++) {
        }

        TEST_ASSERT_TRUE(j < 6u);
        TEST_ASSERT_TRUE((option.type == HDRG_OPTION_FLOAT
                              ? option.default_float
                              : (double)option.default_int) == values[j]);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_test_writer(ctx, "hdrg.jpeg", zero, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(ctx, "hdrg.jpeg", top, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, hdrg_test_writer(ctx, "hdrg.png", NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, hdrg_test_writer(ctx, "no.such", NULL, 0));
    hdrg_context_destroy(ctx);
}

static void test_out_of_domain_peak(void)
{
    display.peak_nits = 20000.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_display_caps_init(&display);
    display.gamut = HDRG_GAMUT_BT2020 + 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    /* a still file carries the display's peak; BT.2020 clamps to what it can tag */
    hdrg_display_caps_init(&display);
    display.peak_nits = 600.0f;
    display.gamut = HDRG_GAMUT_BT2020;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));

    hdrg_display_caps_init(&display);
    params.apl_budget_nits = -1.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.apl_peak_fraction = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

static void test_out_of_domain_diffuse(void)
{
    params.diffuse_max = 0.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.diffuse_max = 4.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

static void test_out_of_domain_grading(void)
{
    params.contrast = 5.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.clarity = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.saturation = 3.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.highlight_desat = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    /* the light predictor's amount: 0..1, the whole of it converts */
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.light_predictor = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    params.light_predictor = -0.25f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    params.light_predictor = 1.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_TEST_BUILTIN_STATUS(PREDICT), convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    encode.tonemap_knee = 0.05f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_encode_params_init(&encode);
    encode.gainmap_gamma = 10.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_encode_params_init(&encode);
    encode.gainmap_headroom_cap_stops = 6.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_encode_params_init(&encode);
    params.expand_strength = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.expand_reach = -0.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.preset = 3;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.sdr_eotf = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

/* コンテキストの記述子: 版の違う struct_size は拒否、NULL は既定、SIMD の上限 */
static void test_context_desc(void)
{
    hdrg_context_desc_t desc;
    hdrg_raw_image_t raw;
    hdrg_stats_t stats;
    hdrg_context_t *ctx;
    uint8_t *pixels, *output;
    size_t output_size;

    hdrg_context_desc_init(&desc);
    desc.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_context_create(&desc, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_context_create(NULL, NULL));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    hdrg_context_destroy(ctx);

    pixels = hdrg_test_image(64, 64);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 64;
    raw.height = 64;
    raw.bit_depth = 8;
    raw.stride = 64 * 3;

    hdrg_context_desc_init(&desc);
    desc.simd_max = HDRG_SIMD_SCALAR;
    desc.thread_count = 2;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, hdrg_get_last_stats(ctx, &stats)); /* none yet */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_get_last_stats(ctx, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(ctx, &stats));
    TEST_ASSERT_EQUAL_UINT8(HDRG_SIMD_SCALAR, stats.simd_tier);

    hdrg_free(output);
    hdrg_context_destroy(ctx);
    free(pixels);
}

/* 画素数の上限: 入力・出力・セッション・各デコーダのヘッダで、確保の前に HDRG_ERR_LIMIT */
static void test_pixel_limit(void)
{
    hdrg_session_config_t config;
    hdrg_context_desc_t desc;
    hdrg_raw_image_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint8_t *pixels, *output, *jpeg, file[512];
    size_t output_size, jpeg_size, size;

    pixels = hdrg_test_image(64, 64);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 64;
    raw.height = 64;
    raw.bit_depth = 8;
    raw.stride = 64 * 3;

    hdrg_context_desc_init(&desc);
    TEST_ASSERT_TRUE(desc.max_pixels == 0);
    desc.max_pixels = 64u * 64u - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_LIMIT,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_LIMIT, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&raw), NULL,
                                                              NULL, &config, &session));
    hdrg_context_destroy(ctx);

    /* at the limit; an output larger than it */
    desc.max_pixels = 64u * 64u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    hdrg_free(output);
    encode.output_width = 128;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_LIMIT,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, &encode, &output, &output_size));
    hdrg_context_destroy(ctx);

    /* the default limit against headers that claim more */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &jpeg, &jpeg_size));
    TEST_ASSERT_TRUE(jpeg_set_size(jpeg, jpeg_size, 60000, 60000));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_LIMIT, hdrg_convert(ctx, jpeg, jpeg_size, NULL, NULL, NULL,
                                                       &output, &output_size));
    hdrg_free(jpeg);

    /* the readers the build has (a file of none: no reader takes it) */
    size = png_header(file, 60000, 60000);
    TEST_ASSERT_EQUAL_INT(PNG_LIMIT,
                          hdrg_convert(ctx, file, size, NULL, NULL, NULL, &output, &output_size));

    size = gif_image(file, 60000, 60000, 2);
    TEST_ASSERT_EQUAL_INT((HDRG_BUILTIN_GIF != 0) ? HDRG_ERR_LIMIT : HDRG_ERR_UNSUPPORTED_FORMAT,
                          hdrg_convert(ctx, file, size, NULL, NULL, NULL, &output, &output_size));

    /* the first image of a GIF still decodes */
    size = gif_image(file, 4, 4, 16);
    TEST_ASSERT_EQUAL_INT((HDRG_BUILTIN_GIF != 0) ? HDRG_OK : HDRG_ERR_UNSUPPORTED_FORMAT,
                          hdrg_convert(ctx, file, size, NULL, NULL, NULL, &output, &output_size));
    hdrg_free(output);

    hdrg_context_destroy(ctx);

#if SIZE_MAX <= UINT32_MAX
    /* where size_t is 32 bits, a limit raised past it is capped (2^28 - 1): sizes never wrap */
    desc.max_pixels = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    size = png_header(file, 60000, 60000);
    TEST_ASSERT_EQUAL_INT(PNG_LIMIT,
                          hdrg_convert(ctx, file, size, NULL, NULL, NULL, &output, &output_size));
    hdrg_context_destroy(ctx);
#endif

    free(pixels);
}

/* 名前での設定: 一覧の各名前が設定でき、未知の名前・NULL の構造体・型の外の値は拒否 */
static void test_settings_by_name(void)
{
    hdrg_session_config_t config;
    hdrg_setting_t setting;
    uint32_t i;

    hdrg_session_config_init(&config);

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        TEST_ASSERT_NOT_NULL(setting.name);
        TEST_ASSERT_TRUE(setting.part <= HDRG_SETTING_SESSION && setting.kind <= HDRG_SETTING_U16);
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, (setting.part == HDRG_SETTING_SESSION)
                         ? hdrg_session_setting_set(setting.name, 1.0, &config)
                         : hdrg_setting_set(setting.name, 1.0, &params, &display, &encode));
    }

    TEST_ASSERT_TRUE(i >= 50u);
    TEST_ASSERT_FALSE(hdrg_setting_at(i, &setting));
    TEST_ASSERT_FALSE(hdrg_setting_at(0, NULL));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("diffuse-max", 2.5, &params, NULL, NULL));
    TEST_ASSERT_TRUE(params.diffuse_max == 2.5f);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("sdr-white", 240.0, NULL, &display, NULL));
    TEST_ASSERT_TRUE(display.sdr_white_nits == 240.0f);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("scale-shift", 2.0, NULL, NULL, &encode));
    TEST_ASSERT_EQUAL_UINT8(2, encode.gainmap_scale_shift);

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          hdrg_setting_set("no-such", 1.0, &params, &display, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_setting_set(NULL, 1.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("peak-nits", 1.0, &params, NULL, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("diffuse-max", NAN, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("scale-shift", 2.5, NULL, NULL, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("scale-shift", 300.0, NULL, NULL, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          hdrg_setting_set("quality", 90.0, NULL, NULL, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("dither", 2.0, NULL, NULL, &encode));
}

/*
 * the named values: each name's value and back, numbers otherwise, the unknown refused; a
 * session's settings into its config alone; the preset initializes the intent
 */
static void test_settings_names(void)
{
    const char *text;
    hdrg_session_config_t config;
    hdrg_params_t game;
    hdrg_setting_t setting;
    uint32_t i, j, named;
    double value, parsed;

    named = 0;

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        for (j = 0; hdrg_setting_value_at(setting.name, j, &text, &value); j++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse(setting.name, text, &parsed));
            TEST_ASSERT_TRUE(parsed == value);
            TEST_ASSERT_EQUAL_STRING(text, hdrg_setting_value_name(setting.name, value));
        }

        TEST_ASSERT_EQUAL_UINT32(setting.values, j);
        named += (j > 0) ? 1u : 0u;
    }

    TEST_ASSERT_TRUE(named >= 10u);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse("temporal", "two-pass", &value));
    TEST_ASSERT_TRUE(value == (double)HDRG_TEMPORAL_TWO_PASS);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse("upscaler", "nis", &value));
    TEST_ASSERT_TRUE(value == (double)HDRG_UPSCALER_NIS);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse("diffuse-max", "2.5", &value));
    TEST_ASSERT_TRUE(value == 2.5);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_setting_parse("latency", "fast", &value));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, hdrg_setting_parse("no-such", "1", &value));
    TEST_ASSERT_NULL(hdrg_setting_value_name("diffuse-max", 2.0));
    TEST_ASSERT_FALSE(hdrg_setting_value_at("latency", 3, &text, &value));

    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse("past", "auto", &value));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_setting_set("past", 4.0, &config));
    TEST_ASSERT_EQUAL_UINT16(4, config.past);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_setting_set("past", value, &config));
    TEST_ASSERT_EQUAL_UINT16(HDRG_SESSION_AUTO, config.past);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_setting_set("flash-off", 1.0, &config));
    TEST_ASSERT_TRUE(config.flash_off);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_setting_set("past", 70000.0, &config));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("latency", 1.0, &params, &display, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_session_setting_set("diffuse-max", 1.0, &config));

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_params_init(&game, HDRG_PRESET_GAME);
    params.diffuse_max = 3.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_parse("preset", "game", &value));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("preset", value, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&game, &params, sizeof(params));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_set("preset", 3.0, &params, NULL, NULL));
}

/* a switch set off by name reads back off */
static void test_settings_switch_off(void)
{
    double value;

    encode.dither = true;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("dither", 0.0, NULL, NULL, &encode));
    TEST_ASSERT_FALSE(encode.dither);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("dither", NULL, NULL, &encode, &value));
    TEST_ASSERT_TRUE(value == 0.0);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("dither", 1.0, NULL, NULL, &encode));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("dither", NULL, NULL, &encode, &value));
    TEST_ASSERT_TRUE(value == 1.0);
}

/* every status its own text; a code no status has the unknown one */
static void test_status_strings(void)
{
    const char *texts[HDRG_ERR_LIMIT + 1u];
    uint32_t i, j;

    for (i = 0; i <= HDRG_ERR_LIMIT; i++) {
        texts[i] = hdrg_status_string(i);
        TEST_ASSERT_NOT_NULL(texts[i]);
        TEST_ASSERT_TRUE(strcmp(texts[i], hdrg_status_string(UINT32_C(0xFFFF))) != 0);

        for (j = 0; j < i; j++) {
            TEST_ASSERT_TRUE(strcmp(texts[i], texts[j]) != 0);
        }
    }

    TEST_ASSERT_EQUAL_STRING("unknown status", hdrg_status_string(HDRG_ERR_LIMIT + 1u));
}

/*
 * The output's size: one side given derives the other from the aspect ratio (refused past
 * 65535); a JPEG's sides stop at 65500 (jpegli's), refused before the work
 */
static void test_output_sides(void)
{
    hdrg_allocator_t alloc;
    hdrg_decoded_t decoded;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    uint8_t *pixels, *wide, *output;
    size_t output_size;

    pixels = hdrg_test_image(64, 32);
    wide = (uint8_t *)calloc((size_t)65501 * 3u, 1);
    TEST_ASSERT_TRUE(pixels != NULL && wide != NULL);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    alloc = hdrg_test_allocator();

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 64;
    raw.height = 32;
    raw.bit_depth = 8;
    raw.stride = 64 * 3;

    /* the height alone: twice the width */
    encode.output_height = 48;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, &encode, &output, &output_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, output, output_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT32(96, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(48, decoded.height);
    hdrg_decoded_free(&alloc, &decoded);
    hdrg_free(output);

    /* a derived side past 65535 */
    encode.output_height = 40000;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, &encode, &output, &output_size));

    /* a JPEG 65501 wide: given, or the input's */
    encode.output_width = 65501;
    encode.output_height = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, &encode, &output, &output_size));
    raw.pixels = wide;
    raw.width = 65501;
    raw.height = 1;
    raw.stride = (size_t)65501 * 3u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    raw.width = 65500;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    hdrg_free(output);

    hdrg_context_destroy(ctx);
    free(wide);
    free(pixels);
}

/* 名前での読み取りは設定の往復になり、表示能力のピークの上限は既定 1000、ピーク・ヘッドルームに従う
 */
static void test_settings_get_and_peak(void)
{
    hdrg_session_config_t config;
    hdrg_setting_t setting;
    uint32_t i;
    double value;

    hdrg_params_init(&params, HDRG_PRESET_ANIME);
    hdrg_display_caps_init(&display);
    hdrg_encode_params_init(&encode);

    hdrg_session_config_init(&config);

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        if (setting.part == HDRG_SETTING_SESSION) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_setting_get(setting.name, &config, &value));
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_setting_set(setting.name, value, &config));
        } else {
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_setting_get(setting.name, &params, &display, &encode, &value));
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_setting_set(setting.name, value, &params, &display, &encode));
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("contrast", 1.25, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("contrast", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(value == 1.25);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("dither", NULL, NULL, &encode, &value));
    TEST_ASSERT_TRUE(value == 1.0);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          hdrg_setting_get("no-such", &params, NULL, NULL, &value));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_setting_get("peak-nits", &params, NULL, NULL, &value));

    TEST_ASSERT_TRUE(hdrg_display_caps_peak(NULL) == 1000.0f);
    display.peak_nits = 600.0f;
    TEST_ASSERT_TRUE(hdrg_display_caps_peak(&display) == 600.0f);
    display.peak_nits = 0.0f;
    display.headroom_stops = 1.0f;
    TEST_ASSERT_TRUE(hdrg_display_caps_peak(&display) == 406.0f);
    display.struct_size = 3;
    TEST_ASSERT_TRUE(hdrg_display_caps_peak(&display) == 0.0f);
}

/*
 * 粒状性の方針: 既定は AUTO (静止画は PRESERVE、PHOTO の動画のセッションは RESYNTH)、REDUCE は
 * 前処理を通す、静止画で明示した RESYNTH は未対応、grain_amount は 0〜4
 */
static void test_grain_policy(void)
{
    TEST_ASSERT_EQUAL_UINT8(HDRG_GRAIN_AUTO, params.grain_policy);
    TEST_ASSERT_TRUE(params.denoise_strength < 0.0f);
    TEST_ASSERT_TRUE(params.grain_amount == 1.0f);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));

    params.grain_policy = HDRG_GRAIN_REDUCE;
    TEST_ASSERT_EQUAL_INT(HDRG_TEST_BUILTIN_STATUS(DENOISE), convert_status(&params));

    params.denoise_strength = 4.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.denoise_strength = NAN;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.denoise_strength = 2.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_TEST_BUILTIN_STATUS(DENOISE), convert_status(&params));

    params.grain_policy = HDRG_GRAIN_RESYNTH;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, convert_status(&params));

    params.grain_policy = HDRG_GRAIN_AUTO + 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.grain_policy = HDRG_GRAIN_AUTO;
    params.grain_amount = 4.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.grain_amount = -0.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
}

/*
 * Deblocking, deringing, defringing and debanding: the presets' defaults
 * (PHOTO deblocks and debands at 0.5, ANIME also derings and defringes and
 * debands at 2.5, GAME none), 0 to 4, 0 to 3, 0 to 3 and 0 to 3, set by their
 * names
 */
static void test_prefilter_defaults(void)
{
    hdrg_params_t preset;
    double value;

    TEST_ASSERT_TRUE(params.deblock_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBLOCK, 1.0f) &&
                     params.dering_strength == 0.0f &&
                     params.deband_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBAND, 0.5f));
    hdrg_params_init(&preset, HDRG_PRESET_ANIME);
    TEST_ASSERT_TRUE(preset.deblock_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBLOCK, 1.0f) &&
                     preset.dering_strength == HDRG_TEST_BUILTIN_STRENGTH(DERING, 2.0f) &&
                     preset.defringe_strength == HDRG_TEST_BUILTIN_STRENGTH(DEFRINGE, 3.0f) &&
                     preset.deband_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBAND, 2.5f));
    hdrg_params_init(&preset, HDRG_PRESET_GAME);
    TEST_ASSERT_TRUE(preset.deblock_strength == 0.0f && preset.dering_strength == 0.0f);

    params.dering_strength = 3.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.dering_strength = 2.0f;
    TEST_ASSERT_EQUAL_INT(prefilter_status(&params), convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("dering", 0.5, &params, NULL, NULL));
    TEST_ASSERT_TRUE(params.dering_strength == 0.5f);

    params.deblock_strength = 1.0f;
    TEST_ASSERT_EQUAL_INT(prefilter_status(&params), convert_status(&params));

    params.deblock_strength = -0.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.deblock_strength = 4.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    params.deblock_strength = NAN;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("deblock", 2.5, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("deblock", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.deblock_strength == 2.5f && value == 2.5);

    TEST_ASSERT_TRUE(params.defringe_strength == 0.0f);
    params.defringe_strength = 3.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("defringe", 3.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(prefilter_status(&params), convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("defringe", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.defringe_strength == 3.0f && value == 3.0);

    TEST_ASSERT_TRUE(params.deband_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBAND, 0.5f) &&
                     preset.deband_strength == 0.0f);
    hdrg_params_init(&preset, HDRG_PRESET_ANIME);
    TEST_ASSERT_TRUE(preset.deband_strength == HDRG_TEST_BUILTIN_STRENGTH(DEBAND, 2.5f));
    params.deband_strength = 3.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("deband", 1.5, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(prefilter_status(&params), convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("deband", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.deband_strength == 1.5f && value == 1.5);

    /* the temporal noise reduction (a session's): PHOTO and ANIME 1, GAME 0, 0 to 4 */
    TEST_ASSERT_TRUE(params.mctf_strength == HDRG_TEST_BUILTIN_STRENGTH(MCTF, 1.0f) &&
                     preset.mctf_strength == HDRG_TEST_BUILTIN_STRENGTH(MCTF, 1.0f));
    hdrg_params_init(&preset, HDRG_PRESET_GAME);
    TEST_ASSERT_TRUE(preset.mctf_strength == 0.0f);
    params.mctf_strength = 4.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    params.mctf_strength = -0.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("mctf", 2.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(prefilter_status(&params), convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("mctf", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.mctf_strength == 2.0f && value == 2.0);
}

/* the chroma fit: segmented for ANIME, else linear; nothing past segmented; set by name */
static void test_chroma_fit(void)
{
    hdrg_params_t preset;
    double value;

    TEST_ASSERT_EQUAL_UINT8(HDRG_CHROMA_FIT_LINEAR, params.chroma_fit);
    hdrg_params_init(&preset, HDRG_PRESET_ANIME);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CHROMA_FIT_SEGMENTED, preset.chroma_fit);
    hdrg_params_init(&preset, HDRG_PRESET_GAME);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CHROMA_FIT_LINEAR, preset.chroma_fit);

    params.chroma_fit = HDRG_CHROMA_FIT_SEGMENTED + 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("chroma-fit", 1.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("chroma-fit", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.chroma_fit == HDRG_CHROMA_FIT_SEGMENTED && value == 1.0);
}

/* the upscaler: Lanczos for PHOTO, the edge-adaptive for ANIME and GAME; nothing past NIS; set by
 * name */
static void test_upscaler(void)
{
    const uint8_t edge = (HDRG_BUILTIN_EDGE != 0) ? HDRG_UPSCALER_EDGE : HDRG_UPSCALER_LANCZOS;
    hdrg_params_t preset;
    double value;

    TEST_ASSERT_EQUAL_UINT8(HDRG_UPSCALER_LANCZOS, params.upscaler);
    hdrg_params_init(&preset, HDRG_PRESET_ANIME);
    TEST_ASSERT_EQUAL_UINT8(edge, preset.upscaler);
    hdrg_params_init(&preset, HDRG_PRESET_GAME);
    TEST_ASSERT_EQUAL_UINT8(edge, preset.upscaler);

    params.upscaler = HDRG_UPSCALER_NIS + 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, convert_status(&params));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("upscaler", 2.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("upscaler", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.upscaler == HDRG_UPSCALER_FSR1 && value == 2.0);
}

/* the colored lights: on for ANIME, else off; set by name */
static void test_colored_lights(void)
{
    hdrg_params_t preset;
    double value;

    TEST_ASSERT_FALSE(params.colored_lights);
    hdrg_params_init(&preset, HDRG_PRESET_ANIME);
    TEST_ASSERT_TRUE(preset.colored_lights);
    hdrg_params_init(&preset, HDRG_PRESET_GAME);
    TEST_ASSERT_FALSE(preset.colored_lights);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set("colored-lights", 1.0, &params, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, convert_status(&params));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_get("colored-lights", &params, NULL, NULL, &value));
    TEST_ASSERT_TRUE(params.colored_lights && value == 1.0);
}

#ifndef HDRG_WITH_LCMS2
/* lcms2 なしでは「P3 PQ」プロファイルを作れないので単層 PQ とハイブリッドは拒否される */
static void test_pq_needs_lcms2(void)
{
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    encode.output_mode = HDRG_OUTPUT_PQ;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_ICC_UNSUPPORTED, convert_status(&params));

    encode.output_mode = HDRG_OUTPUT_HYBRID;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_ICC_UNSUPPORTED, convert_status(&params));
}
#endif

void setUp(void)
{
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    hdrg_display_caps_init(&display);
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_presets);
    RUN_TEST(test_raw_color_codes);
    RUN_TEST(test_null_params_uses_defaults);
    RUN_TEST(test_explicit_defaults_accepted);
    RUN_TEST(test_bad_struct_size_rejected);
    RUN_TEST(test_out_of_domain_strength);
    RUN_TEST(test_out_of_domain_scale_shift);
    RUN_TEST(test_out_of_domain_quality);
    RUN_TEST(test_out_of_domain_peak);
    RUN_TEST(test_out_of_domain_diffuse);
    RUN_TEST(test_out_of_domain_grading);
    RUN_TEST(test_grain_policy);
    RUN_TEST(test_prefilter_defaults);
    RUN_TEST(test_chroma_fit);
    RUN_TEST(test_upscaler);
    RUN_TEST(test_colored_lights);
    RUN_TEST(test_context_desc);
    RUN_TEST(test_pixel_limit);
    RUN_TEST(test_settings_by_name);
    RUN_TEST(test_settings_get_and_peak);
    RUN_TEST(test_settings_names);
    RUN_TEST(test_settings_switch_off);
    RUN_TEST(test_status_strings);
    RUN_TEST(test_output_sides);
#ifndef HDRG_WITH_LCMS2
    RUN_TEST(test_pq_needs_lcms2);
#endif

    return UNITY_END();
}
