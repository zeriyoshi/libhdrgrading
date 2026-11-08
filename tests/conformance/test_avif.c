/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <avif/avif.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "container.h"
#include "core/yuv.h"
#include "io/io.h"
#include "pipeline.h"
#include "host/codec_host.h"

/*
 * AVIF: the three forms' signaling (10-bit
 * full-range Y'CbCr, the CICP, the gain map's direction, the PQ bases'
 * CLLI); at the lossless quantizer the decoded planes are exactly the
 * pipeline's rows through core/yuv (4:4:4 and 4:2:0, an odd size too) and
 * the CLLI its light; the gain map recovers the HDR and the SDR as the
 * JPEG's does; the same bytes whatever the conversion's threads on one encoder
 * thread (threads 1: libaom's bytes may change with its threads), an AVIF on
 * the default's; the input (libaom's
 * decoder): an SDR AVIF's pixels, its irot as the orientation, the base of a
 * gain-map AVIF, a PQ one rejected; the Exif carried upright; the settings'
 * ranges. The codec is the AVIF module's (src/plugins/avif, loaded by its path).
 */

#define TA_W   192u
#define TA_H   128u
#define TA_ID  "org.hdrgrading.avif"
#define TA_444 0u
#define TA_420 1u

/* the Exif of test_orientation: a little-endian TIFF with orientation 6 */
static const uint8_t ta_exif6[] = {
    'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static hdrg_context_t *g_ctx;
static uint8_t *g_pixels;
static uint32_t g_w, g_h;

static void ta_image(uint32_t width, uint32_t height)
{
    free(g_pixels);
    g_pixels = hdrg_test_image(width, height);
    TEST_ASSERT_NOT_NULL(g_pixels);
    g_w = width;
    g_h = height;
}

static void ta_raw(hdrg_raw_image_t *raw)
{
    memset(raw, 0, sizeof(*raw));
    raw->pixels = g_pixels;
    raw->width = g_w;
    raw->height = g_h;
    raw->bit_depth = 8;
    raw->stride = (size_t)g_w * 3u;
    raw->primaries = HDRG_PRIMARIES_BT709;
    raw->transfer = HDRG_TRANSFER_SRGB;
}

/* a context of threads with the AVIF module */
static void ta_context(hdrg_context_t **ctx, uint32_t threads)
{
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_context(ctx, NULL, threads));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_context_load_plugin(*ctx, HDRG_TEST_AVIF_MODULE));
}

/*
 * the AVIF writer of the context's conversions with its chroma and quality, on one thread: the
 * system's libaom's bytes may change with its threads (the writer's default the conversion's)
 */
static void ta_writer(hdrg_context_t *ctx, uint8_t subsampling, uint8_t quality)
{
    const char *opts[4];
    char q[32], gq[40];

    snprintf(q, sizeof(q), "quality=%u", (unsigned int)quality);
    snprintf(gq, sizeof(gq), "gainmap_quality=%u", (unsigned int)quality);
    opts[0] = q;
    opts[1] = gq;
    opts[2] = (subsampling == TA_420) ? "chroma=420" : "chroma=444";
    opts[3] = "threads=1";
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(ctx, TA_ID, opts, 4));
}

/* an AVIF by its leading ftyp's brand */
static bool ta_is_avif(const uint8_t *data, size_t size)
{
    return size >= 12 && memcmp(data + 4, "ftypavif", 8) == 0;
}

/* the settings of a still conversion as hdrg_convert resolves them */
static void ta_settings(uint8_t mode, hdrg_params_t *params, hdrg_encode_params_t *encode)
{
    hdrg_params_init(params, HDRG_PRESET_PHOTO);
    params->effort = HDRG_EFFORT_QUALITY;
    params->grain_policy = HDRG_GRAIN_PRESERVE;
    hdrg_encode_params_init(encode);
    encode->output_mode = mode;
}

