/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/foundation/thread.h"
#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
#include "hdrgrading/gpu/filter_gk.h"
#endif

/*
 * A module's GPU filters (test_metal, test_vulkan, test_webgpu), built like a plugin project apart
 * (src/plugins): PREFILTER copies on each kind of device the build has, on a GPU layer of the
 * module's own (hdrgrading-gpu linked in), the context's device shared with it
 * (filter_gpu_share). The jobs given their input and their output on the device are counted
 * (hdrg_test_gpu_jobs: the tests read them).
 */
#define TG_LIT(s) s, sizeof(s) - 1u

#if defined(HDRG_WITH_WEBGPU)
/* a WebGPU copy */
typedef struct {
    hdrg_gpu_t *gpu;
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES];
    WGPUBuffer stage, readback; /* the host's frames: uploaded, read back (the device's thread) */
    size_t staged;
    uint32_t width, height;
} tg_webgpu_t;

typedef struct {
    tg_webgpu_t *tw;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;
} tg_webgpu_work_t;
#endif

#if defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
/* a Metal or Vulkan copy */
typedef struct {
    const hdrg_gk_t *gk; /* first: the backend's frame_alloc's */
    hdrg_gk_device_t device;
    uint32_t width, height;
} tg_gk_t;

/* the backends with the module's prefix (the library's own classes are hdrg.metal.*) */
static hdrg_gk_backend_t g_tg_backends[2];
#endif

/* the jobs given their input, and their output, on the device */
static hdrg_atomic_u32_t g_tg_in, g_tg_out;

static void tg_count(bool in, bool out)
{
    (void)hdrg_atomic_u32_fetch_add_acq_rel(&g_tg_in, in ? 1u : 0u);
    (void)hdrg_atomic_u32_fetch_add_acq_rel(&g_tg_out, out ? 1u : 0u);
}

static hdrg_status_t tg_configure(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                  uint32_t *width, uint32_t *height)
{
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, width, height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || *width == 0 || *height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return api->nego_set_output(nego, *width, *height, HDRG_FORMAT_RGB16);
}

static hdrg_status_t tg_register(hdrg_class_t *cls, const hdrg_plugin_api_t *api,
                                 hdrg_filter_create_fn_t create, hdrg_filter_destroy_fn_t destroy,
                                 hdrg_filter_configure_fn_t configure,
                                 hdrg_filter_submit_fn_t submit,
                                 hdrg_filter_device_release_fn_t release)
{
    hdrg_status_t status;

    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                                                 HDRG_FILTER_GPU_FRAMES)
                                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, configure) : status;
    status = (status == HDRG_OK) ? api->class_set_submit(cls, submit) : status;
    status = (status == HDRG_OK) ? api->class_set_device_release(cls, release) : status;

    return status;
}

#if defined(HDRG_WITH_WEBGPU)
/* the device's thread (the library's, the module's work run there): the copy submitted */
static void tg_webgpu_run(hdrg_gpu_t *gpu, void *arg)
{
    const void *mapped;
    tg_webgpu_work_t *work = (tg_webgpu_work_t *)arg;
    tg_webgpu_t *tw = work->tw;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;
    WGPUBuffer in, out;
    hdrg_status_t status;
    size_t size;

    size = work->frames.in_stride * tw->height;
    status = HDRG_ERR_NOMEM;

    /* the host's frames through buffers of its own, uploaded and read back */
    if (tw->staged < size) {
        if (tw->stage != NULL) {
            wgpuBufferRelease(tw->stage);
            wgpuBufferRelease(tw->readback);
        }

        tw->stage = hdrg_gpu_buffer(gpu, size, WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
        tw->readback =
            hdrg_gpu_buffer(gpu, size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        tw->staged = (tw->stage != NULL && tw->readback != NULL) ? size : 0u;
    }

    if (tw->staged >= size && (work->frames.out_frame == NULL ||
                               hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, size))) {
        in = (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame)
                                             : tw->stage;
        out = (work->frames.out_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.out_frame)
                                               : tw->readback;

        hdrg_gpu_scope_begin(gpu);

        if (work->frames.in_frame == NULL) {
            wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), tw->stage, 0, work->frames.in, size);
        }

        encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, in, 0, out, 0, size);
        commands = wgpuCommandEncoderFinish(encoder, NULL);
        wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);

        status = hdrg_gpu_scope_end(gpu) ? HDRG_OK : HDRG_ERR_INTERNAL;
    }

    if (status == HDRG_OK && work->frames.out_frame == NULL) {
        mapped = hdrg_gpu_map_read(gpu, tw->readback, size);
        status = (mapped != NULL) ? HDRG_OK : HDRG_ERR_INTERNAL;

        if (mapped != NULL) {
            memcpy(work->frames.out, mapped, size);
            wgpuBufferUnmap(tw->readback);
        }
    }

    (void)work->api->job_done(work->job, status);
    work->api->mem_free(work->api, work);
}

