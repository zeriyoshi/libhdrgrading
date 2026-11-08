/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "gpu_gather.h"

#include <string.h>

#include "gpu_convert.h"

#define HDRG_GG_OUT 11u /* the core's slot of the read-back */

static inline uint32_t hdrg_gg_div(uint32_t n, uint32_t d)
{
    return (n + d - 1u) / d;
}

extern hdrg_status_t hdrg_gpu_gather(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec, uint8_t slot,
                                     uint32_t width, uint32_t height, uint8_t shift,
                                     uint32_t proxy_w, uint32_t proxy_h, uint16_t *proxy,
                                     const hdrg_noise_grid_t *grid, uint16_t *mosaic)
{
    const uint8_t slots[2] = {slot, HDRG_GG_OUT};
    hdrg_gc_dispatch_t dispatches[2];
    hdrg_gpu_gather_params_t params;
    uint8_t *view, count;
    size_t size;

    memset(&params, 0, sizeof(params));
    params.width = width;
    params.height = height;
    params.pitch = (uint32_t)(hdrg_gpu_convert_pitch(width) / sizeof(uint32_t));
    params.shift = shift;
    params.proxy_w = (proxy != NULL) ? proxy_w : 0u;
    params.proxy_h = (proxy != NULL) ? proxy_h : 0u;
    params.proxy_stride = 3u * hdrg_gg_div(params.proxy_w, 2u);
    params.tiles_at = params.proxy_stride * params.proxy_h;
    count = 0;

    if (proxy != NULL) {
        dispatches[count].entry = 0;
        dispatches[count].groups_x = hdrg_gg_div(hdrg_gg_div(proxy_w, 2u), HDRG_GC_GROUP);
        dispatches[count].groups_y = proxy_h;
        count++;
    }

    if (grid != NULL && mosaic != NULL) {
        params.nx = grid->nx;
        params.ny = grid->ny;
        params.step = grid->step;
        params.first_x = grid->first_x;
        params.first_y = grid->first_y;
        params.mosaic_stride = 27u * grid->nx;
        dispatches[count].entry = 1;
        dispatches[count].groups_x = hdrg_gg_div(9u * grid->nx, HDRG_GC_GROUP);
        dispatches[count].groups_y = HDRG_NOISE_WINDOW * grid->ny;
        count++;
    }

    if (count == 0) {
        return HDRG_OK;
    }

    size =
        ((size_t)params.tiles_at + ((size_t)params.mosaic_stride * HDRG_NOISE_WINDOW * params.ny)) *
        sizeof(uint32_t);

    if (!hdrg_gpu_core_buffer(gpu, HDRG_GG_OUT, size, &view)) {
        return HDRG_ERR_NOMEM;
    }

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_GATHER, &params, slots, dispatches, count, 0u,
                           1u << HDRG_GG_OUT)) {
        return HDRG_ERR_INTERNAL;
    }

    if (proxy != NULL) {
        hdrg_gpu_core_copy(exec, view, (size_t)params.proxy_stride * sizeof(uint32_t),
                           (uint8_t *)proxy, (size_t)proxy_w * 6u, (size_t)proxy_w * 6u, proxy_h);
    }

    if (params.mosaic_stride > 0u) {
        hdrg_gpu_core_copy(exec, view + ((size_t)params.tiles_at * sizeof(uint32_t)),
                           (size_t)params.mosaic_stride * sizeof(uint32_t), (uint8_t *)mosaic,
                           (size_t)grid->nx * HDRG_NOISE_WINDOW * 6u,
                           (size_t)grid->nx * HDRG_NOISE_WINDOW * 6u, HDRG_NOISE_WINDOW * grid->ny);
    }

    return HDRG_OK;
}