static void ta_convert(hdrg_context_t *ctx, uint8_t mode, uint8_t subsampling, uint8_t quality,
                       uint8_t **out, size_t *size)
{
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_params_t params;

    ta_settings(mode, &params, &encode);
    ta_writer(ctx, subsampling, quality);
    ta_raw(&raw);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK,
                             hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, out, size));
    TEST_ASSERT_TRUE(ta_is_avif(*out, *size));
}

/* data through the context's readers (the module's for an AVIF), max_pixels the limit */
static hdrg_status_t ta_read(const uint8_t *data, size_t size, uint64_t max_pixels,
                             hdrg_decoded_t *out)
{
    return hdrg_codec_read(&g_ctx->host, data, size, max_pixels, HDRG_CHROMA_FIT_LINEAR, out);
}

/* the file decoded by libavif, its gain map too */
static avifDecoder *ta_decode(const uint8_t *data, size_t size)
{
    avifDecoder *decoder;

    decoder = avifDecoderCreate();
    TEST_ASSERT_NOT_NULL(decoder);
    decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifDecoderSetIOMemory(decoder, data, size));
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifDecoderParse(decoder));
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifDecoderNextImage(decoder));

    return decoder;
}

/* the pipeline's 16-bit rows of the conversion (and the light its CLLI is rounded from) */
static void ta_reference(uint8_t mode, uint16_t *rows, float *max_nits, float *avg_nits)
{
    hdrg_display_caps_t display, caps;
    hdrg_encode_params_t encode;
    hdrg_container_t container;
    hdrg_pipeline_t pipeline;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_color_t color;
    uint32_t y0;

    ta_settings(mode, &params, &encode);
    ta_writer(g_ctx, TA_444, 100);
    ta_raw(&raw);
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

    for (y0 = 0; y0 < g_h; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_UINT32(
            HDRG_OK,
            hdrg_pipeline_rows(g_ctx, &pipeline, y0, HDRG_MIN(HDRG_APPLY_BAND_ROWS, g_h - y0),
                               (uint8_t *)(void *)(rows + (size_t)y0 * g_w * 3u)));
    }

    hdrg_pipeline_end(g_ctx, &pipeline);
    hdrg_pipeline_light(&pipeline, max_nits, avg_nits);
    hdrg_pipeline_close(g_ctx, &pipeline);
}

/* the decoded planes against the reference rows through core/yuv (the encoder's conversion) */
static void ta_expect_planes(const avifImage *image, const uint16_t *rows, uint8_t layout,
                             uint8_t matrix)
{
    hdrg_yuv_t yuv;
    uint32_t c, y, width, height;
    uint16_t *planes[3];

    memset(&yuv, 0, sizeof(yuv));
    width = (layout == HDRG_YUV_420) ? (g_w + 1u) / 2u : g_w;
    height = (layout == HDRG_YUV_420) ? (g_h + 1u) / 2u : g_h;

    for (c = 0; c < 3u; c++) {
        planes[c] = (uint16_t *)calloc((c == 0) ? (size_t)g_w * g_h : (size_t)width * height,
                                       sizeof(uint16_t));
        TEST_ASSERT_NOT_NULL(planes[c]);
        yuv.planes[c] = planes[c];
        yuv.strides[c] = ((c == 0) ? g_w : width) * sizeof(uint16_t);
    }

    yuv.width = g_w;
    yuv.height = g_h;
    yuv.layout = layout;
    yuv.matrix = matrix;
    yuv.bits = 10;
    yuv.siting = HDRG_CHROMA_LEFT;
    yuv.full_range = true;

    if (layout == HDRG_YUV_420) {
        hdrg_rgb16_to_yuv420(NULL, rows, 0, g_h, &yuv);
    } else {
        hdrg_rgb16_to_yuv444(NULL, rows, 0, g_h, &yuv);
    }

    for (c = 0; c < 3u; c++) {
        for (y = 0; y < ((c == 0) ? g_h : height); y++) {
            TEST_ASSERT_EQUAL_MEMORY(planes[c] + (size_t)y * ((c == 0) ? g_w : width),
                                     image->yuvPlanes[c] + (size_t)y * image->yuvRowBytes[c],
                                     ((c == 0) ? g_w : width) * sizeof(uint16_t));
        }

        free(planes[c]);
    }
}

