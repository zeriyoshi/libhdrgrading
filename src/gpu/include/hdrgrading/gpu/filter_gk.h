/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_FILTER_GK_H
#define HDRG_GPU_FILTER_GK_H

/*
 * The GPU filters on a native API (gk.h: Metal, Vulkan), one code for both: the GPU layer's part
 * of them. A class of a backend (hdrg_gk_class: <prefix>.<name>, hdrg_gk_metal_backend's
 * hdrg.metal, hdrg_gk_vulkan_backend's hdrg.vulkan; its class user pointer the backend) takes the
 * context's device of the backend's kind (filter_gpu_share, hdrg_gk_plugin_device: in the
 * library's binary the library's own object on it, in a module's an object of the module's on
 * the same device and queue). Their jobs' frames are host memory of the device the GPU uses in
 * place (class_set_frames: no upload, no read-back), all of them made and released by the
 * backend's frame_alloc / frame_free: the host hands a frame from one filter of the backend (of a
 * binary) to the next without a copy. The PREFILTER filters also declare the backend's kind
 * (filter_set_device) and HDRG_FILTER_GPU_FRAMES: between two of them working next to each other
 * with a frame in flight at most (LOW, stills) the frame stays on the device as one of the
 * producer's device frames (its native buffer: the next wraps it, hdrg_gk_wrap), and a job whose
 * output goes on there ends once committed: the passes run back to back on the device's queue and
 * the host waits for the chain's last alone.
 */
#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/gpu/gk.h"

#if defined(HDRG_WITH_METAL)
#define HDRG_GK_MSL(name) hdrg_msl_##name
#else
#define HDRG_GK_MSL(name) NULL
#endif
#if defined(HDRG_WITH_VULKAN)
#define HDRG_GK_SPV(name) hdrg_spv_##name, &hdrg_spv_##name##_size
#else
#define HDRG_GK_SPV(name) NULL, NULL
#endif
/*
 * a filter's shader: Tint's MSL (hdrg_msl_<name>) and SPIR-V (hdrg_spv_<name>, its size) of the
 * WGSL entry point, embedded by its project (hdrg_embed_shaders)
 */
#define HDRG_GK_SHADER(name, entry) {entry, HDRG_GK_MSL(name), HDRG_GK_SPV(name)}

#define HDRG_GK_DEVICE_FRAMES 2u /* a filter's device frames: one given, one released late */

/* a backend: its device's ops, its classes' prefix, its frames' functions */
typedef struct {
    const hdrg_gk_ops_t *ops;
    const char *prefix; /* hdrg.metal */
    hdrg_filter_frame_alloc_fn_t frame_alloc;
    hdrg_filter_frame_free_fn_t frame_free;
} hdrg_gk_backend_t;

/* a PREFILTER filter's frames on the device: its own (to the next filter), the others' it read */
typedef struct {
    hdrg_gk_frame_t frames[HDRG_GK_DEVICE_FRAMES];
    hdrg_gk_wraps_t read;
} hdrg_gk_device_t;

/* a job's frames: its buffers, their rows' bytes, and the device frames among them */
typedef struct {
    hdrg_gk_buffer_t *in, *out;
    hdrg_gpu_share_frame_t *in_frame; /* the previous filter's (NULL: the host's frame) */
    hdrg_gk_frame_t *out_frame;       /* one of the filter's own (NULL: the host's frame) */
    const uint8_t *in_host;           /* the host's frames' samples (else NULL) */
    uint8_t *out_host;
    size_t in_stride, out_stride;
} hdrg_gk_job_t;

/* the backends: Metal's (HDRG_WITH_METAL), Vulkan's (HDRG_WITH_VULKAN) */
#if defined(HDRG_WITH_METAL)
extern const hdrg_gk_backend_t hdrg_gk_metal_backend;
#endif
#if defined(HDRG_WITH_VULKAN)
extern const hdrg_gk_backend_t hdrg_gk_vulkan_backend;
#endif

/*
 * the context's device of the filter's backend's kind (its class's user pointer, filter_plugin.c):
 * an object of the filter's on it (hdrg_gk_plugin_release in its destroy, after its kernels and
 * buffers); HDRG_ERR_UNSUPPORTED when there is none
 */
extern hdrg_status_t hdrg_gk_plugin_device(hdrg_filter_t *filter, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_t **gk);
/* ... and its release (gk NULL: none) */
extern void hdrg_gk_plugin_release(const hdrg_plugin_api_t *api, const hdrg_gk_t *gk);
/* a class of the backend: <prefix>.<name>, its user pointer the backend (NULL: failed) */
extern hdrg_class_t *hdrg_gk_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                   const hdrg_gk_backend_t *backend, const char *name);
/*
 * the frame_alloc of every filter of the backend (its instance starts with
 * its const hdrg_gk_t *): an RGB16 or RGBA16 frame (one plane, rows of a
 * multiple of 256 bytes) in a host buffer of the device, its handle the
 * buffer
 */
extern hdrg_status_t hdrg_gk_frame_alloc(const hdrg_gk_t *gk, uint32_t format, uint32_t width,
                                         uint32_t height, uint8_t **planes, size_t *strides,
                                         void **handle);
/* ... and its frame_free: any filter's instance of the backend releases any of them */
extern void hdrg_gk_frame_free(const hdrg_gk_t *gk, void *handle);
/*
 * A PREFILTER job of a whole width x height frame of pixel bytes a pixel:
 * its input (the previous filter's device frame, wrapped in device's read,
 * or the host's frame) and output (one of device's frames, given to the
 * next filter on the device when it wants it, or the host's frame), checked
 */
extern hdrg_status_t hdrg_gk_job_frames(const hdrg_gk_t *gk, hdrg_gk_device_t *device,
                                        hdrg_job_t *job, const hdrg_plugin_api_t *api,
                                        uint32_t width, uint32_t height, uint32_t pixel,
                                        hdrg_gk_job_t *out);
/*
 * the recorded pass committed: the job ends once it is committed when its
 * output is a device frame (the frame marked failed with the pass), else
 * once the GPU is through (failed with the pass or its input's); false
 * when it could not commit (no job_done then)
 */
extern bool hdrg_gk_job_commit(const hdrg_gk_t *gk, hdrg_gk_pass_t *pass, hdrg_job_t *job,
                               const hdrg_plugin_api_t *api, const hdrg_gk_job_t *frames);
/* a job of a filter passing the frame as it is (host frames alone) */
extern hdrg_status_t hdrg_gk_job_pass(hdrg_job_t *job, const hdrg_plugin_api_t *api,
                                      const hdrg_gk_job_t *frames, uint32_t width, uint32_t height,
                                      uint32_t pixel);
/* the device frames' buffers and the wraps released (the instance's destroy) */
extern void hdrg_gk_device_frames_free(const hdrg_gk_t *gk, hdrg_gk_device_t *device);
/* the device_release of the PREFILTER filters */
extern void HDRG_PLUGIN_CALL hdrg_gk_device_release(void *inst, void *frame,
                                                    const hdrg_plugin_api_t *api);

#endif /* HDRG_GPU_FILTER_GK_H */
