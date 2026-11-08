/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * jpegli: the libjpeg API (its wrapper) to read, its native API (jpegli_*) to write. The C++-only
 * includes of the native headers are satisfied by src/io/jpegli_compat/ when compiling as C
 */
#include <jpeglib.h>
#include <lib/jpegli/decode.h>
#include <lib/jpegli/encode.h>

#include "parts.h"
#include "jpeg_mux.h"

#define HDRG_JPEG_ID       "hdrg.jpeg"
#define HDRG_JPEG_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_JPEG_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_JPEG_MAX_SIDE 65500u /* jpegli's JPEG_MAX_DIMENSION */
#define HDRG_JPEG_ICC_MAX  65519u /* a profile in one APP2 segment at most (jpeg_mux's) */
#define HDRG_JPEG_STRIP    8u     /* iMCU rows a chroma conversion emits */
#define HDRG_JPEG_MARGIN                                                                           \
    8u /* luma rows of context on each side of it (the conversion reaches 5)                       \
        */

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
} hdrg_jpeg_error_t;

/*
 * The raw planes of a strip: luma rows [base, base + filled) of the image
 * (the chroma rows of the same luma rows), and a strip's R'G'B'
 */
typedef struct {
    uint16_t *luma, *chroma[2], *rgb;
    uint32_t base, filled, capacity, imcu, vfactor;
    uint32_t fit;                      /* HDRG_CHROMA_FIT_* */
    size_t luma_stride, chroma_stride; /* in samples */
} hdrg_jpeg_raw_t;

/* a jpegli compressor into its memory (jpegli_mem_dest: its own malloc) */
typedef struct {
    struct jpeg_compress_struct cinfo;
    hdrg_jpeg_error_t error;
    unsigned long mem_size;
    unsigned char *mem;
    bool created, failed;
} hdrg_jpeg_enc_t;

/* the writer's instance */
typedef struct {
    hdrg_jpeg_enc_t base;
    double lumi_nits;
    uint32_t form;
    uint8_t quality, gainmap_quality;
    bool hagc, apple_xmp, hdrgm_xmp;
} hdrg_jpeg_writer_t;

static void hdrg_jpeg_error_exit(j_common_ptr cinfo)
{
    hdrg_jpeg_error_t *err = (hdrg_jpeg_error_t *)cinfo->err;

    longjmp(err->jump, 1);
}

static void hdrg_jpeg_output_message(j_common_ptr cinfo)
{
    (void)cinfo;
}

/* 4:2:0 or 4:2:2 Y'CbCr, which takes the luma-guided chroma upsampling (else jpegli's R'G'B') */
static bool hdrg_jpeg_subsampled(const struct jpeg_decompress_struct *cinfo)
{
    return cinfo->num_components == 3 && cinfo->jpeg_color_space == JCS_YCbCr &&
           cinfo->comp_info[0].h_samp_factor == 2 &&
           (cinfo->comp_info[0].v_samp_factor == 1 || cinfo->comp_info[0].v_samp_factor == 2) &&
           cinfo->comp_info[1].h_samp_factor == 1 && cinfo->comp_info[1].v_samp_factor == 1 &&
           cinfo->comp_info[2].h_samp_factor == 1 && cinfo->comp_info[2].v_samp_factor == 1;
}

static void hdrg_jpeg_raw_free(const hdrg_plugin_api_t *api, hdrg_jpeg_raw_t *raw)
{
    if (raw == NULL) {
        return;
    }

    api->mem_free(api, raw->luma);
    api->mem_free(api, raw->rgb);
    api->mem_free(api, raw);
}