/* lossless: the decoded planes are the rows' Y'CbCr, the CLLI their light */
static void ta_lossless(uint8_t mode, uint8_t subsampling)
{
    const avifImage *image;
    avifDecoder *decoder;
    uint16_t *rows;
    uint8_t *out;
    size_t size;
    float max_nits, avg_nits;

    rows = (uint16_t *)malloc((size_t)g_w * g_h * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(rows);
    ta_reference(mode, rows, &max_nits, &avg_nits);
    ta_convert(g_ctx, mode, subsampling, 100, &out, &size);

    decoder = ta_decode(out, size);
    image = decoder->image;
    TEST_ASSERT_EQUAL_UINT32(g_w, image->width);
    TEST_ASSERT_EQUAL_UINT32(g_h, image->height);
    TEST_ASSERT_EQUAL_UINT32(10, image->depth);
    ta_expect_planes(image, rows, (subsampling == TA_420) ? HDRG_YUV_420 : HDRG_YUV_444,
                     (mode == HDRG_OUTPUT_GAINMAP) ? HDRG_MATRIX_BT601 : HDRG_MATRIX_BT2020);

    if (mode != HDRG_OUTPUT_GAINMAP) {
        TEST_ASSERT_TRUE(max_nits > 400.0f);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(max_nits + 0.5f), image->clli.maxCLL);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(avg_nits + 0.5f), image->clli.maxPALL);
    }

    avifDecoderDestroy(decoder);
    hdrg_free(out);
    free(rows);
}

static void test_signaling(void)
{
    const avifImage *image;
    const avifGainMap *map;
    static const uint8_t modes[3] = {HDRG_OUTPUT_GAINMAP, HDRG_OUTPUT_HYBRID, HDRG_OUTPUT_PQ};
    avifDecoder *decoder;
    uint8_t m, *out;
    size_t size;

    for (m = 0; m < 3u; m++) {
        ta_convert(g_ctx, modes[m], TA_444, 90, &out, &size);
        decoder = ta_decode(out, size);
        image = decoder->image;
        map = image->gainMap;

        TEST_ASSERT_EQUAL_UINT32(10, image->depth);
        TEST_ASSERT_EQUAL_INT(AVIF_PIXEL_FORMAT_YUV444, image->yuvFormat);
        TEST_ASSERT_EQUAL_INT(AVIF_RANGE_FULL, image->yuvRange);
        TEST_ASSERT_EQUAL_UINT32(0, image->icc.size);
        TEST_ASSERT_EQUAL_UINT32(AVIF_TRANSFORM_NONE, image->transformFlags);

        if (modes[m] == HDRG_OUTPUT_GAINMAP) {
            /* PHOTO expands the gamut: a Display P3 base, its gain map up to the HDR */
            TEST_ASSERT_EQUAL_INT(AVIF_COLOR_PRIMARIES_SMPTE432, image->colorPrimaries);
            TEST_ASSERT_EQUAL_INT(AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                                  image->transferCharacteristics);
            TEST_ASSERT_EQUAL_INT(AVIF_MATRIX_COEFFICIENTS_BT601, image->matrixCoefficients);
            TEST_ASSERT_EQUAL_UINT16(0, image->clli.maxCLL);
            TEST_ASSERT_NOT_NULL(map);
            TEST_ASSERT_EQUAL_UINT32(0, map->baseHdrHeadroom.n);
            TEST_ASSERT_TRUE(map->alternateHdrHeadroom.n > 0);
            TEST_ASSERT_EQUAL_INT(AVIF_TRANSFER_CHARACTERISTICS_PQ,
                                  map->altTransferCharacteristics);
        } else {
            TEST_ASSERT_EQUAL_INT(AVIF_COLOR_PRIMARIES_BT2020, image->colorPrimaries);
            TEST_ASSERT_EQUAL_INT(AVIF_TRANSFER_CHARACTERISTICS_PQ, image->transferCharacteristics);
            TEST_ASSERT_EQUAL_INT(AVIF_MATRIX_COEFFICIENTS_BT2020_NCL, image->matrixCoefficients);
            TEST_ASSERT_TRUE(image->clli.maxCLL > 400);
            TEST_ASSERT_TRUE(image->clli.maxPALL > 0 && image->clli.maxPALL < image->clli.maxCLL);
        }

        if (modes[m] == HDRG_OUTPUT_HYBRID) {
            TEST_ASSERT_NOT_NULL(map);
            TEST_ASSERT_TRUE(map->baseHdrHeadroom.n > 0);
            TEST_ASSERT_EQUAL_UINT32(0, map->alternateHdrHeadroom.n);
            TEST_ASSERT_TRUE(map->gainMapMin[0].n < 0);
            TEST_ASSERT_EQUAL_INT(AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                                  map->altTransferCharacteristics);
        }

        if (modes[m] == HDRG_OUTPUT_PQ) {
            TEST_ASSERT_NULL(map);
        } else {
            /* 8-bit monochrome at half the size (gainmap_scale_shift 1) */
            TEST_ASSERT_NOT_NULL(map->image);
            TEST_ASSERT_EQUAL_UINT32(TA_W / 2u, map->image->width);
            TEST_ASSERT_EQUAL_UINT32(TA_H / 2u, map->image->height);
            TEST_ASSERT_EQUAL_UINT32(8, map->image->depth);
            TEST_ASSERT_EQUAL_INT(AVIF_PIXEL_FORMAT_YUV400, map->image->yuvFormat);
            TEST_ASSERT_TRUE(map->useBaseColorSpace);
        }

        avifDecoderDestroy(decoder);
        hdrg_free(out);
    }
}

