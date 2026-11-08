/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CONTAINER_H
#define HDRG_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#include "pipeline.h"

/*
 * The still-image file forms (io layer): GAINMAP (SDR base + gain map),
 * HYBRID (PQ primary + SDR-recovering gain map) and PQ (single layer),
 * written by a codec's writer (src/host/codec_host.h: the context's,
 * hdrg_context_set_writer). hdrg_container_select resolves the writer and
 * the form (HDRG_OUTPUT_AUTO: the writer's own) and refuses a form the writer
 * does not take (HDRG_ERR_UNSUPPORTED), or one it cannot signal without
 * lcms2 (a writer signaling ICC profiles: HYBRID and PQ,
 * HDRG_ERR_ICC_UNSUPPORTED). hdrg_container_caps gives the display the file
 * is rendered for (the assumed viewing profile; an ICC writer is sRGB without
 * lcms2, which writes no P3 profile; a CICP writer's PQ bases are a BT.2020
 * container). hdrg_container_prepare sets the pipeline's rows for the writer
 * before the frame (8 or 16 bits; the PQ bases' light) and refuses a side
 * past the writer's limit. hdrg_container_write renders the pipeline band
 * by band into the writer, then gives it the gain map, its metadata and the
 * light.
 */
typedef struct {
    const hdrg_class_t *writer;
    const hdrg_options_t *options; /* resolved; NULL: the defaults */
} hdrg_container_t;

extern hdrg_status_t hdrg_container_select(hdrg_context_t *ctx, hdrg_encode_params_t *encode,
                                           hdrg_container_t *out);
extern void hdrg_container_caps(const hdrg_container_t *container,
                                const hdrg_display_caps_t *display,
                                const hdrg_encode_params_t *encode, hdrg_display_caps_t *out);
extern hdrg_status_t hdrg_container_prepare(const hdrg_container_t *container, hdrg_context_t *ctx,
                                            hdrg_pipeline_t *pipeline,
                                            const hdrg_encode_params_t *encode);
/* exif: with its "Exif\0\0" prefix (NULL: none) */
extern hdrg_status_t hdrg_container_write(const hdrg_container_t *container, hdrg_context_t *ctx,
                                          hdrg_pipeline_t *pipeline,
                                          const hdrg_encode_params_t *encode, const uint8_t *exif,
                                          size_t exif_size, uint8_t **output, size_t *output_size);

#endif /* HDRG_CONTAINER_H */
