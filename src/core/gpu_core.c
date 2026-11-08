/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "gpu_core.h"

#include <string.h>

#include "gpu_analyze.h"
#include "gpu_apply.h"
#include "gpu_convert.h"
#include "gpu_gather.h"

#if defined(HDRG_WITH_CVPIXELBUFFER)
#include "hdrgrading/gpu/apple.h"
#endif

#if defined(HDRG_WITH_WEBGPU)
#define HDRG_GC_WGSL(name) hdrg_wgsl_##name
#else
#define HDRG_GC_WGSL(name) NULL
#endif
#if defined(HDRG_WITH_METAL)
#define HDRG_GC_MSL(name) hdrg_msl_##name
#else
#define HDRG_GC_MSL(name) NULL
#endif
#if defined(HDRG_WITH_VULKAN)
#define HDRG_GC_SPV(name) hdrg_spv_##name, &hdrg_spv_##name##_size
#else
#define HDRG_GC_SPV(name) NULL, NULL
#endif
#define HDRG_GC_SHADER(name, entry) {entry, HDRG_GC_MSL(name), HDRG_GC_SPV(name)}

/* rows of bytes between two row layouts */
typedef struct {
    const uint8_t *src;
    uint8_t *dst;
    size_t src_stride, dst_stride, bytes;
} hdrg_gc_copy_t;

#if defined(HDRG_WITH_WEBGPU)
extern const char hdrg_wgsl_apply[], hdrg_wgsl_convert[], hdrg_wgsl_gather[], hdrg_wgsl_io[],
    hdrg_wgsl_analyze[];
#endif
#if defined(HDRG_WITH_METAL)
extern const char hdrg_msl_apply_apply[], hdrg_msl_apply_yuv[], hdrg_msl_convert_sites[],
    hdrg_msl_convert_slopes[], hdrg_msl_convert_pixels[], hdrg_msl_gather_proxy[],
    hdrg_msl_gather_tiles[], hdrg_msl_io_copy_rows[], hdrg_msl_analyze_edges[],
    hdrg_msl_analyze_cells_of[], hdrg_msl_analyze_hist[], hdrg_msl_analyze_moments_of[];
#endif
#if defined(HDRG_WITH_VULKAN)
extern const unsigned char hdrg_spv_apply_apply[], hdrg_spv_apply_yuv[], hdrg_spv_convert_sites[],
    hdrg_spv_convert_slopes[], hdrg_spv_convert_pixels[], hdrg_spv_gather_proxy[],
    hdrg_spv_gather_tiles[], hdrg_spv_io_copy_rows[], hdrg_spv_analyze_edges[],
    hdrg_spv_analyze_cells_of[], hdrg_spv_analyze_hist[], hdrg_spv_analyze_moments_of[];
extern const size_t hdrg_spv_apply_apply_size, hdrg_spv_apply_yuv_size, hdrg_spv_convert_sites_size,
    hdrg_spv_convert_slopes_size, hdrg_spv_convert_pixels_size, hdrg_spv_gather_proxy_size,
    hdrg_spv_gather_tiles_size, hdrg_spv_io_copy_rows_size, hdrg_spv_analyze_edges_size,
    hdrg_spv_analyze_cells_of_size, hdrg_spv_analyze_hist_size, hdrg_spv_analyze_moments_of_size;
#endif

static const char *const hdrg_gc_apply_names[] = {"source", "luts",  "frame",
                                                  "dst",    "light", "planes"};
static const char *const hdrg_gc_convert_names[] = {"yuv", "base", "knots", "values", "rgb"};
static const char *const hdrg_gc_gather_names[] = {"frame", "dst"};
static const char *const hdrg_gc_copy_names[] = {"src", "dst"};
static const char *const hdrg_gc_analyze_names[] = {"source", "luts",   "cells",
                                                    "counts", "lights", "moments"};