static void test_lossless_444(void)
{
    ta_lossless(HDRG_OUTPUT_GAINMAP, TA_444);
    ta_lossless(HDRG_OUTPUT_HYBRID, TA_444);
    ta_lossless(HDRG_OUTPUT_PQ, TA_444);
}

static void test_lossless_420(void)
{
    const avifImage *image;
    avifDecoder *decoder;
    uint8_t *out;
    size_t size;

    ta_lossless(HDRG_OUTPUT_GAINMAP, TA_420);
    ta_lossless(HDRG_OUTPUT_PQ, TA_420);

    /* an odd size: the last row and column's chroma alone */
    ta_image(TA_W - 1u, TA_H - 1u);
    ta_lossless(HDRG_OUTPUT_HYBRID, TA_420);

    ta_convert(g_ctx, HDRG_OUTPUT_PQ, TA_420, 90, &out, &size);
    decoder = ta_decode(out, size);
    image = decoder->image;
    TEST_ASSERT_EQUAL_INT(AVIF_PIXEL_FORMAT_YUV420, image->yuvFormat);
    TEST_ASSERT_EQUAL_INT(AVIF_CHROMA_SAMPLE_POSITION_VERTICAL, image->yuvChromaSamplePosition);
    avifDecoderDestroy(decoder);
    hdrg_free(out);
}

