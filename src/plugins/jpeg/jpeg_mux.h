/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CODECS_JPEG_MUX_H
#define HDRG_CODECS_JPEG_MUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "parts.h"

/*
 * The two-layer gain-map JPEG (MPF + ISO 21496-1 + XMP) of two plain JPEG
 * streams of jpegli's (SOI, a JFIF APP0, the rest). The direction is the
 * headroom pair of the values: an SDR base (0, H) with a gain map in [0, H],
 * an HDR base (hybrid) (H, 0) with one in [-H, 0].
 */
typedef struct {
    const uint8_t *base, *gainmap;      /* the primary rendition's, the monochrome gain map's */
    const uint8_t *exif;                /* TIFF (NULL: none) */
    const uint8_t *icc;                 /* the primary's profile (NULL: none) */
    const uint8_t *iso;                 /* ISO 21496-1 GainMapMetadata (write_iso21496) */
    double values[HDRG_GAINMAP_VALUES]; /* write_gainmap's: the XMP's */
    size_t base_size, gainmap_size, exif_size, icc_size, iso_size;
    bool apple_xmp, hdrgm_xmp; /* the gain map stream's XMP (an SDR base's) */
} hdrg_jpeg_mux_t;

/* the file through the writer's write_output */
extern hdrg_status_t hdrg_jpeg_mux_gainmap(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                           const hdrg_jpeg_mux_t *input);
/*
 * A single-layer JPEG of the writer's with its ICC (one APP2, after APP0)
 * and Exif (an APP1 after it), as the writer would have placed them, through
 * write_output
 */
extern hdrg_status_t hdrg_jpeg_mux_single(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                          const uint8_t *jpeg, size_t size, const uint8_t *icc,
                                          size_t icc_size, const uint8_t *exif, size_t exif_size);

#endif /* HDRG_CODECS_JPEG_MUX_H */