static hdrg_jpeg_raw_t *hdrg_jpeg_raw_create(const hdrg_plugin_api_t *api,
                                             const struct jpeg_decompress_struct *cinfo,
                                             uint32_t fit)
{
    hdrg_jpeg_raw_t *raw;
    size_t chroma_rows;

    raw = (hdrg_jpeg_raw_t *)api->mem_calloc(api, 1, sizeof(hdrg_jpeg_raw_t));
    if (raw == NULL) {
        return NULL;
    }

    raw->vfactor = (uint32_t)cinfo->comp_info[0].v_samp_factor;
    raw->fit = fit;
    raw->imcu = raw->vfactor * DCTSIZE;
    raw->capacity = (HDRG_JPEG_STRIP + 2u) * raw->imcu + 2u * HDRG_JPEG_MARGIN;
    raw->luma_stride = (size_t)cinfo->comp_info[0].width_in_blocks * DCTSIZE;
    raw->chroma_stride = (size_t)cinfo->comp_info[1].width_in_blocks * DCTSIZE;
    chroma_rows = raw->capacity / raw->vfactor;

    raw->luma = (uint16_t *)api->mem_alloc(
        api, ((size_t)raw->capacity * raw->luma_stride + 2u * chroma_rows * raw->chroma_stride) *
                 sizeof(uint16_t));
    raw->rgb = (uint16_t *)api->mem_alloc(api, (size_t)raw->capacity * cinfo->output_width * 3u *
                                                   sizeof(uint16_t));
    if (raw->luma == NULL || raw->rgb == NULL) {
        hdrg_jpeg_raw_free(api, raw);

        return NULL;
    }

    raw->chroma[0] = raw->luma + (size_t)raw->capacity * raw->luma_stride;
    raw->chroma[1] = raw->chroma[0] + chroma_rows * raw->chroma_stride;

    return raw;
}

/* the next iMCU row into the strip's planes */
static bool hdrg_jpeg_raw_read(struct jpeg_decompress_struct *cinfo, hdrg_jpeg_raw_t *raw)
{
    JSAMPROW rows[3][2u * DCTSIZE];
    JSAMPARRAY planes[3];
    uint32_t i;
    uint8_t c;

    for (i = 0; i < raw->imcu; i++) {
        rows[0][i] = (JSAMPROW)(void *)(raw->luma + (size_t)(raw->filled + i) * raw->luma_stride);
    }

    for (c = 0; c < 2u; c++) {
        for (i = 0; i < DCTSIZE; i++) {
            rows[1u + c][i] =
                (JSAMPROW)(void *)(raw->chroma[c] +
                                   (size_t)(raw->filled / raw->vfactor + i) * raw->chroma_stride);
        }
    }

    for (c = 0; c < 3u; c++) {
        planes[c] = rows[c];
    }

    if (jpeg_read_raw_data(cinfo, planes, raw->imcu) != raw->imcu) {
        return false;
    }

    raw->filled += raw->imcu;

    return true;
}

/*
 * The pixels of a 4:2:0 / 4:2:2 JPEG through the luma-guided chroma
 * upsampling (the table's yuv_to_rgb16, JFIF siting): the planes come raw at
 * 16 bits, a strip of HDRG_JPEG_STRIP iMCU rows at a time is converted with
 * HDRG_JPEG_MARGIN luma rows of context on each side (so every pixel is the
 * whole picture's) and rounded once to 8 bits
 */
