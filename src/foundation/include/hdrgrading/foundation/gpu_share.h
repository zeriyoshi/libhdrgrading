/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_GPU_SHARE_H
#define HDRG_FOUNDATION_GPU_SHARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/plugin.h"

/*
 * The GPU of a context as the library and its plugins share it (filter_gpu_share of filter.h):
 * the native device and queue of a kind with what using them together takes, and the frames the
 * GPU filters and the library hand each other on the device. Native handles alone cross between
 * binaries: a plugin's GPU layer (src/gpu, the hdrgrading-gpu library it links) wraps them in
 * objects of its own (in the library's binary the library's own objects, shared; in a module's,
 * the module's on the same device and queue). Using it is optional: filter_device gives the raw
 * device and queue.
 */
#define HDRG_GPU_SHARE_VERSION 1u

typedef void(HDRG_PLUGIN_CALL *hdrg_gpu_share_fn_t)(void *arg);

/*
 * The context's device of kind (HDRG_DEVICE_* of gpu.h): device, queue
 * METAL id<MTLDevice> and id<MTLCommandQueue>; VULKAN a const
 * hdrg_vulkan_device_t * (gpu.h: its lock_queue the lock every submission
 * to the queue takes, the library's own among them) and its VkQueue;
 * WEBGPU a WGPUDevice and its WGPUQueue, instance the WGPUInstance its
 * waits block on, run(user, fn, arg) fn(arg) on the device's thread after
 * the work queued before it (the one thread that uses the device;
 * HDRG_ERR_NOMEM: not queued). valid until the plugin's deinit
 */
typedef struct {
    uint32_t struct_size; /* sizeof(hdrg_gpu_share_t): the asker's */
    uint32_t version;     /* HDRG_GPU_SHARE_VERSION: the asker's */
    uint32_t kind;
    void *device, *queue, *instance;
    hdrg_status_t(HDRG_PLUGIN_CALL *run)(void *user, hdrg_gpu_share_fn_t fn, void *arg);
    void *user;
} hdrg_gpu_share_t;

/*
 * A frame on the device (job_device, job_set_output_device,
 * job_device_window of filter.h): 16-bit R'G'B' rows of the host frames'
 * pitch (the width x 6 rounded up to 256) in buffer, its producer's (METAL
 * an id<MTLBuffer>, VULKAN a VkBuffer, WEBGPU a WGPUBuffer). METAL, VULKAN:
 * failed when the work writing it (or one before) failed, its completion's;
 * pending the completions yet to run of the work reading or writing it (the
 * consumers' too: their jobs end at the commit, the frame touched a moment
 * after the GPU is through). WEBGPU: the device's thread runs the work in
 * order, neither is used
 */
typedef struct {
    uint32_t kind; /* HDRG_DEVICE_* */
    uint64_t buffer;
    size_t size;
    hdrg_atomic_u32_t failed, pending;
    uint8_t busy; /* the producer's: given and not released yet (1) */
} hdrg_gpu_share_frame_t;

/*
 * The completions of the work on frames (in, out: NULL for none) to come:
 * their jobs may end at the commit (the next work follows on the queue)
 */
static inline void hdrg_gpu_share_hold(hdrg_gpu_share_frame_t *in, hdrg_gpu_share_frame_t *out,
                                       uint32_t delta)
{
    if (in != NULL) {
        (void)hdrg_atomic_u32_fetch_add_acq_rel(&in->pending, delta);
    }

    if (out != NULL) {
        (void)hdrg_atomic_u32_fetch_add_acq_rel(&out->pending, delta);
    }
}

/* a completion through with its frames (its last touch of them) */
static inline void hdrg_gpu_share_drop(hdrg_gpu_share_frame_t *in, hdrg_gpu_share_frame_t *out)
{
    hdrg_gpu_share_hold(in, out, UINT32_MAX);
}

/* until no completion is to touch frame: its failed final, the frame free to be given or freed */
static inline void hdrg_gpu_share_wait(hdrg_gpu_share_frame_t *frame)
{
    while (hdrg_atomic_u32_load_acquire(&frame->pending) != 0u) {
        hdrg_thread_yield();
    }
}

#endif /* HDRG_FOUNDATION_GPU_SHARE_H */
