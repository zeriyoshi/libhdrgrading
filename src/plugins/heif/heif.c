/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The HEIF writer module (org.hdrgrading.heif): a plugin of the codec ABI over libheif and its
 * HEVC encoder (x265), never linked into libhdrgrading, and writing alone (no HEIF is read: the
 * parsers of libheif are left out of the conversions' input). The forms as 10-bit HEVC (Main10):
 * full-range Y'CbCr (the library's conversion), the colors signaled by nclx alone, the gain maps
 * as ISO 21496-1 ('tmap' with an 'altr' group) and an SDR base's also as Apple's auxiliary HDR
 * gain map, the PQ bases' light as 'clli'. libheif encodes the images, bmff.c writes the file.
 */
#include <stdio.h>
#include <string.h>

#include <libheif/heif.h>

#include "bmff.h"

#define HDRG_HEIF_LIT(s)   s, sizeof(s) - 1u
#define HDRG_HEIF_ID       "org.hdrgrading.heif"
#define HDRG_HEIF_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_HEIF_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_HEIF_MAX_SIDE 65535u
#define HDRG_HEIF_ITEMS    4u
#define HDRG_HEIF_DEPTH    10u
/* the tmap item's payload: its version byte, then ISO 21496-1's GainMapMetadata */
#define HDRG_HEIF_TMAP 128u

/* the writer's instance: the image its rows go into */
typedef struct {
    struct heif_image *image;
    uint32_t desc[HDRG_YUVD_WORDS];
    uint32_t width, height, primaries, transfer, threads;
    uint8_t quality, gainmap_quality;
    bool subsampled, apple_xmp;
} hdrg_heif_writer_t;

/* libheif's file, written into memory of the context's */
typedef struct {
    const hdrg_plugin_api_t *api;
    uint8_t *data;
    size_t size;
} hdrg_heif_sink_t;

static struct heif_error hdrg_heif_write(struct heif_context *ctx, const void *data, size_t size,
                                         void *user)
{
    hdrg_heif_sink_t *sink = (hdrg_heif_sink_t *)user;
    struct heif_error error;

    (void)ctx;
    memset(&error, 0, sizeof(error));
    error.message = "";

    sink->api->mem_free(sink->api, sink->data);
    sink->data = (uint8_t *)sink->api->mem_alloc(sink->api, (size > 0) ? size : 1u);
    sink->size = 0;

    if (sink->data == NULL) {
        error.code = heif_error_Memory_allocation_error;
        error.message = "out of memory";

        return error;
    }

    memcpy(sink->data, data, size);
    sink->size = size;

    return error;
}

/* an image of 10-bit Y'CbCr planes (4:2:0 or 4:4:4) and its full-range nclx */
static hdrg_status_t hdrg_heif_image(uint32_t width, uint32_t height, bool subsampled,
                                     uint32_t primaries, uint32_t transfer, uint32_t matrix,
                                     struct heif_image **out)
{
    const uint32_t cw = subsampled ? (width + 1u) / 2u : width,
                   ch = subsampled ? (height + 1u) / 2u : height;
    struct heif_color_profile_nclx *nclx;
    struct heif_error error;
    struct heif_image *image;

    *out = NULL;

    error = heif_image_create((int)width, (int)height, heif_colorspace_YCbCr,
                              subsampled ? heif_chroma_420 : heif_chroma_444, &image);
    if (error.code != heif_error_Ok) {
        return HDRG_ERR_NOMEM;
    }

    *out = image;

    if (heif_image_add_plane(image, heif_channel_Y, (int)width, (int)height, HDRG_HEIF_DEPTH)
                .code != heif_error_Ok ||
        heif_image_add_plane(image, heif_channel_Cb, (int)cw, (int)ch, HDRG_HEIF_DEPTH).code !=
            heif_error_Ok ||
        heif_image_add_plane(image, heif_channel_Cr, (int)cw, (int)ch, HDRG_HEIF_DEPTH).code !=
            heif_error_Ok) {
        return HDRG_ERR_NOMEM;
    }