static hdrg_status_t hdrg_jpeg_read_strips(const hdrg_plugin_api_t *api,
                                           struct jpeg_decompress_struct *cinfo,
                                           hdrg_jpeg_raw_t *raw, uint8_t *pixels, size_t stride)
{
    const uint32_t height = cinfo->output_height, width = cinfo->output_width;
    const void *planes[3];
    hdrg_status_t status;
    uint32_t emitted, end, need, first, top, y, keep, desc[HDRG_YUVD_WORDS];
    uint16_t *src;
    uint8_t *dst;
    size_t x, row, strides[3];

    emitted = 0;
    row = (size_t)width * 3u;

    while (emitted < height) {
        end = (emitted + HDRG_JPEG_STRIP * raw->imcu < height)
                  ? emitted + HDRG_JPEG_STRIP * raw->imcu
                  : height;
        need = (end + HDRG_JPEG_MARGIN < height) ? end + HDRG_JPEG_MARGIN : height;

        while (raw->base + raw->filled < need) {
            if (!hdrg_jpeg_raw_read(cinfo, raw)) {
                return HDRG_ERR_DECODE;
            }
        }

        first = (emitted > HDRG_JPEG_MARGIN) ? emitted - HDRG_JPEG_MARGIN : 0u;
        top = (raw->base + raw->filled < height) ? raw->base + raw->filled : height;

        planes[0] = raw->luma + (size_t)(first - raw->base) * raw->luma_stride;
        planes[1] =
            raw->chroma[0] + (size_t)((first - raw->base) / raw->vfactor) * raw->chroma_stride;
        planes[2] =
            raw->chroma[1] + (size_t)((first - raw->base) / raw->vfactor) * raw->chroma_stride;
        strides[0] = raw->luma_stride * sizeof(uint16_t);
        strides[1] = raw->chroma_stride * sizeof(uint16_t);
        strides[2] = strides[1];
        desc[HDRG_YUVD_WIDTH] = width;
        desc[HDRG_YUVD_HEIGHT] = top - first;
        desc[HDRG_YUVD_LAYOUT] = (raw->vfactor == 2u) ? HDRG_YUV_420 : HDRG_YUV_422;
        desc[HDRG_YUVD_MATRIX] = HDRG_MATRIX_BT601;
        desc[HDRG_YUVD_BITS] = 16;
        desc[HDRG_YUVD_SITING] = HDRG_CHROMA_CENTER;
        desc[HDRG_YUVD_FLAGS] = HDRG_YUVF_FULL_RANGE | HDRG_YUVF_JFIF;

        status = api->yuv_to_rgb16(api, desc, planes, strides, raw->fit, raw->rgb,
                                   row * sizeof(uint16_t));
        if (status != HDRG_OK) {
            return status;
        }

        for (y = emitted; y < end; y++) {
            src = raw->rgb + (size_t)(y - first) * row;
            dst = pixels + (size_t)y * stride;

            for (x = 0; x < row; x++) {
                dst[x] = (uint8_t)(((uint32_t)src[x] * 255u + 32767u) / 65535u);
            }
        }

        emitted = end;

        /* keep the rows from the next strip's context on */
        keep = (emitted > HDRG_JPEG_MARGIN) ? emitted - HDRG_JPEG_MARGIN : 0u;
        if (emitted < height && keep > raw->base) {
            memmove(raw->luma, raw->luma + (size_t)(keep - raw->base) * raw->luma_stride,
                    (size_t)(raw->base + raw->filled - keep) * raw->luma_stride * sizeof(uint16_t));
            memmove(raw->chroma[0],
                    raw->chroma[0] +
                        (size_t)((keep - raw->base) / raw->vfactor) * raw->chroma_stride,
                    (size_t)((raw->base + raw->filled - keep) / raw->vfactor) * raw->chroma_stride *
                        sizeof(uint16_t));
            memmove(raw->chroma[1],
                    raw->chroma[1] +
                        (size_t)((keep - raw->base) / raw->vfactor) * raw->chroma_stride,
                    (size_t)((raw->base + raw->filled - keep) / raw->vfactor) * raw->chroma_stride *
                        sizeof(uint16_t));
            raw->filled -= keep - raw->base;
            raw->base = keep;
        }
    }

    return HDRG_OK;
}

static uint32_t HDRG_PLUGIN_CALL hdrg_jpeg_probe(const uint8_t *data, size_t size,
                                                 const hdrg_plugin_api_t *api)
{
    (void)api;

    return (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) ? 1u : 0u;
}

