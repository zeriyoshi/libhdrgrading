/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GPU_GATHER_H
#define HDRG_CORE_GPU_GATHER_H

#include <stdint.h>

#include "gpu_core.h"
#include "noise.h"

/* the shader's uniform (Params of src/core/wgsl/gather.wgsl), 64 bytes: its members in order */
typedef struct {
    uint32_t width, height, pitch, shift, proxy_w, proxy_h, proxy_stride, nx, ny, step, first_x,
        first_y, mosaic_stride, tiles_at, pad0, pad1;
} hdrg_gpu_gather_params_t;

/*
 * What the analysis reads of the width x height frame at slot (16-bit
 * R'G'B', rows of hdrg_gpu_convert_pitch) kept on the GPU,
 * read back: its proxy of shift (src/pipeline/pipeline.c's: proxy_w x proxy_h, the
 * center pixel of each 2^shift square) into tight rows of proxy (NULL: none),
 * and the noise estimate's windows of grid into the tight rows of mosaic
 * (hdrg_noise_estimate_mosaic's layout; NULL: none). Copies: the CPU's
 * values. The copies out run on exec
 */
extern hdrg_status_t hdrg_gpu_gather(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec, uint8_t slot,
                                     uint32_t width, uint32_t height, uint8_t shift,
                                     uint32_t proxy_w, uint32_t proxy_h, uint16_t *proxy,
                                     const hdrg_noise_grid_t *grid, uint16_t *mosaic);

#endif /* HDRG_CORE_GPU_GATHER_H */
