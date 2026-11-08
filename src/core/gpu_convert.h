/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GPU_CONVERT_H
#define HDRG_CORE_GPU_CONVERT_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_core.h"
#include "yuv.h"

/* the shader's uniform (Params of src/core/wgsl/convert.wgsl), 96 bytes: its members in order */
typedef struct {
    uint32_t width, height, cw, ch, bytes, y_stride, c_stride, cb_at, cr_at, dx, dy, fit,
        out_stride, sites, pairs, pad0;
    float y_off, inv_y, c_off, inv_c, cr_r, cb_b, cb_g, cr_g;
} hdrg_gpu_convert_params_t;

/* bytes a row of the GPU's 16-bit R'G'B' frames: width x 6 rounded up to 256 */
extern size_t hdrg_gpu_convert_pitch(uint32_t width);
/*
 * The conversion of yuv (its layout, bits, fit) as the shader runs it: its
 * uniform, the bytes of its buffers (the planes packed: Y' rows of y_stride
 * words, Cb and Cr of c_stride from cb_at and cr_at; the sites' base; the
 * knots; their values; the R'G'B' rows of out_stride words) and its
 * dispatches (sites and slopes when subsampled, then pixels: *count)
 */
extern void hdrg_gpu_convert_layout(const hdrg_yuv_t *yuv, hdrg_gpu_convert_params_t *params,
                                    size_t sizes[5], hdrg_gc_dispatch_t dispatches[3],
                                    uint8_t *count);
/*
 * The Y'CbCr picture yuv (its planes in host memory, a layout and fit
 * hdrg_yuv_to_rgb16 takes) to 16-bit R'G'B' on the core's GPU, into rows of stride bytes of rgb
 * (slot HDRG_GC_SLOTS), or kept on the GPU in the frame slot (rows of hdrg_gpu_convert_pitch; rgb
 * not written): hdrg_yuv_to_rgb16's within L2. device: the planes on the app's GPU instead of yuv's
 * (video.h; HDRG_ERR_INVALID_ARG when they do not fit). The copies in and out run on exec
 */
extern hdrg_status_t hdrg_gpu_convert_frame(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                            const hdrg_yuv_t *yuv, uint16_t *rgb, size_t stride,
                                            uint8_t slot, const hdrg_device_frame_t *device);

#endif /* HDRG_CORE_GPU_CONVERT_H */