/* the picture: its ICC (wins), the first Exif APP1; the color is the host's otherwise */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jpeg_read(hdrg_reader_t *reader, const uint8_t *data,
                                                     size_t size, const hdrg_plugin_api_t *api)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_marker_struct *marker;
    hdrg_jpeg_error_t error;
    volatile hdrg_status_t status; /* set after setjmp, read after a longjmp */
    hdrg_jpeg_raw_t *volatile raw; /* read after a longjmp (C11 7.13.2.1) */
    JOCTET *icc_data;
    uint64_t max_pixels;
    uint32_t fit;
    unsigned int icc_len;
    uint8_t *row, *pixels;
    size_t stride;
    volatile bool started, exif;

    status = HDRG_ERR_DECODE;
    icc_data = NULL;
    raw = NULL;
    started = false;
    exif = false;

    (void)api->read_limits(reader, &max_pixels, &fit);

    cinfo.err = jpeg_std_error(&error.mgr);
    error.mgr.error_exit = hdrg_jpeg_error_exit;
    error.mgr.output_message = hdrg_jpeg_output_message;

    if (setjmp(error.jump)) {
        goto fail;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)size);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        goto fail;
    }

    /* before jpegli allocates for the picture (a progressive file's coefficients) */
    if ((uint64_t)cinfo.image_width * cinfo.image_height > max_pixels) {
        status = HDRG_ERR_LIMIT;

        goto fail;
    }

    /*
     * jpegli only allows ICC extraction while the decoder is mid-stream. The
     * buffer is released right here, so fail: never reads icc_data after a
     * longjmp
     */
    if (!jpeg_read_icc_profile(&cinfo, &icc_data, &icc_len)) {
        icc_len = 0;
    }

    status = (icc_len > 0) ? api->read_set_icc(reader, icc_data, icc_len) : HDRG_OK;
    free(icc_data); /* the dependency's: jpeg_read_icc_profile's, by malloc */

    if (status != HDRG_OK) {
        goto fail;
    }

    for (marker = cinfo.marker_list; marker != NULL; marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 1 && marker->data_length > 6 &&
            memcmp(marker->data, "Exif\0\0", 6) == 0 && !exif) {
            status = api->read_set_exif(reader, marker->data + 6, marker->data_length - 6u);
            if (status != HDRG_OK) {
                goto fail;
            }

            exif = true;
        }
    }

    status = HDRG_ERR_DECODE;
    cinfo.out_color_space = JCS_RGB;

    if (hdrg_jpeg_subsampled(&cinfo)) {
        cinfo.raw_data_out = TRUE;
        jpegli_set_output_format(&cinfo, JPEGLI_TYPE_UINT16, JPEGLI_NATIVE_ENDIAN);
    }

    if (!jpeg_start_decompress(&cinfo)) {
        goto fail;
    }

    started = true;

    if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_components != 3) {
        goto fail;
    }

    status = api->read_image(reader, cinfo.output_width, cinfo.output_height, 8, &pixels, &stride);
    if (status != HDRG_OK) {
        goto fail;
    }

    if (cinfo.raw_data_out) {
        raw = hdrg_jpeg_raw_create(api, &cinfo, fit);
        if (raw == NULL) {
            status = HDRG_ERR_NOMEM;

            goto fail;
        }

        status = hdrg_jpeg_read_strips(api, &cinfo, raw, pixels, stride);
        if (status != HDRG_OK) {
            goto fail;
        }
    }

    status = HDRG_ERR_DECODE;

    while (!cinfo.raw_data_out && cinfo.output_scanline < cinfo.output_height) {
        row = pixels + (size_t)cinfo.output_scanline * stride;
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
            goto fail;
        }
    }

    (void)jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    hdrg_jpeg_raw_free(api, raw);

    return HDRG_OK;

fail:
    if (started) {
        jpeg_abort_decompress(&cinfo);
    }

    jpeg_destroy_decompress(&cinfo);
    hdrg_jpeg_raw_free(api, raw);

    return status;
}

/*
 * a compressor of a picture of components (3: RGB at 4:4:4, 1: gray) at quality into its memory;
 * false when jpegli fails
 */