static void tg_webgpu_release(hdrg_gpu_t *gpu, void *arg)
{
    tg_webgpu_t *tw = (tg_webgpu_t *)arg;

    (void)gpu;

    if (tw->stage != NULL) {
        wgpuBufferRelease(tw->stage);
        wgpuBufferRelease(tw->readback);
    }

    hdrg_webgpu_device_frames_free(tw->device);
}

static void tg_webgpu_free(tg_webgpu_t *tw, const hdrg_plugin_api_t *api)
{
    if (tw->gpu != NULL) {
        (void)hdrg_gpu_call(tw->gpu, tg_webgpu_release, tw);
    }

    hdrg_gpu_destroy(tw->gpu);
    api->mem_free(api, tw);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_webgpu_create(hdrg_filter_t *filter,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    tg_webgpu_t *tw;
    hdrg_status_t status;

    (void)options;

    tw = (tg_webgpu_t *)api->mem_calloc(api, 1, sizeof(*tw));
    if (tw == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->filter_set_max_inflight(filter, 1u);
    status = (status == HDRG_OK) ? api->filter_set_device(filter, HDRG_DEVICE_WEBGPU) : status;
    status = (status == HDRG_OK) ? hdrg_webgpu_device(filter, api, &tw->gpu) : status;

    if (status != HDRG_OK) {
        tg_webgpu_free(tw, api);

        return status;
    }

    *inst = tw;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL tg_webgpu_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    tg_webgpu_free((tg_webgpu_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_webgpu_configure(void *inst, hdrg_negotiation_t *nego,
                                                          const hdrg_plugin_api_t *api)
{
    tg_webgpu_t *tw = (tg_webgpu_t *)inst;

    return tg_configure(nego, api, &tw->width, &tw->height);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_webgpu_submit(void *inst, hdrg_job_t *job,
                                                       const hdrg_plugin_api_t *api)
{
    tg_webgpu_t *tw = (tg_webgpu_t *)inst;
    tg_webgpu_work_t *work;
    hdrg_status_t status;

    work = (tg_webgpu_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->tw = tw;
    work->job = job;
    work->api = api;
    status = hdrg_webgpu_job_frames(tw->device, job, api, tw->width, tw->height, &work->frames);

    if (status == HDRG_OK && work->frames.in_stride != work->frames.out_stride) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status == HDRG_OK) {
        tg_count(work->frames.in_frame != NULL, work->frames.out_frame != NULL);
    }

    if (status != HDRG_OK || !hdrg_gpu_run(tw->gpu, tg_webgpu_run, work)) {
        api->mem_free(api, work);

        return (status != HDRG_OK) ? status : HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}
#endif

#if defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
static void tg_gk_free(tg_gk_t *tk, const hdrg_plugin_api_t *api)
{
    hdrg_gk_device_frames_free(tk->gk, &tk->device);
    hdrg_gk_plugin_release(api, tk->gk);
    api->mem_free(api, tk);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_gk_create(hdrg_filter_t *filter,
                                                   const hdrg_options_t *options, void **inst,
                                                   const hdrg_plugin_api_t *api)
{
    tg_gk_t *tk;
    hdrg_status_t status;

    (void)options;

    tk = (tg_gk_t *)api->mem_calloc(api, 1, sizeof(*tk));
    if (tk == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->filter_set_max_inflight(filter, 1u);
    status = (status == HDRG_OK) ? hdrg_gk_plugin_device(filter, api, &tk->gk) : status;
    status = (status == HDRG_OK) ? api->filter_set_device(filter, tk->gk->ops->kind) : status;

    if (status != HDRG_OK) {
        tg_gk_free(tk, api);

        return status;
    }

    *inst = tk;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL tg_gk_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    tg_gk_free((tg_gk_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_gk_configure(void *inst, hdrg_negotiation_t *nego,
                                                      const hdrg_plugin_api_t *api)
{
    tg_gk_t *tk = (tg_gk_t *)inst;

    return tg_configure(nego, api, &tk->width, &tk->height);
}

static hdrg_status_t HDRG_PLUGIN_CALL tg_gk_submit(void *inst, hdrg_job_t *job,
                                                   const hdrg_plugin_api_t *api)
{
    tg_gk_t *tk = (tg_gk_t *)inst;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;

    status = hdrg_gk_job_frames(tk->gk, &tk->device, job, api, tk->width, tk->height, 6u, &frames);
    if (status != HDRG_OK) {
        return status;
    }

    if (frames.in_stride != frames.out_stride) {
        return HDRG_ERR_INTERNAL;
    }

    tg_count(frames.in_frame != NULL, frames.out_frame != NULL);

    pass = hdrg_gk_begin(tk->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    hdrg_gk_copy_buffer(tk->gk, pass, frames.in, frames.out, frames.in_stride * tk->height);

    return hdrg_gk_job_commit(tk->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

static hdrg_status_t tg_gk_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                    hdrg_gk_backend_t *backend, const hdrg_gk_backend_t *library,
                                    const char *prefix)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    *backend = *library;
    backend->prefix = prefix;
    cls = hdrg_gk_class(plugin, api, backend, "copy");
    status = tg_register(cls, api, tg_gk_create, tg_gk_destroy, tg_gk_configure, tg_gk_submit,
                         hdrg_gk_device_release);
    status = (status == HDRG_OK)
                 ? api->class_set_frames(cls, backend->frame_alloc, backend->frame_free)
                 : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}
#endif

static hdrg_status_t HDRG_PLUGIN_CALL tg_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
#if defined(HDRG_WITH_WEBGPU)
    hdrg_class_t *cls;
#endif
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TG_LIT("org.test.gpu"), TG_LIT("GPU copies"),
                                  TG_LIT("1.0.0"), TG_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TG_LIT("MIT"),
                                                           TG_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        cls = api->class_create(plugin, TG_LIT("org.test.gpu.webgpu.copy"));
        status = tg_register(cls, api, tg_webgpu_create, tg_webgpu_destroy, tg_webgpu_configure,
                             tg_webgpu_submit, hdrg_webgpu_device_release);
        status = (status == HDRG_OK) ? api->class_register(cls) : status;
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = tg_gk_register(plugin, api, &g_tg_backends[0], &hdrg_gk_metal_backend,
                                "org.test.gpu.metal");
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = tg_gk_register(plugin, api, &g_tg_backends[1], &hdrg_gk_vulkan_backend,
                                "org.test.gpu.vulkan");
    }
#endif

    return status;
}

/* the jobs so far given their input, and their output, on the device */
HDRG_PLUGIN_EXTERN_C HDRG_PLUGIN_EXPORT void HDRG_PLUGIN_CALL hdrg_test_gpu_jobs(uint32_t *in,
                                                                                 uint32_t *out)
{
    *in = hdrg_atomic_u32_load_acquire(&g_tg_in);
    *out = hdrg_atomic_u32_load_acquire(&g_tg_out);
}

HDRG_PLUGIN_DEFINE(tg_init)
