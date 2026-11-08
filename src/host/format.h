/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_FORMAT_H
#define HDRG_HOST_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/exec.h"
#include "hdrgrading/filter.h"

#define HDRG_FORMAT_ALIGN 256u   /* plane strides and offsets: multiples of it */
#define HDRG_FORMAT_PAGE  16384u /* frames: their size a multiple, their base aligned to it */

/* a frame of a stage format: its planes at offsets from its base, with their strides */
typedef struct {
    size_t offsets[3], strides[3];
    size_t bytes; /* aligned: a multiple of HDRG_FORMAT_PAGE */
    uint32_t format, width, height;
    uint8_t planes;
} hdrg_format_layout_t;

/* a frame's planes: where each starts, its stride (host buffers or a plugin's frames) */
typedef struct {
    uint8_t *planes[3];
    size_t strides[3];
} hdrg_format_frame_t;

/* how the Y'CbCr formats code the frame (the filter's declaration) */
typedef struct {
    uint8_t matrix; /* HDRG_MATRIX_* of video.h (H.273) */
    bool full_range;
} hdrg_format_yuv_t;

extern bool hdrg_format_known(uint32_t format);
/* the format converts any rows on their own (4:2:0 converts whole frames) */
extern bool hdrg_format_banded(uint32_t format);
/* aligned: a whole frame's (strides of HDRG_FORMAT_ALIGN, pages); else packed rows */
extern bool hdrg_format_layout(uint32_t format, uint32_t width, uint32_t height, bool aligned,
                               hdrg_format_layout_t *out);
/* the planes of a frame of the layout at base */
extern void hdrg_format_view(const hdrg_format_layout_t *layout, uint8_t *base,
                             hdrg_format_frame_t *out);
/* the bytes of a row of the plane, packed */
extern size_t hdrg_format_row_bytes(const hdrg_format_layout_t *layout, uint8_t plane);
/* the bytes of the format's samples (its planes and strides are aligned to them) */
extern size_t hdrg_format_sample_bytes(uint32_t format);
/* the format's planes */
extern uint8_t hdrg_format_planes(uint32_t format);
/* the plane's row of frame row y (a 4:2:0 chroma plane: y / 2) */
extern uint32_t hdrg_format_plane_row(uint32_t format, uint8_t plane, uint32_t y);
/* the scratch bytes the conversions of a frame of the layout take (4:2:0) */
extern size_t hdrg_format_scratch(const hdrg_format_layout_t *layout);
/*
 * RGB16 rows (rgb: row y0, rgb_stride bytes a row) into rows [y0, y0 + rows)
 * of frame, of the layout's format and size (a banded format: any rows;
 * 4:2:0: all of them, the RGB16 rows tight; scratch: hdrg_format_scratch
 * bytes)
 */
extern void hdrg_format_pack(const hdrg_exec_t *exec, const hdrg_format_layout_t *layout,
                             const hdrg_format_yuv_t *yuv, const uint16_t *rgb, size_t rgb_stride,
                             uint32_t y0, uint32_t rows, const hdrg_format_frame_t *frame,
                             void *scratch);
/* ... and back: rows [y0, y0 + rows) of frame into RGB16 rows at rgb */
extern void hdrg_format_unpack(const hdrg_exec_t *exec, const hdrg_format_layout_t *layout,
                               const hdrg_format_yuv_t *yuv, const hdrg_format_frame_t *frame,
                               uint32_t y0, uint32_t rows, uint16_t *rgb, size_t rgb_stride,
                               void *scratch);

#endif /* HDRG_HOST_FORMAT_H */
