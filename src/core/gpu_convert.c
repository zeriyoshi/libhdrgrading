/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "gpu_convert.h"

#include <string.h>

/* the shader's buffers (src/core/wgsl/convert.wgsl), the core's slots */
#define HDRG_GV_YUV     6u  /* the Y'CbCr planes, rows packed to words */
#define HDRG_GV_BASE    7u  /* the sites' luma, slopes, chroma and bend flags (the GPU's alone) */
#define HDRG_GV_KNOTS   8u  /* SEGMENTED: the bent lines' knots ... */
#define HDRG_GV_VALUES  9u  /* ... and their chroma */
#define HDRG_GV_RGB     10u /* the 16-bit R'G'B' rows */
#define HDRG_GV_BUFFERS 5u

/* the shader's FIT_* */
#define HDRG_GV_FIT_NONE      0u
#define HDRG_GV_FIT_LINEAR    1u
#define HDRG_GV_FIT_SEGMENTED 2u

static inline uint32_t hdrg_gv_div(uint32_t n, uint32_t d)
{
    return (n + d - 1u) / d;
}

extern size_t hdrg_gpu_convert_pitch(uint32_t width)
{
    return ((size_t)width * 6u + 255u) & ~(size_t)255u;
}

extern void hdrg_gpu_convert_layout(const hdrg_yuv_t *yuv, hdrg_gpu_convert_params_t *params,
                                    size_t sizes[5], hdrg_gc_dispatch_t dispatches[3],
                                    uint8_t *count)
{
    const bool subsampled = yuv->layout != HDRG_YUV_444,
               segmented = subsampled && yuv->fit == HDRG_CHROMA_FIT_SEGMENTED;
    uint32_t offsets[2];
    uint8_t i;
    size_t plane;
    float coeffs[8];

    memset(params, 0, sizeof(*params));
    hdrg_yuv_input_coeffs(yuv, coeffs, offsets);

    params->width = yuv->width;
    params->height = yuv->height;
    params->cw = subsampled ? hdrg_gv_div(yuv->width, 2u) : yuv->width;
    params->ch = (yuv->layout == HDRG_YUV_420) ? hdrg_gv_div(yuv->height, 2u) : yuv->height;
    params->bytes = (yuv->bits > 8u) ? 2u : 1u;
    params->pairs = yuv->interleaved ? 1u : 0u;
    params->y_stride = hdrg_gv_div(params->width * params->bytes, 4u);
    params->c_stride = hdrg_gv_div(params->cw * params->bytes * (yuv->interleaved ? 2u : 1u), 4u);
    params->cb_at = params->y_stride * params->height;
    params->cr_at = params->cb_at + (yuv->interleaved ? 0u : params->c_stride * params->ch);
    params->dx = offsets[0];
    params->dy = offsets[1];
    params->fit = !subsampled ? HDRG_GV_FIT_NONE
                  : segmented ? HDRG_GV_FIT_SEGMENTED
                              : HDRG_GV_FIT_LINEAR;
    params->out_stride = (uint32_t)(hdrg_gpu_convert_pitch(yuv->width) / sizeof(uint32_t));
    params->sites = params->cw * params->ch;
    params->y_off = coeffs[0];
    params->inv_y = coeffs[1];
    params->c_off = coeffs[2];
    params->inv_c = coeffs[3];
    params->cr_r = coeffs[4];
    params->cb_b = coeffs[5];
    params->cb_g = coeffs[6];
    params->cr_g = coeffs[7];

    plane = (size_t)params->sites * sizeof(float);
    sizes[0] = ((size_t)params->cr_at + (size_t)params->c_stride * params->ch) * sizeof(uint32_t);
    sizes[1] = subsampled ? 6u * plane : sizeof(float);
    sizes[2] = segmented ? 3u * plane : sizeof(float);
    sizes[3] = segmented ? 6u * plane : sizeof(float);
    sizes[4] = (size_t)params->out_stride * params->height * sizeof(uint32_t);
    *count = 0;

    for (i = 0; subsampled && i < 2u; i++) {
        dispatches[*count].entry = i;
        dispatches[*count].groups_x = hdrg_gv_div(params->cw, HDRG_GC_GROUP);
        dispatches[*count].groups_y = params->ch;
        (*count)++;
    }

    dispatches[*count].entry = 2;
    dispatches[*count].groups_x = hdrg_gv_div(hdrg_gv_div(params->width, 2u), HDRG_GC_GROUP);
    dispatches[*count].groups_y = params->height;
    (*count)++;
}

