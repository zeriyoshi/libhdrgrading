/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_APPLE_H
#define HDRG_GPU_APPLE_H

#include <stdbool.h>
#include <stdint.h>

#include "hdrgrading/video.h"

/*
 * An app's CVPixelBufferRef over an IOSurface (HDRG_FRAME_CVPIXELBUFFER,
 * HDRG_WITH_CVPIXELBUFFER: Apple, with Metal or WebGPU): its surface, its
 * Y'CbCr pixel format as a side's pixels (layout, bits, interleaved, msb,
 * full_range; the matrix and chroma location are the stream's, not the
 * buffer's), and each plane's texels: their size, bytes and components
 */
typedef struct {
    void *surface; /* IOSurfaceRef, the pixel buffer's */
    hdrg_session_pixfmt_t pixfmt;
    uint32_t widths[3], heights[3];
    uint8_t planes;
    uint8_t bytes[3];      /* a texel's */
    uint8_t components[3]; /* 1, or 2 for Cb and Cr interleaved */
} hdrg_apple_surface_t;

/* false: not a pixel buffer over an IOSurface of a Y'CbCr format the library reads */
extern bool hdrg_apple_surface(void *pixel_buffer, hdrg_apple_surface_t *out);

#endif /* HDRG_GPU_APPLE_H */
