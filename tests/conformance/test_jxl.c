/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <jxl/decode.h>
#include <jxl/gain_map.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "container.h"
#include "io/io.h"
#include "pipeline.h"

/*
 * JPEG XL (hdrg.jxl, the bundled libjxl): the three forms' signaling (the base's color and
 * intensity target, the jhgm gain map: its ISO 21496-1 metadata, the alternate's color, its
 * 8-bit grey codestream at the gain map's size; none for PQ), HYBRID by default; lossless
 * (quality 100) the decoded 16-bit base exactly the pipeline's rows; the same bytes whatever the
 * threads; the Exif carried upright; the writer's options' ranges
 */

#define TJ_W  192u
#define TJ_H  128u
#define TJ_ID "hdrg.jxl"

/* the Exif of test_orientation: a little-endian TIFF with orientation 6 */
static const uint8_t tj_exif6[] = {
    'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* a file decoded by libjxl: its header, color, base (16-bit R'G'B') and boxes */
typedef struct {
    JxlBasicInfo info;
    JxlColorEncoding color;
    uint16_t *pixels;
    const uint8_t *jhgm, *exif; /* the boxes' payloads in the file (NULL: none) */
    size_t jhgm_size, exif_size;
} tj_decoded_t;

static hdrg_context_t *g_ctx;
static uint8_t *g_pixels;

static void tj_raw(hdrg_raw_image_t *raw)
{
    memset(raw, 0, sizeof(*raw));
    raw->pixels = g_pixels;
    raw->width = TJ_W;
    raw->height = TJ_H;
    raw->bit_depth = 8;
    raw->stride = (size_t)TJ_W * 3u;
    raw->primaries = HDRG_PRIMARIES_BT709;
    raw->transfer = HDRG_TRANSFER_SRGB;
}

/* the JPEG XL writer of the context's conversions at quality */
static void tj_writer(hdrg_context_t *ctx, uint8_t quality)
{
    const char *opts[2];
    char q[32], gq[40];

    snprintf(q, sizeof(q), "quality=%u", (unsigned int)quality);
    snprintf(gq, sizeof(gq), "gainmap_quality=%u", (unsigned int)quality);
    opts[0] = q;
    opts[1] = gq;
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(ctx, TJ_ID, opts, 2));
}

static void tj_settings(uint8_t mode, hdrg_params_t *params, hdrg_encode_params_t *encode)
{
    hdrg_params_init(params, HDRG_PRESET_PHOTO);
    params->effort = HDRG_EFFORT_QUALITY;
    params->grain_policy = HDRG_GRAIN_PRESERVE;
    hdrg_encode_params_init(encode);
    encode->output_mode = mode;
}

static void tj_convert(hdrg_context_t *ctx, uint8_t mode, uint8_t quality, uint8_t **out,
                       size_t *size)
{
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_params_t params;

    tj_settings(mode, &params, &encode);
    tj_writer(ctx, quality);
    tj_raw(&raw);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK,
                             hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, out, size));
    TEST_ASSERT_TRUE(*size >= 12u && memcmp(*out + 4, "JXL \r\n\x87\n", 8) == 0);
}

/* the container's top-level box of type (its payload), NULL: none (a naked codestream too) */
static const uint8_t *tj_box(const uint8_t *data, size_t size, const char *type, size_t *length)
{
    uint64_t box;
    size_t at;

    *length = 0;

    if (size < 12u || memcmp(data + 4, "JXL ", 4) != 0) {
        return NULL;
    }

    for (at = 0; at + 8u <= size; at += (size_t)box) {
        box = ((uint64_t)data[at] << 24) | ((uint64_t)data[at + 1] << 16) |
              ((uint64_t)data[at + 2] << 8) | data[at + 3];

        /* 0: the box runs to the end of the file */
        box = (box == 0) ? (uint64_t)(size - at) : box;
        TEST_ASSERT_TRUE(box >= 8u && box <= size - at);

        if (memcmp(data + at + 4, type, 4) == 0) {
            *length = (size_t)box - 8u;

            return data + at + 8u;
        }
    }

    *length = 0;

    return NULL;
}

