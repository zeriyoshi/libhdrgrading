/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <libheif/heif.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#endif

#include "../test_jpeg.h"
#include "../test_util.h"
#include "container.h"
#include "core/yuv.h"
#include "io/io.h"
#include "pipeline.h"
#include "host/codec_host.h"

/*
 * HEIF (HDRG_WITH_HEIF): the three forms
 * read back by libheif (10-bit 4:2:0 Main10, the nclx, the CLLI); the boxes
 * our muxer writes around libheif's HEVC (the 'tmap' brand, the hidden gain
 * map the input of a 'tmap' item that an 'altr' group puts before the base,
 * its ISO 21496-1 payload, Apple's auxiliary gain map and XMP in GAINMAP
 * alone); the base close to the pipeline's rows; the gain map recovering the
 * declared headroom; the same bytes whatever the threads; no HEIF read (the
 * module writes alone: an SDR HEIF of libheif's and its own outputs are not
 * inputs); the Exif carried upright; on Apple, ImageIO's ISO and Apple gain
 * maps; the settings' ranges. The writer is the HEIF module's
 * (src/plugins/heif, loaded by its path).
 */

#define TH_W  192u
#define TH_H  128u
#define TH_ID "org.hdrgrading.heif"

/* the Exif of test_orientation: a little-endian TIFF with orientation 6 */
static const uint8_t th_exif6[] = {
    'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static hdrg_context_t *g_ctx;
static uint8_t *g_pixels;

static uint32_t th_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* the first box of type in [at, end) (no descent): its start and size */
static bool th_box(const uint8_t *data, size_t at, size_t end, const char *type, size_t *box,
                   size_t *size)
{
    size_t length;

    while (at + 8u <= end) {
        length = th_be32(data + at);

        if (length < 8u || length > end - at) {
            return false;
        }

        if (memcmp(data + at + 4, type, 4) == 0) {
            *box = at;
            *size = length;

            return true;
        }

        at += length;
    }

    return false;
}

/* needle somewhere in [data, data + size) */
static bool th_has(const uint8_t *data, size_t size, const char *needle)
{
    const size_t length = strlen(needle);
    size_t i;

    for (i = 0; i + length <= size; i++) {
        if (memcmp(data + i, needle, length) == 0) {
            return true;
        }
    }

    return false;
}

static void th_raw(hdrg_raw_image_t *raw)
{
    memset(raw, 0, sizeof(*raw));
    raw->pixels = g_pixels;
    raw->width = TH_W;
    raw->height = TH_H;
    raw->bit_depth = 8;
    raw->stride = (size_t)TH_W * 3u;
    raw->primaries = HDRG_PRIMARIES_BT709;
    raw->transfer = HDRG_TRANSFER_SRGB;
}

/* a context of threads with the HEIF module, its writer the conversions' */
static void th_context(hdrg_context_t **ctx, uint32_t threads)
{
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_context(ctx, NULL, threads));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_context_load_plugin(*ctx, HDRG_TEST_HEIF_MODULE));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(*ctx, TH_ID, NULL, 0));
}

/* a HEIF of libheif's (its major brand: 'heix', the 10-bit HEVC's) */
static bool th_is_heif(const uint8_t *data, size_t size)
{
    return size >= 12 &&
           (memcmp(data + 4, "ftypheix", 8) == 0 || memcmp(data + 4, "ftypheic", 8) == 0);
}

static void th_settings(uint8_t mode, hdrg_params_t *params, hdrg_encode_params_t *encode)
{
    hdrg_params_init(params, HDRG_PRESET_PHOTO);
    params->effort = HDRG_EFFORT_QUALITY;
    params->grain_policy = HDRG_GRAIN_PRESERVE;
    hdrg_encode_params_init(encode);
    encode->output_mode = mode;
}

/* hdrg_convert_raw into a HEIF; false: libheif has no HEVC encoder (a system one without x265) */
static bool th_convert(hdrg_context_t *ctx, uint8_t mode, uint8_t **out, size_t *size)
{
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_status_t status;

    th_settings(mode, &params, &encode);
    th_raw(&raw);
    status = hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, out, size);

    if (status == HDRG_ERR_UNSUPPORTED) {
        return false;
    }

    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, status);
    TEST_ASSERT_TRUE(th_is_heif(*out, *size));

    return true;
}

