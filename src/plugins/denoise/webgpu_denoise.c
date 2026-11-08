/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_WN_ID        "hdrg.webgpu.denoise"
#define HDRG_WN_LIT(s)    s, sizeof(s) - 1u
#define HDRG_WN_INFLIGHT  2u /* frames in flight: the next one's upload meets this one's */
#define HDRG_WN_BINDINGS  5u
#define HDRG_WN_GROUP     8u /* the shader's workgroups: 8 x 8 */
#define HDRG_WN_SUBMITTED (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst)

/*
 * hdrg.denoise.spatial (src/plugins/denoise/denoise.c) on the GPU
 * (src/plugins/denoise/wgsl/denoise.wgsl): the same noise from the frame's side
 * data, windows, coefficients and passthrough, in f32 (within the L2
 * tolerance of the CPU's), whole RGB16 frames uploaded, filtered in two
 * dispatches and read back. ASYNC: the jobs run in order on the device's
 * thread (src/gpu/webgpu), HDRG_WN_INFLIGHT of them in flight.
 */
typedef struct {
    hdrg_gpu_t *gpu;
    hdrg_webgpu_device_frame_t device[HDRG_WEBGPU_DEVICE_FRAMES]; /* to the next filter */
    WGPUBindGroupLayout group_layout;                             /* the device's thread */
    WGPUPipelineLayout layout;
    WGPUComputePipeline coefs, guide;
    WGPUBuffer params, input, coef_a, coef_b, output, readback;
    size_t in_size, coef_size, out_size; /* the buffers' (0: none yet) */
    uint32_t width, height;              /* configure's */
    bool active;
    bool built; /* create: the pipelines */
    double strength, chroma, sigma, chroma_sigma[2], level[HDRG_DENOISE_BINS];
    int64_t radius;
} hdrg_wn_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, radius, pad[3];
    float sigma[4], level[HDRG_DENOISE_BINS];
} hdrg_wn_params_t;

/* a job as submitted: what its run needs (the next frame's configure may come first) */
typedef struct {
    hdrg_wn_t *wn;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_webgpu_job_t frames;
    hdrg_wn_params_t params;
} hdrg_wn_work_t;

static void hdrg_wn_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the layouts and the two pipelines */
static void hdrg_wn_build(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferBindingType types[HDRG_WN_BINDINGS] = {
        WGPUBufferBindingType_Uniform, WGPUBufferBindingType_ReadOnlyStorage,
        WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage,
        WGPUBufferBindingType_Storage};
    hdrg_wn_t *wn = (hdrg_wn_t *)arg;
    WGPUBindGroupLayoutEntry entries[HDRG_WN_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;

    for (i = 0; i < HDRG_WN_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = types[i];
    }

    group_desc.entryCount = HDRG_WN_BINDINGS;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wn->group_layout = wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &wn->group_layout;
    wn->layout = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return;
    }

    module = hdrg_gpu_module(gpu, hdrg_wgsl_denoise);
    if (module == NULL) {
        return;
    }

    wn->coefs = hdrg_gpu_pipeline(gpu, module, "coefs", wn->layout);
    wn->guide = hdrg_gpu_pipeline(gpu, module, "guide", wn->layout);
    wn->params = hdrg_gpu_buffer(gpu, sizeof(hdrg_wn_params_t),
                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    wn->built = wn->coefs != NULL && wn->guide != NULL && wn->params != NULL;
}

/* the device's thread: every object */
static void hdrg_wn_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)arg;

    (void)gpu;

    if (wn->coefs != NULL) {
        wgpuComputePipelineRelease(wn->coefs);
    }

    if (wn->guide != NULL) {
        wgpuComputePipelineRelease(wn->guide);
    }

    if (wn->layout != NULL) {
        wgpuPipelineLayoutRelease(wn->layout);
    }

    if (wn->group_layout != NULL) {
        wgpuBindGroupLayoutRelease(wn->group_layout);
    }

    hdrg_wn_release_buffer(&wn->params);
    hdrg_wn_release_buffer(&wn->input);
    hdrg_wn_release_buffer(&wn->coef_a);
    hdrg_wn_release_buffer(&wn->coef_b);
    hdrg_wn_release_buffer(&wn->output);
    hdrg_wn_release_buffer(&wn->readback);
    hdrg_webgpu_device_frames_free(wn->device);
}

