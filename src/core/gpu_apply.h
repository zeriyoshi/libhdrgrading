/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GPU_APPLY_H
#define HDRG_CORE_GPU_APPLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hdrgrading/filter.h>

#include "apply.h"
#include "gpu_convert.h"
#include "gpu_core.h"
#include "yuv.h"

/* the shader's buffers (src/core/wgsl/apply.wgsl), the core's slots 0.. */
#define HDRG_GA_SOURCE  0u /* the source's rows, packed to words */
#define HDRG_GA_LUTS    1u /* the source's EOTF tables, the color LUT's nodes, the PQ table */
#define HDRG_GA_FRAME   2u /* the frame's grid positions, boost, near maxima, base and coring */
#define HDRG_GA_DST     3u /* the PQ R'G'B' rows */
#define HDRG_GA_LIGHT   4u /* the rows' largest max(R, G, B), their workgroups' sums, the light */
#define HDRG_GA_PLANES  5u /* Y'CbCr 4:2:0 output: Y', Cb, Cr */
#define HDRG_GA_BUFFERS 6u

/* the shader's uniform (Params), 240 bytes: its members in its order */
typedef struct {
    uint32_t width, height, src_stride, out_stride, comps, wide, entries, pack, grid_w, grid_h,
        flags, groups_x, lut_at, pq_at, rows_at, boost_at, near_at, base_at, coring_at;
    float max_code, diffuse, span, pivot, floor, contrast, clarity, src_kr, src_kg, src_kb, out_kr,
        out_kg, out_kb, desat, knee, range, white, fixed_lo, fixed_hi;
    uint32_t yuv_bytes, y_stride, c_stride, cw, ch, cb_at, cr_at;
    float kr, kg, kb, inv_cb, inv_cr, y_off, y_range, c_off, c_range, top;
    uint32_t pairs, shift, grain_at, light_at;
} hdrg_gpu_apply_params_t;

/* the yuv entry's threads a chroma row (a word of Cb and of Cr, or two words of the pairs) */
extern uint32_t hdrg_gpu_apply_yuv_threads(const hdrg_gpu_apply_params_t *params);

/*
 * The apply bound to its plan and render (hdrg_apply_bind, single-layer PQ)
 * as the state of the shader (an app's GPU: hdrg_apply_state_t): its
 * uniform without the light into params, the bytes of its LUT buffer into
 * luts and of its frame buffer into frame (NULL: their sizes alone, into
 * *luts_size and *frame_size)
 */
extern void hdrg_gpu_apply_export(const hdrg_apply_t *apply, const hdrg_yuv_t *yuv,
                                  hdrg_gpu_apply_params_t *params, size_t *luts_size,
                                  size_t *frame_size, uint8_t *luts, uint32_t *frame);
/*
 * The whole frame of the apply bound to its plan and render (hdrg_apply_bind,
 * single-layer PQ, not banded) on the core's GPU into
 * rows of dst (stride bytes: width x 3 samples of apply->sample_bytes) or,
 * when yuv lays out 4:2:0, into its planes; the light into the apply's
 * chunks when it has them. source: the frame slot the source is kept in on
 * the GPU (16-bit R'G'B' of hdrg_gpu_convert_pitch), or HDRG_GC_SLOTS for
 * apply's source's rows. device: the output's planes on the app's GPU
 * instead of dst / yuv's (video.h; HDRG_ERR_INVALID_ARG when they do not
 * fit). The copies in and out run on exec
 */
extern hdrg_status_t hdrg_gpu_apply_frame(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                          const hdrg_apply_t *apply, uint8_t *dst, size_t stride,
                                          const hdrg_yuv_t *yuv, uint8_t source,
                                          const hdrg_device_frame_t *device);

#endif /* HDRG_CORE_GPU_APPLY_H */