#define TH_CONVERT(ctx, mode, out, size)                                                           \
    do {                                                                                           \
        if (!th_convert((ctx), (mode), (out), (size))) {                                           \
            TEST_IGNORE_MESSAGE("libheif has no HEVC encoder");                                    \
        }                                                                                          \
    } while (0)

/* the pipeline's 16-bit rows of the conversion */
static void th_reference(uint8_t mode, uint16_t *rows)
{
    hdrg_display_caps_t display, caps;
    hdrg_encode_params_t encode;
    hdrg_container_t container;
    hdrg_pipeline_t pipeline;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_color_t color;
    uint32_t y0;

    th_settings(mode, &params, &encode);
    th_raw(&raw);
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

    for (y0 = 0; y0 < TH_H; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_UINT32(
            HDRG_OK,
            hdrg_pipeline_rows(g_ctx, &pipeline, y0, HDRG_MIN(HDRG_APPLY_BAND_ROWS, TH_H - y0),
                               (uint8_t *)(void *)(rows + (size_t)y0 * TH_W * 3u)));
    }

    hdrg_pipeline_end(g_ctx, &pipeline);
    hdrg_pipeline_close(g_ctx, &pipeline);
}

/* libheif's primary handle of a file (and its context to free) */
static struct heif_image_handle *th_primary(const uint8_t *data, size_t size,
                                            struct heif_context **ctx)
{
    struct heif_image_handle *handle;

    *ctx = heif_context_alloc();
    TEST_ASSERT_NOT_NULL(*ctx);
    TEST_ASSERT_EQUAL_INT(heif_error_Ok,
                          heif_context_read_from_memory_without_copy(*ctx, data, size, NULL).code);
    TEST_ASSERT_EQUAL_INT(heif_error_Ok, heif_context_get_primary_image_handle(*ctx, &handle).code);

    return handle;
}

static void test_signaling(void)
{
    static const uint8_t modes[3] = {HDRG_OUTPUT_GAINMAP, HDRG_OUTPUT_HYBRID, HDRG_OUTPUT_PQ};
    struct heif_color_profile_nclx *nclx;
    struct heif_image_handle *handle;
    struct heif_context *ctx;
    uint8_t m, *out;
    int aux;
    size_t size, ftyp, ftyp_size, meta, meta_size, box, box_size;

    for (m = 0; m < 3u; m++) {
        TH_CONVERT(g_ctx, modes[m], &out, &size);
        handle = th_primary(out, size, &ctx);

        TEST_ASSERT_EQUAL_INT(TH_W, heif_image_handle_get_width(handle));
        TEST_ASSERT_EQUAL_INT(TH_H, heif_image_handle_get_height(handle));
        TEST_ASSERT_EQUAL_INT(10, heif_image_handle_get_luma_bits_per_pixel(handle));
        TEST_ASSERT_EQUAL_INT(heif_error_Ok,
                              heif_image_handle_get_nclx_color_profile(handle, &nclx).code);
        TEST_ASSERT_EQUAL_INT(1, nclx->full_range_flag);

        if (modes[m] == HDRG_OUTPUT_GAINMAP) {
            /* PHOTO expands the gamut: a Display P3 base */
            TEST_ASSERT_EQUAL_INT(12, nclx->color_primaries);
            TEST_ASSERT_EQUAL_INT(13, nclx->transfer_characteristics);
        } else {
            TEST_ASSERT_EQUAL_INT(9, nclx->color_primaries);
            TEST_ASSERT_EQUAL_INT(16, nclx->transfer_characteristics);
            TEST_ASSERT_EQUAL_INT(9, nclx->matrix_coefficients);
        }

        heif_nclx_color_profile_free(nclx);

        /* Apple's auxiliary gain map: an SDR base's alone */
        aux = heif_image_handle_get_number_of_auxiliary_images(
            handle, LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH);
        TEST_ASSERT_EQUAL_INT((modes[m] == HDRG_OUTPUT_GAINMAP) ? 1 : 0, aux);

        heif_image_handle_release(handle);
        heif_context_free(ctx);

        /* the boxes: 'tmap' among the brands, its item and the 'altr' group, the PQ base's clli */
        TEST_ASSERT_TRUE(th_box(out, 0, size, "ftyp", &ftyp, &ftyp_size));
        TEST_ASSERT_TRUE(th_box(out, 0, size, "meta", &meta, &meta_size));
        TEST_ASSERT_EQUAL((modes[m] != HDRG_OUTPUT_PQ), th_has(out + ftyp, ftyp_size, "tmap"));
        TEST_ASSERT_EQUAL((modes[m] != HDRG_OUTPUT_PQ),
                          th_box(out, meta + 12u, meta + meta_size, "grpl", &box, &box_size));
        TEST_ASSERT_EQUAL((modes[m] != HDRG_OUTPUT_PQ), th_has(out + meta, meta_size, "altr"));
        TEST_ASSERT_EQUAL((modes[m] != HDRG_OUTPUT_GAINMAP), th_has(out + meta, meta_size, "clli"));
        TEST_ASSERT_EQUAL((modes[m] == HDRG_OUTPUT_GAINMAP),
                          th_has(out + meta, meta_size, "urn:com:apple:photo:2020:aux:hdrgainmap"));
        hdrg_free(out);
    }
}