/* data decoded (its base's pixels when pixels) */
static void tj_decode(const uint8_t *data, size_t size, bool pixels, tj_decoded_t *out)
{
    const JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    JxlDecoderStatus status;
    JxlDecoder *dec;
    size_t bytes;

    memset(out, 0, sizeof(*out));
    dec = JxlDecoderCreate(NULL);
    TEST_ASSERT_NOT_NULL(dec);
    TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                               dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                                        (pixels ? JXL_DEC_FULL_IMAGE : 0)));
    TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data, size));
    JxlDecoderCloseInput(dec);

    for (;;) {
        status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_BASIC_INFO) {
            TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &out->info));
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS,
                                  JxlDecoderGetColorAsEncodedProfile(
                                      dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &out->color));

            if (!pixels) {
                break;
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS,
                                  JxlDecoderImageOutBufferSize(dec, &format, &bytes));
            out->pixels = (uint16_t *)malloc(bytes);
            TEST_ASSERT_NOT_NULL(out->pixels);
            TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS,
                                  JxlDecoderSetImageOutBuffer(dec, &format, out->pixels, bytes));
        } else if (status == JXL_DEC_FULL_IMAGE) {
            continue;
        } else {
            TEST_ASSERT_EQUAL_INT(JXL_DEC_SUCCESS, status);

            break;
        }
    }

    JxlDecoderDestroy(dec);
    out->jhgm = tj_box(data, size, "jhgm", &out->jhgm_size);
    out->exif = tj_box(data, size, "Exif", &out->exif_size);
}

/* the pipeline's 16-bit rows of the conversion */
static void tj_reference(uint8_t mode, uint16_t *rows)
{
    hdrg_display_caps_t display, caps;
    hdrg_encode_params_t encode;
    hdrg_container_t container;
    hdrg_pipeline_t pipeline;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_color_t color;
    uint32_t y0;

    tj_settings(mode, &params, &encode);
    tj_writer(g_ctx, 100);
    tj_raw(&raw);
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&display);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_container_select(g_ctx, &encode, &container));
    hdrg_container_caps(&container, &display, &encode, &caps);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK,
                             hdrg_pipeline_open(g_ctx, &raw, &color, &params, &encode, &caps, 0,
                                                HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK,
                             hdrg_container_prepare(&container, g_ctx, &pipeline, &encode));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_pipeline_frame(g_ctx, &pipeline, raw.pixels));

    for (y0 = 0; y0 < TJ_H; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_UINT32(
            HDRG_OK,
            hdrg_pipeline_rows(g_ctx, &pipeline, y0, HDRG_MIN(HDRG_APPLY_BAND_ROWS, TJ_H - y0),
                               (uint8_t *)(void *)(rows + (size_t)y0 * TJ_W * 3u)));
    }

    hdrg_pipeline_end(g_ctx, &pipeline);
    hdrg_pipeline_close(g_ctx, &pipeline);
}

/* the jhgm bundle's gain map: its codestream an 8-bit grey picture of width x height */
static void tj_expect_gainmap(const tj_decoded_t *decoded, uint32_t width, uint32_t height,
                              JxlTransferFunction alternate)
{
    JxlGainMapBundle bundle;
    tj_decoded_t map;
    size_t read;

    TEST_ASSERT_NOT_NULL(decoded->jhgm);
    memset(&bundle, 0, sizeof(bundle));
    TEST_ASSERT_TRUE(JxlGainMapReadBundle(&bundle, decoded->jhgm, decoded->jhgm_size, &read));
    TEST_ASSERT_EQUAL_size_t(decoded->jhgm_size, read);
    TEST_ASSERT_EQUAL_UINT8(0, bundle.jhgm_version);
    TEST_ASSERT_TRUE(bundle.gain_map_metadata_size > 8u);
    TEST_ASSERT_TRUE(bundle.has_color_encoding);
    TEST_ASSERT_EQUAL_INT(alternate, bundle.color_encoding.transfer_function);

    tj_decode(bundle.gain_map, bundle.gain_map_size, false, &map);
    TEST_ASSERT_EQUAL_UINT32(width, map.info.xsize);
    TEST_ASSERT_EQUAL_UINT32(height, map.info.ysize);
    TEST_ASSERT_EQUAL_UINT32(1, map.info.num_color_channels);
    TEST_ASSERT_EQUAL_UINT32(8, map.info.bits_per_sample);
}