extern hdrg_status_t hdrg_gpu_convert_frame(hdrg_gpu_core_t *gpu, const hdrg_exec_t *exec,
                                            const hdrg_yuv_t *yuv, uint16_t *rgb, size_t stride,
                                            uint8_t slot, const hdrg_device_frame_t *device)
{
    const bool back = slot >= HDRG_GC_SLOTS;
    hdrg_gc_dispatch_t dispatches[3];
    hdrg_gpu_convert_params_t params;
    hdrg_status_t status;
    uint8_t slots[HDRG_GV_BUFFERS] = {HDRG_GV_YUV, HDRG_GV_BASE, HDRG_GV_KNOTS, HDRG_GV_VALUES,
                                      HDRG_GV_RGB},
            *views[HDRG_GV_BUFFERS], count, i;
    size_t sizes[HDRG_GV_BUFFERS];

    hdrg_gpu_convert_layout(yuv, &params, sizes, dispatches, &count);

    /* the planes in, the frame out (or kept in the frame slot): the sites' planes stay */
    slots[HDRG_GV_BUFFERS - 1u] = back ? HDRG_GV_RGB : slot;

    for (i = 0; i < HDRG_GV_BUFFERS; i++) {
        views[i] = NULL;

        if (!hdrg_gpu_core_buffer(
                gpu, slots[i], sizes[i],
                ((i == 0u && device == NULL) || (back && i == HDRG_GV_BUFFERS - 1u)) ? &views[i]
                                                                                     : NULL)) {
            return HDRG_ERR_NOMEM;
        }
    }

    /* the app's planes on the GPU copied there, else the host's uploaded */
    if (device != NULL) {
        status = hdrg_gpu_core_plane(gpu, device, 0, HDRG_GV_YUV, 0, params.y_stride,
                                     params.width * params.bytes, params.height,
                                     hdrg_gc_texel(params.bytes, false), true);

        for (i = 1; status == HDRG_OK && i < (yuv->interleaved ? 2u : 3u); i++) {
            status = hdrg_gpu_core_plane(
                gpu, device, i, HDRG_GV_YUV, (i == 1u) ? params.cb_at : params.cr_at,
                params.c_stride, params.cw * params.bytes * (yuv->interleaved ? 2u : 1u), params.ch,
                hdrg_gc_texel(params.bytes, yuv->interleaved), true);
        }

        if (status != HDRG_OK) {
            return status;
        }
    } else {
        hdrg_gpu_core_copy(exec, (const uint8_t *)yuv->planes[0], yuv->strides[0], views[0],
                           (size_t)params.y_stride * sizeof(uint32_t),
                           (size_t)params.width * params.bytes, params.height);
        hdrg_gpu_core_copy(exec, (const uint8_t *)yuv->planes[1], yuv->strides[1],
                           views[0] + (size_t)params.cb_at * sizeof(uint32_t),
                           (size_t)params.c_stride * sizeof(uint32_t),
                           (size_t)params.cw * params.bytes * (yuv->interleaved ? 2u : 1u),
                           params.ch);

        if (!yuv->interleaved) {
            hdrg_gpu_core_copy(exec, (const uint8_t *)yuv->planes[2], yuv->strides[2],
                               views[0] + (size_t)params.cr_at * sizeof(uint32_t),
                               (size_t)params.c_stride * sizeof(uint32_t),
                               (size_t)params.cw * params.bytes, params.ch);
        }
    }

    if (!hdrg_gpu_core_run(gpu, HDRG_GC_CONVERT, &params, slots, dispatches, count,
                           (device != NULL) ? 0u : (1u << HDRG_GV_YUV),
                           back ? (1u << HDRG_GV_RGB) : 0u)) {
        return HDRG_ERR_INTERNAL;
    }

    if (back) {
        hdrg_gpu_core_copy(exec, views[HDRG_GV_BUFFERS - 1u], hdrg_gpu_convert_pitch(yuv->width),
                           (uint8_t *)rgb, stride, (size_t)params.width * 6u, params.height);
    }

    return HDRG_OK;
}