static const hdrg_gc_entry_t hdrg_gc_apply_entries[] = {
    {HDRG_GC_SHADER(apply_apply, "apply"), {0, 1, 2, 3, 4}, 5},
    {HDRG_GC_SHADER(apply_yuv, "yuv"), {3, 5}, 2},
};
static const hdrg_gc_entry_t hdrg_gc_convert_entries[] = {
    {HDRG_GC_SHADER(convert_sites, "sites"), {0, 1}, 2},
    {HDRG_GC_SHADER(convert_slopes, "slopes"), {0, 1, 2, 3}, 4},
    {HDRG_GC_SHADER(convert_pixels, "pixels"), {0, 1, 2, 3, 4}, 5},
};
static const hdrg_gc_entry_t hdrg_gc_gather_entries[] = {
    {HDRG_GC_SHADER(gather_proxy, "proxy"), {0, 1}, 2},
    {HDRG_GC_SHADER(gather_tiles, "tiles"), {0, 1}, 2},
};
static const hdrg_gc_entry_t hdrg_gc_copy_entries[] = {
    {HDRG_GC_SHADER(io_copy_rows, "copy_rows"), {0, 1}, 2},
};
static const hdrg_gc_entry_t hdrg_gc_analyze_entries[] = {
    {HDRG_GC_SHADER(analyze_edges, "edges"), {0, 1, 3}, 3},
    {HDRG_GC_SHADER(analyze_cells_of, "cells_of"), {0, 1, 2}, 3},
    {HDRG_GC_SHADER(analyze_hist, "hist"), {0, 1, 3}, 3},
    {HDRG_GC_SHADER(analyze_moments_of, "moments_of"), {0, 1, 4, 5}, 4},
};

const hdrg_gc_program_t hdrg_gc_programs[HDRG_GC_PROGRAMS] = {
    {HDRG_GC_WGSL(apply), hdrg_gc_apply_names, hdrg_gc_apply_entries,
     (uint32_t)sizeof(hdrg_gpu_apply_params_t), 0x38u, 6, 2},
    {HDRG_GC_WGSL(convert), hdrg_gc_convert_names, hdrg_gc_convert_entries,
     (uint32_t)sizeof(hdrg_gpu_convert_params_t), 0x1eu, 5, 3},
    {HDRG_GC_WGSL(gather), hdrg_gc_gather_names, hdrg_gc_gather_entries,
     (uint32_t)sizeof(hdrg_gpu_gather_params_t), 0x2u, 2, 2},
    {HDRG_GC_WGSL(io), hdrg_gc_copy_names, hdrg_gc_copy_entries,
     (uint32_t)sizeof(hdrg_gc_copy_params_t), 0x2u, 2, 1},
    {HDRG_GC_WGSL(analyze), hdrg_gc_analyze_names, hdrg_gc_analyze_entries,
     (uint32_t)sizeof(hdrg_gpu_analyze_params_t), 0x2cu, 6, 4},
};

static void hdrg_gc_copy_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_gc_copy_t *copy = (const hdrg_gc_copy_t *)arg;
    uint32_t y;

    for (y = row_begin; y < row_end; y++) {
        memcpy(copy->dst + (size_t)y * copy->dst_stride, copy->src + (size_t)y * copy->src_stride,
               copy->bytes);
    }
}

/* rows of words from slot src to slot dst on the GPU (io.wgsl), waited for */
static bool hdrg_gc_copy_slots(hdrg_gpu_core_t *gpu, uint8_t src, uint8_t dst,
                               const hdrg_gc_copy_params_t *params)
{
    const uint8_t slots[2] = {src, dst};
    hdrg_gc_dispatch_t dispatch;

    dispatch.entry = 0;
    dispatch.groups_x = (params->words + HDRG_GC_GROUP - 1u) / HDRG_GC_GROUP;
    dispatch.groups_y = params->rows;

    return hdrg_gpu_core_run(gpu, HDRG_GC_COPY, params, slots, &dispatch, 1, 0u, 0u);
}