static bool hdrg_jpeg_enc_start(hdrg_jpeg_enc_t *enc, uint32_t width, uint32_t height,
                                uint8_t components, uint8_t quality)
{
    struct jpeg_compress_struct *cinfo = &enc->cinfo;

    enc->cinfo.err = jpegli_std_error(&enc->error.mgr);
    enc->error.mgr.error_exit = hdrg_jpeg_error_exit;
    enc->error.mgr.output_message = hdrg_jpeg_output_message;

    if (setjmp(enc->error.jump)) {
        enc->failed = true;

        return false;
    }

    jpegli_create_compress(cinfo);
    enc->created = true;
    jpegli_mem_dest(cinfo, &enc->mem, &enc->mem_size);

    cinfo->image_width = width;
    cinfo->image_height = height;
    cinfo->input_components = components;
    cinfo->in_color_space = (components == 1) ? JCS_GRAYSCALE : JCS_RGB;

    jpegli_set_defaults(cinfo);
    jpegli_set_quality(cinfo, quality, TRUE);

    /* a JFIF APP0 first (the iPhone's files have it; jpegli leaves it out) */
    cinfo->write_JFIF_header = TRUE;

    if (components == 3) {
        /* PQ-encoded pixels: chroma carries small-amplitude but
         * display-expanded detail — never subsample */
        cinfo->comp_info[0].h_samp_factor = 1;
        cinfo->comp_info[0].v_samp_factor = 1;
    }

    /* jpegli's adaptive quantization is tuned for gamma-domain masking; on
     * low-amplitude PQ-domain signals it over-quantizes flat detail
     * (measured -5% detail retention). Disable it. */
    jpegli_enable_adaptive_quantization(cinfo, FALSE);

    jpegli_start_compress(cinfo, TRUE);

    return true;
}

static bool hdrg_jpeg_enc_rows(hdrg_jpeg_enc_t *enc, const uint8_t *pixels, uint32_t rows,
                               size_t stride)
{
    JSAMPROW row;
    uint32_t i;

    if (enc->failed) {
        return false;
    }

    if (setjmp(enc->error.jump)) {
        enc->failed = true;

        return false;
    }

    for (i = 0; i < rows; i++) {
        row = (JSAMPROW)(pixels + (size_t)i * stride);
        (void)jpegli_write_scanlines(&enc->cinfo, &row, 1);
    }

    return true;
}

/* the finished stream in enc->mem (enc->mem_size bytes) */
static bool hdrg_jpeg_enc_finish(hdrg_jpeg_enc_t *enc)
{
    if (enc->failed || enc->cinfo.next_scanline < enc->cinfo.image_height) {
        return false;
    }

    if (setjmp(enc->error.jump)) {
        enc->failed = true;

        return false;
    }

    jpegli_finish_compress(&enc->cinfo);

    return true;
}

static void hdrg_jpeg_enc_free(hdrg_jpeg_enc_t *enc)
{
    if (enc->created) {
        jpegli_destroy_compress(&enc->cinfo);
    }

    free(enc->mem); /* the dependency's: jpegli_mem_dest's, by malloc */
    enc->mem = NULL;
    enc->created = false;
}

/* an option of the writer's (declared, so present) */
static int64_t hdrg_jpeg_option(const hdrg_plugin_api_t *api, const hdrg_options_t *options,
                                const char *key)
{
    int64_t value;

    value = 0;
    (void)api->option_get_int(options, key, strlen(key), &value);

    return value;
}