/* the base's white specular times the gain map's largest gain reaches the declared headroom */
static void test_gainmap_recovers(void)
{
    const avifImage *image;
    const avifGainMap *map;
    avifDecoder *decoder;
    uint32_t x, y;
    uint16_t base_max;
    uint8_t gm_max, *out;
    size_t size;
    float headroom, recon;

    ta_convert(g_ctx, HDRG_OUTPUT_GAINMAP, TA_444, 90, &out, &size);
    decoder = ta_decode(out, size);
    image = decoder->image;
    map = image->gainMap;
    headroom = (float)map->alternateHdrHeadroom.n / (float)map->alternateHdrHeadroom.d;
    TEST_ASSERT_TRUE(headroom > 0.5f);

    base_max = 0;
    for (y = 0; y < image->height; y++) {
        for (x = 0; x < image->width; x++) {
            base_max = (uint16_t)HDRG_TEST_MAX(
                base_max,
                ((const uint16_t *)(const void *)(image->yuvPlanes[AVIF_CHAN_Y] +
                                                  (size_t)y * image->yuvRowBytes[AVIF_CHAN_Y]))[x]);
        }
    }

    gm_max = 0;
    for (y = 0; y < map->image->height; y++) {
        for (x = 0; x < map->image->width; x++) {
            gm_max = (uint8_t)HDRG_TEST_MAX(
                gm_max,
                map->image->yuvPlanes[AVIF_CHAN_Y][(size_t)y * map->image->yuvRowBytes[0] + x]);
        }
    }

    TEST_ASSERT_TRUE(base_max >= 1000);
    TEST_ASSERT_TRUE(gm_max >= 250);
    recon = exp2f((float)gm_max / 255.0f * headroom);
    TEST_ASSERT_FLOAT_WITHIN(0.15f * exp2f(headroom), exp2f(headroom), recon);

    avifDecoderDestroy(decoder);
    hdrg_free(out);
}

/* libavif's tone map of the hybrid at headroom 0 is its SDR alternate: white where the base peaks
 */
static void test_hybrid_recovers_sdr(void)
{
    const avifImage *image;
    avifDiagnostics diag;
    avifDecoder *decoder;
    avifRGBImage rgb;
    uint32_t x, y, top;
    uint8_t *out;
    size_t size;

    ta_convert(g_ctx, HDRG_OUTPUT_HYBRID, TA_444, 90, &out, &size);
    decoder = ta_decode(out, size);
    image = decoder->image;

    memset(&rgb, 0, sizeof(rgb));
    rgb.width = image->width;
    rgb.height = image->height;
    rgb.depth = 8;
    rgb.format = AVIF_RGB_FORMAT_RGB;
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifRGBImageAllocatePixels(&rgb));
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifImageApplyGainMap(image, image->gainMap, 0.0f,
                                                                AVIF_COLOR_PRIMARIES_BT709,
                                                                AVIF_TRANSFER_CHARACTERISTICS_SRGB,
                                                                &rgb, NULL, &diag));

    /* the specular dot (hdrg_test_image's, at an eighth of the size) is SDR white again */
    top = 0;
    for (y = TA_H / 8u; y < TA_H / 8u + 3u; y++) {
        for (x = TA_W / 8u; x < TA_W / 8u + 3u; x++) {
            top = HDRG_TEST_MAX(top, rgb.pixels[(size_t)y * rgb.rowBytes + (size_t)x * 3u + 1u]);
        }
    }

    TEST_ASSERT_TRUE(top >= 235);

    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    hdrg_free(out);
}

/*
 * the encoder on one thread (threads 1): the same bytes whatever the conversion's threads; on the
 * conversion's threads (the default) an AVIF libavif reads, its size the frame's
 */
static void test_threads_same_bytes(void)
{
    static const uint8_t modes[2] = {HDRG_OUTPUT_GAINMAP, HDRG_OUTPUT_PQ};
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_context_t *one;
    avifDecoder *decoder;
    uint8_t m, *a, *b;
    size_t a_size, b_size;

    ta_context(&one, 1);

    for (m = 0; m < 2u; m++) {
        ta_convert(one, modes[m], TA_444, 90, &a, &a_size);
        ta_convert(g_ctx, modes[m], TA_444, 90, &b, &b_size);
        TEST_ASSERT_EQUAL_size_t(a_size, b_size);
        TEST_ASSERT_EQUAL_MEMORY(a, b, a_size);
        hdrg_free(a);
        hdrg_free(b);
    }

    hdrg_context_destroy(one);

    ta_settings(HDRG_OUTPUT_GAINMAP, &params, &encode);
    ta_raw(&raw);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(g_ctx, TA_ID, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK,
                             hdrg_convert_raw(g_ctx, &raw, &params, NULL, &encode, &a, &a_size));
    TEST_ASSERT_TRUE(ta_is_avif(a, a_size));
    decoder = ta_decode(a, a_size);
    TEST_ASSERT_EQUAL_UINT32(g_w, decoder->image->width);
    TEST_ASSERT_EQUAL_UINT32(g_h, decoder->image->height);
    avifDecoderDestroy(decoder);
    hdrg_free(a);
}