    nclx = heif_nclx_color_profile_alloc();
    if (nclx == NULL) {
        return HDRG_ERR_NOMEM;
    }

    nclx->color_primaries = (enum heif_color_primaries)primaries;
    nclx->transfer_characteristics = (enum heif_transfer_characteristics)transfer;
    nclx->matrix_coefficients = (enum heif_matrix_coefficients)matrix;
    nclx->full_range_flag = 1;
    error = heif_image_set_nclx_color_profile(image, nclx);
    heif_nclx_color_profile_free(nclx);

    return (error.code == heif_error_Ok) ? HDRG_OK : HDRG_ERR_ENCODE;
}

/* the gain map as 10-bit 4:2:0 gray: its 8 bits replicated into 10, neutral chroma */
static hdrg_status_t hdrg_heif_gainmap(const uint8_t *pixels, size_t pixels_stride, uint32_t width,
                                       uint32_t height, struct heif_image **out)
{
    hdrg_status_t status;
    uint32_t x, y;
    uint16_t value;
    uint8_t c, *row;
    int stride;

    status = hdrg_heif_image(width, height, true, 2, 2, HDRG_MATRIX_BT601, out);
    if (status != HDRG_OK) {
        return status;
    }

    row = heif_image_get_plane(*out, heif_channel_Y, &stride);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            value = (uint16_t)(((uint16_t)pixels[(size_t)y * pixels_stride + x] << 2) |
                               (pixels[(size_t)y * pixels_stride + x] >> 6));
            memcpy(row + (size_t)y * (size_t)stride + (size_t)x * 2u, &value, 2);
        }
    }

    value = 512;

    for (c = 0; c < 2u; c++) {
        row = heif_image_get_plane(*out, (c == 0) ? heif_channel_Cb : heif_channel_Cr, &stride);

        for (y = 0; y < (height + 1u) / 2u; y++) {
            for (x = 0; x < (width + 1u) / 2u; x++) {
                memcpy(row + (size_t)y * (size_t)stride + (size_t)x * 2u, &value, 2);
            }
        }
    }

    return HDRG_OK;
}

/* encodes image into ctx at quality: its handle's item */
static hdrg_status_t hdrg_heif_encode(struct heif_context *ctx, struct heif_encoder *encoder,
                                      const struct heif_image *image, uint8_t quality,
                                      const char *chroma, heif_item_id *out)
{
    struct heif_encoding_options *options;
    struct heif_image_handle *handle;
    struct heif_error error;

    if (heif_encoder_set_lossy_quality(encoder, quality).code != heif_error_Ok ||
        heif_encoder_set_parameter_string(encoder, "chroma", chroma).code != heif_error_Ok) {
        return HDRG_ERR_ENCODE;
    }

    options = heif_encoding_options_alloc();
    if (options == NULL) {
        return HDRG_ERR_NOMEM;
    }

    options->save_alpha_channel = 0;
    handle = NULL;
    error = heif_context_encode_image(ctx, image, encoder, options, &handle);
    heif_encoding_options_free(options);

    if (error.code != heif_error_Ok || handle == NULL) {
        return HDRG_ERR_ENCODE;
    }

    *out = heif_image_handle_get_item_id(handle);
    heif_image_handle_release(handle);

    return HDRG_OK;
}

/* the item of id among items */
static const hdrg_bmff_item_t *hdrg_heif_item(const hdrg_bmff_item_t *items, uint32_t count,
                                              uint32_t id)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (items[i].id == id) {
            return &items[i];
        }
    }

    return NULL;
}

/* an option of the writer's (declared, so present) */
static int64_t hdrg_heif_option(const hdrg_plugin_api_t *api, const hdrg_options_t *options,
                                const char *key)
{
    int64_t value;

    value = 0;
    (void)api->option_get_int(options, key, strlen(key), &value);

    return value;
}