/* the base's profile of the host's (none: an sRGB base); *icc freed by the caller */
static hdrg_status_t hdrg_jpeg_icc(const hdrg_plugin_api_t *api, const hdrg_writer_t *writer,
                                   const hdrg_jpeg_writer_t *inst, bool hagc, uint8_t **icc,
                                   size_t *size)
{
    hdrg_status_t status;

    *size = 0;

    /* made once into the room of one APP2 segment (a larger profile goes in none) */
    *icc = (uint8_t *)api->mem_alloc(api, HDRG_JPEG_ICC_MAX);
    if (*icc == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->write_icc(writer, hagc ? 1u : 0u, inst->lumi_nits, *icc, HDRG_JPEG_ICC_MAX, size);

    /* no profile (an sRGB base), or one past the segment: none written */
    if (status == HDRG_ERR_NOT_FOUND || (status == HDRG_OK && *size > HDRG_JPEG_ICC_MAX)) {
        api->mem_free(api, *icc);
        *icc = NULL;
        *size = 0;

        return HDRG_OK;
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jpeg_create(hdrg_writer_t *writer,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    hdrg_jpeg_writer_t *state;
    uint32_t width, height;

    state = (hdrg_jpeg_writer_t *)api->mem_calloc(api, 1, sizeof(*state));
    if (state == NULL) {
        return HDRG_ERR_NOMEM;
    }

    *inst = state;
    state->quality = (uint8_t)hdrg_jpeg_option(api, options, "quality");
    state->gainmap_quality = (uint8_t)hdrg_jpeg_option(api, options, "gainmap_quality");
    state->hagc = hdrg_jpeg_option(api, options, "hagc") != 0;
    state->apple_xmp = hdrg_jpeg_option(api, options, "apple_xmp") != 0;
    state->hdrgm_xmp = hdrg_jpeg_option(api, options, "hdrgm_xmp") != 0;
    (void)api->option_get_float(options, "icc_lumi", strlen("icc_lumi"), &state->lumi_nits);
    (void)api->write_image(writer, &width, &height, &state->form, NULL, NULL, NULL);

    /* no ICC or Exif of its own: the muxer adds them (the ICC's HAGC fitted to the rows' light) */
    if (!hdrg_jpeg_enc_start(&state->base, width, height, 3, state->quality)) {
        hdrg_jpeg_enc_free(&state->base);
        api->mem_free(api, state);
        *inst = NULL;

        return HDRG_ERR_ENCODE;
    }

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jpeg_rows(void *inst, uint32_t y0, uint32_t rows,
                                                     const uint8_t *data, size_t stride,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_jpeg_writer_t *state = (hdrg_jpeg_writer_t *)inst;

    (void)y0;
    (void)api;

    return hdrg_jpeg_enc_rows(&state->base, data, rows, stride) ? HDRG_OK : HDRG_ERR_ENCODE;
}

/* the gain map stream of a two-layer form: its JPEG (gray, the gain map quality) */
static hdrg_status_t hdrg_jpeg_gainmap(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                       hdrg_jpeg_writer_t *state, const uint8_t *icc,
                                       size_t icc_size)
{
    const uint8_t *gm_pixels, *tiff;
    hdrg_jpeg_enc_t gm;
    hdrg_jpeg_mux_t mux;
    hdrg_status_t status;
    uint32_t gw, gh;
    uint8_t iso[128];
    size_t gm_stride, iso_size, tiff_size;

    memset(&mux, 0, sizeof(mux));
    memset(&gm, 0, sizeof(gm));

    status = api->write_gainmap(writer, &gm_pixels, &gm_stride, &gw, &gh, mux.values);
    if (status == HDRG_OK) {
        status = api->write_iso21496(writer, iso, sizeof(iso), &iso_size);
    }

    if (status == HDRG_OK && iso_size > sizeof(iso)) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_jpeg_enc_start(&gm, gw, gh, 1, state->gainmap_quality) ||
        !hdrg_jpeg_enc_rows(&gm, gm_pixels, gh, gm_stride) || !hdrg_jpeg_enc_finish(&gm)) {
        hdrg_jpeg_enc_free(&gm);

        return HDRG_ERR_ENCODE;
    }

    if (api->write_exif(writer, &tiff, &tiff_size) == HDRG_OK) {
        mux.exif = tiff;
        mux.exif_size = tiff_size;
    }

    mux.base = state->base.mem;
    mux.base_size = state->base.mem_size;
    mux.gainmap = gm.mem;
    mux.gainmap_size = gm.mem_size;
    mux.icc = icc;
    mux.icc_size = icc_size;
    mux.iso = iso;
    mux.iso_size = iso_size;

    /* an SDR base's alone: Apple's and hdrgm's semantics assume one */
    if (state->form == HDRG_OUTPUT_GAINMAP) {
        mux.apple_xmp = state->apple_xmp;
        mux.hdrgm_xmp = state->hdrgm_xmp;
    }

    status = hdrg_jpeg_mux_gainmap(api, writer, &mux);
    hdrg_jpeg_enc_free(&gm);

    return status;
}

/*
 * GAINMAP: the SDR base with a Display P3 profile when P3 (none for sRGB) and the up map;
 * HYBRID: the PQ / P3 base with its "P3 PQ" profile (HAGC) and the down map, ISO 21496-1 metadata
 * alone; PQ: the base with its profile
 */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jpeg_finish(void *inst, hdrg_writer_t *writer,
                                                       const hdrg_plugin_api_t *api)
{
    const uint8_t *tiff;
    hdrg_jpeg_writer_t *state = (hdrg_jpeg_writer_t *)inst;
    hdrg_status_t status;
    uint8_t *icc;
    size_t icc_size, tiff_size;

    if (!hdrg_jpeg_enc_finish(&state->base)) {
        return HDRG_ERR_ENCODE;
    }

    status = hdrg_jpeg_icc(api, writer, state, state->hagc && state->form != HDRG_OUTPUT_GAINMAP,
                           &icc, &icc_size);
    if (status != HDRG_OK) {
        api->mem_free(api, icc);

        return status;
    }

    if (state->form != HDRG_OUTPUT_PQ) {
        status = hdrg_jpeg_gainmap(api, writer, state, icc, icc_size);
    } else {
        if (api->write_exif(writer, &tiff, &tiff_size) != HDRG_OK) {
            tiff = NULL;
            tiff_size = 0;
        }

        status = hdrg_jpeg_mux_single(api, writer, state->base.mem, state->base.mem_size, icc,
                                      icc_size, tiff, tiff_size);
    }

    api->mem_free(api, icc);

    return status;
}

static void HDRG_PLUGIN_CALL hdrg_jpeg_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_jpeg_writer_t *state = (hdrg_jpeg_writer_t *)inst;

    if (state == NULL) {
        return;
    }

    hdrg_jpeg_enc_free(&state->base);
    api->mem_free(api, state);
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_jpeg_init(hdrg_plugin_t *plugin,
                                                           const hdrg_plugin_api_t *api)
{
    const uint32_t forms = HDRG_CODEC_FORM(HDRG_OUTPUT_GAINMAP) |
                           HDRG_CODEC_FORM(HDRG_OUTPUT_HYBRID) | HDRG_CODEC_FORM(HDRG_OUTPUT_PQ);
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_CODEC_LIT(HDRG_JPEG_ID), HDRG_CODEC_LIT("JPEG"),
                                  HDRG_CODEC_LIT("1.0.0"), HDRG_CODEC_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_CODEC_LIT(HDRG_JPEG_LICENSE),
                                         HDRG_CODEC_LIT(HDRG_JPEG_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_CODEC_LIT(HDRG_JPEG_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_CODEC_LIT("jpg,jpeg"));
    if (status == HDRG_OK) {
        status = api->class_set_reader(cls, hdrg_jpeg_probe, hdrg_jpeg_read);
    }

    if (status == HDRG_OK) {
        status = api->class_set_writer(cls, forms, HDRG_OUTPUT_GAINMAP, HDRG_CODEC_ROWS_RGB8,
                                       HDRG_CODEC_COLOR_ICC, HDRG_JPEG_MAX_SIDE, hdrg_jpeg_create,
                                       hdrg_jpeg_rows, hdrg_jpeg_finish, hdrg_jpeg_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_CODEC_LIT("quality"), 98, 1, 100,
            HDRG_CODEC_LIT("the base's quality (4:4:4, no adaptive quantization)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_CODEC_LIT("gainmap_quality"), 90, 1, 100,
                                           HDRG_CODEC_LIT("the gain map stream's quality"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_CODEC_LIT("hagc"), 1, 0, 1,
            HDRG_CODEC_LIT("HYBRID and PQ: the ST 2094-50 gain curve in the profile's HAGC tag"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_CODEC_LIT("icc_lumi"), 0.0, 0.0, 10000.0,
            HDRG_CODEC_LIT("the profile's luminance tag (reference white, cd/m2; 0: none)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_CODEC_LIT("apple_xmp"), 1, 0, 1,
            HDRG_CODEC_LIT("GAINMAP: Apple's HDRGainMap XMP on the gain map stream"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_CODEC_LIT("hdrgm_xmp"), 1, 0, 1,
            HDRG_CODEC_LIT("GAINMAP: Adobe / Google hdrgm XMP and the GContainer"));
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