/* the device's thread: the frame and coefficient buffers for a job, kept while large enough */
static bool hdrg_wn_buffers(hdrg_gpu_t *gpu, hdrg_wn_t *wn, size_t in_size, size_t coef_size,
                            size_t out_size)
{
    if (in_size > wn->in_size) {
        hdrg_wn_release_buffer(&wn->input);
        wn->input = hdrg_gpu_buffer(gpu, in_size, HDRG_WN_SUBMITTED);
        wn->in_size = (wn->input != NULL) ? in_size : 0u;
    }

    if (coef_size > wn->coef_size) {
        hdrg_wn_release_buffer(&wn->coef_a);
        hdrg_wn_release_buffer(&wn->coef_b);
        wn->coef_a = hdrg_gpu_buffer(gpu, coef_size, WGPUBufferUsage_Storage);
        wn->coef_b = hdrg_gpu_buffer(gpu, coef_size, WGPUBufferUsage_Storage);
        wn->coef_size = (wn->coef_a != NULL && wn->coef_b != NULL) ? coef_size : 0u;
    }

    if (out_size > wn->out_size) {
        hdrg_wn_release_buffer(&wn->output);
        hdrg_wn_release_buffer(&wn->readback);
        wn->output =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        wn->readback =
            hdrg_gpu_buffer(gpu, out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        wn->out_size = (wn->output != NULL && wn->readback != NULL) ? out_size : 0u;
    }

    return wn->in_size >= in_size && wn->coef_size >= coef_size && wn->out_size >= out_size;
}

/* the device's thread: a job's upload, dispatches and read-back */
static hdrg_status_t hdrg_wn_frame(hdrg_gpu_t *gpu, const hdrg_wn_work_t *work)
{
    const size_t in_size = work->frames.in_stride * work->params.height,
                 out_size = work->frames.out_stride * work->params.height,
                 coef_size = (size_t)work->params.width * work->params.height * 3u * sizeof(float);
    const void *mapped;
    hdrg_wn_t *wn = work->wn;
    WGPUBindGroupEntry entries[HDRG_WN_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBuffer buffers[HDRG_WN_BINDINGS], in_buffer, out_buffer;
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i, groups_y;
    bool ok;

    /* (the frames on the device stand in for the input and output buffers: none made) */
    if (!hdrg_wn_buffers(gpu, wn, (work->frames.in_frame != NULL) ? 0u : in_size, coef_size,
                         (work->frames.out_frame != NULL) ? 0u : out_size)) {
        return HDRG_ERR_NOMEM;
    }

    /* the device frames: the previous filter's for the input, one of its own for the output */
    if (work->frames.out_frame != NULL &&
        !hdrg_webgpu_device_frame_fit(gpu, work->frames.out_frame, out_size)) {
        return HDRG_ERR_NOMEM;
    }

    in_buffer =
        (work->frames.in_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.in_frame) : wn->input;
    out_buffer = (work->frames.out_frame != NULL) ? hdrg_gpu_frame_buffer(work->frames.out_frame)
                                                  : wn->output;

    buffers[0] = wn->params;
    buffers[1] = in_buffer;
    buffers[2] = wn->coef_a;
    buffers[3] = wn->coef_b;
    buffers[4] = out_buffer;

    for (i = 0; i < HDRG_WN_BINDINGS; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer = buffers[i];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = wn->group_layout;
    group_desc.entryCount = HDRG_WN_BINDINGS;
    group_desc.entries = entries;
    groups_y = (work->params.height + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP;

    hdrg_gpu_scope_begin(gpu);

    if (work->frames.in_frame == NULL) {
        wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->input, 0, work->frames.in, in_size);
    }

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), wn->params, 0, &work->params, sizeof(work->params));
    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderSetPipeline(pass, wn->coefs);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (work->params.width + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP, groups_y, 1);
    wgpuComputePassEncoderSetPipeline(pass, wn->guide);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, ((work->params.width + 1u) / 2u + HDRG_WN_GROUP - 1u) / HDRG_WN_GROUP, groups_y, 1);
    wgpuComputePassEncoderEnd(pass);

    if (work->frames.out_frame == NULL) {
        wgpuCommandEncoderCopyBufferToBuffer(encoder, wn->output, 0, wn->readback, 0, out_size);
    }

    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);

    ok = hdrg_gpu_scope_end(gpu);

    /* the next filter's job follows on the device's thread: nothing to read back */
    if (work->frames.out_frame != NULL) {
        return ok ? HDRG_OK : HDRG_ERR_INTERNAL;
    }

    mapped = ok ? hdrg_gpu_map_read(gpu, wn->readback, out_size) : NULL;

    if (mapped == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memcpy(work->frames.out, mapped, out_size);
    wgpuBufferUnmap(wn->readback);

    return HDRG_OK;
}

