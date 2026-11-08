/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_WEBGPU_H
#define HDRG_GPU_WEBGPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <webgpu/webgpu.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/foundation/gpu_share.h"

/*
 * A WebGPU device (Dawn's webgpu.h, HDRG_WITH_WEBGPU) for
 * the GPU filters, with a thread that runs their work on it one item at a
 * time in the order given (a browser's, emdawnwebgpu: the work runs inline
 * on the calling thread, its waits suspend the export through JSPI): the device and its queue are
 * used from that thread alone, so a work item may wait there for its own commands (hdrg_gpu_finish,
 * hdrg_gpu_map_read) while its caller goes on. The waits block in wgpuInstanceWaitAny: an app's
 * device must come from an instance with TimedWaitAny. Work items report the device's errors
 * through hdrg_gpu_scope_begin / hdrg_gpu_scope_end.
 */
typedef struct hdrg_gpu hdrg_gpu_t;

typedef void (*hdrg_gpu_work_fn_t)(hdrg_gpu_t *gpu, void *arg);

/*
 * A frame on the device the library's GPU work hands from one part to the
 * next (the WebGPU filters' job_device / job_set_output_device, a session's
 * frames of the core: src/core/gpu_core) is an hdrg_gpu_share_frame_t of
 * WEBGPU: its buffer a WGPUBuffer (the device's thread), busy the
 * producer's (the host's thread)
 */
static inline WGPUBuffer hdrg_gpu_frame_buffer(const hdrg_gpu_share_frame_t *frame)
{
    return (WGPUBuffer)(uintptr_t)frame->buffer;
}

/*
 * device NULL: a high-performance adapter's device of its own (false: no
 * adapter or device to be had); otherwise the app's (it keeps a reference)
 * or one an hdrg_gpu_t has (that one, shared: a device is used from one
 * thread for a table). api: the plugin table whose allocator it uses until
 * destroyed (by its last holder)
 */
extern bool hdrg_gpu_create(const hdrg_plugin_api_t *api, WGPUDevice device, hdrg_gpu_t **out);
/*
 * the device of share (filter_gpu_share's): the binary's hdrg_gpu_t on it
 * for api (the library's own, shared), else one of the binary's own whose
 * work runs on share's thread (run); false: none to be had
 */
extern bool hdrg_gpu_adopt(const hdrg_plugin_api_t *api, const hdrg_gpu_share_t *share,
                           hdrg_gpu_t **out);
/* the share of gpu (filter_gpu_share): its handles, run on its thread; valid while gpu is */
extern void hdrg_gpu_share(hdrg_gpu_t *gpu, hdrg_gpu_share_t *share);
/* the work queued done, the thread joined, the references released (by the last holder) */
extern void hdrg_gpu_destroy(hdrg_gpu_t *gpu);
extern WGPUDevice hdrg_gpu_device(const hdrg_gpu_t *gpu);
extern WGPUQueue hdrg_gpu_queue(const hdrg_gpu_t *gpu);
/* fn(gpu, arg) on the device's thread after the work queued before it; false: out of memory */
extern bool hdrg_gpu_run(hdrg_gpu_t *gpu, hdrg_gpu_work_fn_t fn, void *arg);
/* hdrg_gpu_run, returning once fn returned (not from the device's thread) */
extern bool hdrg_gpu_call(hdrg_gpu_t *gpu, hdrg_gpu_work_fn_t fn, void *arg);
/* the device's thread: errors (validation, out of memory, internal) caught from here ... */
extern void hdrg_gpu_scope_begin(hdrg_gpu_t *gpu);
/* ... to here: true when none, nor a lost device */
extern bool hdrg_gpu_scope_end(hdrg_gpu_t *gpu);
/* the device's thread: the commands submitted so far done; false: the device failed */
extern bool hdrg_gpu_finish(hdrg_gpu_t *gpu);
/*
 * the device's thread: buffer (MapRead) mapped for reading its first size
 * bytes (wgpuBufferUnmap after); NULL: it failed
 */
extern const void *hdrg_gpu_map_read(hdrg_gpu_t *gpu, WGPUBuffer buffer, size_t size);
/* the device's thread: a WGSL module; NULL: it does not compile */
extern WGPUShaderModule hdrg_gpu_module(hdrg_gpu_t *gpu, const char *wgsl);
/* the device's thread: a compute pipeline of the module's entry point with layout; NULL: failed */
extern WGPUComputePipeline hdrg_gpu_pipeline(hdrg_gpu_t *gpu, WGPUShaderModule module,
                                             const char *entry, WGPUPipelineLayout layout);
/* the device's thread: a buffer of size bytes and usage; NULL: it failed */
extern WGPUBuffer hdrg_gpu_buffer(hdrg_gpu_t *gpu, size_t size, WGPUBufferUsage usage);

#endif /* HDRG_GPU_WEBGPU_H */
