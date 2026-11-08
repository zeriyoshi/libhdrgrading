/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_CODEC_HOST_H
#define HDRG_HOST_CODEC_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../io/io.h"
#include "host.h"

/* a file being read: the picture of the reader's read (hdrg_codec_read) */
struct hdrg_reader {
    uint32_t magic;
    const hdrg_plugin_host_t *host;
    const hdrg_class_t *cls;
    hdrg_decoded_t *out;
    uint64_t max_pixels;
    uint8_t fit;
    bool has_image, has_color, has_orientation;
};

/*
 * The base's profile a writer's write_icc asks for (container.c): hagc
 * fitted to the rows' light (finish only), lumi_nits > 0 a luminance tag;
 * *out allocated with the host's allocator
 */
typedef hdrg_status_t (*hdrg_writer_icc_fn_t)(void *user, bool hagc, float lumi_nits, uint8_t **out,
                                              size_t *size);

/* a file being written: what the conversion gives its writer (hdrg_writer_*) */
struct hdrg_writer {
    uint32_t magic;
    hdrg_plugin_host_t *host;
    const hdrg_class_t *cls;
    void *inst;
    hdrg_writer_icc_fn_t icc;
    void *icc_user;
    const uint8_t *exif;      /* TIFF, its orientation 1 (NULL: none) */
    const uint8_t *gm_pixels; /* finish: the stored gain map (NULL: none) */
    uint8_t *out;             /* write_output's bytes */
    size_t exif_size, gm_stride, out_size, out_cap;
    double gm_values[HDRG_GAINMAP_VALUES];
    uint32_t width, height, form, primaries, transfer, threads;
    uint32_t gm_width, gm_height, alt_primaries, alt_transfer, alt_bits;
    uint32_t max_cll, max_fall, peak;
    bool created, finishing;
};

/* the API table's codec calls (host.c's table) */
extern uint32_t HDRG_PLUGIN_CALL hdrg_api_run_workers(const hdrg_plugin_api_t *api);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_run_parallel(const hdrg_plugin_api_t *api,
                                                            uint32_t count, hdrg_parallel_fn_t fn,
                                                            void *user);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_yuv_to_rgb16(const hdrg_plugin_api_t *api,
                                                            const uint32_t *desc,
                                                            const void *const *planes,
                                                            const size_t *strides, uint32_t fit,
                                                            uint16_t *rgb, size_t stride);
extern hdrg_status_t HDRG_PLUGIN_CALL
hdrg_api_rgb16_to_yuv(const hdrg_plugin_api_t *api, const uint32_t *desc, void *const *planes,
                      const size_t *strides, const uint16_t *rgb, uint32_t y0, uint32_t rows);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_users(const hdrg_reader_t *reader,
                                                          void **plugin_user, void **class_user);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_limits(const hdrg_reader_t *reader,
                                                           uint64_t *max_pixels,
                                                           uint32_t *chroma_fit);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_image(hdrg_reader_t *reader, uint32_t width,
                                                          uint32_t height, uint32_t bits,
                                                          uint8_t **pixels, size_t *stride);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_cicp(hdrg_reader_t *reader,
                                                             uint32_t primaries, uint32_t transfer);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_xy(hdrg_reader_t *reader, const double *xy,
                                                           double gamma);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_icc(hdrg_reader_t *reader,
                                                            const uint8_t *icc, size_t size);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_exif(hdrg_reader_t *reader,
                                                             const uint8_t *tiff, size_t size);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_read_set_orientation(hdrg_reader_t *reader,
                                                                    uint32_t orientation);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_users(const hdrg_writer_t *writer,
                                                           void **plugin_user, void **class_user);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_image(const hdrg_writer_t *writer,
                                                           uint32_t *width, uint32_t *height,
                                                           uint32_t *form, uint32_t *primaries,
                                                           uint32_t *transfer, uint32_t *threads);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_exif(const hdrg_writer_t *writer,
                                                          const uint8_t **tiff, size_t *size);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_icc(const hdrg_writer_t *writer, uint32_t hagc,
                                                         double lumi_nits, uint8_t *buf, size_t cap,
                                                         size_t *needed);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_gainmap(const hdrg_writer_t *writer,
                                                             const uint8_t **pixels, size_t *stride,
                                                             uint32_t *width, uint32_t *height,
                                                             double *values);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_alternate(const hdrg_writer_t *writer,
                                                               uint32_t *primaries,
                                                               uint32_t *transfer, uint32_t *bits);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_iso21496(const hdrg_writer_t *writer,
                                                              uint8_t *buf, size_t cap,
                                                              size_t *needed);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_apple_xmp(const hdrg_writer_t *writer,
                                                               uint8_t *buf, size_t cap,
                                                               size_t *needed);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_light(const hdrg_writer_t *writer,
                                                           uint32_t *max_cll, uint32_t *max_fall,
                                                           uint32_t *peak);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_write_output(hdrg_writer_t *writer,
                                                            const uint8_t *data, size_t size);

/*
 * input through the first of the host's accepted readers recognizing it,
 * into out (the color resolved: an ICC profile's, else the reader's, else a
 * DCF Adobe RGB Exif's, else sRGB; the orientation the reader's or its
 * Exif's): HDRG_ERR_UNSUPPORTED_FORMAT when none does
 */
extern hdrg_status_t hdrg_codec_read(const hdrg_plugin_host_t *host, const uint8_t *input,
                                     size_t size, uint64_t max_pixels, uint8_t fit,
                                     hdrg_decoded_t *out);
/* the writer of the host's conversions and its resolved options (NULL: the defaults) */
extern const hdrg_class_t *hdrg_codec_writer(const hdrg_plugin_host_t *host,
                                             const hdrg_options_t **options);
/* a writer of cls: the caller fills its fields, then create, rows, finish, destroy */
extern void hdrg_writer_init(hdrg_writer_t *writer, hdrg_plugin_host_t *host,
                             const hdrg_class_t *cls);
/* options NULL: the class's defaults */
extern hdrg_status_t hdrg_writer_create(hdrg_writer_t *writer, const hdrg_options_t *options);
extern hdrg_status_t hdrg_writer_rows(hdrg_writer_t *writer, uint32_t y0, uint32_t rows,
                                      const uint8_t *data, size_t stride);
/* the file into an output buffer (hdrg_outbuf_alloc) */
extern hdrg_status_t hdrg_writer_finish(hdrg_writer_t *writer, uint8_t **output,
                                        size_t *output_size);
/* NULL inst: nothing to destroy */
extern void hdrg_writer_destroy(hdrg_writer_t *writer);

#endif /* HDRG_HOST_CODEC_HOST_H */