/* the tmap item's payload (the first in idat): version 0, then the ISO 21496-1 body */
static void test_tmap_payload(void)
{
    hdrg_stats_t stats;
    uint8_t *out;
    size_t size, meta, meta_size, idat, idat_size;
    float base, alternate;

    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &out, &size);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    TEST_ASSERT_TRUE(th_box(out, 0, size, "meta", &meta, &meta_size));
    TEST_ASSERT_TRUE(th_box(out, meta + 12u, meta + meta_size, "idat", &idat, &idat_size));
    TEST_ASSERT_TRUE(idat_size >= 8u + 1u + HDRG_ISO_BODY_BYTES);

    idat += 8u;
    TEST_ASSERT_EQUAL_UINT8(0, out[idat]);        /* version */
    TEST_ASSERT_EQUAL_UINT8(0x40, out[idat + 5]); /* one channel, the base's color space */
    base = (float)(int32_t)th_be32(out + idat + 6) / (float)th_be32(out + idat + 10);
    alternate = (float)(int32_t)th_be32(out + idat + 14) / (float)th_be32(out + idat + 18);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, base);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, stats.applied_max_gain_stops, alternate);
    hdrg_free(out);
}

/*
 * the primary image's 16-bit R'G'B' (libheif's planes, its 10-bit 4:2:0 Y'CbCr, through core/yuv
 * as the input conversion of the library's: the base's BT.601 full range, the chroma at LEFT)
 */
static void th_rows(const uint8_t *data, size_t size, uint16_t *rgb)
{
    static const enum heif_channel channels[3] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
    struct heif_image_handle *handle;
    struct heif_context *ctx;
    struct heif_image *image;
    hdrg_yuv_t yuv;
    uint8_t c;
    int stride;
    float *scratch;

    handle = th_primary(data, size, &ctx);
    TEST_ASSERT_EQUAL_INT(
        heif_error_Ok,
        heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_420, NULL).code);
    memset(&yuv, 0, sizeof(yuv));

    for (c = 0; c < 3u; c++) {
        yuv.planes[c] = heif_image_get_plane_readonly(image, channels[c], &stride);
        yuv.strides[c] = (size_t)stride;
    }

    yuv.width = TH_W;
    yuv.height = TH_H;
    yuv.layout = HDRG_YUV_420;
    yuv.matrix = HDRG_MATRIX_BT601;
    yuv.bits = 10;
    yuv.siting = HDRG_CHROMA_LEFT;
    yuv.fit = HDRG_CHROMA_FIT_LINEAR;
    yuv.full_range = true;
    TEST_ASSERT_TRUE(hdrg_yuv_valid(&yuv) && hdrg_yuv_fits(&yuv));
    scratch = (float *)malloc(hdrg_yuv_scratch(&yuv) * sizeof(float));
    TEST_ASSERT_NOT_NULL(scratch);
    hdrg_yuv_to_rgb16(NULL, &yuv, scratch, rgb, (size_t)TH_W * 6u);

    free(scratch);
    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
}

