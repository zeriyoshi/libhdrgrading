/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>

#include <png.h>

#include "parts.h"

#define HDRG_PNG_CHUNKS     64u /* the ancillary chunks libpng caches at most */
#define HDRG_PNG_ID         "hdrg.png"
#define HDRG_PNG_LICENSE    "Apache-2.0 OR MIT"
#define HDRG_PNG_COPY       "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_PNG_GAMMA_MIN  0.2 /* file gamma (encoding exponent) accepted from gAMA */
#define HDRG_PNG_GAMMA_MAX  5.0
#define HDRG_PNG_GAMMA_SRGB 1e-3

typedef struct {
    const uint8_t *data;
    size_t size, offset;
} hdrg_png_source_t;

static void hdrg_png_read_fn(png_structp png, png_bytep out, png_size_t length)
{
    hdrg_png_source_t *source = (hdrg_png_source_t *)png_get_io_ptr(png);

    if (length > source->size - source->offset) {
        png_error(png, "read past end");
    }

    memcpy(out, source->data + source->offset, length);
    source->offset += length;
}

/* libpng's allocation (and zlib's through it) by the context's allocator */
static png_voidp hdrg_png_malloc(png_structp png, png_alloc_size_t size)
{
    const hdrg_plugin_api_t *api = (const hdrg_plugin_api_t *)png_get_mem_ptr(png);

    return api->mem_alloc(api, size);
}

static void hdrg_png_free(png_structp png, png_voidp ptr)
{
    const hdrg_plugin_api_t *api = (const hdrg_plugin_api_t *)png_get_mem_ptr(png);

    api->mem_free(api, ptr);
}

static void hdrg_png_error_fn(png_structp png, png_const_charp message)
{
    (void)message;

    longjmp(png_jmpbuf(png), 1);
}

static void hdrg_png_warning_fn(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

/*
 * Color resolution: cICP wins, then iCCP (the host resolves it after the
 * pixels, which lcms2 may convert), then sRGB, then cHRM / gAMA, else sRGB.
 */
static hdrg_status_t hdrg_png_color(png_structp png, png_infop info, hdrg_reader_t *reader,
                                    const hdrg_plugin_api_t *api)
{
    png_charp icc_name;
    png_bytep icc_profile;
    png_uint_32 icc_len;
    png_byte cp, tf, mc, vfr;
    int compression, intent;
    bool has_xy;
    double xy[8], file_gamma, gamma;

    if (png_get_cICP(png, info, &cp, &tf, &mc, &vfr) != 0) {
        /* RGB, full range, an SDR transfer */
        if (mc == 0 && vfr == 1 && cp < HDRG_PRIMARIES_ADOBE_RGB && tf < HDRG_TRANSFER_ADOBE_RGB &&
            api->read_set_cicp(reader, cp, tf) == HDRG_OK) {
            return HDRG_OK;
        }

        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    if (png_get_iCCP(png, info, &icc_name, &compression, &icc_profile, &icc_len) != 0 &&
        icc_len > 0) {
        return api->read_set_icc(reader, icc_profile, icc_len);
    }

    if (png_get_sRGB(png, info, &intent) != 0) {
        return HDRG_OK;
    }

    has_xy = png_get_cHRM(png, info, &xy[6], &xy[7], &xy[0], &xy[1], &xy[2], &xy[3], &xy[4],
                          &xy[5]) != 0;
    gamma = 0.0;

    /* the 1 / 2.2 sRGB writers put next to the sRGB chunk is the sRGB curve */
    if (png_get_gAMA(png, info, &file_gamma) != 0 && file_gamma >= HDRG_PNG_GAMMA_MIN &&
        file_gamma <= HDRG_PNG_GAMMA_MAX && fabs(file_gamma - 1.0 / 2.2) > HDRG_PNG_GAMMA_SRGB) {
        gamma = 1.0 / file_gamma;
    }

    /* unusable chromaticities leave the sRGB primaries */
    if (has_xy && api->read_set_xy(reader, xy, gamma) == HDRG_OK) {
        return HDRG_OK;
    }

    return api->read_set_xy(reader, NULL, gamma);
}

static uint32_t HDRG_PLUGIN_CALL hdrg_png_probe(const uint8_t *data, size_t size,
                                                const hdrg_plugin_api_t *api)
{
    (void)api;

    return (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) ? 1u : 0u;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_png_read(hdrg_reader_t *reader, const uint8_t *data,
                                                    size_t size, const hdrg_plugin_api_t *api)
{
#ifdef PNG_READ_SWAP_SUPPORTED
    const uint16_t probe = 1;
#endif
    hdrg_png_source_t source;
    volatile hdrg_status_t status; /* set after setjmp, read after a longjmp */
    hdrg_status_t step;
    png_structp png;
    png_infop info;
    png_bytep *volatile rows; /* freed after a longjmp (C11 7.13.2.1) */
    png_uint_32 width, height;
    uint64_t max_pixels;
    uint32_t y, bits;
    uint8_t *pixels;
    int bit_depth, color_type;
    size_t stride;

    status = HDRG_ERR_DECODE;
    rows = NULL;

    (void)api->read_limits(reader, &max_pixels, NULL);

    png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, hdrg_png_error_fn,
                                   hdrg_png_warning_fn, (png_voidp)(uintptr_t)api, hdrg_png_malloc,
                                   hdrg_png_free);
    if (png == NULL) {
        return HDRG_ERR_NOMEM;
    }

    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);

        return HDRG_ERR_NOMEM;
    }

    if (setjmp(png_jmpbuf(png))) {
        goto fail;
    }

    source.data = data;
    source.size = size;
    source.offset = 0;

    png_set_read_fn(png, &source, hdrg_png_read_fn);

    /*
     * the text and the suggested palettes the reader never uses: dropped unread (each could
     * inflate to libpng's 8 MB and be kept), and few chunks cached at all
     */
#if defined(PNG_HANDLE_AS_UNKNOWN_SUPPORTED)
    png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_NEVER,
                                (png_const_bytep) "tEXt\0zTXt\0iTXt\0sPLT\0", 4);