/* an SDR AVIF of libavif's: lossless BT.601 4:4:4, rotated a quarter turn (irot 1) */
static void ta_sdr_avif(uint8_t format, avifRWData *out)
{
    avifEncoder *encoder;
    avifImage *image;
    avifRGBImage rgb;

    image = avifImageCreate(g_w, g_h, 8, (avifPixelFormat)format);
    TEST_ASSERT_NOT_NULL(image);
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    image->yuvRange = AVIF_RANGE_FULL;
    image->transformFlags = AVIF_TRANSFORM_IROT;
    image->irot.angle = 1;

    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.pixels = g_pixels;
    rgb.rowBytes = g_w * 3u;
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifImageRGBToYUV(image, &rgb));

    encoder = avifEncoderCreate();
    TEST_ASSERT_NOT_NULL(encoder);
    encoder->quality = 100;
    encoder->speed = 9;
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK, avifEncoderWrite(encoder, image, out));
    avifEncoderDestroy(encoder);
    avifImageDestroy(image);
}

static void test_avif_input(void)
{
    hdrg_allocator_t alloc;
    hdrg_decoded_t decoded;
    avifRWData data = AVIF_DATA_EMPTY;
    uint32_t i, worst;
    uint16_t *pixels;
    uint8_t *out;
    int32_t d;
    size_t size;

    alloc = hdrg_test_allocator();
    ta_sdr_avif(AVIF_PIXEL_FORMAT_YUV444, &data);
    TEST_ASSERT_TRUE(ta_is_avif(data.data, data.size));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, ta_read(data.data, data.size, UINT64_MAX, &decoded));
    TEST_ASSERT_EQUAL_UINT32(g_w, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(16, decoded.bit_depth);
    /* irot 1 (a quarter turn anti-clockwise) is the EXIF orientation 8 */
    TEST_ASSERT_EQUAL_UINT16(8, decoded.orientation);
    TEST_ASSERT_TRUE(hdrg_color_within(&decoded.color, HDRG_GAMUT_SRGB));

    /* the 8-bit Y'CbCr's rounding alone */
    pixels = (uint16_t *)(void *)decoded.pixels;
    worst = 0;

    for (i = 0; i < g_w * g_h * 3u; i++) {
        d = (int32_t)pixels[i] - (int32_t)g_pixels[i] * 257;
        worst = HDRG_TEST_MAX(worst, (uint32_t)abs(d));
    }

    TEST_ASSERT_TRUE(worst <= 2u * 257u);
    hdrg_decoded_free(&g_ctx->alloc, &decoded);

    /* converted upright: the output is the rotated size */
    TEST_ASSERT_EQUAL_UINT32(
        HDRG_OK, hdrg_convert(g_ctx, data.data, data.size, NULL, NULL, NULL, &out, &size));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_decode_any(&alloc, NULL, out, size, UINT64_MAX,
                                                      HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT32(g_h, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(g_w, decoded.height);
    hdrg_decoded_free(&alloc, &decoded);
    hdrg_free(out);
    avifRWDataFree(&data);

    /* 4:2:0 through core/yuv's chroma upsampling */
    ta_sdr_avif(AVIF_PIXEL_FORMAT_YUV420, &data);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, ta_read(data.data, data.size, UINT64_MAX, &decoded));
    TEST_ASSERT_EQUAL_UINT32(g_h, decoded.height);
    hdrg_decoded_free(&g_ctx->alloc, &decoded);
    avifRWDataFree(&data);
}

/* a gain-map AVIF reads as its SDR base; a PQ one is not an SDR source */
static void test_own_outputs_as_input(void)
{
    hdrg_decoded_t decoded;
    uint8_t *out;
    size_t size;

    ta_convert(g_ctx, HDRG_OUTPUT_GAINMAP, TA_444, 90, &out, &size);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, ta_read(out, size, UINT64_MAX, &decoded));
    TEST_ASSERT_EQUAL_UINT32(g_w, decoded.width);
    TEST_ASSERT_FALSE(hdrg_color_within(&decoded.color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&decoded.color, HDRG_GAMUT_P3));
    hdrg_decoded_free(&g_ctx->alloc, &decoded);
    hdrg_free(out);

    ta_convert(g_ctx, HDRG_OUTPUT_PQ, TA_444, 90, &out, &size);
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_ICC_UNSUPPORTED, ta_read(out, size, UINT64_MAX, &decoded));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_LIMIT,
                             ta_read(out, size, (uint64_t)g_w * g_h - 1u, &decoded));
    hdrg_free(out);
}