/* the decoded base against the pipeline's rows (HEVC and 4:2:0 lose a little) */
static void test_base_close_to_rows(void)
{
    uint16_t *rows, *pixels;
    uint8_t *out;
    size_t size, i;
    double sum, d, psnr;

    rows = (uint16_t *)malloc((size_t)TH_W * TH_H * 3u * sizeof(uint16_t));
    pixels = (uint16_t *)malloc((size_t)TH_W * TH_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_NOT_NULL(pixels);
    th_reference(HDRG_OUTPUT_GAINMAP, rows);
    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &out, &size);
    th_rows(out, size, pixels);

    sum = 0.0;

    for (i = 0; i < (size_t)TH_W * TH_H * 3u; i++) {
        d = ((double)pixels[i] - (double)rows[i]) / 65535.0;
        sum += d * d;
    }

    psnr = 10.0 * log10(1.0 / (sum / ((double)TH_W * TH_H * 3.0)));
    TEST_ASSERT_TRUE(psnr > 40.0);

    hdrg_free(out);
    free(pixels);
    free(rows);
}

/* Apple's gain map (the same item as the tmap's): its brightest gain is the declared headroom */
static void test_gainmap_recovers(void)
{
    const uint8_t *plane;
    struct heif_image_handle *handle, *aux;
    struct heif_context *ctx;
    struct heif_image *image;
    heif_item_id id;
    hdrg_stats_t stats;
    uint32_t x, y;
    uint16_t value, top;
    uint8_t *out;
    int stride;
    size_t size;
    float recon;

    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &out, &size);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));
    handle = th_primary(out, size, &ctx);
    TEST_ASSERT_EQUAL_INT(1, heif_image_handle_get_list_of_auxiliary_image_IDs(
                                 handle, LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA, &id, 1));
    TEST_ASSERT_EQUAL_INT(heif_error_Ok,
                          heif_image_handle_get_auxiliary_image_handle(handle, id, &aux).code);
    TEST_ASSERT_EQUAL_INT(TH_W / 2, heif_image_handle_get_width(aux));
    TEST_ASSERT_EQUAL_INT(heif_error_Ok, heif_decode_image(aux, &image, heif_colorspace_undefined,
                                                           heif_chroma_undefined, NULL)
                                             .code);

    plane = heif_image_get_plane_readonly(image, heif_channel_Y, &stride);
    top = 0;

    for (y = 0; y < TH_H / 2u; y++) {
        for (x = 0; x < TH_W / 2u; x++) {
            memcpy(&value, plane + (size_t)y * (size_t)stride + (size_t)x * 2u, 2);
            top = (uint16_t)HDRG_TEST_MAX(top, value);
        }
    }

    TEST_ASSERT_TRUE(top >= 1000u);
    recon = exp2f((float)top / 1023.0f * stats.applied_max_gain_stops);
    TEST_ASSERT_FLOAT_WITHIN(0.15f * exp2f(stats.applied_max_gain_stops),
                             exp2f(stats.applied_max_gain_stops), recon);

    heif_image_release(image);
    heif_image_handle_release(aux);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    hdrg_free(out);
}

static void test_threads_same_bytes(void)
{
    hdrg_context_t *one;
    uint8_t *a, *b;
    size_t a_size, b_size;

    th_context(&one, 1);
    TH_CONVERT(one, HDRG_OUTPUT_GAINMAP, &a, &a_size);
    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &b, &b_size);
    TEST_ASSERT_EQUAL_size_t(a_size, b_size);
    TEST_ASSERT_EQUAL_MEMORY(a, b, a_size);
    hdrg_free(a);
    hdrg_free(b);
    hdrg_context_destroy(one);
}

static struct heif_error th_write(struct heif_context *ctx, const void *data, size_t size,
                                  void *user)
{
    hdrg_raw_image_t *sink = (hdrg_raw_image_t *)user;
    struct heif_error error;
    uint8_t *copy;

    (void)ctx;
    copy = (uint8_t *)malloc(size);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, data, size);
    sink->pixels = copy;
    sink->stride = size;
    memset(&error, 0, sizeof(error));
    error.message = "";

    return error;
}

