/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "codec_host.h"

#include <string.h>

#include "../core/yuv.h"
#include "../priv.h"
#include "defaults.h"

#define HDRG_READER_MAGIC UINT32_C(0x48524452)
#define HDRG_WRITER_MAGIC UINT32_C(0x48575254)
/* H.273 transfer characteristics of HDR signals: SMPTE ST 2084 (PQ), ARIB STD-B67 (HLG) */
#define HDRG_CODECS_TRANSFER_PQ  16u
#define HDRG_CODECS_TRANSFER_HLG 18u
/* ISO 21496-1's offsets of the library's gain maps */
#define HDRG_CODECS_GAINMAP_OFFSET 0.00001

/* run_parallel's work on the pool */
typedef struct {
    hdrg_parallel_fn_t fn;
    void *user;
} hdrg_codecs_task_t;

/* a table the plugins received is its host's first member */
static inline const hdrg_plugin_host_t *hdrg_codecs_host(const hdrg_plugin_api_t *api)
{
    return (const hdrg_plugin_host_t *)(const void *)api;
}

static inline bool hdrg_reader_ok(const hdrg_reader_t *reader)
{
    return reader != NULL && reader->magic == HDRG_READER_MAGIC;
}

static inline bool hdrg_writer_ok(const hdrg_writer_t *writer)
{
    return writer != NULL && writer->magic == HDRG_WRITER_MAGIC;
}

/* bytes into a caller's buffer: needed their size, nothing written when cap is smaller */
static hdrg_status_t hdrg_codecs_copy_out(const uint8_t *data, size_t size, uint8_t *buf,
                                          size_t cap, size_t *needed)
{
    if (needed == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *needed = size;

    if (buf != NULL && cap >= size) {
        memcpy(buf, data, size);
    }

    return HDRG_OK;
}

static void hdrg_codecs_task(uint32_t task_index, uint32_t worker_index, void *arg)
{
    const hdrg_codecs_task_t *task = (const hdrg_codecs_task_t *)arg;

    task->fn(task->user, task_index, worker_index);
}

/* the Y'CbCr description of the ABI's words; false for an invalid one */
static bool hdrg_codecs_yuv(const uint32_t *desc, hdrg_yuv_t *out)
{
    memset(out, 0, sizeof(*out));

    if (desc == NULL || desc[HDRG_YUVD_LAYOUT] > UINT8_MAX || desc[HDRG_YUVD_MATRIX] > UINT8_MAX ||
        desc[HDRG_YUVD_BITS] > UINT8_MAX || desc[HDRG_YUVD_SITING] > HDRG_CHROMA_BOTTOM ||
        (desc[HDRG_YUVD_FLAGS] &
         ~(HDRG_YUVF_FULL_RANGE | HDRG_YUVF_JFIF | HDRG_YUVF_INTERLEAVED | HDRG_YUVF_MSB)) != 0) {
        return false;
    }

    out->width = desc[HDRG_YUVD_WIDTH];
    out->height = desc[HDRG_YUVD_HEIGHT];
    out->layout = (uint8_t)desc[HDRG_YUVD_LAYOUT];
    out->matrix = (uint8_t)desc[HDRG_YUVD_MATRIX];
    out->bits = (uint8_t)desc[HDRG_YUVD_BITS];
    out->siting = (uint8_t)desc[HDRG_YUVD_SITING];
    out->full_range = (desc[HDRG_YUVD_FLAGS] & (HDRG_YUVF_FULL_RANGE | HDRG_YUVF_JFIF)) != 0;
    out->jfif = (desc[HDRG_YUVD_FLAGS] & HDRG_YUVF_JFIF) != 0;
    out->interleaved = (desc[HDRG_YUVD_FLAGS] & HDRG_YUVF_INTERLEAVED) != 0;
    out->msb = (desc[HDRG_YUVD_FLAGS] & HDRG_YUVF_MSB) != 0;

    return hdrg_yuv_valid(out);
}

/* the decoded picture's color when the reader left it to the host, and its orientation */
static hdrg_status_t hdrg_codecs_resolve(const hdrg_reader_t *reader)
{
    const hdrg_allocator_t *alloc = &reader->host->alloc;
    const uint8_t *tiff;
    hdrg_decoded_t *out = reader->out;
    size_t tiff_size;

    tiff = (out->exif != NULL) ? out->exif + 6 : NULL;
    tiff_size = (out->exif != NULL) ? out->exif_size - 6u : 0u;

    if (!reader->has_orientation && tiff != NULL) {
        out->orientation = hdrg_exif_orientation(tiff, tiff_size);
    }

    if (out->icc != NULL) {
        return hdrg_decoded_icc_color(alloc, out);
    }

    if (reader->has_color) {
        return HDRG_OK;
    }

    if (tiff != NULL && hdrg_exif_adobe_rgb(tiff, tiff_size)) {
        (void)hdrg_color_from_codes(HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB, &out->color);
    } else {
        (void)hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &out->color);
    }

    return HDRG_OK;
}

