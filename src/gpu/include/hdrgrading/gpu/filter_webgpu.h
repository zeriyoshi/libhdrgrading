/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_FILTER_WEBGPU_H
#define HDRG_GPU_FILTER_WEBGPU_H

/*
 * The WebGPU filters' part of the GPU layer (HDRG_WITH_WEBGPU): their device, the context's
 * (filter_gpu_share: in the library's binary the library's own hdrg_gpu_t, its thread the device's;
 * in a module's an hdrg_gpu_t of the module's whose work runs on that thread), and the PREFILTER
 * jobs' frames. The filters also declare HDRG_DEVICE_WEBGPU (filter_set_device) and
 * HDRG_FILTER_GPU_FRAMES: between two of them working next to each other with a frame in flight
 * at most (LOW, stills) the frame stays on the device as one of the producer's device frames
 * (hdrg_gpu_share_frame_t: no read-back, no upload), and a job whose output goes on there ends
 * once submitted (the device's thread runs the jobs in order on one queue).
 */
#include "hdrgrading/gpu/webgpu.h"

#define HDRG_WEBGPU_DEVICE_FRAMES 2u /* a filter's device frames: one given, one released late */

/* a filter's frame on the device (job_set_output_device / job_device): one of WEBGPU */
typedef hdrg_gpu_share_frame_t hdrg_webgpu_device_frame_t;

/* a job's frames: the host's rows, or device frames */
typedef struct {
    hdrg_webgpu_device_frame_t *in_frame, *out_frame; /* NULL: the host's frame */
    const uint8_t *in;
    uint8_t *out;
    size_t in_stride, out_stride;
} hdrg_webgpu_job_t;

/*
 * the context's WebGPU device for filter (filter_plugin.c), a reference of the filter's
 * (hdrg_gpu_destroy in its destroy, after its objects on the device): HDRG_ERR_UNSUPPORTED when
 * none
 */
extern hdrg_status_t hdrg_webgpu_device(hdrg_filter_t *filter, const hdrg_plugin_api_t *api,
                                        hdrg_gpu_t **gpu);
/*
 * A PREFILTER job of a whole width x height RGB16 frame: its input (the
 * previous filter's device frame, or the host's rows) and output (one of
 * frames, given to the next filter on the device when it wants it, or the
 * host's rows), checked
 */
extern hdrg_status_t
hdrg_webgpu_job_frames(hdrg_webgpu_device_frame_t frames[HDRG_WEBGPU_DEVICE_FRAMES],
                       hdrg_job_t *job, const hdrg_plugin_api_t *api, uint32_t width,
                       uint32_t height, hdrg_webgpu_job_t *out);
/* the device's thread: a device frame's buffer, made for size bytes; false: failed */
extern bool hdrg_webgpu_device_frame_fit(hdrg_gpu_t *gpu, hdrg_webgpu_device_frame_t *frame,
                                         size_t size);
/* the device's thread: the device frames' buffers released (the instance's) */
extern void
hdrg_webgpu_device_frames_free(hdrg_webgpu_device_frame_t frames[HDRG_WEBGPU_DEVICE_FRAMES]);
/* the device_release of the WebGPU filters */
extern void HDRG_PLUGIN_CALL hdrg_webgpu_device_release(void *inst, void *frame,
                                                        const hdrg_plugin_api_t *api);

#endif /* HDRG_GPU_FILTER_WEBGPU_H */