/*
 * an sRGB HEIF of libheif's from the test image, 10-bit (the fetched x265 has Main10 alone), a
 * quarter turn (irot 1)
 */
static bool th_sdr_heif(uint8_t **out, size_t *size)
{
    struct heif_encoding_options *options;
    struct heif_encoder *encoder;
    struct heif_context *ctx;
    struct heif_image *image;
    struct heif_writer writer;
    hdrg_raw_image_t sink;
    uint32_t x, y;
    uint16_t value;
    uint8_t *plane;
    int stride;

    ctx = heif_context_alloc();

    if (heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder).code !=
        heif_error_Ok) {
        heif_context_free(ctx);

        return false;
    }

    TEST_ASSERT_EQUAL_INT(heif_error_Ok,
                          heif_image_create((int)TH_W, (int)TH_H, heif_colorspace_RGB,
                                            heif_chroma_interleaved_RRGGBB_LE, &image)
                              .code);
    TEST_ASSERT_EQUAL_INT(
        heif_error_Ok,
        heif_image_add_plane(image, heif_channel_interleaved, (int)TH_W, (int)TH_H, 10).code);
    plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);

    for (y = 0; y < TH_H; y++) {
        for (x = 0; x < TH_W * 3u; x++) {
            value = (uint16_t)(((uint16_t)g_pixels[(size_t)y * TH_W * 3u + x] << 2) |
                               (g_pixels[(size_t)y * TH_W * 3u + x] >> 6));
            plane[(size_t)y * (size_t)stride + (size_t)x * 2u] = (uint8_t)value;
            plane[(size_t)y * (size_t)stride + (size_t)x * 2u + 1u] = (uint8_t)(value >> 8);
        }
    }

    options = heif_encoding_options_alloc();
    options->image_orientation = heif_orientation_rotate_90_cw;
    TEST_ASSERT_EQUAL_INT(heif_error_Ok, heif_encoder_set_lossy_quality(encoder, 95).code);
    TEST_ASSERT_EQUAL_INT(heif_error_Ok,
                          heif_context_encode_image(ctx, image, encoder, options, NULL).code);

    memset(&sink, 0, sizeof(sink));
    writer.writer_api_version = 1;
    writer.write = th_write;
    TEST_ASSERT_EQUAL_INT(heif_error_Ok, heif_context_write(ctx, &writer, &sink).code);

    heif_encoding_options_free(options);
    heif_encoder_release(encoder);
    heif_image_release(image);
    heif_context_free(ctx);

    *out = (uint8_t *)(uintptr_t)sink.pixels;
    *size = sink.stride;

    return true;
}

/* no HEIF is read: libheif's SDR file, nor a gain-map HEIF of the module's */
static void test_heif_not_read(void)
{
    uint8_t *data, *out;
    size_t size, out_size;

    if (!th_sdr_heif(&data, &size)) {
        TEST_IGNORE_MESSAGE("libheif has no HEVC encoder");
    }

    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_UNSUPPORTED_FORMAT,
                             hdrg_convert(g_ctx, data, size, NULL, NULL, NULL, &out, &out_size));
    free(data);

    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &data, &size);
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_UNSUPPORTED_FORMAT,
                             hdrg_convert(g_ctx, data, size, NULL, NULL, NULL, &out, &out_size));
    hdrg_free(data);
}