extern hdrg_status_t hdrg_gpu_core_create(const hdrg_plugin_host_t *host, uint32_t kind,
                                          hdrg_gpu_core_t **out)
{
    const hdrg_plugin_api_t *api = &host->api;
    const hdrg_gc_ops_t *const backends[] = {
#if defined(HDRG_WITH_METAL)
        &hdrg_gc_metal,
#endif
#if defined(HDRG_WITH_VULKAN)
        &hdrg_gc_vulkan,
#endif
#if defined(HDRG_WITH_WEBGPU)
        &hdrg_gc_webgpu,
#endif
        NULL,
    };
    hdrg_gpu_core_t *gpu;
    uint8_t i;
    void *state, *device, *queue;

    *out = NULL;

    for (i = 0; backends[i] != NULL; i++) {
        state = NULL;

        if ((kind != 0 && backends[i]->kind != kind) ||
            !hdrg_host_device(host, backends[i]->kind, &device, &queue) ||
            !backends[i]->open(api, device, queue, &state)) {
            continue;
        }

        gpu = (hdrg_gpu_core_t *)api->mem_calloc(api, 1, sizeof(*gpu));
        if (gpu == NULL) {
            backends[i]->close(state);

            return HDRG_ERR_NOMEM;
        }

        gpu->api = api;
        gpu->ops = backends[i];
        gpu->state = state;
        *out = gpu;

        return HDRG_OK;
    }

    return HDRG_ERR_UNSUPPORTED;
}

extern void hdrg_gpu_core_destroy(hdrg_gpu_core_t *gpu)
{
    if (gpu == NULL) {
        return;
    }

    gpu->ops->close(gpu->state);
    gpu->api->mem_free(gpu->api, gpu);
}

extern uint32_t hdrg_gpu_core_device(const hdrg_gpu_core_t *gpu)
{
    return gpu->ops->kind;
}

extern bool hdrg_gpu_core_buffer(hdrg_gpu_core_t *gpu, uint8_t slot, size_t size, uint8_t **view)
{
    return gpu->ops->buffer(gpu->state, slot, size, view);
}

extern bool hdrg_gpu_core_run(hdrg_gpu_core_t *gpu, uint8_t program, const void *params,
                              const uint8_t *slots, const hdrg_gc_dispatch_t *dispatches,
                              uint8_t count, uint64_t in, uint64_t out)
{
    return gpu->ops->run(gpu->state, program, params, slots, dispatches, count, in, out);
}

extern void *hdrg_gpu_core_frame(hdrg_gpu_core_t *gpu, uint8_t slot)
{
    return gpu->ops->frame(gpu->state, slot);
}

extern bool hdrg_gpu_core_take(hdrg_gpu_core_t *gpu, const void *frame, uint8_t slot, size_t size)
{
    return gpu->ops->take(gpu->state, frame, slot, size);
}

extern bool hdrg_gpu_core_import(hdrg_gpu_core_t *gpu, uint8_t slot, uint32_t kind, void *handle,
                                 size_t bytes, size_t *size)
{
    return gpu->ops->import(gpu->state, slot, kind, handle, bytes, size);
}