static void test_signaling(void)
{
    static const uint8_t modes[4] = {HDRG_OUTPUT_AUTO, HDRG_OUTPUT_GAINMAP, HDRG_OUTPUT_HYBRID,
                                     HDRG_OUTPUT_PQ};
    tj_decoded_t decoded;
    uint8_t m, *out;
    size_t size;

    for (m = 0; m < 4u; m++) {
        tj_convert(g_ctx, modes[m], 90, &out, &size);
        tj_decode(out, size, false, &decoded);

        TEST_ASSERT_EQUAL_UINT32(TJ_W, decoded.info.xsize);
        TEST_ASSERT_EQUAL_UINT32(TJ_H, decoded.info.ysize);
        TEST_ASSERT_EQUAL_UINT32(16, decoded.info.bits_per_sample);
        TEST_ASSERT_EQUAL_UINT32(3, decoded.info.num_color_channels);
        TEST_ASSERT_EQUAL_INT(JXL_COLOR_SPACE_RGB, decoded.color.color_space);
        TEST_ASSERT_EQUAL_INT(JXL_WHITE_POINT_D65, decoded.color.white_point);

        if (modes[m] == HDRG_OUTPUT_GAINMAP) {
            /* PHOTO expands the gamut: a Display P3 base, its gain map up to the HDR */
            TEST_ASSERT_EQUAL_INT(JXL_PRIMARIES_P3, decoded.color.primaries);
            TEST_ASSERT_EQUAL_INT(JXL_TRANSFER_FUNCTION_SRGB, decoded.color.transfer_function);
            tj_expect_gainmap(&decoded, TJ_W / 2u, TJ_H / 2u, JXL_TRANSFER_FUNCTION_PQ);
        } else {
            /* the PQ base (HYBRID the default): BT.2100, its intensity target the peak */
            TEST_ASSERT_EQUAL_INT(JXL_PRIMARIES_2100, decoded.color.primaries);
            TEST_ASSERT_EQUAL_INT(JXL_TRANSFER_FUNCTION_PQ, decoded.color.transfer_function);
            TEST_ASSERT_TRUE(decoded.info.intensity_target > 400.0f &&
                             decoded.info.intensity_target < 10000.0f);
        }

        if (modes[m] == HDRG_OUTPUT_PQ) {
            TEST_ASSERT_NULL(decoded.jhgm);
        } else if (modes[m] != HDRG_OUTPUT_GAINMAP) {
            tj_expect_gainmap(&decoded, TJ_W / 2u, TJ_H / 2u, JXL_TRANSFER_FUNCTION_SRGB);
        }

        TEST_ASSERT_NULL(decoded.exif);
        hdrg_free(out);
    }
}