/* a JPEG's Exif (orientation 6) goes into the HEIF upright */
static void test_exif_carried(void)
{
    struct heif_image_handle *handle;
    struct heif_context *ctx;
    struct heif_encoder *encoder;
    hdrg_encode_params_t encode;
    hdrg_allocator_t alloc;
    hdrg_status_t status;
    heif_item_id id;
    uint8_t *jpeg, *out, *exif;
    size_t jpeg_size, size, exif_size;

    ctx = heif_context_alloc();

    if (heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder).code !=
        heif_error_Ok) {
        heif_context_free(ctx);
        TEST_IGNORE_MESSAGE("libheif has no HEVC encoder");
    }

    heif_encoder_release(encoder);
    heif_context_free(ctx);

    alloc = hdrg_test_allocator();
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_jpeg_rgb(&alloc, g_pixels, TH_W, TH_H,
                                                         (size_t)TH_W * 3u, 95, NULL, 0, th_exif6,
                                                         sizeof(th_exif6), &jpeg, &jpeg_size));
    hdrg_encode_params_init(&encode);
    status = hdrg_convert(g_ctx, jpeg, jpeg_size, NULL, NULL, &encode, &out, &size);
    hdrg_mem_free(&alloc, jpeg);
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, status);

    handle = th_primary(out, size, &ctx);
    TEST_ASSERT_EQUAL_INT(TH_H, heif_image_handle_get_width(handle));
    TEST_ASSERT_EQUAL_INT(1,
                          heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1));
    exif_size = heif_image_handle_get_metadata_size(handle, id);
    exif = (uint8_t *)malloc(exif_size);
    TEST_ASSERT_NOT_NULL(exif);
    TEST_ASSERT_EQUAL_INT(heif_error_Ok, heif_image_handle_get_metadata(handle, id, exif).code);
    TEST_ASSERT_EQUAL_UINT32(0, th_be32(exif));
    TEST_ASSERT_EQUAL_UINT16(1, hdrg_exif_orientation(exif + 4, exif_size - 4u));

    free(exif);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    hdrg_free(out);
}

/* Apple's ImageIO reads the HEIF with both of its gain maps (macOS 15 and later) */
static void test_imageio_gainmaps(void)
{
#if defined(__APPLE__)
    CFDictionaryRef iso, apple;
    CGImageSourceRef source;
    CFDataRef data;
    CGImageRef image;
    uint8_t *out;
    size_t size;

    if (!__builtin_available(macOS 15.0, *)) {
        TEST_IGNORE_MESSAGE("ImageIO reads ISO gain maps from macOS 15");
    }

    TH_CONVERT(g_ctx, HDRG_OUTPUT_GAINMAP, &out, &size);
    data = CFDataCreate(NULL, out, (CFIndex)size);
    source = CGImageSourceCreateWithData(data, NULL);
    TEST_ASSERT_NOT_NULL(source);

    image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    TEST_ASSERT_NOT_NULL(image);
    TEST_ASSERT_EQUAL_size_t(TH_W, CGImageGetWidth(image));
    TEST_ASSERT_EQUAL_size_t(10, CGImageGetBitsPerComponent(image));
    CGImageRelease(image);

    iso = CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, 0, kCGImageAuxiliaryDataTypeISOGainMap);
    apple =
        CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, 0, kCGImageAuxiliaryDataTypeHDRGainMap);
    TEST_ASSERT_NOT_NULL(iso);
    TEST_ASSERT_NOT_NULL(apple);

    CFRelease(apple);
    CFRelease(iso);
    CFRelease(source);
    CFRelease(data);
    hdrg_free(out);
#else
    TEST_IGNORE_MESSAGE("ImageIO is Apple's");
#endif
}

/* the writer's options checked (an unknown id, the qualities past their range, a chroma) */
static void test_settings_ranges(void)
{
    static const char *const quality[] = {"quality=101"}, *const gm[] = {"gainmap_quality=101"},
                             *const chroma[] = {"chroma=422"};
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    uint8_t *out;
    size_t size;

    th_raw(&raw);
    hdrg_encode_params_init(&encode);
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_NOT_FOUND,
                             hdrg_test_writer(g_ctx, "org.hdrgrading.nonesuch", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TH_ID, quality, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG, hdrg_test_writer(g_ctx, TH_ID, gm, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OK, hdrg_test_writer(g_ctx, TH_ID, chroma, 1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_ERR_INVALID_ARG,
                             hdrg_convert_raw(g_ctx, &raw, NULL, NULL, &encode, &out, &size));
}

void setUp(void)
{
    th_context(&g_ctx, 4);
    g_pixels = hdrg_test_image(TH_W, TH_H);
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
    RUN_TEST(test_tmap_payload);
    RUN_TEST(test_base_close_to_rows);
    RUN_TEST(test_gainmap_recovers);
    RUN_TEST(test_threads_same_bytes);
    RUN_TEST(test_heif_not_read);
    RUN_TEST(test_exif_carried);
    RUN_TEST(test_imageio_gainmaps);
    RUN_TEST(test_settings_ranges);

    return UNITY_END();
}