static void HDRG_PLUGIN_CALL hdrg_heif_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_heif_writer_t *state = (hdrg_heif_writer_t *)inst;

    if (state == NULL) {
        return;
    }

    heif_image_release(state->image);
    api->mem_free(api, state);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_heif_create(hdrg_writer_t *writer,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    hdrg_heif_writer_t *state;
    hdrg_status_t status;
    size_t needed;
    char chroma[8];

    *inst = NULL;

    if (api->option_get_string(options, HDRG_HEIF_LIT("chroma"), chroma, sizeof(chroma), &needed) !=
            HDRG_OK ||
        (strcmp(chroma, "444") != 0 && strcmp(chroma, "420") != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    state = (hdrg_heif_writer_t *)api->mem_calloc(api, 1, sizeof(*state));
    if (state == NULL) {
        return HDRG_ERR_NOMEM;
    }

    state->subsampled = strcmp(chroma, "420") == 0;
    state->quality = (uint8_t)hdrg_heif_option(api, options, "quality");
    state->gainmap_quality = (uint8_t)hdrg_heif_option(api, options, "gainmap_quality");
    state->apple_xmp = hdrg_heif_option(api, options, "apple_xmp") != 0;
    (void)api->write_image(writer, &state->width, &state->height, NULL, &state->primaries,
                           &state->transfer, &state->threads);
    state->desc[HDRG_YUVD_WIDTH] = state->width;
    state->desc[HDRG_YUVD_HEIGHT] = state->height;
    state->desc[HDRG_YUVD_LAYOUT] = state->subsampled ? HDRG_YUV_420 : HDRG_YUV_444;
    state->desc[HDRG_YUVD_MATRIX] =
        (state->primaries == HDRG_PRIMARIES_BT2020) ? HDRG_MATRIX_BT2020 : HDRG_MATRIX_BT601;
    state->desc[HDRG_YUVD_BITS] = HDRG_HEIF_DEPTH;
    state->desc[HDRG_YUVD_SITING] = HDRG_CHROMA_LEFT;
    state->desc[HDRG_YUVD_FLAGS] = HDRG_YUVF_FULL_RANGE;

    status = hdrg_heif_image(state->width, state->height, state->subsampled, state->primaries,
                             state->transfer, state->desc[HDRG_YUVD_MATRIX], &state->image);
    if (status != HDRG_OK) {
        hdrg_heif_destroy(state, api);

        return status;
    }

    *inst = state;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_heif_rows(void *inst, uint32_t y0, uint32_t rows,
                                                     const uint8_t *data, size_t stride,
                                                     const hdrg_plugin_api_t *api)
{
    const hdrg_heif_writer_t *state = (const hdrg_heif_writer_t *)inst;
    uint8_t c;
    int plane_stride;
    size_t strides[3];
    void *planes[3];

    (void)stride;

    for (c = 0; c < 3u; c++) {
        planes[c] = heif_image_get_plane(state->image,
                                         (c == 0) ? heif_channel_Y
                                                  : ((c == 1) ? heif_channel_Cb : heif_channel_Cr),
                                         &plane_stride);
        strides[c] = (size_t)plane_stride;
    }

    return api->rgb16_to_yuv(api, state->desc, planes, strides,
                             (const uint16_t *)(const void *)data, y0, rows);
}

/* the file's gain map: its image item, the tmap payload and colr, Apple's XMP (an SDR base's) */
static hdrg_status_t hdrg_heif_tmap(hdrg_writer_t *writer, const hdrg_plugin_api_t *api,
                                    const hdrg_heif_writer_t *state, hdrg_heif_file_t *file,
                                    uint8_t *tmap, char *xmp, size_t xmp_cap)
{
    hdrg_status_t status;
    uint32_t primaries, transfer, bits;
    size_t size;

    status = api->write_alternate(writer, &primaries, &transfer, &bits);
    if (status == HDRG_OK) {
        status = api->write_iso21496(writer, tmap + 1, HDRG_HEIF_TMAP - 1u, &size);
    }

    if (status != HDRG_OK || size > HDRG_HEIF_TMAP - 1u) {
        return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
    }

    tmap[0] = 0; /* version */
    file->tmap = tmap;
    file->tmap_size = 1u + size;
    file->alt_primaries = (uint8_t)primaries;
    file->alt_transfer = (uint8_t)transfer;
    file->alt_depth = (uint8_t)bits;

    /* Apple's HDR gain map assumes an SDR base, as its XMP in a JPEG */
    if (state->apple_xmp &&
        api->write_apple_xmp(writer, (uint8_t *)xmp, xmp_cap, &size) == HDRG_OK &&
        size <= xmp_cap) {
        file->xmp = (const uint8_t *)xmp;
        file->xmp_size = size;
    }

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_heif_finish(void *inst, hdrg_writer_t *writer,
                                                       const hdrg_plugin_api_t *api)
{
    const hdrg_heif_writer_t *state = (const hdrg_heif_writer_t *)inst;
    const hdrg_bmff_item_t *item;
    const uint8_t *gm_pixels;
    hdrg_bmff_item_t items[HDRG_HEIF_ITEMS];
    hdrg_heif_file_t file;
    hdrg_heif_sink_t sink;
    hdrg_status_t status, gm_status;
    heif_item_id base_id, gm_id;
    struct heif_context *ctx;
    struct heif_encoder *encoder;
    struct heif_image *gm_image;
    struct heif_writer sink_writer;
    uint32_t count, primary, gw, gh, max_cll, max_fall;
    uint8_t tmap[HDRG_HEIF_TMAP];
    size_t gm_stride;
    char threads[16], xmp[2048];

    encoder = NULL;
    gm_image = NULL;
    gm_id = 0;
    memset(&sink, 0, sizeof(sink));
    memset(&file, 0, sizeof(file));
    sink.api = api;

    gm_status = api->write_gainmap(writer, &gm_pixels, &gm_stride, &gw, &gh, NULL);
    if (gm_status != HDRG_OK && gm_status != HDRG_ERR_NOT_FOUND) {
        return gm_status;
    }

    ctx = heif_context_alloc();
    if (ctx == NULL) {
        return HDRG_ERR_NOMEM;
    }

    if (heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder).code !=
        heif_error_Ok) {
        status = HDRG_ERR_UNSUPPORTED;

        goto cleanup;
    }

    /* x265's thread pool: the context's threads */
    (void)snprintf(threads, sizeof(threads), "%u",
                   (unsigned int)((state->threads > 1u) ? state->threads : 1u));
    (void)heif_encoder_set_parameter_string(encoder, "x265:pools", threads);

    status = hdrg_heif_encode(ctx, encoder, state->image, state->quality,
                              state->subsampled ? "420" : "444", &base_id);

    if (status == HDRG_OK && gm_status == HDRG_OK) {
        status = hdrg_heif_gainmap(gm_pixels, gm_stride, gw, gh, &gm_image);
        if (status == HDRG_OK) {
            status =
                hdrg_heif_encode(ctx, encoder, gm_image, state->gainmap_quality, "420", &gm_id);
        }
    }

    if (status != HDRG_OK) {
        goto cleanup;
    }

    sink_writer.writer_api_version = 1;
    sink_writer.write = hdrg_heif_write;

    if (heif_context_write(ctx, &sink_writer, &sink).code != heif_error_Ok || sink.data == NULL) {
        status = (sink.data == NULL) ? HDRG_ERR_NOMEM : HDRG_ERR_ENCODE;

        goto cleanup;
    }

    status = hdrg_bmff_items(sink.data, sink.size, items, HDRG_HEIF_ITEMS, &count, &primary,
                             &file.ftyp, &file.ftyp_size);
    if (status != HDRG_OK) {
        goto cleanup;
    }

    item = hdrg_heif_item(items, count, base_id);
    status = (item != NULL) ? api->write_light(writer, &max_cll, &max_fall, NULL) : HDRG_ERR_ENCODE;

    if (status != HDRG_OK) {
        goto cleanup;
    }

    file.base.item = *item;
    file.base.width = state->width;
    file.base.height = state->height;
    file.base.max_cll = (uint16_t)((max_cll < 65535u) ? max_cll : 65535u);
    file.base.max_fall = (uint16_t)((max_fall < 65535u) ? max_fall : 65535u);
    file.base.primaries = (uint8_t)state->primaries;
    file.base.transfer = (uint8_t)state->transfer;
    file.base.matrix = (uint8_t)state->desc[HDRG_YUVD_MATRIX];
    file.base.depth = HDRG_HEIF_DEPTH;

    if (api->write_exif(writer, &file.exif, &file.exif_size) != HDRG_OK) {
        file.exif = NULL;
        file.exif_size = 0;
    }

    if (gm_status == HDRG_OK) {
        item = hdrg_heif_item(items, count, gm_id);
        if (item == NULL) {
            status = HDRG_ERR_ENCODE;

            goto cleanup;
        }

        file.gainmap.item = *item;
        file.gainmap.width = gw;
        file.gainmap.height = gh;
        file.gainmap.primaries = 2;
        file.gainmap.transfer = 2;
        file.gainmap.matrix = HDRG_MATRIX_BT601;
        file.gainmap.depth = HDRG_HEIF_DEPTH;

        status = hdrg_heif_tmap(writer, api, state, &file, tmap, xmp, sizeof(xmp));
    }

    if (status == HDRG_OK) {
        status = hdrg_heif_mux(api, writer, &file);
    }

cleanup:
    heif_image_release(gm_image);
    heif_encoder_release(encoder);
    heif_context_free(ctx);
    api->mem_free(api, sink.data);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_heif_init(hdrg_plugin_t *plugin,
                                                     const hdrg_plugin_api_t *api)
{
    const uint32_t forms = HDRG_CODEC_FORM(HDRG_OUTPUT_GAINMAP) |
                           HDRG_CODEC_FORM(HDRG_OUTPUT_HYBRID) | HDRG_CODEC_FORM(HDRG_OUTPUT_PQ);
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_HEIF_LIT(HDRG_HEIF_ID), HDRG_HEIF_LIT("HEIF"),
                                  HDRG_HEIF_LIT("1.0.0"), HDRG_HEIF_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_HEIF_LIT(HDRG_HEIF_LICENSE),
                                         HDRG_HEIF_LIT(HDRG_HEIF_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_HEIF_LIT(HDRG_HEIF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_HEIF_LIT("heic,heif"));
    if (status == HDRG_OK) {
        status = api->class_set_writer(cls, forms, HDRG_OUTPUT_GAINMAP, HDRG_CODEC_ROWS_RGB16,
                                       HDRG_CODEC_COLOR_CICP, HDRG_HEIF_MAX_SIDE, hdrg_heif_create,
                                       hdrg_heif_rows, hdrg_heif_finish, hdrg_heif_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_HEIF_LIT("quality"), 70, 0, 100,
            HDRG_HEIF_LIT("libheif's quality of the base (x265's CRF 50 - q / 2)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_HEIF_LIT("gainmap_quality"), 70, 0, 100,
                                           HDRG_HEIF_LIT("... of the gain map"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_string(
            cls, HDRG_HEIF_LIT("chroma"), HDRG_HEIF_LIT("420"),
            HDRG_HEIF_LIT("420 (Main10) or 444 (RExt, few decoders take it)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_HEIF_LIT("apple_xmp"), 1, 0, 1,
            HDRG_HEIF_LIT("GAINMAP: Apple's auxiliary HDR gain map and its XMP too"));
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