/* a JPEG's Exif (orientation 6) goes into the AVIF upright: orientation 1, no irot */
static void test_exif_carried(void)
{
    const avifImage *image;
    hdrg_encode_params_t encode;
    hdrg_allocator_t alloc;
    avifDecoder *decoder;
    uint8_t *jpeg, *out;
    size_t offset, jpeg_size, size;

    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_jpeg_rgb(&alloc, g_pixels, g_w, g_h,
                                                         (size_t)g_w * 3u, 95, NULL, 0, ta_exif6,
                                                         sizeof(ta_exif6), &jpeg, &jpeg_size));

    hdrg_encode_params_init(&encode);
    ta_writer(g_ctx, TA_444, 90);
    TEST_ASSERT_EQUAL_UINT32(
        HDRG_OK, hdrg_convert(g_ctx, jpeg, jpeg_size, NULL, NULL, &encode, &out, &size));

    decoder = ta_decode(out, size);
    image = decoder->image;
    TEST_ASSERT_EQUAL_UINT32(g_h, image->width);
    TEST_ASSERT_EQUAL_UINT32(g_w, image->height);
    TEST_ASSERT_EQUAL_UINT32(AVIF_TRANSFORM_NONE,
                             image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR));
    TEST_ASSERT_TRUE(image->exif.size > 0);
    TEST_ASSERT_EQUAL_INT(AVIF_RESULT_OK,
                          avifGetExifTiffHeaderOffset(image->exif.data, image->exif.size, &offset));
    TEST_ASSERT_EQUAL_UINT16(
        1, hdrg_exif_orientation(image->exif.data + offset, image->exif.size - offset));

    avifDecoderDestroy(decoder);
    hdrg_free(out);
    hdrg_mem_free(&alloc, jpeg);
}

/* the writer's options checked (an unknown id, a quality or speed past its range, a chroma) */
static void test_settings_ranges(void)
{
    static const char *const quality[] = {"quality=101"}, *const speed[] = {"speed=11"},
                             *const chroma[] = {"chroma=422"};
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    uint8_t *out;
    size_t size;

    ta_raw(&raw);
    hdrg_encode_params_init(&encode);
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_NOT_FOUND,
                             hdrg_test_writer(g_ctx, "org.hdrgrading.nonesuch", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TA_ID, quality, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TA_ID, speed, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(g_ctx, TA_ID, chroma, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG,
                             hdrg_convert_raw(g_ctx, &raw, NULL, NULL, &encode, &out, &size));
}

void setUp(void)
{
    ta_context(&g_ctx, 4);
    ta_image(TA_W, TA_H);
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
    RUN_TEST(test_lossless_444);
    RUN_TEST(test_lossless_420);
    RUN_TEST(test_gainmap_recovers);
    RUN_TEST(test_hybrid_recovers_sdr);
    RUN_TEST(test_threads_same_bytes);
    RUN_TEST(test_avif_input);
    RUN_TEST(test_own_outputs_as_input);
    RUN_TEST(test_exif_carried);
    RUN_TEST(test_settings_ranges);

    return UNITY_END();
}