#endif
#if defined(PNG_USER_LIMITS_SUPPORTED)
    png_set_chunk_cache_max(png, HDRG_PNG_CHUNKS);
#endif
    png_read_info(png, info);
    png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);

    /* libpng refuses a side of 0 or past its limits (png_check_IHDR) */
    if ((uint64_t)width * height > max_pixels) {
        status = HDRG_ERR_LIMIT;

        goto fail;
    }

    /* status keeps its value from before setjmp until a goto fail */
    step = hdrg_png_color(png, info, reader, api);
    if (step != HDRG_OK) {
        status = step;

        goto fail;
    }

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }

    png_set_strip_alpha(png);

    if (bit_depth < 8) {
        png_set_packing(png);
    }

#ifdef PNG_READ_SWAP_SUPPORTED
    if (bit_depth == 16 && *(const uint8_t *)&probe == 1) {
        png_set_swap(png);
    }
#endif

    png_read_update_info(png, info);

    if (png_get_channels(png, info) != 3) {
        goto fail;
    }

    bits = (png_get_bit_depth(png, info) == 16) ? 16u : 8u;
    if ((size_t)width * 3u * (bits / 8u) < png_get_rowbytes(png, info)) {
        goto fail;
    }

    step = api->read_image(reader, width, height, bits, &pixels, &stride);
    if (step != HDRG_OK) {
        status = step;

        goto fail;
    }

    rows = (png_bytep *)api->mem_alloc(api, sizeof(png_bytep) * height);
    if (rows == NULL) {
        status = HDRG_ERR_NOMEM;

        goto fail;
    }

    for (y = 0; y < height; y++) {
        rows[y] = pixels + (size_t)y * stride;
    }

    png_read_image(png, rows);
    png_read_end(png, NULL);

    api->mem_free(api, rows);
    png_destroy_read_struct(&png, &info, NULL);

    return HDRG_OK;

fail:
    api->mem_free(api, rows);
    png_destroy_read_struct(&png, &info, NULL);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_png_init(hdrg_plugin_t *plugin,
                                                          const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_CODEC_LIT(HDRG_PNG_ID), HDRG_CODEC_LIT("PNG"),
                                  HDRG_CODEC_LIT("1.0.0"), HDRG_CODEC_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_CODEC_LIT(HDRG_PNG_LICENSE),
                                         HDRG_CODEC_LIT(HDRG_PNG_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_CODEC_LIT(HDRG_PNG_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_CODEC_LIT("png"));
    if (status == HDRG_OK) {
        status = api->class_set_reader(cls, hdrg_png_probe, hdrg_png_read);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
