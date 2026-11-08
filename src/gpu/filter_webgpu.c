/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/gpu/filter_webgpu.h"

#define HDRG_WG_ALIGN 256u /* a device frame's row: the host frames' (HDRG_FORMAT_ALIGN) */

extern hdrg_status_t
hdrg_webgpu_job_frames(hdrg_webgpu_device_frame_t frames[HDRG_WEBGPU_DEVICE_FRAMES],
                       hdrg_job_t *job, const hdrg_plugin_api_t *api, uint32_t width,
                       uint32_t height, hdrg_webgpu_job_t *out)
{
    const size_t pitch = ((size_t)width * 6u + HDRG_WG_ALIGN - 1u) / HDRG_WG_ALIGN * HDRG_WG_ALIGN;
    hdrg_status_t status;
    uint64_t ready;
    uint32_t begin, end, row_begin, row_end, k;
    uint8_t wanted;
    void *in_device;

    memset(out, 0, sizeof(*out));

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_device(job, &in_device, &ready, &wanted);
    }

    if (status != HDRG_OK || begin != 0u || end != height) {
        return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
    }

    /* the input: the previous filter's device frame, or the host's rows */
    if (in_device != NULL) {
        out->in_frame = (hdrg_webgpu_device_frame_t *)in_device;
        out->in_stride = pitch;

        if (out->in_frame->kind != HDRG_DEVICE_WEBGPU || out->in_frame->buffer == 0u) {
            return HDRG_ERR_INTERNAL;
        }
    } else {
        status = api->job_input(job, 0, &out->in, &out->in_stride, &row_begin, &row_end);
        if (status != HDRG_OK || row_begin != 0u || row_end != height) {
            return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
        }
    }

    /* the output: a device frame of the filter's own for the next filter, or the host's rows */
    if (wanted != 0u) {
        for (k = 0; k < HDRG_WEBGPU_DEVICE_FRAMES && frames[k].busy; k++) {
        }

        if (k == HDRG_WEBGPU_DEVICE_FRAMES) {
            return HDRG_ERR_INTERNAL;
        }

        status = api->job_set_output_device(job, &frames[k], 0);
        if (status != HDRG_OK) {
            return status;
        }

        frames[k].busy = 1u;
        out->out_frame = &frames[k];
        out->out_stride = pitch;
    } else {
        status = api->job_output(job, 0, &out->out, &out->out_stride);
        if (status != HDRG_OK) {
            return status;
        }
    }

    return (out->in_stride % 4u == 0u && out->out_stride % 4u == 0u) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern bool hdrg_webgpu_device_frame_fit(hdrg_gpu_t *gpu, hdrg_webgpu_device_frame_t *frame,
                                         size_t size)
{
    WGPUBuffer buffer;

    if (frame->size >= size) {
        return true;
    }

    if (frame->buffer != 0u) {
        wgpuBufferRelease(hdrg_gpu_frame_buffer(frame));
    }

    /* written by a dispatch or a copy, read by the next or copied out (the core's take) */
    buffer = hdrg_gpu_buffer(
        gpu, size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    frame->kind = HDRG_DEVICE_WEBGPU;
    frame->buffer = (uint64_t)(uintptr_t)buffer;
    frame->size = (buffer != NULL) ? size : 0u;

    return buffer != NULL;
}

extern void
hdrg_webgpu_device_frames_free(hdrg_webgpu_device_frame_t frames[HDRG_WEBGPU_DEVICE_FRAMES])
{
    uint32_t k;

    for (k = 0; k < HDRG_WEBGPU_DEVICE_FRAMES; k++) {
        if (frames[k].buffer != 0u) {
            wgpuBufferRelease(hdrg_gpu_frame_buffer(&frames[k]));
        }

        frames[k].buffer = 0u;
        frames[k].size = 0;
        frames[k].busy = 0u;
    }
}

extern void HDRG_PLUGIN_CALL hdrg_webgpu_device_release(void *inst, void *frame,
                                                        const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;

    ((hdrg_webgpu_device_frame_t *)frame)->busy = 0u;
}
