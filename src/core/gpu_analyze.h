/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GPU_ANALYZE_H
#define HDRG_CORE_GPU_ANALYZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "analyze.h"
#include "gpu_core.h"
#include "image.h"

/* the shader's uniform (Params of src/core/wgsl/analyze.wgsl), 112 bytes: its members in order */
typedef struct {
    uint32_t width, height, src_stride, step_shift, offset, src_width, src_height, entries,
        cell_shift, qw, qh, x0, x1, y0, y1, cell, grid_w, grid_h, light_shift, light_w, light_h,
        has_light;
    float kr, kg, kb, floor;
    uint32_t pad0, pad1;
} hdrg_gpu_analyze_params_t;

/*
 * A picture the GPU sums over: width x height pixels, each the pixel at
 * 2^step_shift times its place plus offset (clamped) of a source of 16-bit
 * R'G'B' rows of stride words, src_width x src_height; in slot (a frame kept
 * on the GPU) or, slot HDRG_GC_SLOTS, src's rows uploaded (then src is the
 * picture itself). Its linear light: src's EOTF tables (entries a channel)
 * and luminance coefficients
 */
typedef struct {
    const hdrg_source_t *src;
    uint32_t width, height, src_width, src_height, stride;
    uint8_t step_shift, slot;
} hdrg_gpu_picture_t;

/* the analysis's working buffers on the CPU's side (reused frame to frame) */
typedef struct {
    const float *lut; /* the EOTF tables last uploaded */
    uint64_t *hist;
    size_t edges_cap;
    float *edges;
} hdrg_gpu_analyze_t;

/*
 * Whether the GPU can sum over src's pixels: 16-bit R'G'B' (3 samples a
 * pixel), its EOTF tables of 65536 entries a channel
 */
extern bool hdrg_gpu_analyze_takes(const hdrg_source_t *src);
/*
 * Pass A's sums (src/core/analyze.c) of the picture on the GPU into given
 * (its arrays the work's, valid until the next call): the cells of
 * 2^cell_shift pixels, the bars (from the rows' and columns' maxima: the
 * CPU's exactly), the histogram of the bars' rectangle. Within L2 of the
 * CPU's
 */
extern hdrg_status_t hdrg_gpu_analyze_stats(hdrg_gpu_core_t *gpu, const hdrg_allocator_t *alloc,
                                            const hdrg_exec_t *exec, hdrg_gpu_analyze_t *work,
                                            const hdrg_gpu_picture_t *picture, uint8_t cell_shift,
                                            hdrg_analyze_given_t *given);
/*
 * Pass B's sums (src/core/plan.c's hdrg_plan_moments_fn_t) of the picture on
 * the GPU: each plan cell's moments into sums and its grid into grid
 */
extern hdrg_status_t hdrg_gpu_analyze_moments(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                              hdrg_gpu_analyze_t *work,
                                              const hdrg_gpu_picture_t *picture,
                                              const hdrg_analysis_t *analysis, uint8_t light_shift,
                                              uint32_t cell, uint32_t grid_w, uint32_t grid_h,
                                              double *sums, float *grid);
/* the work's buffers freed */
extern void hdrg_gpu_analyze_free(const hdrg_allocator_t *alloc, hdrg_gpu_analyze_t *work);

#endif /* HDRG_CORE_GPU_ANALYZE_H */