/* lossless: the decoded base is the pipeline's rows */
static void test_lossless(void)
{
    static const uint8_t modes[3] = {HDRG_OUTPUT_GAINMAP, HDRG_OUTPUT_HYBRID, HDRG_OUTPUT_PQ};
    tj_decoded_t decoded;
    uint16_t *rows;
    uint8_t m, *out;
    size_t size;

    rows = (uint16_t *)malloc((size_t)TJ_W * TJ_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(rows);

    for (m = 0; m < 3u; m++) {
        tj_reference(modes[m], rows);
        tj_convert(g_ctx, modes[m], 100, &out, &size);
        tj_decode(out, size, true, &decoded);
        TEST_ASSERT_NOT_NULL(decoded.pixels);
        TEST_ASSERT_TRUE(decoded.info.uses_original_profile);
        TEST_ASSERT_EQUAL_MEMORY(rows, decoded.pixels, (size_t)TJ_W * TJ_H * 3u * sizeof(uint16_t));
        free(decoded.pixels);
        hdrg_free(out);
    }

    free(rows);
}

static void test_threads_same_bytes(void)
{
    static const uint8_t modes[2] = {HDRG_OUTPUT_HYBRID, HDRG_OUTPUT_GAINMAP};
    hdrg_context_t *one;
    uint8_t m, *a, *b;
    size_t a_size, b_size;

    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_context(&one, NULL, 1));

    for (m = 0; m < 2u; m++) {
        tj_convert(one, modes[m], 90, &a, &a_size);
        tj_convert(g_ctx, modes[m], 90, &b, &b_size);
        TEST_ASSERT_EQUAL_size_t(a_size, b_size);
        TEST_ASSERT_EQUAL_MEMORY(a, b, a_size);
        hdrg_free(a);
        hdrg_free(b);
    }

    hdrg_context_destroy(one);
}

/* the Exif of a JPEG input: in its box (after a zero offset), its orientation 1 (upright) */
static void test_exif_carried(void)
{
    hdrg_encode_params_t encode;
    hdrg_allocator_t alloc;
    tj_decoded_t decoded;
    uint8_t *jpeg, *out;
    size_t jpeg_size, size;

    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_jpeg_rgb(&alloc, g_pixels, TJ_W, TJ_H,
                                                         (size_t)TJ_W * 3u, 95, NULL, 0, tj_exif6,
                                                         sizeof(tj_exif6), &jpeg, &jpeg_size));

    hdrg_encode_params_init(&encode);
    tj_writer(g_ctx, 90);
    TEST_ASSERT_EQUAL_UINT32(
        HDRG_OK, hdrg_convert(g_ctx, jpeg, jpeg_size, NULL, NULL, &encode, &out, &size));

    tj_decode(out, size, false, &decoded);
    TEST_ASSERT_EQUAL_UINT32(TJ_H, decoded.info.xsize);
    TEST_ASSERT_EQUAL_UINT32(TJ_W, decoded.info.ysize);
    TEST_ASSERT_EQUAL_INT(JXL_ORIENT_IDENTITY, decoded.info.orientation);
    TEST_ASSERT_NOT_NULL(decoded.exif);
    TEST_ASSERT_TRUE(decoded.exif_size > 4u);
    TEST_ASSERT_EQUAL_UINT8(0,
                            decoded.exif[0] | decoded.exif[1] | decoded.exif[2] | decoded.exif[3]);
    TEST_ASSERT_EQUAL_UINT16(1, hdrg_exif_orientation(decoded.exif + 4, decoded.exif_size - 4u));

    hdrg_free(out);
    hdrg_mem_free(&alloc, jpeg);
}

/* the writer's options checked: a quality, an effort past its range */
static void test_settings_ranges(void)
{
    static const char *const quality[] = {"quality=101"}, *const effort[] = {"effort=11"},
                             *const slowest[] = {"effort=1"};

    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TJ_ID, quality, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TJ_ID, effort, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(g_ctx, TJ_ID, slowest, 1));
}

void setUp(void)
{
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_context(&g_ctx, NULL, 4));
    g_pixels = hdrg_test_image(TJ_W, TJ_H);
    TEST_ASSERT_NOT_NULL(g_pixels);
}

void tearDown(void)
{
    hdrg_context_destroy(g_ctx);
    g_ctx = NULL;
    free(g_pixels);
    g_pixels = NULL;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_signaling);
    RUN_TEST(test_lossless);
    RUN_TEST(test_threads_same_bytes);
    RUN_TEST(test_exif_carried);
    RUN_TEST(test_settings_ranges);

    return UNITY_END();
}