extern hdrg_status_t hdrg_gpu_core_plane(hdrg_gpu_core_t *gpu, const hdrg_device_frame_t *frame,
                                         uint8_t p, uint8_t slot, uint32_t at, uint32_t pitch,
                                         uint32_t bytes, uint32_t rows, uint8_t texel, bool in)
{
    const uint64_t offset = frame->offsets[p], stride = frame->pitches[p];
    const uint32_t words = (bytes + 3u) / 4u;
    hdrg_gc_copy_params_t params;
#if defined(HDRG_WITH_CVPIXELBUFFER)
    hdrg_apple_surface_t surface;
    uint8_t texel_bytes;
#endif
    size_t size;

    if (rows == 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* an AHardwareBuffer's plane: its texels copied (the slot's rows hold them packed) */
    if (frame->kind == HDRG_FRAME_AHARDWAREBUFFER) {
        if (gpu->ops->ahb == NULL) {
            return HDRG_ERR_UNSUPPORTED;
        }

        if (frame->handles[0] == NULL || bytes % hdrg_gc_texel_bytes(texel) != 0u) {
            return HDRG_ERR_INVALID_ARG;
        }

        return gpu->ops->ahb(gpu->state, frame->handles[0], p, texel, slot,
                             (size_t)at * sizeof(uint32_t), (size_t)pitch * sizeof(uint32_t),
                             bytes / hdrg_gc_texel_bytes(texel), rows, in);
    }

#if defined(HDRG_WITH_CVPIXELBUFFER)
    /* a pixel buffer's plane: its texels blitted (the slot's rows hold them packed) */
    if (frame->kind == HDRG_FRAME_CVPIXELBUFFER) {
        texel_bytes = hdrg_gc_texel_bytes(texel);

        if (gpu->ops->surface == NULL) {
            return HDRG_ERR_UNSUPPORTED;
        }

        if (!hdrg_apple_surface(frame->handles[0], &surface) || p >= surface.planes ||
            surface.bytes[p] != texel_bytes || bytes % texel_bytes != 0u ||
            surface.widths[p] < bytes / texel_bytes || surface.heights[p] < rows) {
            return HDRG_ERR_INVALID_ARG;
        }

        return gpu->ops->surface(gpu->state, surface.surface, p, texel, slot,
                                 (size_t)at * sizeof(uint32_t), (size_t)pitch * sizeof(uint32_t),
                                 bytes / texel_bytes, rows, in);
    }
#endif

    if (frame->kind != HDRG_FRAME_BUFFERS && frame->kind != HDRG_FRAME_DMABUF) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the copy's words in 32 bits, the plane's bytes in 64 (no product wraps) */
    if (offset % 4u != 0 || stride % 4u != 0 || stride < (uint64_t)words * 4u ||
        offset / 4u > UINT32_MAX || stride / 4u > UINT32_MAX ||
        (rows > 1u && stride > (UINT64_MAX - offset - (uint64_t)words * 4u) / (rows - 1u)) ||
        frame->sizes[p] > (uint64_t)SIZE_MAX) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* a dma-buf the device takes none of: unsupported, as a kind it does not take */
    if (!hdrg_gpu_core_import(gpu, (uint8_t)(HDRG_GC_APP0 + p), frame->kind, frame->handles[p],
                              (size_t)frame->sizes[p], &size)) {
        return (frame->kind == HDRG_FRAME_DMABUF) ? HDRG_ERR_UNSUPPORTED : HDRG_ERR_INVALID_ARG;
    }

    if (offset + (stride * (rows - 1u)) + ((uint64_t)words * 4u) > (uint64_t)size) {
        return HDRG_ERR_INVALID_ARG;
    }

    memset(&params, 0, sizeof(params));
    params.rows = rows;
    params.words = words;
    params.src_at = in ? (uint32_t)(offset / 4u) : at;
    params.src_pitch = in ? (uint32_t)(stride / 4u) : pitch;
    params.dst_at = in ? at : (uint32_t)(offset / 4u);
    params.dst_pitch = in ? pitch : (uint32_t)(stride / 4u);

    return hdrg_gc_copy_slots(gpu, in ? (uint8_t)(HDRG_GC_APP0 + p) : slot,
                              in ? slot : (uint8_t)(HDRG_GC_APP0 + p), &params)
               ? HDRG_OK
               : HDRG_ERR_INTERNAL;
}

extern void hdrg_gpu_core_copy(const hdrg_exec_t *exec, const uint8_t *src, size_t src_stride,
                               uint8_t *dst, size_t dst_stride, size_t bytes, uint32_t rows)
{
    hdrg_gc_copy_t copy;

    copy.src = src;
    copy.dst = dst;
    copy.src_stride = src_stride;
    copy.dst_stride = dst_stride;
    copy.bytes = bytes;

    hdrg_exec_rows(exec, rows, hdrg_gc_copy_rows, &copy);
}
