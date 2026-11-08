/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_YUV_H
#define HDRG_CORE_YUV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exec.h"
#include "hdrgrading/video.h"

/*
 * A planar Y'CbCr picture: 8-bit samples (bits 8) or 16-bit native-endian
 * samples holding bits of value (9..16; msb: in their high bits, the low
 * ones 0, as P010), Cb and Cr in planes of their own or alternating in
 * planes[1] (interleaved, as NV12: planes[2] unused). Subsampled chroma sits where
 * siting (H.273 chroma location) puts it: on the even luma columns or
 * between two, on the even luma rows, between two or on the odd ones
 * (4:2:2: the columns only). A JFIF (JPEG) picture is full range with the
 * chroma centered on 128 / 255 of the top code (and sited at the center).
 */
typedef struct {
    const void *planes[3]; /* Y, Cb, Cr (the output conversion writes through them) */
    size_t strides[3];     /* bytes */
    uint32_t width, height;
    uint8_t layout;   /* HDRG_YUV_* */
    uint8_t matrix;   /* HDRG_MATRIX_* */
    uint8_t bits;     /* 8..16 */
    uint8_t siting;   /* HDRG_CHROMA_* */
    uint8_t fit;      /* HDRG_CHROMA_FIT_*: how the upsampled chroma follows the luma */
    bool full_range;  /* false: video (limited) range */
    bool jfif;        /* the JFIF chroma center (full_range too) */
    bool interleaved; /* Cb, Cr alternating in planes[1] */
    bool msb;         /* bits above 8 in the high bits of the 16-bit samples */
} hdrg_yuv_t;

/* the size, layout, matrix and bits are ones the conversions take */
extern bool hdrg_yuv_valid(const hdrg_yuv_t *yuv);
/* the planes are set and their strides hold a row of a valid picture */
extern bool hdrg_yuv_fits(const hdrg_yuv_t *yuv);
/* floats of the scratch hdrg_yuv_to_rgb16 needs for a picture of the layout and fit (4:4:4: none)
 */
extern size_t hdrg_yuv_scratch(const hdrg_yuv_t *yuv);
/*
 * R'G'B' (the source's code values, 16-bit: x 65535) of a Y'CbCr picture;
 * rgb has rows of stride bytes (at least width * 3 samples). Subsampled chroma is upsampled
 * bilinearly between the two nearest sites on each axis plus the luma's detail where the chroma
 * follows the luma (chroma from luma): each chroma sample's slope against the luma filtered to its
 * site (a site on a luma sample: [1 2 1] / 4 around it, between two: their mean, on each axis) over
 * its 3 x 3 neighborhood (ridge: a luma variance of (4 / 255)^2; HDRG_CHROMA_FIT_LINEAR times the
 * correlation's square, so noise and texture that do not follow the luma add none), interpolated
 * like the chroma, times the luma's departure from its own interpolation. Where they do not follow
 * each other (a flat or neutral chroma, an isoluminant edge) the slope is 0 and the chroma is the
 * bilinear one. HDRG_CHROMA_FIT_SEGMENTED bends the line where it leaves a residual (above 4 / 255
 * rms): knots at the darkest, middle (3-means) and brightest luma of the neighborhood's pixels, the
 * three chroma values fitted to the samples through their sites' filters (ridge toward the line,
 * within the samples' range widened by a quarter of it), taken in full where the bend leaves at
 * most 0.3 of the line's residual and not at all from 0.7. A pixel adds what the bent lines of its
 * nearest samples depart from their lines (weighted like the chroma), each at its luma or along the
 * chord between its 3 x 3 neighborhood's darkest and brightest luma, whichever departs less.
 * scratch holds hdrg_yuv_scratch floats (NULL: the bilinear chroma alone)
 */
extern void hdrg_yuv_to_rgb16(const hdrg_exec_t *exec, const hdrg_yuv_t *yuv, float *scratch,
                              uint16_t *rgb, size_t stride);
/*
 * Rows [y0, y0 + rows) of 16-bit R'G'B' (width * 3 samples a row from rgb's
 * first row = row y0) into the 4:2:0 Y'CbCr picture yuv (its planes
 * written, chroma location HDRG_CHROMA_LEFT): y0 even, the chroma of each
 * row pair from the [1 2 1] / 4 filter on the even columns and the mean of
 * the two rows
 */
extern void hdrg_rgb16_to_yuv420(const hdrg_exec_t *exec, const uint16_t *rgb, uint32_t y0,
                                 uint32_t rows, const hdrg_yuv_t *yuv);
/* ... and into the 4:4:4 picture yuv (any y0): each pixel's Y', Cb, Cr */
extern void hdrg_rgb16_to_yuv444(const hdrg_exec_t *exec, const uint16_t *rgb, uint32_t y0,
                                 uint32_t rows, const hdrg_yuv_t *yuv);
/*
 * the output conversion's matrix and range (the GPU's, src/core/gpu_apply.c): kr, kg, kb,
 * 1 / (2 (1 - kb)), 1 / (2 (1 - kr)), the Y' offset and range, the chroma's, the top code
 */
extern void hdrg_yuv_output_coeffs(const hdrg_yuv_t *yuv, float out[10]);
/*
 * the input conversion's (the GPU's, src/core/gpu_convert.c): the Y' offset and 1 / range, the
 * chroma's, then cr_r, cb_b, cb_g, cr_g (R' = Y' + cr_r Cr, B' = Y' + cb_b Cb, G' = Y' - cb_g Cb -
 * cr_g Cr); offsets: the chroma sites' columns and rows (hdrg_yuv_to_rgb16's, 3: not subsampled)
 */
extern void hdrg_yuv_input_coeffs(const hdrg_yuv_t *yuv, float out[8], uint32_t offsets[2]);

#endif /* HDRG_CORE_YUV_H */
