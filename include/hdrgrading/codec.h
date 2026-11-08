/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading codec plugin ABI: the still-image file formats a context
 * reads and writes, as classes of a plugin (filter.h's API table, the same
 * registration, options and memory). A class made with class_create is a
 * codec once it has a reader or a writer (class_set_reader,
 * class_set_writer), never a filter too.
 *
 * - A reader recognizes a file by its first bytes (probe) and decodes it
 *   (read) into a picture the host allocates (read_image): interleaved
 *   R'G'B' with its color (read_set_cicp / xy / icc), Exif and orientation.
 *   The host resolves the color (an ICC profile through lcms2 where it needs a
 *   CMM, else a DCF Adobe RGB Exif, else sRGB), rejects HDR transfers (PQ,
 *   HLG: HDRG_ERR_ICC_UNSUPPORTED) and turns the picture upright. The
 *   readers of a context are probed in registration order (the built-in
 *   ones first).
 * - A writer takes the rendition's rows in order (rows) in the format it
 *   declared, then the file's gain map, metadata and light (finish), and
 *   appends the file's bytes (write_output). The host picks the form and the
 *   base's color for the writer's color signaling: hdrg_context_set_writer
 *   chooses it for the following conversions (default hdrg.jpeg).
 * - The Y'CbCr conversions of the library (its chroma from luma) are the
 *   table's yuv_to_rgb16 and rgb16_to_yuv; a codec's work on threads goes
 *   through run_parallel, on the context's threads.
 *
 * A codec plugin includes filter.h: the table, its registration and its
 * memory, which includes this header.
 */
#ifndef HDRGRADING_CODEC_H
#define HDRGRADING_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hdrg_reader hdrg_reader_t; /* a file being read */
typedef struct hdrg_writer hdrg_writer_t; /* a file being written */

/* the forms a writer takes (class_set_writer): bits of the HDRG_OUTPUT_* values */
#define HDRG_CODEC_FORM(mode) (UINT32_C(1) << (mode))

/*
 * how a writer signals its base's color. CICP: H.273 code points (or the
 * format's equivalent): an SDR base in sRGB or Display P3, a PQ base in a
 * BT.2020 container (its colors within P3). ICC: a profile the host makes
 * (write_icc): Display P3 or sRGB (none) for an SDR base and "P3 PQ" for a
 * PQ one with lcms2; without it sRGB alone, and no PQ base
 * (HDRG_ERR_ICC_UNSUPPORTED)
 */
#define HDRG_CODEC_COLOR_CICP UINT32_C(1)
#define HDRG_CODEC_COLOR_ICC  UINT32_C(2)

/* the rows a writer takes */
#define HDRG_CODEC_ROWS_RGB8  UINT32_C(1) /* interleaved 8-bit R'G'B' (encode->dither) */
#define HDRG_CODEC_ROWS_RGB16 UINT32_C(2) /* interleaved 16-bit (the codes x 65535) */

/* write_gainmap's values: log2 but the gamma */
#define HDRG_GAINMAP_BASE_HEADROOM 0u /* the base's HDR headroom */
#define HDRG_GAINMAP_ALT_HEADROOM  1u /* the alternate rendition's */
#define HDRG_GAINMAP_MIN           2u /* the gain a stored 0 decodes to */
#define HDRG_GAINMAP_MAX           3u /* ... a stored 1 */
#define HDRG_GAINMAP_GAMMA         4u /* the stored map's encoding gamma */
#define HDRG_GAINMAP_BASE_OFFSET   5u /* ISO 21496-1's offsets, linear */
#define HDRG_GAINMAP_ALT_OFFSET    6u
#define HDRG_GAINMAP_VALUES        7u

/*
 * a Y'CbCr picture's description (yuv_to_rgb16, rgb16_to_yuv):
 * HDRG_YUVD_WORDS words. Samples of 8 bits are bytes, of more uint16_t in
 * native order
 */
#define HDRG_YUVD_WIDTH  0u
#define HDRG_YUVD_HEIGHT 1u
#define HDRG_YUVD_LAYOUT 2u /* HDRG_YUV_420 / 422 / 444 (hdrgrading.h) */
#define HDRG_YUVD_MATRIX 3u /* HDRG_MATRIX_* (hdrgrading.h) */
#define HDRG_YUVD_BITS   4u /* 8 to 16 */
#define HDRG_YUVD_SITING 5u /* HDRG_CHROMA_* (hdrgrading.h) */
#define HDRG_YUVD_FLAGS  6u /* HDRG_YUVF_* */
#define HDRG_YUVD_WORDS  7u

#define HDRG_YUVF_FULL_RANGE  UINT32_C(0x01) /* else video (limited) range */
#define HDRG_YUVF_JFIF        UINT32_C(0x02) /* JFIF's chroma center (128 / 255), full range */
#define HDRG_YUVF_INTERLEAVED UINT32_C(0x04) /* Cb, Cr alternating in planes[1] */
#define HDRG_YUVF_MSB         UINT32_C(0x08) /* the bits in the high bits of the samples */

/*
 * required of a reader: 1 when data (size bytes, the whole file) is a file
 * of the class's by its first bytes, else 0. Cheap: every reader's probe may
 * run on every input
 */
typedef uint32_t(HDRG_PLUGIN_CALL *hdrg_codec_probe_fn_t)(const uint8_t *data, size_t size,
                                                          const hdrg_plugin_api_t *api);
/*
 * required of a reader: decodes data into the picture of read_image with
 * its color; the first picture of a file of several. The input is not
 * trusted: a reader checks every size it reads before using it
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_codec_read_fn_t)(hdrg_reader_t *reader,
                                                              const uint8_t *data, size_t size,
                                                              const hdrg_plugin_api_t *api);
/*
 * required of a writer: the instance (*inst) writing one file
 * (write_image describes it) from validated options, valid within this
 * call alone. A failure leaves nothing of the instance (destroy is not
 * called)
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_codec_create_fn_t)(hdrg_writer_t *writer,
                                                                const hdrg_options_t *options,
                                                                void **inst,
                                                                const hdrg_plugin_api_t *api);
/*
 * required of a writer: rows [y0, y0 + rows) of the picture in order, in the
 * class's row format, rows of stride bytes valid within the call
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_codec_rows_fn_t)(void *inst, uint32_t y0,
                                                              uint32_t rows, const uint8_t *data,
                                                              size_t stride,
                                                              const hdrg_plugin_api_t *api);
/*
 * required of a writer, after the last rows: the gain map, its metadata and
 * the light are known (write_gainmap, write_light); the file goes out
 * through write_output
 */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_codec_finish_fn_t)(void *inst, hdrg_writer_t *writer,
                                                                const hdrg_plugin_api_t *api);
/* required of a writer, whether finish ran or not */
typedef void(HDRG_PLUGIN_CALL *hdrg_codec_destroy_fn_t)(void *inst, const hdrg_plugin_api_t *api);
/* run_parallel's work: index in [0, count), worker below run_workers' count */
typedef void(HDRG_PLUGIN_CALL *hdrg_parallel_fn_t)(void *user, uint32_t index, uint32_t worker);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_CODEC_H */