extern uint32_t HDRG_PLUGIN_CALL hdrg_api_run_workers(const hdrg_plugin_api_t *api)
{
    uint32_t workers;

    if (api == NULL) {
        return 1;
    }

    workers = hdrg_exec_concurrency(&hdrg_codecs_host(api)->exec);

    return (workers > 0) ? workers : 1u;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_run_parallel(const hdrg_plugin_api_t *api,
                                                            uint32_t count, hdrg_parallel_fn_t fn,
                                                            void *user)
{
    const hdrg_exec_t *exec;
    hdrg_codecs_task_t task;
    uint32_t i;

    if (api == NULL || fn == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    exec = &hdrg_codecs_host(api)->exec;

    if (exec->pool == NULL || exec->max_workers <= 1 || count <= 1) {
        for (i = 0; i < count; i++) {
            fn(user, i, 0);
        }

        return HDRG_OK;
    }

    task.fn = fn;
    task.user = user;
    hdrg_pool_parallel_for(exec->pool, count, exec->max_workers, hdrg_codecs_task, &task);

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_yuv_to_rgb16(const hdrg_plugin_api_t *api,
                                                            const uint32_t *desc,
                                                            const void *const *planes,
                                                            const size_t *strides, uint32_t fit,
                                                            uint16_t *rgb, size_t stride)
{
    const hdrg_plugin_host_t *host;
    hdrg_yuv_t yuv;
    uint8_t c;
    size_t scratch_size;
    float *scratch;

    if (api == NULL || planes == NULL || strides == NULL || rgb == NULL ||
        fit > HDRG_CHROMA_FIT_SEGMENTED || !hdrg_codecs_yuv(desc, &yuv)) {
        return HDRG_ERR_INVALID_ARG;
    }

    for (c = 0; c < 3u; c++) {
        yuv.planes[c] = planes[c];
        yuv.strides[c] = strides[c];
    }

    yuv.fit = (uint8_t)fit;

    if (!hdrg_yuv_fits(&yuv) || stride < (size_t)yuv.width * 3u * sizeof(uint16_t)) {
        return HDRG_ERR_INVALID_ARG;
    }

    host = hdrg_codecs_host(api);
    scratch = NULL;
    scratch_size = hdrg_yuv_scratch(&yuv);

    if (scratch_size > 0) {
        scratch = (float *)hdrg_mem_alloc(&host->alloc, scratch_size * sizeof(float));
        if (scratch == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    hdrg_yuv_to_rgb16(&host->exec, &yuv, scratch, rgb, stride);
    hdrg_mem_free(&host->alloc, scratch);

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL
hdrg_api_rgb16_to_yuv(const hdrg_plugin_api_t *api, const uint32_t *desc, void *const *planes,
                      const size_t *strides, const uint16_t *rgb, uint32_t y0, uint32_t rows)
{
    hdrg_yuv_t yuv;
    uint8_t c;

    if (api == NULL || planes == NULL || strides == NULL || rgb == NULL ||
        !hdrg_codecs_yuv(desc, &yuv) || y0 > yuv.height || rows > yuv.height - y0) {
        return HDRG_ERR_INVALID_ARG;
    }

    for (c = 0; c < 3u; c++) {
        yuv.planes[c] = planes[c];
        yuv.strides[c] = strides[c];
    }

    if (!hdrg_yuv_fits(&yuv)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (yuv.layout == HDRG_YUV_444) {
        hdrg_rgb16_to_yuv444(&hdrg_codecs_host(api)->exec, rgb, y0, rows, &yuv);

        return HDRG_OK;
    }

    if (yuv.layout != HDRG_YUV_420 || (y0 & 1u) != 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_rgb16_to_yuv420(&hdrg_codecs_host(api)->exec, rgb, y0, rows, &yuv);

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_users(const hdrg_reader_t *reader,
                                                          void **plugin_user, void **class_user)
{
    if (!hdrg_reader_ok(reader)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (plugin_user != NULL) {
        *plugin_user = reader->cls->plugin->user;
    }

    if (class_user != NULL) {
        *class_user = reader->cls->user;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_limits(const hdrg_reader_t *reader,
                                                           uint64_t *max_pixels,
                                                           uint32_t *chroma_fit)
{
    if (!hdrg_reader_ok(reader)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (max_pixels != NULL) {
        *max_pixels = reader->max_pixels;
    }

    if (chroma_fit != NULL) {
        *chroma_fit = reader->fit;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_image(hdrg_reader_t *reader, uint32_t width,
                                                          uint32_t height, uint32_t bits,
                                                          uint8_t **pixels, size_t *stride)
{
    hdrg_decoded_t *out;
    uint64_t row;

    if (!hdrg_reader_ok(reader) || pixels == NULL || stride == NULL || width == 0 || height == 0 ||
        (bits != 8 && bits != 16)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (reader->has_image) {
        return HDRG_ERR_INVALID_STATE;
    }

    if ((uint64_t)width * height > reader->max_pixels) {
        return HDRG_ERR_LIMIT;
    }

    row = (uint64_t)width * 3u * (bits / 8u);
    if (row * height > (uint64_t)SIZE_MAX) {
        return HDRG_ERR_LIMIT;
    }

    out = reader->out;
    out->pixels = (uint8_t *)hdrg_mem_alloc(&reader->host->alloc, (size_t)(row * height));
    if (out->pixels == NULL) {
        return HDRG_ERR_NOMEM;
    }

    out->width = width;
    out->height = height;
    out->bit_depth = bits;
    out->stride = (size_t)row;
    reader->has_image = true;
    *pixels = out->pixels;
    *stride = out->stride;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_cicp(hdrg_reader_t *reader,
                                                             uint32_t primaries, uint32_t transfer)
{
    hdrg_color_t color;

    if (!hdrg_reader_ok(reader)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (transfer == HDRG_CODECS_TRANSFER_PQ || transfer == HDRG_CODECS_TRANSFER_HLG) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    if (primaries > UINT8_MAX || transfer > UINT8_MAX ||
        !hdrg_color_from_codes((uint8_t)primaries, (uint8_t)transfer, &color)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_color_free(&reader->host->alloc, &reader->out->color);
    reader->out->color = color;
    reader->has_color = true;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_xy(hdrg_reader_t *reader, const double *xy,
                                                           double gamma)
{
    hdrg_color_t color;
    uint8_t c;

    if (!hdrg_reader_ok(reader) || !(gamma == 0.0 || (gamma >= 0.1 && gamma <= 10.0))) {
        return HDRG_ERR_INVALID_ARG;
    }

    (void)hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color);

    if (xy != NULL && !hdrg_color_set_xy(xy, &color)) {
        return HDRG_ERR_INVALID_ARG;
    }

    for (c = 0; gamma > 0.0 && c < 3u; c++) {
        hdrg_curve_set_gamma(gamma, &color.curve[c]);
    }

    if (gamma > 0.0) {
        color.srgb_curve = false;
        color.transfer = 0;
    }

    hdrg_color_snap(&reader->host->alloc, &color);
    hdrg_color_free(&reader->host->alloc, &reader->out->color);
    reader->out->color = color;
    reader->has_color = true;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_icc(hdrg_reader_t *reader,
                                                            const uint8_t *icc, size_t size)
{
    uint8_t *copy;

    if (!hdrg_reader_ok(reader) || icc == NULL || size == 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    copy = (uint8_t *)hdrg_mem_alloc(&reader->host->alloc, size);
    if (copy == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memcpy(copy, icc, size);
    hdrg_mem_free(&reader->host->alloc, reader->out->icc);
    reader->out->icc = copy;
    reader->out->icc_size = size;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_exif(hdrg_reader_t *reader,
                                                             const uint8_t *tiff, size_t size)
{
    uint8_t *copy;

    if (!hdrg_reader_ok(reader) || tiff == NULL || size == 0 || size > SIZE_MAX - 6u) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* kept with its JPEG APP1 header, as the writers take it */
    copy = (uint8_t *)hdrg_mem_alloc(&reader->host->alloc, size + 6u);
    if (copy == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memcpy(copy, "Exif\0\0", 6);
    memcpy(copy + 6, tiff, size);
    hdrg_mem_free(&reader->host->alloc, reader->out->exif);
    reader->out->exif = copy;
    reader->out->exif_size = size + 6u;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_orientation(hdrg_reader_t *reader,
                                                                    uint32_t orientation)
{
    if (!hdrg_reader_ok(reader) || orientation < 1 || orientation > 8) {
        return HDRG_ERR_INVALID_ARG;
    }

    reader->out->orientation = (uint16_t)orientation;
    reader->has_orientation = true;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_users(const hdrg_writer_t *writer,
                                                           void **plugin_user, void **class_user)
{
    if (!hdrg_writer_ok(writer)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (plugin_user != NULL) {
        *plugin_user = writer->cls->plugin->user;
    }

    if (class_user != NULL) {
        *class_user = writer->cls->user;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_image(const hdrg_writer_t *writer,
                                                           uint32_t *width, uint32_t *height,
                                                           uint32_t *form, uint32_t *primaries,
                                                           uint32_t *transfer, uint32_t *threads)
{
    if (!hdrg_writer_ok(writer)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (width != NULL) {
        *width = writer->width;
    }

    if (height != NULL) {
        *height = writer->height;
    }

    if (form != NULL) {
        *form = writer->form;
    }

    if (primaries != NULL) {
        *primaries = writer->primaries;
    }

    if (transfer != NULL) {
        *transfer = writer->transfer;
    }

    if (threads != NULL) {
        *threads = writer->threads;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_exif(const hdrg_writer_t *writer,
                                                          const uint8_t **tiff, size_t *size)
{
    if (!hdrg_writer_ok(writer) || tiff == NULL || size == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *tiff = writer->exif;
    *size = writer->exif_size;

    return (writer->exif != NULL) ? HDRG_OK : HDRG_ERR_NOT_FOUND;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_icc(const hdrg_writer_t *writer, uint32_t hagc,
                                                         double lumi_nits, uint8_t *buf, size_t cap,
                                                         size_t *needed)
{
    hdrg_status_t status;
    uint8_t *icc;
    size_t size;

    if (!hdrg_writer_ok(writer) || needed == NULL || hagc > 1u ||
        !(lumi_nits >= 0.0 && lumi_nits <= 10000.0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (writer->cls->color != HDRG_CODEC_COLOR_ICC || writer->icc == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (writer->primaries == HDRG_PRIMARIES_BT709 && writer->transfer == HDRG_TRANSFER_SRGB) {
        return HDRG_ERR_NOT_FOUND;
    }

    status = writer->icc(writer->icc_user, hagc != 0 && writer->finishing, (float)lumi_nits, &icc,
                         &size);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_codecs_copy_out(icc, size, buf, cap, needed);
    hdrg_mem_free(&writer->host->alloc, icc);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_gainmap(const hdrg_writer_t *writer,
                                                             const uint8_t **pixels, size_t *stride,
                                                             uint32_t *width, uint32_t *height,
                                                             double *values)
{
    if (!hdrg_writer_ok(writer) || pixels == NULL || stride == NULL || width == NULL ||
        height == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (writer->gm_pixels == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    *pixels = writer->gm_pixels;
    *stride = writer->gm_stride;
    *width = writer->gm_width;
    *height = writer->gm_height;

    if (values != NULL) {
        memcpy(values, writer->gm_values, sizeof(writer->gm_values));
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_alternate(const hdrg_writer_t *writer,
                                                               uint32_t *primaries,
                                                               uint32_t *transfer, uint32_t *bits)
{
    if (!hdrg_writer_ok(writer) || primaries == NULL || transfer == NULL || bits == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (writer->gm_pixels == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    *primaries = writer->alt_primaries;
    *transfer = writer->alt_transfer;
    *bits = writer->alt_bits;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_iso21496(const hdrg_writer_t *writer,
                                                              uint8_t *buf, size_t cap,
                                                              size_t *needed)
{
    uint8_t body[HDRG_ISO_BODY_BYTES];

    if (!hdrg_writer_ok(writer)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (writer->gm_pixels == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    hdrg_gainmap_iso_body(writer->gm_values, body);

    return hdrg_codecs_copy_out(body, sizeof(body), buf, cap, needed);
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_apple_xmp(const hdrg_writer_t *writer,
                                                               uint8_t *buf, size_t cap,
                                                               size_t *needed)
{
    size_t size;
    char xmp[2048];

    if (!hdrg_writer_ok(writer)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (writer->gm_pixels == NULL || writer->form != HDRG_OUTPUT_GAINMAP) {
        return HDRG_ERR_NOT_FOUND;
    }

    size = hdrg_gainmap_apple_xmp(writer->gm_values[HDRG_GAINMAP_ALT_HEADROOM], xmp, sizeof(xmp));
    if (size == 0) {
        return HDRG_ERR_INTERNAL;
    }

    return hdrg_codecs_copy_out((const uint8_t *)xmp, size, buf, cap, needed);
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_light(const hdrg_writer_t *writer,
                                                           uint32_t *max_cll, uint32_t *max_fall,
                                                           uint32_t *peak)
{
    if (!hdrg_writer_ok(writer)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (max_cll != NULL) {
        *max_cll = writer->max_cll;
    }

    if (max_fall != NULL) {
        *max_fall = writer->max_fall;
    }

    if (peak != NULL) {
        *peak = writer->peak;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_output(hdrg_writer_t *writer,
                                                            const uint8_t *data, size_t size)
{
    uint8_t *grown;
    size_t cap;

    if (!hdrg_writer_ok(writer) || (data == NULL && size > 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!writer->finishing) {
        return HDRG_ERR_INVALID_STATE;
    }

    /* the file after room for the output buffer's header (handed over as it is: no copy) */
    if (size > SIZE_MAX - sizeof(hdrg_outbuf_header_t) - writer->out_size) {
        return HDRG_ERR_NOMEM;
    }

    if (writer->out_size + size > writer->out_cap) {
        cap = (writer->out_cap > 0) ? writer->out_cap : 4096u;

        while (cap < writer->out_size + size) {
            cap = (cap > (SIZE_MAX - sizeof(hdrg_outbuf_header_t)) / 2u) ? writer->out_size + size
                                                                         : cap * 2u;
        }

        grown = (uint8_t *)hdrg_mem_realloc(&writer->host->alloc, writer->out,
                                            sizeof(hdrg_outbuf_header_t) + cap);
        if (grown == NULL) {
            return HDRG_ERR_NOMEM;
        }

        writer->out = grown;
        writer->out_cap = cap;
    }

    if (size > 0) {
        memcpy(writer->out + sizeof(hdrg_outbuf_header_t) + writer->out_size, data, size);
    }

    writer->out_size += size;

    return HDRG_OK;
}

extern void hdrg_decoded_free(const hdrg_allocator_t *alloc, hdrg_decoded_t *decoded)
{
    if (decoded == NULL) {
        return;
    }

    hdrg_color_free(alloc, &decoded->color);
    hdrg_mem_free(alloc, decoded->pixels);
    hdrg_mem_free(alloc, decoded->icc);
    hdrg_mem_free(alloc, decoded->exif);
    memset(decoded, 0, sizeof(*decoded));
}

extern hdrg_status_t hdrg_codec_read(const hdrg_plugin_host_t *host, const uint8_t *input,
                                     size_t size, uint64_t max_pixels, uint8_t fit,
                                     hdrg_decoded_t *out)
{
    const hdrg_plugin_t *plugin;
    const hdrg_class_t *cls;
    hdrg_reader_t reader;
    hdrg_status_t status;
    uint32_t i, j;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < host->plugin_count; i++) {
        plugin = host->plugins[i];

        for (j = 0; plugin->accepted && j < plugin->class_count; j++) {
            cls = plugin->classes[j];
            if (!cls->registered || cls->kind != HDRG_CLASS_CODEC || cls->probe == NULL ||
                cls->probe(input, size, &host->api) != 1u) {
                continue;
            }

            memset(&reader, 0, sizeof(reader));
            reader.magic = HDRG_READER_MAGIC;
            reader.host = host;
            reader.cls = cls;
            reader.out = out;
            reader.max_pixels = max_pixels;
            reader.fit = fit;

            status = cls->read(&reader, input, size, &host->api);
            if (status == HDRG_OK && !reader.has_image) {
                status = HDRG_ERR_DECODE;
            }

            if (status == HDRG_OK) {
                status = hdrg_codecs_resolve(&reader);
            }

            if (status != HDRG_OK) {
                hdrg_decoded_free(&host->alloc, out);
            }

            return status;
        }
    }

    return HDRG_ERR_UNSUPPORTED_FORMAT;
}

extern const hdrg_class_t *hdrg_codec_writer(const hdrg_plugin_host_t *host,
                                             const hdrg_options_t **options)
{
    const hdrg_class_t *cls;

    *options = host->writer_options;

    if (host->writer != NULL) {
        return host->writer;
    }

    cls = hdrg_default_find(host, HDRG_DEFAULT_WRITER);

    return (cls != NULL && cls->kind == HDRG_CLASS_CODEC && cls->write_create != NULL) ? cls : NULL;
}

extern void hdrg_writer_init(hdrg_writer_t *writer, hdrg_plugin_host_t *host,
                             const hdrg_class_t *cls)
{
    memset(writer, 0, sizeof(*writer));
    writer->magic = HDRG_WRITER_MAGIC;
    writer->host = host;
    writer->cls = cls;
    writer->gm_values[HDRG_GAINMAP_GAMMA] = 1.0;
    writer->gm_values[HDRG_GAINMAP_BASE_OFFSET] = HDRG_CODECS_GAINMAP_OFFSET;
    writer->gm_values[HDRG_GAINMAP_ALT_OFFSET] = HDRG_CODECS_GAINMAP_OFFSET;
}

extern hdrg_status_t hdrg_writer_create(hdrg_writer_t *writer, const hdrg_options_t *options)
{
    hdrg_options_t *defaults;
    hdrg_status_t status;

    defaults = NULL;

    if (options == NULL) {
        status = hdrg_host_resolve_options(writer->host, writer->cls, NULL, &defaults);
        if (status != HDRG_OK) {
            return status;
        }

        options = defaults;
    }

    status = writer->cls->write_create(writer, options, &writer->inst, &writer->host->api);
    writer->created = status == HDRG_OK;
    hdrg_options_destroy(defaults);

    return status;
}

extern hdrg_status_t hdrg_writer_rows(hdrg_writer_t *writer, uint32_t y0, uint32_t rows,
                                      const uint8_t *data, size_t stride)
{
    return writer->cls->write_rows(writer->inst, y0, rows, data, stride, &writer->host->api);
}

extern hdrg_status_t hdrg_writer_finish(hdrg_writer_t *writer, uint8_t **output,
                                        size_t *output_size)
{
    hdrg_status_t status;
    uint8_t *shrunk;

    writer->finishing = true;

    status = writer->cls->write_finish(writer->inst, writer, &writer->host->api);
    if (status != HDRG_OK) {
        return status;
    }

    if (writer->out_size == 0) {
        return HDRG_ERR_ENCODE;
    }

    /* the grown room given back (kept as it is when the allocator cannot), the block handed over */
    shrunk = (uint8_t *)hdrg_mem_realloc(&writer->host->alloc, writer->out,
                                         sizeof(hdrg_outbuf_header_t) + writer->out_size);
    writer->out = (shrunk != NULL) ? shrunk : writer->out;
    *output = hdrg_outbuf_adopt(&writer->host->alloc, writer->out);
    *output_size = writer->out_size;
    writer->out = NULL;
    writer->out_size = 0;
    writer->out_cap = 0;

    return HDRG_OK;
}

extern void hdrg_writer_destroy(hdrg_writer_t *writer)
{
    if (writer->created) {
        writer->cls->write_destroy(writer->inst, &writer->host->api);
    }

    hdrg_mem_free(&writer->host->alloc, writer->out);
    writer->out = NULL;
    writer->inst = NULL;
    writer->created = false;
}