static void hdrg_wn_run(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_wn_work_t *work = (hdrg_wn_work_t *)arg;

    (void)work->api->job_done(work->job, hdrg_wn_frame(gpu, work));
    work->api->mem_free(work->api, work);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn;
    hdrg_status_t status;

    wn = (hdrg_wn_t *)api->mem_calloc(api, 1, sizeof(*wn));
    if (wn == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_WN_LIT("strength"), &wn->strength);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_WN_LIT("chroma"), &wn->chroma);
    }

    if (status == HDRG_OK) {
        status = api->option_get_int(options, HDRG_WN_LIT("radius"), &wn->radius);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_WN_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, HDRG_DEVICE_WEBGPU);
    }

    if (status == HDRG_OK) {
        status = hdrg_webgpu_device(filter, api, &wn->gpu);
    }

    if (status == HDRG_OK && !hdrg_gpu_call(wn->gpu, hdrg_wn_build, wn)) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !wn->built) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        if (wn->gpu != NULL) {
            (void)hdrg_gpu_call(wn->gpu, hdrg_wn_release, wn);
        }

        hdrg_gpu_destroy(wn->gpu);
        api->mem_free(api, wn);

        return status;
    }

    *inst = wn;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_wn_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)inst;

    (void)hdrg_gpu_call(wn->gpu, hdrg_wn_release, wn);
    hdrg_gpu_destroy(wn->gpu);
    api->mem_free(api, wn);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &wn->width, &wn->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || wn->width == 0 || wn->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_denoise_settings(nego, api, wn->strength, &wn->sigma, wn->level, wn->chroma_sigma,
                                   &wn->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, wn->width, wn->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !wn->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_wn_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_wn_t *wn = (hdrg_wn_t *)inst;
    hdrg_wn_work_t *work;
    hdrg_status_t status;
    uint32_t i;

    work = (hdrg_wn_work_t *)api->mem_calloc(api, 1, sizeof(*work));
    if (work == NULL) {
        return HDRG_ERR_NOMEM;
    }

    work->wn = wn;
    work->job = job;
    work->api = api;
    status = hdrg_webgpu_job_frames(wn->device, job, api, wn->width, wn->height, &work->frames);

    if (status != HDRG_OK) {
        api->mem_free(api, work);

        return status;
    }

    work->params.width = wn->width;
    work->params.height = wn->height;
    work->params.in_stride = (uint32_t)(work->frames.in_stride / 4u);
    work->params.out_stride = (uint32_t)(work->frames.out_stride / 4u);
    work->params.radius = (uint32_t)wn->radius;
    work->params.sigma[0] = (float)wn->sigma;
    work->params.sigma[1] = (float)(wn->chroma_sigma[0] * wn->strength * wn->chroma);
    work->params.sigma[2] = (float)(wn->chroma_sigma[1] * wn->strength * wn->chroma);
    work->params.sigma[3] = (float)wn->strength;

    for (i = 0; i < HDRG_DENOISE_BINS; i++) {
        work->params.level[i] = (float)wn->level[i];
    }

    if (!hdrg_gpu_run(wn->gpu, hdrg_wn_run, work)) {
        api->mem_free(api, work);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_webgpu_denoise_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, HDRG_WN_LIT(HDRG_WN_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_WN_LIT("strength"), 1.0, 0.0, 4.0,
        HDRG_WN_LIT("smoothing threshold over the measured noise level (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_WN_LIT("chroma"), 2.0, 0.0, 4.0,
            HDRG_WN_LIT("the chroma threshold over the luma's (chroma noise reads worse)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_WN_LIT("radius"), 2, 1, 4,
                                           HDRG_WN_LIT("window radius in pixels"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_wn_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_wn_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_wn_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_wn_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_webgpu_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
